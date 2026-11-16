# Data-driven content (`data/`)

Designers edit traits, perks, skills, items, NPCs and quests in plain JSON files under `data/`; the server loads, validates and
applies them at startup and again on `/reload`. Everything in `data/` is authored by us and tracked in git. The original game files
(`extracted/`, `gamedata/`, `_work/`, saves) never go there.

```
data/
  manifest.json          {"schema": 1, "name": ..., "description": ...}   (required, marks the folder and its schema version)
  skills/*.json          base-value formulas of the 14 skills
  traits/*.json          the trait list of the creation screen (+ max picks) and trait effects
  perks/*.json           perk definitions: requirements, effects, optional Lua hook
  items/*.json           overrides of / additions to the item prototypes (weapons, armor, ammo, misc)
  npcs/*.json            overrides of / additions to the critter prototypes (+ spawn defaults)
  quests/*.json          quest definitions for the Pip-Boy log (title, text, rewards)
```

Files are read in file-name order. Files whose name starts with `_` or `.` (or that do not end in `.json`) are ignored, which is
handy to park an example or disable a file. `//` and `/* */` comments are allowed inside the JSON files.

## 1. Layers and merging

```
 base layer      original prototypes of gamedata/server/protos.json (f3_dataimport output; never edited)
   + data layer  records of data/items and data/npcs                (merged by id)
   = effective   what the server uses
```

* A record whose `id` is the key of an existing prototype (`Weapons9mm_Autoloader.WEA`, case-insensitive) **overrides** it: only
  the fields written in the record change, everything else keeps the original value. List fields (`inventory`, `equipment`,
  `armor.values`) are replaced as a whole.
* A record with `"base": "<existing id>"` and a **new** `id` **adds** a prototype: it starts as a copy of the original prototype
  `base`, then the record's fields are applied. New prototypes get ids after the original ones and keep them for the life of the
  process (also across `/reload`, also if the record is removed and comes back).
* A record that names an unknown id without `base` is an error (that is how typos are caught); `base` with an id that already
  exists is an error too.
* Items and NPCs share one id namespace (prototype keys); an id can only appear once in `data/`.
* The other kinds (traits, perks, quests, skills) have no base layer: their ids are defined here (skills: the 14 names are fixed,
  the file overrides the built-in formula, which is the one the server used before the data layer existed).
