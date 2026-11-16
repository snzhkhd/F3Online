// NPC life: respawn after death and the leash / walk-home behaviour (docs/server-data.md, "NPC respawn and home").
//
// Respawn: every NPC placed from the map data or from data/npcs "spawns" is tracked (SpawnRec: prototype, home cell, map instance data). When it is
// killed a job is queued: after `respawn` seconds (data/npcs record of its prototype; unset = config RespawnDefaultSec; 0 = never) a fresh critter
// is created on the home cell with the default HP / inventory / equipment / script state. While a living player is within RespawnPlayerRadius cells of
// the spot the job waits (retried every few seconds). A script that deletes a LIVING NPC on purpose (AMX DestroyEntity: story / cleanup) forgets
// its record: such an NPC does not come back. Corpses are removed when the respawn happens if they are still lying there.
//
// Home: every NPC remembers its spawn cell. A hunter that is farther than AggroLeashCells from home drops its target, walks back and is healed on
// arrival; it also walks back after any fight that moved it. A map that has no player left resets its hunters at once (they are unobserved).
#include <algorithm>
#include <sstream>
#include "AmxHost.h"
#include "Scripting.h"
#include "Server.h"
#include "common/log.h"

namespace f3 {

void Server::TrackSpawn(Critter& c, const EntityDef* inst, const std::string& nameFallback, int respawnOverride) {
    if (!c.IsNpc() || c.isStatic || c.protoPid < 0) return;
    SpawnRec r;
    r.pid = static_cast<uint16_t>(c.protoPid);
    r.map = c.home.has ? c.home.map : c.mapId;
    r.pos = c.home.has ? c.home.pos : c.pos;
    if (inst) { r.hasInst = true; r.inst = *inst; }
    r.nameFallback = nameFallback;
    r.respawnOverride = respawnOverride;
    spawnRecs_[c.id] = std::move(r);
}

void Server::ForgetSpawn(CritterId id) { spawnRecs_.erase(id); }

int Server::RespawnSecFor(const SpawnRec& r) const {
    if (r.respawnOverride >= 0) return r.respawnOverride;
    if (const Proto* p = data_.GetProto(r.pid))
        if (p->respawn >= 0) return p->respawn;
    return std::max(0, rt_.RespawnDefaultSec);
}

void Server::QueueRespawn(Critter& dead) {
    auto it = spawnRecs_.find(dead.id);
    if (it == spawnRecs_.end()) return;
    SpawnRec rec = std::move(it->second);
    spawnRecs_.erase(it);
    const int sec = RespawnSecFor(rec);
    if (sec <= 0) return;
    RespawnJob j;
    j.rec = std::move(rec);
    j.due = tick_ + static_cast<uint64_t>(sec) * static_cast<uint64_t>(cfg_.tickRate);
    j.corpse = dead.id;
    respawnQ_.push_back(std::move(j));
}

void Server::TickRespawns() {
    for (size_t i = 0; i < respawnQ_.size();) {
        RespawnJob& j = respawnQ_[i];
        if (tick_ < j.due) { ++i; continue; }
        Map* m = world_.GetMap(j.rec.map);
        const Proto* p = data_.GetProto(j.rec.pid);
        if (!m || !p) { respawnQ_.erase(respawnQ_.begin() + i); continue; }   // map / prototype gone (data reload): nothing to respawn
        bool watched = false;
        for (Critter* pl : m->critters)
            if (pl->IsPlayer() && !pl->dead && Server::InRange(pl->pos, j.rec.pos, rt_.RespawnPlayerRadius)) { watched = true; break; }
        if (watched) { j.due = tick_ + static_cast<uint64_t>(5) * cfg_.tickRate; ++i; continue; }
        if (j.corpse) RemoveCritter(j.corpse);   // the corpse may still be lying there (map without players does not clean up)
        const SpawnRec rec = j.rec;
        Critter* c = SpawnCritterProto(rec.map, *p, rec.pos, rec.hasInst ? &rec.inst : nullptr);
        if (!c) { j.corpse = 0; j.due = tick_ + static_cast<uint64_t>(10) * cfg_.tickRate; ++i; continue; }   // no free cell: retry
        if (c->instName.empty() && !rec.nameFallback.empty()) c->instName = rec.nameFallback;
        TrackSpawn(*c, rec.hasInst ? &rec.inst : nullptr, rec.nameFallback, rec.respawnOverride);
        log::Info("npc respawn: '%s' back at %d,%d (map %s)", c->name.c_str(), rec.pos.x, rec.pos.y, m->name.c_str());
        respawnQ_.erase(respawnQ_.begin() + i);
    }
}

// ------------------------------------------------------------------ leash / home

void Server::TickNpcHome(Critter& c) {
    if (!c.IsNpc() || c.dead || c.isStatic || !c.home.has || c.home.map != c.mapId) return;
    const int dh = TileDist(c.pos, c.home.pos);
    if (c.target != 0) {
        c.aggro = true;
        c.returning = false;
        if (rt_.AggroLeashCells > 0 && dh > rt_.AggroLeashCells) {   // too far from home: give up, walk back
            c.target = 0;
            StopMove(c, 1);
            if (amx_) amx_->ClearActionsOf(c.id);   // an A_Attack / A_MoveTo queue would send it straight back into the fight
            c.aggro = false;
            c.returning = true;
            c.returnSinceTick = tick_;
            c.nextHomeTick = 0;
        }
        return;
    }
    if (c.aggro) {   // the fight is over (target dead, gone or out of reach)
        c.aggro = false;
        if (dh > 0) { c.returning = true; c.returnSinceTick = tick_; c.nextHomeTick = 0; }
        else if (c.hp < c.maxHp) Heal(c, c.maxHp);
    }
    if (!c.returning) return;
    if (dh == 0) {
        c.returning = false;
        if (c.hp < c.maxHp) Heal(c, c.maxHp);
        return;
    }
    Map* m = world_.GetMap(c.mapId);
    if (!m) return;
    if (tick_ - c.returnSinceTick > static_cast<uint64_t>(rt_.HomeGiveUpSec) * cfg_.tickRate) {   // blocked for too long: put it back
        c.returning = false;
        Teleport(c, m->id, c.home.pos);
        if (c.hp < c.maxHp) Heal(c, c.maxHp);
        return;
    }
    if (c.IsMoving() || !IsFree(c) || tick_ < c.nextHomeTick) return;
    c.nextHomeTick = tick_ + 10;
    TilePos goal = c.home.pos;
    Critter* occ = m->CritterAt(goal);
    if (occ && occ != &c) {   // someone stands on the home cell: the nearest free cell next to it is home enough
        if (dh <= 2) { c.returning = false; if (c.hp < c.maxHp) Heal(c, c.maxHp); return; }
        TilePos near{};
        if (m->FindFreeNear(goal, 2, near)) goal = near;
    }
    MoveCritterTo(c, goal);
}

// The last player left the map: hunters are unobserved, they go home instantly and heal; nothing is left half way across the map.
void Server::ResetNpcsOfMap(Map& m) {
    std::vector<CritterId> ids;
    for (Critter* c : m.critters) if (c->IsNpc() && !c->dead && !c->isStatic) ids.push_back(c->id);
    for (CritterId id : ids) {
        Critter* c = world_.GetCritter(id);
        if (!c) continue;
        const bool touched = c->aggro || c->returning || c->target != 0;
        c->target = 0; c->aggro = false; c->returning = false;
        if (touched && c->home.has && c->home.map == m.id && c->pos != c->home.pos) {
            StopMove(*c, 1);
            if (amx_) amx_->ClearActionsOf(c->id);
            Teleport(*c, m.id, c->home.pos);
        }
        if (c->hp < c->maxHp) { c->hp = c->maxHp; }
    }
}

// ------------------------------------------------------------------ dev / test helper (/npcdev, --dev-kit servers only)
//   find <name part>      first matching NPC of the caller's map (living first): "id=7 x=.. y=.. hp=.. dead=0 home=x,y returning=0"
//   info <id>             the same for one critter
//   kill <id>             kills it
//   spawn <proto key> <x> <y> [respawn seconds]   tracked spawn on the caller's map (home = that cell); prints the id
//   at <x> <y> | count <name part> | hurt <id> <hp> | warp <id> <x> <y>
//   pending               number of queued respawns
//   radius <cells>        RespawnPlayerRadius for this run;  leash <cells>  AggroLeashCells for this run
//   target <npc id> <critter id>   makes the NPC hunt that critter (like OnAttacked)
//   cleanup <0|1>         AmxJunktownCleanup for this run
std::string Server::NpcDev(Critter& caller, const std::string& args) {
    std::istringstream is(args);
    std::string cmd;
    is >> cmd;
    auto describe = [&](const Critter& c) {
        std::ostringstream o;
        o << "id=" << c.id << " name=" << c.name << " x=" << c.pos.x << " y=" << c.pos.y << " hp=" << c.hp << "/" << c.maxHp << " dead=" << (c.dead ? 1 : 0)
          << " home=" << c.home.pos.x << "," << c.home.pos.y << " returning=" << (c.returning ? 1 : 0) << " target=" << c.target << " map=" << c.mapId;
        return o.str();
    };
    if (cmd == "find") {
        std::string part;
        std::getline(is >> std::ws, part);
        std::string low = part;
        for (char& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        const Critter* best = nullptr;
        for (auto& [id, c] : world_.Critters()) {
            if (!c->IsNpc() || c->mapId != caller.mapId) continue;
            std::string n = c->name;
            for (char& ch : n) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (n.find(low) == std::string::npos) continue;
            if (!best || (best->dead && !c->dead)) best = c.get();
        }
        return best ? describe(*best) : "none";
    }
    if (cmd == "info") {
        unsigned id = 0; is >> id;
        const Critter* c = world_.GetCritter(id);
        return c ? describe(*c) : "none";
    }
    if (cmd == "kill") {
        unsigned id = 0; is >> id;
        Critter* c = world_.GetCritter(id);
        if (!c) return "none";
        KillCritter(*c, nullptr);
        return "killed";
    }
    if (cmd == "spawn") {
        std::string key; int x = 0, y = 0, rs = -1;
        is >> key >> x >> y >> rs;
        const Proto* p = data_.FindProto(key);
        if (!p || p->cls != PClass::Critter) return "unknown critter prototype";
        Critter* c = SpawnCritterProto(caller.mapId, *p, TilePos{static_cast<int16_t>(x), static_cast<int16_t>(y)}, nullptr);
        if (!c) return "spawn failed";
        TrackSpawn(*c, nullptr, std::string(), rs);
        return "spawned " + describe(*c);
    }
    if (cmd == "at") {   // NPC standing on a cell of the caller's map (living first)
        int x = 0, y = 0; is >> x >> y;
        const Critter* best = nullptr;
        for (auto& [id, c] : world_.Critters())
            if (c->IsNpc() && c->mapId == caller.mapId && c->pos.x == x && c->pos.y == y && (!best || (best->dead && !c->dead))) best = c.get();
        return best ? describe(*best) : "none";
    }
    if (cmd == "count") {   // living NPCs of the caller's map whose name contains the text
        std::string part;
        std::getline(is >> std::ws, part);
        int n = 0;
        for (auto& [id, c] : world_.Critters()) if (c->IsNpc() && !c->dead && c->mapId == caller.mapId && c->name.find(part) != std::string::npos) ++n;
        return "count " + std::to_string(n);
    }
    if (cmd == "hurt") { unsigned id = 0; int n = 0; is >> id >> n; Critter* c = world_.GetCritter(id); if (!c || c->dead) return "none"; c->hp = std::max(1, c->hp - n); return "ok"; }
    if (cmd == "warp") { unsigned id = 0; int x = 0, y = 0; is >> id >> x >> y; Critter* c = world_.GetCritter(id); if (!c) return "none"; StopMove(*c, 1); return Teleport(*c, c->mapId, TilePos{static_cast<int16_t>(x), static_cast<int16_t>(y)}) ? "ok" : "failed"; }
    if (cmd == "removeat") {   // removes every NPC (corpses too) standing on a cell of the caller's map, no respawn (test clean-up)
        int x = 0, y = 0; is >> x >> y;
        std::vector<CritterId> ids;
        for (auto& [id, c] : world_.Critters()) if (c->IsNpc() && c->mapId == caller.mapId && c->pos.x == x && c->pos.y == y) ids.push_back(id);
        for (CritterId id : ids) { ForgetSpawn(id); RemoveCritter(id); }
        return "removed " + std::to_string(ids.size());
    }
    if (cmd == "pending") return "pending " + std::to_string(respawnQ_.size());
    if (cmd == "radius") { int v = 0; is >> v; rt_.RespawnPlayerRadius = std::clamp(v, 0, 400); return "ok"; }
    if (cmd == "leash") { int v = 0; is >> v; rt_.AggroLeashCells = std::max(0, v); return "ok"; }
    if (cmd == "cleanup") { int v = 0; is >> v; rt_.AmxJunktownCleanup = v != 0; return "ok"; }
    if (cmd == "target") {
        unsigned n = 0, t = 0; is >> n >> t;
        Critter* c = world_.GetCritter(n);
        if (!c || !world_.GetCritter(t)) return "none";
        c->target = t; c->nextRepathTick = 0;
        return "ok";
    }
    return "usage: find <name> | info <id> | kill <id> | spawn <proto> <x> <y> [respawn s] | pending | radius N | leash N | cleanup 0|1 | target <npc> <critter>";
}

}  // namespace f3
