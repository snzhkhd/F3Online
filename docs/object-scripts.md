# Object scripts

Behaviour for placed map objects (computers, terminals, levers, doors, containers), authored as data and, when needed, as a Lua file.
The server owns everything: range, usability, one-shot and cooldown are checked on the server, and state lives in the world database.
The client only gets a cursor label and a name.

Two layers, usable together:

* **Declarative bindings** in `data/objects/*.json`: a list of steps that reuse the dialog condition / effect vocabulary (`docs/dialogs.md`).
* **Lua** in `scripts/objects/<name>.lua`: hooks `on_use`, `on_skill`, `on_look` (plus `can_use`, `on_loaded`) for logic that data cannot express.

Order on every use: `can_use` (Lua) -> script hook -> if the hook returned `true` the steps are skipped, otherwise the JSON steps run.

Files whose name starts with `_` are ignored by the server (templates and parked samples). `data/objects/_samples.json` holds three samples;
rename or copy it to `samples.json` to switch them on (the original tutorial script already drives the Vault 13 computer, hence parked).

## 1. Binding: which object

```json
{ "schema": 1, "kind": "objects", "records": [
  { "id": "vault_computer", "map": "00_04_Tutorial_Vault", "object": "Computer07cDS_Tech_000", "script": "objects/vault_computer.lua" } ] }
```

| Field | Meaning |
| --- | --- |
| `id` | unique id of the binding (letters, digits, `_ . -`) |
| `map` | map name, checked against the loaded maps |
| `object` | the **instance name** of the object on the map. This is the stable key: it survives map edits that move or renumber objects |
| `proto` + `cell` | fallback when the object has no instance name: prototype name and cell `[x, y]`. `proto` alone is accepted too (first match) |

A binding needs `object` or `proto`/`cell`. At server start (and after `/reload`) every binding is resolved to a map object; the log says
`objects: N of M binding(s) bound, K unresolved` and warns for each binding that matches nothing. Unresolved bindings are harmless.

## 2. Binding: behaviour and presentation

| Field | Meaning |
| --- | --- |
| `label` | cursor label, `Use` by default (`Hack`, `Pull`, `Search`, ...) |
| `name`, `desc` | override of the display name / description. Locale keys `object.<id>.name`, `object.<id>.desc`, `object.<id>.label` (in `data/locale/<lang>/*.json`) override the literal text per language |
| `script` | Lua file below `scripts/`, usually `objects/<name>.lua` |
| `usable` | `false`: cannot be used (looking still works) |
| `one_shot` | `true` / `"world"`: can be used once for everyone (a `used` mark in the world state, persisted); `"player"`: once per player (player variable `obj.<id>.used`) |
| `cooldown` | seconds between uses (in memory, reset by a restart) |
| `range` | use range in tiles; `0` / absent: the normal door range (`DoorOpenRange`) |
| `hidden_until` | condition (same syntax as dialog conditions); the object is invisible and unusable until it holds. Re-evaluated when flags change |
| `on_use` / `on_look` | step lists |
| `skills` | map skill name -> step list, run when the player uses that skill on the object |

## 3. Steps

A step is an object with exactly one "kind" key, optionally with `if` (a condition; the step is skipped when false).

| Kind | Fields | Effect |
| --- | --- | --- |
| `dialog` | `dialog` (dialog id) | starts a data dialog spoken by the object; the rest of the list is not run |
| `door` | `door` (instance name or `[x, y]`), `action` (`open close toggle lock unlock`) | operates a door / container of the same map |
| `skill` | `skill`, `difficulty`, `roll`, `reward_xp`, `success`, `failure` | skill check; `roll: true` rolls a percent chance (skill - difficulty), otherwise the skill value must reach the difficulty. Runs the `success` or `failure` list |
| `give` / `take` | item id, `count` | items |
| `quest` | quest id, `state` | quest state |
| `flag` | `flag`, `set` (true / false / number), `world` | player flag, or a world flag with `"world": true` |
| `xp` | amount | experience |
| `message` | text, optional `key` | message to the player (locale key `key` when given) |
| `teleport` | map, `entry` or `cell` | map transition |
| `play` | effect name | calls the Lua hook `object_effect(player, obj, name)`; there is no client effect message yet |
| `call` | Lua function name | global `fn(player, obj)` |
| `default_use` | `true` | the engine's own door / container use |
| `end` | `true` | stops the list |

