# Server on real game data

How `f3_server` runs on the original Van Buren demo data: the importer, the server-side file formats, the gameplay layer,
the protocol extension, the Lua API and the state of the original AMX scripts. Companion to `docs/formats/maps.md`
(file formats of the original data) and `docs/architecture.md`.

## 1. Pipeline

```
grptool unpack  ->  extracted/            (gitignored)
f3_dataimport   ->  gamedata/server/      (gitignored, never commit)
f3_server       ->  loads gamedata/server automatically (or --data DIR, --no-data)
```

```
cmake --preset vs2022 && cmake --build build/vs2022 --config Release
build\vs2022\bin\f3_dataimport.exe --extracted extracted --stf F:\_work\F3_Demo\English.stf --out gamedata\server
build\vs2022\bin\f3_server.exe            # finds gamedata\server next to the binary / in a parent of the cwd
build\vs2022\bin\f3_gametest.exe --port 4040   # end-to-end test against a running data-driven server
```
`English.stf` is not extracted by grptool as a file of `extracted/`; pass the original `F:\_work\F3_Demo\English.stf`.
Without gamedata the server falls back to the built-in Lua test map (smoke test part 1 forces this with `--no-data`).
The importer takes 1 s and writes about 13 MB. It `#include`s `tools/mapinfo/main.cpp` (all parsers are the strict ones that
verified 119/119 scripts, 285/285 templates, 42 maps), so there is a single implementation of every format.

### Output of `f3_dataimport`

| file | content |
|---|---|
| `manifest.json` | `maps[] {name,width,height,entities,has_emap}`, `start_map`, `start_entry` (from `Engine/sys.ini`) |
| `maps/<name>.json` | `name, tre_base, width, height (cells), cells_per_unit=2, walk, entry_points[{id,slots[{x,z,yaw}]}], regions[{kind,type,pts[[x,z]],y,script,target_map,target_entry,band_id}], paths[{name,pts}], entities[...]` in world units |
| `maps/<name>.walk` | `"WALK" u32 w u32 h` + `w*h` raw mapdata class bytes (`0x2f` = blocked) |
| `protos.json` | one object per `.een` template, key `"<template>.<TYPE>"` (285 entries) |
| `strings.tsv` | `English.stf` as `id<TAB>text` (1-based ids, as referenced by GENT) |
| `scripts/` | all `.scra/.scrb/.ini` copied verbatim (the AMX host and `globals.ini` read them from here) |

Map names are the emap file stem (`00_03_Tutorial_Junktown`), which is what `ETTR` transitions and `sys.ini` use. Tre-only maps
(no emap: `0301_Mining_Cave_2`, ...) get their tre base name; tre-base aliases of emap maps (`03_Tutorial_Junktown`) are not emitted twice.
39 maps are written; 3 test emaps without a grid are skipped (`Test_Building`, `Test_Junktown_fences_metas`, `Weapon_Ammo_Item_Test1`).
Entities: `{name (instance), proto, type, x,y,z, yaw, team, script, dialog, skin, effects[], inventory[]}`.

Prototype fields (`protos.json`): `class` (critter/weapon/armor/ammo/item/door/container/usable), `model, icon, icon_ground, name, desc, name2`
(English.stf via GENT ids; `name2` is the critter "type" string), `team, script, dialog, skin, inventory[]` (EEOV defaults), and
* critters: `critter{special[7], stat_words[8], derived_overrides[[idx,val]], portrait, uncalled_chances[8], equipment[], attack_modes[], sockets}`
  and `static` (true for `Useable_*` and `DS_*` critter-class props: barrels, windows);
* weapons: `item{class, subclass, raw_hex}`, `weapon{raw_prefix[15 words], weapon_class, caliber, range, dmg_min, dmg_max, attack_modes[]}`;
* armor: `armor.values[7]`; ammo: `ammo{caliber, dmg_min, dmg_max, mod, raw[]}`; doors/containers/usables: `use_kind`, `gdor_hex`, `gcon_hex`.

### What is decoded (and how sure we are)

Decoded from value patterns across all templates (not from code): this is the bit that needs F3.exe confirmation.

| field | reading | confidence |
|---|---|---|
| GIAM word0 / GIWP word1 | caliber id, equal in ammo and weapon (`.44`=8, `.45`=9, `9mm`=12, `.50`=10, `.22`=3, `.223`=2) | high |
| GIAM words 1,2 | ammo damage min,max (`.44` 15-25, `9mm` 5-12, `.45` 10-18) | medium |
| GIWP word2 | range (15..20) | medium (units unknown, treated as world units) |
| GIWP words 6,7 | weapon base damage min,max as 16.16 fixed point (revolver 6.0, minigun 54) | medium |
| GIWP word0 | weapon class (1 pistol, 4 heavy/energy, 6 melee) | low |
| GWAM word0, word6 | attack mode type (2 aimed single, 1 burst, 6/8/9 melee) and an AP-like cost (5..50) | low |
| GWAM words 9,10 | melee/unarmed damage min,max (animal data matches expectations) | medium |
| GIAR 7 words | per-damage-type armor values (leather `4,4,4,0,8,4,0`, power armor `32,24,12,0,36,12,0`); the server uses word0/2 as threshold and word1/4 as AC | low |
| GCRE special[7] | S P E C I A L (order assumed) | high for values, order assumed |
| GCRE `stat_words`, `derived_overrides` | not used (hit points are derived with the classic formula instead) | unknown |
| weights, values | not found in the demo templates; defaults are used (weapon 4, armor 8, other 1) | n/a |

