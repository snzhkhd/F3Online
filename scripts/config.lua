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
    RunSpeed                  = 20.0,  -- 9 units/s, the stride of the Run clip		18.0
    MoveClearance             = 0.3,   -- cells kept from walls by smoothed (string pulled) path segments
    AttackMinMs               = 400,   -- attack duration = clamp(weapon cost word * AttackMsPerCost, min, max): 9mm 800 ms,
    AttackMaxMs               = 1000,  -- heavy weapons are capped (a long action must never lock walking for seconds)
    AttackMsPerCost           = 40,
    RtApCostAttackPerCost     = 5,     -- placeholder
    -- attack AP (1/100) = weapon cost word * RtApCostAttackPerCost, at least RtApCostAttackMin (9mm: 1 AP)
    RtApCostAttackMin         = 50,
    AggroAlertRadius          = 8,     -- cells: same-team NPCs near a shot-at NPC join the fight (0 = off)
    ForceMiss                 = false, -- dev/test: every attack misses (the /forcemiss command toggles it at run time)
    CarryBase                 = 25,    -- carry limit = CarryBase + CarryPerStrength * Strength
    CarryPerStrength          = 25,
    WeightWeapon              = 4,     -- default unit weight by item class (the demo data carries no weights)
    WeightArmor               = 8,
    WeightAmmo                = 1,
    WeightItem                = 1,
    ItemWeights               = {      -- per prototype overrides: ["Weapons9mm_Autoloader.WEA"] = 3,
    },
    -- ---- character system (docs/server-data.md section 11). Placeholders: the demo data carries no formulas.
    RequireCharacterCreation  = true,  -- new account characters allocate SPECIAL + tag skills first (guests / dev kit characters are Default_PC)
    SpecialBase               = 5,     -- creation: every SPECIAL starts at SpecialBase ...
    CharPoints                = 5,     -- ... plus CharPoints free points (7 * 5 + 5 = 40 in total), each value SpecialMin..SpecialMax
    SpecialMin                = 1,
    SpecialMax                = 10,
    TagCount                  = 3,     -- tag skills (the original UI says "maximum of three tag skills")
    TraitMax                  = 2,     -- optional traits chosen at creation (original: two of sixteen; stored only, no effects yet)
    -- doors / containers: Security (lockpick) chance = clamp(skill - lock DC, min, max) percent; the original lock DC data is 0 everywhere
    LockDefaultDC             = 0,     -- DC of locks that have none
    LockpickMinChance         = 5,
    LockpickMaxChance         = 95,
    LockpickNeedsKit          = 0,     -- 1: a Security Kit item is required (original skill text "requires lockpicks")
    LockpickXp                = 10,    -- xp for a picked lock
    DoorOpenRange             = 3,     -- cells (to the nearest doorway cell) from which a door / container can be operated
    SkillUseRange             = 5,     -- cells: skill use on a critter / object farther away is refused (no AP spent)
    TagBonus                  = 20,    -- added to a tag skill
    SkillMax                  = 100,
    StartSkillPoints          = 10,    -- unspent skill points of a fresh character
    SkillRateBase             = 5,     -- skill points per level = SkillRateBase + SkillRatePerInt * Intelligence
    SkillRatePerInt           = 2,
    XpBase                    = 1000,  -- xp needed for level L = XpBase * L * (L - 1) / 2
    MaxLevel                  = 12,
    PerkRate                  = 3,     -- a perk point every PerkRate levels (placeholder, perks are not implemented)
    HpBase                    = 15,    -- maxHp = HpBase + S + 2E + (level - 1) * (HpPerLevelBase + E / 2)
    HpPerLevelBase            = 2,
    KillXpBase                = 30,    -- xp for killing an NPC = KillXpBase + KillXpPerHp * its max HP, unless XpByProto lists it
    KillXpPerHp               = 8,
    XpByProto                 = {      -- ["Some_Critter.CRT"] = 250,
    },
    TransferLock              = 3000,  -- ms during which a transition region cannot fire again after a map change
}
