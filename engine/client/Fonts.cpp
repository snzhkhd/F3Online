#include "Fonts.h"
#include <algorithm>
#include <vector>
#include "Settings.h"
#include "common/log.h"
#include "common/utf8.h"

namespace f3::fonts {
namespace {

Font g_body{}, g_head{};
bool g_haveBody = false, g_haveHead = false, g_always = false;
constexpr int kBaseSize = 48;

std::vector<int> Codepoints() {
    std::vector<int> cps;
    for (int c = 0x20; c <= 0x7E; ++c) cps.push_back(c);
    for (int c = 0xA0; c <= 0xFF; ++c) cps.push_back(c);          // Latin-1 (accents, ° « » ...)
    for (int c = 0x400; c <= 0x4FF; ++c) cps.push_back(c);        // Cyrillic
    for (int c : {0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x201E, 0x2022, 0x2026, 0x2116}) cps.push_back(c);   // dashes, quotes, bullet, ellipsis, numero sign
    return cps;
}

bool Has(const Font& f, int cp) {
    if (f.texture.id == 0) return false;
    const int i = GetGlyphIndex(f, cp);
    return i >= 0 && i < f.glyphCount && f.glyphs[i].value == cp;
}

// Loads a candidate; accepted when it has the Cyrillic letters (А Я а я Ё ё).
bool TryLoad(const std::string& path, Font& out) {
    if (path.empty() || !FileExists(path.c_str())) return false;
    std::vector<int> cps = Codepoints();
    Font f = LoadFontEx(path.c_str(), kBaseSize, cps.data(), static_cast<int>(cps.size()));
    if (f.texture.id == 0) return false;
    if (!(Has(f, 0x410) && Has(f, 0x42F) && Has(f, 0x430) && Has(f, 0x44F) && Has(f, 0x401) && Has(f, 0x451))) {
        log::Warn("font %s has no Cyrillic glyphs, skipped", path.c_str());
        UnloadFont(f);
        return false;
    }
    GenTextureMipmaps(&f.texture);
    SetTextureFilter(f.texture, TEXTURE_FILTER_TRILINEAR);
    out = f;
    return true;
}

bool Find(const std::vector<std::string>& candidates, const std::string& explicitPath, Font& out, std::string* used) {
    if (!explicitPath.empty() && TryLoad(explicitPath, out)) { *used = explicitPath; return true; }
    for (const std::string& p : candidates)
        if (TryLoad(p, out)) { *used = p; return true; }
    return false;
}

}  // namespace

bool Load(const Options& o) {
    Unload();
    // candidates in priority order: explicit file, the --font-dir / font_dir folder, a clean system sans for body text (the original game
    // body font is a plain sans too; system fonts are loaded at run time and never copied), the project assets/fonts folder, last resorts
    std::vector<std::string> assetDirs;
    for (const std::string& r : SearchRoots()) assetDirs.push_back(r + "/assets/fonts");
    const std::string sys = "C:/Windows/Fonts/";
    std::vector<std::string> bodyC, headC;
    if (!o.dir.empty()) { bodyC.push_back(o.dir + "/regular.ttf"); headC.push_back(o.dir + "/display.ttf"); }
    for (const char* n : {"tahoma.ttf", "segoeui.ttf"}) bodyC.push_back(sys + n);
    for (const std::string& d : assetDirs) bodyC.push_back(d + "/regular.ttf");
    bodyC.push_back(sys + "arial.ttf");
    for (const std::string& d : assetDirs) headC.push_back(d + "/display.ttf");
    for (const std::string& d : assetDirs) headC.push_back(d + "/regular.ttf");
    for (const char* n : {"tahomabd.ttf", "segoeuib.ttf", "arialbd.ttf"}) headC.push_back(sys + n);
    std::string bodyUsed, headUsed;
    g_haveBody = Find(bodyC, o.body, g_body, &bodyUsed);
    g_haveHead = Find(headC, o.head, g_head, &headUsed);
    if (!g_haveHead && g_haveBody) { g_head = g_body; g_haveHead = true; headUsed = bodyUsed + " (shared)"; }
    if (g_haveBody) log::Info("fonts: body %s, head %s", bodyUsed.c_str(), headUsed.c_str());
    else log::Warn("fonts: no TTF with Cyrillic glyphs found (assets/fonts/README.md): non-Latin text shows as '?'");
    return g_haveBody;
}

void Unload() {
    if (g_haveBody) UnloadFont(g_body);
    if (g_haveHead && g_head.texture.id != g_body.texture.id) UnloadFont(g_head);
    g_body = Font{};
    g_head = Font{};
    g_haveBody = g_haveHead = false;
}

bool Loaded() { return g_haveBody; }
void SetAlways(bool on) { g_always = on; }

static bool NeedsWide(const std::string& s) {
    for (unsigned char c : s) if (c >= 0xCD) return true;   // lead byte of U+0370 and above (0xCD 0xB0 ...)
    return false;
}

const Font& For(Role role, const std::string& text, const Font& latin) {
    if (!g_haveBody) return latin;
    if (!g_always && !NeedsWide(text)) return latin;
    return role == Role::Head && g_haveHead ? g_head : g_body;
}

void Draw(const std::string& text, int x, int y, int size, Color c) {
    if (text.empty()) return;
    if (!g_haveBody || (!g_always && !NeedsWide(text))) { DrawText(text.c_str(), x, y, size, c); return; }
    DrawTextEx(g_body, text.c_str(), Vector2{static_cast<float>(x), static_cast<float>(y)}, static_cast<float>(size), std::max(1.0f, size / 14.0f), c);
}

int Measure(const std::string& text, int size) {
    if (text.empty()) return 0;
    if (!g_haveBody || (!g_always && !NeedsWide(text))) return MeasureText(text.c_str(), size);
    return static_cast<int>(MeasureTextEx(g_body, text.c_str(), static_cast<float>(size), std::max(1.0f, size / 14.0f)).x);
}

int MissingGlyphs(const std::string& text) {
    int n = 0;
    for (size_t i = 0; i < text.size();) {
        uint32_t cp;
        utf8::Next(text, i, cp);
        if (cp < 0x20) continue;
        if (!(g_haveBody && Has(g_body, static_cast<int>(cp)))) ++n;
    }
    return n;
}

}  // namespace f3::fonts