Unfinished weapons exist: 35 of 51 weapons have an all-zero GIWP (62 bytes) and no attack modes (grenades, knives, bats...). They
work as melee items with a default damage of 2-6 plus the Strength bonus.

## 2. Server data model

* `DataStore` (`engine/server/DataStore.*`): loads manifest, protos, strings and `globals.ini` (596 variables, copied to the runtime
  globals table used by `get_global/set_global` and the AMX natives). Prototype ids (`pid`, u16) are the index in the **key-sorted**
  order of `protos.json` (`nlohmann::json` objects are sorted), so a client that reads the same `.een` set can reproduce them;
  `ItemAdd.pid` and every `InvEntry.pid` are such ids. A pid lookup by key is case-insensitive.
* `Map` (`World.h`): `blocked[]` = `CellSolid(walk)` (walls, prop footprints, fences; `engine/common/gamedata/CellClass.h`) at 2 cells per world unit; the grid starts at the map origin `originX/originZ` (map json `origin_x`, `origin_z`, `origin_verified`; tre root bbox min), cell = `(floor(2(x-originX)), floor(2(z-originZ)))`, `entries[]` (id -> slots),
  `regions[]` (polygon in cell units, ray-casting point test), `paths[]`, `items` (ground items, doors, containers, usables).
  All 39 maps are loaded at start (13 MB of grids); only maps with players are simulated (NPC scripts, movement, combat, AMX).
* Pathfinding: A* on the real grid (8-connected, no corner cutting), per-thread scratch buffers with generation stamps so a search on a
  644x644 map does not pay O(cells) for clearing; limit 120000 expanded nodes.
* Visibility radius is `Server::kViewCells = 48` cells (24 world units); `kViewRadius` of `protocol.h` is no longer used by the server.

### Entity spawning (`Game.cpp: LoadWorldData`)

* `CRT` -> NPC critter (`name` = English.stf name or the type string, SPECIAL from the template, `team` from the instance EEOV, template
  `equipment` worn, instance (or template) `inventory` carried; first weapon auto-equipped). Props flagged `static` are placed exactly
  on their (usually blocked) cell and never move. If the cell is blocked for a normal critter the nearest free cell within 6 is used.
* `WEA/ARM/AMO/ITM` -> ground object. `CON` -> container with contents from EEOV + template inventory (stackables merged).
  `DOR` -> door; its footprint is the nearest 8-connected `0x1b` doorway component (radius 12, cap 800 cells); only a locked closed door blocks it.
  `USE` -> usable object (reported to the Lua hook `on_use`).
* Instances whose template does not resolve (11 stale references in the test maps) are skipped with a count in the log.
* Entry points: login uses the start map and `start_entry` (`sys.ini` says 0, Junktown has ids 1 and 2, so an unknown id falls back
  to the first entry point); the first free slot of the entry point is used, else the nearest free cell.
* Transitions: players that step into an `EMTR` kind-0 region (`ETTR` target map + entry id) are moved to that map/entry
  (3 s re-trigger lock). Script-trigger regions (kind 1) call the Lua hooks `on_region_enter/leave(c, script_name)`.

## 3. Gameplay (`engine/server/Game.cpp`)

All numbers below are placeholders unless noted; formulas of the original game are not known.

* Stats: `maxHp = 15 + S + 2E`, carry weight `25 + 25 S`, `AC = A + armor AC`, sequence `2P`, melee bonus `max(0, S-5)`. Players use `Default_PC.CRT`
  (all SPECIAL 6, 33 HP), team 1. A new character owns and wears NOTHING (base body). The dev starter kit (Lua `on_login`: 9mm autoloader in hand 1, 30 x 9mm, leather outfit worn, .44 revolver) is only given when the server runs with `--dev-kit` (`f3_gametest` expects it; `f3_gametest --expect-empty` checks the default).
* Inventory: stackable ammo/misc, one equipped weapon and one armor, carry weight limit for players, drop to ground, pick up (the server walks
  the player next to the item first, then picks up; pending actions are cancelled by a new move), take from containers.
