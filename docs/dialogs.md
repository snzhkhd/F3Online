# Data dialogs (`data/dialogs/`)

A **data dialog** is a conversation graph written as JSON: nodes with text, answers that appear only when a condition holds, and
effects that run when an answer is chosen. The server runs it through the existing dialog protocol (`S2C_DialogOpen`,
`S2C_DialogNode`, `S2C_DialogClose`, `C2S_DialogChoose`, `C2S_DialogClose`), so the client needs no change. Data dialogs live
next to the original AMX dialogs: a critter whose `dialog` field names a data dialog id uses the graph, any other value still
runs the original `.scra` script. Shipped samples: `data/dialogs/junktown.json` (three NPCs in `data/npcs/junktown_test.json`,
spawned in Junktown; walk there and talk to Scrapper Joe, the Drifter and Old Mara; `/near Joe` teleports next to one).

## 1. Binding a dialog to an NPC

```json
// data/npcs/junktown_test.json
{ "id": "F3_Junktown_Scrapper", "base": "Critters04_BFBFMale.CRT", "name": "Scrapper Joe", "dialog": "junktown_scrapper", "script": "",
  "spawns": [ { "map": "00_03_Tutorial_Junktown", "cell": [190, 556] } ] }
```

`dialog` on an NPC record (or on a map instance, via the prototype) is looked up first among the data dialogs (case-insensitive id);
when none matches, it is the name of an original dialog and the AMX path runs as before. A dialog id and an original dialog name
must therefore not collide (the validator does not know the AMX names). `speaker` in the dialog record is optional documentation
plus a check: it must name an existing critter prototype.

The shipped sample NPCs also carry `"equipment": [ ... ]` (a leather outfit or biohazard suit plus a crowbar / pistol / hatchet): equipment is worn on
spawn, and the worn armor and the hand-1 weapon are sent to the clients as the appearance block, so the figure is drawn dressed and armed. A record without
`equipment` (and a base prototype without one) is drawn in the base body, i.e. in underwear. Pick ids from `data/items/armor.json` / `weapons.json`.

## 2. File format

Envelope as everywhere (`docs/data-format.md`, 2): `{ "schema": 1, "kind": "dialogs", "records": [ ... ] }`. Strings have a 1500
character limit. Ids: letters, digits, `_ . -`.

```json
{ "id": "junktown_scrapper",
  "speaker": "F3_Junktown_Scrapper",
  "start": [ { "if": "quest:scrapper_toolkit==done", "goto": "after" },
             { "if": "quest:scrapper_toolkit==active", "goto": "reminder" },
             { "goto": "greeting" } ],
  "nodes": [
    { "id": "greeting",
      "text": "Hey, stranger. Need something?",
      "on_enter": [ { "flag": "scrapper_seen", "set": true } ],
      "answers": [
        { "id": "work", "text": "I am looking for work.", "if": [ "stat:level>=2", "!flag:scrapper_met" ],
          "do": [ { "quest": "scrapper_toolkit", "state": "add" } ], "goto": "job" },
        { "id": "yes",  "stf": 11, "do": [ { "trade": true } ] },
        { "id": "bye",  "text": "Not now." }
      ] } ] }
```

| field | meaning |
|---|---|
| `id` | required, unique among dialogs (case-insensitive) |
| `speaker` | optional critter prototype id, checked |
| `start` | node id, or a list of routes `{ "if": <condition>, "goto": <node> }` evaluated in order; a route without `if` always matches. Default: the first node. When no route matches (or the node does not exist) the player is told the NPC has nothing to say |
| `nodes` | required, 1..200 nodes |
| node `id` | unique within the dialog |
| node `text` / `stf` | exactly one of the two (see 5) |
| node `on_enter` | effects run every time the node is entered, before its text is sent |
| node `answers` | up to 24 answers (the protocol carries at most 24 replies) |
| answer `id` | unique within the node; used as the locale key suffix |
| answer `text` / `stf` | exactly one of the two |
| answer `if` | condition; the answer is offered only when it holds |
| answer `do` | list of effects, run in order when the answer is chosen |
| answer `goto` | the next node. Missing, empty or `"end"` ends the conversation |

A node without any visible answer gets one automatic "leave" reply (`dlg.leave`), so the player is never stuck. Replies keep the
order of the file.

## 3. Conditions

A condition is a string `<kind>:<name><op><value>` or a combinator. Operators: `>= <= == != > <` (the shipped samples use `>=`
and `==`). A leading `!` negates a string condition.

| condition | true when |
|---|---|
| `skill:Barter>=40` | total skill value of the player (any of the 14 skill names, case-insensitive) |
| `stat:level>=2` | `level`, `hp`, `max_hp`, `xp`, or a SPECIAL name (`strength` ... `luck`) |
| `item:KitsTool_Kit.ITM>=1` | the player carries that many of an item prototype id |
| `quest:scrapper_toolkit==done` | quest state: `none` (never added), `active`, `done`, `failed` |
| `flag:scrapper_met` | the dialog flag is non-zero (`flag:name>=2` compares the value) |
| `lua:my_check` | the Lua function `my_check(player, npc)` returns true (use sparingly) |

Combinators are JSON values: an **array** means all of them (AND), `{ "any": [ ... ] }` is OR, `{ "all": [ ... ] }` is AND and
`{ "not": <condition> }` negates. They nest freely: `{ "any": [ "stat:strength>=6", { "not": "flag:x" } ] }`.

Flags are per-player global variables (`SetGlobalFor`, in Lua `c:get_gvar(name)` / `c:set_gvar(name, value)`, `/gvar name`), saved
with the character. There is no karma condition by design.

## 4. Effects

An effect is an object with **exactly one action key** (plus its parameters):

