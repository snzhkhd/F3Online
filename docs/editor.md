# Content editor

A browser based editor for the `data/` folder, served by `f3_server` itself. It edits the same JSON files the server loads
(`docs/data-format.md`), validates them with the server's own validator and can make the running server re-read them.

Source: `engine/server/EditorCore.*` (HTTP free API), `EditorSchema.cpp` (schema), `EditorServer.*` (HTTP front), `tools/editor/` (UI; `i18n.js` holds the UI strings).

## 1. Starting it

```
f3_server --local --editor                      # http://127.0.0.1:4041/
f3_server --local --editor --editor-port 4050
f3_server --local --editor --editor-dir tools/editor     # serve the UI from another folder
```

The editor is **off by default**; `--local` does not turn it on, and neither do `--editor-port`, `--editor-bind` or `--editor-dir` on their own.
On start the server prints one line with the full address, including a random access token:

```
editor: http://127.0.0.1:4041/?token=549fb8343684de3253399c8b39df8ba7
```

Open that address in a browser. The token stays in the address bar so F5 works; the page keeps it in memory only.
The UI files are looked up in `tools/editor/` from the current directory upwards (or `--editor-dir`). Without them only `/api` is served.
Edits go to the content folder the server runs with (`--content DIR`, else the `data/` it found); the log line shows which one.

The editor works without the original game data too; then the original prototypes (base layer, original STF texts) are unavailable
and the UI says so.

## 2. Security model

The editor can write files, so it is closed in layers:

| Layer | Rule |
| --- | --- |
| Bind address | Loopback only: `127.0.0.1`, `::1`, `localhost`. Anything else (`--editor-bind 0.0.0.0`, a LAN address) is refused at startup, exit code 2. |
| Peer | Every request is checked: the TCP peer must be a loopback address. |
| Host header | Must be `127.0.0.1:<port>`, `localhost:<port>` or `[::1]:<port>` (blocks DNS rebinding: a page on `evil.example` resolving to 127.0.0.1 is refused with 403). |
| Token | 128 bit random token per run, printed in the console. `/api/*` needs it in the `X-F3-Token` header (constant time compare, 401 otherwise). The `?token=` query is accepted only for the page itself (`GET /`), never for `/api`. |
| CSRF | Writing methods (`PUT`, `POST`, `DELETE`) must send `Content-Type: application/json`, which a cross-site form cannot do without a CORS preflight, and the server sends **no CORS headers**, so other pages cannot read answers either. |
| Response headers | `Cache-Control: no-store`, `X-Content-Type-Options: nosniff`, `Referrer-Policy: no-referrer`, `X-Frame-Options: DENY`, and a CSP (`default-src 'self'`, no inline script). |
| File access | Confined to the content folder: a file is addressed by `kind` + `file`; `kind` must be one of the known kinds (`items`, `npcs`, `perks`, `traits`, `skills`, `quests`, `dialogs`) or `locale/<lang>`; `file` is a flat name of `[A-Za-z0-9_.-]`, ends in `.json`, has no `..`, no leading `.`, no Windows device name. The resolved path is checked against the folder again (`weakly_canonical`). Static UI files: flat names, extension whitelist. |
| Threads | The HTTP threads never touch game state. Validation against the live data, reload and STF lookups run on the server thread through `Server::PostTask` (the same queue as `PostCommand`); the original prototypes are copied once, before the HTTP thread starts. A wait that the main loop does not answer in 20 s fails the request instead of hanging. |

Still, it is a development tool: run it on a machine you trust, and do not forward the port.

## 3. What the UI does

* **Lists** per kind (items with weapon / armor / ammo / kit / other chips, npcs, perks, traits, skills, quests, dialogs): search, origin and file filters.
  Each row says where the record comes from: `hand` (hand-authored file), `gen` (file with a `generator` key, written by `--export-data`) or `base`
  (original prototype that is not in `data/`; tick "include original prototypes" or type two letters to see them).
* **Form** generated from the schema: types, enums, ranges, required marks, reference pickers (item, npc, quest, perk, trait, dialog, map, flag; STF id with a
  read-only preview of the original text and a search box), lists with add / move / duplicate / remove.
* **Diff vs base** (items and npcs): fields that differ from the original prototype are highlighted in the form, listed in the Diff tab, and have
  "reset to base". An absent field inherits the base value (shown as a hint).
* **JSON** tab: the record as text. It applies as soon as it parses; text that does not parse stays in the box and is never written. The preview below shows the whole file as it will be saved.
* **Problems** panel: the server's validator on the whole data folder (the edited file replaced by your working copy), messages are `data/<kind>/<file> <path> message`,
  clicking one jumps to the record, the field is marked in the form.