* Doors: `Use` toggles; unlocked doors also open by proximity (a critter within 5 cells) and auto-close after 3 s with nobody near (`TickDoors`, every 5 ticks).
  A locked door (`doorLocked` from GDOR byte 1, or `LockDoor`/`UnlockDoor` from scripts, `IsLocked`) refuses to open with a message and blocks its footprint.
  `CellsSet` is sent to every player on the map; closing is refused while a critter stands in the doorway.
* Combat is **real time**, not the original 6 s rounds (sys.ini `Seconds Per Round = 6`): `Attack(target)` makes the critter approach until the
  weapon range (ranged: `range * 2` cells and line of sight on the grid; melee: adjacent) and fire once per cooldown. Action duration (see section 9) in seconds =
  `clamp(GWAM cost / 10, 0.5, 4)` (guess). Hit chance `45 + 4A (+2S melee) - AC - 2 * max(0, distance_units - P)` clamped 5..95; damage = ammo
  damage range (needs matching ammo in the inventory, one round consumed per shot; NPCs have infinite ammo) else weapon range, melee uses GWAM
  min/max + Strength bonus; critical on `Luck %` for double damage; armor reduces by threshold but never below a quarter of the roll.
  Attacked NPCs retaliate. Death: critter flagged dead (no tile blocking), NPCs drop their inventory on the ground and vanish after 90 s,
  players respawn at the map's first entry point after 5 s with full HP.
* NPC AI is Lua: `scripts/npc/hostile.lua` (team 2 attacks the nearest team-1 critter within 28 cells), `idle` for everything else.
  `pick_npc_type(npc)` in `main.lua` assigns the behaviour to data-spawned critters on their first tick.

## 4. Protocol extension (`engine/common/protocol_game.h`)

`protocol.h` is **unchanged**. The new header uses unused id ranges (client->server 20..39, server->client 120..139); old clients ignore unknown
server messages. `ItemAdd` now also carries doors, containers and usables; an `ObjectInfo` follows it for those.

| id | message | payload |
|---|---|---|
| C 20 | PickUp | `u32 groundItemId` |
| C 21 | Drop | `u32 invId` |
| C 22 | Equip | `u32 invId, u8 on` |
| C 23 | Use | `u32 objectId` (door toggle / container list / usable) |
| C 24 | Attack | `u32 critterId` (0 = stop) |
| C 25 | Take | `u32 containerId, u32 invId (0 = all)` |
| C 26 | Examine | `u32 id, u8 isCritter` (answered with a system chat line: name, stf description, hp) |
| C 27 | ReqState | resend Stats + Inventory |
| S 120 | Stats | `u32 critterId, u8 special[7], u8 level, i16 hp, i16 maxHp, u8 armorClass, u8 sequence, u8 meleeBonus, u16 carryWeight, u16 carried` |
| S 121 | Inventory | `u32 owner, u16 n, n x {u32 id, u16 pid, u16 count, u8 equipped, str name}` |
| S 122 | ObjectInfo | `u32 id, u8 kind (0 ground,1 door,2 container,3 usable), u8 open` |
| S 123 | CritterState | `u32 id, i16 hp, i16 maxHp, u8 dead` |
| S 124 | CombatEvent | `u32 attacker, u32 target, u8 result (0 miss,1 hit,2 crit,3 kill), i16 damage, i16 targetHp` |
| S 125 | Container | same list layout as Inventory, `owner` = container id |
| S 126 | CellsSet | `u16 n, n x {i16 x, i16 y, u8 blocked}` |

Existing messages: `MapInfo` now carries the real grid (up to 644x644 = 414k bytes, one ENet reliable fragmented packet) and the real map name;
map transitions resend `MapInfo` (the client must reset its critter/item state on it, as it already does for `teleport`).
Critter names are the English.stf names; positions are cells (2 per world unit), `MoveTo` is in cells.

## 5. Lua API additions

Hooks: `on_death(c, killer)`, `on_damage(c, attacker, amount)`, `on_pickup(c, obj_id, proto_key)`, `on_use(c, obj_id, proto_key)`,
`on_region_enter/leave(c, script)`, `pick_npc_type(npc)`.
Critter: `hp max_hp level team dead is_static target script dialog proto`, `special(1..7|name) set_special inventory() give(key,count) equip(invId,on)
pickup drop use_object attack(id) damage(n[,by]) heal(n) kill enter_map(name,entry) weight`.
Map: `objects()` (id,name,kind,x,y,proto,open,count), `entry(id)`, `region_count()`.
Globals: `data_loaded() map_count() map_names() proto_info(key) spawn_npc_proto(map,key,x,y) spawn_item_proto(map,key,x,y[,count]) get_global set_global`.
Chat dev commands (main.lua): `/stats /inv /maps /goto <map> [entry] /tp x y /spawn <proto> /heal`.

## 6. Original AMX scripts