Flag, quest and item conditions are the ones of `docs/dialogs.md` (`skill:Science>=50`, `flag:name==1`, `quest:id==completed`, `{ "any": [...] }`, ...).

Example, a box that needs Science and opens a door:

```json
{ "id": "t_box", "map": "00_04_Tutorial_Vault", "object": "Box_BlueDS_Vault_000", "label": "Search",
  "skills": { "Science": [ { "skill": "Science", "difficulty": 50, "roll": true, "reward_xp": 5,
      "success": [ { "message": "Opened." }, { "flag": "box_hacked" }, { "door": "Door01Vault13_005", "action": "open" } ],
      "failure": [ { "message": "Nothing happens." } ] } ] } }
```

## 4. Lua API

```lua
function on_use(player, obj)            -- return true when handled (the steps are skipped)
function on_skill(player, obj, skill)   -- skill: English name, e.g. "Science"; false = default handling (lockpicking, ...)
function on_look(player, obj)
function can_use(player, obj)           -- false refuses before anything else (tell the player why)
function on_loaded(obj)                 -- after binding (start, /reload)
function object_effect(player, obj, name)   -- global hook for the `play` step
```

Each script file runs in its own environment (names do not clash). Keep the top level to function definitions.

`obj` is a MapObject handle: properties `id valid name display_name binding map proto kind x y is_open is_locked`;
methods `get_flag(name)`, `set_flag(name, number)` (persistent per object), `used()`, `set_used(bool)`, `open() close() toggle() lock() unlock()`,
`start_dialog(player, dialog_id)`.

Globals: `object_by_name(map, name)`, `object_by_id(id)`, `bound_object(binding_id)`, `start_dialog(player, dialog_id, obj)`,
`skill_check(player, "Science", difficulty [, roll])`, `objects()` (entries carry `inst` and `binding`), `spawn_item_proto(map, key, x, y, count, name)`,
plus the normal player API (`player:tell`, `give`, `set_gvar`, ...). Samples: `scripts/objects/vault_computer.lua` (dialog on use; a Science check unlocks and opens the overseer door),
`scripts/objects/vault_lever.lua` (one-shot lever). Template: `scripts/objects/_template.lua`.

Chat commands: `/objs` lists the bound objects within 12 cells; with `--dev-kit` `/useobj <object id>` uses an object from where you stand (server range check, no walking).

## 5. Persistence and reload

* State is the SQLite table `object_state` (migration v2): per map and object key (instance name, else `proto@x,y`) a JSON `{used, flags}`.
  It is saved with the rest of the world state and restored at start. One-shot marks and object flags survive restarts.
* `/reload` re-reads `data/objects/` and the scripts, re-binds, and keeps the state. A removed binding keeps its stored state.
* The cooldown is not persisted.

## 6. Editor

`f3_server --editor` -> **Object scripts** (`docs/editor.md`, section 7): the bindings list, a form for the fields above, and the step builders
(the same condition builder as dialogs, nested success / failure lists, a door picker). The **object picker** lists the objects of a loaded map
(id, name, model, kind, position; search box) with a 2D scatter plot: click a dot or a row to fill `map` + `object`. **Create Lua script from template**
writes `scripts/objects/<id>.lua` when it does not exist (never overwrites), shows the path and a `vscode://` link, and the script status line shows live whether the file
exists and loads and which hooks it defines. The object list needs the running server (`/api/objects`); the rest of the editor does not.

## 7. Validation

`--validate-data` and the editor check: unknown fields, missing `map`, a binding without `object` / `proto`, unknown map / dialog / item / quest / skill, door actions,
bad cells, oversized step lists, a `script` path that is not a relative `.lua` path, and duplicate ids. Whether the script file exists and loads is shown live by the editor (`/api/objscript`) and logged by the server at load.

## 8. Known gaps

* `play` has no client effect: only the Lua hook `object_effect` runs.
* The cursor label is sent per object; a change of `label` or `name` at `/reload` reaches clients that re-enter the map.
* The object list in the editor reflects the maps loaded by the running server; bindings of unloaded maps cannot be picked.
* Object scripts replace nothing in the original AMX scripts: where an original script already drives an object (the Vault 13 computer), bind only when you want to override it.
* The cooldown is in memory only; per-player one-shot state needs a player variable (it is saved with the character).
