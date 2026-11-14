// mapinfo - Van Buren (Fallout 3 tech demo) world/logic format reader.
//
//   mapinfo map    <extracted-dir> <name> [--json] [--grid] [--stf English.stf]
//   mapinfo list   <extracted-dir>
//   mapinfo een    <file.een | extracted-dir/_grp/name.een> [--json] [--stf English.stf]
//   mapinfo amx    <file.scra|file.scrb> [--json]
//   mapinfo verify <extracted-dir> [--verbose]
//
// <extracted-dir> is the output of tools/grptool (extracted/<grp>/<name>.<ext>).
// Formats are documented in docs/formats/maps.md. No dependencies, C++17.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

struct Err : std::runtime_error {
    explicit Err(const std::string& s) : std::runtime_error(s) {}
};

// ---------------------------------------------------------------------------------------------
// small JSON value
// ---------------------------------------------------------------------------------------------
struct J {
    enum K { Null, Bool, Int, Num, Str, Arr, Obj } k = Null;
    bool b = false;
    long long i = 0;
    double d = 0;
    std::string s;
    std::vector<J> a;
    std::vector<std::pair<std::string, J>> o;
    J() {}
    static J B(bool v) { J j; j.k = Bool; j.b = v; return j; }
    static J I(long long v) { J j; j.k = Int; j.i = v; return j; }
    static J N(double v) { J j; j.k = Num; j.d = v; return j; }
    static J S(const std::string& v) { J j; j.k = Str; j.s = v; return j; }
    static J A() { J j; j.k = Arr; return j; }
    static J O() { J j; j.k = Obj; return j; }
    J& add(J v) { a.push_back(std::move(v)); return *this; }
    J& set(const std::string& key, J v) { o.emplace_back(key, std::move(v)); return *this; }
    bool scalarArr() const {
        if (k != Arr) return false;
        for (auto& e : a) if (e.k == Arr || e.k == Obj) return false;
        return true;
    }
};
static J jf3(const float* v) { J r = J::A(); for (int n = 0; n < 3; n++) r.add(J::N(v[n])); return r; }
static J jvec(const std::vector<u32>& v) { J r = J::A(); for (u32 x : v) r.add(J::I(x)); return r; }
static J jstrs(const std::vector<std::string>& v) { J r = J::A(); for (auto& x : v) r.add(J::S(x)); return r; }

static void jesc(const std::string& s, std::string& out) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) { char t[8]; snprintf(t, sizeof t, "\\u%04x", c); out += t; }
                else if (c >= 0x80) { // Windows-1252 -> UTF-8 (Latin-1 range is identical for our purposes)
                    out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F));
                } else out += (char)c;
        }
    }
    out += '"';
}
static void jdump(const J& j, std::string& out, int ind) {
    auto pad = [&](int n) { out.append((size_t)n * 2, ' '); };
    switch (j.k) {
        case J::Null: out += "null"; break;
        case J::Bool: out += j.b ? "true" : "false"; break;
        case J::Int: out += std::to_string(j.i); break;
        case J::Num: {
            if (!std::isfinite(j.d)) { out += "null"; break; }
            char t[40]; snprintf(t, sizeof t, "%.7g", j.d); out += t; break;
        }
        case J::Str: jesc(j.s, out); break;
        case J::Arr:
            if (j.a.empty()) { out += "[]"; break; }
            if (j.scalarArr()) {
                out += '[';
                for (size_t n = 0; n < j.a.size(); n++) { if (n) out += ", "; jdump(j.a[n], out, ind + 1); }
                out += ']';
            } else {
                out += "[\n";
                for (size_t n = 0; n < j.a.size(); n++) {
                    pad(ind + 1); jdump(j.a[n], out, ind + 1);
                    if (n + 1 < j.a.size()) out += ',';
                    out += '\n';
                }
                pad(ind); out += ']';
            }
            break;
        case J::Obj:
            if (j.o.empty()) { out += "{}"; break; }
            out += "{\n";
            for (size_t n = 0; n < j.o.size(); n++) {
                pad(ind + 1); jesc(j.o[n].first, out); out += ": "; jdump(j.o[n].second, out, ind + 1);
                if (n + 1 < j.o.size()) out += ',';
                out += '\n';
            }
            pad(ind); out += '}';
            break;
    }
}
static std::string jstr(const J& j) { std::string s; jdump(j, s, 0); s += '\n'; return s; }