**Inventory** (`mapinfo amx` over all 119 scripts: 36 `.scra` dialogues, 83 `.scrb` behaviours): 1478 public entries, 115 distinct natives.

| group | natives (scripts using them) |
|---|---|
| float module (Pawn standard) | `float floatadd floatsub floatmul floatdiv floatfract floatround floatcmp` (48-81) |
| self / state | `Myself`(105) `GetCurrentHP GetMaxHP AddHP IsDead HasDied GetGender GetRace SetValue`(~78) `GetPrimaryAttribute GetTotalSkill`(76/66) `Protagonist GetType` |
| messages | `SendMessage MessageArgCount MessageGetArg`(78) `SendMessageToTeam A_SendMessage` |
| globals / random / time | `GetGlobal SetGlobal RandomNumber GetTimeStamp SecondsPassed Hour Minute` |
| display | `DisplayStringRef FloatingDamageText FloatingSpeechText A_FloatingSpeechText` |
| world queries | `GetObject FindObjects FindEntity MatchEntity Distance PointDistance GetLOS GetLOSPoint GetPosition GetFacing GetDirectionToEntity IsEntityInArea GetMapID GetWayPoint* GetNavPoint* GetNearestPath IsUnderFOW` |
| teams / squads | `CreateTeam AddToTeam GetTeam GetTeamRank GetTeamAttitude SetTeamAttitude SetTeamRank CreateSquad AddToSquad GetSquadID GetSquadRank SetSquadRank GetSquadAttitude SetSquadAttitude FindTeamMembers` |
| actions / tasks | `A_Wait A_MoveToPosition A_MoveToEntity A_Attack A_JustAttack A_Wander A_PlayAnim A_Kill A_ReloadWeapon OpenTask CloseTask ClearActions IsIdle SetFacing SetFacingTarget` |
| doors / items / sound / vfx | `UnlockDoor LockDoor IsLocked HasItem GiveNewItem PartyHasItem IsItemEquipped EquipItem AttachSound RemoveSound AttachVEG RemoveVEG GetVEGID MapEffectSetActive MapEffectResetElapsedTime SetModel ActivateEntity DeactivateEntity DestroyEntity` |
| dialogue (`.scra` only) | `RegisterNodeText RegisterReply SetCurrentNodeName SetNodeTextFunction AddEntry PCSpeaker StartDialogue StartStore IsInDialogue IsInParty` |
| publics | `.scrb`: `OnCreate OnLoad OnUpdate OnMessage OnDamage OnDeath OnDestroy UseSkill GetUsableSkills ShowSkills` (+ `OnAttacked OnOpen OnClose OnEnter OnLeave OnStartDialogue`); `.scra`: nodes `N<k> NA NC NU NTA NTC L[AC]<i>_R<j>_N<k> R[ACL]<j>_N<k>` |

**Feasibility verdict: feasible, and implemented as a prototype.** All 119 files are AMX file version 6 / 32-bit cells / non-compact and use exactly the
standard Pawn 3.x opcode table (checked with a disassembler: zero invalid opcodes, all jump/call targets on instruction boundaries, all publics on `PROC`,
all `sysreq` indices < native count). compuphase's current `amx.c` rejects them (it needs file version 11), and the file versions in between differ
in loader details, so instead of patching a third-party library `engine/server/Amx.{h,cpp}` is a 450-line own interpreter (no dependency).
`AmxHost` binds natives by name, creates one instance (own data/heap/stack, 23 KB) per scripted entity, and runs `OnCreate`, `OnLoad` once and `OnUpdate`
once per second on maps with players, plus `OnDeath`. Verified: `Template_MOB.OnCreate/OnUpdate` execute with a plausible native call stream
(`Myself, GetTeam, CreateTeam, AddToTeam, GetPosition, GetFacing, floatsub..., SetValue(self,attr,1)...`), the scripts of Junktown and the vault run live
without VM errors. Console: `amx <script name> <public>` loads a script and traces every native call with arguments.

Implemented natives (about 46 bound): the float module, `Myself Protagonist RandomNumber GetGlobal SetGlobal GetCurrentHP GetMaxHP AddHP IsDead HasDied GetTeam
GetGender GetRace GetLOS Distance GetPosition GetFacing DestroyEntity GetTimeStamp SecondsPassed Hour Minute GetMapID CreateTeam` and accept-and-ignore stubs for
`AddToTeam SetTeamAttitude SetTeamRank CreateSquad AddToSquad SetSquadAttitude SetSquadRank ClearActions AttachSound RemoveSound SetValue UnlockDoor LockDoor`.
Everything else returns 0 and is logged once (`amx: unimplemented native X`).

