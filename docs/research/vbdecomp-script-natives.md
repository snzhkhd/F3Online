# AMX natives vs. the Van Buren decompilation

Source: `_work/vbdecomp-main/repos/openvb/` (see `vbdecomp-overview.md`). File names are in `src/game/` unless noted; `engine:` = `openje/src/...`.
Table size: 115 natives we know (plus the 8 `float*` natives). **Every one of them resolves to a defined `Native*` body.**
Tables: `engineNatives` @6FFAF8 (155 entries), `gameNatives` @6F3750 (45), `float_Natives` (9) in `GameScriptNatives.cpp`.

Completeness labels: **full** = logic is in the native/its direct callee; **engine** = native is thin, logic lives in a reconstructed engine method (also present);
**stub** = the original does (almost) nothing; **partial** = depends on state we do not have.

## Common rules (apply to every native)

* Owner id (`Myself`) top 3 bits: 0 entity, 1 map (id = map id), 2 trigger. Map-scoped natives (waypoints, nav points, `FindObjects`) resolve the map from the owner (`ResolveScriptMapID`).
* `VerifyScriptParameters` requires the **exact** argument count; a mismatch logs and returns 0. Some amxdis call sites with "argc 1,2" are therefore errors in the original.
* Unknown or inactive entity: usually logs and returns 0 (`IsDead` returns **1**; `SetTeamRank` returns 1).
* Vectors are `float[3]` passed by reference; floats are IEEE bits in cells; `Distance`, `PointDistance`, `GetFacing`, `GetDirectionToEntity` return **truncated integers**.
* Units are world units (1 world unit = 2 of our 0.5 cells).

## Implementation status (our server, `engine/server/Amx*.cpp`)

Done and tested in `f3_gametest` (`/amxnative`, dev-kit servers): the mismatches of this document are fixed. **Done:** `Distance`, `PointDistance`, `GetFacing`,
`GetDirectionToEntity` (truncated ints), `SetFacing`, `SetFacingTarget`, `A_MoveToPosition` / `A_Wander` / `FindObjects` / `GetPosition` / `GetWayPointPosition`
(float[3], world units), `A_MoveToEntity` (stop distance, reachability), `HasDied` (`_DEAD_<name>` counter), `IsDead` (unresolved = 1), `HasItem` / `PartyHasItem`
(counts by resource name), `floatdiv` / `floatround` / `floatcmp` / `floatfract`, `AddToSquad(entity, squad)` (the argument order in the old table was wrong; AddToTeam is
(entity, team) too), `Set/GetTeamAttitude`, `Set/GetSquadAttitude`, exact argument counts via `Argc` (warn once, return 0), `FindObjects` + `GetObject` + `GetType`,
`GetWayPointCount`, `GetNearestPath`, `GetNavPointCount/Position` (always empty), `AttachVEG` / `RemoveVEG` / `GetVEGID` (ids only), `A_Attack` / `A_JustAttack`
(through `Server::TickCombat`), `GetLOS` / `GetLOSPoint` (grid approximation), `IsUnderFOW` (server model).
Findings from the source while implementing: the `FindObjects` order is farthest first (`dist(a) >= dist(b)`), `RecordDeath` has no caller in any demo script or engine path
(our `_DEAD_` counter is an addition so that scripts can observe deaths), `GetNavPoint*` are empty because the demo maps have no EMNP.
**Still unbound:** `MessageGetArg`, `MessageArgCount`, `SendMessage`, `SendMessageToTeam`, `A_SendMessage`, `FloatingDamageText`, `FindEntity`, `MatchEntity`, `FindTeamMembers`,
`MapEffectSetActive`, `MapEffectResetElapsedTime`, `GiveNewItem`, `IsItemEquipped`, `EquipItem`, `SetModel`, `ActivateEntity`, `DeactivateEntity`. **Stubs:** `AddToTeam`, `AttachSound`,
`RemoveSound`, `SetValue`, `IsInParty`, `A_ReloadWeapon`, `StartStore` (original no-op), `A_PlayAnim` (queue slot only). See `docs/amx-scripts.md` for the approximations.

