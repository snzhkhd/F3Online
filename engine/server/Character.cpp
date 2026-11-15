// Character system: SPECIAL allocation, derived stats, skills, experience / levels, skill use. See docs/server-data.md section 11.
// All formulas are placeholders in the spirit of Fallout 1/2 (the demo data carries no formulas): every number is in scripts/config.lua.
#include <algorithm>
#include <bitset>
#include "Scripting.h"
#include "Server.h"
#include "common/log.h"

namespace f3 {

// ------------------------------------------------------------------ formulas

int Server::SkillRate(const Critter& c) const { return std::max(1, rt_.SkillRateBase + rt_.SkillRatePerInt * c.special[4]); }

// base value of a skill from SPECIAL (S P E C I A L = indices 0..6)
int Server::SkillBase(const Critter& c, int skill) const {
    const int S = c.special[0], P = c.special[1], E = c.special[2], C = c.special[3], I = c.special[4], A = c.special[5];
    switch (skill) {
        case 0: return 5 + 4 * A;               // Firearms
        case 1: return 20 + 2 * (S + A);        // Melee
        case 2: return 30 + 2 * (S + A);        // Unarmed
        case 3: return 4 * C;                   // Barter
        case 4: return 5 + 4 * C;               // Persuasion
        case 5: return 5 + 3 * C + I;           // Deception
        case 6: return 3 * I;                   // Mechanics
        case 7: return 2 * (P + I);             // Medic
        case 8: return 5 + 2 * (E + I);         // Outdoorsman
        case 9: return 4 * I;                   // Science
        case 10: return 10 + P + A;             // Security
        case 11: return 5 + 3 * A;              // Sneak
        case 12: return 3 + 3 * A;              // Steal
        case 13: return 10 + P + A;             // Traps
        default: return 0;
    }
}

int Server::SkillValue(const Critter& c, int skill) const {
    if (skill < 0 || skill >= kSkillCount) return 0;
    int v = SkillBase(c, skill);
    if (c.tagMask & (1u << skill)) v += rt_.TagBonus;
    auto it = c.skillRanks.find(kSkillNames[skill]);
    if (it != c.skillRanks.end()) v += it->second;
    return std::clamp(v, 0, rt_.SkillMax);
}

// total xp needed to be at `level` (level 1 = 0)
int Server::XpForLevel(int level) const { return level <= 1 ? 0 : rt_.XpBase * level * (level - 1) / 2; }

void Server::RecalcDerived(Critter& c) {
    const int E = c.special[2], S = c.special[0];
    c.maxHp = std::max(1, rt_.HpBase + S + 2 * E + (c.level - 1) * (rt_.HpPerLevelBase + E / 2));
    if (c.hp > c.maxHp) c.hp = c.maxHp;
}

// ------------------------------------------------------------------ messages

void Server::SendCharSheet(Critter& c) {
    if (!c.peer || !c.IsPlayer()) return;
    gnet::CharSheet m;
    m.created = c.created;
    m.level = static_cast<uint8_t>(std::min(255, c.level));
    m.perkPoints = static_cast<uint8_t>(std::min(255, c.perkPoints));
    m.unspentSpecial = static_cast<uint8_t>(std::min(255, c.unspentSpecial));
    m.creationTotal = static_cast<uint8_t>(7 * rt_.SpecialBase + rt_.CharPoints);
    m.xp = static_cast<uint32_t>(c.xp);
    m.xpNext = c.level >= rt_.MaxLevel ? 0 : static_cast<uint32_t>(XpForLevel(c.level + 1));
    m.skillPoints = static_cast<uint16_t>(std::min(65535, c.skillPoints));
    m.tagMask = c.tagMask;
    for (int i = 0; i < 7; i++) m.special[i] = static_cast<uint8_t>(c.special[i]);
    for (int i = 0; i < kSkillCount; i++) {
        m.skill[i] = static_cast<uint16_t>(SkillValue(c, i));
        auto it = c.skillRanks.find(kSkillNames[i]);
        m.rank[i] = static_cast<uint8_t>(it == c.skillRanks.end() ? 0 : std::min(255, it->second));
    }
    m.maxAp = static_cast<uint8_t>(std::min(255, MaxAp(c) / 100));
    m.healRate = static_cast<uint8_t>(std::max(1, c.special[2] / 3));
    m.critChance = static_cast<uint8_t>(std::min(100, c.special[6]));
    m.skillRate = static_cast<uint8_t>(std::min(255, SkillRate(c)));
    m.tagCount = static_cast<uint8_t>(rt_.TagCount);
    m.traitMask = c.traitMask;
    m.traitMax = static_cast<uint8_t>(rt_.TraitMax);
    Send(c, gnet::PackG(m));
}

// ------------------------------------------------------------------ creation

bool Server::CreateCharacter(Critter& c, const gnet::CharCreate& m) {
    if (c.created || !c.IsPlayer()) return false;
    int sum = 0;
    for (int i = 0; i < 7; i++) {
        if (m.special[i] < rt_.SpecialMin || m.special[i] > rt_.SpecialMax) { Tell(c, "Character creation: SPECIAL values must be between " + std::to_string(rt_.SpecialMin) + " and " + std::to_string(rt_.SpecialMax) + "."); return false; }
        sum += m.special[i];
    }
    const int total = 7 * rt_.SpecialBase + rt_.CharPoints;
    if (sum != total) { Tell(c, "Character creation: distribute exactly " + std::to_string(total) + " SPECIAL points (you used " + std::to_string(sum) + ")."); return false; }
    if (m.tagMask >> kSkillCount) { Tell(c, "Character creation: invalid tag skills."); return false; }
    if (static_cast<int>(std::bitset<16>(m.tagMask).count()) != rt_.TagCount) { Tell(c, "Character creation: choose exactly " + std::to_string(rt_.TagCount) + " tag skills."); return false; }
    if (static_cast<int>(std::bitset<16>(m.traitMask).count()) > rt_.TraitMax) { Tell(c, "Character creation: choose at most " + std::to_string(rt_.TraitMax) + " traits."); return false; }
    for (int i = 0; i < 7; i++) c.special[i] = m.special[i];
    c.tagMask = m.tagMask;
    c.traitMask = m.traitMask;
    c.created = true;
    c.unspentSpecial = 0;
    c.level = 1;
    c.xp = 0;
    c.skillRanks.clear();
    c.skillPoints = rt_.StartSkillPoints;
    RecalcDerived(c);
    c.hp = c.maxHp;
    c.ap = -1;
    // the session now enters the world: placement (saved position of a pre-existing legacy character or the start entry), MapInfo,
    // Stats, CharSheet, Inventory, on_login, first save
    nlohmann::json legacy;
    auto it = creatingSaved_.find(c.id);
    if (it != creatingSaved_.end()) { legacy = std::move(it->second); creatingSaved_.erase(it); }
    const CritterId id = c.id;
    EnterWorld(c, legacy.is_object() ? &legacy : nullptr);
    Critter* cc = world_.GetCritter(id);
    if (!cc || cc->mapId == 0) return true;   // vetoed / failed: the session is gone
    SendCritterState(*cc);
    SendWaitState(*cc);
    Tell(*cc, "Character created. Welcome to the wasteland!");
    SavePlayer(*cc);
    return true;
}

// ------------------------------------------------------------------ skills

bool Server::SpendSkillPoints(Critter& c, int skill, int points) {
    if (!c.created || skill < 0 || skill >= kSkillCount || points < 1) return false;
    int& rank = c.skillRanks[kSkillNames[skill]];
    points = std::min(points, c.skillPoints);
    points = std::min(points, rt_.SkillMax - SkillValue(c, skill));   // never above the cap
    if (points < 1) return false;
    rank += points;
    c.skillPoints -= points;
    SendCharSheet(c);
    return true;
}

bool Server::UseSkill(Critter& c, int skill, int kind, uint32_t target) {
    if (!c.created || c.dead || skill < 0 || skill >= kSkillCount) return false;
    const int cost = rt_.RtApCostUseSkill * 100;   // whole AP like FOnline (the other RtApCost* keys are 1/100 AP)
    const uint8_t action = static_cast<uint8_t>(GMsgChar::C2S_UseSkill);
    if (CheckAction(c, action, cost) != gnet::ActionResult::Ok) return false;
    CommitAction(c, action, cost, rt_.Breaktime);
    if (!scripting_) return true;
    // hook: use_skill(cr, skill_name, skill_value, kind, target_id) -> true when handled
    const bool handled = scripting_->CallHandled("use_skill", &c, std::string(kSkillNames[skill]), SkillValue(c, skill), kind, static_cast<int>(target));
    if (!handled) Tell(c, std::string("You cannot use ") + kSkillNames[skill] + " like that.");
    return true;
}

// ------------------------------------------------------------------ experience

int Server::KillXp(const Critter& victim) const {
    if (const Proto* p = victim.protoPid >= 0 ? data_.GetProto(static_cast<uint16_t>(victim.protoPid)) : nullptr) {
        auto it = rt_.XpByProto.find(p->key);
        if (it != rt_.XpByProto.end()) return it->second;
    }
    return std::max(0, rt_.KillXpBase + rt_.KillXpPerHp * victim.maxHp);
}

void Server::AddXp(Critter& c, int amount) {
    if (!c.IsPlayer() || !c.created || amount <= 0) return;
    c.xp += amount;
    Tell(c, "You gain " + std::to_string(amount) + " experience points.");
    bool leveled = false;
    while (c.level < rt_.MaxLevel && c.xp >= XpForLevel(c.level + 1)) {
        const int oldMax = c.maxHp;
        ++c.level;
        RecalcDerived(c);
        c.hp = std::min(c.maxHp, c.hp + (c.maxHp - oldMax));
        const int sp = SkillRate(c);
        c.skillPoints += sp;
        std::string msg = "Level up! You are now level " + std::to_string(c.level) + ": +" + std::to_string(sp) + " skill points";
        if (rt_.PerkRate > 0 && c.level % rt_.PerkRate == 0) { ++c.perkPoints; msg += ", +1 perk point (perks are not implemented yet)"; }
        Tell(c, msg + ".");
        leveled = true;
        if (scripting_) scripting_->Call("on_levelup", &c, c.level);
    }
    if (leveled) { SendStats(c); SendCritterState(c); }
    SendCharSheet(c);
}

}  // namespace f3
