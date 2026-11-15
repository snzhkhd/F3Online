# Original AMX scripts (tools/amxdis)

The 119 original scripts (`extracted/Scripts/*.scra|*.scrb`, copied to `gamedata/server/scripts`) are Pawn AMX v6 programs
(see `docs/server-data.md` section 6 for the format verdict and the VM in `engine/server/Amx.cpp`). There is no source and no
native header, so `tools/amxdis` is the way to read them.

## Viewing and reading scripts

```
cmake -S tools/amxdis -B build/amxdis && cmake --build build/amxdis --config Release
amxdis extracted/Scripts -o extracted/amx_listing        # one <script>.txt listing per file (extracted/ is gitignored)
amxdis "extracted/Scripts/00 Frank.scra"                 # one script to stdout
amxdis --summary extracted/Scripts                       # per script: publics + natives actually called (call-site counts)
amxdis --natives extracted/Scripts [--bound engine/server/AmxHost.cpp] > natives.txt
```

A listing has: header (file/amx version, section sizes), publics with code addresses, the native table (`index name`),
all decoded data strings (`@addr: "text"`, packed and unpacked), and the disassembly. In the code, `call` targets and
`sysreq.c N ; Name` are symbolic, jump targets are `L_<hex>` labels, `push.c <addr>` / `const.pri <addr>` that point at a data string
are annotated with the string. Debug records (`line`, `symbol`) are hidden. Reading tips:

* Pawn calling convention: arguments are pushed right-to-left, then `push.c 4*argc`, then `call`/`sysreq.c`; callee result is `pri`.
  So for `push.c 140 ; "RL1_N1"`, `push.c 136`, `push.c 2082`, `push.c 1`, `push.c 16`, `sysreq.c RegisterReply` the call is
  `RegisterReply(1, 2082, 136, "RL1_N1")` (the first pushed value is the LAST argument).
* Strings passed to natives are data addresses; the decoded text is the comment. Text ids ("RL1_N1", "00_Talked_Frank") are keys of
  the `English.stf`-style string tables.
* `.scra` files are dialogues (publics `OnStartDialogue`, node functions `N<k>` (node start), `NU<k>_N<k>` (builds reply list), `RC/LC...`
  reply conditions, `RA/LA...` reply actions/results); `.scrb` are behaviours (`OnCreate OnUpdate OnDeath OnMessage ...`).
* Float values are IEEE bits in cells (`float*` natives are the Pawn float module).

## Natives

115 distinct natives; 46 are bound by the server (accept-and-ignore stubs included). "argc" = argument counts seen at call sites
(`push.c` immediately before `sysreq.c`). Signatures are inferred (no header); `?` means unknown purpose. Generated from
`amxdis --natives` plus hand notes.

