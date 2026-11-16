# Localization

Languages: `en` (base) and `ru`. All our own UI and server strings are looked up by key; the English text lives in `data/locale/en`, other
languages only override or add. Library: `engine/common/Localization.{h,cpp}` (namespace `f3::loc`, CMake lib `f3_locale`), UTF-8 helpers in
`engine/common/utf8.h`, client text layout in `engine/client/TextUtil.h`, fonts in `engine/client/Fonts.{h,cpp}`.

## Layout

```
data/locale/<lang>/*.json     flat tables  "key": "text with {placeholders}"; every *.json of a folder is merged
  en/messages.json            server messages (chat / system lines, command output, login errors)
  en/ui.json                  client UI (menus, creation screen, dialog hints, cursors, Pip-Boy, loot, inventory ...)
  ru/messages.json ui.json    Russian translation of the two tables above
  ru/content.json             Russian names / descriptions of data (perks, traits, skills, quests)
  ru/stf.json                 Russian overrides of original English.stf texts ("stf.<id>")
```

Rules: no plural forms (word the text so it needs none, e.g. "Items: {n}"); `{name}` is a placeholder, `{{` and `}}` are literal braces;
unknown placeholders stay in the text. Fallback chain: selected language -> `en` -> the key itself.

## Using keys

C++: `loc::T("key")`, `loc::T("key", {{"n", 3}, {"name", s}})`, `loc::TOr("key", fallback)`. Args may be strings, ints, `size_t`; format
doubles into a string first. Server code uses the per-player helpers `TellK(critter, key, args)`, `L`, `LOr`, `Stf`, `SkillName`
(language = `Critter::lang`). Lua: `c:tell_key("lua.welcome", {name = c.name})`.

Languages: server default `--lang CODE` / server config `lang`; client `--lang CODE` / `client.cfg` `lang=`. The client sends its language
in the login request (trailing optional field `lang`, old clients omit it and get the server default). `/lang` shows the player's
language, `/lang <code>` switches it at run time (server re-sends the content definitions; the client also reloads its tables).

## Data (perks, traits, skills, quests, stf)

Chosen approach: **convention keys, data JSON stays English-only**. The English name / description are in `data/*.json`
(`name`, `desc`); a translation is a key in a locale file:

| key | overrides |
|-----|-----------|
| `perk.<id>.name`, `perk.<id>.desc` | perk of data/perks |
| `trait.<id>.name`, `trait.<id>.desc` | trait |
| `skill.<EnglishName>.name`, `skill.<EnglishName>.desc` | skill |
| `quest.<id>.title`, `quest.<id>.text` | quest |
| `stf.<id>` | text of the original English.stf (1-based id; item / NPC names, UI labels) |

The server resolves them per player when it syncs content definitions and when it writes quest texts; a missing key falls back to the English
data text. `name_ru` / `desc_ru` fields are NOT used. See also `docs/data-format.md`.

## Adding a language (e.g. `de`)

1. Copy `data/locale/en/*.json` to `data/locale/de/` and translate the values (keep placeholders). Optionally add `content.json` and `stf.json`.
2. Run `f3_server --check-locale --strict`: lists missing keys, placeholder mismatches, stale keys.
3. Use it: `--lang de` / `lang=de`. Add `de` to nothing else: languages are discovered from the folders.
4. Cyrillic / non-Latin scripts need a TTF with those glyphs, see `assets/fonts/README.md`; for Latin scripts with accents extend the codepoint
   ranges in `Fonts.cpp`.

## Coverage tool

`f3_server --check-locale [--strict] [--locale DIR] [--content DIR] [--src DIR]`: per language, missing keys (vs `en`), extra keys,
placeholder mismatches (error), keys used in the source but undefined (error), keys defined but unused (warning), data keys without
translation. `stf.*` keys count as overrides. `--strict` makes missing translations errors. Exit code 1 on errors. Wired into
`engine/tests/smoke_test.ps1` (block 0c, including a broken fixture that must fail).

## Fonts and text layout

The original `.gui` fonts are Latin-only bitmap fonts. For `lang != en`, or for any text with non-Latin characters, the client draws with
TTF replacement fonts loaded with U+0020-007E, U+00A0-00FF, U+0400-04FF (LoadFontEx): body role Tahoma / Segoe UI (system) or
`assets/fonts/regular.ttf`, head role `assets/fonts/display.ttf`. Fonts are not committed (license), see `assets/fonts/README.md`.
Label text that no longer fits its box is shrunk (`OrigUi::FitSize`, down to 62%). Measuring and wrapping (dialog, loot, chat, bubbles,
nameplates, tooltips) work on code points (`txt::Wrap`, `txt::Ellipsize`).

## UTF-8 rules

* Everything is UTF-8: network strings, JSON, SQLite text, locale files, the client command line (converted from the wide command line).
* Character / account names: 2-16 code points, Latin or Cyrillic letters, digits and `_`, no spaces / control characters / broken UTF-8
  (`utf8::ValidName`); account lookup is case-insensitive across Latin and Cyrillic (`utf8::Lower`).
* Chat: invalid sequences and control characters are dropped, trimmed, at most 200 code points (`utf8::SanitizeText`).
* Client input: typed UTF-8 text, Ctrl+V paste; Backspace removes a whole code point.

## Tests

`f3_localetest` (loader, fallback, placeholders, check report, UTF-8, wrap), `f3_dbtest` (Cyrillic round trip in SQLite),
`f3_gametest --locale` (ru / en / default-language players get the right server messages, Cyrillic names and chat), `--check-locale --strict`.

## Known gaps

Text baked into the original images (STATS, SKILLS, OPTIONS, DONE / CANCEL, SPECIAL plates) stays English; NPC dialogue and most item names
are English (only the `stf.*` overrides are Russian); developer / debug output stays English; the language cannot be chosen on the login
screen yet (`--lang`, `client.cfg`, or `/lang` in game).
