#pragma once
// Cyrillic capable TTF fonts of the client. The original interface fonts (Fonts\r_fallouty.ttf, TT0807M_.TTF of the game install) have
// Latin glyphs only; for languages that need more (and for any string that contains non-Latin characters, e.g. a Cyrillic player
// name) the client switches to two replacement fonts loaded with the code point ranges 0x20-0x7E, 0xA0-0xFF and 0x400-0x4FF plus common
// punctuation: a "body" font (text, chat, lists) and a "head" font (titles, buttons, numbers). The fonts are NOT part of the repository
// (licenses unclear), see assets/fonts/README.md for where they are searched and how to configure them.
#include <string>
#include <raylib.h>

namespace f3::fonts {

enum class Role { Body, Head };

struct Options {
    std::string dir;     // client.cfg font_dir / --font-dir: extra folder searched first
    std::string body;    // client.cfg font_body: explicit file (overrides the search)
    std::string head;    // client.cfg font_head
};

// Needs the window (GPU). Loads the fonts; returns false when no usable font with Cyrillic glyphs was found (the caller keeps its Latin
// fonts and raylib's default font; Cyrillic then shows as '?'). Logs the files that were chosen.
bool Load(const Options& o);
void Unload();
bool Loaded();

// true: every string uses the replacement fonts (language other than English); false: only strings with characters >= U+0370 do.
void SetAlways(bool on);

// The font to draw / measure `text` with in `role`: a replacement font when needed, else `latin` (the caller's own font).
const Font& For(Role role, const std::string& text, const Font& latin);
// Plain raylib style helpers (body role, replacement font when available, else raylib's default font).
void Draw(const std::string& text, int x, int y, int size, Color c);
int Measure(const std::string& text, int size);
inline void Draw(const char* text, int x, int y, int size, Color c) { Draw(std::string(text ? text : ""), x, y, size, c); }
inline int Measure(const char* text, int size) { return Measure(std::string(text ? text : ""), size); }
// Code points in the string that no loaded font has a glyph for (0 = all can be drawn); for tests / diagnostics.
int MissingGlyphs(const std::string& text);

}  // namespace f3::fonts