| native | calls | files | argc | bound | inferred signature |
|---|---:|---:|---|---|---|
| `Myself` | 7310 | 105 | 0 | yes | () -> entity handle of the script owner |
| `AttachSound` | 1949 | 72 | 3 | stub | (entity, sound$, ?) |
| `RegisterReply` | 1680 | 40 | 4 | yes | (flag, stfId, actionPublic$, logicPublic$) adds a reply; chosen -> action public, then logic public (bound, dialogue engine) |
| `SetNodeTextFunction` | 1245 | 40 | 1 | yes | (publicName$) reply builder of the node (NU<k>_N<k>); "EndDialogue" ends the conversation (bound) |
| `GetGender` | 970 | 79 | 1 | yes | (entity) -> int |
| `GetGlobal` | 865 | 79 | 1,2 | yes | (name$) -> int (globals.ini / server global table) |
| `RandomNumber` | 667 | 90 | 2 | yes | (lo, hi) -> int |
| `RegisterNodeText` | 642 | 32 | 1 | yes | (textId$) registers the NPC text of the current node (English.stf key) |
| `GetRace` | 583 | 78 | 1 | yes | (entity) -> int |
| `GetSquadID` | 536 | 48 | 1 | yes | (entity) -> squad id |
| `SetCurrentNodeName` | 508 | 40 | 1 | yes | (name$) names the node being built |
| `GetTeam` | 475 | 62 | 1 | yes | (entity) -> team id |
| `SetGlobal` | 375 | 39 | 2 | yes | (name$, value) |
| `SetSquadAttitude` | 362 | 48 | 3 | yes | (squadA, squadB, attitude) directed |
| `float` | 334 | 79 | 1 | yes | (int) -> float bits (Pawn float module) |
| `GetTimeStamp` | 332 | 77 | 0 | yes | () -> int seconds |
| `GetCurrentHP` | 299 | 79 | 1 | yes | (entity) -> int |
| `RemoveSound` | 292 | 72 | 2 | stub | (entity, ?) |
| `PCSpeaker` | 291 | 28 | 0 | yes | () -> entity of the player speaking |
| `MessageGetArg` | 284 | 78 | 1 | no | (i) -> int |
| `FloatingSpeechText` | 225 | 65 | 3,6 | yes | (entity, textId$, ?) text above head; 6-arg form has extras |
| `floatdiv` | 204 | 78 | 2 | yes | (a, b) IEEE, x/0 = +-inf |
| `Protagonist` | 196 | 56 | 0 | yes | () -> player entity |
| `GetTeamRank` | 194 | 48 | 1 | yes | (entity) -> rank |
| `IsDead` | 177 | 48 | 1 | yes | (entity) -> bool, an unresolved entity counts as dead (1) |
| `GetPrimaryAttribute` | 175 | 76 | 2 | yes | (entity, attr) -> int |
| `GetTotalSkill` | 175 | 66 | 2 | yes | (entity, skill) -> int |
| `AddEntry` | 165 | 28 | 2 | yes | (?, ?) adds journal/quest entry (2 args) |
| `GetPosition` | 163 | 48 | 2 | yes | (entity, out float pos[3]) world units, y = 0 |
| `SetValue` | 156 | 78 | 3 | stub | (entity, attr, value) |
| `FloatingDamageText` | 156 | 78 | 5 | no | (entity, ...) 5 args |
| `AddHP` | 156 | 78 | 2 | yes | (entity, delta) |
| `SetSquadRank` | 145 | 48 | 2 | yes | (entity, rank) |
| `GetSquadRank` | 144 | 48 | 1 | yes | (entity) -> rank |
| `AddToSquad` | 144 | 48 | 2 | yes | (entity, squad) -- entity first (the old table had it reversed) |
| `floatcmp` | 141 | 78 | 2 | yes | (a, b) -> -1/0/1 |
| `GetObject` | 101 | 51 | 2 | yes | (handle, index) -> entity id, 0 out of range (lists live one tick) |
| `GetTeamAttitude` | 100 | 48 | 2 | yes | (teamA, teamB) -> attitude (equal ids 1) |
| `GetFacing` | 100 | 48 | 1 | yes | (entity) -> int degrees (0 = +Z, 90 = +X, 8 directions) |
| `floatadd` | 99 | 48 | 2 | yes | (a, b) |
| `Hour` | 98 | 48 | 0 | yes | () -> int |
| `FindObjects` | 97 | 48 | 3 | yes | (out handle, float pos[3], radius) -> count, farthest first, owner excluded |
| `SecondsPassed` | 97 | 75 | 1 | yes | (timestamp) -> int |
| `floatsub` | 96 | 48 | 2 | yes | (a, b) |
| `DisplayStringRef` | 89 | 78 | 4 | yes | (...) 4 args, prints string to the player log |
| `SendMessage` | 87 | 78 | 2,3,4 | no | (entity, msgId[, a1[, a2]]) -> delivers OnMessage |
| `ClearActions` | 85 | 77 | 1 | yes | (entity) |
| `floatmul` | 84 | 79 | 2 | yes | (a, b) |
| `MessageArgCount` | 80 | 78 | 0 | no | () -> int (inside OnMessage) |
| `GetMaxHP` | 80 | 78 | 1 | yes | (entity) -> int |
| `floatround` | 79 | 79 | 2 | yes | (a[, mode]) 1 floor, 2 ceil, 3 toward zero, default floor(x+.5) |
| `UnlockDoor` | 78 | 78 | 1 | yes | (door) |
| `floatfract` | 78 | 78 | 1 | yes | (a) |
| `Distance` | 67 | 48 | 2 | yes | (entityA, entityB) -> int, truncated world units (2 args exactly) |
| `SetTeamAttitude` | 64 | 30 | 3 | yes | (teamA, teamB, attitude) directed |
| `OpenTask` | 54 | 14 | 2 | yes | (taskName$, ?) quest task opened |
| `GetDirectionToEntity` | 50 | 48 | 2 | yes | (entity, target) -> int degrees, acos(dir.z), negative for x < 0 |
| `GetLOS` | 49 | 48 | 2 | yes | (a, b) -> 0 or 2, grid line approximation |
| `GetSquadAttitude` | 49 | 48 | 2 | yes | (squadA, squadB) -> attitude (equal ids 1) |
| `DestroyEntity` | 49 | 47 | 1 | yes | (entity) |
| `CloseTask` | 49 | 15 | 2 | yes | (taskName$, ?) quest task closed |
| `CreateSquad` | 48 | 48 | 0 | yes | (...) -> squad id |
| `CreateTeam` | 48 | 48 | 0 | yes | (...) -> team id |
| `AddToTeam` | 48 | 48 | 2 | stub | (entity, team) -- stub: logged, no team change |
| `IsEntityInArea` | 47 | 47 | 2 | yes | (entity, area) -> bool |
| `GetType` | 46 | 46 | 1 | yes | (entity) -> entity class (1 critter, 2 usable, 3 door, 4 container, 5 item, 6 weapon, 7 armor, 9 ammo) |
| `IsIdle` | 44 | 21 | 1 | yes | (entity) -> bool |
| `StartStore` | 42 | 4 | 1 | stub | (npc) |
| `A_Wait` | 38 | 16 | 2 | yes | (entity, seconds) queued action |
| `FindEntity` | 38 | 10 | 1 | no | (name$) -> entity |
| `A_PlayAnim` | 36 | 7 | 5 | partial | (entity, anim, ...) 5 args |
| `SetFacingTarget` | 26 | 17 | 2 | yes | (entity, target) |
| `A_FloatingSpeechText` | 21 | 6 | 6 | yes | (entity, textId$, ... ) action-queued variant |
| `A_MoveToPosition` | 20 | 16 | 4 | yes | (entity, float dest[3], stopDist, ignored) -- world units, exactly 4 args |
| `A_Attack` | 18 | 15 | 2 | yes | (entity, target) -- real attack through the server combat; the action ends after one attack |
| `IsInParty` | 16 | 1 | 1 | stub | (entity) -> bool |
| `IsLocked` | 14 | 6 | 1 | yes | ? |
| `MapEffectSetActive` | 10 | 5 | 3 | no | (?, ?, active) |
| `A_Wander` | 10 | 8 | 4 | yes | (entity, float upperLeft[3], float lowerRight[3], repetitions) -- 0 = forever |
| `GiveNewItem` | 10 | 2 | 3 | no | (entity, itemType, count) |
| `A_MoveToEntity` | 9 | 6 | 4 | yes | (entity, target, stopDist, reach) -- stops within 5 units; negative stopDist = arrived |
| `GetWayPointCount` | 8 | 7 | 1 | yes | (path$) -> int (EPTH path of the owner's map) |
| `IsItemEquipped` | 7 | 3 | 2 | no | (entity, itemType) |
| `GetWayPointPosition` | 7 | 7 | 3 | yes | (path$, idx, out float pos[3]) -> 1, 0 when out of range |
| `SendMessageToTeam` | 6 | 1 | 2,4 | no | (team, msgId, a1, a2) |
| `PartyHasItem` | 5 | 2 | 1 | yes | (resource$) -> count in the protagonist's inventory |
| `PointDistance` | 5 | 2 | 2 | yes | (p1[3], p2[3]) -> int, truncated world units |
| `GetMapID` | 5 | 5 | 1 | yes | () -> int |
| `HasItem` | 5 | 3 | 2 | yes | (entity, resource$) -> item COUNT by resource name (e.g. "Ammo9mm.AMO") |
| `AttachVEG` | 5 | 5 | 2 | yes | (entity, effect$) -> effect id (logged, nothing rendered) |
| `GetNavPointCount` | 4 | 1 | 0 | yes | () -> 0 (EMNP is empty on the demo maps) |
| `HasDied` | 4 | 1 | 1 | yes | (name$) -> int, the global counter `_DEAD_<instance name>` (not IsDead) |
| `GetVEGID` | 3 | 2 | 2 | yes | (entity, name$) -> effect id, 0 if none |
| `SetFacing` | 3 | 2 | 2 | yes | (entity, int degrees) |
| `MatchEntity` | 3 | 3 | 2 | no | (a, b) -> bool |
| `GetNavPointPosition` | 3 | 1 | 2 | yes | (idx, out float pos[3]) -> 0 |
| `RemoveVEG` | 3 | 2 | 2 | yes | (entity, id) -> 1 |
| `DeactivateEntity` | 3 | 2 | 1 | no | (entity) |
| `A_SendMessage` | 3 | 3 | 2 | no | (entity, msgId) action-queued variant |
| `A_JustAttack` | 2 | 2 | 2 | yes | (entity, target) -- same without walking or turning |
| `Minute` | 2 | 1 | 0 | yes | () -> int |
| `IsUnderFOW` | 2 | 1 | 1 | yes | (entity) -> 1 when no living player on the map can see the entity (server model) |
| `EquipItem` | 2 | 1 | 3 | no | (entity, itemType, slot) |
| `GetNearestPath` | 2 | 2 | 2 | yes | (entity, out name$) -> 1, path whose first waypoint is nearest |
| `IsInDialogue` | 2 | 1 | 1 | yes | (entity) -> bool |
| `StartDialogue` | 2 | 2 | 2 | yes | (npc, pc) |
| `ActivateEntity` | 2 | 1 | 1 | no | (entity) |
| `FindTeamMembers` | 1 | 1 | 2 | no | (team, ?) |
| `MapEffectResetElapsedTime` | 1 | 1 | 2 | no | (?, ?) |
| `SetTeamRank` | 1 | 1 | 2 | yes | (entity, rank) |
| `LockDoor` | 1 | 1 | 1 | yes | (door) |
| `SetModel` | 1 | 1 | 2 | no | (entity, model$) |
| `A_Kill` | 1 | 1 | 1 | yes | (entity) |
| `GetLOSPoint` | 1 | 1 | 2 | yes | (entity, float point[3]) -> 0 or 2, grid line approximation |
| `A_ReloadWeapon` | 1 | 1 | 1 | stub | (entity) |

## Dialogue engine (server, `AmxHost::StartDialog`)

Graph of a `.scra` (verified by running Armstrong): `OnStartDialogue` -> `RL1_N1` picks the first node and calls `N<k>`. A node function runs `NA<k>`,
`SetCurrentNodeName`, `SetNodeTextFunction("NU<k>_N<k>")` and (if `NTC` holds) `NTA` + `RegisterNodeText(stfId)`. The server then calls the
reply builder `NU<k>_N<k>`, which calls `RegisterReply(flag, stfId, "RA..", "RL..")` for every reply whose `RC<j>_N<k>` condition holds, and sends
`DialogNode` (text and replies, stf ids resolved from `strings.tsv`, `{cNNN}` tags stripped). `DialogChoose(i)` runs the reply's action public then its
logic public (`LA..` sets globals / tasks, then `N<next>`); the node is presented again. `SetNodeTextFunction("EndDialogue")` closes. Quest natives
`OpenTask/CloseTask/AddEntry` set globals `Task<id>` (1 open, 2 closed) / `TaskEntry<id>` and tell the player. Behaviour scripts that call
`StartDialogue(npc, pc)` (00_TutorialFrank) start a conversation themselves. Protocol: `protocol_game.h` dialogue block (C 30 Talk / 31 DialogChoose /
32 DialogClose, S 136 DialogOpen / 137 DialogNode / 138 DialogClose). Talking walks the player next to the NPC first (`Server::TalkTo`). Client: Talk cursor
(NPC prototype has a dialogue) sends Talk; raylib window with wrapped text and one button per reply; Esc leaves. Verified headless by
`f3_gametest` (Armstrong: open, 8 choices, goodbye closes); the raylib window itself compiles but was not exercised visually.

## Behaviour host (step 3: NPC behaviours, `AmxHost` + `AmxActions.cpp`)

* `OnUpdate(time)` receives the server time in ms (the scripts compare it with their own stamp; without the argument they never ran). It is
  called every 250 ms per instance, staggered. A script may destroy its own entity (`DestroyEntity(Myself())`): detached instances live on
  in a graveyard until the end of the tick.
* Per player quest globals: `GetGlobal/SetGlobal` for the "protagonist" (the conversation player, else the nearest player within 20 cells)
  go to that player's `gvars` (saved with the character); other globals stay shared. `scripts/original/tutorial.lua` task/journal state
  (`Task<id>`, `TaskEntry<id>`) is per player; the door/generator/life-support state is world state (shared) on purpose.
* Action queue per critter (`A_Wait`, `A_PlayAnim`, `A_MoveToPosition/Entity`, `A_Wander`, `A_FloatingSpeechText`, `A_Kill`, `A_ReloadWeapon`,
  `ClearActions`, `IsIdle`) executed on the 20 Hz tick; `FloatingSpeechText`/`A_FloatingSpeechText` send the STF text as chat from the NPC.
* Also bound: squads/team ranks and attitudes (`CreateSquad`, `AddToSquad(entity, squad)`, `GetSquadID`, `SetSquadRank`, `GetSquadRank`, `SetTeamRank`,
  `GetTeamRank`, `Set/GetTeamAttitude`, `Set/GetSquadAttitude`), `GetPrimaryAttribute`, `GetTotalSkill`, `DisplayStringRef`, `IsEntityInArea` (the area is
  a map name), the spatial natives of `AmxSpatial.cpp` (see below), `HasItem`/`PartyHasItem` (counts), `HasDied`.
* Argument counts: like `VerifyScriptParameters` of the original, a native called with a different number of arguments logs one warning
  ("expects N parameter(s) but received M") and returns 0.
* `A_Attack` / `A_JustAttack` are real: the action sets the critter's target and the server combat (`Server::TickCombat`: weapon range, line of sight, AP,
  breaktime, hit roll, damage) does the rest; the action ends after ONE attack (or when the target dies, after 30 s, or at once when the target is gone).
  `A_Attack` walks into range, `A_JustAttack` only attacks when already in range. `SetAttacksEnabled(false)` (dev command `/amxattacks off`) makes both
  end at once. The scripted insurgents of the tutorial map therefore hunt players like in the original; test clients switch it off (see Tests).