* Removing a record, or deleting its file, and reloading brings the base value back. Instances that already exist (items on the
  ground, in inventories, spawned NPCs) keep working; names and numbers are looked up from the prototype when they are used, so they
  follow the data, while values copied at spawn time (an NPC's HP, team, inventory) keep their old value until the next spawn.

## 2. Envelope, schema version

Every file in a kind folder has the same envelope:

```json
{
  "schema": 1,
  "kind": "perks",
  "records": [ ... ]
}
```

| field | meaning |
|---|---|
| `schema` | integer, required. This build understands `1`. A newer number is rejected with a clear message, never half-read. |
| `kind` | required, must equal the folder name (catches files dropped in the wrong folder) |
| `records` | array of record objects, required (may be empty) |
| `max_picks` | `traits` files only, see 3.2 |

`manifest.json` carries its own `schema` (the schema of the folder as a whole). Rules for changing the format: adding an optional
field keeps the schema; renaming, removing or re-typing a field bumps it, and the loader keeps reading the old schemas.

Strictness: **unknown fields are errors** (typo detection), wrong types and out of range values are errors, ids must be unique
(case-insensitive) within their namespace.

## 3. Kinds

Ids of traits / perks / quests: letters, digits, `_ . -`, at most 64 characters. Strings have length limits (descriptions 800).

### 3.1 skills

```json
{ "schema": 1, "kind": "skills", "records": [
  { "id": "Firearms", "desc": "Pistols, rifles and everything that shoots.",
    "base": { "const": 5, "agility": 4 } }
] }
```
`id` one of Firearms, Melee, Unarmed, Barter, Persuasion, Deception, Mechanics, Medic, Outdoorsman, Science, Security, Sneak, Steal,
Traps. `base`: skill base value = `const + sum(factor * SPECIAL)`, keys `const`, `strength`, `perception`, `endurance`,
`charisma`, `intelligence`, `agility`, `luck`; omitted keys are 0. If `base` is present it replaces the whole formula. The tag bonus,
spent skill points and effects come on top. `data/skills/skills.json` lists all fourteen formulas as shipped.

### 3.2 traits

```json
{ "schema": 1, "kind": "traits", "max_picks": 2, "records": [
  { "id": "gifted", "index": 6, "name": "Gifted", "desc": "...",
    "effects": [ { "stat": "skill.Melee", "add": 5 } ], "hook": "optional_lua_name" }
] }
```
* `index` (0..15, unique) is the bit of the character's trait mask and the slot of the creation screen. It is what is stored in
  saved characters, so never renumber an existing trait.
* `max_picks`: how many traits may be chosen at creation (0..16). It overrides `TraitMax` of `scripts/config.lua`; when no traits
  file sets it, `TraitMax` (default 2) is used. Two files must not disagree.
* The server rejects a creation request with a trait bit that has no definition (as soon as any trait is defined).
* `effects` and `hook`: see 4. The shipped 16 traits (the original list) have **no effects yet**, which keeps the game exactly as
  before; their descriptions say "(no effect yet)" until a designer gives them one.

### 3.3 perks

```json
{ "schema": 1, "kind": "perks", "records": [
  { "id": "thick_skin", "name": "Thick Skin", "desc": "...", "ranks": 2,
    "requires": { "level": 6, "special": { "endurance": 6 }, "skills": { "Melee": 30 },
                  "perks": ["toughness"], "traits": ["gifted"] },
    "effects": [ { "stat": "armor_class", "add": 5 } ],
    "hook": "perk_thick_skin" }
] }
```
| field | meaning |
|---|---|
| `ranks` | 1..10 (default 1); effects are applied once per rank |
| `requires.level` | minimum level |
| `requires.special` | minimum SPECIAL values (`strength` ... `luck`, 1..10) |
| `requires.skills` | minimum total skill values by skill name |
| `requires.perks` / `requires.traits` | perk / trait ids that must be held (references are checked; cycles are errors) |
| `effects` | declarative stat modifiers, see 4 |
| `hook` | optional name of a Lua function in `scripts/`, called as `fn(critter, perk_id, rank)` right after the perk is taken |

A character earns a perk point every `PerkRate` levels (`config.lua`, default 3). Spend flow: `/perks` lists every perk with
`[*]` taken, `[+]` available now, `[ ]` locked (with the reason), `/perk take <id>` spends a point. Perks are stored per character
(`perks` object in the save, id -> rank). Lua: `c:perk_rank(id)`, `c:take_perk(id)`, `c:has_trait(id)`, `perk_info(id)`.

### 3.4 items

```json
{ "schema": 1, "kind": "items", "records": [
  { "id": "Weapons9mm_Autoloader.WEA", "name": "Service Pistol", "desc": "Reliable.", "weight": 3,
    "weapon": { "range": 18, "dmg_min": 7, "dmg_max": 13 } },
  { "id": "Armor_Leather.ARM", "armor": { "values": [4,4,4,0,8,4,0] }, "head_armor": false },
  { "id": "Ammo9mm.AMO", "ammo": { "dmg_min": 6, "dmg_max": 14 } },
  { "id": "F3_Pistol_Mk2.WEA", "base": "Weapons9mm_Autoloader.WEA", "name": "Pistol Mk2", "weapon": { "dmg_max": 20 } }
] }
```
Fields (all optional except `id`): `base` (new record, see 1), `name`, `desc`, `weight` (0..10000; wins over `ItemWeights` of
`config.lua`), `weapon{range, dmg_min, dmg_max}` (weapons only), `armor{values[7]}` and `head_armor` (armor only), `ammo{dmg_min,
dmg_max}` (ammunition only). A group for the wrong class of item is an error. `id` must not name a critter (use `npcs`). A new
item clones the **original** prototype `base` (model, icon and the rest), independent of the order of records.

### 3.5 npcs

```json
{ "schema": 1, "kind": "npcs", "records": [
  { "id": "Critters00_TMBobby.CRT", "name": "Bobby", "team": 3, "level": 4, "max_hp": 77, "xp": 250,
    "special": { "strength": 8 }, "script": "...", "dialog": "...",
    "equipment": ["Armor_Leather.ARM"], "inventory": ["Weapons9mm_Autoloader.WEA"],
    "spawns": [ { "map": "00_03_Tutorial_Junktown", "cell": [120, 80], "count": 2 } ] }
] }
```
Fields: `base` (new record), `name`, `team` (0..255), `level` (1..99, the starting level of the HP formula), `max_hp` (fixes the
maximum HP), `xp` (experience for killing it; replaces the `XpByProto` table of `config.lua`, which still works as a fallback),
`special{...}` (partial: the omitted SPECIAL values keep the original), `script`, `dialog`, `equipment` (worn on spawn),
`inventory` (carried / loot), `spawns` (see below). Every item in `equipment` / `inventory` must exist (checked). Map instances that
carry their own inventory in the original map data keep it; the NPC `inventory` is the template default the instances fall back to.

`spawns` places extra copies at server start: `map` must be a loaded map (checked), `cell` is `[x, y]` in cells, `count` 1..50.
Spawns are executed once at startup; `/reload` does not respawn.

### 3.6 quests

```json
{ "schema": 1, "kind": "quests", "records": [
  { "id": "demo_first_steps", "title": "First steps", "text": "...", "xp": 100,
    "items": [ { "item": "Junk.ITM", "count": 3 } ] }
] }
```
Lua `c:quest_start(id)` adds the task (title and text from the record) to the Pip-Boy; `c:quest_done(id)` pays the rewards (xp and
`items`, each item must exist) once per character. Quests that only exist in Lua (`quest_add`) are unchanged.

## 4. Stat modifiers (`effects`)

`{ "stat": "<name>", "add": <integer> }`, summed over all traits and perks of a character (perk effects times rank). An unknown stat is
an error that lists the valid names.

| stat | effect |
|---|---|
| `max_hp` | maximum hit points |
| `max_ap` | action points (whole AP) |
| `carry_weight` | carry limit |
| `armor_class` | armor class shown / used for hit chances |
| `sequence` | sequence |
| `crit_chance` | critical hit chance in percent points |
| `melee_damage` | melee damage bonus |
| `xp_gain` | percent more experience from everything |
| `skill_rate` | skill points per level |
| `heal_rate` | healing rate (character sheet) |
| `skill.<Name>` | total value of that skill, e.g. `skill.Firearms` |

SPECIAL modifiers are not supported yet (many derived values read the raw SPECIAL).

## 5. Validation

At startup the whole folder is validated; **any error stops the server** (exit code 1) unless it is started with `--lenient`, in
which case every error is logged as a warning and the offending records are dropped. Every message names the file and the path of
the problem, for example:

```
data/perks/perks.json: records[3].requires.level: expected integer, got string
data/perks/perks.json: perk 'needs_ghost'.requires.perks: unknown perk 'ghost_perk'
data/npcs/npcs.json: records[0].inventory[1]: unknown item 'Ghost.ITM'
data/perks/b.json: records[0].id: duplicate id 'X' (first defined in data/perks/a.json records[0])
```
Checked: JSON syntax, envelope and schema version, types and ranges, unknown fields, duplicate ids, unknown base / override ids,
unknown items in loot / equipment / quest rewards, unknown maps in spawns, perk and trait references, perk requirement cycles, trait
index collisions, stat names, Lua hook names (syntax only). Without game data (`--no-data`) only the structure and the references
between data files can be checked; item / npc records are then not applied and a warning says so.

Command line (`f3_server`):

| flag | meaning |
|---|---|
| `--content DIR` | the data folder (default: the nearest `data/` next to the binary or in a parent of the cwd) |
| `--lenient` | errors become warnings, bad records are dropped |
| `--validate-data` | validate `data/` (against the gamedata when present, `--no-data` to skip it), print the report, exit 0 (ok) / 1 (errors) / 2 (setup problem), do not start the server |
| `--dev-commands` | allow `/reload` from player chat (implied by `--dev-kit`) |

`engine/tests/smoke_test.ps1` runs `f3_datatest` (loader unit tests on a synthetic gamedata), `--validate-data` on `data/` and on the
deliberately broken `engine/tests/data_bad/`, and a server end-to-end test (`f3_gametest --content`).

## 6. Reloading

* Console `reload [data|scripts|all]` (default `all`) and chat `/reload [data|scripts|all]` (dev servers only: `--dev-kit` or
  `--dev-commands`; on other servers the command is refused). Lua: `reload_data()` returns the report text.
* The new folder is read and validated completely first. On any error nothing changes (the previous data stays active) and the
  first errors are reported. On success it is applied in one step on the main loop; the report lists the kinds that changed (added /
  removed / changed ids), e.g. `perks: +0 -0 ~1 (changed: toughness)`.
* Safe application: existing critters and items keep their ids; the prototype table only grows (new records get new ids, removed
  records stay valid for instances that exist); spawns, pickups and drops after the reload use the new numbers. Players get their
  derived values (HP ...) recomputed and a fresh content sync.

## 7. What the client gets (`S2C_ContentDefs`, id 130)

Sent after login (also while the session waits in character creation) and after every reload; old clients ignore it, old servers never
send it. Contents: schema, revision, trait max, the **trait list** (index, id, name, description), the **perk list** (id, name,
description, ranks, level), and the **prototype overrides** (pid, name, description, weapon range / damage, armor values; new
prototypes with the pid of the original they are cloned from), split in chunks of 40. The creation screen reads its trait names,
tooltips and slot layout from the list (slots without a trait are hidden); a client that never receives the message keeps the
original English.stf trait names. The client applies prototype overrides to its `ProtoDb` so inventory and examine texts match.

## 7b. Localization of data

Data JSON carries English text only (`name`, `desc`, quest `title` / `text`). Translations are locale keys by convention, not per-record
fields (`name_ru` / `desc_ru` do not exist): `perk.<id>.name|desc`, `trait.<id>.name|desc`, `skill.<EnglishName>.name|desc`,
`quest.<id>.title|text` in `data/locale/<lang>/content.json`; `stf.<id>` overrides an original English.stf text. Fallback: language -> English
data text. The content sync (7) is built per player language. Details: `docs/localization.md`.

## 8. Known gaps

* Effects cover the stats listed in 4; no SPECIAL modifiers, no conditional or per-weapon effects yet.
* The shipped traits have no effects; the shipped perks are placeholder numbers.
* Perk spending is a chat command (`/perk take`); the client has no perk window yet (the data is already synced to it).
* NPC `spawns` run only at startup. Values copied into a critter at spawn time are not refreshed by `/reload`.
* Item records cannot add a prototype class from scratch (new records clone an existing one) and cannot change a model or icon.
