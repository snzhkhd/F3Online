# UI fonts with Cyrillic glyphs

The original interface fonts of the game (`Fonts\r_fallouty.ttf`, `TT0807M_.TTF` of the install) have Latin glyphs only. For every language
other than English (and for any string that contains non-Latin characters, e.g. a Cyrillic player name) the client draws text with two
replacement TTF fonts loaded with the code points U+0020-007E, U+00A0-00FF, U+0400-04FF and common punctuation:

| role | used for | file names tried (first with Cyrillic glyphs wins) |
|------|----------|-----------------------------------------------------|
| body | text, chat, lists, tooltips | `regular.ttf` in font_dir, then system `tahoma.ttf`, `segoeui.ttf`, then `assets/fonts/regular.ttf`, `arial.ttf` (the supplied regular.ttf has italic-like Cyrillic) |
| head | titles, buttons, numbers    | `display.ttf`, `regular.ttf`, then system `segoeuib.ttf`, `tahomabd.ttf`, `arialbd.ttf` |

**The font files are NOT part of the repository** (their licenses are not clear). This folder is ignored by git except for this file.
The three files that were used during development (`regular.ttf`, `display.ttf`, `decayed.ttf`) come from the sibling project
`F:\RayLib\FRay\assets\Fonts`; copy any TTF with Cyrillic coverage here under those names, or point the client at it:

* `client.cfg`: `font_dir=<folder>`, `font_body=<file.ttf>`, `font_head=<file.ttf>`
* command line: `--font-dir <folder>`

Search order: explicit `font_body` / `font_head`, then `font_dir` / `--font-dir`, then `assets/fonts` next to the exe / cwd and their parents,
then `C:\Windows\Fonts` (system fonts are loaded at run time and never copied). When nothing usable is found the client logs a warning and
keeps the Latin fonts; Cyrillic text then shows as `?`.