// ---------------------------------------------------------------------------------------------
// binary reader
// ---------------------------------------------------------------------------------------------
typedef std::vector<u8> Bytes;
static Bytes readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw Err("cannot open " + p.string());
    f.seekg(0, std::ios::end);
    std::streamoff n = f.tellg();
    f.seekg(0);
    Bytes b((size_t)n);
    if (n) f.read((char*)b.data(), n);
    return b;
}
struct Rd {
    const u8* b; size_t n; size_t o;
    Rd(const u8* p, size_t len) : b(p), n(len), o(0) {}
    explicit Rd(const Bytes& v) : b(v.data()), n(v.size()), o(0) {}
    void need(size_t k) const { if (o + k > n) throw Err("read past end (need " + std::to_string(k) + " at " + std::to_string(o) + "/" + std::to_string(n) + ")"); }
    size_t left() const { return n - o; }
    bool eof() const { return o == n; }
    u8 r8() { need(1); return b[o++]; }
    u16 r16() { need(2); u16 v = (u16)(b[o] | (b[o + 1] << 8)); o += 2; return v; }
    u32 r32() { need(4); u32 v = b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | ((u32)b[o + 3] << 24); o += 4; return v; }
    float rf() { u32 v = r32(); float f; memcpy(&f, &v, 4); return f; }
    std::string str() { u16 l = r16(); need(l); std::string s((const char*)b + o, l); o += l; return s; }
    Rd sub(size_t len) { need(len); Rd r(b + o, len); o += len; return r; }
    void skip(size_t k) { need(k); o += k; }
    std::string tag() { need(4); std::string s((const char*)b + o, 4); o += 4; return s; }
};
static std::string hex(const u8* p, size_t n, size_t maxn = 4096) {
    static const char* H = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n && i < maxn; i++) { s += H[p[i] >> 4]; s += H[p[i] & 15]; }
    if (n > maxn) s += "...";
    return s;
}
static std::vector<std::string> scanStrings(const u8* p, size_t n) {
    std::vector<std::string> out;
    for (size_t o = 0; o + 2 <= n;) {
        u16 l = (u16)(p[o] | (p[o + 1] << 8));
        if (l >= 3 && l <= 96 && o + 2 + l <= n) {
            bool ok = true;
            for (size_t k = 0; k < l; k++) if (p[o + 2 + k] < 32 || p[o + 2 + k] > 126) { ok = false; break; }
            if (ok) { out.emplace_back((const char*)p + o + 2, l); o += 2 + l; continue; }
        }
        o++;
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// string table (English.stf), optional
// ---------------------------------------------------------------------------------------------
struct Stf {
    Bytes data; u32 count = 0; bool ok = false;
    void load(const fs::path& p) {
        data = readFile(p); Rd r(data);
        if (r.r32() != 3) throw Err("stf version");
        r.r32(); count = r.r32(); ok = true;
    }
    std::string get(u32 id) const {  // GENT ids are 1-based; 0 = none
        if (!ok || id == 0 || id > count) return "";
        size_t rec = 12 + 16 * (size_t)(id - 1);
        Rd r(data.data() + rec, 8);
        u32 off = r.r32(), len = r.r32();
        if ((size_t)off + len > data.size()) return "";
        return std::string((const char*)data.data() + off, len);
    }
};

// ---------------------------------------------------------------------------------------------
// EEOV - entity override block (shared by .een templates and map instances)
// ---------------------------------------------------------------------------------------------
struct Eeov {
    u32 ver = 0;
    std::string inst;
    u8 b3[3] = {0, 0, 0};
    u32 team = 0, B = 0;
    std::string dialogScript, aiScript;
    std::vector<std::pair<std::string, u32>> pairs;
    u8 f179 = 0; float scale = 1; std::string skin;
    std::vector<std::string> fx;
    u8 f178 = 0, f17a = 0;
    std::vector<std::string> inv;
};
static Eeov parseEeov(Rd& r) {  // r positioned at the chunk header
    Eeov e;
    if (r.tag() != "EEOV") throw Err("EEOV tag expected");
    e.ver = r.r32();
    u32 size = r.r32();
    if (size < 12) throw Err("EEOV size");
    if (e.ver != 1 && e.ver != 2) throw Err("EEOV version " + std::to_string(e.ver));
    Rd b = r.sub(size - 12);
    e.inst = b.str();
    for (int n = 0; n < 3; n++) e.b3[n] = b.r8();
    e.team = b.r32(); e.B = b.r32();
    e.dialogScript = b.str(); e.aiScript = b.str();
    u32 np = b.r32();
    if (np > 4096) throw Err("EEOV pair count");
    for (u32 n = 0; n < np; n++) { std::string s = b.str(); u32 v = b.r32(); e.pairs.emplace_back(s, v); }
    e.f179 = b.r8(); e.scale = b.rf(); e.skin = b.str();
    u8 nf = b.r8();
    for (u8 n = 0; n < nf; n++) e.fx.push_back(b.str());
    e.f178 = b.r8(); e.f17a = b.r8();
    if (e.ver >= 2) {
        u32 ni = b.r32();
        if (ni > 4096) throw Err("EEOV inventory count");
        for (u32 n = 0; n < ni; n++) e.inv.push_back(b.str());
    }
    if (!b.eof()) throw Err("EEOV trailing bytes: " + std::to_string(b.left()));
    return e;
}
static J eeovJson(const Eeov& e) {
    J j = J::O();
    j.set("block_version", J::I(e.ver));
    j.set("instance_name", J::S(e.inst));
    j.set("team", J::I(e.team));
    if (!e.dialogScript.empty()) j.set("dialog_script", J::S(e.dialogScript));
    if (!e.aiScript.empty()) j.set("script", J::S(e.aiScript));
    if (!e.skin.empty()) j.set("skin", J::S(e.skin));
    j.set("scale", J::N(e.scale));
    if (!e.fx.empty()) j.set("effects", jstrs(e.fx));
    if (!e.inv.empty()) j.set("inventory", jstrs(e.inv));
    if (!e.pairs.empty()) {
        J p = J::A();
        for (auto& x : e.pairs) { J q = J::O(); q.set("name", J::S(x.first)); q.set("value", J::I(x.second)); p.add(q); }
        j.set("named_values", p);
    }
    J fl = J::O();
    fl.set("b0_b1_b2", J::A().add(J::I(e.b3[0])).add(J::I(e.b3[1])).add(J::I(e.b3[2])));
    fl.set("u32_B", J::I(e.B));
    fl.set("f179", J::I(e.f179)); fl.set("f178", J::I(e.f178)); fl.set("f17a", J::I(e.f17a));
    j.set("raw_flags", fl);
    return j;
}

// ---------------------------------------------------------------------------------------------
// .een entity templates
// ---------------------------------------------------------------------------------------------
struct RawChunk { std::string tag; u32 ver = 0; Bytes data; };
struct Gwam { u32 m[11]; u8 b0; u32 a; u8 b1; u32 bb; u8 b2, b3; u32 c, d; std::string s; u8 e, f; };
struct Gcre {
    u32 special[7]; u32 st[8];
    std::vector<std::pair<u32, u32>> tab13, tab82;
    std::vector<u32> l3, l4; u32 u274 = 0;
    std::string portrait; u32 u294 = 0; std::string s298; u32 u2b4 = 0; u8 b2b8 = 0;
    std::vector<std::string> sockA;
    std::vector<std::pair<std::string, std::string>> sockPairs;
    u8 sockVis[12];
    u32 nModes = 0; u32 unc[8]; u32 g6a[6], g6b[6];
    std::vector<std::string> inv;
    std::vector<Gwam> modes;
};
struct Een {
    std::string model, icon1, icon2; u8 flags5[5];
    Eeov ov;
    u32 gent[8]; bool hasGent = false;
    bool hasGcre = false; Gcre gcre;
    std::vector<RawChunk> other;   // GITM GIWP GIAR GIAM GOBJ GDOR GCON
};
static Gwam parseGwam(Rd& r) {
    Gwam g;
    if (r.tag() != "GWAM") throw Err("GWAM tag expected");
    u32 ver = r.r32(); u32 size = r.r32();
    if (ver != 5) throw Err("GWAM version " + std::to_string(ver));
    Rd b = r.sub(size - 12);
    for (int n = 0; n < 11; n++) g.m[n] = b.r32();
    g.b0 = b.r8(); g.a = b.r32(); g.b1 = b.r8(); g.bb = b.r32(); g.b2 = b.r8(); g.b3 = b.r8();
    g.c = b.r32(); g.d = b.r32(); g.s = b.str(); g.e = b.r8(); g.f = b.r8();
    if (!b.eof()) throw Err("GWAM trailing bytes");
    return g;
}
static Gcre parseGcre(Rd b, u32 ver) {
    // Layout derived from the F3.exe GameCreature reader (FUN_0040e360 and callees), data version 4.
    if (ver != 4) throw Err("GCRE version " + std::to_string(ver) + " not supported");
    Gcre g;
    for (int n = 0; n < 7; n++) g.special[n] = b.r32();
    for (int n = 0; n < 8; n++) g.st[n] = b.r32();
    u32 c = b.r32(); if (c > 1000) throw Err("tab13"); for (u32 n = 0; n < c; n++) { u32 x = b.r32(), y = b.r32(); g.tab13.emplace_back(x, y); }
    c = b.r32(); if (c > 1000) throw Err("tab82"); for (u32 n = 0; n < c; n++) { u32 x = b.r32(), y = b.r32(); g.tab82.emplace_back(x, y); }
    c = b.r32(); if (c > 1000) throw Err("l3"); for (u32 n = 0; n < c; n++) g.l3.push_back(b.r32());
    c = b.r32(); if (c > 1000) throw Err("l4"); for (u32 n = 0; n < c; n++) g.l4.push_back(b.r32());
    g.u274 = b.r32();
    g.portrait = b.str(); g.u294 = b.r32(); g.s298 = b.str(); g.u2b4 = b.r32(); g.b2b8 = b.r8();
    for (int n = 0; n < 12; n++) g.sockA.push_back(b.str());
    for (int n = 0; n < 12; n++) { std::string x = b.str(), y = b.str(); g.sockPairs.emplace_back(x, y); }
    for (int n = 0; n < 12; n++) g.sockVis[n] = b.r8();
    g.nModes = b.r32();
    for (int n = 0; n < 8; n++) g.unc[n] = b.r32();
    for (int n = 0; n < 6; n++) g.g6a[n] = b.r32();
    for (int n = 0; n < 6; n++) g.g6b[n] = b.r32();
    c = b.r32(); if (c > 4096) throw Err("inv"); for (u32 n = 0; n < c; n++) g.inv.push_back(b.str());
    for (u32 n = 0; n < g.nModes; n++) g.modes.push_back(parseGwam(b));
    if (!b.eof()) throw Err("GCRE trailing bytes: " + std::to_string(b.left()));
    return g;
}
static Een parseEen(const Bytes& data) {
    Een e;
    Rd r(data);
    bool gotEen2 = false;
    while (!r.eof()) {
        std::string tag = r.tag(); u32 ver = r.r32(); u32 size = r.r32();
        if (size < 12) throw Err("chunk " + tag + " size");
        Rd b = r.sub(size - 12);
        if (tag == "EEN2") {
            e.model = b.str(); e.icon1 = b.str(); e.icon2 = b.str();
            for (int n = 0; n < 5; n++) e.flags5[n] = b.r8();
            e.ov = parseEeov(b);
            if (!b.eof()) throw Err("EEN2 trailing bytes");
            gotEen2 = true;
        } else if (tag == "GENT") {
            if (b.left() != 32) throw Err("GENT size");
            for (int n = 0; n < 8; n++) e.gent[n] = b.r32();
            e.hasGent = true;
        } else if (tag == "GCRE") {
            e.gcre = parseGcre(b, ver); e.hasGcre = true;
        } else {
            RawChunk rc; rc.tag = tag; rc.ver = ver; rc.data.assign(b.b, b.b + b.n);
            e.other.push_back(std::move(rc));
        }
    }
    if (!gotEen2) throw Err("no EEN2 chunk");
    return e;
}
static J gwamJson(const Gwam& g) {
    J j = J::O();
    J m = J::A(); for (int n = 0; n < 11; n++) m.add(J::I(g.m[n]));
    j.set("u32_0_to_10", m);
    j.set("damage_min_guess", J::I(g.m[9])); j.set("damage_max_guess", J::I(g.m[10]));
    j.set("b0", J::I(g.b0)); j.set("a", J::I(g.a)); j.set("b1", J::I(g.b1)); j.set("b", J::I(g.bb));
    j.set("b2", J::I(g.b2)); j.set("b3", J::I(g.b3)); j.set("c", J::I(g.c)); j.set("d", J::I(g.d));
    j.set("str", J::S(g.s)); j.set("e", J::I(g.e)); j.set("f", J::I(g.f));
    return j;
}
static J eenJson(const Een& e, const Stf* stf, const std::string& name) {
    J j = J::O();
    j.set("template", J::S(name));
    j.set("model", J::S(e.model));
    if (!e.icon1.empty()) j.set("icon_inventory", J::S(e.icon1));
    if (!e.icon2.empty()) j.set("icon_action", J::S(e.icon2));
    { J f = J::A(); for (int n = 0; n < 5; n++) f.add(J::I(e.flags5[n])); j.set("een2_flags", f); }
    j.set("defaults", eeovJson(e.ov));
    if (e.hasGent) {
        J g = J::O();
        g.set("name_str_id", J::I(e.gent[0])); g.set("desc_str_id", J::I(e.gent[1])); g.set("type_str_id", J::I(e.gent[2]));
        if (stf && stf->ok) {
            g.set("name", J::S(stf->get(e.gent[0]))); g.set("description", J::S(stf->get(e.gent[1])));
            g.set("type_name", J::S(stf->get(e.gent[2])));
        }
        g.set("words_3_to_7", J::A().add(J::I(e.gent[3])).add(J::I(e.gent[4])).add(J::I(e.gent[5])).add(J::I(e.gent[6])).add(J::I(e.gent[7])));
        j.set("GENT", g);
    }
    if (e.hasGcre) {
        const Gcre& c = e.gcre; J g = J::O();
        J sp = J::A(); for (int n = 0; n < 7; n++) sp.add(J::I(c.special[n]));
        g.set("special", sp);
        J st = J::A(); for (int n = 0; n < 8; n++) st.add(J::I(c.st[n]));
        g.set("stat_words_7_to_14", st);
        g.set("level_guess", J::I(c.st[0])); g.set("hp_guess", J::I(c.st[4]));
        { J t = J::A(); for (auto& p : c.tab13) t.add(J::A().add(J::I(p.first)).add(J::I(p.second))); g.set("derived_stat_overrides", t); }
        { J t = J::A(); for (auto& p : c.tab82) t.add(J::A().add(J::I(p.first)).add(J::I(p.second))); g.set("skill_overrides", t); }
        g.set("list3", jvec(c.l3)); g.set("list4", jvec(c.l4)); g.set("u274", J::I(c.u274));
        g.set("portrait", J::S(c.portrait));
        g.set("u294", J::I(c.u294));
        J sk = J::A();
        for (int n = 0; n < 12; n++) {
            J s = J::O(); s.set("socket", J::I(n)); s.set("name_a", J::S(c.sockA[n]));
            s.set("model", J::S(c.sockPairs[n].first)); s.set("texture", J::S(c.sockPairs[n].second));
            s.set("visible", J::I(c.sockVis[n])); sk.add(s);
        }
        g.set("sockets", sk);
        { J t = J::A(); for (int n = 0; n < 8; n++) t.add(J::I(c.unc[n])); g.set("base_uncalled_chances_sum100", t); }
        { J t = J::A(); for (int n = 0; n < 6; n++) t.add(J::I(c.g6a[n])); g.set("six_words_a", t); }
        { J t = J::A(); for (int n = 0; n < 6; n++) t.add(J::I(c.g6b[n])); g.set("six_words_b", t); }
        g.set("equipment_and_inventory", jstrs(c.inv));
        { J t = J::A(); for (auto& m : c.modes) t.add(gwamJson(m)); g.set("attack_modes", t); }
        j.set("GCRE", g);
    }
    for (auto& rc : e.other) {
        J g = J::O(); g.set("version", J::I(rc.ver)); g.set("size", J::I(rc.data.size()));
        if (rc.data.size() % 4 == 0 && rc.data.size() <= 64) {
            J w = J::A(); Rd r(rc.data); while (!r.eof()) w.add(J::I(r.r32())); g.set("words", w);
        }
        auto st = scanStrings(rc.data.data(), rc.data.size());
        if (!st.empty()) g.set("strings", jstrs(st));
        g.set("hex", J::S(hex(rc.data.data(), rc.data.size(), 512)));
        j.set(rc.tag, g);
    }
    return j;
}

// ---------------------------------------------------------------------------------------------
// .mapdata - per-cell RLE grid
// ---------------------------------------------------------------------------------------------
struct Grid {
    u32 w = 0, h = 0;
    std::vector<u8> cell;           // w*h
    std::map<int, u32> hist;
    u32 runs = 0;
};
static Grid parseGrid(const Bytes& d) {
    Rd r(d); Grid g;
    g.w = r.r32(); g.h = r.r32();
    if (g.w == 0 || g.h == 0 || (unsigned long long)g.w * g.h > 64000000ull) throw Err("grid dims");
    g.cell.resize((size_t)g.w * g.h);
    for (u32 y = 0; y < g.h; y++) {
        u32 x = 0;
        while (x < g.w) {
            u32 len = r.r32(); u8 v = r.r8(); g.runs++;
            if (len == 0 || x + len > g.w) throw Err("run overflows row " + std::to_string(y));
            memset(&g.cell[(size_t)y * g.w + x], v, len);
            g.hist[v] += len; x += len;
        }
    }
    if (!r.eof()) throw Err("mapdata trailing bytes: " + std::to_string(r.left()));
    return g;
}

// ---------------------------------------------------------------------------------------------
// .tre - render/level container
// ---------------------------------------------------------------------------------------------
struct Tre {
    u32 headWords[4] = {0, 0, 0, 0};
    std::vector<std::pair<std::string, u32>> chunks;   // top-level
    u32 lightCount = 0;
    std::vector<std::vector<float>> lights;            // 16 words each
    u32 lvlW = 0, lvlH = 0; float lvlA = 0, lvlB = 0; u32 lvl0Size = 0; u32 lvlChunks = 0;
    u32 matCount = 0; std::vector<std::string> mats;
    u32 txtVer = 0, txtWord2 = 0; std::vector<std::string> texs;
    u32 vtxCount = 0, vtxBytes = 0; bool vtxStride44 = false;
    bool hasTreeBox = false; float treeBox[6] = {0, 0, 0, 0, 0, 0}; u32 treeSize = 0;
    u32 tail = 0; std::string tailTag;
};
static Tre parseTre(const Bytes& d) {
    Tre t; Rd r(d);
    if (r.tag() != "8TRE") throw Err("not a TRE file");
    u32 total = r.r32();
    if ((size_t)total + 8 > d.size()) throw Err("TRE size beyond file");
    Rd body(d.data() + 8, total);
    while (!body.eof()) {
        std::string tag = body.tag(); u32 sz = body.r32();
        Rd c = body.sub(sz);
        t.chunks.emplace_back(tag, sz);
        if (tag == "HEAD") {
            if (sz != 16) throw Err("HEAD size");
            for (int n = 0; n < 4; n++) t.headWords[n] = c.r32();
        } else if (tag == "LGTD" || tag == "LVLD" || tag == "MATD") {
            while (!c.eof()) {
                std::string st = c.tag(); u32 ss = c.r32(); Rd s = c.sub(ss);
                if (tag == "LGTD") {
                    if (st == "INFO") t.lightCount = s.r32();
                    else if (st == "LGTR") { std::vector<float> w; while (!s.eof()) { u32 v = s.r32(); float f; memcpy(&f, &v, 4); w.push_back(f); } t.lights.push_back(w); }
                } else if (tag == "LVLD") {
                    if (st == "INFO") { t.lvlW = s.r32(); t.lvlH = s.r32(); t.lvlA = s.rf(); t.lvlB = s.rf(); }
                    else if (st.compare(0, 3, "LVL") == 0) { t.lvlChunks++; if (st == "LVL0") t.lvl0Size = ss; }
                } else {  // MATD
                    if (st == "INFO") t.matCount = s.r32();
                    else if (st == "MATR") {
                        s.r32();
                        std::string nm((const char*)s.b + s.o, std::min<size_t>(32, s.left()));
                        nm = nm.substr(0, nm.find('\0'));
                        t.mats.push_back(nm);
                    }
                }
            }
        } else if (tag == "TXTD") {
            t.txtVer = c.r32(); t.txtWord2 = c.r32();
            while (!c.eof()) {
                if (c.tag() != "TXTR") throw Err("TXTR expected");
                u32 l = c.r32(); Rd s = c.sub(l);
                std::string nm((const char*)s.b, l);
                nm = nm.substr(0, nm.find('\0'));
                t.texs.push_back(nm);
            }
            if (!c.eof()) throw Err("TXTD trailing");
        } else if (tag == "VTXD") {
            c.r32();
            if (c.tag() != "VTXB") throw Err("VTXB expected");
            u32 vs = c.r32(); t.vtxBytes = vs;
            t.vtxCount = c.r32();
            t.vtxStride44 = ((unsigned long long)t.vtxCount * 44 + 44 == vs);
        } else if (tag == "TREE") {
            t.treeSize = sz;
            if (sz >= 8 + 8 + 24) {
                std::string nt = c.tag(); c.r32();
                if (nt == "NODE" && c.tag() == "INFO") {
                    c.r32();
                    for (int n = 0; n < 6; n++) t.treeBox[n] = c.rf();
                    t.hasTreeBox = true;
                }
            }
        }
    }
    t.tail = (u32)(d.size() - 8 - total);
    if (t.tail >= 4) t.tailTag = std::string((const char*)d.data() + 8 + total, 4);
    return t;
}

// ---------------------------------------------------------------------------------------------
// .emap - entities / logic regions
// ---------------------------------------------------------------------------------------------
struct EntryPoint { u8 id; std::vector<std::array<float, 3>> pos; std::vector<float> yaw; };
struct Region {
    u32 kind = 0; std::vector<std::array<float, 3>> pts;
    bool hasBlk = false; u32 blkId = 0; u8 blkRgb[3] = {0, 0, 0};
    bool hasScript = false; std::string script; u32 scriptExtra = 0;
    bool hasTrans = false; std::string tmap; u8 tentry = 0, tflag = 0;
};
struct PathNode { float pos[3]; float extra[3]; };
struct PathD { std::string name; std::vector<PathNode> nodes; };
struct Sound { std::string name; float pos[3]; std::string file; u8 a, b; };
struct Effect { std::string name; float pos[3], rot[3]; u8 a; std::string veg; u8 b; };
struct Icon { float pos[3]; std::string icon; u32 type; };
struct Ent {
    std::string tmpl, type, name; float pos[3], rot[3]; u8 flag; Eeov ov;
};
struct Emap {
    u32 ver = 0, hdrSize = 0;
    std::string tre, rle, am; Bytes hdrTail;
    std::vector<EntryPoint> eps; bool hasCam = false; float cam[4] = {0, 0, 0, 0};
    std::vector<Region> regions; std::vector<PathD> paths; std::vector<Sound> sounds;
    std::vector<Effect> effects; std::vector<Icon> icons; std::vector<Ent> ents;
    std::vector<u32> emnp; std::vector<RawChunk> other;
    std::map<std::string, u32> chunkCount;
};
static Emap parseEmap(const Bytes& d) {
    Emap m; Rd r(d);
    if (r.tag() != "EMAP") throw Err("not an EMAP file");
    m.ver = r.r32(); m.hdrSize = r.r32(); r.r32();
    if (m.ver != 5) throw Err("EMAP version " + std::to_string(m.ver));
    m.tre = r.str(); m.rle = r.str(); m.am = r.str();
    if (m.hdrSize < r.o || m.hdrSize > d.size()) throw Err("EMAP header size");
    m.hdrTail.assign(d.data() + r.o, d.data() + m.hdrSize);
    r.o = m.hdrSize;
    Region* last = nullptr;
    while (!r.eof()) {
        std::string tag = r.tag(); u32 ver = r.r32(); u32 size = r.r32();
        if (size < 12) throw Err("chunk " + tag + " size");
        Rd b = r.sub(size - 12);
        m.chunkCount[tag]++;
        auto done = [&](const char* what) { if (!b.eof()) throw Err(std::string(what) + " trailing bytes: " + std::to_string(b.left())); };
        if (tag == "EME2") {
            Ent e;
            e.name = b.str();
            size_t dot = e.name.rfind('.');
            if (dot == std::string::npos) throw Err("EME2 template without type suffix");
            e.tmpl = e.name.substr(0, dot); e.type = e.name.substr(dot + 1);
            for (int n = 0; n < 3; n++) e.pos[n] = b.rf();
            for (int n = 0; n < 3; n++) e.rot[n] = b.rf();
            e.flag = b.r8();
            e.ov = parseEeov(b);
            done("EME2");
            m.ents.push_back(std::move(e));
        } else if (tag == "EMEP") {
            EntryPoint ep; ep.id = b.r8();
            if ((b.left() % 16) != 0) throw Err("EMEP size");
            size_t n = b.left() / 16;
            for (size_t k = 0; k < n; k++) { std::array<float, 3> p; for (int q = 0; q < 3; q++) p[q] = b.rf(); ep.pos.push_back(p); }
            for (size_t k = 0; k < n; k++) ep.yaw.push_back(b.rf());
            m.eps.push_back(std::move(ep));
        } else if (tag == "ECAM") {
            for (int n = 0; n < 4; n++) m.cam[n] = b.rf();
            m.hasCam = true; done("ECAM");
        } else if (tag == "EMNP") {
            while (!b.eof()) m.emnp.push_back(b.r32());
        } else if (tag == "EMTR") {
            Region g; g.kind = b.r32(); u32 n = b.r32();
            if (n > 100000) throw Err("EMTR count");
            for (u32 k = 0; k < n; k++) { std::array<float, 3> p; for (int q = 0; q < 3; q++) p[q] = b.rf(); g.pts.push_back(p); }
            done("EMTR");
            m.regions.push_back(std::move(g)); last = &m.regions.back();
        } else if (tag == "EBTR") {
            if (!last) throw Err("EBTR before EMTR");
            last->hasBlk = true; last->blkId = b.r32();
            for (int n = 0; n < 3; n++) last->blkRgb[n] = b.r8();
            done("EBTR");
        } else if (tag == "ESTR") {
            if (!last) throw Err("ESTR before EMTR");
            last->hasScript = true; last->script = b.str(); last->scriptExtra = b.r32(); done("ESTR");
        } else if (tag == "ETTR") {
            if (!last) throw Err("ETTR before EMTR");
            last->hasTrans = true; last->tmap = b.str(); last->tentry = b.r8(); last->tflag = b.r8(); done("ETTR");
        } else if (tag == "EPTH") {
            PathD p; p.name = b.str(); u32 n = b.r32();
            if (n > 100000) throw Err("EPTH count");
            for (u32 k = 0; k < n; k++) { PathNode pn; for (int q = 0; q < 3; q++) pn.pos[q] = b.rf(); for (int q = 0; q < 3; q++) pn.extra[q] = b.rf(); p.nodes.push_back(pn); }
            done("EPTH"); m.paths.push_back(std::move(p));
        } else if (tag == "EMSD") {
            Sound s; s.name = b.str(); for (int q = 0; q < 3; q++) s.pos[q] = b.rf();
            s.file = b.str(); s.a = b.r8(); s.b = b.r8(); done("EMSD"); m.sounds.push_back(std::move(s));
        } else if (tag == "EMEF") {
            Effect e; e.name = b.str(); for (int q = 0; q < 3; q++) e.pos[q] = b.rf();
            for (int q = 0; q < 3; q++) e.rot[q] = b.rf();
            e.a = b.r8(); e.veg = b.str(); e.b = b.r8(); done("EMEF"); m.effects.push_back(std::move(e));
        } else if (tag == "EMNO") {
            Icon c; for (int q = 0; q < 3; q++) c.pos[q] = b.rf();
            c.icon = b.str(); c.type = b.r32(); done("EMNO"); m.icons.push_back(std::move(c));
        } else {
            RawChunk rc; rc.tag = tag; rc.ver = ver; rc.data.assign(b.b, b.b + b.n); m.other.push_back(std::move(rc));
        }
    }
    return m;
}

// ---------------------------------------------------------------------------------------------
// AMX (Pawn) scripts
// ---------------------------------------------------------------------------------------------
struct Amx {
    u32 size; u16 magic; u8 fver, aver; u16 flags, defsize;
    int32_t cod, dat, hea, stp, cip, pub, nat, lib, pvar, tags, nt;
    std::vector<std::pair<u32, std::string>> publics, natives;
    bool sizeOk;
};
static Amx parseAmx(const Bytes& d) {
    Rd r(d); Amx a;
    a.size = r.r32(); a.magic = r.r16(); a.fver = r.r8(); a.aver = r.r8(); a.flags = r.r16(); a.defsize = r.r16();
    int32_t* f[] = {&a.cod, &a.dat, &a.hea, &a.stp, &a.cip, &a.pub, &a.nat, &a.lib, &a.pvar, &a.tags, &a.nt};
    for (auto p : f) *p = (int32_t)r.r32();
    if (a.magic != 0xF1E0) throw Err("not an AMX file (magic)");
    a.sizeOk = (a.size == d.size());
    if (a.defsize != 40) throw Err("unexpected AMX defsize " + std::to_string(a.defsize));
    auto table = [&](int32_t from, int32_t to, std::vector<std::pair<u32, std::string>>& out) {
        if (from < 0 || to < from || (size_t)to > d.size()) throw Err("AMX table bounds");
        for (int32_t o = from; o + 40 <= to; o += 40) {
            Rd e(d.data() + o, 40);
            u32 addr = e.r32();
            std::string nm((const char*)e.b + 4, 36); nm = nm.substr(0, nm.find('\0'));
            out.emplace_back(addr, nm);
        }
    };
    table(a.pub, a.nat, a.publics);
    table(a.nat, a.lib, a.natives);
    if (a.cod < 0 || a.dat < a.cod || a.hea < a.dat || (size_t)a.hea > d.size()) throw Err("AMX section bounds");
    return a;
}
static J amxJson(const Amx& a, const std::string& name) {
    J j = J::O();
    j.set("file", J::S(name)); j.set("size", J::I(a.size)); j.set("size_matches_file", J::B(a.sizeOk));
    j.set("magic", J::S("0xF1E0")); j.set("file_version", J::I(a.fver)); j.set("amx_version", J::I(a.aver));
    j.set("flags", J::I(a.flags)); j.set("defsize", J::I(a.defsize));
    j.set("code_bytes", J::I(a.dat - a.cod)); j.set("data_bytes", J::I(a.hea - a.dat));
    j.set("stack_heap_bytes", J::I(a.stp - a.hea)); j.set("main_cip", J::I(a.cip));
    J p = J::A(); for (auto& x : a.publics) p.add(J::O().set("name", J::S(x.second)).set("code_offset", J::I(x.first)));
    j.set("publics", p);
    std::vector<std::string> n; for (auto& x : a.natives) n.push_back(x.second);
    j.set("natives", jstrs(n));
    return j;
}

// ---------------------------------------------------------------------------------------------
// paths / resolving
// ---------------------------------------------------------------------------------------------
static const char* grpForType(const std::string& t) {
    static const std::map<std::string, const char*> m = {
        {"CRT", "_CRT"}, {"CON", "_CON"}, {"USE", "_USE"}, {"DOR", "_DOR"},
        {"ITM", "_ITM"}, {"WEA", "_WEA"}, {"ARM", "_ARM"}, {"AMO", "_AMO"}};
    auto it = m.find(t);
    return it == m.end() ? nullptr : it->second;
}
static std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
static fs::path findMapFile(const fs::path& root, const std::string& base, const char* ext) {
    // the original engine opens files case-insensitively (emap says "Kihantest", the file is "KihanTest")
    std::string want = lower(base + ext);
    for (const char* g : {"Maps", "Engine"}) {
        fs::path dir = root / g;
        if (!fs::exists(dir)) continue;
        for (auto& e : fs::directory_iterator(dir))
            if (lower(e.path().filename().string()) == want) return e.path();
    }
    return fs::path();
}
// emap file stem by tre base name ("04_Tutorial_Vault" -> Maps/00_04_Tutorial_Vault.emap)
struct MapIndex { std::map<std::string, fs::path> emapByTre, emapByStem; std::set<std::string> treBases; };
static MapIndex buildIndex(const fs::path& root) {
    MapIndex ix;
    for (const char* g : {"Maps", "Engine"}) {
        fs::path dir = root / g;
        if (!fs::exists(dir)) continue;
        for (auto& e : fs::directory_iterator(dir)) {
            std::string ext = e.path().extension().string(), stem = e.path().stem().string();
            if (ext == ".tre") ix.treBases.insert(lower(stem));
            if (ext == ".emap") {
                ix.emapByStem[lower(stem)] = e.path();
                try {
                    Bytes d = readFile(e.path()); Rd r(d);
                    r.skip(16);
                    std::string t = r.str();
                    if (t.size() > 2 && t.compare(t.size() - 2, 2, ".8") == 0) t.resize(t.size() - 2);
                    ix.emapByTre[lower(t)] = e.path();
                } catch (...) {}
            }
        }
    }
    return ix;
}

struct MapBundle {
    std::string name; bool hasEmap = false, hasGrid = false, hasTre = false;
    Emap emap; Grid grid; Tre tre;
    std::string emapPath, gridPath, trePath, treBase;
    std::vector<std::string> errors;
};
static MapBundle loadMap(const fs::path& root, const MapIndex& ix, const std::string& name) {
    MapBundle mb; mb.name = name;
    std::string treBase = name;
    fs::path emapP;
    std::string ln = lower(name);
    if (ix.emapByStem.count(ln)) emapP = ix.emapByStem.at(ln);
    else if (ix.emapByTre.count(ln)) emapP = ix.emapByTre.at(ln);
    if (!emapP.empty()) {
        try {
            mb.emap = parseEmap(readFile(emapP)); mb.hasEmap = true; mb.emapPath = emapP.string();
            treBase = mb.emap.tre; if (treBase.size() > 2 && treBase.compare(treBase.size() - 2, 2, ".8") == 0) treBase.resize(treBase.size() - 2);
        } catch (std::exception& e) { mb.errors.push_back(std::string("emap: ") + e.what()); }
    }
    mb.treBase = treBase;
    fs::path gp = findMapFile(root, treBase, ".mapdata"), tp = findMapFile(root, treBase, ".tre");
    if (!gp.empty()) {
        try { mb.grid = parseGrid(readFile(gp)); mb.hasGrid = true; mb.gridPath = gp.string(); }
        catch (std::exception& e) { mb.errors.push_back(std::string("mapdata: ") + e.what()); }
    }
    if (!tp.empty()) {
        try { mb.tre = parseTre(readFile(tp)); mb.hasTre = true; mb.trePath = tp.string(); }
        catch (std::exception& e) { mb.errors.push_back(std::string("tre: ") + e.what()); }
    }
    if (emapP.empty() && gp.empty() && tp.empty()) mb.errors.push_back("map not found: " + name);
    return mb;
}

// ---------------------------------------------------------------------------------------------
// map -> JSON / text
// ---------------------------------------------------------------------------------------------
static J mapJson(const MapBundle& mb, bool withGrid) {
    J j = J::O();
    j.set("map", J::S(mb.name));
    j.set("tre_base", J::S(mb.treBase));
    if (mb.hasGrid) {
        const Grid& g = mb.grid;
        J o = J::O();
        o.set("width_cells", J::I(g.w)); o.set("height_cells", J::I(g.h));
        o.set("cells_per_world_unit", J::I(2));
        o.set("world_width", J::N(g.w / 2.0)); o.set("world_height", J::N(g.h / 2.0));
        J h = J::O(); for (auto& p : g.hist) { char k[8]; snprintf(k, sizeof k, "0x%02x", p.first); h.set(k, J::I(p.second)); }
        o.set("value_histogram", h);
        o.set("rle_runs", J::I(g.runs));
        if (withGrid) {
            J rows = J::A();
            for (u32 y = 0; y < g.h; y++) {
                J row = J::A(); u32 x = 0;
                while (x < g.w) {
                    u8 v = g.cell[(size_t)y * g.w + x]; u32 e = x;
                    while (e < g.w && g.cell[(size_t)y * g.w + e] == v) e++;
                    row.add(J::A().add(J::I(v)).add(J::I(e - x))); x = e;
                }
                rows.add(row);
            }
            o.set("rows_rle_value_count", rows);
        }
        j.set("grid", o);
    }
    if (mb.hasTre) {
        const Tre& t = mb.tre; J o = J::O();
        o.set("head_words", J::A().add(J::I(t.headWords[0])).add(J::I(t.headWords[1])).add(J::I(t.headWords[2])).add(J::I(t.headWords[3])));
        { J c = J::A(); for (auto& x : t.chunks) c.add(J::O().set("tag", J::S(x.first)).set("size", J::I(x.second))); o.set("chunks", c); }
        o.set("light_count", J::I(t.lightCount));
        {
            J l = J::A();
            for (auto& w : t.lights) {
                if (w.size() != 16) continue;
                u32 ty, md; memcpy(&ty, &w[0], 4); memcpy(&md, &w[1], 4);
                J q = J::O();
                q.set("type", J::I(ty)); q.set("mode", J::I(md));
                q.set("color", jf3(&w[2])); q.set("pos", jf3(&w[5])); q.set("dir_or_nan", jf3(&w[8]));
                q.set("params", jf3(&w[11])); l.add(q);
            }
            o.set("lights", l);
        }
        o.set("level_grid", J::O().set("width", J::I(t.lvlW)).set("height", J::I(t.lvlH)).set("a", J::N(t.lvlA)).set("b", J::N(t.lvlB)).set("lvl0_bytes", J::I(t.lvl0Size)));
        o.set("texture_words", J::A().add(J::I(t.txtVer)).add(J::I(t.txtWord2))); o.set("materials", jstrs(t.mats)); o.set("textures", jstrs(t.texs));
        o.set("vertex_count", J::I(t.vtxCount)); o.set("vertex_bytes", J::I(t.vtxBytes)); o.set("vertex_stride_44", J::B(t.vtxStride44));
        if (t.hasTreeBox) {
            J bb = J::A(); for (int n = 0; n < 6; n++) bb.add(J::N(t.treeBox[n]));
            o.set("tree_root_bbox_min_xyz_max_xyz", bb);
        }
        o.set("tree_bytes", J::I(t.treeSize)); o.set("tail_bytes", J::I(t.tail));
        j.set("tre", o);
    }
    if (mb.hasEmap) {
        const Emap& m = mb.emap; J o = J::O();
        j.set("emap_file", J::S(fs::path(mb.emapPath).filename().string()));
        o.set("version", J::I(m.ver)); o.set("tre", J::S(m.tre)); o.set("rle", J::S(m.rle)); o.set("automap_texture", J::S(m.am));
        o.set("header_tail_hex", J::S(hex(m.hdrTail.data(), m.hdrTail.size())));
        if (m.hdrTail.size() == 27) {
            Rd r(m.hdrTail.data(), 27); r.skip(15);
            o.set("header_tail_floats", J::A().add(J::N(r.rf())).add(J::N(r.rf())).add(J::N(r.rf())));
        }
        { J c = J::O(); for (auto& p : m.chunkCount) c.set(p.first, J::I(p.second)); o.set("chunk_counts", c); }
        j.set("emap", o);
        J eps = J::A();
        for (auto& ep : m.eps) {
            J e = J::O(); e.set("id", J::I(ep.id)); J sl = J::A();
            for (size_t k = 0; k < ep.pos.size(); k++) sl.add(J::O().set("pos", jf3(ep.pos[k].data())).set("yaw", J::N(ep.yaw[k])));
            e.set("slots", sl); eps.add(e);
        }
        j.set("entry_points", eps);
        if (m.hasCam) j.set("camera_ecam", J::A().add(J::N(m.cam[0])).add(J::N(m.cam[1])).add(J::N(m.cam[2])).add(J::N(m.cam[3])));
        J rg = J::A();
        for (auto& g : m.regions) {
            J e = J::O(); e.set("kind", J::I(g.kind)); J pts = J::A(); for (auto& p : g.pts) pts.add(jf3(p.data())); e.set("points_xyz", pts);
            if (g.hasBlk) e.set("blk", J::O().set("id", J::I(g.blkId)).set("rgb", J::A().add(J::I(g.blkRgb[0])).add(J::I(g.blkRgb[1])).add(J::I(g.blkRgb[2]))));
            if (g.hasScript) e.set("script", J::S(g.script));
            if (g.hasTrans) e.set("transition", J::O().set("map", J::S(g.tmap)).set("entry_point", J::I(g.tentry)).set("flag", J::I(g.tflag)));
            rg.add(e);
        }
        j.set("regions", rg);
        J ps = J::A();
        for (auto& p : m.paths) {
            J e = J::O(); e.set("name", J::S(p.name)); J nd = J::A();
            for (auto& n : p.nodes) nd.add(J::O().set("pos", jf3(n.pos)).set("extra", jf3(n.extra)));
            e.set("nodes", nd); ps.add(e);
        }
        j.set("paths", ps);
        J sd = J::A();
        for (auto& s : m.sounds) sd.add(J::O().set("name", J::S(s.name)).set("pos", jf3(s.pos)).set("file", J::S(s.file)).set("flags", J::A().add(J::I(s.a)).add(J::I(s.b))));
        j.set("sound_emitters", sd);
        J ef = J::A();
        for (auto& s : m.effects) ef.add(J::O().set("name", J::S(s.name)).set("pos", jf3(s.pos)).set("rot", jf3(s.rot)).set("effect", J::S(s.veg)).set("flags", J::A().add(J::I(s.a)).add(J::I(s.b))));
        j.set("effect_emitters", ef);
        J ic = J::A();
        for (auto& s : m.icons) ic.add(J::O().set("pos", jf3(s.pos)).set("icon", J::S(s.icon)).set("type", J::I(s.type)));
        j.set("area_map_icons", ic);
        if (!m.emnp.empty()) j.set("emnp", jvec(m.emnp));
        { J ot = J::A(); for (auto& c : m.other) ot.add(J::O().set("tag", J::S(c.tag)).set("size", J::I(c.data.size()))); j.set("other_chunks", ot); }
        J ob = J::A();
        int idx = 0;
        for (auto& e : m.ents) {
            J o2 = J::O();
            o2.set("index", J::I(idx++));
            o2.set("template", J::S(e.tmpl)); o2.set("type", J::S(e.type));
            o2.set("pos", jf3(e.pos)); o2.set("yaw", J::N(e.rot[0]));
            if (e.rot[1] != 0 || e.rot[2] != 0) o2.set("rot_extra", J::A().add(J::N(e.rot[1])).add(J::N(e.rot[2])));
            o2.set("flag", J::I(e.flag));
            o2.set("instance", eeovJson(e.ov));
            ob.add(o2);
        }
        j.set("objects", ob);
    }
    if (!mb.errors.empty()) j.set("errors", jstrs(mb.errors));
    return j;
}

static std::string f1(float v) { char t[32]; snprintf(t, sizeof t, "%.2f", v); return t; }
static void mapText(const MapBundle& mb) {
    printf("== map %s  (tre base: %s)\n", mb.name.c_str(), mb.treBase.c_str());
    if (mb.hasGrid) {
        const Grid& g = mb.grid;
        printf("grid: %u x %u cells (%.1f x %.1f world units, 2 cells/unit), %u RLE runs\n", g.w, g.h, g.w / 2.0, g.h / 2.0, g.runs);
        printf("  values:");
        for (auto& p : g.hist) printf(" 0x%02x=%u", p.first, p.second);
        printf("\n");
    }
    if (mb.hasTre) {
        const Tre& t = mb.tre;
        printf("tre: head=[%u,%u,%u,%u] lights=%u level_grid=%ux%u mats=%zu textures=%zu verts=%u (%s) tree=%u bytes tail=%u\n",
               t.headWords[0], t.headWords[1], t.headWords[2], t.headWords[3], t.lightCount, t.lvlW, t.lvlH, t.mats.size(), t.texs.size(),
               t.vtxCount, t.vtxStride44 ? "44-byte stride" : "stride?", t.treeSize, t.tail);
        if (t.hasTreeBox) printf("  root bbox: (%.1f,%.1f,%.1f)-(%.1f,%.1f,%.1f)\n", t.treeBox[0], t.treeBox[1], t.treeBox[2], t.treeBox[3], t.treeBox[4], t.treeBox[5]);
    }
    if (mb.hasEmap) {
        const Emap& m = mb.emap;
        printf("emap v%u: ", m.ver);
        for (auto& p : m.chunkCount) printf("%s=%u ", p.first.c_str(), p.second);
        printf("\n");
        for (auto& ep : m.eps) {
            printf("entry point %u:", ep.id);
            for (size_t k = 0; k < ep.pos.size(); k++) printf(" (%s,%s,%s)", f1(ep.pos[k][0]).c_str(), f1(ep.pos[k][1]).c_str(), f1(ep.pos[k][2]).c_str());
            printf("\n");
        }
        for (auto& g : m.regions) {
            printf("region kind=%u pts=%zu", g.kind, g.pts.size());
            if (g.hasBlk) printf(" blk=%u", g.blkId);
            if (g.hasScript) printf(" script=%s", g.script.c_str());
            if (g.hasTrans) printf(" -> %s entry %u", g.tmap.c_str(), g.tentry);
            printf("\n");
        }
        for (auto& p : m.paths) printf("path %s: %zu nodes\n", p.name.c_str(), p.nodes.size());
        printf("sounds=%zu effects=%zu icons=%zu objects=%zu\n", m.sounds.size(), m.effects.size(), m.icons.size(), m.ents.size());
        int i = 0;
        for (auto& e : m.ents) {
            printf("%3d %-44s %-3s (%8.2f %6.2f %8.2f) yaw %6.3f team %u", i++, e.tmpl.c_str(), e.type.c_str(), e.pos[0], e.pos[1], e.pos[2], e.rot[0], e.ov.team);
            if (!e.ov.aiScript.empty()) printf(" script=%s", e.ov.aiScript.c_str());
            if (!e.ov.dialogScript.empty()) printf(" dialog=%s", e.ov.dialogScript.c_str());
            if (!e.ov.inv.empty()) printf(" inv=%zu", e.ov.inv.size());
            printf("\n");
        }
    }
    for (auto& s : mb.errors) printf("ERROR: %s\n", s.c_str());
}

// ---------------------------------------------------------------------------------------------
// commands
// ---------------------------------------------------------------------------------------------
static std::string argVal(int argc, char** argv, const char* key) {
    for (int i = 0; i + 1 < argc; i++) if (!strcmp(argv[i], key)) return argv[i + 1];
    return "";
}
static bool hasFlag(int argc, char** argv, const char* key) {
    for (int i = 0; i < argc; i++) if (!strcmp(argv[i], key)) return true;
    return false;
}
static void loadStf(int argc, char** argv, Stf& stf) {
    std::string p = argVal(argc, argv, "--stf");
    if (!p.empty()) stf.load(p);
}

static int cmdList(const fs::path& root) {
    MapIndex ix = buildIndex(root);
    std::set<std::string> all = ix.treBases;
    for (auto& p : ix.emapByTre) all.insert(p.first);
    for (auto& n : all) {
        printf("%-34s tre=%d emap=%s\n", n.c_str(), ix.treBases.count(n) ? 1 : 0, ix.emapByTre.count(n) ? ix.emapByTre.at(n).filename().string().c_str() : "-");
    }
    return 0;
}

static int cmdMap(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "usage: mapinfo map <extracted-dir> <name> [--json] [--grid]\n"); return 2; }
    fs::path root = argv[2];
    MapIndex ix = buildIndex(root);
    MapBundle mb = loadMap(root, ix, argv[3]);
    if (hasFlag(argc, argv, "--json")) fputs(jstr(mapJson(mb, hasFlag(argc, argv, "--grid"))).c_str(), stdout);
    else mapText(mb);
    return mb.errors.empty() ? 0 : 1;
}

