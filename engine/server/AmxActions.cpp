// AMX action queue + the natives of the tutorial / NPC behaviour scripts (A_*, speech, facing, squads, ranks, items, attributes).
// Signatures are inferred from the listings (tools/amxdis): see docs/amx-scripts.md "Natives". Arguments arrive in call order (a[0] = first).
#include <algorithm>
#include <cmath>
#include <cstring>
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
                    case ActType::Move: if (!server_.StartMove(*c, a.x, a.y, a.run, false)) a.endMs = now; break;
                    case ActType::Attack: if (Critter* t = server_.GetWorld().GetCritter(a.target)) { (void)t;   /* AMX-driven attacks are deliberately not executed: aggression stays in scripts/npc/hostile.lua (see docs/amx-scripts.md) */ } a.endMs = now; break;
                    default: break;
                }
            }
            bool done = false;
            switch (a.type) {
                case ActType::Move: done = !c->IsMoving() || now >= a.endMs + 20000; break;
                default: done = now >= a.endMs; break;
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
    auto speech = [this](AmxInstance&, const AmxCell* a, int n, bool queued) -> AmxCell {
        if (n < 2) return 0;
        Critter* c = CritterOf(a[0]);
        if (!c) return 0;
        std::string t = DlgText(a[1]);
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
    N["A_MoveToPosition"] = [this, cellOf](AmxInstance&, const AmxCell* a, int n) -> AmxCell {   // (entity, x, y(height), z) world units as floats
        Critter* c = n > 3 ? CritterOf(a[0]) : nullptr;
        if (!c) return 0;
        Act q; q.type = ActType::Move; q.run = false;
        cellOf(*c, FVal(a[1]), FVal(a[3]), q.x, q.y);
        QueueAction(c->id, std::move(q));
        return 1;
    };
    N["A_MoveToEntity"] = [this](AmxInstance&, const AmxCell* a, int n) -> AmxCell {   // (entity, target, ?, ?)
        Critter* c = n > 1 ? CritterOf(a[0]) : nullptr;
        Critter* t = n > 1 ? CritterOf(a[1]) : nullptr;
        if (!c || !t) return 0;
        Act q; q.type = ActType::Move; q.run = false; q.x = t->fx; q.y = t->fy;
        QueueAction(c->id, std::move(q));
        return 1;
    };
    auto attack = [this](AmxInstance&, const AmxCell* a, int n) -> AmxCell {   // (entity, target)
        Critter* c = n > 1 ? CritterOf(a[0]) : nullptr;
        Critter* t = n > 1 ? CritterOf(a[1]) : nullptr;
        if (!c || !t) return 0;
        Act q; q.type = ActType::Attack; q.target = t->id;
        QueueAction(c->id, std::move(q));
        return 1;
    };
    N["A_Attack"] = attack;
    N["A_JustAttack"] = attack;
    N["A_Wander"] = [](AmxInstance&, const AmxCell*, int) -> AmxCell { return 0; };   // wandering is left to the Lua npc behaviours
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

    // ---- facing / geometry
    auto dirFromAngle = [](float deg) {   // original angles are degrees; the server has 8 directions (45 degrees each)
        int d = static_cast<int>(std::lround(deg / 45.f)) & 7;
        return static_cast<Dir>(d);
    };
    N["SetFacing"] = [this, dirFromAngle](AmxInstance&, const AmxCell* a, int n) -> AmxCell {   // (entity, angle float)
        if (Critter* c = n > 1 ? CritterOf(a[0]) : nullptr) { c->dir = dirFromAngle(FVal(a[1])); return 1; }
        return 0;
    };
    N["SetFacingTarget"] = [this](AmxInstance&, const AmxCell* a, int n) -> AmxCell {   // (entity, target)
        Critter* c = n > 1 ? CritterOf(a[0]) : nullptr;
        Critter* t = n > 1 ? CritterOf(a[1]) : nullptr;
        if (!c || !t) return 0;
        c->dir = DirFromDelta(t->pos.x - c->pos.x, t->pos.y - c->pos.y);
        return 1;
    };
    N["GetDirectionToEntity"] = [this](AmxInstance&, const AmxCell* a, int n) -> AmxCell {   // (entity, target) -> angle float
        Critter* c = n > 1 ? CritterOf(a[0]) : nullptr;
        Critter* t = n > 1 ? CritterOf(a[1]) : nullptr;
        if (!c || !t) return FBits(0.f);
        return FBits(static_cast<float>(static_cast<int>(DirFromDelta(t->pos.x - c->pos.x, t->pos.y - c->pos.y)) * 45));
    };
    N["PointDistance"] = [this](AmxInstance& m, const AmxCell* a, int n) -> AmxCell {   // (float p1[3], float p2[3]) by reference, world units
        if (n < 2) return FBits(0.f);
        float d[3];
        for (int i = 0; i < 3; i++) d[i] = FVal(m.Get(a[0] + 4 * i)) - FVal(m.Get(a[1] + 4 * i));
        return FBits(std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
    };

    // ---- squads / team ranks (stored per critter; the scripts compare them to decide who leads)
    N["CreateSquad"] = [this](AmxInstance&, const AmxCell*, int) -> AmxCell { return nextSquad_++; };
    N["AddToSquad"] = [this](AmxInstance&, const AmxCell* a, int n) -> AmxCell {   // (squad, entity)
        if (Critter* c = n > 1 ? CritterOf(a[1]) : nullptr) { squadOf_[c->id] = a[0]; return 1; }
        return 0;
    };
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
        std::string t = DlgText(a[n - 1]);
        if (p && !t.empty()) server_.Tell(*p, t);
        return p ? 1 : 0;
    };
    N["IsEntityInArea"] = [this](AmxInstance& m, const AmxCell* a, int n) -> AmxCell {   // (entity, area$): the area names a map here
        Critter* c = n > 1 ? CritterOf(a[0]) : nullptr;
        if (!c) return 0;
        const Map* mp = server_.GetWorld().GetMap(c->mapId);
        return mp && DataStore::Lower(mp->name) == DataStore::Lower(m.GetString(a[1])) ? 1 : 0;
    };
    N["HasItem"] = [this](AmxInstance&, const AmxCell*, int) -> AmxCell { return 0; };   // item type ids are unresolved: nobody owns anything
    N["PartyHasItem"] = N["HasItem"];
    N["IsInParty"] = [](AmxInstance&, const AmxCell*, int) -> AmxCell { return 0; };
    N["GetWayPointCount"] = [](AmxInstance&, const AmxCell*, int) -> AmxCell { return 0; };   // named paths are not exported by the importer
    N["GetNearestPath"] = [](AmxInstance&, const AmxCell*, int) -> AmxCell { return 0; };
    (void)self;
}

}  // namespace f3
