// Doors and containers: the server owns their state. States: Open / Closed / Locked, plus LockFlag bits (no-open, no-pick, jammed,
// key-only), a lock DC and a key item. A closed door blocks its doorway cells (movement, pathing, line of sight); critters that may
// operate it (unlocked, or carrying the key) open it on the way. Containers share the state model: loot only while open.
// Lockpicking (Security skill through C2S_UseSkill) and keys (inventory items) live here; Lua / AMX scripts reach it through
// Scripting.cpp / AmxHost.cpp. State is persisted per map in the store (table world_objects). See docs/research/doors.md.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <random>
#include <enet/enet.h>
#include <nlohmann/json.hpp>
#include "Scripting.h"
#include "Server.h"
#include "common/log.h"

namespace f3 {

using json = nlohmann::json;

namespace {
std::string Lower(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}
std::string StripExt(const std::string& s) {
    const size_t d = s.rfind('.');
    return d != std::string::npos && s.size() - d <= 5 ? s.substr(0, d) : s;
}
bool KeyMatches(const Proto& p, const std::string& keyId) {
    const std::string k = Lower(keyId), pk = Lower(p.key);
    return pk == k || StripExt(pk) == StripExt(k);
}
std::mt19937& Rng() {
    static std::mt19937 r(std::random_device{}());
    return r;
}
const char* Noun(const Item& o) { return o.kind == ObjKind::Door ? "door" : "container"; }
}  // namespace

const char* Server::OperateResultName(OperateResult r) {
    switch (r) {
        case OperateResult::Opened: return "opened";
        case OperateResult::OpenedWithKey: return "unlocked_key";
        case OperateResult::Closed: return "closed";
        case OperateResult::AlreadyOpen: return "already_open";
        case OperateResult::AlreadyClosed: return "already_closed";
        case OperateResult::Locked: return "locked";
        case OperateResult::NoOpen: return "no_open";
        case OperateResult::Jammed: return "jammed";
        case OperateResult::Blocked: return "blocked";
        case OperateResult::Vetoed: return "vetoed";
        case OperateResult::NotOperable: return "not_operable";
    }
    return "?";
}

// ------------------------------------------------------------------ state

void Server::NotifyObject(Map& m, const Item& o) {
    if (o.kind == ObjKind::Ground) return;
    ByteBuffer b;
    {
        gnet::ObjectInfo oi;
        oi.id = o.id; oi.kind = static_cast<gnet::ObjectKind>(o.kind); oi.open = o.open ? 1 : 0; oi.locked = o.locked ? 1 : 0; oi.flags = o.lockFlags;
        b = gnet::PackG(oi);
    }
    for (Critter* p : m.critters)
        if (p->IsPlayer() && p->visItems.count(o.id)) Send(*p, b);
}

// A closed door keeps its doorway cells blocked, an open one releases them.
void Server::ApplyDoorBlock(Map& m, Item& door) {
    const uint8_t want = door.open ? 0 : 1;
    gnet::CellsSet cs;
    for (TilePos t : door.footprint) {
        uint8_t& b = m.blocked[static_cast<size_t>(t.y) * m.width + t.x];
        if (b != want) { b = want; cs.cells.push_back({t.x, t.y, want}); }
    }
    m.dirty = true;
    if (cs.cells.empty()) return;
    ByteBuffer cb = gnet::PackG(cs);
    for (Critter* p : m.critters)
        if (p->IsPlayer()) Send(*p, cb);
}

void Server::SetOpen(Map& m, Item& o, bool open) {
    o.open = open;
    m.objDirty = true;
    if (o.kind == ObjKind::Door) ApplyDoorBlock(m, o);
    NotifyObject(m, o);
}

void Server::LockChanged(Item& o, const char* reason) {
    if (Map* m = world_.GetMap(o.mapId)) { m->objDirty = true; NotifyObject(*m, o); }
    if (scripting_) scripting_->Call("lock_changed", static_cast<int>(o.id), o.locked, static_cast<int>(o.lockFlags), std::string(reason));
}

// Script-forced open / close (Lua set_door_open, AMX): no lock, key or flag checks. Closing is refused while a critter stands in the doorway.
void Server::SetDoor(Map& m, Item& door, bool open) {
    if (open == door.open) return;
    if (!open)
        for (TilePos t : door.footprint)
            if (m.CritterAt(t)) return;
    SetOpen(m, door, open);
    if (amx_) amx_->CallObject(door.id, open ? "OnOpen" : "OnClose", 0);
}

void Server::SetDoorLocked(Item& o, bool locked, const char* reason) {
    if ((o.kind != ObjKind::Door && o.kind != ObjKind::Container) || o.locked == locked) return;
    o.locked = locked;
    LockChanged(o, reason);
}

void Server::SetLockFlags(Item& o, uint8_t flags, const char* reason) {
    if ((o.kind != ObjKind::Door && o.kind != ObjKind::Container) || o.lockFlags == flags) return;
    o.lockFlags = flags;
    LockChanged(o, reason);
}

void Server::SetLockDC(Item& o, int dc) {
    o.lockDC = std::max(0, dc);
    if (Map* m = world_.GetMap(o.mapId)) m->objDirty = true;
}

void Server::SetLockKey(Item& o, const std::string& keyId) {
    o.keyId = keyId;
    if (Map* m = world_.GetMap(o.mapId)) m->objDirty = true;
}

// ------------------------------------------------------------------ keys and operating

bool Server::HasKey(const Critter& c, const Item& o, std::string* keyName) const {
    if (o.keyId.empty()) return false;
    for (const InvItem& it : c.inventory) {
        const Proto* p = data_.GetProto(it.pid);
        if (p && KeyMatches(*p, o.keyId)) {
            if (keyName) {
                std::string n = p->ReadableName();
                const std::string l = Lower(n);
                if (l.size() > 4 && l.compare(l.size() - 4, 4, " key") == 0) n.resize(n.size() - 4);   // the message appends " key"
                *keyName = n;
            }
            return true;
        }
    }
    return false;
}

bool Server::CanOperate(const Critter& c, const Item& o) const {
    if (o.lockFlags & (kLockNoOpen | kLockJammed)) return false;
    return !o.locked || HasKey(c, o);
}

Server::OperateResult Server::Operate(Critter* actor, Map& m, Item& o, int want) {
    if (o.kind != ObjKind::Door && o.kind != ObjKind::Container) return OperateResult::NotOperable;
    const CritterId actorId = actor ? actor->id : 0;
    if (o.open) {
        if (want == 1) return OperateResult::AlreadyOpen;
        if (o.kind == ObjKind::Door)
            for (TilePos t : o.footprint)
                if (m.CritterAt(t)) return OperateResult::Blocked;   // something stands in the doorway
        if (amx_ && !amx_->CallObject(o.id, "OnClose", actorId)) return OperateResult::Vetoed;
        SetOpen(m, o, false);
        return OperateResult::Closed;
    }
    if (want == 0) return OperateResult::AlreadyClosed;
    if (o.lockFlags & kLockNoOpen) return OperateResult::NoOpen;
    if (o.lockFlags & kLockJammed) return OperateResult::Jammed;
    // The original runs the OnOpen script before the lock check; the stock door template answers 0 while IsLocked, so a key holder would
    // never get in. Here the engine decides the lock (key / no key) first and the script only sees an unlocked door.
    bool key = false;
    if (o.locked) {
        if (!actor || !HasKey(*actor, o)) return OperateResult::Locked;
        key = true;
        o.locked = false;
    }
    if (amx_ && !amx_->CallObject(o.id, "OnOpen", actorId)) { if (key) o.locked = true; return OperateResult::Vetoed; }
    if (key) LockChanged(o, "key");
    SetOpen(m, o, true);
    return key ? OperateResult::OpenedWithKey : OperateResult::Opened;
}

void Server::ReportOperate(Critter* actor, const Item& o, OperateResult r) {
    if (actor && actor->IsPlayer()) {
        std::string name = o.name;
        if (const Proto* p = data_.GetProto(o.pid)) name = p->ReadableName();
        auto stf = [&](int id, const std::string& fallback) { std::string t = data_.String(id); return t.empty() ? fallback : t; };
        switch (r) {
            case OperateResult::Locked:
                Tell(*actor, o.kind == ObjKind::Door ? stf(1299, "The door is locked.") : "The " + name + " is locked.");
                break;
            case OperateResult::NoOpen: Tell(*actor, "The " + name + " cannot be opened."); break;
            case OperateResult::Jammed: Tell(*actor, "The " + name + " is jammed."); break;
            case OperateResult::Blocked: Tell(*actor, "Something is in the way."); break;
            case OperateResult::OpenedWithKey: {
                std::string kn = "right";
                HasKey(*actor, o, &kn);
                std::string fmt = stf(1358, "You opened the door with the %s key.");
                char buf[256];
                std::snprintf(buf, sizeof buf, fmt.c_str(), kn.c_str());
                Tell(*actor, o.kind == ObjKind::Door ? std::string(buf) : "You unlock the " + name + " with the " + kn + " key.");
                break;
            }
            default: break;
        }
    }
    if (scripting_ && r != OperateResult::NotOperable)
        scripting_->Call("door_use", actor, static_cast<int>(o.id), std::string(OperateResultName(r)), r == OperateResult::OpenedWithKey);
}

bool Server::UseObject(Critter& c, ItemId objId) {
    Map* m = world_.GetMap(c.mapId);
    if (!m || c.dead) return false;
    auto it = m->items.find(objId);
    if (it == m->items.end()) return false;
    Item& o = it->second;
    if (ObjDist(o, c.pos) > rt_.DoorOpenRange) return false;
    switch (o.kind) {
        case ObjKind::Door: {
            OperateResult r = Operate(&c, *m, o, -1);
            ReportOperate(&c, o, r);
            break;
        }
        case ObjKind::Container: {
            if (!o.open) ReportOperate(&c, o, Operate(&c, *m, o, 1));
            if (o.open) SendContainer(c, o.id, false);   // contents are only listed while the container is open
            break;
        }
        case ObjKind::Usable: break;
        case ObjKind::Ground: return false;
    }
    const Proto* p = data_.GetProto(o.pid);
    if (scripting_) scripting_->Call("on_use", &c, static_cast<int>(objId), p ? p->key : std::string(), o.script);
    return true;
}

int Server::ObjDist(const Item& o, TilePos from) const {
    int d = TileDist(o.pos, from);
    for (TilePos t : o.footprint) d = std::min(d, TileDist(t, from));
    return d;
}

// ------------------------------------------------------------------ walking through doors

bool Server::DoorPassable(const Critter& c, Map& m, int cell) {
    auto dc = m.doorCell.find(cell);
    if (dc == m.doorCell.end()) return false;
    auto it = m.items.find(dc->second);
    return it != m.items.end() && it->second.kind == ObjKind::Door && !it->second.open && CanOperate(c, it->second);
}

void Server::OpenDoorAt(Critter& c, Map& m, int x, int y, bool report) {
    if (!m.InBounds(x, y)) return;
    auto dc = m.doorCell.find(y * m.width + x);
    if (dc == m.doorCell.end()) return;
    auto it = m.items.find(dc->second);
    if (it == m.items.end() || it->second.open) return;
    Item& d = it->second;
    // original GameActionMoveTo::OnMoved: the operate-door action is only queued for critters outside the player's party; a party member
    // just stops in front of the door (it opens it with a click)
    if (c.IsPlayer() && !rt_.PlayerAutoOpenDoors) {
        if (report && !CanOperate(c, d)) ReportOperate(&c, d, d.lockFlags & kLockNoOpen ? OperateResult::NoOpen : d.lockFlags & kLockJammed ? OperateResult::Jammed : OperateResult::Locked);
        return;
    }
    if (CanOperate(c, d)) { ReportOperate(&c, d, Operate(&c, m, d, 1)); return; }
    if (report) ReportOperate(&c, d, d.lockFlags & kLockNoOpen ? OperateResult::NoOpen : d.lockFlags & kLockJammed ? OperateResult::Jammed : OperateResult::Locked);
}

// Opens the closed doors that lie on the next ~3 cells of the critter's path so they are open (and animated) when it gets there.
void Server::OpenDoorsAhead(Critter& c, Map& m) {
    if (!c.mv.active || m.doorCell.empty()) return;
    if (c.IsPlayer() && !rt_.PlayerAutoOpenDoors) return;
    float px = c.fx, py = c.fy, left = 3.0f;
    for (size_t seg = c.mv.seg; seg + 1 < c.mv.pts.size() && left > 0; ++seg) {
        const float bx = c.mv.pts[seg + 1].x, by = c.mv.pts[seg + 1].y;
        const float dx = bx - px, dy = by - py, len = std::sqrt(dx * dx + dy * dy);
        const int n = std::max(1, static_cast<int>(std::ceil(std::min(len, left) / 0.5f)));
        for (int i = 1; i <= n; ++i) {
            const float t = std::min(len, left) * static_cast<float>(i) / n / std::max(len, 1e-4f);
            OpenDoorAt(c, m, static_cast<int>(std::floor(px + dx * t)), static_cast<int>(std::floor(py + dy * t)), false);
        }
        left -= len;
        px = bx; py = by;
    }
}

// Containers do not stay open forever: closed again when nobody is near (the client also sends C2S_CloseObj when the loot window closes).
void Server::TickObjects(Map& m) {
    for (auto& [id, it] : m.items) {
        if (it.kind != ObjKind::Container || !it.open) continue;
        bool anyNear = false;
        for (Critter* c : m.critters)
            if (c->IsPlayer() && !c->dead && TileDist(c->pos, it.pos) <= rt_.DoorOpenRange + 3) { anyNear = true; break; }
        if (!anyNear) SetOpen(m, it, false);
    }
}

// ------------------------------------------------------------------ lockpicking

int Server::LockChance(const Critter& c, const Item& o, int* dcOut) {
    int skill = 0;
    for (int i = 0; i < kSkillCount; ++i)
        if (std::string(kSkillNames[i]) == "Security") skill = SkillValue(c, i);
    const int dc = o.lockDC > 0 ? o.lockDC : rt_.LockDefaultDC;
    if (dcOut) *dcOut = dc;
    return std::clamp(skill - dc, rt_.LockpickMinChance, rt_.LockpickMaxChance);
}

bool Server::Lockpick(Critter& c, Item& o) {
    if (o.kind != ObjKind::Door && o.kind != ObjKind::Container) return false;
    Map* m = world_.GetMap(o.mapId);
    if (!m) return false;
    std::string name = o.name;
    if (const Proto* p = data_.GetProto(o.pid)) name = p->ReadableName();
    auto say = [&](const std::string& t) { Tell(c, t); };
    auto note = [&](const char* result) { if (scripting_) scripting_->Call("door_use", &c, static_cast<int>(o.id), std::string(result), false); };
    if (o.open) { say("The " + name + " is open."); note("pick_not_needed"); return true; }
    if (!o.locked) { say("The " + name + " is not locked."); note("pick_not_needed"); return true; }
    if (o.lockFlags & kLockNoOpen) { say("The " + name + " cannot be opened."); note("no_open"); return true; }
    if (o.lockFlags & (kLockNoPick | kLockKeyOnly)) { say("This lock cannot be picked."); note("pick_impossible"); return true; }
    if (rt_.LockpickNeedsKit) {
        bool kit = false;
        for (const InvItem& it : c.inventory)
            if (const Proto* p = data_.GetProto(it.pid); p && Lower(p->key).find("security_kit") != std::string::npos) kit = true;
        if (!kit) { say("You need a Security Kit to pick locks."); note("pick_no_kit"); return true; }
    }
    int dc = 0;
    int chance = LockChance(c, o, &dc);
    if (scripting_) {
        if (auto v = scripting_->CallNumber("lockpick_chance", &c, static_cast<int>(o.id), chance)) chance = std::clamp(static_cast<int>(*v), 0, 100);
    }
    const int roll = rt_.ForceLockRoll >= 0 ? (rt_.ForceLockRoll ? 1 : 101) : std::uniform_int_distribution<int>(1, 100)(Rng());
    if (roll <= chance) {
        o.locked = false;
        LockChanged(o, "pick");
        std::string msg = o.kind == ObjKind::Door ? data_.String(672) : std::string();
        say(msg.empty() ? "You successfully unlock the " + name + "." : msg);
        note("picked");
        if (rt_.LockpickXp > 0) AddXp(c, rt_.LockpickXp);
    } else {
        say("You fail to pick the lock.");
        note("pick_failed");
    }
    return true;
}

// ------------------------------------------------------------------ persistence (table world_objects, one set of rows per map)

namespace {
std::string ObjKey(const DataStore& d, const Item& o) {
    const Proto* p = d.GetProto(o.pid);
    return (p ? p->key : o.name) + "@" + std::to_string(o.pos.x) + "," + std::to_string(o.pos.y);
}
}  // namespace

void Server::SaveWorldState(bool force) {
    if (!store_.IsOpen()) return;
    int saved = 0;
    Store::Tx tx(store_);   // all dirty maps in one commit
    std::vector<Map*> done;
    for (auto& mp : world_.Maps()) {
        Map& m = *mp;
        if (!m.objDirty && !force) continue;
        std::vector<WorldObjRow> rows;
        for (auto& [id, o] : m.items) {
            if (o.kind != ObjKind::Door && o.kind != ObjKind::Container) continue;
            WorldObjRow r;
            r.key = ObjKey(data_, o);
            r.open = o.open;
            r.locked = o.locked;
            r.flags = o.lockFlags;
            r.dc = o.lockDC;
            r.lockKey = o.keyId;
            if (o.kind == ObjKind::Container) {
                json contents = json::array();
                for (const InvItem& it : o.contents)
                    if (const Proto* p = data_.GetProto(it.pid)) contents.push_back({{"proto", p->key}, {"count", it.count}});
                r.contents = contents.dump();
            }
            rows.push_back(std::move(r));
        }
        if (rows.empty()) { m.objDirty = false; continue; }
        if (store_.SaveWorldObjects(m.name, rows)) { done.push_back(&m); ++saved; }
        else log::Error("world state of map %s: save FAILED", m.name.c_str());
    }
    if (!tx.Commit()) { log::Error("world state: commit FAILED, will retry"); return; }   // objDirty stays set
    for (Map* m : done) m->objDirty = false;
    if (saved) log::Info("saved door / container state of %d map(s)", saved);
}

void Server::LoadWorldState() {
    if (!store_.IsOpen()) return;
    int maps = 0, objs = 0;
    for (auto& mp : world_.Maps()) {
        Map& m = *mp;
        const std::vector<WorldObjRow> rows = store_.LoadWorldObjects(m.name);
        if (rows.empty()) continue;
        std::unordered_map<std::string, Item*> byKey;
        for (auto& [id, o] : m.items)
            if (o.kind == ObjKind::Door || o.kind == ObjKind::Container) byKey[ObjKey(data_, o)] = &o;
        for (const WorldObjRow& r : rows) {
            auto it = byKey.find(r.key);
            if (it == byKey.end()) continue;
            Item& o = *it->second;
            o.open = r.open;
            o.locked = r.locked;
            o.lockFlags = static_cast<uint8_t>(r.flags);
            o.lockDC = r.dc;
            o.keyId = r.lockKey;
            if (o.kind == ObjKind::Container) {
                o.open = false;   // a container is closed again after a restart
                const json contents = r.contents.empty() ? json() : json::parse(r.contents, nullptr, false);
                if (contents.is_array()) {
                    o.contents.clear();
                    for (const json& c : contents)
                        if (const Proto* p = data_.FindProto(c.value("proto", std::string())))
                            o.contents.push_back(InvItem{world_.NextItemId(), p->pid, static_cast<uint16_t>(c.value("count", 1)), false, 0});
                }
            } else {
                for (TilePos t : o.footprint) m.blocked[static_cast<size_t>(t.y) * m.width + t.x] = o.open ? 0 : 1;
            }
            ++objs;
        }
        ++maps;
    }
    if (objs) log::Info("world state: %d door / container(s) restored on %d map(s)", objs, maps);
}

}  // namespace f3