static int cmdEen(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: mapinfo een <file.een> [--json] [--stf English.stf]\n"); return 2; }
    Stf stf; loadStf(argc, argv, stf);
    fs::path p = argv[2];
    Een e = parseEen(readFile(p));
    J j = eenJson(e, &stf, p.stem().string());
    if (hasFlag(argc, argv, "--json")) { fputs(jstr(j).c_str(), stdout); return 0; }
    printf("template %s  model=%s\n", p.stem().string().c_str(), e.model.c_str());
    if (stf.ok && e.hasGent) printf("  name: %s\n  desc: %s\n", stf.get(e.gent[0]).c_str(), stf.get(e.gent[1]).c_str());
    printf("  default script=%s dialog=%s team=%u skin=%s inventory=%zu\n", e.ov.aiScript.c_str(), e.ov.dialogScript.c_str(), e.ov.team, e.ov.skin.c_str(), e.ov.inv.size());
    if (e.hasGcre) {
        printf("  SPECIAL?:");
        for (int n = 0; n < 7; n++) printf(" %u", e.gcre.special[n]);
        printf("  attack modes: %zu  equipment: %zu\n", e.gcre.modes.size(), e.gcre.inv.size());
    }
    for (auto& rc : e.other) printf("  chunk %s v%u %zu bytes\n", rc.tag.c_str(), rc.ver, rc.data.size());
    return 0;
}

