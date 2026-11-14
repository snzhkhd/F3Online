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
| `AttachSound` | 1949 | 72 | 3 | yes | (entity, sound$, ?) |
| `RegisterReply` | 1680 | 40 | 4 | no | (flag, stfId, actionPublic$, logicPublic$) adds a reply; chosen -> action public, then logic public (bound, dialogue engine) |
| `SetNodeTextFunction` | 1245 | 40 | 1 | no | (publicName$) reply builder of the node (NU<k>_N<k>); "EndDialogue" ends the conversation (bound) |
| `GetGender` | 970 | 79 | 1 | yes | (entity) -> int |
| `GetGlobal` | 865 | 79 | 1,2 | yes | (name$) -> int (globals.ini / server global table) |
| `RandomNumber` | 667 | 90 | 2 | yes | (lo, hi) -> int |
| `RegisterNodeText` | 642 | 32 | 1 | no | (textId$) registers the NPC text of the current node (English.stf key) |
| `GetRace` | 583 | 78 | 1 | yes | (entity) -> int |
| `GetSquadID` | 536 | 48 | 1 | no | (entity) -> squad id |
| `SetCurrentNodeName` | 508 | 40 | 1 | no | (name$) names the node being built |
| `GetTeam` | 475 | 62 | 1 | yes | (entity) -> team id |
| `SetGlobal` | 375 | 39 | 2 | yes | (name$, value) |
| `SetSquadAttitude` | 362 | 48 | 3 | yes | (squad, other, attitude) |
| `float` | 334 | 79 | 1 | yes | (int) -> float bits (Pawn float module) |
| `GetTimeStamp` | 332 | 77 | 0 | yes | () -> int seconds |
| `GetCurrentHP` | 299 | 79 | 1 | yes | (entity) -> int |
| `RemoveSound` | 292 | 72 | 2 | yes | (entity, ?) |
| `PCSpeaker` | 291 | 28 | 0 | no | () -> entity of the player speaking |
| `MessageGetArg` | 284 | 78 | 1 | no | (i) -> int |
| `FloatingSpeechText` | 225 | 65 | 3,6 | no | (entity, textId$, ?) text above head; 6-arg form has extras |
| `floatdiv` | 204 | 78 | 2 | yes | (a, b) |
| `Protagonist` | 196 | 56 | 0 | yes | () -> player entity |
| `GetTeamRank` | 194 | 48 | 1 | no | (entity) -> rank |
| `IsDead` | 177 | 48 | 1 | yes | (entity) -> bool |
| `GetPrimaryAttribute` | 175 | 76 | 2 | no | (entity, attr) -> int |
| `GetTotalSkill` | 175 | 66 | 2 | no | (entity, skill) -> int |
| `AddEntry` | 165 | 28 | 2 | no | (?, ?) adds journal/quest entry (2 args) |
| `GetPosition` | 163 | 48 | 2 | yes | (entity, float pos[3]) by-reference |
| `SetValue` | 156 | 78 | 3 | yes | (entity, attr, value) |
| `FloatingDamageText` | 156 | 78 | 5 | no | (entity, ...) 5 args |
| `AddHP` | 156 | 78 | 2 | yes | (entity, delta) |
| `SetSquadRank` | 145 | 48 | 2 | yes | (entity, rank) |
| `GetSquadRank` | 144 | 48 | 1 | no | (entity) -> rank |
| `AddToSquad` | 144 | 48 | 2 | yes | (squad, entity) |
| `floatcmp` | 141 | 78 | 2 | yes | (a, b) -> -1/0/1 |
| `GetObject` | 101 | 51 | 2 | no | (entity |
| `GetTeamAttitude` | 100 | 48 | 2 | no | (teamA, teamB) -> attitude |
| `GetFacing` | 100 | 48 | 1 | yes | (entity) -> float bits |
| `floatadd` | 99 | 48 | 2 | yes | (a, b) |
| `Hour` | 98 | 48 | 0 | yes | () -> int |
| `FindObjects` | 97 | 48 | 3 | no | (...) -> count/first, 3 args |
| `SecondsPassed` | 97 | 75 | 1 | yes | (timestamp) -> int |
| `floatsub` | 96 | 48 | 2 | yes | (a, b) |
| `DisplayStringRef` | 89 | 78 | 4 | no | (...) 4 args, prints string to the player log |
| `SendMessage` | 87 | 78 | 2,3,4 | no | (entity, msgId[, a1[, a2]]) -> delivers OnMessage |
| `ClearActions` | 85 | 77 | 1 | yes | (entity) |
| `floatmul` | 84 | 79 | 2 | yes | (a, b) |
| `MessageArgCount` | 80 | 78 | 0 | no | () -> int (inside OnMessage) |
| `GetMaxHP` | 80 | 78 | 1 | yes | (entity) -> int |
| `floatround` | 79 | 79 | 2 | yes | (a[, mode]) |
| `UnlockDoor` | 78 | 78 | 1 | yes | (door) |
| `floatfract` | 78 | 78 | 1 | yes | (a) |
| `Distance` | 67 | 48 | 2 | yes | (entityA, entityB) -> float bits |
| `SetTeamAttitude` | 64 | 30 | 3 | yes | (teamA, teamB, attitude) |
| `OpenTask` | 54 | 14 | 2 | no | (taskName$, ?) quest task opened |
| `GetDirectionToEntity` | 50 | 48 | 2 | no | (entity, target) -> float bits |
| `GetLOS` | 49 | 48 | 2 | yes | (a, b) -> bool |
| `GetSquadAttitude` | 49 | 48 | 2 | no | (squadA, squadB) |
| `DestroyEntity` | 49 | 47 | 1 | yes | (entity) |
| `CloseTask` | 49 | 15 | 2 | no | (taskName$, ?) quest task closed |
| `CreateSquad` | 48 | 48 | 0 | yes | (...) -> squad id |
| `CreateTeam` | 48 | 48 | 0 | yes | (...) -> team id |
| `AddToTeam` | 48 | 48 | 2 | yes | (team, entity) |
| `IsEntityInArea` | 47 | 47 | 2 | no | (entity, area) -> bool |
| `GetType` | 46 | 46 | 1 | no | (entity) -> entity type (critter/item/map object) |
| `IsIdle` | 44 | 21 | 1 | no | (entity) -> bool |
| `StartStore` | 42 | 4 | 1 | no | (npc) |
| `A_Wait` | 38 | 16 | 2 | no | (entity, seconds) queued action |
| `FindEntity` | 38 | 10 | 1 | no | (name$) -> entity |
| `A_PlayAnim` | 36 | 7 | 5 | no | (entity, anim, ...) 5 args |
| `SetFacingTarget` | 26 | 17 | 2 | no | (entity, target) |
| `A_FloatingSpeechText` | 21 | 6 | 6 | no | (entity, textId$, ... ) action-queued variant |
| `A_MoveToPosition` | 20 | 16 | 4 | no | (entity, x, y, z) |
| `A_Attack` | 18 | 15 | 2 | no | (entity, target) |
| `IsInParty` | 16 | 1 | 1 | no | (entity) -> bool |
| `IsLocked` | 14 | 6 | 1 | yes | ? |
| `MapEffectSetActive` | 10 | 5 | 3 | no | (?, ?, active) |
| `A_Wander` | 10 | 8 | 4 | no | (entity, ?, ?, ?) |
| `GiveNewItem` | 10 | 2 | 3 | no | (entity, itemType, count) |
| `A_MoveToEntity` | 9 | 6 | 4 | no | (entity, target, ?, ?) |
| `GetWayPointCount` | 8 | 7 | 1 | no | (path$) -> int |
| `IsItemEquipped` | 7 | 3 | 2 | no | (entity, itemType) |
| `GetWayPointPosition` | 7 | 7 | 3 | no | (path$, idx, float pos[3]) |
| `SendMessageToTeam` | 6 | 1 | 2,4 | no | (team, msgId, a1, a2) |
| `PartyHasItem` | 5 | 2 | 1 | no | (itemType) -> bool |
| `PointDistance` | 5 | 2 | 2 | no | (p1[], p2[]) -> float bits |
| `GetMapID` | 5 | 5 | 1 | yes | () -> int |
| `HasItem` | 5 | 3 | 2 | no | (entity, itemType) -> bool/count |
| `AttachVEG` | 5 | 5 | 2 | no | (entity, effect$) visual effect group |
| `GetNavPointCount` | 4 | 1 | 0 | no | () -> int |
| `HasDied` | 4 | 1 | 1 | yes | (entity) -> bool |
| `GetVEGID` | 3 | 2 | 2 | no | (entity, name$) |
| `SetFacing` | 3 | 2 | 2 | no | (entity, angle) |
| `MatchEntity` | 3 | 3 | 2 | no | (a, b) -> bool |
| `GetNavPointPosition` | 3 | 1 | 2 | no | (idx, float pos[3]) |
| `RemoveVEG` | 3 | 2 | 2 | no | (entity, id) |
| `DeactivateEntity` | 3 | 2 | 1 | no | (entity) |
| `A_SendMessage` | 3 | 3 | 2 | no | (entity, msgId) action-queued variant |
| `A_JustAttack` | 2 | 2 | 2 | no | (entity, target) |
| `Minute` | 2 | 1 | 0 | yes | () -> int |
| `IsUnderFOW` | 2 | 1 | 1 | no | (entity) -> bool |
| `EquipItem` | 2 | 1 | 3 | no | (entity, itemType, slot) |
| `GetNearestPath` | 2 | 2 | 2 | no | (entity, ?) |
| `IsInDialogue` | 2 | 1 | 1 | no | (entity) -> bool |
| `StartDialogue` | 2 | 2 | 2 | no | (npc, pc) |
| `ActivateEntity` | 2 | 1 | 1 | no | (entity) |
| `FindTeamMembers` | 1 | 1 | 2 | no | (team, ?) |
| `MapEffectResetElapsedTime` | 1 | 1 | 2 | no | (?, ?) |
| `SetTeamRank` | 1 | 1 | 2 | yes | (entity, rank) |
| `LockDoor` | 1 | 1 | 1 | yes | (door) |
| `SetModel` | 1 | 1 | 2 | no | (entity, model$) |
| `A_Kill` | 1 | 1 | 1 | no | (entity) |
| `GetLOSPoint` | 1 | 1 | 2 | no | (entity, point[]) |
| `A_ReloadWeapon` | 1 | 1 | 1 | no | (entity) |

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

## Port status

| script | status |
|---|---|
| `00 Corporal Armstrong.scra`, `00 Frank.scra` (all 36 `.scra` use the same natives) | running on the AMX dialogue engine (Armstrong tested; Frank not driven in a test) |
| `00_TutorialGeneratorRoomDoor01` | Lua `scripts/original/tutorial.lua` (UseSkill lockpick -> `00_Lock_Status`, unlock) |
| `00_TutorialGenerator01` | Lua port (repair -> `00_Generator_Status`, `00_Life_Support_Status`, task 3231 closed); tested via `/usescript` |
| `00_TutorialComputer01` | Lua (status message only; map effect `Life_SupportEffects` and message passing not ported) |
| `00_Tutorial_Vault_Door_Console` | Lua (unlocks/opens `Outer_EntranceDS_Doors*`; the console animation `A_PlayAnim` is not ported) |
| `00_TutorialFrank.scrb`, `CaptainArmstrong.scrb` | AMX only: starts dialogue via `StartDialogue`; walking/`A_*` actions are unimplemented natives |
| insurgents, dogs, barrels, windows, vault dwellers, map scripts, `00_TutorialDefault_VaultDoor` | not ported (insurgent hostility still comes from `scripts/npc/hostile.lua` by team) |
| everything else (~100 scripts) | not ported; AMX prototype runs their OnCreate/OnUpdate |

Not done: skill-use UI (UseSkill identifiers), the `A_*` action queue, messages, squads/teams, stores, `A_FloatingSpeechText`.
The Lua hook `on_use(c, obj_id, proto_key, script)` now receives the original script name; `stf(id)`, `set_door_locked`, `set_door_open` are new Lua globals.
