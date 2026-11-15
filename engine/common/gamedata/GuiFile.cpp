#include "GuiFile.h"
#include <cstring>

namespace f3::gd {
namespace {
bool PrintableStr(const uint8_t* d, size_t size, size_t off, size_t& len) {
    if (off + 4 > size) return false;
    uint32_t n;
    std::memcpy(&n, d + off, 4);
    if (n < 3 || n > 64 || off + 4 + n > size) return false;
    for (uint32_t i = 0; i < n; ++i)
        if (d[off + 4 + i] < 32 || d[off + 4 + i] > 126) return false;
    len = n;
    return true;
}
bool EndsWith(const std::string& s, const char* ext) {
    size_t n = std::strlen(ext);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if ((s[s.size() - n + i] | 0x20) != ext[i]) return false;
    return true;
}
}  // namespace

std::string GuiWidget::font() const {
    for (auto& s : misc)
        if (s.find('.') == std::string::npos) return s;
    return {};
}

int GuiFile::Find(const std::string& n, int from) const {
    for (size_t i = from < 0 ? 0 : from; i < widgets.size(); ++i)
        if (widgets[i].name == n) return static_cast<int>(i);
    return -1;
}

bool ParseGui(const uint8_t* d, size_t size, GuiFile& out) {
    out = GuiFile{};
    if (size < 16 || std::memcmp(d, "GUI V1.3", 8) != 0) return false;
    size_t l;
    if (PrintableStr(d, size, 8, l)) out.name.assign(reinterpret_cast<const char*>(d) + 12, l);
    // Widget record anchor: name string, 3 flag bytes, skin ini string, colour(3) alpha(1), rect (4 x i32).
    std::vector<size_t> ends;   // where each widget's header ends (start of its trailing strings)
    std::vector<size_t> starts;
    size_t i = 0;
    while (i + 8 < size) {
        size_t nl, il;
        if (PrintableStr(d, size, i, nl)) {
            size_t o = i + 4 + nl;
            if (PrintableStr(d, size, o + 3, il)) {
                std::string ini(reinterpret_cast<const char*>(d) + o + 7, il);
                if (il > 4 && EndsWith(ini, ".ini") && o + 7 + il + 20 <= size) {
                    GuiWidget w;
                    w.name.assign(reinterpret_cast<const char*>(d) + i + 4, nl);
                    std::memcpy(w.flags, d + o, 3);
                    w.skin = ini;
                    size_t p = o + 7 + il;
                    w.alpha = d[p + 3];
                    std::memcpy(w.rect, d + p + 4, 16);
                    out.widgets.push_back(std::move(w));
                    starts.push_back(i);
                    ends.push_back(p + 20);
                    i = p + 20;
                    continue;
                }
            }
        }
        ++i;
    }
    for (size_t k = 0; k < out.widgets.size(); ++k) {
        size_t e = k + 1 < starts.size() ? starts[k + 1] : size, j = ends[k];
        std::vector<std::string> tex;
        while (j + 4 < e) {
            size_t n;
            if (PrintableStr(d, size, j, n) && j + 4 + n <= e) {
                std::string s(reinterpret_cast<const char*>(d) + j + 4, n);
                if (EndsWith(s, ".tga") || EndsWith(s, ".bmp") || EndsWith(s, ".dds")) tex.push_back(s);
                else out.widgets[k].misc.push_back(s);
                j += 4 + n;
            } else {
                ++j;
            }
        }
        // the trailing 9 textures are the skin fragments (a child widget's own record starts at its name)
        if (tex.size() >= 9) out.widgets[k].frag.assign(tex.begin(), tex.begin() + 9);
    }
    return !out.widgets.empty();
}

}  // namespace f3::gd