static int cmdAmx(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: mapinfo amx <file> [--json]\n"); return 2; }
    fs::path p = argv[2];
    Amx a = parseAmx(readFile(p));
    J j = amxJson(a, p.filename().string());
    if (hasFlag(argc, argv, "--json")) { fputs(jstr(j).c_str(), stdout); return 0; }
    printf("%s: AMX v%u/%u flags=0x%x code=%d data=%d stack+heap=%d\n", p.filename().string().c_str(), a.fver, a.aver, a.flags, a.dat - a.cod, a.hea - a.dat, a.stp - a.hea);
    printf("publics (%zu):", a.publics.size()); for (auto& x : a.publics) printf(" %s", x.second.c_str());
    printf("\nnatives (%zu):", a.natives.size()); for (auto& x : a.natives) printf(" %s", x.second.c_str());
    printf("\n");
    return 0;
}

static int cmdVerify(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: mapinfo verify <extracted-dir> [--verbose]\n"); return 2; }
    fs::path root = argv[2]; bool verbose = hasFlag(argc, argv, "--verbose");
    MapIndex ix = buildIndex(root);
    std::set<std::string> all = ix.treBases;
    for (auto& p : ix.emapByTre) all.insert(p.first);
    int maps = 0, mapsOk = 0, emapN = 0, emapOk = 0, gridN = 0, gridOk = 0, treN = 0, treOk = 0, ents = 0, entTmplOk = 0, entModelOk = 0, entScriptOk = 0, entScriptN = 0, invN = 0, invOk = 0;
    std::set<std::string> missing;
    auto scriptExists = [&](const std::string& s) {
        if (s.empty()) return true;
        std::string b = s; if (b.size() > 4 && b.compare(b.size() - 4, 4, ".amx") == 0) b.resize(b.size() - 4);
        return fs::exists(root / "Scripts" / (b + ".scrb")) || fs::exists(root / "Scripts" / (b + ".scra"));
    };
    for (auto& n : all) {
        maps++;
        MapBundle mb = loadMap(root, ix, n);
        bool ok = mb.errors.empty();
        if (ix.emapByTre.count(n)) { emapN++; if (mb.hasEmap) emapOk++; }
        if (!findMapFile(root, n, ".mapdata").empty()) { gridN++; if (mb.hasGrid) gridOk++; }
        if (ix.treBases.count(n)) { treN++; if (mb.hasTre) treOk++; }
        if (mb.hasEmap) {
            for (auto& e : mb.emap.ents) {
                ents++;
                const char* g = grpForType(e.type);
                fs::path tp = g ? root / g / (e.tmpl + ".een") : fs::path();
                bool te = !tp.empty() && fs::exists(tp);
                if (te) entTmplOk++; else missing.insert(e.name);
                if (te) {
                    try {
                        Een t = parseEen(readFile(tp));
                        bool me = fs::exists(root / "Props" / (t.model + ".b3d")) || fs::exists(root / "Items" / (t.model + ".b3d")) || fs::exists(root / "Critters" / (t.model + ".b3d")) || fs::exists(root / "Tiles" / (t.model + ".b3d"));
                        if (me) entModelOk++; else missing.insert("model:" + t.model);
                    } catch (...) {}
                }
                entScriptN++; if (scriptExists(e.ov.aiScript)) entScriptOk++; else missing.insert("script:" + e.ov.aiScript);
                for (auto& it : e.ov.inv) {
                    invN++;
                    size_t dot = it.rfind('.'); const char* gg = dot == std::string::npos ? nullptr : grpForType(it.substr(dot + 1));
                    if (gg && fs::exists(root / gg / (it.substr(0, dot) + ".een"))) invOk++; else missing.insert("inv:" + it);
                }
            }
            for (auto& g : mb.emap.regions) if (g.hasScript && !scriptExists(g.script)) missing.insert("script:" + g.script);
        }
        if (ok) mapsOk++;
        if (verbose || !ok) {
            printf("%-4s %-34s emap=%d grid=%d tre=%d ents=%zu", ok ? "OK" : "FAIL", n.c_str(), mb.hasEmap, mb.hasGrid, mb.hasTre, mb.emap.ents.size());
            for (auto& s : mb.errors) printf(" [%s]", s.c_str());
            printf("\n");
        }
    }
    printf("\nmaps (unique tre/emap bases): %d, fully parsed: %d\n", maps, mapsOk);
    printf("emap: %d/%d  mapdata: %d/%d  tre: %d/%d\n", emapOk, emapN, gridOk, gridN, treOk, treN);
    printf("entities: %d  template .een found: %d  model b3d found: %d  ai-script found: %d/%d  inventory refs resolved: %d/%d\n", ents, entTmplOk, entModelOk, entScriptOk, entScriptN, invOk, invN);

    // all .een templates and all AMX scripts
    int eenN = 0, eenOk = 0;
    for (const char* g : {"_CRT", "_WEA", "_ARM", "_AMO", "_USE", "_DOR", "_ITM", "_CON"}) {
        if (!fs::exists(root / g)) continue;
        for (auto& f : fs::directory_iterator(root / g)) {
            if (f.path().extension() != ".een") continue;
            eenN++;
            try { parseEen(readFile(f.path())); eenOk++; } catch (std::exception& e) { printf("een FAIL %s: %s\n", f.path().filename().string().c_str(), e.what()); }
        }
    }
    int amxN = 0, amxOk = 0;
    if (fs::exists(root / "Scripts"))
        for (auto& f : fs::directory_iterator(root / "Scripts")) {
            std::string ext = f.path().extension().string();
            if (ext != ".scra" && ext != ".scrb") continue;
            amxN++;
            try { Amx a = parseAmx(readFile(f.path())); if (a.sizeOk) amxOk++; else printf("amx size mismatch %s\n", f.path().filename().string().c_str()); }
            catch (std::exception& e) { printf("amx FAIL %s: %s\n", f.path().filename().string().c_str(), e.what()); }
        }
    printf("entity templates (.een): %d/%d  AMX scripts: %d/%d\n", eenOk, eenN, amxOk, amxN);
    if (!missing.empty()) { printf("unresolved references (%zu):\n", missing.size()); for (auto& s : missing) printf("  %s\n", s.c_str()); }
    return (mapsOk == maps && eenOk == eenN && amxOk == amxN) ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr,
                "mapinfo - Van Buren map/logic format tool\n"
                "  mapinfo list   <extracted-dir>\n"
                "  mapinfo map    <extracted-dir> <name> [--json] [--grid]\n"
                "  mapinfo een    <file.een> [--json] [--stf English.stf]\n"
                "  mapinfo amx    <file.scra|file.scrb> [--json]\n"
                "  mapinfo verify <extracted-dir> [--verbose]\n");
        return 2;
    }
    std::string c = argv[1];
    try {
        if (c == "list" && argc >= 3) return cmdList(argv[2]);
        if (c == "map") return cmdMap(argc, argv);
        if (c == "een") return cmdEen(argc, argv);
        if (c == "amx") return cmdAmx(argc, argv);
        if (c == "verify") return cmdVerify(argc, argv);
    } catch (std::exception& e) {
        fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    fprintf(stderr, "unknown command\n");
    return 2;
}