* `A_Wander(entity, upperLeft, lowerRight, reps)`: like the original, queues Wait (1000..5999 ms), a move to a random point of the rectangle, then the next
  repetition (0 = forever). The rectangle needs `upperLeft.x <= lowerRight.x` and `upperLeft.z >= lowerRight.z`, otherwise the upper-left value is kept.
* Spatial natives (`AmxSpatial.cpp`), world units = 2 cells, world = cell / 2 + map origin, y is always 0 (no height model):
  * `Distance`, `PointDistance`, `GetFacing`, `GetDirectionToEntity` return truncated ints like the original (`__ftol2`). Angles are `acos(dir.z)` in
    degrees (0 = +Z = cell +y, 90 = +X), negative when x < 0. Facing is stored as 8 directions, so `SetFacing(37)` reads back 45.
  * `FindObjects(out handle, float pos[3], radius)`: owner's map, owner excluded, visible entities whose box intersects the circle (XZ), sorted FARTHEST first
    (`dist(a) >= dist(b)`, checked in the decompilation), output cell written only when count > 0; `GetObject(handle, i)` is 0 out of range.
  * `GetLOS` / `GetLOSPoint`: APPROXIMATION. Bresenham line over the walkability grid, endpoints excluded; returns 2 (clear) or 0. The original uses a height
    grid (`AreaMap`) and returns 0/1/2; scripts use it as a boolean.
  * `IsUnderFOW(entity)`: SERVER MODEL. The original reads visibility bits filled by the client. Here: an entity is under fog when no living player on its map
    has it within perception range with a clear line of sight (players always see themselves).
  * `GetWayPointCount/Position`, `GetNearestPath`: the owner's map paths (EPTH, `Map::paths`, names in `Map::pathNames`). `GetNavPoint*` read EMNP, empty on all
    demo maps (count 0).
  * `AttachVEG/RemoveVEG/GetVEGID`: ids only, nothing is rendered (no effect protocol); `AttachSound` is still a stub.
  * `HasDied(name$)` reads the global `_DEAD_<instance name>`; the server increments it when a script NPC dies (`AmxHost::OnDeath`). NOTE: the original has
    `RecordDeath` but nothing in the demo calls it, so with the real engine the counter never moves; ours is an addition.
