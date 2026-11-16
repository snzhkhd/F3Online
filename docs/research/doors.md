# Doors and containers: the original model and how F3Online reproduces it

Sources: `_work/vbdecomp-main/repos/openvb/src/game` (GameDoor, GameContainer, path cost, Operate), the GDOR/GCON blobs in
`protos.json` (`gdor_hex`, `gcon_hex`), the AMX natives `LockDoor / UnlockDoor / IsLocked / GetLockDC`, STF messages.

## Original behaviour

* **State**: `open`, `locked`, `keyResource` (string), `lockDC` (valueC0). A door is Open, Closed or Locked (closed + locked).
* **Data** (GDOR 12 bytes in the shipped data, GCON 14): `u8 open, u8 locked, u16 len + key string, i32 lockDC, i32 randomRange`
  (GCON has one more trailing string). In the shipped data every `lockDC` is 0, only `Door02` is locked, key strings are empty.
* **Collision / pathing / LOS**: a closed door occupies its footprint grid (the `0x1b` doorway cells); opening removes it, so
  walking, projectiles and line of sight follow the state. Path cost of a door the walker cannot operate is +32765 (effectively a
  wall); a walker that can operate it (unlocked, or has the key) paths through.
* **Who opens doors by walking** (`GameActionMoveTo::OnMoved`, decompiled): when the walker's footprint meets a closed door and the route crosses the door plane, the
  operate-door action (`GameActionOperateDoor`, Wait 1000, MoveTo retry) is queued only `if (!g_World->party->Contains(owner))`. So **NPCs / AI / AMX-driven
  movement open doors on their path (and wait about a second), but the player and party members do not**: they just stop (`StopMoving`, no retry) in front of the door
  and open it with a click. Locked doors: `GameDoor::CanOperate` (locked -> inventory has `keyResource`) feeds the +32765 path cost, so a locked door is avoided
  unless it is the only way; the original passes the *door* instead of the walker to `CanOperate` (decompiled quirk), F3Online checks the walker.
* **Operate** order: the script veto (`OnOpen` / `OnClose`, result 0 refuses) runs before the lock check; a locked door opens only
  if the actor carries the key (the key is not consumed). Messages: STF 1299 (locked), 1358 (needs the %s key), 672 (unlocked).
* **Containers** toggle open with no lock logic in the original; loot only when open. Natives: `LockDoor`, `UnlockDoor`, `IsLocked`
  (doors only), `GetLockDC`.
* **Animation**: PlayAnimation 514, clip `<model>_Open_In_01` on the door skeleton (the slab drops into the floor).

## The bug

Unlocked doors never blocked anything and were opened only by a server proximity timer, so the closed art was shown while the
cells were free. Doors without a skeleton were hidden when open, and the client only synced door state for items that were
drawn (not fogged). Result: "closed" doors you can walk through, doors that vanish.

## F3Online implementation

* **Server owns the state** (`Item`: `open, locked, lockFlags, lockDC, keyId, contents`). `engine/server/Doors.cpp` holds
  Operate, Lockpick, use, passability and persistence.
* **Flags** (`LockFlag`): `NoOpen 1` (cannot be opened), `NoPick 2` (cannot be picked), `Jammed 4`, `KeyOnly 8` (key required, no pick).
* **Blocking**: a closed door blocks its footprint cells in `Map::blocked` (also LOS); locked doors block like walls.
  Pathfinding passes through closed doors the walker can operate. NPCs (native AI, Lua `move_to`, AMX `A_MoveTo*`, wander, combat chase: all go through
  `Server::StartMove`) open them along the path (`OpenDoorsAhead`, `OpenDoorAt`) when they may (unlocked, or the key item is in their inventory); locked doors without the
  key block them like walls. **Players do not open doors by walking** (config `PlayerAutoOpenDoors = 0`, like the original): the walk stops in front of the door, a click
  (`C2S_Use`) opens it. `PlayerAutoOpenDoors = 1` restores the old auto-open. No auto-close.
* **Operate order**: key unlock, then the AMX `OnOpen` veto (the lock is restored on veto), then the Lua `door_use` hook.
* **Lockpick**: `UseSkill(Security)` on a locked door/container; chance `clamp(Security - lockDC, LockpickMinChance, LockpickMaxChance)`,
  optional kit (`LockpickNeedsKit`), XP (`LockpickXp`), `ForceLockRoll` for tests. Hook `lockpick_chance`.
* **Keys**: inventory items whose proto matches `keyId`; not consumed.
* **Containers** share the model; loot requires `open && !locked`.
* **Persistence**: `<saves>/world/<map>.json`, key `protoKey@x,y`; containers store contents. Saved on shutdown and periodically.
* **Protocol** (backward compatible): `ObjectInfo` gains trailing `u8 locked, u8 flags` (readers tolerate their absence);
  new `C2S_CloseObj = 40` (server dispatch range 20..47).
* **Client**: state from ObjectInfo for all known doors (`SyncDoors`), rigged doors play `Open_In_01`, skeleton-less doors sink
  0.9 s and are not drawn when fully sunk, cursor label `[Open] / [Close] / [Locked]` (`ObjectActionLabel`), `--autouse` for tests.
* **Lua**: `get_lock, set_lock, operate_object, set_lock_roll, save_world`, `LOCK_*` constants, hooks `door_use`, `lock_changed`,
  `lockpick_chance`; dev commands `/doors /lock /lockroll /saveworld /give`, `/tp [map] x y`. AMX: `GetLockDC`.
* **Config** (`scripts/config.lua`): `LockDefaultDC, LockpickMinChance, LockpickMaxChance, LockpickNeedsKit, LockpickXp, ForceLockRoll, DoorOpenRange`.
* **Tests**: `f3_gametest --doors 1|2` (player stops at a closed door, an NPC (dev `/npcspawn /npcgo /npckey`) walks through an unlocked one, is blocked by a locked one, passes with the key, AMX `A_MoveToPosition` too; state transitions, locked door blocks path, lockpick success and failure, key, container
  lock, persistence across a server restart); part 4 of `engine/tests/smoke_test.ps1`.

## Known gaps

No per-instance GDOR overrides from maps; no auto-close; containers have no open clip; Junktown doors have no footprint so they never
block; LOS has no dedicated test (it uses the same blocked grid); `LockpickNeedsKit` is off by default.