## Table

| Native | Where (file : function) | Completeness | Semantics to replicate |
|---|---|---|---|
| Myself | GameScriptNatives.cpp | full | owner id |
| Protagonist | GameScriptPlayerNatives.cpp | full | the single local PC id; for us: caller's/nearest player |
| PCSpeaker | GameScriptDialogueAuxNatives.cpp @5C3A50 (`DialogueManager::GetPCSpeaker`) | full | the PC listening to `owner` in an active dialogue, else 0 |
| GetGender / GetRace | GameScriptCreatureNatives.cpp | full | `baseValues[5]` / `baseValues[3]` from the GCRE chunk (also GetEthnicity `[6]`, GetSubRace `valueD8`, GetLevel `[0]`) |
| GetType | GameScriptQueryNatives.cpp | full | `entityClass`: 1 critter, 2 usable, 3 door, 4 container, 5 item, 6 weapon, 7 armor, 9 ammo |
| GetObject | GameScriptQueryNatives.cpp + startup/FrameLookupCaches.cpp | full | entity id from a frame list handle, 0 when index out of range. Lists live one frame |
| FindObjects | GameScriptObjectSearch.cpp (`FindObjects`) + openje graphics `OctreeShape.cpp` | **full** | `(out handle, float pos[3], float radius) -> count`. Owner's map, owner excluded, only `renderEnabled` objects. Circle in the XZ plane against each object's AABB (height ignored). Sorted with `dist(a) >= dist(b)` (farthest first, verified in the source; implemented). Output cell written only when count>0; handles are per frame |
| FindEntity | GameScriptNatives.cpp (`FindEntityByName`) | full | exact instance-name lookup, 0 if absent |
| MatchEntity | GameScriptQueryNatives.cpp | full | glob (`*`, `?`, `\`), case-insensitive over `displayName` of all live entities; returns count + list handle |
| IsDead | GameScriptQueryNatives.cpp | engine | `state==7 \|\| currentHealth<=0`; unresolved entity -> 1 |
| HasDied | GameScriptDeathRecords.cpp @5C8750 | full | reads global `_DEAD_<name>` (a **counter**, not `IsDead`); `RecordDeath` increments `_DEAD_<displayName>`. Our old alias `HasDied=IsDead` was wrong (fixed). Takes a NAME string. `RecordDeath` has no caller in the demo |
| IsIdle | GameScriptQueryNatives.cpp | engine | action queue idle |
| IsInParty | GameScriptQueryNatives.cpp | full | party membership |
| IsInDialogue | GameScriptDialogueNatives.cpp @5C39D0 | full | entity is speaker or listener of any active dialogue |
| IsLocked | GameScriptItemNatives.cpp | full | doors only, raw `locked` byte (containers: 0) |
| LockDoor / UnlockDoor | GameScriptDoorNatives.cpp @45A1B0 / @45A240 | full | sets `door->locked` 1/0, door entities only |
| IsEntityInArea | GameScriptAreaNatives.cpp | full | **case-insensitive prefix match** of the argument against the entity's map resource name; no trigger geometry |
| GetMapID | GameScriptNatives.cpp | full | `mapHandle` |
| IsUnderFOW | GameScriptFogNatives.cpp @5C7D10 | **partial** | `!AreaMap visible bit of the entity's cell` (0 if FOW disabled). The visible bits are cleared per frame and filled by client-side vision (`QueueVisibleCell`). The server has no model; define it ourselves (e.g. no player within sight/LOS of the entity) |
| GetPosition | GameScriptSpatialNatives.cpp | full | writes float x,y(height),z world units |
| GetFacing | GameScriptSpatialNatives.cpp | full | **integer** degrees of `rotation.x` (ours returns float bits: bug) |
| SetFacing | GameScriptSpatialNatives.cpp | full | integer degrees |
| SetFacingTarget | GameScriptSpatialNatives.cpp | full | yaw = `acos(dir.z)` in degrees, negated if `dir.x<0` (0=+Z, 90=+X) |
| GetDirectionToEntity | GameScriptSpatialNatives.cpp | full | same angle, truncated int |
| Distance / PointDistance | GameScriptSpatialNatives.cpp / GameScriptPointNatives.cpp | full | 3D distance, **truncated int** (scripts compare with int constants) |
| GetLOS | GameScriptSightNatives.cpp -> engine: `EntitySight.cpp`, `AreaMapSight.cpp` | engine | needs the map's fog/LOS height grid (`AreaMap`). Eye = model height + y; DDA ray over cells inside the target's bounds; returns 0/1/2 (script use = boolean) |
| GetLOSPoint | GameScriptSightNatives.cpp -> `Entity::GetLOSPoint` | engine | same ray test to a point `float[3]`; 0 or 2 |
| GetTimeStamp | GameScriptNatives.cpp | full | Gregorian calendar seconds + entity time offset `value29C` |
| SecondsPassed | GameScriptCalendarNatives.cpp | full | `now - ts`, clamped at 0 |
| Hour / Minute | GameScriptCalendarNatives.cpp | full | calendar components (+entity offset) |
| RandomNumber | GameScriptPointNatives.cpp -> startup/Random*.cpp | full | inclusive, swaps reversed bounds, returns lo if equal; LCG `s*1664525+1013904223`, `(s>>8)&0x7FFF` |
| GetGlobal | GameScriptGlobalQueryNatives.cpp | full | one global table, exactly one argument |
| SetGlobal | GameScriptVariableNatives.cpp | full | `Server::SetGlobal`, replicated; Inc/Dec wrap at 32 bits |
| SetValue | GameScriptValueNatives.cpp | full | per-entity integer slot, returns the value |
| GetCurrentHP / GetMaxHP | GameScriptCreatureNatives.cpp | full | `currentHealth`, `maximumHealth` (template GENT chunk, not derived from SPECIAL) |
| AddHP | GameScriptHealthNatives.cpp | full | `SetEntityHealth(cur+delta)`, clamped to max, packet sent; **never kills** (death only comes from the damage effect). SetHP same |
| FloatingDamageText | GameScriptDamageText.cpp @5C2500 | full | `(ent, text$, r,g,b)` broadcast over the entity |
| FloatingSpeechText | GameScriptSpeechNatives.cpp @5C25E0 | full | `(ent, strref, ms, r,g,b)` 6 args; ms 0 = `(len*0.1+1)*1000` |
| A_FloatingSpeechText | GameScriptSpeechNatives.cpp | full | same but queued as an action |
| DisplayStringRef | GameScriptDisplayNatives.cpp @45A410 | full (client) | `(strref,r,g,b)` message-log line; client-only |
| HasItem | GameScriptInventoryQueries.cpp @5C84C0 | full | **count** of items with that resource name |
| PartyHasItem | GameScriptInventoryQueries.cpp @5C88A0 | full | sum over party members |
| GiveNewItem | GameScriptGiveItem.cpp @5C8A60 | full | `(ent, resource$, count)`: create + add to inventory, count times |
| IsItemEquipped | GameScriptItemNatives.cpp | full | any equipped item with that resource |
| EquipItem | GameScriptItemNatives.cpp @45A580 | full | `(ent, resource$, slot)`: first matching item, `Equip(item, slot)` |
| GetTeam / GetTeamRank | GameScriptTeamNatives.cpp | full | `value368` / `value36C` |
| SetTeamRank | same | full | sets + broadcasts, returns 1 even without entity |
| AddToTeam | same -> engine: `WorldTeams.cpp` | engine | fails for unknown team/already member; leaves the old team |
| CreateTeam / CreateSquad | same | engine | ids 1..1999 preexist, new ones from 2000 |
| SetTeamAttitude / SetSquadAttitude | same | engine | directed A->B value; engine itself never reads attitudes for combat |
| GetTeamAttitude / GetSquadAttitude | GameScriptAttitudeNatives.cpp | full | equal ids -> 1; unset -> 0 |
| FindTeamMembers | GameScriptTeamNatives.cpp | full | `(out handle, team) -> count`, list per frame |
| GetSquadID / GetSquadRank / SetSquadRank / AddToSquad | GameScriptTeamNatives.cpp | full | parallel to teams; `AddToSquad(entity, squad)` (checked, implemented) |
| SendMessage | GameScriptMessageNatives.cpp | full | enqueues `[from,msg,args...]` on target; one message per entity update, delivered to `OnMessage(from,msg)` before `OnUpdate` |
| SendMessageToTeam | GameScriptMessageGroups.cpp | full | all members; squad variant returns delivered count |
| A_SendMessage | GameScriptMessageGroups.cpp | full | queued as an action on the caller |
| MessageArgCount / MessageGetArg | GameScriptMessageNatives.cpp | full | extra args of the current message (`values[i+2]`, only upper bound checked) |
| ClearActions | GameScriptNatives.cpp | full | cancel current + delete queue |
| A_Wait | GameScriptNatives.cpp | full | `ActionWait` in ms |
| A_PlayAnim | GameScriptNatives.cpp | full | `(ent, group, file, loops, reverse)` |
| A_MoveToPosition | GameScriptNatives.cpp | full | **dest is a `float[3]`**, `stopDist` used only if >0 (default 5), 4th arg ignored; speed `value94` (default 10) world units/s, A* on nav grid |
| A_MoveToEntity | GameScriptNatives.cpp | full | `stopDist` stored only if **negative** (quirk); reachability -1 = none |
| A_Wander | GameScriptWanderNative.cpp | full | `(ent, float tl[3], float br[3], repetitions)`; random point in rect, wait 1000..6000 ms, 0 = forever |
| A_Kill | GameScriptKillNative.cpp @5C8E70 | full | ClearActions + die action |
| A_ReloadWeapon | GameScriptReloadNative.cpp @45A380 | full | `GameActionReload` |
| A_Attack | GameScriptAttackNatives.cpp @459EB0 | **engine (large)** | queues `GameActionAttack` (auto-move, rotate). Logic: `GameActionAttack*.cpp`, `GameWeapon*.cpp`, `GameEffectDamage.cpp` (all present, see findings 2) |
| A_JustAttack | same @459F70 | engine | same with `autoMove=0`, `rotateToTarget=0` |
| DestroyEntity | GameScriptEntityLifetimeNatives.cpp @5CA5F0 | full | deferred `Entity::Destroy` (OnDestroy then removal) |
| ActivateEntity / DeactivateEntity | GameScriptEntityActivationNatives.cpp | full | toggle `renderEnabled` (also excludes from FindObjects) |
| SetModel | GameScriptModelNatives.cpp @5C78A0 | full | direct model swap |
| StartDialogue | GameScriptDialogueNatives.cpp @45A040 | full | ClearActions then `GameActionInitiateDialogue` (walk to target, begin script) |
| RegisterReply / RegisterNodeText / SetNodeTextFunction / SetCurrentNodeName | GameScriptDialogueContextNatives.cpp @5C4060/@5C3980/@5C41B0/@5C42A0 | full | see findings 5 (reply keyed by replyID, textID 0 -> STF 184, revision bump on node text function change, node names uppercased) |
| StartStore | GameScriptStoreNative.cpp @5C21A0 | **stub** | validates the string and returns 1; no store UI in the demo |
| OpenTask / CloseTask / AddEntry | GameScriptJournalNatives.cpp | full | two DWORD ids; global journal (state 1 open / 2 closed); AddEntry deduped; broadcast |
| GetNavPointCount / GetNavPointPosition | GameScriptNavigationNatives.cpp | full | owner map EMNP; index check is `> size` (off by one in original) |
| GetWayPointCount / GetWayPointPosition | same | full | owner map EPTH path by exact name; idx>=count -> 0; position as `float[3]` |
| GetNearestPath | same | full | path whose first waypoint is nearest (3D), writes name string |
| AttachSound / RemoveSound | GameScriptSoundNatives.cpp @5C2860 / @5C2940 | full | allocate id, broadcast, return id; Remove returns 1 always |
| AttachVEG / RemoveVEG / GetVEGID | GameScriptVfxNatives.cpp @5C29B0 / @5C2A80 / @5C2AF0 | full | AttachVEG allocates an effect id, broadcasts, returns id; Remove returns 1; GetVEGID exact-name lookup, 0 if none |
| MapEffectSetActive / MapEffectResetElapsedTime | GameScriptNatives.cpp | full | by effect name on the map (EMEF); 1/0 |
| GetPrimaryAttribute | GameScriptCreatureNatives.cpp | full | `baseSpecial[idx]` unclamped; order ST,PE,EN,CH,IN,AG,LK |
| GetTotalSkill | same | full | `GameCreature::GetTotalSkill`, formulas in findings 2.2 |
| float* (8) | src/dependencies/small-script/src/float.c | full | floatdiv by zero returns inf (ours 0); floatround modes 1 floor, 2 ceil, 3 toward zero, default `floor(x+.5)` |

## The natives we leave unbound

| Native | Verdict | Implementation notes |
|---|---|---|
| FindObjects | **done** | circle(XZ) vs object AABB over renderEnabled objects on the owner's map; we already have positions, use radius + AABB half extents (about 1 world unit for critters) |
| GetLOS / GetLOSPoint | **done (grid approximation)** | needs the EMAP height grid (not exported yet) or a simplified blocked-cells Bresenham on our walkability grid (acceptable approximation, scripts only use it as boolean) |
| GetWayPointCount/Position, GetNearestPath, nav points | **done** | EPTH/EMNP chunks, already in the map format docs |
| AttachVEG/RemoveVEG/GetVEGID, AttachSound | VEG **done** (ids), AttachSound stub | server only allocates ids and broadcasts; return the id |
| IsUnderFOW | **done (server model)** | no server-side model; recommended: true when no player in the party can see the cell |
| A_Attack / A_JustAttack | **done** | native is trivial, behaviour = combat model (findings section 2), which our `Game.cpp` combat only approximates |
| StartStore | stub bound | original no-op |
| Skill use, lockpick, repair | **not natives** | `GameActionUseSkill.cpp`, `GameSkillMenu.cpp`, `GameEntityUseSkill.cpp`: engine calls the script public `UseSkill(actor, selector)` and `ShowSkills()/GetUsableSkills()`; the work is in the `.scrb` scripts |

## Verdict

1. **Native layer: yes, fully reimplementable** (Lua or C++) from this source. All 115 natives have real bodies; the only genuinely client-coupled one is `IsUnderFOW`; `FindObjects` ordering needs one runtime check. Estimate: **15-20 working days** including fixing the known mismatches (int returns, vector args, exact-argc, global/journal semantics).
2. **Porting the Pawn scripts to Lua by hand: not feasible.** 119 scripts, ~4.3 MB bytecode (36 `.scra` generated dialogue graphs, 83 `.scrb` mostly near-clones of Template_MOB/DOOR). **Recommendation: keep the `.amx` bytecode and our VM (`Amx.cpp`) and complete the natives**; hand port only selected scripts (5 tutorial ones are already in Lua).
3. Optional: a mechanical AMX -> Lua transpiler (standard Pawn opcodes, Lua 5.4 `goto`) is possible in **10-15 days**, output unreadable; only worthwhile if the VM must be dropped.