* **Save** writes the file atomically in the exporter's formatting (`DataFormat.cpp` is shared with `--export-data`, so git diffs stay clean; shipped generated files
  round-trip byte for byte, tested). Invalid input is **never** written: the answer is 422 with the errors and the work stays in the browser. "Save as draft"
  writes `_draft_<file>` instead, which the server ignores. The working copy of every file with unsaved edits is also kept in `localStorage` and restored after F5.
* **Generated files**: saving one asks first. "Take over" removes the `generator` key so the next `--export-data` leaves the file alone.
* **Undo / redo** (Ctrl+Z / Ctrl+Y) over all edits of the session, typing is coalesced; a dot marks files with unsaved changes.
* **Apply to running server** calls the same logic as the console `/reload` and shows the reload diff report (`items: +0 -0 ~1 (changed: ...)`).
* **Dialogs**: graph of nodes (SVG, edges are `goto`, blue = conditional), node list, node editor, answers with condition and effect builders
  (descriptors come from the schema). Checks: unreachable nodes, dead ends (nodes without answers), unknown `goto` targets, unknown start. Renaming a node repoints every `goto`.
* **Locale**: en and a chosen language side by side with the missing / placeholder status of `--check-locale` (the same code), per key, with the original data text as a
  placeholder; a new key can be added in all languages at once.
* **Object scripts**: a marked, inactive section, see section 7.

* **UI language** (`RU` / `EN` button in the header): the editor's own texts (buttons, labels, hints, messages, form labels and help of the schema, the common validator
  message prefixes such as `unknown item '...'`) switch between English and Russian. The choice is kept in `localStorage` (`f3ed.lang`); without one the browser language
  decides (Russian browsers start in Russian, everything else in English). Only the editor itself is translated: record ids, names, texts of the data files, JSON, the
  Locale tab contents and technical vocabulary (condition / effect kinds, skill names, field keys) are never touched, and the saved files do not depend on the UI language.
  See section 3b for how to add texts or a language.

Known limits: a hand file with `//` comments loses them on save (the UI warns, the API reports `hasComments` / `hadCommentsLost`); a file is edited as a whole document, there is no merge of
concurrent changes (the etag check turns them into a conflict dialog); deleting a file moves it to `_deleted_<name>` (ignored by the server) instead of destroying it.

### 3b. UI strings (`tools/editor/i18n.js`)

The code of the UI is written in English; `i18n.js` translates what it puts into the page. The table maps the **English text to its translation** (`Ed.i18n.add(lang, table, patterns)`),
so a new button needs no key: write the English text, then add `'Text': 'Перевод'` to the `ru` table. A text without an entry stays English (nothing breaks, the missing
entries are easy to find by switching the language and looking for English left over). A DOM translator walks text nodes and the `title` / `placeholder` / `aria-label`
attributes, remembers each original (so switching back restores it) and watches for new nodes (a `MutationObserver`), which is why render code does not call a lookup function.
Not translated by design: `<input>` / `<textarea>` values, `<pre>` / `<code>`, elements with class `mono`, `id`, `nm`, `json` or the attribute `data-notr`, SVG text.

* Texts with numbers or names inside (`3 records`, `Cannot load X: ...`) use the **patterns** list: `[regex, (match, tr) => translation]`; `tr()` translates a captured part.
  Keys are compared trimmed (fragments around inline elements often carry blanks).
* `Ed.T('Cannot load {file}', { file })` is for the few strings that are not shown through the DOM (a `confirm()`, a value).
* A new language: `Ed.i18n.add('de', { ... }, [ ... ], 'Deutsch')` in its own file loaded after `i18n.js`; the header button then cycles through all languages, and `navigator.language`
  picks it by default when it matches.

## 4. API

All under `/api`, JSON in and out, header `X-F3-Token`. Errors are `{"ok":false,"code":"...","error":"..."}`.

| Method and path | Purpose |
| --- | --- |
| `GET kinds` | kinds with their files (records, generated, draft), locale languages and files, `hasGamedata`, `canReload`, `dataDir` |
| `GET schema` | the machine-readable schema (section 5) |
| `GET list?kind=K` | rows (`id`, `name`, `file`, `origin`, ...) plus the files of the kind; `origin=base` rows are the original prototypes |
| `GET get?kind=K&file=F` | the parsed file, raw text, `etag`, `generated`, `hasComments`; `get?kind=K&id=ID` returns the base record of an original prototype |
| `PUT put` | `{kind, file, doc, etag?, ackGenerated?, takeover?, draft?}`: validates, then writes atomically. 409 `conflict` (stale etag) / `generated`, 422 with `errors` |
| `DELETE delete` | `{kind, file, id}` removes a record, `{kind, file, confirm:true}` moves the file to `_deleted_`; refused while other records refer to it |
| `POST validate` | `{}` validates the folder, `{kind, file, doc}` validates it with that file replaced (nothing written) |
| `POST format` | `{doc}` returns the canonical text |
| `POST reload` | the server re-reads `data/` and returns the diff report |
| `GET locale-status` | per key en / target text, file, status (`ok`, `missing`, `extra`, `placeholders`) and the `--check-locale` report |
| `GET refs` | id lists for the pickers |
| `GET stf?id=N` / `stf?q=text` | original STF text / search |

