-- F3Online real-time tunables. FOnline names and defaults mirror F:\_work\sdk\Server\scripts\config.fos (without the "__" prefix).
-- Hot-reloadable: the server re-reads this file when it changes (checked about once per second) and on the console command `reload`.
-- A missing key keeps the built-in default. Times are milliseconds, "1 AP == 100" (AP-cost values are in 1/100 AP, maximum 255 in FOnline).
config = {
    -- ---- from config.fos
    CritterIdleTick           = 1000,  -- npc think interval for scripts that use cr:think_due(config.CritterIdleTick)
    Breaktime                 = 500,   -- wait after item / use / pick / drop actions (cr:wait(Breaktime))
    TimeoutTransfer           = 0,     -- extra lock after a map transfer, added to TransferLock
    TimeoutBattle             = 500,   -- the battle flag lasts this long after a combat action (attacker and target)
    ApRegeneration            = 7000,  -- ms for a full AP regeneration (0 -> max AP)
    RtApCostCritterWalk       = 0,     -- per cell crossed, 1/100 AP
    RtApCostCritterRun        = 0,
    RtApCostMoveItemContainer = 0,     -- Take from a container
    RtApCostMoveItemInventory = 0,     -- Equip / unequip
    RtApCostPickItem          = 0,
    RtApCostDropItem          = 0,
    RtApCostReloadWeapon      = 0,     -- (no reload action yet)
    RtApCostPickCritter       = 0,     -- (no pick-critter action yet)
    RtApCostUseItem           = 0,     -- doors, containers, usables
    RtApCostUseSkill          = 2,     -- (no skills yet)
    RtAlwaysRun               = true,  -- players run by default
    RunOnCombat               = true,  -- false: no running while the battle flag is set

    -- ---- F3Online additions
    ApBase                    = 5,     -- max AP = ApBase + Agility / ApAgilityDiv
    ApAgilityDiv              = 2,
    WalkSpeed                 = 3.0,   -- cells per second (1 cell = 0.5 world unit)
    RunSpeed                  = 6.0,
    MoveClearance             = 0.3,   -- cells kept from walls by smoothed (string pulled) path segments
    AttackMinMs               = 500,   -- attack duration = clamp(weapon cost word * AttackMsPerCost, min, max)
    AttackMaxMs               = 4000,
    AttackMsPerCost           = 100,
    RtApCostAttackPerCost     = 10,    -- attack AP (1/100) = weapon cost word * this, at least RtApCostAttackMin
    RtApCostAttackMin         = 50,
    TransferLock              = 3000,  -- ms during which a transition region cannot fire again after a map change
}
