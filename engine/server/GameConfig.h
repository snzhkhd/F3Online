#pragma once
#include <map>
#include <string>
// Real-time tunables (FOnline config.fos names). Filled from scripts/config.lua (see Scripting::LoadConfig), hot-reloadable.
namespace f3 {

struct GameConfig {
    // from config.fos
    int CritterIdleTick = 1000;
    int Breaktime = 500;
    int TimeoutTransfer = 0;
    int TimeoutBattle = 500;
    int ApRegeneration = 7000;
    int RtApCostCritterWalk = 0, RtApCostCritterRun = 0;
    int RtApCostMoveItemContainer = 0, RtApCostMoveItemInventory = 0;
    int RtApCostPickItem = 0, RtApCostDropItem = 0, RtApCostReloadWeapon = 0, RtApCostPickCritter = 0;
    int RtApCostUseItem = 0, RtApCostUseSkill = 2;
    bool RtAlwaysRun = true;
    bool RunOnCombat = true;
    // F3Online additions
    int ApBase = 5, ApAgilityDiv = 2;
    double WalkSpeed = 9.0, RunSpeed = 18.0; // cells per second (4.5 / 9 world units/s = the stride of the Walk / Run clips)
    double MoveClearance = 0.3;              // cells
    int AttackMinMs = 400, AttackMaxMs = 1000, AttackMsPerCost = 40;
    int RtApCostAttackPerCost = 5, RtApCostAttackMin = 50;
    int TransferLock = 3000;
    int AggroAlertRadius = 8;   // cells: same-team NPCs this close to a shot-at NPC join the fight (0 = off)
    bool ForceMiss = false;     // dev/test: every attack misses (hit roll skipped)
    // carry weight (server authoritative): limit = CarryBase + CarryPerStrength * ST; unit weights by item class
    // (the demo data has no weights), ItemWeights overrides single prototypes by key ("Weapons9mm_Autoloader.WEA")
    int CarryBase = 25, CarryPerStrength = 25;
    int WeightWeapon = 4, WeightArmor = 8, WeightAmmo = 1, WeightItem = 1;
    std::map<std::string, int> ItemWeights;
    // character system (Character.cpp)
    bool RequireCharacterCreation = true;   // new account characters must allocate SPECIAL + tag skills before playing
    int SpecialBase = 5, CharPoints = 5, SpecialMin = 1, SpecialMax = 10;   // creation: 7 * SpecialBase + CharPoints = 40 points in total
    int TagCount = 3, TagBonus = 20;
    int TraitMax = 2;          // optional traits at creation (the original allows two of sixteen); stored, effects not implemented
    int SkillUseRange = 5;     // cells: UseSkill on a critter / object farther away is refused without spending AP
    int SkillMax = 100, StartSkillPoints = 10;
    int SkillRateBase = 5, SkillRatePerInt = 2;   // skill points per level = base + per * Intelligence
    int XpBase = 1000, MaxLevel = 12, PerkRate = 3;   // xp for level L = XpBase * L * (L - 1) / 2
    int HpBase = 15, HpPerLevelBase = 2;               // maxHp = HpBase + S + 2E + (level - 1) * (HpPerLevelBase + E / 2)
    // fog of war / visibility: a player sees (and is sent) critters, items and speech within ViewBase + ViewPerPerception * Perception cells
    // (Euclidean, 1 cell = 0.5 world unit); the same radius goes to the client in MapInfo for the fog edge. Default PE 5 -> 48 cells.
    int ViewBase = 28, ViewPerPerception = 4;
    // doors / containers (Doors.cpp): lockpicking = Security skill against the lock DC
    int LockDefaultDC = 0;                    // DC of locks whose data has none (the original GDOR / GCON lockDC is 0 everywhere)
    int LockpickMinChance = 5, LockpickMaxChance = 95;   // percent bounds of skill - DC
    int LockpickNeedsKit = 0;                 // 1: a Security Kit item is required to pick (original skill text: "requires lockpicks")
    int LockpickXp = 10;                      // xp for a picked lock
    int ForceLockRoll = -1;                   // dev/test: 1 = every pick succeeds, 0 = every pick fails, -1 = random
    int PlayerAutoOpenDoors = 0;              // 1: players open operable doors by walking into them; 0 (original): only NPCs do, a player stops at the door and uses it
    int DoorOpenRange = 3;                    // cells from the door (nearest doorway cell) to operate it / open a container
    int KillXpBase = 30, KillXpPerHp = 8;              // xp for killing an NPC unless XpByProto has its prototype
    std::map<std::string, int> XpByProto;
};

}  // namespace f3
