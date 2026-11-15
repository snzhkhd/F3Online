# Van Buren decompilation (`_work/vbdecomp-main`): overview

Research note. Source studied: `F:\_work\F3Online\_work\vbdecomp-main` (git-ignored, never committed; `_work/` is in `.gitignore`).
Everything below was read statically; nothing from the decompilation was built or run.
All paths below are relative to `_work/vbdecomp-main/` unless stated otherwise.

## 1. What it is

It is **not** a raw IDA/Ghidra dump. It is a hand-curated *source reconstruction* of the Fallout: Van Buren tech demo (`F3.exe`, base
`0x400000`, SHA256 `4652a3d0...b3579b87`) that is meant to recompile with the original toolchain (VC7 13.00.9466, C++98, Nov 2001 Platform SDK,
DirectX 8.1). It builds on the community projects OpenJE/openvb (game) and OpenJE/openje (engine) and a fork of Small/Pawn
(`kran27/small-script`).

* README: "Out of 10,110 original functions, 9,949 (98.4%) have been attributed to their original source. 5,242 are specific game/engine
  functions, 4,662 have bodies, but are not guaranteed accurate." At least 3,447 game/engine functions (65.8%) have historical exact-match
  receipts (byte-for-byte VC7 compiler output), "not revalidated after the latest cleanup". The rest: 4,115 library functions (CRT/STL/DirectX
  headers etc.), 592 compiler glue functions, 161 of unresolved origin.
* `AGENTS.md`/`docs/matching-policy.md`/`docs/reporting.md`: "preserve original bugs", no invented success paths, unknown layouts stay explicit.
  Consequence for us: **semantic quirks are preserved as-is** (e.g. off-by-one `index > size`, `IncGlobal` sharing `DecGlobal`'s diagnostics,
  integer truncation of floats); the code is a faithful but ugly behaviour reference, not clean design.
* There is no game data in it (`deps/game` is untracked and absent). Only source, docs and tooling.
* Static-analysis databases (IDA) are referenced by `scripts/Get-AnalysisDatabase.ps1` but not shipped.

## 2. Naming status: named, with raw offsets where layout is unknown

* Classes, methods, enums, locals and most fields are **named** (`GameCreature::GetTotalSkill`, `GameActionAttack::BuildSubAttacks`,
  `DialogueManager::SelectReply`, `World::UpdateServerEntities`, ...). Native functions are named `Native<Script name>`.
* Almost every function carries its original address in a trailing comment (`//5C2860.`): about 6,000 such comments. This lets us cross-reference
  with an `F3.exe` database (we already use FUN_xxxxxxxx names in `docs/formats/maps.md`).
* Fields of unknown meaning keep raw names: `value160`, `value29C`, `value368`, `field0x50`, `valueD8` (about 3,000 occurrences). Examples:
  `Entity::value368` = team id, `value36C` = team rank, `value29C` = per-entity time offset, `GameItem::value160` = item type,
  `GameCreature::value7FC` = evasion (derived attribute 4). Where we needed them they are decoded in `vbdecomp-findings.md`.
* Messages/diagnostic strings are intact and very informative (every native has its error text: "GetLOSPoint() is unable to find the source entity.").

## 3. Module / file map

Counts are `.cpp/.hpp/.h/.c` files and lines.

| Path | Files / lines | Content | State |
|---|---:|---|---|
| `repos/openvb/src/game` | 338 / 31.4k | **F3 game layer**: `GameCreature/Weapon/Armor/Ammo/Item/Door/Container/UsableObject`, actions (`GameActionAttack*`, `GameActionUseSkill`, `...MoveTo`, `...Wander`, `...Reload`, ...), `GameEffectDamage`, `GameInventory`, `GameWorld`, journal/Pip-Boy, character generation, all GUI screens (`ActionbarInterface`, `InventoryInterface`, `StoreCatalogueScreen`, `WorldMapInterface`, ...), and **all 115+ script natives** (`GameScript*.cpp`) | named, bodies present for nearly everything |
| `repos/openvb/src/startup` | 97 / 5.8k | `GameServer` (journal/HP/move-mode/weather packets), `GameClient`, console commands, `Random*`, `FrameLookupCaches` (script list handles), settings, `GameScriptingComponent` | named |
| `repos/openvb/src/F3` | 59 / 2.9k | upstream OpenJE header stubs of the same classes | headers only |
| `repos/openvb/openje/src/entity` | 144 / 14.3k | **JE engine world layer**: `World`, `Map`, `Entity` (+ actions, effects, messages, scripts, schedule), `Inventory`, `Store`/`CStore`, `Combat`, `Gregorian` calendar, `TeamSystem/SquadSystem`, `Trigger`, `PathManager`/`PathSearch` (A*), `MapGrid`, `MapFog`/`AreaMap` (LOS height grid), `MapNavigation` (nav points, waypoint paths), `EffectTree*`, `VisualEffect` | named |
| `repos/openvb/openje/src/network` | 89 / 7.3k | `Server`/`Client` (`EngineServer*.cpp`, `EngineClient*.cpp`), packet ids (`packet.field0x30 = N`), `DialogueManager*` (the dialogue state machine), teams/vfx/sound/damage-text/party/map-transfer publishing | named |
| `repos/openvb/openje/src/scripting` | 6 / 1.2k | `Script_Small` (AMX wrapper: Load, Run, Execv, `PreLoad` of public variables), `ScriptEnvironment`, in-game script debugger | named |
| `repos/openvb/openje/src/graphics` | 408 / 56k | D3D8 renderer, **Octree** (static scene + dynamic-object spatial queries + shapes), animation instances, camera, lighting, VFX (`.veg`) | named, large |
| `repos/openvb/openje/src/gui` | 158 / 11.8k | window toolkit, dialogue window, console, edit, etc. | named |
| `repos/openvb/openje/src/resources`, `model`, `audio`, `strings`, `configuration`, `filesystem`, `diagnostics` | ~110 / 11k | resource system (group files `.grp`, extension -> type table in `ResourceFormats.cpp`), b3d/gr2 models, ACM sound, `StringSystem` (`English.stf`, `{token}` substitution), `.ini` parsing | named |
| `repos/openvb/openje/src/je` | 984 / 32.7k | upstream OpenJE public headers (class layouts, virtual tables) | headers |
| `src/dependencies/small-script/src` | 31 / 22k | **Pawn/Small AMX runtime and compiler** (`amx.c`, `float.c`, `amxcore.c`, `sc*.c` compiler, `sdbg.c`), F3 runtime adaptations; `float_Natives` live in `float.c` | vendored fork |
| `src/dependencies/zlib-1.1.4` | 25 / 8k | zlib | vendored |
| `config/`, `scripts/`, `tools/` | - | Windows build/setup scripts (PowerShell/Python), toolchain pins, `agent-templates/*.md` | tooling only |

