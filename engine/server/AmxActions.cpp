// AMX action queue + the natives of the tutorial / NPC behaviour scripts (A_*, speech, facing, squads, ranks, items, attributes).
// Signatures are inferred from the listings (tools/amxdis): see docs/amx-scripts.md "Natives". Arguments arrive in call order (a[0] = first).
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include "AmxHost.h"
#include "Server.h"
#include "common/log.h"

namespace f3 {

namespace {
AmxCell FBits(float f) { AmxCell c; std::memcpy(&c, &f, 4); return c; }
float FVal(AmxCell c) { float f; std::memcpy(&f, &c, 4); return f; }
}  // namespace

// Critter whose conversation / quest context a script call belongs to: the running conversation's player, else (behaviour
// scripts such as Frank's OnUpdate) the nearest player within 20 cells of the script owner: it plays the part of the original
// single-player "protagonist" so per-player quest globals work for world NPCs too.
Critter* AmxHost::Protagonist(AmxInstance& m) {
    if (active_) return server_.GetWorld().GetCritter(active_->pc);
    Critter* self = CritterOf(Handle(m.userData));
    if (!self) return nullptr;
    Critter* best = nullptr;
    int bd = 21;
    if (Map* mp = server_.GetWorld().GetMap(self->mapId))
        for (Critter* o : mp->critters)
            if (o->IsPlayer() && !o->dead) { int d = TileDist(o->pos, self->pos); if (d < bd) { bd = d; best = o; } }
    return best;
}

void AmxHost::QueueAction(CritterId id, Act a) { actions_[id].push_back(std::move(a)); }

// Runs every tick for critters with a queue: one action at a time, like the original action queue (A_* append, ClearActions empties).
void AmxHost::TickActions(const std::unordered_set<MapId>& active) {
    const uint64_t now = server_.NowMs();
    for (auto it = actions_.begin(); it != actions_.end();) {
        Critter* c = server_.GetWorld().GetCritter(it->first);
        if (!c || c->dead) { it = actions_.erase(it); continue; }
        auto& q = it->second;
        if (!q.empty() && active.count(c->mapId)) {
            Act& a = q.front();
            if (!a.started) {
                a.started = true;
                a.endMs = now + static_cast<uint64_t>(std::max(0, a.ms));
                switch (a.type) {
                    case ActType::Speech: if (!a.text.empty()) server_.Say(*c, a.text); break;
                    case ActType::Move: {
                        if (a.follow) {   // A_MoveToEntity: head for the target's current position
                            Critter* t = server_.GetWorld().GetCritter(a.target);
                            if (!t || t->mapId != c->mapId) { a.endMs = now; break; }
                            a.x = t->fx; a.y = t->fy; a.lastRepath = now;
                        }
                        if (!server_.StartMove(*c, a.x, a.y, a.run, false)) a.endMs = now;
                        break;
                    }
                    case ActType::Attack: {
                        // A_Attack / A_JustAttack: hand the target to the server combat (TickCombat: weapon range, auto-move, line of sight,
                        // AP, breaktime, hit roll, damage). The action ends after ONE attack, when the target is gone, or after 30 s.
                        Critter* t = server_.GetWorld().GetCritter(a.target);
                        a.endMs = now + 30000;
                        a.startAttacks = c->attacksDone;
                        const bool reachable = t && t != c && !t->dead && t->mapId == c->mapId;
                        if (!attacksEnabled_ || !reachable || ((a.noMove || c->isStatic) && !server_.InAttackRange(*c, *t))) { a.endMs = now; break; }
                        server_.StartAttack(*c, a.target);
                        break;
                    }
                    case ActType::Wander: {   // original ActionWander: pick a point in the rectangle, queue Wait(1000..5999 ms), MoveTo, then the next Wander
                        const Act w = a;
                        a.endMs = now;
                        float tx = w.ulx, tz = w.ulz;
                        auto range = [&](float first, float last) {
                            if (first == last) return first;
                            if (last < first) std::swap(first, last);
                            return first + static_cast<float>(RandomRange(0, 1 << 30) % static_cast<int>(last - first + 1.f));
                        };
                        if (w.lrx >= w.ulx) tx = range(w.ulx, w.lrx);        // lowerRight.x < upperLeft.x: invalid rect, keeps upperLeft.x
                        if (!(w.ulz < w.lrz)) tz = range(w.ulz, w.lrz);      // upperLeft.z < lowerRight.z: invalid rect, keeps upperLeft.z
                        const Map* mp = server_.GetWorld().GetMap(c->mapId);
                        Act wait; wait.type = ActType::Wait; wait.ms = RandomRange(1000, 5999);
                        Act move; move.type = ActType::Move;
                        move.x = (tx - (mp ? mp->originX : 0.f)) * 2.f; move.y = (tz - (mp ? mp->originZ : 0.f)) * 2.f;
                        q.push_back(std::move(wait));
                        q.push_back(std::move(move));
                        if (w.reps != 1) { Act next = w; next.started = false; next.reps = w.reps ? w.reps - 1 : 0; q.push_back(std::move(next)); }
                        break;
                    }
                    default: break;
                }
            }
            Act& cur = q.front();   // (the queue may have grown, the front is still the running action)
            bool done = false;
            switch (cur.type) {
                case ActType::Move:
                    if (cur.follow) {
                        Critter* t = server_.GetWorld().GetCritter(cur.target);
                        if (!t || t->mapId != c->mapId) { if (c->IsMoving()) server_.StopMove(*c, 1); done = true; break; }
                        const Map* mp = server_.GetWorld().GetMap(c->mapId);
                        const float dx = (t->fx - c->fx) * 0.5f, dz = (t->fy - c->fy) * 0.5f;
                        const float dist = std::sqrt(dx * dx + dz * dz);
                        const float tdx = (t->fx - cur.x) * 0.5f, tdz = (t->fy - cur.y) * 0.5f;
                        if (std::sqrt(tdx * tdx + tdz * tdz) > 20.f) {   // the target walked away: re-path every 2 s
                            if (now - cur.lastRepath >= 2000) {
                                cur.lastRepath = now; cur.x = t->fx; cur.y = t->fy;
                                if (!server_.StartMove(*c, cur.x, cur.y, cur.run, false)) done = true;
                            }
                        } else if (dist <= cur.stopDist) {
                            bool ok = true;
                            if (cur.reach >= 0 && mp)
                                ok = LineClear(*mp, c->fx * 0.5f + mp->originX, c->fy * 0.5f + mp->originZ, t->fx * 0.5f + mp->originX, t->fy * 0.5f + mp->originZ) == (cur.reach == 2);
                            if (ok) { if (c->IsMoving()) server_.StopMove(*c, 1); done = true; }
                        }
                        if (!done && (!c->IsMoving() || now >= cur.endMs + 20000)) done = true;
                    } else done = !c->IsMoving() || now >= cur.endMs + 20000;
                    break;
                case ActType::Attack: {
                    Critter* t = server_.GetWorld().GetCritter(cur.target);
                    const bool attacked = c->attacksDone > cur.startAttacks;
                    if (attacked && c->target == cur.target) c->target = 0;   // exactly one attack: stop TickCombat before the breaktime ends
                    done = !t || t->dead || t->mapId != c->mapId || now >= cur.endMs || (attacked && server_.IsFree(*c));
                    if (done && c->target == cur.target) c->target = 0;
                    break;
                }
                default: done = now >= cur.endMs; break;
            }
            if (done) q.pop_front();
        }
        if (q.empty()) it = actions_.erase(it); else ++it;
    }
}

bool AmxHost::IsIdle(CritterId id) const {
    auto it = actions_.find(id);
    return it == actions_.end() || it->second.empty();
}

void AmxHost::OnAttacked(Critter& victim, Critter& attacker) {
    auto it = instances_.find(victim.id);
    if (it == instances_.end() || !it->second.created || !it->second.inst->Script().HasPublic("OnAttacked")) return;
    AmxCell h = Handle(attacker.id);
    it->second.inst->Call("OnAttacked", {h, h});
}

void AmxHost::BindActionNatives() {
    auto& N = natives_;
    auto self = [this](AmxInstance& m) { return CritterOf(Handle(m.userData)); };
    auto cellOf = [this](const Critter& c, float wx, float wz, float& cx, float& cy) {
        const Map* mp = server_.GetWorld().GetMap(c.mapId);
        cx = (wx - (mp ? mp->originX : 0.f)) * 2.f;
        cy = (wz - (mp ? mp->originZ : 0.f)) * 2.f;
    };

    // ---- speech: (entity, stfId, durationMs[, r, g, b]). Spoken as local chat (the client has no floating text yet).
    auto speech = [this](AmxInstance& m, const AmxCell* a, int n, bool queued) -> AmxCell {
        if (n < 2) return 0;
        Critter* c = CritterOf(a[0]);
        if (!c) return 0;
        std::string t = DlgText(a[1], Protagonist(m));   // the nearest player supplies <CHARNAME>
        if (t.empty()) return 0;
        if (queued) { Act q; q.type = ActType::Speech; q.text = t; q.ms = n > 2 ? std::clamp(a[2], 0, 20000) : 1500; QueueAction(c->id, std::move(q)); }
        else server_.Say(*c, t);
        return 1;
    };
    N["FloatingSpeechText"] = [speech](AmxInstance& m, const AmxCell* a, int n) { return speech(m, a, n, false); };
    N["A_FloatingSpeechText"] = [speech](AmxInstance& m, const AmxCell* a, int n) { return speech(m, a, n, true); };

    // ---- action queue
    N["A_Wait"] = [this](AmxInstance&, const AmxCell* a, int n) -> AmxCell {   // (entity, ms)
        Critter* c = n > 1 ? CritterOf(a[0]) : nullptr;
        if (!c) return 0;
        Act q; q.type = ActType::Wait; q.ms = std::clamp(a[1], 0, 600000);
        QueueAction(c->id, std::move(q));
        return 1;
    };
    N["A_PlayAnim"] = [this](AmxInstance&, const AmxCell* a, int n) -> AmxCell {   // (entity, anim, ...): no animation protocol yet, occupies the queue briefly
        Critter* c = n > 0 ? CritterOf(a[0]) : nullptr;
        if (!c) return 0;
        Act q; q.type = ActType::Wait; q.ms = 600;
        QueueAction(c->id, std::move(q));
        return 1;
    };
    // A_MoveToPosition(entity, float dest[3], stopDist, ignored): the destination is a vector in world units; the original verifies four
    // parameters, stores stopDist only if > 0 and only uses it when moving to an entity, so it is irrelevant here.
    N["A_MoveToPosition"] = [this, cellOf](AmxInstance& m, const AmxCell* a, int n) -> AmxCell {
        if (!Argc("A_MoveToPosition", n, 4)) return 0;
        Critter* c = CritterOf(a[0]);
        float v[3];
        if (!c || !ReadVec(m, a[1], v)) return 0;
        Act q; q.type = ActType::Move; q.run = false;
        cellOf(*c, v[0], v[2], q.x, q.y);
        QueueAction(c->id, std::move(q));
        return 1;
    };
    // A_MoveToEntity(entity, target, stopDist, reachability): stopDist replaces the default 5 world units only when NEGATIVE (original quirk:
    // it is an unsigned compare, so a negative value means "arrived at once"); reachability != -1 additionally requires GetLOS == value.
    N["A_MoveToEntity"] = [this](AmxInstance&, const AmxCell* a, int n) -> AmxCell {
        if (!Argc("A_MoveToEntity", n, 4)) return 0;
        Critter* c = CritterOf(a[0]);
        Critter* t = CritterOf(a[1]);
        if (!c || !t) return 0;
        Act q; q.type = ActType::Move; q.run = false; q.follow = true; q.target = t->id; q.x = t->fx; q.y = t->fy;
        if (a[2] < 0) q.stopDist = 1e30f;
        if (a[3] != -1) q.reach = a[3];
        QueueAction(c->id, std::move(q));
        return 1;
    };
    // A_Attack(entity, target) / A_JustAttack(entity, target): queue an attack action (see TickActions). JustAttack neither walks nor turns.
    auto attack = [this](const char* name, bool noMove) {
        return [this, name, noMove](AmxInstance&, const AmxCell* a, int n) -> AmxCell {
            if (!Argc(name, n, 2)) return 0;
            Critter* c = CritterOf(a[0]);
            Critter* t = CritterOf(a[1]);
            if (!c || !t) return 0;
            Act q; q.type = ActType::Attack; q.target = t->id; q.noMove = noMove;
            QueueAction(c->id, std::move(q));
            return 1;
        };
    };
    N["A_Attack"] = attack("A_Attack", false);
    N["A_JustAttack"] = attack("A_JustAttack", true);
    // A_Wander(entity, float upperLeft[3], float lowerRight[3], repetitions): random points in the rectangle, 0 repetitions = forever
    N["A_Wander"] = [this](AmxInstance& m, const AmxCell* a, int n) -> AmxCell {
        if (!Argc("A_Wander", n, 4)) return 0;
        Critter* c = CritterOf(a[0]);
        float ul[3], lr[3];
        if (!c || !ReadVec(m, a[1], ul) || !ReadVec(m, a[2], lr)) return 0;
        Act q; q.type = ActType::Wander; q.ulx = ul[0]; q.ulz = ul[2]; q.lrx = lr[0]; q.lrz = lr[2]; q.reps = a[3];
        QueueAction(c->id, std::move(q));
        return 1;
    };
    N["A_Kill"] = [this](AmxInstance&, const AmxCell* a, int n) -> AmxCell {
        Critter* c = n > 0 ? CritterOf(a[0]) : nullptr;
        if (c && c->IsNpc()) { server_.KillCritter(*c, nullptr); return 1; }
        return 0;
    };
    N["A_ReloadWeapon"] = [](AmxInstance&, const AmxCell*, int) -> AmxCell { return 0; };
    N["ClearActions"] = [this](AmxInstance&, const AmxCell* a, int n) -> AmxCell {
        if (Critter* c = n > 0 ? CritterOf(a[0]) : nullptr) { actions_.erase(c->id); server_.StopMove(*c, 1); c->target = 0; }
        return 0;
    };
    N["IsIdle"] = [this](AmxInstance&, const AmxCell* a, int n) -> AmxCell {
        Critter* c = n > 0 ? CritterOf(a[0]) : nullptr;
        return c ? (IsIdle(c->id) && !c->IsMoving() ? 1 : 0) : 1;
    };

    // ---- squads / team ranks (stored per critter; the scripts compare them to decide who leads)
    N["CreateSquad"] = [this](AmxInstance&, const AmxCell*, int) -> AmxCell { return nextSquad_++; };
    // AddToSquad(entity, squad): entity FIRST (original NativeAddToSquad -> AssignEntitySquad(entity, squad); the old (squad, entity) order was wrong)
    N["AddToSquad"] = [this](AmxInstance&, const AmxCell* a, int n) -> AmxCell {
        if (!Argc("AddToSquad", n, 2)) return 0;
        if (Critter* c = CritterOf(a[0])) { squadOf_[c->id] = a[1]; return 1; }
        return 0;
    };
    // attitudes are directed (source -> target) integers; equal ids are always 1 (original GetTeamAttitude / GetSquadAttitude), unset 0.
    // The engine never reads them for combat, only the scripts do (their hostility decisions).
    auto attSet = [this](const char* name, std::map<std::pair<int, int>, int>& tab) {
        return [this, name, &tab](AmxInstance&, const AmxCell* a, int n) -> AmxCell {
            if (!Argc(name, n, 3)) return 0;
            if (a[0] <= 0 || a[1] <= 0) return 0;
            tab[{a[0], a[1]}] = a[2];
            return 1;
        };
    };
    auto attGet = [this](const char* name, std::map<std::pair<int, int>, int>& tab) {
        return [this, name, &tab](AmxInstance&, const AmxCell* a, int n) -> AmxCell {
            if (!Argc(name, n, 2)) return 0;
            if (a[0] == a[1]) return 1;
            auto it = tab.find({a[0], a[1]});
            return it == tab.end() ? 0 : it->second;
        };
    };
    N["SetTeamAttitude"] = attSet("SetTeamAttitude", teamAttitude_);
    N["SetSquadAttitude"] = attSet("SetSquadAttitude", squadAttitude_);
    N["GetTeamAttitude"] = attGet("GetTeamAttitude", teamAttitude_);
    N["GetSquadAttitude"] = attGet("GetSquadAttitude", squadAttitude_);
    N["GetSquadID"] = [this](AmxInstance&, const AmxCell* a, int n) -> AmxCell {
        Critter* c = n > 0 ? CritterOf(a[0]) : nullptr;
        auto it = c ? squadOf_.find(c->id) : squadOf_.end();
        return it == squadOf_.end() ? 0 : it->second;
    };
    N["SetSquadRank"] = [this](AmxInstance&, const AmxCell* a, int n) -> AmxCell {   // (entity, rank)
        if (Critter* c = n > 1 ? CritterOf(a[0]) : nullptr) { squadRank_[c->id] = a[1]; return 1; }
        return 0;
    };
    N["GetSquadRank"] = [this](AmxInstance&, const AmxCell* a, int n) -> AmxCell {
        Critter* c = n > 0 ? CritterOf(a[0]) : nullptr;
        auto it = c ? squadRank_.find(c->id) : squadRank_.end();
        return it == squadRank_.end() ? 0 : it->second;
    };
    N["SetTeamRank"] = [this](AmxInstance&, const AmxCell* a, int n) -> AmxCell {
        if (Critter* c = n > 1 ? CritterOf(a[0]) : nullptr) { teamRank_[c->id] = a[1]; return 1; }
        return 0;
    };
    N["GetTeamRank"] = [this](AmxInstance&, const AmxCell* a, int n) -> AmxCell {
        Critter* c = n > 0 ? CritterOf(a[0]) : nullptr;
        auto it = c ? teamRank_.find(c->id) : teamRank_.end();
        return it == teamRank_.end() ? 0 : it->second;
    };

    // ---- attributes, skills, items, messages to the player
    N["GetPrimaryAttribute"] = [this](AmxInstance&, const AmxCell* a, int n) -> AmxCell {   // (entity, attr 0..6 = S P E C I A L) (guess)
        Critter* c = n > 1 ? CritterOf(a[0]) : nullptr;
        return c && a[1] >= 0 && a[1] < 7 ? c->special[a[1]] : 0;
    };
    N["GetTotalSkill"] = [this](AmxInstance&, const AmxCell* a, int n) -> AmxCell {   // (entity, skill): ids unknown, mapped onto the server skill list in order
        Critter* c = n > 1 ? CritterOf(a[0]) : nullptr;
        return c && a[1] >= 0 && a[1] < kSkillCount ? server_.SkillValue(*c, a[1]) : 0;
    };
    N["DisplayStringRef"] = [this](AmxInstance& m, const AmxCell* a, int n) -> AmxCell {   // (?, ?, ?, stfId) -> a line in the player's log (guess: last arg is the string)
        if (n < 1) return 0;
        Critter* p = Protagonist(m);
        std::string t = DlgText(a[n - 1], p);
        if (p && !t.empty()) server_.Tell(*p, t);
        return p ? 1 : 0;
    };
    N["IsEntityInArea"] = [this](AmxInstance& m, const AmxCell* a, int n) -> AmxCell {   // (entity, area$): the area names a map here
        Critter* c = n > 1 ? CritterOf(a[0]) : nullptr;
        if (!c) return 0;
        const Map* mp = server_.GetWorld().GetMap(c->mapId);
        return mp && DataStore::Lower(mp->name) == DataStore::Lower(m.GetString(a[1])) ? 1 : 0;
    };
    N["IsInParty"] = [](AmxInstance&, const AmxCell*, int) -> AmxCell { return 0; };   // there is no party in the multiplayer demo
    (void)self;
}

}  // namespace f3
