# Van Buren decompilation: findings useful for F3Online

Paths relative to `_work/vbdecomp-main/repos/openvb/` (`src/game` unless stated; `engine:` = `openje/src/`).

## 1. Corrections to our earlier guesses

| Area | Our docs said | Decompilation says |
|---|---|---|
| GENT last two words (`docs/formats/maps.md`) | 15 / 1 / 100 unknown | `maxHealth`, `currentHealth` (critters 15, Default_PC 100). Not derived from SPECIAL |
| GCRE "derived overrides" | derived stats | the 13 **skills** as (index,value) pairs, then trait/perk pairs (82 entries), selected traits, tag skills, `value274` |
| GWAM "word6 = AP cost" | AP | it is `range`; AP is the later `actionPoints` field; word0 = animation id (1 = burst) |
| `Distance`, `GetFacing`, `GetDirectionToEntity` | float bits | **truncated ints** (`Distance` compared with `jsgeq 25`) |
| `A_MoveToPosition`, `A_Wander`, `FindObjects` args | x,y,z cells | `float[3]` vectors by reference, world units |
| `HasDied` | alias of IsDead | counter global `_DEAD_<name>` |
| `HasItem` | bool | count |
| `StartStore` | shop | no-op |
| `floatdiv` / `floatround` | /0 -> 0, `std::round` | /0 -> inf; modes 1 floor, 2 ceil, 3 trunc, default floor(x+.5) |
| XP / level up | planned | **does not exist in the demo** (`GetLevel` = `baseValues[0]`) |
| Dialogue `RegisterReply` | list order | keyed by replyID (first wins, shown in id order); textID 0 -> STF 184 |
| `GetTeamAttitude` | 0 default | equal ids -> 1 |

## 2. Combat, stats

### 2.1 SPECIAL and derived stats (`GameCreatureStats.cpp`, `GetDerivedAttribute`)
Order ST,PE,EN,CH,IN,AG,LK (idx 0..6). Chargen: 40 points, exactly 3 tag skills, max 2 of 16 traits (`CharacterGenerationStats.cpp`).
Derived: 0 maxAP = AG+20; 1 carry = 25*(ST+1); 2 sequence = 2*PE; 3 crit% = LK; 4 evasion = `value7FC`; 5 heal rate = EN; 6 melee bonus = ST; 7 perception range = 3*PE+40; 8 perk rate 3; 9..11 resist % (Bio, Flash, Rad) = 0; 12 skill rate = 2*IN+5; 13 throw range = 3*ST; 14 AP tick period ms.

### 2.2 Skills (`GameCreature::GetTotalSkill`)
13 skills; total = base + bonus (+20 if tagged). Bonus: 0,1 = 50; 2 = EN+2ST; 3,4 = 3CH; 5 = IN+2CH; 6,7 = PE+2IN; 8 = EN+2PE; 9 = 3IN; 10,11 = AG+2PE; 12 = PE+2AG.
Probable names (inferred): Firearms, Melee, Unarmed, Barter, Persuasion, Deception, Mechanics, Medic, Outdoorsman, Science, Security, Sneak, Steal.

### 2.3 Action points
`actionBudget` regenerates in `GameCreature::UpdateClient`: rate = maxAP/6000 per ms (full bar in 6 s, matching sys.ini Seconds Per Round = 6); actions cost `mode->actionPoints`; a called shot adds +3 AP. `engine:Combat.cpp`: mode 0 real time, mode 1 turn based (user setting), actors in state 5.

### 2.4 Attack (`GameActionAttack*.cpp`, `GameWeapon*.cpp`, `GameEffectDamage.cpp`)
Flow: ResolveAttack -> auto-move if out of range or no LOS -> PrepareAttack -> BuildSubAttacks -> RollHit / ChooseHitPart / NotifyAttacked (script `OnAttacked`) / CalculateAndQueueDamage / PublishPendingDamage; damage applied by `GameEffectDamage` (it also causes death).
* Called-part penalties {10,40,40,40,40,60,120,60}; part 9 = uncalled (0). Chance clamped 0..95, roll `rand(0,100)` inclusive, hit if `chance>=roll`.
* Melee/unarmed: mode accuracy + total skill (1 or 2) - (evasion + penalty). Ranged: skill 0; thrown uses skill 1 and range 3*ST. Distance penalty multiplier = 200/((range-10)/5), penalty = max(0, -2 - trunc(dist*-0.2)).
* Damage = base + rand(min,max) + ammo modifiers (+ST bonus for damage type 4). Defense = resistance (parts 0-4,7) or threshold (parts 5,6); if defense>0 subtract ammo armor adjustment; damage -= half + rand(0,half), floor 0.
* Burst (animation==1): `shots` subattacks with yaw spread 0,0,+5,-5,+10,-10 deg. Hit part from `uncalledChances[8]` (sum 100).
* Damage types: 0 Ballistic, 1 Biological, 2 Electrical, 3 EMP, 4 Normal, 5 Heat. Mode `kind`: thrown 2; weaponClass 1..5 ranged; 6..8 melee; 9..10 unarmed.
* Death: `IsDead` = state 7 or HP<=0. SetHP/AddHP do not kill.
* Movement: 8-neighbour A*, speed `value94` (default 10) world units/s.