Top-level docs: `README.md` (state), `AGENTS.md` (rules), `docs/build.md`, `docs/dependency-fidelity.md`, `docs/matching-policy.md`.

## 4. Subsystem coverage as seen from our project's needs

| Subsystem | Where | Completeness |
|---|---|---|
| Script natives (155 engine + 45 game table entries; the 115 we know are a subset) | `src/game/GameScript*.cpp`, tables in `GameScriptNatives.cpp` (`engineNatives`@6FFAF8, `gameNatives`@6F3750) | all have bodies, see `vbdecomp-script-natives.md` |
| Script host (VM wrapper, public calls, message queue, queued functions) | `openje/src/scripting/ScriptSmall.cpp`, `entity/EntityScript.cpp`, `EntityMessages.cpp`, `EntitySchedule.cpp`, `EntityUpdate.cpp` | complete |
| Dialogue engine (server) | `openje/src/network/DialogueManager*.cpp` | complete (multi-party, revisions) |
| Combat (to-hit, damage, burst/cone targeting, AP budget, hit parts, effect) | `src/game/GameActionAttack*.cpp`, `GameWeapon*.cpp`, `GameCreature*.cpp`, `GameEffectDamage.cpp`, `openje/src/entity/Combat.cpp` | complete formulas |
| Stats (SPECIAL, 13 skills, derived attributes, tags/traits/perks) | `GameCreatureStats.cpp`, `GameCreatureActionPoints.cpp` | complete; **no XP/level-up system exists in the demo** |
| Entity/map/template loaders | `GameCreature::Read*`, `GameWeapon::Read*`, `GameItem/Armor/Ammo/Door/Container/UsableObject::Read*`, `openje/src/entity/Map*Read*.cpp`, `Trigger.cpp` | complete (chunk-level field order) |
| Pathfinding / movement | `openje/src/entity/Path*.cpp`, `ActionMoveTo.cpp`, `GameActionMoveTo.cpp` | complete (8-neighbour A* on a grid) |
| LOS / fog of war | `openje/src/entity/EntitySight.cpp`, `AreaMapSight.cpp`, `MapFog*.cpp` | complete (height-grid raycast) |
| Stores | `openje/src/entity/Store.cpp` (`CStore` file reader), `src/game/StoreCatalogueScreen.cpp` | data class only; the `StartStore` native itself does nothing in the demo |
| AI behaviours | **not in the engine**: they are in the `.scrb` Pawn scripts (Template_MOB etc.) | n/a (see verdict) |
| Renderer, audio, GUI | `openje/src/graphics`, `audio`, `gui` | large, not reviewed in depth |

## 5. Caveats

* "Has a body" is not "verified": per `reporting.md` source presence is not a match. Spot checks (see findings) agree with our own data/script
  analysis in most places, and disagree with several of our earlier guesses (listed in `vbdecomp-findings.md` section 1).
* Many bodies call helper methods that are themselves reconstructed. Only a handful of leaf boundaries are marked unresolved (`OctreeUnresolved.cpp`
  is empty; a few `throw std::logic_error("...original ... unresolved")` guards on out-of-domain table indices).
* Numbers are in **world units** (`float` positions, script `Distance`/ranges/radii), not our 0.5-unit grid cells (1 world unit = 2 of our
  cells). The fog/LOS `AreaMap` maps world x/z to a `width x height` bit grid by bounding box (`AreaMap::WorldToCell`); `GetCellSpacing()`
  returns the constant 2.0f.
* Network packet numbers in this source (`packet.field0x30 = 22`...) are the original F3 protocol; irrelevant to our own protocol.
