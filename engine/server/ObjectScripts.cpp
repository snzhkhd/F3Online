// Object scripts (data/objects, scripts/objects/*.lua; docs/object-scripts.md): behaviour bound to map objects such as computers,
// terminals, levers, doors and containers. A binding (ObjectDef, Content.cpp) names a map object by map + instance name (fallback: cell
// + prototype); behaviour is a declarative step list (the dialog condition / effect vocabulary plus dialog, door, skill check, message,
// teleport, ...) and / or a Lua file with on_use / on_skill / on_look hooks. Everything runs on the server thread, authoritative:
// range, usable, one-shot and cooldown are checked here, never by the client. One-shot marks and per-object flags are persisted in the
// store (table object_state), the bindings are re-resolved on map load and on /reload.
#include <algorithm>
#include <random>
#include <nlohmann/json.hpp>
#include "Scripting.h"
#include "Server.h"
#include "common/log.h"

namespace f3 {

using json = nlohmann::json;

namespace {
std::string Lc(std::string s) { for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }
std::string NoExt(const std::string& s) {
    const size_t d = s.rfind('.');
    return d != std::string::npos && s.size() - d <= 5 ? s.substr(0, d) : s;
}
std::mt19937& ObjRng() {
    static std::mt19937 r(ServerSeed() ^ 0x51ed270bu);
    return r;
}
const char* const kKindNames[4] = {"item", "door", "container", "usable"};
}  // namespace

// ------------------------------------------------------------------ state

std::string Server::ObjectKey(const Item& o) const {
    if (!o.instName.empty()) return o.instName;
    const Proto* p = data_.GetProto(o.pid);
    return (p ? p->key : o.name) + "@" + std::to_string(o.pos.x) + "," + std::to_string(o.pos.y);
}

Server::ObjState& Server::ObjStateOf(const Map& m, const Item& o) {
    if (!objStateLoaded_.count(m.name)) {   // the rows of a map are read the first time one of its objects is asked
        objStateLoaded_.insert(m.name);
        if (store_.IsOpen())
            for (const ObjStateRow& r : store_.LoadObjectState(m.name)) {
                const json j = json::parse(r.data, nullptr, false);
                if (!j.is_object()) continue;
                ObjState st;
                st.used = j.value("used", 0) != 0;
                if (j.contains("flags") && j["flags"].is_object())
                    for (auto it = j["flags"].begin(); it != j["flags"].end(); ++it)
                        if (it.value().is_number()) st.flags[it.key()] = it.value().get<double>();
                objState_[m.name + "\n" + r.key] = std::move(st);
            }
    }
    objStateDirty_.insert(m.name);   // the caller may change it
    return objState_[m.name + "\n" + ObjectKey(o)];
}

void Server::SaveObjectStates() {
    if (!store_.IsOpen() || objStateDirty_.empty()) return;
    for (auto it = objStateDirty_.begin(); it != objStateDirty_.end();) {
        const std::string prefix = *it + "\n";
        std::vector<ObjStateRow> rows;
        for (auto e = objState_.lower_bound(prefix); e != objState_.end() && e->first.compare(0, prefix.size(), prefix) == 0; ++e) {
            if (!e->second.used && e->second.flags.empty()) continue;
            json j = {{"used", e->second.used ? 1 : 0}, {"flags", json::object()}};
            for (auto& [k, v] : e->second.flags) j["flags"][k] = v;
            rows.push_back(ObjStateRow{e->first.substr(prefix.size()), j.dump()});
        }
        if (store_.SaveObjectState(*it, rows)) it = objStateDirty_.erase(it);
        else { log::Error("object state of map %s: save FAILED, will retry", it->c_str()); ++it; }
    }
}

// ------------------------------------------------------------------ binding

const ObjectDef* Server::BoundDef(const Item& o) const { return o.bindId.empty() ? nullptr : content_.FindObject(o.bindId); }

static bool ProtoMatches(const DataStore& d, const Item& it, const std::string& want) {
    if (want.empty()) return true;
    const Proto* p = d.GetProto(it.pid);
    return p && NoExt(Lc(p->key)) == NoExt(Lc(want));
}

void Server::BindObjectItem(Map& m, Item& it) {
    it.bindId.clear();
    if (it.kind == ObjKind::Ground) return;
    for (const ObjectDef& d : content_.objects) {
        if (Lc(d.map) != Lc(m.name)) continue;
        if (!d.object.empty() && !it.instName.empty() && Lc(d.object) == Lc(it.instName)) { it.bindId = d.id; return; }
        if (d.cx < 0 || it.pos.x != d.cx || it.pos.y != d.cy || !ProtoMatches(data_, it, d.proto)) continue;
        bool named = false;   // the cell fallback only applies when no object carries the name
        if (!d.object.empty()) for (auto& [id, o] : m.items) if (o.instName == d.object) named = true;
        if (!named) { it.bindId = d.id; return; }
    }
}

void Server::BindObjects() {
    objUnresolved_.clear();
    for (auto& mp : world_.Maps()) for (auto& [id, it] : mp->items) it.bindId.clear();
    int bound = 0;
    std::vector<std::string> rels;
    std::vector<std::pair<Map*, ItemId>> loaded;
    for (const ObjectDef& d : content_.objects) {
        Map* m = world_.FindMapByName(d.map);
        if (!m) { objUnresolved_.push_back(d.id + ": map '" + d.map + "' is not loaded"); continue; }
        Item* found = nullptr;
        if (!d.object.empty())
            for (auto& [id, it] : m->items) if (it.kind != ObjKind::Ground && !it.instName.empty() && Lc(it.instName) == Lc(d.object)) { found = &it; break; }
        if (!found && d.cx >= 0)
            for (auto& [id, it] : m->items)
                if (it.kind != ObjKind::Ground && it.pos.x == d.cx && it.pos.y == d.cy && ProtoMatches(data_, it, d.proto) && (!found || it.id < found->id)) found = &it;
        if (!found) { objUnresolved_.push_back(d.id + ": no object '" + (d.object.empty() ? "cell " + std::to_string(d.cx) + "," + std::to_string(d.cy) : d.object) + "' on map " + d.map); continue; }
        if (!found->bindId.empty()) { objUnresolved_.push_back(d.id + ": the object is already bound by '" + found->bindId + "'"); continue; }
        found->bindId = d.id;
        ++bound;
        if (!d.script.empty()) { rels.push_back(d.script); loaded.emplace_back(m, found->id); }
    }
    for (const std::string& u : objUnresolved_) log::Warn("objects: %s", u.c_str());
    if (scripting_) {
        for (const ObjectDef& d : content_.objects) if (!d.script.empty() && std::find(rels.begin(), rels.end(), d.script) == rels.end()) rels.push_back(d.script);   // unbound ones still get checked / loaded
        scripting_->LoadObjectScripts(rels);
        for (auto& [m, id] : loaded) {
            auto it = m->items.find(id);
            const ObjectDef* d = it != m->items.end() ? BoundDef(it->second) : nullptr;
            if (d && scripting_->HasObjectHook(d->script, "on_loaded")) scripting_->CallObjectHook(d->script, "on_loaded", nullptr, LuaObj{this, id});
        }
    }
    if (!content_.objects.empty()) log::Info("objects: %d of %zu binding(s) bound to map objects, %zu unresolved", bound, content_.objects.size(), objUnresolved_.size());
}

// ------------------------------------------------------------------ presentation

std::string Server::ObjectName(const Critter& c, const Item& o) const {
    const ObjectDef* d = BoundDef(o);
    return d && !d->name.empty() ? LOr(c, "object." + d->id + ".name", d->name) : o.name;
}
std::string Server::ObjectDesc(const Critter& c, const Item& o) const {
    const ObjectDef* d = BoundDef(o);
    return d && !d->desc.empty() ? LOr(c, "object." + d->id + ".desc", d->desc) : std::string();
}
std::string Server::ObjectLabel(const Critter& c, const Item& o) const {
    const ObjectDef* d = BoundDef(o);
    return d && !d->label.empty() ? LOr(c, "object." + d->id + ".label", d->label) : std::string();
}
int Server::ObjectRange(const Item& o, int def) const {
    const ObjectDef* d = BoundDef(o);
    return d && d->range > 0 ? d->range : def;
}
bool Server::ObjectHiddenFor(Critter& c, const Item& o) {
    const ObjectDef* d = BoundDef(o);
    return d && d->hasHidden && c.IsPlayer() && !EvalCond(c, nullptr, d->hiddenUntil);
}

// ------------------------------------------------------------------ running

bool Server::SkillCheck(Critter& c, const std::string& skill, int difficulty, bool roll) {
    int idx = -1;
    for (int i = 0; i < kSkillCount; ++i) if (Lc(kSkillNames[i]) == Lc(skill)) idx = i;
    if (idx < 0) { log::Warn("skill_check: unknown skill '%s'", skill.c_str()); return false; }
    const int value = SkillValue(c, idx);
    if (!roll) return value >= difficulty;
    const int chance = std::clamp(value - difficulty, rt_.LockpickMinChance, rt_.LockpickMaxChance);
    const int r = rt_.ForceLockRoll >= 0 ? (rt_.ForceLockRoll ? 1 : 101) : std::uniform_int_distribution<int>(1, 100)(ObjRng());
    return r <= chance;
}

void Server::DoorAction(Map& m, Item& o, const std::string& action) {
    if (o.kind != ObjKind::Door && o.kind != ObjKind::Container) return;
    if (action == "unlock") { SetDoorLocked(o, false, "object"); return; }
    if (action == "lock") { if (o.open) Operate(nullptr, m, o, 0); SetDoorLocked(o, true, "object"); return; }
    const bool open = action == "open" || (action == "toggle" && !o.open);
    if (open) { if (o.locked) SetDoorLocked(o, false, "object"); Operate(nullptr, m, o, 1); }
    else Operate(nullptr, m, o, 0);
}

bool Server::ObjectGate(Critter& c, Map& m, Item& o, const ObjectDef& d) {
    if (ObjectHiddenFor(c, o)) return false;
    if (!d.usable) { TellK(c, "msg.obj.unusable"); return false; }
    if (ObjDist(o, c.pos) > ObjectRange(o, rt_.DoorOpenRange)) { TellK(c, "msg.too_far"); return false; }
    const bool used = d.oneShot == 1 ? ObjStateOf(m, o).used : d.oneShot == 2 ? GetGlobalFor(&c, "obj." + d.id + ".used") > 0 : false;
    if (used) { TellK(c, "msg.obj.used"); return false; }
    auto cd = objCooldown_.find(o.id);
    if (cd != objCooldown_.end() && NowMs() < cd->second) { TellK(c, "msg.obj.cooldown", {{"s", static_cast<int>((cd->second - NowMs() + 999) / 1000)}}); return false; }
    return true;
}

void Server::ObjectMarkUsed(Critter& c, Item& o, const ObjectDef& d) {
    if (d.oneShot == 1) { if (Map* m = world_.GetMap(o.mapId)) ObjStateOf(*m, o).used = true; }
    else if (d.oneShot == 2) SetGlobalFor(&c, "obj." + d.id + ".used", 1);
    if (d.cooldown > 0) objCooldown_[o.id] = NowMs() + static_cast<uint64_t>(d.cooldown) * 1000;
}

bool Server::RunObjSteps(Critter& pc, Map& m, Item& o, const ObjectDef& d, const std::vector<ObjStep>& steps) {
    const ItemId oid = o.id;
    for (const ObjStep& st : steps) {
        if (st.hasCond && !EvalCond(pc, nullptr, st.cond)) continue;
        switch (st.kind) {
            case ObjStep::Dialog:
                StartObjectDialog(pc, st.s, o, ObjectName(pc, o));
                return true;   // the conversation goes on by itself: the rest of the list is not run
            case ObjStep::Door: {
                Item* t = nullptr;
                for (auto& [id, it] : m.items) {
                    if (it.kind != ObjKind::Door && it.kind != ObjKind::Container) continue;
                    if (!st.s.empty() ? (!it.instName.empty() && Lc(it.instName) == Lc(st.s)) : (it.pos.x == st.x && it.pos.y == st.y)) { t = &it; break; }
                }
                if (!t && st.s.empty()) {   // a doorway cell of a door
                    auto dc = m.doorCell.find(st.y * m.width + st.x);
                    if (dc != m.doorCell.end()) t = FindObject(dc->second);
                }
                if (!t) log::Warn("object '%s': no door / container '%s' on %s", d.id.c_str(), st.s.empty() ? (std::to_string(st.x) + "," + std::to_string(st.y)).c_str() : st.s.c_str(), m.name.c_str());
                else DoorAction(m, *t, st.action);
                break;
            }
            case ObjStep::SkillCheck: {
                const bool ok = SkillCheck(pc, st.skill, st.difficulty, st.roll);
                if (ok && st.xp > 0) AddXp(pc, st.xp);
                if (RunObjSteps(pc, m, o, d, ok ? st.success : st.failure)) return true;
                break;
            }
            case ObjStep::Effect: {
                std::vector<DlgEffect> one{st.fx};
                RunDlgEffects(pc, nullptr, one);
                break;
            }
            case ObjStep::Message: {
                std::string t = st.key.empty() ? st.s : LOr(pc, st.key, st.s);
                for (size_t at = t.find("{player}"); at != std::string::npos; at = t.find("{player}", at + pc.name.size())) t.replace(at, 8, pc.name);
                Tell(pc, t);
                break;
            }
            case ObjStep::Teleport: {
                Map* dst = world_.FindMapByName(st.s);
                if (!dst) { log::Warn("object '%s': teleport to unknown map '%s'", d.id.c_str(), st.s.c_str()); break; }
                if (st.x >= 0) Teleport(pc, dst->id, TilePos{static_cast<int16_t>(st.x), static_cast<int16_t>(st.y)});
                else EnterMap(pc, *dst, st.entry);
                return true;
            }
            case ObjStep::Play:
                if (scripting_) scripting_->Call("object_effect", &pc, LuaObj{this, oid}, st.s);   // hook only: the protocol has no effect message yet
                break;
            case ObjStep::Call:
                if (scripting_) scripting_->Call(st.s.c_str(), &pc, LuaObj{this, oid});
                break;
            case ObjStep::DefaultUse: DefaultUseObject(pc, m, o); break;
            case ObjStep::End: return true;
        }
    }
    return false;
}

bool Server::ObjectUse(Critter& c, Map& m, Item& o) {
    const ObjectDef* dp = BoundDef(o);
    if (!dp) return false;
    const ObjectDef d = *dp;   // copy: Lua may reload the data
    const bool script = !d.script.empty() && scripting_ && scripting_->EnsureObjectScript(d.script);
    const bool luaUse = script && scripting_->HasObjectHook(d.script, "on_use");
    if (d.onUse.empty() && !luaUse) return false;   // only name / desc / hidden / skills: the engine's own use applies
    if (ObjectHiddenFor(c, o)) return true;
    if (!ObjectGate(c, m, o, d)) return true;
    const LuaObj lo{this, o.id};
    if (script && scripting_->HasObjectHook(d.script, "can_use")) {
        sol::object r;
        if (scripting_->CallObjectHook(d.script, "can_use", &r, &c, lo) && r.is<bool>() && !r.as<bool>()) return true;
    }
    bool handled = false;
    if (luaUse) handled = scripting_->CallObjectHandled(d.script, "on_use", &c, lo);
    if (!handled && !d.onUse.empty()) RunObjSteps(c, m, o, d, d.onUse);
    ObjectMarkUsed(c, o, d);
    return true;
}

bool Server::ObjectSkill(Critter& c, Map& m, Item& o, int skill) {
    const ObjectDef* dp = BoundDef(o);
    if (!dp || skill < 0 || skill >= kSkillCount) return false;
    const ObjectDef d = *dp;
    const std::string sk = kSkillNames[skill];
    const std::vector<ObjStep>* steps = nullptr;
    for (auto& [name, v] : d.skills) if (Lc(name) == Lc(sk)) steps = &v;
    const bool script = !d.script.empty() && scripting_ && scripting_->EnsureObjectScript(d.script);
    const bool luaSkill = script && scripting_->HasObjectHook(d.script, "on_skill");
    if (!steps && !luaSkill) return false;
    if (ObjectHiddenFor(c, o)) return true;
    if (!ObjectGate(c, m, o, d)) return true;
    const LuaObj lo{this, o.id};
    bool handled = false;
    if (luaSkill) handled = scripting_->CallObjectHandled(d.script, "on_skill", &c, lo, sk);
    if (!handled && !steps) return false;   // the hook declined and there are no steps: the engine handles the skill (lockpick, use_skill hook)
    if (!handled) RunObjSteps(c, m, o, d, *steps);
    ObjectMarkUsed(c, o, d);
    return true;
}

void Server::ObjectLook(Critter& c, Map& m, Item& o) {
    const ObjectDef* dp = BoundDef(o);
    if (!dp || ObjectHiddenFor(c, o)) return;
    const ObjectDef d = *dp;
    const bool script = !d.script.empty() && scripting_ && scripting_->EnsureObjectScript(d.script);
    bool handled = false;
    if (script && scripting_->HasObjectHook(d.script, "on_look")) handled = scripting_->CallObjectHandled(d.script, "on_look", &c, LuaObj{this, o.id});
    if (!handled && !d.onLook.empty()) RunObjSteps(c, m, o, d, d.onLook);
}

// ------------------------------------------------------------------ editor listing

std::string Server::ObjectsJson(const std::string& map, const std::string& query, int limit) {
    json out = json::object();
    if (map.empty()) {
        json maps = json::array();
        for (auto& mp : world_.Maps()) {
            int n = 0;
            for (auto& [id, it] : mp->items) if (it.kind != ObjKind::Ground) ++n;
            maps.push_back({{"name", mp->name}, {"width", mp->width}, {"height", mp->height}, {"objects", n}});
        }
        out["maps"] = maps;
        return out.dump();
    }
    Map* m = world_.FindMapByName(map);
    if (!m) { out["error"] = "map '" + map + "' is not loaded"; return out.dump(); }
    const std::string q = Lc(query);
    std::vector<const Item*> v;
    for (auto& [id, it] : m->items) {
        if (it.kind == ObjKind::Ground) continue;
        if (!q.empty()) {
            const Proto* p = data_.GetProto(it.pid);
            const std::string hay = Lc(it.instName + " " + it.name + " " + (p ? p->key + " " + p->model : std::string()));
            if (hay.find(q) == std::string::npos) continue;
        }
        v.push_back(&it);
    }
    std::sort(v.begin(), v.end(), [](const Item* a, const Item* b) { return a->instName != b->instName ? a->instName < b->instName : a->id < b->id; });
    json arr = json::array();
    for (const Item* it : v) {
        if (limit > 0 && static_cast<int>(arr.size()) >= limit) break;
        const Proto* p = data_.GetProto(it->pid);
        arr.push_back({{"id", it->id}, {"name", it->instName}, {"display", it->name}, {"proto", p ? p->key : std::string()}, {"model", p ? p->model : std::string()},
                       {"kind", kKindNames[static_cast<int>(it->kind)]}, {"x", it->pos.x}, {"y", it->pos.y}, {"binding", it->bindId}});
    }
    out["map"] = m->name;
    out["width"] = m->width;
    out["height"] = m->height;
    out["total"] = v.size();
    out["objects"] = arr;
    json un = json::array();
    for (const std::string& u : objUnresolved_) un.push_back(u);
    out["unresolved"] = un;
    return out.dump();
}

}  // namespace f3