Save refuses a change when the edited file has errors or when the change introduces **new** errors elsewhere (for example deleting an item another record uses);
errors that were already there are reported as `otherErrors` and do not block.

## 5. How the schema works (form, docs and validator cannot drift)

`EditorSchema.cpp` holds one JSON description per kind: field `key`, `type`, `required`, `min` / `max`, `maxLen`, `enum`, `ref`, nested `fields`, `item` for lists, ...
Field types: `string text int number bool enum intenum object list stringlist intmap cell condition effects statmods dlgstart`.
Everything the validator enumerates itself is **not retyped**: `"enum":"$skills"` and similar placeholders are expanded at run time from the same constants Content.cpp uses
(`kSkillNames`, `SpecialKeyNames()`, `StatNames()`, `DialogStatNames()`, the envelope version `kContentSchema`), and `{"$special": {...}}` expands to one field per SPECIAL key.
`/api/schema` serves the result; the UI builds forms, ranges, pickers and the condition / effect builders from it, nothing about a kind is hard coded in `tools/editor/`
except the special editors named by `editor` (`"dialog"`).

The numeric limits are mirrored from Content.cpp by hand, so `f3_editortest` closes the gap: it walks the schema, and for **every** field feeds the real validator
the minimum and maximum (must be accepted), min-1 / max+1, wrong types, unknown fields and over-long strings (must be rejected). Change a limit on one side only and the test fails.
It also runs one sample of every condition and effect form the builder can emit, and checks that each shipped generated file survives load, edit-free save and compare byte for byte.

## 6. Adding a new kind

1. Teach the loader and validator the kind (Content.cpp / Content.h; see `docs/data-format.md`, section 3).
2. In `EditorSchema.cpp` add a `const char* const kMyKind = R"J({ "id":"mykind", "label":..., "dir":"mykind", "nameField":..., "fields":[...] })J";`
   and add it to the kinds table (next to `{"quests", "Quests", "Content"}`) in `BuildSchema()`.
3. Add the directory name to the known kinds list in `EditorCore.cpp` (`Kinds()`), which is also the whitelist of the path confinement.
4. Reference lists for pickers: add the id list to `Api::Refs` and the `ref` name to `ensureDatalist` in `tools/editor/app.js`.
5. `f3_editortest` picks up the new schema automatically and probes every field against the validator; add a sample record if the kind needs cross references.
6. A kind that needs a special editor sets `"editor":"name"` and registers it from a new file in `tools/editor/` (dialogs: `dialog.js`).

## 7. Extension point: object scripts (not implemented)

Planned: behaviour attached to a placed map object, for example a computer terminal that runs a dialog, a switch that opens a door, an object that needs a skill check.
Nothing reads or writes such files today. The UI has a marked section ("Object scripts", nav group "Planned") and `/api/schema` carries
`extensionPoints.objectScripts` with the idea below; a future module sets `Ed.ext.objects` in `tools/editor/`.

Intended shape, a new kind `objects` with `data/objects/*.json`:

```json
{
  "schema": 1,
  "kind": "objects",
  "records": [
    {
      "id": "junktown_terminal_1",
      "map": "junktown",
      "cell": [112, 87],
      "on_use": [
        { "require_skill": "Science>=50", "else": "terminal_locked_dialog" },
        { "run_dialog": "junktown_terminal" },
        { "open_door": [120, 90] }
      ]
    }
  ]
}
```

* `id` names the object (a placed object of the map file, or a freshly defined one); `map` and `cell` locate it.
* `on_use` is an ordered list of steps executed on the server when a player uses the object, the same vocabulary as dialog effects where it exists
  (`run_dialog` -> dialog id, `open_door` -> the `door` effect of `docs/dialogs.md`, conditions in the `skill:Science>=50` form with the `if` / `else` shape used by dialog routes).
* The steps would be validated by the same code as dialogs (references to dialogs, cells inside the map), exported by `--export-data` for map objects with scripts, and edited by a form generated from
  the schema like every other kind, with a map cell picker as the one new widget.
