// Gameplay on top of the data-driven world: loading maps and entity instances, stats, inventory,
// doors/containers, map transitions and a first-pass real-time combat system.
// Numbers marked "guess" are placeholders: the original formulas are not decoded yet (docs/server-data.md).
#include <enet/enet.h>

#include <algorithm>
#include <cmath>
#include <random>
#include "Scripting.h"
#include "Server.h"
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

bool Server::LoadWorldData() {
    if (!data_.Loaded()) return false;
    size_t entities = 0, skipped = 0;
    for (const std::string& name : data_.MapNames()) {
        MapDef def;
        std::string err;
        if (!data_.LoadMap(name, def, err)) { log::Warn("data: map %s: %s", name.c_str(), err.c_str()); continue; }
        std::vector<uint8_t> blocked(def.walk.size());
        for (size_t i = 0; i < blocked.size(); ++i) blocked[i] = def.walk[i] == 0x2f ? 1 : 0;
        TilePos spawn{static_cast<int16_t>(def.width / 2), static_cast<int16_t>(def.height / 2)};
        Map* m = world_.CreateMap(def.name, def.width, def.height, std::move(blocked), spawn);
        for (auto& ep : def.entries) {
            EntryPoint e;
            e.id = ep.id;
            for (auto& s : ep.slots) e.slots.push_back({CellOf(s.x, s.z), s.yaw});
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
                rg.poly.emplace_back(p.first * 2.0f, p.second * 2.0f);
                rg.minx = std::min(rg.minx, p.first * 2.0f); rg.maxx = std::max(rg.maxx, p.first * 2.0f);
                rg.miny = std::min(rg.miny, p.second * 2.0f); rg.maxy = std::max(rg.maxy, p.second * 2.0f);
            }
            m->regions.push_back(std::move(rg));
        }
        for (auto& path : def.paths) {
            std::vector<TilePos> cells;
            for (auto& p : path) cells.push_back(CellOf(p.first, p.second));
            m->paths.push_back(std::move(cells));
        }
        size_t before = m->critters.size() + m->items.size();
        for (const EntityDef& e : def.entities) {
            const Proto* p = data_.FindProto(e.proto);
            if (!p) { ++skipped; continue; }  // stale template reference in the original data
            TilePos pos = CellOf(e.x, e.z);
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
    c.maxHp = std::max(1, 15 + c.special[0] + 2 * c.special[2]);
    c.hp = c.maxHp;
    c.team = p.team;
    c.isStatic = p.isStatic;
    c.script = p.script;
    c.dialog = p.dialog;
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
            // the closed door sits on blocked cells of the grid; remember the 3x3 blocked cells to release when open
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    if (m->IsBlocked(pos.x + dx, pos.y + dy) && m->InBounds(pos.x + dx, pos.y + dy))
                        it.footprint.push_back(TilePos{static_cast<int16_t>(pos.x + dx), static_cast<int16_t>(pos.y + dy)});
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

int Server::Weight(const Critter& c) const {
    int w = 0;
    for (auto& it : c.inventory)
        if (const Proto* p = data_.GetProto(it.pid)) w += p->weight * it.count;
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
const Proto* EquippedWeapon(const Critter& c, const DataStore& d) {
    for (auto& it : c.inventory)
        if (it.equipped)
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
    s.carryWeight = static_cast<uint16_t>(25 + 25 * c.special[0]);
    s.carried = static_cast<uint16_t>(Weight(c));
    Send(c, gnet::PackG(s));
}

static gnet::InvEntry MakeEntry(const DataStore& d, const InvItem& it) {
    gnet::InvEntry e;
    e.id = it.id; e.pid = it.pid; e.count = it.count; e.equipped = it.equipped ? 1 : 0;
    if (const Proto* p = d.GetProto(it.pid)) e.name = p->DisplayName();
    return e;
}

void Server::SendInventory(Critter& c) {
    if (!c.peer) return;
    gnet::Inventory m;
    m.owner = c.id;
    for (auto& it : c.inventory) m.items.push_back(MakeEntry(data_, it));
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

bool Server::EquipItem(Critter& c, ItemId invId, bool on) {
    InvItem* target = nullptr;
    for (auto& it : c.inventory)
        if (it.id == invId) target = &it;
    if (!target) return false;
    const Proto* p = data_.GetProto(target->pid);
    if (!p || !Equippable(*p)) return false;
    if (on)
        for (auto& it : c.inventory)
            if (&it != target && it.equipped)
                if (const Proto* q = data_.GetProto(it.pid))
                    if (q->cls == p->cls) it.equipped = false;
    target->equipped = on;
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
    if (c.IsPlayer() && Weight(c) + p->weight * it->second.count > 25 + 25 * c.special[0]) {
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

bool Server::TakeFrom(Critter& c, ItemId containerId, ItemId entry) {
    Map* m = world_.GetMap(c.mapId);
    if (!m || c.dead) return false;
    auto it = m->items.find(containerId);
    if (it == m->items.end() || it->second.kind != ObjKind::Container) return false;
    if (TileDist(it->second.pos, c.pos) > 3) return false;
    auto& cont = it->second.contents;
    bool any = false;
    for (size_t i = 0; i < cont.size();) {
        if (entry != 0 && cont[i].id != entry) { ++i; continue; }
        const Proto* p = data_.GetProto(cont[i].pid);
        if (p && (!c.IsPlayer() || Weight(c) + p->weight * cont[i].count <= 25 + 25 * c.special[0])) {
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
        gnet::Container cm;
        cm.owner = containerId;
        for (auto& e : cont) cm.items.push_back(MakeEntry(data_, e));
        Send(c, gnet::PackG(cm));
    }
    return any;
}

void Server::SetDoor(Map& m, Item& door, bool open) {
    gnet::CellsSet cs;
    for (TilePos t : door.footprint) {
        if (!open && m.CritterAt(t)) return;  // something stands in the doorway: refuse to close
    }
    door.open = open;
    for (TilePos t : door.footprint) {
        m.blocked[static_cast<size_t>(t.y) * m.width + t.x] = open ? 0 : 1;
        cs.cells.push_back({t.x, t.y, static_cast<uint8_t>(open ? 0 : 1)});
    }
    ByteBuffer cb = gnet::PackG(cs), ob;
    gnet::ObjectInfo oi;
    oi.id = door.id; oi.kind = gnet::ObjectKind::Door; oi.open = open ? 1 : 0;
    ob = gnet::PackG(oi);
    for (Critter* p : m.critters)
        if (p->IsPlayer()) { if (!cs.cells.empty()) Send(*p, cb); Send(*p, ob); }
}

bool Server::UseObject(Critter& c, ItemId objId) {
    Map* m = world_.GetMap(c.mapId);
    if (!m || c.dead) return false;
    auto it = m->items.find(objId);
    if (it == m->items.end()) return false;
    Item& o = it->second;
    if (TileDist(o.pos, c.pos) > 3) return false;
    switch (o.kind) {
        case ObjKind::Door: SetDoor(*m, o, !o.open); break;
        case ObjKind::Container:
            if (c.IsPlayer()) {
                gnet::Container cm;
                cm.owner = o.id;
                for (auto& e : o.contents) cm.items.push_back(MakeEntry(data_, e));
                Send(c, gnet::PackG(cm));
            }
            break;
        case ObjKind::Usable: break;
        case ObjKind::Ground: return false;
    }
    const Proto* p = data_.GetProto(o.pid);
    if (scripting_) scripting_->Call("on_use", &c, static_cast<int>(objId), p ? p->key : std::string());
    return true;
}

void Server::BeginInteract(Critter& c, PendingType type, ItemId id, ItemId sub) {
    Map* m = world_.GetMap(c.mapId);
    if (!m || c.dead) return;
    auto it = m->items.find(id);
    if (it == m->items.end()) return;
    c.pending = Pending{type, id, sub};
    if (TileDist(it->second.pos, c.pos) <= 2) { c.path.clear(); RunPending(c); return; }
    // stand on a free cell within 3 cells of the object, nearest to us first (the object may sit inside a wall)
    std::vector<TilePos> spots;
    TilePos op = it->second.pos;
    for (int dy = -3; dy <= 3; ++dy)
        for (int dx = -3; dx <= 3; ++dx) {
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

void Server::RunPending(Critter& c) {
    Pending p = c.pending;
    c.pending = Pending{};
    switch (p.type) {
        case PendingType::Pickup: PickUp(c, p.id); break;
        case PendingType::Use: UseObject(c, p.id); break;
        case PendingType::Take: TakeFrom(c, p.id, p.sub); break;
        case PendingType::None: break;
    }
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
        c.regionLockUntil = tick_ + static_cast<uint64_t>(cfg_.tickRate) * 3;
        c.inRegion = -1;
        if (c.IsPlayer()) { SendStats(c); SendInventory(c); }
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
    float cx = c.pos.x + 0.5f, cy = c.pos.y + 0.5f;
    int inside = -1;
    for (size_t i = 0; i < m->regions.size(); ++i) {
        const Region& r = m->regions[i];
        if (r.kind != 0 && r.kind != 1) continue;
        if (!r.Contains(cx, cy)) continue;
        if (r.kind == 0) {
            if (tick_ < c.regionLockUntil) return;
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

void Server::TickCombat(Critter& c) {
    if (c.dead || c.target == 0) return;
    Critter* t = world_.GetCritter(c.target);
    Map* m = world_.GetMap(c.mapId);
    if (!t || !m || t->dead || t->mapId != c.mapId) { c.target = 0; return; }
    AttackPlan plan = PlanAttack(c, data_);
    int dist = TileDist(c.pos, t->pos);
    bool los = dist <= 1 || HasLos(*m, c.pos, t->pos);
    if (dist > plan.rangeCells || !los) {
        if (c.isStatic) return;
        if (c.path.empty() || tick_ >= c.nextRepathTick) {
            c.nextRepathTick = tick_ + 10;
            MoveCritterTo(c, t->pos);
        }
        return;
    }
    c.path.clear();
    if (tick_ < c.nextAttackTick) return;
    if (plan.ranged && !plan.ammo && c.IsPlayer()) {
        Tell(c, "You are out of ammo.");
        c.target = 0;
        return;
    }
    // one attack per cooldown; the raw weapon "cost" word is turned into seconds (guess: cost/10, 0.5..4 s)
    double secs = std::clamp(plan.cost / 10.0, 0.5, 4.0);
    c.nextAttackTick = tick_ + static_cast<uint64_t>(secs * cfg_.tickRate);
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
    bool hit = RandRange(1, 100) <= chance;
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
    c.path.clear();
    c.target = 0;
    c.pending = Pending{};
    CritterId cid = c.id, kid = killer ? killer->id : 0;
    if (c.IsNpc()) DropAllToGround(c);
    SendCritterState(c);
    if (c.IsPlayer()) { SendStats(c); Tell(c, "You have died."); }
    if (amx_) amx_->OnDeath(c);
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
        case GMsg::C2S_PickUp: {
            gnet::PickUp m; ok = m.Read(b);
            if (ok && budget() && !c->dead) BeginInteract(*c, PendingType::Pickup, m.id, 0);
            break;
        }
        case GMsg::C2S_Drop: {
            gnet::Drop m; ok = m.Read(b);
            if (ok && budget() && !c->dead) DropItem(*c, m.id);
            break;
        }
        case GMsg::C2S_Equip: {
            gnet::Equip m; ok = m.Read(b);
            if (ok && budget() && !c->dead) EquipItem(*c, m.id, m.on != 0);
            break;
        }
        case GMsg::C2S_Use: {
            gnet::Use m; ok = m.Read(b);
            if (ok && budget() && !c->dead) BeginInteract(*c, PendingType::Use, m.id, 0);
            break;
        }
        case GMsg::C2S_Attack: {
            gnet::Attack m; ok = m.Read(b);
            if (ok && budget()) StartAttack(*c, m.id);
            break;
        }
        case GMsg::C2S_Take: {
            gnet::Take m; ok = m.Read(b);
            if (ok && budget() && !c->dead) BeginInteract(*c, PendingType::Take, m.container, m.entry);
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
            if (ok && budget()) { SendStats(*c); SendInventory(*c); }
            break;
        default: ok = false; break;
    }
}

}  // namespace f3