Gaps of the AMX prototype: native **signatures are inferred** (there is no header; names come from the file, arities from tracing), so entity handles,
float vs int returns and by-reference arguments (`GetPosition(ent, float[3])`) are best guesses; the action queue (`A_*`, tasks), messages, teams, `FindObjects`,
`Get/SetValue` attributes, doors and **dialogue** are not implemented (the node publics execute, but `RegisterNodeText/RegisterReply/SetNodeTextFunction/AddEntry`
are unbound, so no dialogue UI can be built yet; `SetNodeTextFunction` receives a function reference). Scripts and Lua do not share state except
the globals table. Lua stays the primary layer for new content; AMX is only an optional compatibility runner.

## 7. Tests

`engine/tests/smoke_test.ps1` (also `ctest`): (1) `f3_server --no-data` + `f3_testclient` (unchanged protocol round trips on the Lua test map);
(2) if `gamedata/server` exists, `f3_server --data` + `f3_gametest` which checks on the real Junktown map: size 384x624 and a walkable
spawn from the entry point, 33 HP / S6 stats and the starter inventory with English names, walking over the real grid, pickup of a
spawned revolver, a hostile NPC attacking a player, player-vs-player combat (hit event, HP update), `/goto` + walking into the Junktown
transition region which loads the Vault (384x334), and opening a door.

## 8. Known gaps

* Real-time combat instead of 6 s rounds; hit/damage/AP numbers are placeholders; no skills, perks, traits, XP or levels, no called shots, no burst fire or area damage.
* Doors: the GDOR locked flag is a guess; Junktown doors have no `0x1b` footprint, so they never block; doors of the 8 stale-grid test maps use an unverified origin. Locks, keys, container/door
  scripts are not applied. No stealing, no usable-skill system (`UseSkill`, `GetUsableSkills`).
* No dialogue engine (see AMX gaps), no stores, no party/squad system, no weights or item values from the data.
* Map content is only what the emap holds: sound/effect emitters, waypoint paths (stored, unused), automap icons and `EMTR` kind-6 volumes are imported but ignored by the server;
  `mapdata` surface classes other than `0x2f` are all treated as walkable.
* The client does not know the protocol extension yet (it ignores the new messages); `protocol_game.h` is the contract.
* Entities are placed with yaw only mapped to the 8 square-grid directions; the grid is square 8-connected (not hex, not 1:1 with the original movement).

## 9. Real-time timing

The server models time the way FOnline does (reference: `sdk/Server/scripts/config.fos`, `Critter::IsFree/IsBusy`, `cr.Wait`, client `ApRegenerationTick`). It is **not** turn based and nothing is tied to grid steps.
Code: `engine/server/Timing.cpp` (clock, AP, gating), `Move.cpp` (continuous movement), `GameConfig.h` + `Scripting::ApplyConfig` (tunables), `scripts/config.lua`.

### Model

* **Clock**: `Server::NowMs()`, milliseconds since server start (+1000, never 0). Sent as `u32`; clients never sync clocks because every timed message also carries `nowMs`.
* **Wait / breaktime**: every critter has `waitEndMs`. The critter is *free* when `now >= waitEndMs`. Item actions (equip, drop, pick up, use door/container/usable, take from container) are rejected while busy and, when accepted, set `wait = Breaktime` (500 ms). Starting a walk is rejected while busy as well. An attack sets its own duration: `clamp(weapon cost word * AttackMsPerCost, AttackMinMs, AttackMaxMs)` (the 9mm autoloader has cost 20, so 2000 ms). A map transfer resets the wait (`player.Wait(0)` in the FOnline scripts). Actions that walk first (pickup/use/take) are gated at request time and again on arrival; if the critter is still busy on arrival they stay pending.
* **Rejection feedback**: every gated request is answered with `WaitState` (`action` = the request id, `result` = `Ok`/`Busy`/`NoAp`/`Invalid`/`Dead`); `WaitState` is also sent on login/`ReqState`/battle changes (`result = None`). Rejected requests are not queued.
* **AP**: stored in 1/100 AP (`AP_DIVIDER = 100`), max AP = `ApBase + Agility / ApAgilityDiv` (Default_PC: Agility 6 gives 8 AP = 800). Regeneration is linear, `ApRegeneration` ms for a full bar (FOnline: `ap += max * delta / ApRegeneration`), computed lazily on every query (`Server::SyncAp`); the client can mirror it from `WaitState`. Costs (1/100 AP): `RtApCostPickItem, DropItem, MoveItemInventory (equip), MoveItemContainer (take), UseItem (doors/containers), CritterWalk/Run (per cell crossed)`; attack = `max(RtApCostAttackMin, weapon cost word * RtApCostAttackPerCost)` (9mm: 200). An attack waits (does not fire) until enough AP have regenerated; other actions are rejected with `NoAp`. `RtApCostUseSkill, ReloadWeapon, PickCritter` are parsed but have no action behind them yet.
* **Battle flag**: attacker and target get `battleEndMs = now + TimeoutBattle` on every shot. While set, map-transition regions do not fire, and with `RunOnCombat = false` critters walk instead of run. `WaitState.battleEndMs` carries it to the client.
* **NPCs** use the same rules (combat goes through `TickCombat`: approach, wait until free and enough AP, shoot, wait). Lua direct calls (`cr:pickup/drop/equip/use_object`) are not gated; scripts decide and may call `cr:wait(ms)`.