* Still unbound (logged once as "unimplemented native", return 0): `A_SendMessage`, `ActivateEntity`, `DeactivateEntity`, `EquipItem`, `FindEntity`, `FindTeamMembers`, `FloatingDamageText`, `GiveNewItem`, `IsItemEquipped`, `MapEffectResetElapsedTime`, `MapEffectSetActive`, `MatchEntity`, `MessageArgCount`, `MessageGetArg`, `SendMessage`, `SendMessageToTeam`, `SetModel`.
  Stubs (bound, do little): `AddToTeam`, `AttachSound`, `RemoveSound`, `SetValue`, `IsInParty`, `A_ReloadWeapon`, `StartStore` (a no-op in the original too);
  `A_PlayAnim` only occupies the action queue.
* Debugging: env `F3_AMX_TRACE=<script name substring>` traces every native call of matching live instances; console `amx <script> <public>`
  runs a test instance with tracing.
* Test: `f3_gametest --tutorial` against a `--dev-kit` server: Armstrong's dialogue closes Task3224, Frank reacts (bark or own conversation),
  his dialogue sets `00_Talked_Frank` for this player only. Set `F3_AMX_ATTACKS=1` to leave the scripted NPC attacks on during that test (the default
  test client sends `/amxattacks off` first).
* Native tests (dev only, `--dev-kit` servers): `/amxnative <program>` runs natives by name on a throw-away instance (`AmxHost::NativeTest`), `/amxattacks on|off`
  toggles AMX attacks. `f3_gametest` (main suite) drives them: int/float returns, argument counts, float module, `AddToSquad`, `HasItem`, LOS grid, FOW,
  `FindObjects`, waypoints, VEG ids, `HasDied`, `A_MoveToPosition`, `A_Wander`, `A_Attack`, `A_JustAttack`. Set `F3_AMX_ATTACKS=1` to leave the scripted NPC attacks on during that test (the default
  test client sends `/amxattacks off` first).