| effect | parameters | what it does |
|---|---|---|
| `{ "give": "Item.ITM", "count": 2 }` | `count` default 1 | adds the item to the player (existing prototype, checked) |
| `{ "take": "Item.ITM", "count": 1 }` | `count` default 1 | removes items from the player (unequips first) |
| `{ "quest": "id", "state": "add" }` | `state`: `add`, `done`, `fail`, `reopen` | quest log; `done` pays the quest rewards once. `done` / `fail` of a task never added adds it first |
| `{ "flag": "name", "set": true }` | `set`: bool or integer | sets the dialog flag (`true` = 1) |
| `{ "xp": 50 }` | integer | grants experience |
| `{ "trade": true }` | none | **stub**: there is no store yet; the player gets the message `msg.trade_missing` and the dialog goes on |
| `{ "attack": true }` | none | ends the dialog; the NPC turns hostile and targets the player |
| `{ "end": true }` | none | ends the dialog (effects after it are not run) |
| `{ "door": [x, y], "action": "open" }` | `action`: `open`, `close`, `lock`, `unlock` | operates the door or container at that cell of the NPC's map |
| `{ "lua": "fn_name" }` | none | calls `fn_name(player, npc)` in `scripts/` (the demo uses `dlg_demo_hook` in `scripts/main.lua`) |

Effects of a node (`on_enter`) and of the chosen answer run in order; when an effect ends the dialog (`end`, `attack`), the rest of
the list and the `goto` are skipped.

## 5. Text, text keys and original text

* `text` is our own English text. `{player}` and `{npc}` are replaced with the names.
* `stf` is the id of a string in the original `English.stf` (1-based): the original text is shown without copying it into git. The
  id must exist in the loaded game data (checked). Original text is never stored in `data/`; use it for short stock phrases only.
* Translations are locale keys, not record fields: `dialog.<dialog id>.<node id>.text` for a node and
  `dialog.<dialog id>.<node id>.<answer id>` for an answer (`a1`, `a2`, ... when the answer has no id), in
  `data/locale/<lang>/*.json` (shipped: `data/locale/ru/dialogs.json`). Fallback: language -> the English `text`. An `stf` text
  follows the player's language through `stf.<id>` overrides and the original per-language strings.
* `f3_server --check-locale` counts these keys as data keys (the coverage figures include them), so a missing Russian line shows up there.
  See `docs/localization.md`.

**Tokens.** The original string tokens are substituted on the server, for original AMX dialogs (`DlgText`, `AmxActions` speech) and data dialogs (`DataDlgText`),
before the text is sent (`ExpandDlgTokens`, `Dialogs.cpp`): `<CHARNAME>` / `<PLAYERNAME>` become the speaking player's name (so Frank's "No, I'm <CHARNAME>" shows the
name), and unknown `<...>` markers are left as they are. `{player}` / `{npc}` keep working. Tests: `f3_gametest --tutorial` (AMX path) and `--dialogs` (data path, "Name is Joe, <CHARNAME>").
## 6. Validation

At startup, with `--validate-data` and on `/reload`, the dialogs are checked together with the rest of `data/`; any error stops the
server (or, with `--lenient`, drops the dialog). Messages name the file and the path, for example:

```
data/dialogs/junktown.json: records[0].nodes[2].answers[1].goto: unknown node 'jobb' (nodes of this dialog: greeting, who, ...)
data/dialogs/junktown.json: records[0].nodes[0].answers[1].do[0]: unknown quest 'ghost' (define it in data/quests)
data/dialogs/junktown.json: dialog 'junktown_drifter'.speaker: unknown prototype 'Nobody.CRT'
```

Checked: JSON types and unknown fields, duplicate dialog / node / answer ids, `goto` and `start` targets, the one-action rule of
effects, condition syntax (kind, skill and stat names, operators), unknown quests, items, speakers and `stf` ids, `text` xor `stf`,
the answer and node limits. An unreachable node is a warning. Lua function names are not resolved at load time (a missing function
is logged when it is called).

## 7. Running and reloading

* `Talk` on an NPC with a data dialog sends `DialogOpen` (speaker name) and the first `DialogNode`; each `DialogChoose` runs the
  effects and sends the next node or `DialogClose`. The server keeps the state per player (dialog id and node id only).
* The conversation ends when the player walks more than 12 cells away, either side dies or leaves, or the player chooses to leave. A
  second `Talk` restarts it from the `start` routes.
* `/reload` re-reads the files. A conversation in progress keeps its node id and looks the node up again on the next step; if the
  node vanished the dialog is closed. The reload report lists changed dialogs (`dialogs: +0 -0 ~1 (changed: junktown_guide)`).
* Original AMX dialogs are untouched: `amx_->InDialog` also reports data dialogs, so the two never run at once for one player.

## 8. Tests

* `f3_datatest`: parsing of every condition and effect form, validation errors (bad `goto`, unknown quest / item / speaker / stf,
  duplicate ids, malformed conditions, one-action rule), lenient mode, the reload diff.
* `f3_gametest --dialogs` (run by `engine/tests/smoke_test.ps1` on a throw-away copy of `data/`): Scrapper Joe end to end with a
  skill-gated and a level-gated answer, quest add / done, give / take, integer and boolean flags, routing by quest state, an item
  condition; the Drifter's `stf` answers, the trade stub, the Lua hook and the attack effect; Old Mara's `any` / `not` conditions.

## 9. Known gaps

* `trade` is a stub; the `door` effect is implemented but not covered by an automatic test.
* The client has no dialog UI specific to data dialogs: they use the generic dialog window (24 replies, no icons, no skill-check
  colouring).
* No karma, reputation or time-of-day conditions; no `random` effect; no per-answer repeat limit (use a flag).
* A dialog id cannot also be an original AMX dialog name (the data dialog wins).