## 3. Data formats (cross-check with `docs/formats/*`)
* GENT v0 six strrefs, v1 + maxHealth, currentHealth.
* GCRE ('ERCG') v0: special[7], baseValues[7] ([0] level, [3] race, [4] age, [5] gender, [6] ethnicity), valueDC, skill pairs, trait pairs, selectedTraits, tagSkills, value274. v1 portrait, speed, material masks, 12 sockets. v2 attackModeCount (0 = failsafe punch kind 3, anim 27, range 3, AP 3, dmg 1-2). v3 uncalledChances[8], baseResistances[6], baseThresholds[6]. v4 starting items. v5 SubRace.
* GWAM v5 record: animation, damageType, shots, values18[3], range, values28[2], minDamage, maxDamage, flag38; v1 value0C, calledShot, actionPoints, consumed, thrown; v2 pose; v3 nameRef; v4 effectName; v5 flag64/65.
* GIWP: weaponClass, caliber/reload per slot, ammo loaded/capacity, attackModeCount, v1 modifiers, then GWAM chunks. GIAM: caliber, min add, max add, armor adj, accuracy adj (+ v2 range add). GIAR: 6 values + one. GITM: item type, sockets, held model. GOBJ: usable type + 6 defenses.
* GDOR: open, locked, keyResource; v1 lockDC, randomRange. GCON same (no lock logic in `Operate`). Door: locked opens only with the key; STF 1358 unlocked, 1299 locked.
* Chunk FourCCs (EME2, ESTR, EPTH, EMTR, EMNP, ETTR, EMSD, EMFG, ECAM) consistent with our docs.

## 4. Script hooks (engine -> script)
OnCreate(time), OnLoad(time), OnUpdate(time) (drains `QueueFunction` queue first), OnCutscene*, OnDestroy, OnDeath, OnMessage(from,msg), OnAttacked(attacker,victim,hit,calledShot,0) (owner, victim, then non-dead entities within 30 units; nonzero result -> EnterCombat), OnDamage(ts,creator,damage,type), OnOpen/OnClose(actor) (nonzero suppresses default), OnEnter/OnLeave(entity,time) on triggers, UseSkill(actor,selector), ShowSkills(), GetUsableSkills(int[26]), GetInfo, Run (encyclopedia/worldmap), OnStartDialogue (.scra). Public variables are preloaded from EESTR/ESTR.
Extension map: `.dlg` = our `.scra`, `.amx` = our `.scrb`.

## 5. Dialogue engine (`engine:network/DialogueManager*.cpp`)
Dialogue keyed by byte id; speaker, listener, current node name (uppercased, previous kept), state machine 0..4, revision byte, per-party-member text and reply maps. `RegisterReply(id, textID, action$, next$)`; `SetNodeTextFunction` bumps revision and clears maps; `"EndDialogue"` ends; `SelectReply` runs the action public then sets the next function; jump links load another .dlg (failure fatal). Reply builder runs once per party member per update.

## 6. Other
* Teams/squads: ids 1..1999 preallocated, new from 2000; attitudes directed; AI lives entirely in `.scrb` (Template_MOB etc.).
* Journal: global task status (1 open, 2 closed) and (task, entry) pairs, deduped.
* Stores: `CStore::Read` = 3 dwords, count, items (str + 5 dwords, third read twice: original bug); no UI is invoked.
* Calendar: Gregorian, per-entity offset `value29C`; time scale default 1. RNG: LCG described in natives doc.
* Not covered/not reviewed: renderer, audio, GUI.
