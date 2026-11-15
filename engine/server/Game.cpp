// Gameplay on top of the data-driven world: loading maps and entity instances, stats, inventory,
// doors/containers, map transitions and a first-pass real-time combat system.
// Numbers marked "guess" are placeholders: the original formulas are not decoded yet (docs/server-data.md).
#include <functional>
#include <enet/enet.h>

#include <algorithm>
#include <cmath>
#include <random>
#include "Scripting.h"
#include "Server.h"
#include "common/gamedata/CellClass.h"
#include "common/log.h"

namespace f3 {

namespace {
std::mt19937& Rng() {
    static std::mt19937 r{std::random_device{}()};
    return r;
}
int RandRange(int lo, int hi) {
    if (hi < lo) std::swap(lo, hi);
    return std::uniform_int_distribution<int>(lo, hi)(Rng());
}

TilePos CellOf(float x, float z) {
    return TilePos{static_cast<int16_t>(std::floor(x * 2.0f)), static_cast<int16_t>(std::floor(z * 2.0f))};
}

Dir DirFromYaw(float yaw) {
    const float pi = 3.14159265f;
    int step = static_cast<int>(std::lround(yaw / (pi / 4)));
    return static_cast<Dir>(((step % 8) + 8) % 8);
}

bool Stackable(const Proto& p) { return p.cls == PClass::Ammo || p.cls == PClass::Item; }
bool Equippable(const Proto& p) { return p.cls == PClass::Weapon || p.cls == PClass::Armor; }

// Bresenham line of sight over the blocked grid (endpoints excluded).
bool HasLos(const Map& m, TilePos a, TilePos b) {
    int x0 = a.x, y0 = a.y, x1 = b.x, y1 = b.y;
    int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    while (true) {
        if ((x0 != a.x || y0 != a.y) && (x0 != b.x || y0 != b.y) && m.IsBlocked(x0, y0)) return false;
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
    return true;
}
}  // namespace

// ------------------------------------------------------------------ world loading

namespace {
// Doorway of a door entity: 8-connected group of 0x1b cells nearest to `pos` (within 12 cells), not owned by another door.
// Falls back to the blocked cells of the 3x3 around the position (doors in walls of test maps) when no doorway class is near.
std::vector<TilePos> DoorFootprint(Map& m, TilePos pos, ItemId owner) {
    std::vector<TilePos> out;
    if (m.cls.empty()) return out;
    auto at = [&](int x, int y) -> int { return m.InBounds(x, y) ? m.cls[static_cast<size_t>(y) * m.width + x] : -1; };
    auto free = [&](int x, int y) { return at(x, y) == gd::kCellDoor && !m.doorCell.count(y * m.width + x); };
    int bestD = 1 << 30, bx = 0, by = 0;
    for (int dy = -12; dy <= 12; ++dy)
        for (int dx = -12; dx <= 12; ++dx)
            if (free(pos.x + dx, pos.y + dy) && dx * dx + dy * dy < bestD) { bestD = dx * dx + dy * dy; bx = pos.x + dx; by = pos.y + dy; }
    if (bestD == (1 << 30)) return out;   // no doorway: the door only animates
    std::vector<TilePos> stack{TilePos{static_cast<int16_t>(bx), static_cast<int16_t>(by)}};
    m.doorCell[by * m.width + bx] = owner;
    while (!stack.empty() && out.size() < 800) {
        TilePos c = stack.back(); stack.pop_back();
        out.push_back(c);
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
                if ((dx || dy) && free(c.x + dx, c.y + dy)) {
                    m.doorCell[(c.y + dy) * m.width + c.x + dx] = owner;
                    stack.push_back(TilePos{static_cast<int16_t>(c.x + dx), static_cast<int16_t>(c.y + dy)});
                }
    }
    return out;
}
}  // namespace

bool Server::LoadWorldData() {
    if (!data_.Loaded()) return false;
    size_t entities = 0, skipped = 0;
    for (const std::string& name : data_.MapNames()) {
        MapDef def;
        std::string err;
        if (!data_.LoadMap(name, def, err)) { log::Warn("data: map %s: %s", name.c_str(), err.c_str()); continue; }
        std::vector<uint8_t> blocked(def.walk.size());
        for (size_t i = 0; i < blocked.size(); ++i) blocked[i] = gd::CellSolid(def.walk[i]) ? 1 : 0;   // doorway cells (0x1b) are handled by the door objects
        TilePos spawn{static_cast<int16_t>(def.width / 2), static_cast<int16_t>(def.height / 2)};
        Map* m = world_.CreateMap(def.name, def.width, def.height, std::move(blocked), spawn);
        m->originX = def.originX; m->originZ = def.originZ;
        m->cls = def.walk;
        // original world position (x, z) -> cell; the grid origin is the tre root bbox min (docs/formats/maps.md s.2)
        auto cellOf = [&](float x, float z) { return CellOf(x - def.originX, z - def.originZ); };
        for (auto& ep : def.entries) {
            EntryPoint e;
            e.id = ep.id;
            for (auto& s : ep.slots) e.slots.push_back({cellOf(s.x, s.z), s.yaw});
            m->entries.push_back(std::move(e));
        }
        if (!m->entries.empty() && !m->entries[0].slots.empty()) m->spawn = m->entries[0].slots[0].pos;
        TilePos fs{};
        if (!m->IsFree(m->spawn) && m->FindFreeNear(m->spawn, 60, fs)) m->spawn = fs;
        for (auto& r : def.regions) {
            Region rg;
            rg.kind = r.kind; rg.script = r.script; rg.targetMap = r.targetMap; rg.targetEntry = r.targetEntry;
            rg.minx = rg.miny = 1e9f; rg.maxx = rg.maxy = -1e9f;
            for (auto& p : r.pts) {
                const float cx = (p.first - def.originX) * 2.0f, cy = (p.second - def.originZ) * 2.0f;
                rg.poly.emplace_back(cx, cy);
                rg.minx = std::min(rg.minx, cx); rg.maxx = std::max(rg.maxx, cx);
                rg.miny = std::min(rg.miny, cy); rg.maxy = std::max(rg.maxy, cy);
            }
            m->regions.push_back(std::move(rg));
        }
        size_t pi = 0;
        for (auto& path : def.paths) {
            std::vector<TilePos> cells;
            for (auto& p : path) cells.push_back(cellOf(p.first, p.second));
            m->paths.push_back(std::move(cells));
            m->pathNames.push_back(pi < def.pathNames.size() ? def.pathNames[pi] : std::string());
            m->pathsWorld.push_back(path);
            ++pi;
        }
        size_t before = m->critters.size() + m->items.size();
        for (const EntityDef& e : def.entities) {
            const Proto* p = data_.FindProto(e.proto);
            if (!p) { ++skipped; continue; }  // stale template reference in the original data
            TilePos pos = cellOf(e.x, e.z);
            if (p->cls == PClass::Critter) SpawnCritterProto(m->id, *p, pos, &e);
            else SpawnObjectProto(m->id, *p, pos, 1, &e);
        }
        entities += m->critters.size() + m->items.size() - before;
    }
    if (!world_.FirstMap()) return false;
    for (auto& [k, v] : data_.Globals()) globals_[k] = v;
    log::Info("data: %zu maps loaded, %zu entities spawned (%zu stale refs skipped)", world_.Maps().size(), entities, skipped);
    return true;
}

void Server::InitCritterStats(Critter& c, const Proto& p) {
    c.protoPid = p.pid;
    for (int i = 0; i < 7; i++) c.special[i] = p.special[i];
    c.level = 1;
    RecalcDerived(c);
    c.hp = c.maxHp;
    c.team = p.team;
    c.isStatic = p.isStatic;
    c.script = p.script;
    c.dialog = p.dialog;
}

double Server::GetGlobalFor(const Critter* pc, const std::string& k) const {
    if (pc) { auto it = pc->gvars.find(k); if (it != pc->gvars.end()) return it->second; }
    return GetGlobal(k);
}

void Server::SetGlobalFor(Critter* pc, const std::string& k, double v) {
    if (pc && pc->IsPlayer()) pc->gvars[k] = v; else globals_[k] = v;
}

Critter* Server::SpawnCritterProto(MapId mapId, const Proto& p, TilePos pos, const EntityDef* inst) {
    Map* m = world_.GetMap(mapId);
    if (!m || !m->InBounds(pos.x, pos.y)) return nullptr;
    TilePos spot = pos;
    bool isStatic = p.isStatic;
    if (!isStatic && !m->IsFree(pos) && !m->FindFreeNear(pos, 6, spot)) { log::Warn("spawn %s: no free tile near %d,%d", p.key.c_str(), pos.x, pos.y); return nullptr; }
    Critter* c = world_.CreateCritter(p.DisplayName(), CritterKind::Npc);
    InitCritterStats(*c, p);
    if (inst) {
        c->team = inst->team;
        if (!inst->script.empty()) c->script = inst->script;
        if (!inst->dialog.empty()) c->dialog = inst->dialog;
        c->dir = DirFromYaw(inst->yaw);
        c->instName = inst->name;
    }
    c->color = 0xC0C0C0FF;
    PlaceCritter(*c, *m, spot);
    // loadout: template equipment is worn, instance (or template) inventory is carried
    for (const std::string& k : p.equipment)
        if (const Proto* ip = data_.FindProto(k)) {
            ItemId id = GiveItem(*c, *ip, 1);
            if (Equippable(*ip)) EquipItem(*c, id, true);
        }
    const std::vector<std::string>& inv = (inst && !inst->inventory.empty()) ? inst->inventory : p.inventory;
    for (const std::string& k : inv)
        if (const Proto* ip = data_.FindProto(k)) GiveItem(*c, *ip, 1);
    bool hasWeapon = false;
    for (auto& it : c->inventory)
        if (it.equipped && data_.GetProto(it.pid)->cls == PClass::Weapon) hasWeapon = true;
    if (!hasWeapon)
        for (auto& it : c->inventory)
            if (data_.GetProto(it.pid)->cls == PClass::Weapon) { EquipItem(*c, it.id, true); break; }
    if (amx_) amx_->AttachCritter(*c);
    return c;
}

ItemId Server::SpawnObjectProto(MapId mapId, const Proto& p, TilePos pos, uint16_t count, const EntityDef* inst) {
    Map* m = world_.GetMap(mapId);
    if (!m || !m->InBounds(pos.x, pos.y)) return 0;
    Item it;
    it.id = world_.NextItemId();
    it.pid = p.pid;
    it.name = p.DisplayName();
    it.mapId = mapId;
    it.pos = pos;
    it.count = count;
    if (inst) it.script = inst->script;
    switch (p.cls) {
        case PClass::Door:
            it.kind = ObjKind::Door;
            it.locked = p.doorLocked;
            it.footprint = DoorFootprint(*m, pos, it.id);
            if (it.locked)
                for (TilePos t : it.footprint) m->blocked[static_cast<size_t>(t.y) * m->width + t.x] = 1;
            break;
        case PClass::Container: {
            it.kind = ObjKind::Container;
            std::vector<std::string> keys = p.inventory;
            if (inst) keys.insert(keys.end(), inst->inventory.begin(), inst->inventory.end());
            for (auto& k : keys)
                if (const Proto* ip = data_.FindProto(k)) {
                    bool merged = false;
                    if (Stackable(*ip))
                        for (auto& e : it.contents)
                            if (e.pid == ip->pid) { e.count++; merged = true; break; }
                    if (!merged) it.contents.push_back(InvItem{world_.NextItemId(), ip->pid, 1, false});
                }
            break;
        }
        case PClass::Usable: it.kind = ObjKind::Usable; break;
        default: it.kind = ObjKind::Ground; break;
    }
    ItemId id = it.id;
    m->items[id] = std::move(it);
    m->dirty = true;
    if (amx_) amx_->AttachObject(m->items[id]);
    return id;
}

// ------------------------------------------------------------------ stats

// Unit weight: per-prototype override (config.ItemWeights), else the class default from the config table.
int Server::ProtoWeight(const Proto& p) const {
    auto o = rt_.ItemWeights.find(p.key);
    if (o != rt_.ItemWeights.end()) return std::max(0, o->second);
    switch (p.cls) {
        case PClass::Weapon: return rt_.WeightWeapon;
        case PClass::Armor: return rt_.WeightArmor;
        case PClass::Ammo: return rt_.WeightAmmo;
        default: return rt_.WeightItem;
    }
}

int Server::CarryLimit(const Critter& c) const { return std::max(0, rt_.CarryBase + rt_.CarryPerStrength * c.special[0]); }

int Server::Weight(const Critter& c) const {
    int w = 0;
    for (auto& it : c.inventory)
        if (const Proto* p = data_.GetProto(it.pid)) w += ProtoWeight(*p) * it.count;
    return w;
}

namespace {
// Armor class and damage threshold of whatever the critter wears (guess: GIAR word 0 / 2 = DT, word 1 / 4 = AC).
struct ArmorInfo { int ac = 0, dt = 0; };
ArmorInfo WornArmor(const Critter& c, const DataStore& d) {
    ArmorInfo a;
    for (auto& it : c.inventory)
        if (it.equipped)
            if (const Proto* p = d.GetProto(it.pid))
                if (p->cls == PClass::Armor) { a.dt += p->armor[0] / 2; a.ac += p->armor[1] / 4; }
    return a;
}
const Proto* EquippedWeapon(const Critter& c, const DataStore& d) {   // the active weapon: hand 1
    for (auto& it : c.inventory)
        if (it.slot == gnet::SlotHand1)
            if (const Proto* p = d.GetProto(it.pid))
                if (p->cls == PClass::Weapon) return p;
    return nullptr;
}
}  // namespace

void Server::SendStats(Critter& c) {
    if (!c.peer) return;
    gnet::Stats s;
    s.critterId = c.id;
    for (int i = 0; i < 7; i++) s.special[i] = static_cast<uint8_t>(c.special[i]);
    s.level = static_cast<uint8_t>(c.level);
    s.hp = static_cast<int16_t>(c.hp);
    s.maxHp = static_cast<int16_t>(c.maxHp);
    s.armorClass = static_cast<uint8_t>(std::min(255, c.special[5] + WornArmor(c, data_).ac));
    s.sequence = static_cast<uint8_t>(2 * c.special[1]);
    s.meleeBonus = static_cast<uint8_t>(std::max(0, c.special[0] - 5));
    s.carryWeight = static_cast<uint16_t>(std::min(65535, CarryLimit(c)));
    s.carried = static_cast<uint16_t>(Weight(c));
    Send(c, gnet::PackG(s));
}

static gnet::InvEntry MakeEntry(const DataStore& d, const InvItem& it, const std::function<int(const Proto&)>& wf) {
    gnet::InvEntry e;
    e.id = it.id; e.pid = it.pid; e.count = it.count; e.equipped = it.slot;
    if (const Proto* p = d.GetProto(it.pid)) { e.name = p->DisplayName(); e.weight = static_cast<uint16_t>(std::clamp(wf(*p), 0, 65535)); }
    return e;
}

void Server::SendInventory(Critter& c) {
    if (!c.peer) return;
    gnet::Inventory m;
    m.owner = c.id;
    for (auto& it : c.inventory) m.items.push_back(MakeEntry(data_, it, [this](const Proto& q) { return ProtoWeight(q); }));
    Send(c, gnet::PackG(m));
}

void Server::SendCritterState(const Critter& c) {
    gnet::CritterState s;
    s.id = c.id; s.hp = static_cast<int16_t>(c.hp); s.maxHp = static_cast<int16_t>(c.maxHp); s.dead = c.dead ? 1 : 0;
    ByteBuffer b = gnet::PackG(s);
    Map* m = world_.GetMap(c.mapId);
    if (!m) return;
    for (Critter* p : m->critters)
        if (p->IsPlayer() && (p == &c || p->visCritters.count(c.id))) Send(*p, b);
}

void Server::SendAppearance(const Critter& c, Critter* onlyTo) {
    gnet::Appearance a;
    a.id = c.id;
    for (auto& it : c.inventory) {
        if (it.slot == gnet::SlotBody) a.body = it.pid;
        else if (it.slot == gnet::SlotHead) a.head = it.pid;
        else if (it.slot == gnet::SlotHand1) a.weapon = it.pid;
    }
    ByteBuffer b = gnet::PackG(a);
    if (onlyTo) { Send(*onlyTo, b); return; }
    Map* m = world_.GetMap(c.mapId);
    if (!m) return;
    for (Critter* p : m->critters)
        if (p->IsPlayer() && (p == &c || p->visCritters.count(c.id))) Send(*p, b);
}

void Server::SendObjectInfo(Critter& to, const Item& it) {
    gnet::ObjectInfo o;
    o.id = it.id;
    o.kind = static_cast<gnet::ObjectKind>(it.kind);
    o.open = it.open ? 1 : 0;
    Send(to, gnet::PackG(o));
}

std::string Server::Examine(Critter&, ItemId id, bool isCritter) {
    if (isCritter) {
        Critter* t = world_.GetCritter(id);
        if (!t) return "";
        std::string s = t->name;
        if (t->protoPid >= 0)
            if (const Proto* p = data_.GetProto(static_cast<uint16_t>(t->protoPid))) {
                if (!p->desc.empty()) s += ": " + p->desc;
                else if (!p->name2.empty() && p->name2 != p->name) s += " (" + p->name2 + ")";
            }
        s += " [HP " + std::to_string(t->hp) + "/" + std::to_string(t->maxHp) + (t->dead ? ", dead" : "") + "]";
        return s;
    }
    for (auto& m : world_.Maps()) {
        auto it = m->items.find(id);
        if (it == m->items.end()) continue;
        std::string s = it->second.name;
        if (const Proto* p = data_.GetProto(it->second.pid))
            if (!p->desc.empty()) s += ": " + p->desc;
        return s;
    }
    return "";
}

// ------------------------------------------------------------------ inventory

ItemId Server::GiveItem(Critter& c, const Proto& p, int count) {
    if (count < 1) return 0;
    if (Stackable(p))
        for (auto& it : c.inventory)
            if (it.pid == p.pid) { it.count = static_cast<uint16_t>(std::min(65535, it.count + count)); return it.id; }
    ItemId first = 0;
    int n = Stackable(p) ? 1 : count;
    for (int i = 0; i < n; i++) {
        InvItem it{world_.NextItemId(), p.pid, static_cast<uint16_t>(Stackable(p) ? count : 1), false};
        if (!first) first = it.id;
        c.inventory.push_back(it);
    }
    return first;
}

// Equip rules (server authoritative): weapons go to the hands (hand 1 = active, hand 2 = spare), armor to the body or
// (helmets) the head slot. `slot` 0 = automatic: a weapon takes hand 1 and pushes the old hand-1 weapon to hand 2 when
// that is free (otherwise it is unequipped); an explicit slot must suit the item class. Moving an equipped weapon onto
// an occupied hand swaps the two.
bool Server::EquipItem(Critter& c, ItemId invId, bool on, uint8_t slot) {
    InvItem* target = nullptr;
    for (auto& it : c.inventory)
        if (it.id == invId) target = &it;
    if (!target) return false;
    const Proto* p = data_.GetProto(target->pid);
    if (!p || !Equippable(*p)) return false;
    auto occupant = [&](uint8_t s) -> InvItem* {
        for (auto& it : c.inventory)
            if (it.slot == s && &it != target) return &it;
        return nullptr;
    };
    auto set = [](InvItem& it, uint8_t s) { it.slot = s; it.equipped = s != 0; };
    if (!on) { set(*target, gnet::SlotNone); }
    else if (p->cls == PClass::Armor) {
        const uint8_t want = p->headArmor ? gnet::SlotHead : gnet::SlotBody;
        if (slot != 0 && slot != want) return false;
        if (InvItem* o = occupant(want)) set(*o, gnet::SlotNone);
        set(*target, want);
    } else {
        if (slot != 0 && slot != gnet::SlotHand1 && slot != gnet::SlotHand2) return false;
        const uint8_t from = target->slot;
        const uint8_t want = slot ? slot : gnet::SlotHand1;
        if (from == want) return true;
        if (InvItem* o = occupant(want)) {
            if (from == gnet::SlotHand1 || from == gnet::SlotHand2) set(*o, from);                  // swap hands
            else if (!slot && want == gnet::SlotHand1 && !occupant(gnet::SlotHand2)) set(*o, gnet::SlotHand2);  // push old weapon
            else set(*o, gnet::SlotNone);
        }
        set(*target, want);
    }
    SendAppearance(c);
    if (c.IsPlayer()) { SendInventory(c); SendStats(c); }
    return true;
}

bool Server::DropItem(Critter& c, ItemId invId) {
    Map* m = world_.GetMap(c.mapId);
    if (!m) return false;
    auto it = std::find_if(c.inventory.begin(), c.inventory.end(), [&](const InvItem& i) { return i.id == invId; });
    if (it == c.inventory.end()) return false;
    const Proto* p = data_.GetProto(it->pid);
    if (!p) return false;
    uint16_t cnt = it->count;
    c.inventory.erase(it);
    SpawnObjectProto(m->id, *p, c.pos, cnt, nullptr);
    if (c.IsPlayer()) { SendInventory(c); SendStats(c); }
    return true;
}

bool Server::PickUp(Critter& c, ItemId groundId) {
    Map* m = world_.GetMap(c.mapId);
    if (!m || c.dead) return false;
    auto it = m->items.find(groundId);
    if (it == m->items.end() || it->second.kind != ObjKind::Ground) return false;
    if (TileDist(it->second.pos, c.pos) > 2) return false;
    const Proto* p = data_.GetProto(it->second.pid);
    if (!p) return false;
    if (c.IsPlayer() && Weight(c) + ProtoWeight(*p) * it->second.count > CarryLimit(c)) {
        Tell(c, "You cannot carry that much.");
        return false;
    }
    GiveItem(c, *p, it->second.count);
    m->items.erase(it);
    m->dirty = true;  // visibility diff removes it from every viewer
    if (c.IsPlayer()) {
        SendInventory(c);
        SendStats(c);
        Tell(c, "You pick up " + p->DisplayName() + ".");
    }
    CritterId cid = c.id;
    if (scripting_) scripting_->Call("on_pickup", &c, static_cast<int>(groundId), p->key);
    (void)cid;
    return true;
}

// Container objects and corpses (dead NPCs keep their inventory) share the loot flow: LootTarget gives the item list when the
// target is on the actor's map and within reach.
std::vector<InvItem>* Server::LootTarget(Critter& c, ItemId id, bool corpse, std::string* name) {
    Map* m = world_.GetMap(c.mapId);
    if (!m || c.dead) return nullptr;
    if (corpse) {
        Critter* t = world_.GetCritter(static_cast<CritterId>(id));
        if (!t || t == &c || !t->dead || !t->IsNpc() || t->mapId != c.mapId || TileDist(t->pos, c.pos) > 3) return nullptr;
        if (name) *name = t->name;
        return &t->inventory;
    }
    auto it = m->items.find(id);
    if (it == m->items.end() || it->second.kind != ObjKind::Container || TileDist(it->second.pos, c.pos) > 3) return nullptr;
    if (name) *name = it->second.name;
    return &it->second.contents;
}

void Server::SendContainer(Critter& c, ItemId id, bool corpse) {
    if (!c.IsPlayer()) return;
    std::string name;
    std::vector<InvItem>* cont = LootTarget(c, id, corpse, &name);
    if (!cont) return;
    gnet::Container cm;
    cm.owner = id;
    cm.isCritter = corpse ? 1 : 0;
    cm.name = name;
    for (auto& e : *cont) cm.items.push_back(MakeEntry(data_, e, [this](const Proto& q) { return ProtoWeight(q); }));
    Send(c, gnet::PackG(cm));
}

bool Server::OpenLoot(Critter& c, ItemId id, bool corpse) {
    if (!LootTarget(c, id, corpse)) return false;
    SendContainer(c, id, corpse);
    return true;
}

bool Server::TakeFrom(Critter& c, ItemId containerId, ItemId entry, bool corpse) {
    std::vector<InvItem>* contp = LootTarget(c, containerId, corpse);
    if (!contp) return false;
    auto& cont = *contp;
    bool any = false;
    for (size_t i = 0; i < cont.size();) {
        if (entry != 0 && cont[i].id != entry) { ++i; continue; }
        const Proto* p = data_.GetProto(cont[i].pid);
        if (p && (!c.IsPlayer() || Weight(c) + ProtoWeight(*p) * cont[i].count <= CarryLimit(c))) {
            GiveItem(c, *p, cont[i].count);
            cont.erase(cont.begin() + static_cast<long>(i));
            any = true;
        } else {
            if (p && c.IsPlayer()) Tell(c, "You cannot carry " + p->DisplayName() + ".");
            ++i;
        }
    }
    if (c.IsPlayer()) {
        SendInventory(c);
        SendStats(c);
        SendContainer(c, containerId, corpse);
    }
    return any;
}

bool Server::PutInto(Critter& c, ItemId containerId, ItemId entry, bool corpse) {
    std::vector<InvItem>* contp = LootTarget(c, containerId, corpse);
    if (!contp) return false;
    auto find = [&]() { return std::find_if(c.inventory.begin(), c.inventory.end(), [&](const InvItem& i) { return i.id == entry; }); };
    auto it = find();
    if (it == c.inventory.end()) return false;
    const Proto* p = data_.GetProto(it->pid);
    if (!p) return false;
    if (it->slot) EquipItem(c, entry, false);   // worn / wielded items are taken off first (updates the appearance)
    it = find();
    if (it == c.inventory.end() || it->slot) return false;
    InvItem moved = *it;
    c.inventory.erase(it);
    bool merged = false;
    if (Stackable(*p))
        for (auto& e : *contp)
            if (e.pid == moved.pid) { e.count = static_cast<uint16_t>(std::min(65535, e.count + moved.count)); merged = true; break; }
    if (!merged) contp->push_back(moved);
    if (c.IsPlayer()) {
        SendInventory(c);
        SendStats(c);
        SendContainer(c, containerId, corpse);
    }
    return true;
}

// Doors. The doorway of a door entity is the 8-connected group of 0x1b cells nearest to its position (the doorway is wider than the
// entity: Vault13 doors are 13x3..5 cells). Rules: a closed door that is not locked lets critters walk through (it opens by
// itself when somebody comes close and closes again 3 s after the last one left, like the original proximity doors); a locked
// door keeps its doorway cells blocked until it is unlocked (UnlockDoor in the original scripts). Doors that do not stand on 0x1b
// cells (outdoor entrance doors, hatches) have no footprint: they animate but never block.
void Server::ApplyDoorBlock(Map& m, Item& door) {
    const uint8_t want = (door.locked && !door.open) ? 1 : 0;
    gnet::CellsSet cs;
    for (TilePos t : door.footprint) {
        uint8_t& b = m.blocked[static_cast<size_t>(t.y) * m.width + t.x];
        if (b != want) { b = want; cs.cells.push_back({t.x, t.y, want}); }
    }
    if (cs.cells.empty()) return;
    ByteBuffer cb = gnet::PackG(cs);
    for (Critter* p : m.critters)
        if (p->IsPlayer()) Send(*p, cb);
}

void Server::SetDoor(Map& m, Item& door, bool open) {
    if (open == door.open) return;
    if (!open)
        for (TilePos t : door.footprint)
            if (m.CritterAt(t)) return;  // something stands in the doorway: refuse to close
    door.open = open;
    door.nearTick = tick_;
    ApplyDoorBlock(m, door);
    gnet::ObjectInfo oi;
    oi.id = door.id; oi.kind = gnet::ObjectKind::Door; oi.open = open ? 1 : 0;
    ByteBuffer ob = gnet::PackG(oi);
    for (Critter* p : m.critters)
        if (p->IsPlayer()) Send(*p, ob);
    if (amx_) amx_->CallObject(door.id, open ? "OnOpen" : "OnClose");
}

void Server::SetDoorLocked(Item& door, bool locked) {
    if (door.kind != ObjKind::Door || door.locked == locked) return;
    door.locked = locked;
    if (Map* m = world_.GetMap(door.mapId)) ApplyDoorBlock(*m, door);
}

Item* Server::FindObject(ItemId id, Map** mapOut) {
    for (auto& m : world_.Maps()) {
        auto it = m->items.find(id);
        if (it != m->items.end()) { if (mapOut) *mapOut = m.get(); return &it->second; }
    }
    return nullptr;
}

void Server::TickDoors(Map& m) {
    const int kNear = 5;   // cells around the doorway that make an unlocked door open
    const uint64_t closeAfter = static_cast<uint64_t>(cfg_.tickRate) * 3;
    for (auto& [id, it] : m.items) {
        if (it.kind != ObjKind::Door) continue;
        int x0 = it.pos.x, x1 = it.pos.x, y0 = it.pos.y, y1 = it.pos.y;
        for (TilePos t : it.footprint) { x0 = std::min<int>(x0, t.x); x1 = std::max<int>(x1, t.x); y0 = std::min<int>(y0, t.y); y1 = std::max<int>(y1, t.y); }
        bool nearby = false;
        for (Critter* c : m.critters)
            if (!c->dead && c->pos.x >= x0 - kNear && c->pos.x <= x1 + kNear && c->pos.y >= y0 - kNear && c->pos.y <= y1 + kNear) { nearby = true; break; }
        if (nearby) {
            it.nearTick = tick_;
            if (!it.open && !it.locked) SetDoor(m, it, true);
        } else if (it.open && tick_ - it.nearTick > closeAfter) {
            SetDoor(m, it, false);
        }
    }
}

bool Server::UseObject(Critter& c, ItemId objId) {
    Map* m = world_.GetMap(c.mapId);
    if (!m || c.dead) return false;
    auto it = m->items.find(objId);
    if (it == m->items.end()) return false;
    Item& o = it->second;
    if (TileDist(o.pos, c.pos) > 3) return false;
    switch (o.kind) {
        case ObjKind::Door:
            if (o.locked && !o.open) { if (c.IsPlayer()) Tell(c, "The door is locked."); break; }
            SetDoor(*m, o, !o.open);
            break;
        case ObjKind::Container:
            SendContainer(c, o.id, false);
            break;
        case ObjKind::Usable: break;
        case ObjKind::Ground: return false;
    }
    const Proto* p = data_.GetProto(o.pid);
    if (scripting_) scripting_->Call("on_use", &c, static_cast<int>(objId), p ? p->key : std::string(), o.script);
    return true;
}

// Reach of the walk-then-act interactions in cells. The walk stops on a cell within this distance and the action re-checks the
// same limit on arrival: a pickup needs 2 (PickUp), use / take / put / loot 3. (Before, the walk went up to 3 cells away and
// PickUp then failed with "invalid" when the player came from afar.)
static int InteractRange(PendingType t) { return t == PendingType::Pickup ? 2 : 3; }

void Server::BeginInteract(Critter& c, PendingType type, ItemId id, ItemId sub, bool critter) {
    Map* m = world_.GetMap(c.mapId);
    if (!m || c.dead) return;
    TilePos op;
    if (critter) {
        Critter* t = world_.GetCritter(static_cast<CritterId>(id));
        if (!t || t->mapId != c.mapId) return;
        op = t->pos;
    } else {
        auto it = m->items.find(id);
        if (it == m->items.end()) return;
        op = it->second.pos;
    }
    c.pending = Pending{type, id, sub, critter};
    const int range = InteractRange(type);
    if (TileDist(op, c.pos) <= range) { StopMove(c); RunPending(c); return; }
    // stand on a free cell within reach of the object, nearest to us first (the object may sit inside a wall)
    std::vector<TilePos> spots;
    for (int dy = -range; dy <= range; ++dy)
        for (int dx = -range; dx <= range; ++dx) {
            TilePos t{static_cast<int16_t>(op.x + dx), static_cast<int16_t>(op.y + dy)};
            if (m->IsFree(t)) spots.push_back(t);
        }
    std::sort(spots.begin(), spots.end(), [&](TilePos a, TilePos b) { return TileDist(a, c.pos) < TileDist(b, c.pos); });
    bool ok = false;
    for (size_t i = 0; i < spots.size() && i < 6 && !ok; ++i) ok = MoveCritterTo(c, spots[i]);
    if (!ok) {
        c.pending = Pending{};
        if (c.IsPlayer()) Tell(c, "You cannot reach that.");
    }
}

void Server::TalkTo(Critter& c, CritterId npcId) {
    Critter* n = world_.GetCritter(npcId);
    if (!amx_ || !n || n == &c || c.dead || n->dead || n->mapId != c.mapId || n->dialog.empty()) return;
    Map* m = world_.GetMap(c.mapId);
    if (!m) return;
    c.pending = Pending{};
    if (TileDist(n->pos, c.pos) <= 3) { StopMove(c); c.talkTarget = 0; amx_->StartDialog(c, *n); return; }
    std::vector<TilePos> spots;   // free cells next to the NPC, nearest to the player first
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx) {
            TilePos t{static_cast<int16_t>(n->pos.x + dx), static_cast<int16_t>(n->pos.y + dy)};
            if (m->IsFree(t)) spots.push_back(t);
        }
    std::sort(spots.begin(), spots.end(), [&](TilePos a, TilePos b) { return TileDist(a, c.pos) < TileDist(b, c.pos); });
    bool ok = false;
    for (size_t i = 0; i < spots.size() && i < 6 && !ok; ++i) ok = MoveCritterTo(c, spots[i]);
    if (ok) c.talkTarget = npcId;
    else Tell(c, "You cannot reach that.");
}

void Server::RunTalk(Critter& c) {
    Critter* n = world_.GetCritter(c.talkTarget);
    if (!amx_ || !n || n->dead || n->mapId != c.mapId) { c.talkTarget = 0; return; }
    if (TileDist(n->pos, c.pos) <= 3) { c.talkTarget = 0; StopMove(c); amx_->StartDialog(c, *n); }
    else if (!c.mv.active) c.talkTarget = 0;   // arrived somewhere else / path blocked
}

// Runs the interaction once the critter has arrived AND is free (breaktime over, enough AP); until then it stays pending.
void Server::RunPending(Critter& c) {
    Pending p = c.pending;
    uint8_t action = 0;
    switch (p.type) {
        case PendingType::Pickup: action = static_cast<uint8_t>(GMsg::C2S_PickUp); break;
        case PendingType::Use: action = static_cast<uint8_t>(GMsg::C2S_Use); break;
        case PendingType::Take: action = static_cast<uint8_t>(GMsg::C2S_Take); break;
        case PendingType::Put: action = static_cast<uint8_t>(GMsg::C2S_Put); break;
        case PendingType::Loot: action = static_cast<uint8_t>(GMsg::C2S_Loot); break;
        case PendingType::None: return;
    }
    // the target must still be there and within reach; while the walk is under way keep waiting, if it ended short (blocked,
    // interrupted) the interaction is dropped with a message instead of failing later as "invalid"
    {
        Map* pm = world_.GetMap(c.mapId);
        TilePos tp;
        bool have = false;
        if (p.critter) { if (Critter* t = world_.GetCritter(static_cast<CritterId>(p.id)); t && t->mapId == c.mapId) { tp = t->pos; have = true; } }
        else if (pm) { auto it = pm->items.find(p.id); if (it != pm->items.end()) { tp = it->second.pos; have = true; } }
        if (!have) { c.pending = Pending{}; return; }
        if (TileDist(tp, c.pos) > InteractRange(p.type)) {
            if (!c.mv.active) { c.pending = Pending{}; if (c.IsPlayer()) Tell(c, "You cannot reach that."); }
            return;
        }
    }
    int cost = ActionCost(action);
    gnet::ActionResult gate = CheckAction(c, action, cost, false);
    if (gate == gnet::ActionResult::Busy || gate == gnet::ActionResult::NoAp) return;  // try again next tick
    c.pending = Pending{};
    if (gate != gnet::ActionResult::Ok) return;
    bool ok = false;
    switch (p.type) {
        case PendingType::Pickup: ok = PickUp(c, p.id); break;
        case PendingType::Use: ok = UseObject(c, p.id); break;
        case PendingType::Take: ok = TakeFrom(c, p.id, p.sub, p.critter); break;
        case PendingType::Put: ok = PutInto(c, p.id, p.sub, p.critter); break;
        case PendingType::Loot: ok = OpenLoot(c, p.id, true); break;
        case PendingType::None: break;
    }
    if (ok) CommitAction(c, action, cost, rt_.Breaktime);
    else if (c.IsPlayer()) SendWaitState(c, action, gnet::ActionResult::Invalid);
}

// ------------------------------------------------------------------ transitions & regions

bool Server::PickEntryPos(Map& dst, int entryId, TilePos& out, Dir& dir) {
    TilePos pos = dst.spawn;
    if (const EntryPoint* ep = dst.FindEntry(entryId)) {
        bool found = false;
        for (auto& s : ep->slots)
            if (dst.IsFree(s.pos)) { pos = s.pos; found = true; break; }
        if (!found && !ep->slots.empty()) pos = ep->slots[0].pos;
        if (!ep->slots.empty()) dir = DirFromYaw(ep->slots[0].yaw);
    }
    out = pos;
    return dst.IsFree(pos) || dst.FindFreeNear(pos, 12, out);
}

bool Server::EnterMap(Critter& c, Map& dst, int entryId) {
    TilePos spot;
    Dir dir = c.dir;
    if (!PickEntryPos(dst, entryId, spot, dir)) return false;
    c.dir = dir;
    bool ok = Teleport(c, dst.id, spot);
    if (ok) {
        c.regionLockUntilMs = NowMs() + static_cast<uint64_t>(std::max(0, rt_.TransferLock) + std::max(0, rt_.TimeoutTransfer));
        c.waitEndMs = 0;  // a transfer resets the wait (FOnline: player.Wait(0) after transfers)
        c.inRegion = -1;
        if (c.IsPlayer()) { SendStats(c); SendInventory(c); SavePlayer(c); }
    }
    return ok;
}

bool Server::EnterMapByName(Critter& c, const std::string& name, int entryId) {
    Map* m = world_.FindMapByName(name);
    return m && EnterMap(c, *m, entryId);
}

void Server::CheckRegions(Critter& c) {
    Map* m = world_.GetMap(c.mapId);
    if (!m || !c.IsPlayer() || c.dead) return;
    float cx = c.fx, cy = c.fy;
    int inside = -1;
    for (size_t i = 0; i < m->regions.size(); ++i) {
        const Region& r = m->regions[i];
        if (r.kind != 0 && r.kind != 1) continue;
        if (!r.Contains(cx, cy)) continue;
        if (r.kind == 0) {
            if (NowMs() < c.regionLockUntilMs) return;
            if (NowMs() < c.battleEndMs) return;  // no map transfer while the battle flag is set
            Map* dst = world_.FindMapByName(r.targetMap);
            if (!dst) {
                if (std::find(warnedMaps_.begin(), warnedMaps_.end(), r.targetMap) == warnedMaps_.end()) {
                    warnedMaps_.push_back(r.targetMap);
                    log::Warn("transition to unknown map '%s'", r.targetMap.c_str());
                }
                return;
            }
            log::Info("'%s' crosses transition %s -> %s (entry %d)", c.name.c_str(), m->name.c_str(), dst->name.c_str(), r.targetEntry);
            EnterMap(c, *dst, r.targetEntry);
            return;
        }
        inside = static_cast<int>(i);
    }
    if (inside != c.inRegion) {
        CritterId id = c.id;
        int old = c.inRegion;
        c.inRegion = inside;
        if (scripting_) {
            if (old >= 0 && old < static_cast<int>(m->regions.size())) scripting_->Call("on_region_leave", &c, m->regions[old].script);
            Critter* cc = world_.GetCritter(id);
            if (cc && inside >= 0) scripting_->Call("on_region_enter", cc, m->regions[inside].script);
        }
    }
}

// ------------------------------------------------------------------ combat

namespace {
struct AttackPlan {
    bool ranged = false;
    int rangeCells = 2;
    int dmgLo = 1, dmgHi = 3;
    int cost = 10;
    const Proto* weapon = nullptr;
    const Proto* ammo = nullptr;
    ItemId ammoInv = 0;
};

AttackPlan PlanAttack(const Critter& c, const DataStore& d) {
    AttackPlan a;
    int meleeBonus = std::max(0, c.special[0] - 5);
    const Proto* w = EquippedWeapon(c, d);
    a.weapon = w;
    if (w) {
        a.cost = !w->modes.empty() ? w->modes[0].ap : 10;
        if (w->caliber >= 0) {
            a.ranged = true;
            a.rangeCells = std::max(4, w->range * 2);
            for (auto& it : c.inventory) {
                const Proto* p = d.GetProto(it.pid);
                if (p && p->cls == PClass::Ammo && p->ammoCaliber == w->caliber) { a.ammo = p; a.ammoInv = it.id; break; }
            }
            if (a.ammo && a.ammo->ammoDmgMax > 0) { a.dmgLo = a.ammo->ammoDmgMin; a.dmgHi = a.ammo->ammoDmgMax; }
            else if (w->wDmgMax > 0) { a.dmgLo = static_cast<int>(w->wDmgMin); a.dmgHi = static_cast<int>(w->wDmgMax); }
            else { a.dmgLo = 3; a.dmgHi = 8; }
            return a;
        }
        // melee weapon: first mode with a damage range, else the weapon's base damage
        for (auto& m : w->modes)
            if (m.dmgMax > 0) { a.dmgLo = m.dmgMin + meleeBonus; a.dmgHi = m.dmgMax + meleeBonus; a.cost = m.ap; return a; }
        if (w->wDmgMax > 0) { a.dmgLo = static_cast<int>(w->wDmgMin) + meleeBonus; a.dmgHi = static_cast<int>(w->wDmgMax) + meleeBonus; }
        else { a.dmgLo = 2 + meleeBonus; a.dmgHi = 6 + meleeBonus; }
        return a;
    }
    // unarmed: strongest attack mode of the critter prototype (animals bite 2-4, the player punches 1-2/1-3)
    a.dmgLo = 1 + meleeBonus; a.dmgHi = 3 + meleeBonus;
    if (c.protoPid >= 0)
        if (const Proto* p = d.GetProto(static_cast<uint16_t>(c.protoPid)))
            for (auto& m : p->modes)
                if (m.dmgMax >= a.dmgHi - meleeBonus) { a.dmgLo = m.dmgMin + meleeBonus; a.dmgHi = m.dmgMax + meleeBonus; a.cost = m.ap; }
    return a;
}
}  // namespace

bool Server::StartAttack(Critter& c, CritterId targetId) {
    if (c.dead) return false;
    if (targetId == 0) { c.target = 0; return true; }
    Critter* t = world_.GetCritter(targetId);
    if (!t || t == &c || t->dead || t->mapId != c.mapId) return false;
    c.target = targetId;
    c.nextRepathTick = 0;
    return true;
}

// Can `c` attack `t` right now without moving (weapon range and line of sight, same test as TickCombat)? Used by A_JustAttack.
bool Server::InAttackRange(const Critter& c, const Critter& t) {
    Map* m = world_.GetMap(c.mapId);
    if (!m || t.mapId != c.mapId) return false;
    AttackPlan plan = PlanAttack(c, data_);
    int dist = TileDist(c.pos, t.pos);
    return dist <= plan.rangeCells && (dist <= 1 || HasLos(*m, c.pos, t.pos));
}

void Server::TickCombat(Critter& c) {
    if (c.dead || c.target == 0) return;
    Critter* t = world_.GetCritter(c.target);
    Map* m = world_.GetMap(c.mapId);
    if (!t || !m || t->dead || t->mapId != c.mapId) { c.target = 0; return; }
    AttackPlan plan = PlanAttack(c, data_);
    int dist = TileDist(c.pos, t->pos);
    bool los = dist <= 1 || HasLos(*m, c.pos, t->pos);
    if (dist > plan.rangeCells || !los) {
        if (c.isStatic || !IsFree(c)) return;
        if (!c.IsMoving() || tick_ >= c.nextRepathTick) {
            c.nextRepathTick = tick_ + 10;
            MoveCritterTo(c, t->pos);
        }
        return;
    }
    if (!IsFree(c)) return;  // breaktime of the previous action is still running
    int apCost = AttackApCost(plan.cost);
    SyncAp(c);
    if (c.ap < apCost) return;  // wait for AP regeneration
    StopMove(c, 1);
    if (plan.ranged && !plan.ammo && c.IsPlayer()) {
        Tell(c, "You are out of ammo.");
        c.target = 0;
        return;
    }
    // one attack per action duration: the raw weapon cost word -> milliseconds (guess: cost * AttackMsPerCost, clamped)
    int durMs = std::clamp(plan.cost * rt_.AttackMsPerCost, rt_.AttackMinMs, std::max(rt_.AttackMinMs, rt_.AttackMaxMs));
    SetBattle(c, false);
    SetBattle(*t, true);
    CommitAction(c, static_cast<uint8_t>(GMsg::C2S_Attack), apCost, durMs);
    ++c.attacksDone;
    int dx = t->pos.x - c.pos.x, dy = t->pos.y - c.pos.y;
    c.dir = DirFromDelta(dx, dy, c.dir);
    if (plan.ranged && plan.ammo && c.IsPlayer()) {
        for (auto it = c.inventory.begin(); it != c.inventory.end(); ++it)
            if (it->id == plan.ammoInv) {
                if (--it->count == 0) c.inventory.erase(it);
                break;
            }
        SendInventory(c);
    }

    // hit chance (guess): 45 + 4*Agility (+2*Strength melee) - 2/unit beyond Perception - target AC
    ArmorInfo ta = WornArmor(*t, data_);
    int ac = t->special[5] + ta.ac;
    int chance = 45 + 4 * c.special[5] - ac;
    if (plan.ranged) chance -= 2 * std::max(0, dist / 2 - c.special[1]);
    else chance += 2 * c.special[0];
    chance = std::clamp(chance, 5, 95);

    gnet::CombatEvent ev;
    ev.attacker = c.id;
    ev.target = t->id;
    ev.durMs = static_cast<uint16_t>(std::clamp(durMs, 0, 65535));
    bool hit = !rt_.ForceMiss && RandRange(1, 100) <= chance;
    OnAttacked(*t, c);   // hit or miss: the target now knows its attacker
    int dmg = 0;
    if (hit) {
        dmg = RandRange(plan.dmgLo, plan.dmgHi);
        bool crit = RandRange(1, 100) <= c.special[6];  // Luck % (guess)
        if (crit) dmg *= 2;
        // threshold reduction, but never below a quarter of the roll (guess)
        dmg = std::max({dmg - ta.dt, dmg / 4, 1});
        ev.result = crit ? gnet::HitResult::Critical : gnet::HitResult::Hit;
    }
    ev.damage = static_cast<int16_t>(dmg);
    CritterId attackerId = c.id, targetId = t->id;
    if (hit) Damage(*t, dmg, &c);
    Critter* tt = world_.GetCritter(targetId);
    if (tt) {
        ev.targetHp = static_cast<int16_t>(std::max(0, tt->hp));
        if (tt->dead) ev.result = gnet::HitResult::Kill;
    }
    ByteBuffer b = gnet::PackG(ev);
    for (Critter* p : m->critters)
        if (p->IsPlayer() && TileDist(p->pos, c.pos) <= kViewCells) Send(*p, b);
    (void)attackerId;
}

// Every attack attempt (hit or miss) against a critter: the NPC target turns on the attacker, same-team NPCs close by join
// (AggroAlertRadius), and the Lua hook on_attacked(target, attacker) runs. Static NPCs never move but keep the rule (no target).
void Server::OnAttacked(Critter& t, Critter& attacker) {
    Map* m = world_.GetMap(t.mapId);
    if (t.dead || !m || t.id == attacker.id) return;
    CritterId aid = attacker.id;
    if (t.IsNpc() && !t.isStatic && t.target == 0 && attacker.mapId == t.mapId) { t.target = aid; t.nextRepathTick = 0; }
    if (t.IsNpc() && rt_.AggroAlertRadius > 0) {
        for (Critter* o : m->critters)
            if (o != &t && o != &attacker && o->IsNpc() && !o->isStatic && !o->dead && o->target == 0 && o->team == t.team &&
                attacker.team != o->team && TileDist(o->pos, t.pos) <= rt_.AggroAlertRadius) {
                o->target = aid;
                o->nextRepathTick = 0;
            }
    }
    if (amx_) {
        Critter* tp = world_.GetCritter(t.id);
        Critter* ap = world_.GetCritter(aid);
        if (tp && ap) amx_->OnAttacked(*tp, *ap);
    }
    if (scripting_) {
        Critter* tp = world_.GetCritter(t.id);
        Critter* ap = world_.GetCritter(aid);
        if (tp && ap) scripting_->Call("on_attacked", tp, ap);
    }
}

void Server::Damage(Critter& t, int amount, Critter* attacker) {
    if (t.dead || amount <= 0) return;
    CritterId tid = t.id, aid = attacker ? attacker->id : 0;
    t.hp -= amount;
    if (attacker && t.IsNpc() && !t.isStatic && t.target == 0 && attacker->mapId == t.mapId) t.target = aid;  // retaliate
    if (t.hp <= 0) { t.hp = 0; KillCritter(t, attacker); }
    else { SendCritterState(t); if (t.IsPlayer()) SendStats(t); }
    if (scripting_) {
        Critter* tp = world_.GetCritter(tid);
        Critter* ap = aid ? world_.GetCritter(aid) : nullptr;
        if (tp) scripting_->Call("on_damage", tp, ap, amount);
    }
}

void Server::Heal(Critter& c, int amount) {
    if (c.dead) return;
    c.hp = std::min(c.maxHp, c.hp + amount);
    SendCritterState(c);
    if (c.IsPlayer()) SendStats(c);
}

void Server::DropAllToGround(Critter& c) {
    Map* m = world_.GetMap(c.mapId);
    if (!m) return;
    std::vector<InvItem> items = std::move(c.inventory);
    c.inventory.clear();
    for (auto& it : items)
        if (const Proto* p = data_.GetProto(it.pid)) {
            TilePos spot = c.pos;
            SpawnObjectProto(m->id, *p, spot, it.count, nullptr);
        }
}

void Server::KillCritter(Critter& c, Critter* killer) {
    if (c.dead) return;
    c.dead = true;
    c.hp = 0;
    c.deadSince = tick_;
    StopMove(c, 1);
    c.target = 0;
    c.pending = Pending{};
    CritterId cid = c.id, kid = killer ? killer->id : 0;
    // an NPC corpse is a container: its inventory stays on it (Loot / Take), see LootTarget
    SendCritterState(c);
    if (c.IsPlayer()) { SendStats(c); Tell(c, "You have died."); }
    if (amx_) amx_->OnDeath(c);
    if (c.IsNpc() && !c.isStatic && killer && killer->IsPlayer()) AddXp(*killer, KillXp(c));
    if (Map* m = world_.GetMap(c.mapId)) m->dirty = true;
    if (scripting_) {
        Critter* cp = world_.GetCritter(cid);
        Critter* kp = kid ? world_.GetCritter(kid) : nullptr;
        if (cp) scripting_->Call("on_death", cp, kp);
    }
}

void Server::Respawn(Critter& c) {
    Map* m = world_.GetMap(c.mapId);
    if (!m) return;
    c.dead = false;
    c.hp = c.maxHp;
    c.target = 0;
    c.ap = -1;
    c.waitEndMs = 0;
    c.battleEndMs = 0;
    EnterMap(c, *m, 0);
    SendCritterState(c);
    SendStats(c);
    Tell(c, "You wake up again.");
}

// ------------------------------------------------------------------ game packets

void Server::HandleGamePacket(ENetPeer*, Critter* c, GMsg id, ByteBuffer& b, bool& ok) {
    auto budget = [&]() {
        if (!c || c->msgBudget <= 0) return false;
        --c->msgBudget;
        return true;
    };
    switch (id) {
        // Item actions are gated like FOnline: rejected while the critter is busy (breaktime) or short of AP; the answer is
        // always a WaitState. Walk-then-act interactions are gated at request time and again on arrival (RunPending).
        case GMsg::C2S_PickUp: {
            gnet::PickUp m; ok = m.Read(b);
            if (ok && budget() && CheckAction(*c, static_cast<uint8_t>(id), ActionCost(static_cast<uint8_t>(id))) == gnet::ActionResult::Ok)
                BeginInteract(*c, PendingType::Pickup, m.id, 0);
            break;
        }
        case GMsg::C2S_Drop: {
            gnet::Drop m; ok = m.Read(b);
            if (ok && budget() && CheckAction(*c, static_cast<uint8_t>(id), ActionCost(static_cast<uint8_t>(id))) == gnet::ActionResult::Ok) {
                if (DropItem(*c, m.id)) CommitAction(*c, static_cast<uint8_t>(id), ActionCost(static_cast<uint8_t>(id)), rt_.Breaktime);
                else SendWaitState(*c, static_cast<uint8_t>(id), gnet::ActionResult::Invalid);
            }
            break;
        }
        case GMsg::C2S_Equip: {
            gnet::Equip m; ok = m.Read(b);
            if (ok && budget() && CheckAction(*c, static_cast<uint8_t>(id), ActionCost(static_cast<uint8_t>(id))) == gnet::ActionResult::Ok) {
                if (EquipItem(*c, m.id, m.on != 0, m.slot)) CommitAction(*c, static_cast<uint8_t>(id), ActionCost(static_cast<uint8_t>(id)), rt_.Breaktime);
                else SendWaitState(*c, static_cast<uint8_t>(id), gnet::ActionResult::Invalid);
            }
            break;
        }
        case GMsg::C2S_Use: {
            gnet::Use m; ok = m.Read(b);
            if (ok && budget() && CheckAction(*c, static_cast<uint8_t>(id), ActionCost(static_cast<uint8_t>(id))) == gnet::ActionResult::Ok)
                BeginInteract(*c, PendingType::Use, m.id, 0);
            break;
        }
        case GMsg::C2S_Attack: {
            gnet::Attack m; ok = m.Read(b);
            if (ok && budget()) StartAttack(*c, m.id);
            break;
        }
        case GMsg::C2S_Take: {
            gnet::Take m; ok = m.Read(b);
            if (ok && budget() && CheckAction(*c, static_cast<uint8_t>(id), ActionCost(static_cast<uint8_t>(id))) == gnet::ActionResult::Ok)
                BeginInteract(*c, PendingType::Take, m.container, m.entry, m.isCritter != 0);
            break;
        }
        case GMsg::C2S_Put: {
            gnet::Put m; ok = m.Read(b);
            if (ok && budget() && CheckAction(*c, static_cast<uint8_t>(id), ActionCost(static_cast<uint8_t>(id))) == gnet::ActionResult::Ok)
                BeginInteract(*c, PendingType::Put, m.container, m.entry, m.isCritter != 0);
            break;
        }
        case GMsg::C2S_Loot: {
            gnet::Loot m; ok = m.Read(b);
            if (ok && budget() && CheckAction(*c, static_cast<uint8_t>(id), ActionCost(static_cast<uint8_t>(id))) == gnet::ActionResult::Ok)
                BeginInteract(*c, PendingType::Loot, m.id, 0, true);
            break;
        }
        case GMsg::C2S_Examine: {
            gnet::Examine m; ok = m.Read(b);
            if (ok && budget()) {
                std::string s = Examine(*c, m.id, m.isCritter != 0);
                if (!s.empty()) Tell(*c, s);
            }
            break;
        }
        case GMsg::C2S_ReqState:
            ok = b.Ok();
            if (ok && budget()) { SendStats(*c); SendCharSheet(*c); SendInventory(*c); SendWaitState(*c); }
            break;
        case static_cast<GMsg>(GMsgChar::C2S_CharCreate): {
            gnet::CharCreate m; ok = m.Read(b);
            if (ok && budget()) CreateCharacter(*c, m);
            break;
        }
        case static_cast<GMsg>(GMsgChar::C2S_SpendSkill): {
            gnet::SpendSkill m; ok = m.Read(b);
            if (ok && budget()) SpendSkillPoints(*c, m.skill, m.points);
            break;
        }
        case static_cast<GMsg>(GMsgChar::C2S_UseSkill): {
            gnet::UseSkill m; ok = m.Read(b);
            if (ok && budget()) UseSkill(*c, m.skill, m.kind, m.target);
            break;
        }
        case static_cast<GMsg>(GMsgDlg::C2S_Talk): {
            gnet::Talk m; ok = m.Read(b);
            if (ok && budget()) TalkTo(*c, m.critter);
            break;
        }
        case static_cast<GMsg>(GMsgDlg::C2S_DialogChoose): {
            gnet::DialogChoose m; ok = m.Read(b);
            if (ok && budget() && amx_) amx_->DialogChoose(*c, m.index);
            break;
        }
        case static_cast<GMsg>(GMsgDlg::C2S_DialogClose):
            ok = b.Ok();
            if (ok && amx_) amx_->DialogClose(*c, false);
            break;
        case static_cast<GMsg>(GMsgRt::C2S_ClientCaps): {
            gnet::ClientCaps m; ok = m.Read(b);
            if (ok) {
                bool first = !(c->caps & gnet::kCapContinuousMove);
                c->caps = m.caps;
                if (first && (c->caps & gnet::kCapContinuousMove)) { SendWaitState(*c); SendCapsResync(*c); }
            }
            break;
        }
        case static_cast<GMsg>(GMsgRt::C2S_MoveToF): {
            gnet::MoveToF m; ok = m.Read(b);
            if (ok) OnMoveReqF(*c, m);
            break;
        }
        default: ok = false; break;
    }
}

}  // namespace f3
