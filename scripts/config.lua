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
    WalkSpeed                 = 9.0,   -- cells per second (1 cell = 0.5 world unit): 4.5 units/s, the stride of the Walk clip
    RunSpeed                  = 18.0,  -- 9 units/s, the stride of the Run clip
    MoveClearance             = 0.3,   -- cells kept from walls by smoothed (string pulled) path segments
    AttackMinMs               = 400,   -- attack duration = clamp(weapon cost word * AttackMsPerCost, min, max): 9mm 800 ms,
    AttackMaxMs               = 1000,  -- heavy weapons are capped (a long action must never lock walking for seconds)
    AttackMsPerCost           = 40,
    RtApCostAttackPerCost     = 5,     -- placeholder
    -- attack AP (1/100) = weapon cost word * RtApCostAttackPerCost, at least RtApCostAttackMin (9mm: 1 AP)
    RtApCostAttackMin         = 50,
    CarryBase                 = 25,    -- carry limit = CarryBase + CarryPerStrength * Strength
    CarryPerStrength          = 25,
    WeightWeapon              = 4,     -- default unit weight by item class (the demo data carries no weights)
    WeightArmor               = 8,
    WeightAmmo                = 1,
    WeightItem                = 1,
    ItemWeights               = {      -- per prototype overrides: ["Weapons9mm_Autoloader.WEA"] = 3,
    },
    TransferLock              = 3000,  -- ms during which a transition region cannot fire again after a map change
}