### Movement

* Positions are float cell coordinates (cell (cx,cy) has its centre at (cx+0.5, cy+0.5); 1 cell = 0.5 world unit); `Critter::pos` is `floor()` of them. The 0.5-unit walk grid is used only for collision and A*.
* A move request is pathfound with the existing A* (8-connected, no corner cutting), then **string pulled** (`Move.cpp: Smooth`): from each point the farthest later point is chosen whose straight segment is walkable with `MoveClearance` (0.3 cells) on both sides and without squeezing through a diagonal corner; consecutive grid cells are always accepted. The result (typically 2-6 points) is sent as one `MovePath` with `startMs` and `speed` (cells/s: `WalkSpeed` 9.0, `RunSpeed` 18.0 = 4.5 / 9 world units per second, the stride of the Walk / Run clips; players run when `RtAlwaysRun`, NPCs run only while chasing a target).
* The server integrates the same polyline every tick in sub-steps of at most 0.5 cell and checks every cell it enters (blocked cells / closed doors, diagonal squeeze, other critters, AP per cell). On a failure the critter stops and viewers get `CritterStop(reason 1)`; at the end `CritterStop(reason 0)`. `MoveToF` allows a precise end point inside a walkable cell (validated: the last hop must stay on free cells, else the cell centre is used).
* Old clients: a client that does not send `ClientCaps(bit0)` keeps receiving the legacy `CritterMove`, one per cell crossed (`snap=1` for teleports goes to everyone and cancels any path). `LoginOk.moveTicks` is legacy and unused by the continuous model.

### Messages (`protocol_game.h`, block "REAL-TIME TIMING"); floats are IEEE-754 bit patterns in a little-endian u32

| id | message | payload |
|---|---|---|
| C 28 | ClientCaps | `u32 caps` (bit0 = continuous movement + WaitState); send once after `LoginOk` |
| C 29 | MoveToF | `f32 x, f32 y` (cells, centre = +0.5), `u8 run` |
| S 127 | MovePath | `u32 id, u32 nowMs, u32 startMs, f32 speed (cells/s), u8 run, u16 n, n x {f32 x, f32 y}` (pts[0] = position at startMs; age = nowMs - startMs) |
| S 128 | CritterStop | `u32 id, f32 x, f32 y, u8 dir, u8 reason` (0 arrived, 1 blocked/rejected/out of AP, 2 teleport, 3 resync after CritterAdd/caps) |
| S 129 | WaitState | `u32 nowMs, u32 waitEndMs, u16 ap, u16 maxAp, u32 apRegenMs, u32 battleEndMs, u8 action, u8 result` (result: 0 Ok, 1 Busy, 2 NoAp, 3 Invalid, 4 Dead, 255 None) |

A rejected walk request while the critter is moving re-sends the remaining `MovePath` (the old path stays authoritative); while standing it sends `CritterStop(1)`.

### Config (`scripts/config.lua`, Lua table `config`, hot reloaded when the file changes (about 1 s poll) and on console `reload`)

FOnline names/defaults from config.fos: `CritterIdleTick 1000, Breaktime 500, TimeoutTransfer 0, TimeoutBattle 500, ApRegeneration 7000, RtApCostCritterWalk 0, RtApCostCritterRun 0, RtApCostMoveItemContainer 0, RtApCostMoveItemInventory 0, RtApCostPickItem 0, RtApCostDropItem 0, RtApCostReloadWeapon 0, RtApCostPickCritter 0, RtApCostUseItem 0, RtApCostUseSkill 2, RtAlwaysRun true, RunOnCombat true`.
F3Online additions: `ApBase 5, ApAgilityDiv 2, WalkSpeed 9.0, RunSpeed 18.0, MoveClearance 0.3, AttackMinMs 400, AttackMaxMs 1000, AttackMsPerCost 40, RtApCostAttackPerCost 5, RtApCostAttackMin 50, TransferLock 3000`. A broken edit keeps the previous values. The TbApCost* keys of config.fos are not used (no turn based mode).

### Lua API

Critter: `wait(ms) is_free() is_busy() wait_left() ap() max_ap() set_ap(v) use_ap(cost) in_battle() set_battle() think_due(ms) fx fy`, `move_to(x,y[,run])`, `is_moving()` (true while a path is active). Globals: `now_ms()`, `config` (the table). The `/ap` chat command prints the state.
`hostile.lua` throttles its target search with `npc:think_due(config.CritterIdleTick)`.

### Tests

