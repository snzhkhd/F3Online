# F3Online architecture and build guide

Authoritative server + thin client + Lua scripts, in the spirit of FOnline.

## Layout

| Path | Contents |
|---|---|
| `engine/common` | `protocol.h` (message ids + structs), `buffer.h` (bounds-checked LE serializer), `types.h` (tile coords, dirs), `lua_bind.h` (sandboxed sol2 state, safe hook calls), `log.h` |
| `engine/server` | `f3_server`: `World` (Item/Critter/Map + A*), `Server` (ENet, 20 Hz tick, login, validated movement, chat, visibility), `Scripting` (sol2 API + hooks + hot reload) |
| `engine/client` | `ClientCore` (net + replicated state, no rendering), `IRenderer` + `RaylibRenderer` (placeholder), `f3_client` (raylib), `f3_testclient` (headless bot) |
| `scripts/` | `main.lua`, `npc/*.lua`, `maps/test_map.lua` |
| `engine/tests/smoke_test.ps1` | starts server + bot, checks round trips (also `ctest`) |

## Build (Windows, VS 2022, CMake >= 3.20)

```
cmake --preset vs2022
cmake --build build/vs2022 --config Release
ctest --test-dir build/vs2022 -C Release      # smoke test
```
Binaries land in `build/vs2022/bin`. `cmake --preset vs2022-server-only` skips raylib.
Dependencies are pinned and fetched at configure time: ENet 1.3.18, Lua 5.4.6 (built from source),
sol2 3.3.0. raylib 5.5 is taken from the local prebuilt copy `F3_RAYLIB_DIR` (default `F:/RayLib/raylib-5.5`,
static MSVC `/MD` build, so use Release for the client); if absent it is fetched from GitHub.

## Run

```
build\vs2022\bin\f3_server.exe [--port 4040] [--scripts DIR] [--tickrate 20] [--run-seconds N]
build\vs2022\bin\f3_client.exe --host 127.0.0.1 --port 4040 --name Alice
```
Server console (stdin): `reload` (hot-reload Lua), `status`, `say <text>`, `quit`.
Client: left click = move, Enter = chat, mouse wheel = zoom. Chat commands from scripts: `/help /who /pos /steps`.

## Protocol

UDP via ENet, channel 0 reliable, packet = `u8 id` + payload (see `protocol.h`).
Client to server: `Login{version,name}`, `MoveTo{x,y}`, `Chat{text}`, `Ping`.
Server to client: `LoginOk`, `LoginFail`, `MapInfo` (size + blocked bitmap), `CritterAdd/Remove/Move`, `ItemAdd/Remove`, `Chat`, `Pong`.
The client never sends positions, only intent; the server pathfinds (A*), steps one tile per
`moveTicks` ticks (default 4 = 5 tiles/s), validates every step (bounds, walls, corner cutting, occupancy)
and broadcasts `CritterMove`. Requests are rate limited (token budget), strings sanitized, parse errors disconnect.

Visibility: per map, each player sees critters/items within `kViewRadius` (24 tiles, Chebyshev).
After each tick on a dirty map the server diffs each player's known set and sends Add/Remove;
moves go only to players that already know the mover. The client interpolates between tiles over `moveTicks/tickRate`.

## Scripting (Lua 5.4 + sol2)

Sandbox: base/string/table/math/utf8/coroutine only; `load_script("rel/path.lua")` replaces dofile (relative to `scripts/`, no `..`).
Hooks: `on_init`, `on_reload`, `on_login(c)` (return false to refuse), `on_logout(c)`, `on_critter_move(c,fx,fy)`,
`on_chat(c,text)` (false = swallow, string = replace), `on_tick(t)`.
NPC behaviours: `npc_types.NAME = { on_tick(npc,t), on_heard(npc,speaker,text) }` (`on_heard` fires for player speech only).
Globals: `create_map{name,width,height,rows,spawn}`, `spawn_npc(map,name,x,y,type[,color])`, `spawn_item`, `get_critter`, `find_player`,
`get_map`, `get_map_by_name`, `players()`, `say`, `tell`, `broadcast`, `remove_critter`, `tick()`, `log`, `TICK_RATE`.
Critter: `id name x y dir map_id npc_type is_npc() is_player() is_moving() move_to(x,y) stop() teleport(map,x,y) say() tell() distance_to() get_var/set_var`.
Map: `id name width height spawn_x spawn_y is_blocked() is_free() critters()`.
Hot reload builds a fresh Lua state and swaps it only if `main.lua` loads; world data (maps, critters, vars) lives in C++ and survives.
Critter/Map objects are raw handles: do not keep them across ticks, keep ids and call `get_critter(id)`.

## Renderer seam

`f3_client` main loop only uses `IRenderer` (`Init/PollInput/BeginFrame/DrawScene/DrawUi/EndFrame/ScreenToTile`) with plain
`SceneView`/`UiView` snapshots. A future renderer loading the original game assets (TGA/DDS tiles, GR2 models) implements
the same interface; networking and state do not change.

## Known gaps

- Square 8-direction tile grid, not hex (isolated in `types.h` and `FindPath`).
- No client-side prediction; no login timeout for idle connections; no persistence/accounts/passwords.
- Single map entry point; one map in sample content; no map transitions from scripts besides `teleport`.
- Items are visible markers only (no pickup/inventory); no combat, stats, or dialogue system.
- Visibility diff is O(players x critters) per dirty map; fine for MVP scale.
- Server-only console is stdin; no remote admin. Scripts are one global Lua state (no per-script sandbox quotas/instruction limits).
- Client GUI was launched but visually verified only minimally; automated smoke test is headless.