* Native tests (dev only, `--dev-kit` servers): `/amxnative <program>` runs natives by name on a throw-away instance (`AmxHost::NativeTest`), `/amxattacks on|off`
  toggles AMX attacks. `f3_gametest` (main suite) drives them: int/float returns, argument counts, float module, `AddToSquad`, `HasItem`, LOS grid, FOW,
  `FindObjects`, waypoints, VEG ids, `HasDied`, `A_MoveToPosition`, `A_Wander`, `A_Attack`, `A_JustAttack`.

## Port status

| script | status |
|---|---|
| `00 Corporal Armstrong.scra`, `00 Frank.scra` (all 36 `.scra` use the same natives) | running on the AMX dialogue engine (Armstrong and Frank driven in `f3_gametest --tutorial`) |
| `00_TutorialGeneratorRoomDoor01` | Lua `scripts/original/tutorial.lua` (UseSkill lockpick -> `00_Lock_Status`, unlock) |
| `00_TutorialGenerator01` | Lua port (repair -> `00_Generator_Status`, `00_Life_Support_Status`, task 3231 closed); tested via `/usescript` |
| `00_TutorialComputer01` | Lua (status message only; map effect `Life_SupportEffects` and message passing not ported) |
| `00_Tutorial_Vault_Door_Console` | Lua (unlocks/opens `Outer_EntranceDS_Doors*`; the console animation `A_PlayAnim` is not ported) |
| `00_TutorialFrank.scrb`, `CaptainArmstrong.scrb` | AMX behaviour runs on the action queue: Frank barks / starts his dialogue (tested); Armstrong's OnUpdate runs (not driven further) |
| insurgents, dogs, barrels, windows, vault dwellers, map scripts, `00_TutorialDefault_VaultDoor` | their behaviour scripts (`.scrb`) run on the AMX host with the natives above; `A_Attack` is real, insurgent hostility additionally comes from `scripts/npc/hostile.lua` by team. Behaviour not verified beyond "runs without script errors" |
| everything else (~100 scripts) | not ported; AMX prototype runs their OnCreate/OnUpdate |

Not done: skill-use UI for object scripts (UseSkill identifiers), messages between scripts (`SendMessage` family), stores, item natives (`GiveNewItem`, `EquipItem`, `IsItemEquipped`), `FindEntity`/`MatchEntity`, map effects.
The Lua hook `on_use(c, obj_id, proto_key, script)` now receives the original script name; `stf(id)`, `set_door_locked`, `set_door_open` are new Lua globals.