`f3_gametest` (bot A announces `ClientCaps`, bot B stays legacy) additionally asserts: initial `WaitState` (800/800 AP, free), `MovePath` segments all on walkable cells and smoothed, stop at the exact cell centre, `MoveToF` exact end point and walk speed, legacy per-cell `CritterMove` for B and B's `MovePath` seen by A at run speed, attack shots at least one action duration apart with AP spent and a 2000 ms wait, AP regenerated server-side after 2 s, a second `Equip` within the breaktime rejected as `Busy`, a move request while busy rejected, accepted again after 500 ms.

### Gaps

No reload/skill/pick-critter actions (their `RtApCost*` keys are inert); no queueing of rejected actions; walk requests are rejected while busy (FOnline-like), so a click right after an attack is dropped; critters still occupy whole cells (collision between critters is cell based, visual overlap during interpolation is possible); a chasing NPC re-paths every 0.5 s (one `MovePath` each); no lag compensation; AP/attack numbers are placeholders like the rest of combat.

### Movement during combat (fix)

* An attack now blocks the attacker only for `clamp(AttackMsPerCost * cost, AttackMinMs, AttackMaxMs)` ms (400..1000) and costs `RtApCostAttackPerCost` (5) AP/100 per cost unit, so a 9mm shot is about 0.8 s and 1 AP.
* A walk request during that busy window is no longer dropped: the auto attack and pending action are cancelled and the walk is queued (`Critter::QueuedMove`), started by `StepCritter` when the critter is free (dropped after 3 s). The client gets `Busy` and shows "busy - walking as soon as ready". `RunOnCombat` and the transfer lock do not affect walking.
* Dev command `/battle [ms]` sets the battle flag (Lua `set_battle_ms`); f3_gametest covers attack then walk away and walking during the battle flag.
* A player standing inside a transition region when the transfer lock or battle flag runs out is now transferred (`StepCritter` re-checks regions); at the new 9 cells/s walk speed the bot used to arrive before the 3 s lock expired.


## Equipment slots and weight (inventory UI)

* `InvEntry.equipped` carries the slot: 0 carried, 1 body, 2 head, 3 hand 1 (active weapon, used for attacks), 4 hand 2 (spare weapon). `C2S_Equip` has an optional trailing `slot` byte (0 = automatic). Rules (server authoritative, `Server::EquipItem`): weapons only in the hands, armor in the body slot, helmets (`*Helmet*` keys) in the head slot; automatic weapon equip takes hand 1 and pushes the previous hand-1 weapon to hand 2 when that is free; equipping onto an occupied hand from the other hand swaps.
* Weight: `Stats.carryWeight` = `CarryBase + CarryPerStrength * ST` (config.lua, 25 + 25*ST), `Stats.carried` = sum of unit weight * count, `InvEntry.weight` = unit weight. The demo data has no weights, so unit weights come from a class table (`WeightWeapon 4, WeightArmor 8, WeightAmmo 1, WeightItem 1`) with per-prototype overrides `config.ItemWeights["Key.WEA"] = n`. Pick up / take from containers is refused over the limit.

## Appearance block (protocol_game.h)

* `S2C_Appearance = 139` `{u32 critter, u16 body, u16 head, u16 weapon}` (prototype indices, 0xFFFF = none): sent to a client when it first sees a critter (right after `CritterProto`) and to everybody who sees the critter (and the critter itself) on every equip / unequip.
  The client builds the worn b3d parts from `armorParts` of those prototypes (players start from the base body: the `Default_PC` sockets for body / feet / back / hands / head are dropped; hair, beard, pip-boy sockets stay) and draws the weapon model in the right hand.
* `CombatEvent` gained a trailing `u16 durMs` (attack action length) used to time the attack animation.

### Aggro on every attack attempt

`Server::OnAttacked(target, attacker)` runs for every attack roll in `TickCombat`, **hit or miss**: an NPC target that is not static and has no target turns on the attacker (the normal combat AI then chases / shoots back), same-team NPCs within `AggroAlertRadius` cells (config.lua, default 8, 0 = off) that have no target join in (never against their own team), and the Lua hook `on_attacked(target, attacker)` is called if defined (the AMX OnDamage-like hook is only called on damage, there is no miss equivalent, so it is untouched). Neutral/friendly NPCs follow the same rule - attacking them makes them hostile (static ones never move). Dev/test: `config.ForceMiss` / chat command `/forcemiss [on|off]` make every attack miss; f3_gametest uses a third bot to shoot a neutral "GMC Soldier" with forced misses and asserts the NPC retaliates.

## 10. Accounts and persistence

Code: `engine/server/Account.*` (SHA-256, HMAC, PBKDF2, atomic file writes, `AccountStore`), `engine/server/Persist.cpp` (`OnAuth`, character <-> JSON, `SavePlayer`).

