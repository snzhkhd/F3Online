#pragma once
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
};

}  // namespace f3
