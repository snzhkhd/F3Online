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

// ------------------------------------------------------------------------------------------------ full tree parser

namespace {
struct Rd {
    const uint8_t* d; size_t n, p = 0; bool ok = true;
    bool Raw(void* out, size_t k) {
        if (!ok || p + k > n) return ok = false;
        std::memcpy(out, d + p, k); p += k; return true;
    }
    uint32_t U32() { uint32_t v = 0; Raw(&v, 4); return v; }
    uint8_t U8() { uint8_t v = 0; Raw(&v, 1); return v; }
    std::string Str() {
        uint32_t k = U32();
        if (!ok || k > 4096 || p + k > n) { ok = false; return {}; }
        std::string s(reinterpret_cast<const char*>(d) + p, k); p += k;
        size_t z = s.find('\0'); if (z != std::string::npos) s.resize(z);
        return s;
    }
};

bool ReadNode(Rd& r, uint32_t type, int version, GuiNode& w, int depth) {
    if (depth > 32 || type > 4) return false;
    w.type = type;
    if (type == GuiNode::Button) {
        w.state = r.U32();
        if (version >= 4) { w.keyboard = r.U8() != 0; w.soundDown = r.Str(); w.soundUp = r.Str(); w.soundEnter = r.Str(); }
    }
    r.Raw(w.rect, 16);
    w.name = r.Str();
    w.hidden = r.U8() != 0; w.frameVisible = r.U8() != 0; w.centerVisible = r.U8() != 0;
    w.skin = r.Str();
    w.color = r.U32();
    r.Raw(w.clientRect, 16);
    if (version >= 2) { w.value60 = r.U8(); w.materialModes = r.U32(); }
    if (version >= 3) {
        w.str0 = r.Str(); w.tipTextRef = r.U32(); w.str1 = r.Str(); w.tipTitleRef = r.U32(); w.tipImage = r.Str();
    }
    for (auto& f : w.frag) {
        f.tileMode = r.U32(); f.frameW = r.U32(); f.frameH = r.U32(); f.frameRate = r.U32(); f.curFrame = r.U32(); f.frameCount = r.U32();
        f.texture = r.Str();
        r.Raw(&f.z, 4); f.color = r.U32(); r.Raw(f.rect, 16);
    }
    for (;;) {
        uint32_t idx = r.U32();
        if (!r.ok) return false;
        if (idx == 0xFFFFFFFFu) break;
        if (idx > 100000) return false;
        w.strings.emplace_back(idx, r.Str());
    }
    uint32_t count = r.U32();
    if (!r.ok || count > 5000) return false;
    w.children.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t t = r.U32();
        if (!r.ok || !ReadNode(r, t, version, w.children[i], depth + 1)) return false;
    }
    if (type == GuiNode::Label || type == GuiNode::Edit) {
        w.strRef = r.U32(); w.fontName = r.Str(); w.fontFlags = r.U32(); w.fontSize = r.U32(); w.fontCharset = r.U32(); w.textColor = r.U32();
        w.textFrame = r.U8(); w.wrap = r.U8(); w.alignment = r.U32();
        for (auto& v : w.pad) v = r.U32();
        if (version >= 1) w.valign = r.U32();
    }
    return r.ok;
}
const GuiNode* FindIn(const std::vector<GuiNode>& v, const std::string& n) {
    for (const auto& c : v) {
        if (c.name == n) return &c;
        if (const GuiNode* f = FindIn(c.children, n)) return f;
    }
    return nullptr;
}
size_t CountIn(const std::vector<GuiNode>& v) { size_t k = v.size(); for (const auto& c : v) k += CountIn(c.children); return k; }
}  // namespace

const GuiNode* GuiNode::Child(const std::string& n) const {
    for (const auto& c : children) if (c.name == n) return &c;
    return nullptr;
}
const GuiNode* GuiNode::Find(const std::string& n) const { return FindIn(children, n); }
const GuiNode* GuiTree::Find(const std::string& n) const { return FindIn(roots, n); }
size_t GuiTree::Count() const { return CountIn(roots); }

bool ParseGuiTree(const uint8_t* data, size_t size, GuiTree& out, std::string* err) {
    out = GuiTree{};
    auto fail = [&](const char* m) { if (err) *err = m; return false; };
    Rd r{data, size};
    uint32_t head = 0;
    r.Raw(&head, 4);
    if (head == 0x20495547u) {   // "GUI "
        uint32_t t = r.U32();
        switch (t) {
            case 0x302E3156u: out.version = 1; break;
            case 0x312E3156u: out.version = 2; break;
            case 0x322E3156u: out.version = 3; break;
            case 0x332E3156u: out.version = 4; break;
            default: return fail("unknown GUI version");
        }
    } else {
        r.p = 0;   // headerless (version 0) file
    }
    out.name = r.Str();
    out.hidden = r.U8() != 0;
    uint32_t count = r.U32();
    if (!r.ok || count > 5000) return fail("bad header");
    out.roots.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t type = r.U32();
        if (!r.ok || !ReadNode(r, type, static_cast<int>(out.version), out.roots[i], 0)) return fail("bad widget record");
    }
    if (r.p != size && err) *err = "trailing bytes: " + std::to_string(size - r.p);
    return true;
}

}  // namespace f3::gd