* **Accounts**: `saves/accounts/acc_<lowercase name>.json` (`--saves DIR`, default `<repo>/saves`, gitignored). `{name, algo "pbkdf2-sha256", iterations (10000, `--pbkdf2-iterations`), salt (16 random bytes, hex), hash (hex), characters[]}`. The password is never stored; the in-repo PBKDF2-HMAC-SHA256 is verified against Python `hashlib.pbkdf2_hmac`. The `acc_` prefix keeps reserved Windows file names (`con`, `nul`) harmless. Names are 2-16 letters/digits/`_`, unique case-insensitively; passwords 4-64 characters. One character per account today (the file holds an array, only index 0 is used).
* **Login rate limit**: 5 failed logins per account key or per IP within 5 minutes lock that key for 30 s (`RateLimited`, even the correct password is refused during the lockout). Unknown names burn the same hashing time (no user enumeration by timing). Registration is limited per IP by the same counter.
* **Protocol** (`protocol_game.h`, ACCOUNT BLOCK): `C2S_Auth = 33 {version, mode 0 login / 1 register, name, password}` answered with `S2C_AuthResult = 140 {code, isNew, text}`; on success the usual `LoginOk, MapInfo, Stats, Inventory` follow, otherwise the server disconnects (retry = reconnect). Codes: `Ok BadCredentials NameTaken InvalidName WeakPassword RateLimited GuestDisabled VersionMismatch ServerError Full`. `protocol.h` is untouched.
  The password crosses the network in clear text inside the ENet packet: use a VPN / TLS tunnel for a public server (a real transport encryption layer is not implemented).
* **Guest login**: the old name-only `C2S_Login` is refused (`GuestDisabled`, "this server requires an account") unless the server runs with `--allow-guest`; `--dev-kit` implies it (dev servers, `f3_gametest`). `--local` only restricts the bind address and does not allow guests. A guest name may not collide with a registered account. Guests are never saved.
* **What is saved** (`Persist.cpp: CharacterToJson`): map name, cell position + direction, HP / max HP, dead flag (a dead character logs in dead and is respawned by the normal 5 s rule), SPECIAL, level, xp, skill points, skill ranks, unspent SPECIAL points, "created" flag, inventory as `{proto key, count, slot}` (prototype keys, not pids, so a re-import does not corrupt saves; unknown keys are dropped with a warning; item ids are re-generated), `vars` (`Critter::vars`, script numeric state such as `steps`, `kit_given`) and `gvars` (per player globals, see below).
* **Per player quest state**: `Critter::gvars` overlays the shared `globals_` table. While an AMX conversation runs (`AmxHost::active_`), `GetGlobal/SetGlobal` and the journal natives `OpenTask/CloseTask/AddEntry` read and write the protagonist's `gvars` (shared defaults come from `globals.ini`); behaviour scripts outside a conversation and Lua `get_global/set_global` still use the shared world table. Lua: `cr:get_gvar/set_gvar`, chat `/gvar name [value]`. Active dialogues are not persisted (closed on logout); the task state they produced is.
* **When it is saved**: after a successful login (file becomes real immediately), every `--save-interval` seconds (default 60, 0 = off), on logout / disconnect, on every map transfer (`EnterMap`, includes the death respawn) and on graceful shutdown (`--run-seconds` expiry, SIGINT/SIGTERM, console `quit`). Writes are atomic: temp file in the same directory, previous version kept as `.bak`, then rename. A hard kill (`Stop-Process -Force`) loses at most the changes since the last save.
* **Duplicate login**: logging in while the account is online saves and kicks the old session ("logged in from another location") and the new session takes over at the saved position.
* **Scripts**: `on_login` runs after the character is restored. The dev kit is given once (`kit_given` var) instead of "inventory is empty".
* **Client**: `f3_client` shows a login screen (name, masked password, Login / Register, error line) before connecting; the name is remembered in `client.cfg`, the password never. `--name X --password P [--register]` skips the screen (scripted tests), `--guest` keeps the old name-only login (needs a server with `--allow-guest`), `--name` alone only prefills the screen. `f3_testclient` accepts `--password`/`--register`.
* **Tests**: `f3_gametest --accounts 1|2 --state FILE` against a server without `--dev-kit` (smoke_test part 3 runs phase 1, RESTARTS the server with the same throw-away `--saves`, then phase 2): guest refused, invalid name / weak password / unknown account / taken name (case-insensitive) / wrong password rejected, register + login, walk, spawn + pick up + equip a revolver, `/gvar`; after the restart: wrong password rejected, case-insensitive login, map + cell position + equipped inventory + per player global restored, duplicate login kicks the first session, failed logins rate limited (also for the right password).
* **Gaps**: single character per account (no selection screen), no password change / account deletion, no transport encryption, no admin tools, saves are JSON files (fine for hundreds of accounts, a database for more), persisted `world` state (ground items, doors, container contents, NPC state) is not saved: only players.
