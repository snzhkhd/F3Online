// modelinfo - Van Buren tech demo (F3.exe) 3D asset inspector / exporter.
//   b3d  : 'B3D 1.1 ' meshes  -> dump / .obj+.mtl export
//   tga  : TGA header dump / decode / PNG export
//   gr2  : Granny2 container inspector (header, sections, compression, object counts)
//   scan : batch parse a directory tree and report success rates
// Formats are documented in docs/formats/models.md. No dependencies (C++17).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <stdexcept>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

static bool readFile(const fs::path& p, Bytes& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    auto n = f.tellg();
    f.seekg(0);
    out.resize((size_t)n);
    if (n > 0) f.read((char*)out.data(), n);
    return true;
}
struct ParseError : std::runtime_error { using std::runtime_error::runtime_error; };

// ------------------------------------------------------------------ reader
struct Reader {
    const uint8_t* d; size_t n; size_t p = 0;
    Reader(const Bytes& b) : d(b.data()), n(b.size()) {}
    void need(size_t k) { if (p + k > n) throw ParseError("unexpected EOF at " + std::to_string(p)); }
    uint8_t u8() { need(1); return d[p++]; }
    uint16_t u16() { need(2); uint16_t v; memcpy(&v, d + p, 2); p += 2; return v; }
    uint32_t u32() { need(4); uint32_t v; memcpy(&v, d + p, 4); p += 4; return v; }
    float f32() { need(4); float v; memcpy(&v, d + p, 4); p += 4; return v; }
    void skip(size_t k) { need(k); p += k; }
    std::string str() { uint16_t l = u16(); need(l); std::string s((const char*)d + p, l); p += l; return s; }
    void fl(float* o, int c) { for (int i = 0; i < c; i++) o[i] = f32(); }
};

// ------------------------------------------------------------------ B3D model
struct B3DTexture { uint8_t flags = 0; std::string name, file; uint32_t w = 0, h = 0; int token = 0; };
struct B3DMaterial {
    std::string name, shader;       // e.g. "VL_BOS_Cloth", "BASE_2X"
    float col[4][4] = {};           // 4 RGBA colours (order of meaning not proven, see docs)
    float f0 = 0, f1 = 0;
    std::string blend, surface;     // "OPAQUE"/..., "SILENT"/...
    uint32_t mask = 0, flags2 = 0;
};
struct B3DBone { std::string name, parent; uint8_t flags = 0; float pos[3] = {}, rot[4] = {}, ext[3] = {}; };
struct B3DVertex {
    float pos[3] = {}, nrm[3] = {}, col[4] = {1, 1, 1, 1};
    int nuv = 0; float uv[2][2] = {};
    float lm[4][2] = {};            // lightmap-ish UV sets (preconverted, optional)
    std::vector<std::pair<uint32_t, float>> w;   // bone weights (legacy layout)
};
struct B3DGroup {
    int32_t matIndex = -1;          // index into B3DModel::materials (legacy layout)
    std::string name, shader, blend, surface;
    std::vector<std::string> textures, extra;
    std::vector<uint32_t> idx;      // triangle list
    uint8_t flagsRaw[3] = {};
};
struct B3DMesh {
    std::string name; bool preconverted = false; uint32_t stride = 0;
    float bmin[3] = {}, bmax[3] = {}; bool hasBounds = false; bool hasLightmap = false;
    std::vector<B3DVertex> verts; std::vector<B3DGroup> groups;
};
struct B3DNode { std::string name; std::vector<B3DNode> kids; std::vector<B3DMesh> meshes; std::vector<std::string> boneGroups; };
struct B3DModel {
    uint8_t converted = 0; bool hasConverted = false;
    float scale = 1, unit = 1024; std::string handed; int32_t tok16 = 0; bool hasTok16 = false;
    std::vector<B3DMaterial> materials; std::vector<B3DTexture> textures; std::vector<B3DBone> bones;
    B3DNode root; bool hasRoot = false;
    std::map<int, int> boneFlagHist;
};

static void readMesh(Reader& r, bool conv, B3DMesh& m) {
    m.name = r.str();
    uint8_t has = conv ? r.u8() : 0;
    if (has) {
        m.preconverted = true;
        uint32_t cnt = r.u32(); m.stride = r.u32();
        if (m.stride != 44) throw ParseError("unsupported vertex stride " + std::to_string(m.stride));
        if ((uint64_t)cnt * m.stride > r.n - r.p) throw ParseError("vertex buffer overruns file");
        const uint8_t* vb = r.d + r.p; r.skip((size_t)cnt * m.stride);
        m.verts.resize(cnt);
        for (uint32_t i = 0; i < cnt; i++) {
            const uint8_t* v = vb + (size_t)i * 44; B3DVertex& o = m.verts[i];
            memcpy(o.pos, v, 12); memcpy(o.nrm, v + 12, 12);
            o.col[2] = v[24] / 255.f; o.col[1] = v[25] / 255.f; o.col[0] = v[26] / 255.f; o.col[3] = v[27] / 255.f; // D3DCOLOR (BGRA bytes)
            memcpy(o.uv[0], v + 28, 8); memcpy(o.uv[1], v + 36, 8); o.nuv = 2;
        }
        r.fl(m.bmin, 3); r.fl(m.bmax, 3); m.hasBounds = true;
        m.hasLightmap = r.u8() != 0;
        if (m.hasLightmap) {
            if (r.u32() != 4) throw ParseError("lightmap set count != 4");
            for (int s = 0; s < 4; s++) for (uint32_t i = 0; i < cnt; i++) { m.verts[i].lm[s][0] = r.f32(); m.verts[i].lm[s][1] = r.f32(); }
        }
        uint32_t G = r.u32(); if (G > 4096) throw ParseError("absurd group count");
        m.groups.resize(G);
        for (auto& g : m.groups) {
            g.name = r.str(); g.shader = r.str(); r.skip(64 + 4);
            g.blend = r.str(); g.surface = r.str();
            g.flagsRaw[0] = r.u8(); g.flagsRaw[1] = r.u8(); g.flagsRaw[2] = r.u8();
            r.skip(4); r.skip(1); r.skip(4); r.skip(1); r.skip(8); r.skip(1); r.skip(8); r.skip(1); r.skip(12); r.skip(1); r.skip(16);
            uint32_t k = r.u32();
            if (k > 64) throw ParseError("absurd texture count");
            if (k) g.textures.push_back(r.str());
            if (k > 1) g.textures.push_back(r.str());
            uint32_t ne = r.u32(); if (ne > 4096) throw ParseError("absurd extra count");
            for (uint32_t i = 0; i < ne; i++) g.extra.push_back(r.str());
        }
        for (auto& g : m.groups) (void)r.str();   // per-group name list (duplicate of g.name in all samples)
        std::vector<uint32_t> ic(G), tc(G);
        for (auto& x : ic) x = r.u32();
        for (auto& x : tc) x = r.u32();
        for (uint32_t g = 0; g < G; g++) {
            if ((uint64_t)ic[g] * 2 > r.n - r.p) throw ParseError("index list overruns file");
            m.groups[g].idx.resize(ic[g]);
            for (auto& x : m.groups[g].idx) { x = r.u16(); if (x >= cnt) throw ParseError("index out of range"); }
        }
    } else {
        uint32_t nv = r.u32();
        if ((uint64_t)nv * 48 > r.n - r.p) throw ParseError("vertex list overruns file");
        m.verts.resize(nv);
        for (auto& v : m.verts) {
            r.fl(v.pos, 3); r.fl(v.nrm, 3); r.fl(v.col, 4);
            uint32_t nuv = r.u32(); if (nuv > 8) throw ParseError("absurd uv count");
            v.nuv = (int)nuv;
            for (uint32_t i = 0; i < nuv; i++) { float t[2]; r.fl(t, 2); if (i < 2) { v.uv[i][0] = t[0]; v.uv[i][1] = t[1]; } }
            uint32_t nw = r.u32(); if (nw > 64) throw ParseError("absurd weight count");
            for (uint32_t i = 0; i < nw; i++) { uint32_t b = r.u32(); float w = r.f32(); v.w.push_back({b, w}); }
        }
        uint32_t ng = r.u32(); if (ng > 4096) throw ParseError("absurd group count");
        m.groups.resize(ng);
        for (auto& g : m.groups) {
            g.matIndex = (int32_t)r.u32(); g.name = r.str();
            uint32_t a = r.u32(); if (a > 4096) throw ParseError("absurd list");
            for (uint32_t i = 0; i < a; i++) g.textures.push_back(r.str());
            uint32_t b = r.u32(); if (b > 4096) throw ParseError("absurd list");
            for (uint32_t i = 0; i < b; i++) g.extra.push_back(r.str());
            uint8_t prim = r.u8(); if (prim != 0xc9) throw ParseError("unknown primitive type " + std::to_string(prim));
            uint32_t n = r.u32();
            if ((uint64_t)n * 5 > r.n - r.p) throw ParseError("face list overruns file");
            if (n % 3) throw ParseError("index count not multiple of 3");
            g.idx.resize(n);
            for (auto& x : g.idx) { x = r.u32(); if (x >= nv) throw ParseError("index out of range"); }
            r.skip(n);  // per-index u8 flag (always 1)
        }
    }
}
static void readNode(Reader& r, bool conv, B3DNode& nd) {
    for (;;) {
        uint8_t t = r.u8();
        switch (t) {
        case 0x0b: return;
        case 0x0c: nd.name = r.str(); break;
        case 0x0a: nd.kids.emplace_back(); readNode(r, conv, nd.kids.back()); break;
        case 0x0f: nd.meshes.emplace_back(); readMesh(r, conv, nd.meshes.back()); break;
        case 0x1b: { uint32_t n = r.u32(); if (n > 4096) throw ParseError("absurd list"); for (uint32_t i = 0; i < n; i++) nd.boneGroups.push_back(r.str()); break; }
        default: { char b[64]; snprintf(b, sizeof b, "unknown node token 0x%02x at 0x%zx", t, r.p - 1); throw ParseError(b); }
        }
    }
}
static void parseB3D(const Bytes& data, B3DModel& m) {  // throws on any inconsistency, incl. trailing bytes
    Reader r(data);
    if (data.size() < 9 || memcmp(data.data(), "B3D 1.1 ", 8) != 0) throw ParseError("not a 'B3D 1.1 ' file");
    r.p = 8;
    for (;;) {
        uint8_t t = r.u8();
        switch (t) {
        case 0x01: if (r.p != data.size()) throw ParseError("trailing bytes after EOF token: " + std::to_string(data.size() - r.p)); return;
        case 0x03: m.scale = r.f32(); break;
        case 0x04: m.unit = r.f32(); break;
        case 0x05: m.handed = r.str(); break;
        case 0x06: r.skip(12); break;
        case 0x07: {
            B3DMaterial mt; mt.shader = r.str(); mt.name = r.str();  // shader/technique id first, material name second
            for (int i = 0; i < 4; i++) r.fl(mt.col[i], 4);
            mt.f0 = r.f32(); mt.f1 = r.f32();
            mt.blend = r.str(); mt.surface = r.str();
            mt.mask = r.u32();
            r.skip(8 * !!(mt.mask & 1) + 8 * !!(mt.mask & 2) + 16 * !!(mt.mask & 4) + 16 * !!(mt.mask & 8) + 24 * !!(mt.mask & 16) + 20 * !!(mt.mask & 32));
            mt.flags2 = r.u32();
            m.materials.push_back(mt); break; }
        case 0x08: case 0x09: {
            B3DTexture tx; tx.token = t; tx.flags = r.u8(); tx.name = r.str(); tx.file = r.str(); tx.w = r.u32(); tx.h = r.u32();
            m.textures.push_back(tx); break; }
        case 0x0a: m.hasRoot = true; readNode(r, m.converted != 0, m.root); break;
        case 0x0e: {
            B3DBone b; b.name = r.str(); b.parent = r.str(); b.flags = r.u8();
            if (b.flags & 1) r.fl(b.pos, 3);
            if (b.flags & 2) r.fl(b.rot, 4);
            if (b.flags & 4) r.fl(b.ext, 3);
            m.boneFlagHist[b.flags]++;
            m.bones.push_back(b); break; }
        case 0x16: m.tok16 = (int32_t)r.u32(); m.hasTok16 = true; break;
        case 0x1c: m.converted = r.u8(); m.hasConverted = true; break;
        default: { char b[64]; snprintf(b, sizeof b, "unknown/invalid top-level token 0x%02x at 0x%zx", t, r.p - 1); throw ParseError(b); }
        }
    }
}
static void collectMeshes(const B3DNode& n, std::vector<const B3DMesh*>& out) {
    for (auto& m : n.meshes) out.push_back(&m);
    for (auto& k : n.kids) collectMeshes(k, out);
}

// ------------------------------------------------------------------ B3D dump / OBJ export
static std::string safeName(std::string s) {
    for (auto& c : s) if (c == ' ' || c == '/' || c == '\\' || c == ':') c = '_';
    return s;
}
static void dumpB3D(const B3DModel& m, FILE* o) {
    fprintf(o, "converted=%d scale=%g unit=%g handedness=%s tok16=%d\n", m.converted, m.scale, m.unit, m.handed.c_str(), m.tok16);
    fprintf(o, "materials=%zu textures=%zu bones=%zu\n", m.materials.size(), m.textures.size(), m.bones.size());
    for (auto& t : m.textures) fprintf(o, "  texture[tok%d fl=%u] name=%s file=%s %ux%u\n", t.token, t.flags, t.name.c_str(), t.file.c_str(), t.w, t.h);
    for (auto& t : m.materials)
        fprintf(o, "  material %s shader=%s blend=%s surface=%s mask=0x%x flags=0x%x diffuse?=(%g %g %g %g)\n", t.name.c_str(), t.shader.c_str(), t.blend.c_str(), t.surface.c_str(), t.mask, t.flags2, t.col[1][0], t.col[1][1], t.col[1][2], t.col[1][3]);
    for (auto& b : m.bones)
        fprintf(o, "  bone %s parent='%s' fl=%u pos=(%g %g %g) axisangle=(%g %g %g | %g deg)\n", b.name.c_str(), b.parent.c_str(), b.flags, b.pos[0], b.pos[1], b.pos[2], b.rot[0], b.rot[1], b.rot[2], b.rot[3]);
    std::vector<const B3DMesh*> ms; collectMeshes(m.root, ms);
    for (auto* me : ms) {
        fprintf(o, "mesh '%s' %s verts=%zu groups=%zu", me->name.c_str(), me->preconverted ? "preconverted(stride44)" : "legacy-list", me->verts.size(), me->groups.size());
        if (me->hasBounds) fprintf(o, " bounds=(%g %g %g)-(%g %g %g) lightmap=%d", me->bmin[0], me->bmin[1], me->bmin[2], me->bmax[0], me->bmax[1], me->bmax[2], me->hasLightmap);
        fprintf(o, "\n");
        for (auto& g : me->groups) {
            fprintf(o, "  group '%s' mat=%d tris=%zu tex=[", g.name.c_str(), g.matIndex, g.idx.size() / 3);
            for (size_t i = 0; i < g.textures.size(); i++) fprintf(o, "%s%s", i ? "," : "", g.textures[i].c_str());
            fprintf(o, "]\n");
        }
    }
}
// Default export: LH (D3D) Y-up -> RH Y-up by negating Z, reversing winding; V flipped. raw=true keeps the file's numbers.
static bool exportOBJ(const B3DModel& m, const std::string& base, bool raw, std::string& err) {
    std::vector<const B3DMesh*> ms; collectMeshes(m.root, ms);
    if (ms.empty()) { err = "no mesh"; return false; }
    fs::path op(base);
    std::ofstream o(op.string() + ".obj"), mt(op.string() + ".mtl");
    if (!o || !mt) { err = "cannot write output"; return false; }
    o << "# exported by modelinfo from B3D; " << (raw ? "raw coordinates (left-handed, Y up)\n" : "converted to right-handed Y-up (z negated, winding reversed)\n");
    o << "mtllib " << op.filename().string() << ".mtl\n";
    std::set<std::string> done; size_t vbase = 1;
    char buf[256];
    for (auto* me : ms) {
        for (auto& v : me->verts) { snprintf(buf, sizeof buf, "v %.6f %.6f %.6f\n", v.pos[0], v.pos[1], raw ? v.pos[2] : -v.pos[2]); o << buf; }
        for (auto& v : me->verts) { snprintf(buf, sizeof buf, "vt %.6f %.6f\n", v.uv[0][0], 1.0f - v.uv[0][1]); o << buf; }
        for (auto& v : me->verts) { snprintf(buf, sizeof buf, "vn %.6f %.6f %.6f\n", v.nrm[0], v.nrm[1], raw ? v.nrm[2] : -v.nrm[2]); o << buf; }
        for (auto& g : me->groups) {
            std::string mn = g.name.empty() ? "default" : safeName(g.name);
            o << "g " << safeName(me->name.empty() ? "mesh" : me->name) << "_" << mn << "\nusemtl " << mn << "\n";
            if (!done.count(mn)) {
                done.insert(mn);
                std::string tex = g.textures.empty() ? "" : g.textures[0];
                if (tex.empty() && !m.textures.empty()) tex = m.textures[0].file;
                mt << "newmtl " << mn << "\nKd 1 1 1\n";
                if (!tex.empty()) mt << "map_Kd " << tex << "\n";
                if (!g.blend.empty() && g.blend != "OPAQUE") mt << "# blend " << g.blend << "\n";
            }
            for (size_t i = 0; i + 2 < g.idx.size(); i += 3) {
                size_t a = g.idx[i] + vbase, b = g.idx[i + 1] + vbase, c = g.idx[i + 2] + vbase;
                if (!raw) std::swap(b, c);
                snprintf(buf, sizeof buf, "f %zu/%zu/%zu %zu/%zu/%zu %zu/%zu/%zu\n", a, a, a, b, b, b, c, c, c); o << buf;
            }
        }
        vbase += me->verts.size();
    }
    return true;
}

// ------------------------------------------------------------------ TGA
struct TgaInfo {
    uint8_t idLen, cmType, imgType; uint16_t cmStart, cmLen; uint8_t cmBits; uint16_t xo, yo, w, h; uint8_t bpp, desc;
    bool footer = false; std::string idStr;
};
static TgaInfo parseTgaHeader(const Bytes& d) {
    if (d.size() < 18) throw ParseError("too small for TGA");
    TgaInfo t; const uint8_t* p = d.data();
    t.idLen = p[0]; t.cmType = p[1]; t.imgType = p[2];
    memcpy(&t.cmStart, p + 3, 2); memcpy(&t.cmLen, p + 5, 2); t.cmBits = p[7];
    memcpy(&t.xo, p + 8, 2); memcpy(&t.yo, p + 10, 2); memcpy(&t.w, p + 12, 2); memcpy(&t.h, p + 14, 2);
    t.bpp = p[16]; t.desc = p[17];
    if (d.size() >= 18 + t.idLen) t.idStr.assign((const char*)p + 18, t.idLen);
    t.footer = d.size() >= 44 && memcmp(d.data() + d.size() - 18, "TRUEVISION-XFILE", 16) == 0;
    return t;
}
// Decode to RGBA8 top-to-bottom.
static void decodeTga(const Bytes& d, std::vector<uint8_t>& rgba, int& W, int& H) {
    TgaInfo t = parseTgaHeader(d);
    int it = t.imgType;
    if (!(it == 1 || it == 2 || it == 3 || it == 9 || it == 10 || it == 11)) throw ParseError("unsupported TGA image type " + std::to_string(it));
    W = t.w; H = t.h; if (!W || !H) throw ParseError("empty TGA");
    size_t pos = 18 + t.idLen;
    std::vector<uint8_t> pal;
    int cmBytes = (t.cmBits + 7) / 8;
    if (t.cmType) { pos += (size_t)t.cmLen * cmBytes; pal.assign(d.begin() + 18 + t.idLen, d.begin() + std::min(d.size(), pos)); }
    int bpp = t.bpp, Bpp = (bpp + 7) / 8;
    if (!(bpp == 8 || bpp == 15 || bpp == 16 || bpp == 24 || bpp == 32)) throw ParseError("unsupported TGA depth");
    std::vector<uint8_t> raw((size_t)W * H * Bpp);
    bool rle = it >= 9;
    if (!rle) { if (pos + raw.size() > d.size()) throw ParseError("truncated TGA pixel data"); memcpy(raw.data(), d.data() + pos, raw.size()); }
    else {
        size_t o = 0;
        while (o < raw.size()) {
            if (pos >= d.size()) throw ParseError("truncated RLE data");
            uint8_t h = d[pos++]; int cnt = (h & 0x7f) + 1;
            if (h & 0x80) { if (pos + Bpp > d.size()) throw ParseError("truncated RLE"); for (int i = 0; i < cnt && o < raw.size(); i++) { memcpy(&raw[o], &d[pos], Bpp); o += Bpp; } pos += Bpp; }
            else { size_t nb = (size_t)cnt * Bpp; if (pos + nb > d.size()) throw ParseError("truncated RLE"); size_t c = std::min(nb, raw.size() - o); memcpy(&raw[o], &d[pos], c); o += c; pos += nb; }
        }
    }
    rgba.assign((size_t)W * H * 4, 255);
    bool topFirst = (t.desc & 0x20) != 0;
    for (int y = 0; y < H; y++) {
        int sy = topFirst ? y : H - 1 - y;
        for (int x = 0; x < W; x++) {
            const uint8_t* s = &raw[((size_t)sy * W + x) * Bpp]; uint8_t* o = &rgba[((size_t)y * W + x) * 4];
            if (it == 1 || it == 9) { size_t k = (size_t)(*s - t.cmStart) * cmBytes; if (k + cmBytes <= pal.size()) { o[0] = pal[k + 2]; o[1] = pal[k + 1]; o[2] = pal[k]; if (cmBytes == 4) o[3] = pal[k + 3]; } }
            else if (it == 3 || it == 11) o[0] = o[1] = o[2] = s[0];
            else if (bpp == 24) { o[0] = s[2]; o[1] = s[1]; o[2] = s[0]; }
            else if (bpp == 32) { o[0] = s[2]; o[1] = s[1]; o[2] = s[0]; o[3] = s[3]; }
            else { uint16_t v = s[0] | s[1] << 8; o[0] = ((v >> 10) & 31) * 255 / 31; o[1] = ((v >> 5) & 31) * 255 / 31; o[2] = (v & 31) * 255 / 31; }
        }
    }
}
static uint32_t crc32(const uint8_t* p, size_t n, uint32_t c = 0) {
    static uint32_t T[256]; static bool init = false;
    if (!init) { for (uint32_t i = 0; i < 256; i++) { uint32_t x = i; for (int k = 0; k < 8; k++) x = x & 1 ? 0xedb88320u ^ (x >> 1) : x >> 1; T[i] = x; } init = true; }
    c = ~c; while (n--) c = T[(c ^ *p++) & 255] ^ (c >> 8); return ~c;
}
static void writePNG(const std::string& path, const std::vector<uint8_t>& rgba, int W, int H) {
    std::vector<uint8_t> raw; raw.reserve((size_t)(W * 4 + 1) * H);
    for (int y = 0; y < H; y++) { raw.push_back(0); raw.insert(raw.end(), rgba.begin() + (size_t)y * W * 4, rgba.begin() + (size_t)(y + 1) * W * 4); }
    std::vector<uint8_t> z{0x78, 0x01}; size_t o = 0; uint32_t a = 1, b = 0;
    for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    while (o < raw.size() || raw.empty()) {
        size_t n = std::min<size_t>(65535, raw.size() - o); bool last = o + n >= raw.size();
        z.push_back(last); z.push_back(n & 255); z.push_back(n >> 8); z.push_back(~n & 255); z.push_back((~n >> 8) & 255);
        z.insert(z.end(), raw.begin() + o, raw.begin() + o + n); o += n; if (last) break;
    }
    uint32_t ad = (b << 16) | a; for (int i = 3; i >= 0; i--) z.push_back((ad >> (i * 8)) & 255);
    std::ofstream f(path, std::ios::binary);
    auto be32 = [&](std::vector<uint8_t>& v, uint32_t x) { for (int i = 3; i >= 0; i--) v.push_back((x >> (i * 8)) & 255); };
    auto chunk = [&](const char* ty, const std::vector<uint8_t>& dat) {
        std::vector<uint8_t> c; be32(c, (uint32_t)dat.size()); c.insert(c.end(), ty, ty + 4); c.insert(c.end(), dat.begin(), dat.end());
        be32(c, crc32(c.data() + 4, c.size() - 4)); f.write((const char*)c.data(), c.size());
    };
    const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10}; f.write((const char*)sig, 8);
    std::vector<uint8_t> ih; be32(ih, W); be32(ih, H); ih.push_back(8); ih.push_back(6); ih.push_back(0); ih.push_back(0); ih.push_back(0);
    chunk("IHDR", ih); chunk("IDAT", z); chunk("IEND", {});
}

// ------------------------------------------------------------------ GR2
struct Gr2Section { uint32_t comp, off, clen, dlen, align, f16, f8, relOff, relCnt, marOff, marCnt; };
struct Gr2File {
    uint32_t headerSize, headerFormat, version, fileSize, crc, secOff, secCount, rootTypeSec, rootTypeOff, rootNodeSec, rootNodeOff, tag;
    std::vector<Gr2Section> secs;
};
static const uint8_t GR2_MAGIC[16] = {0xb8, 0x67, 0xb0, 0xca, 0xf8, 0x6d, 0xb1, 0x0f, 0x84, 0x72, 0x8c, 0x7e, 0x5e, 0x19, 0x00, 0x1e};
static void parseGr2(const Bytes& d, Gr2File& g) {
    if (d.size() < 0x58 || memcmp(d.data(), GR2_MAGIC, 16)) throw ParseError("not a Granny2 (little-endian 32-bit) file");
    Reader r(d); r.p = 0x10;
    g.headerSize = r.u32(); g.headerFormat = r.u32(); r.skip(8);
    g.version = r.u32(); g.fileSize = r.u32(); g.crc = r.u32(); g.secOff = r.u32(); g.secCount = r.u32();
    g.rootTypeSec = r.u32(); g.rootTypeOff = r.u32(); g.rootNodeSec = r.u32(); g.rootNodeOff = r.u32(); g.tag = r.u32();
    if (g.secCount > 64) throw ParseError("absurd section count");
    r.p = 0x20 + g.secOff;
    for (uint32_t i = 0; i < g.secCount; i++) {
        Gr2Section s; uint32_t* a = &s.comp; for (int k = 0; k < 11; k++) a[k] = r.u32();
        if ((uint64_t)s.off + s.clen > d.size()) throw ParseError("section outside file");
        g.secs.push_back(s);
    }
}
#ifdef _WIN32
// Oodle0 (compression id 1) decode and object walking go through the original granny2.dll (32-bit): build Win32.
struct GrannyDll {
    HMODULE h = nullptr;
    void* (__stdcall* ReadEntireFile)(const char*) = nullptr;
    void* (__stdcall* GetFileInfo)(void*) = nullptr;
    const char* (__stdcall* GetVersionString)() = nullptr;
    void (__stdcall* FreeFile)(void*) = nullptr;
    bool load(const std::string& path) {
        h = LoadLibraryA(path.c_str());
        if (!h) return false;
        ReadEntireFile = (decltype(ReadEntireFile))GetProcAddress(h, "_GrannyReadEntireFile@4");
        GetFileInfo = (decltype(GetFileInfo))GetProcAddress(h, "_GrannyGetFileInfo@4");
        GetVersionString = (decltype(GetVersionString))GetProcAddress(h, "_GrannyGetVersionString@0");
        FreeFile = (decltype(FreeFile))GetProcAddress(h, "_GrannyFreeFile@4");
        return ReadEntireFile && GetFileInfo;
    }
};
#endif

// ------------------------------------------------------------------ commands
static std::string g_dll;
static void listFiles(const fs::path& p, const std::string& ext, std::vector<fs::path>& out) {
    if (fs::is_directory(p)) {
        for (auto& e : fs::recursive_directory_iterator(p)) if (e.is_regular_file() && e.path().extension() == ext) out.push_back(e.path());
        std::sort(out.begin(), out.end());
    } else out.push_back(p);
}
static int cmdB3D(std::vector<std::string>& a) {
    std::string outdir; bool raw = false, quiet = false, obj = false;
    std::vector<fs::path> files;
    for (size_t i = 0; i < a.size(); i++) {
        if (a[i] == "--obj") { obj = true; if (i + 1 < a.size()) outdir = a[++i]; }
        else if (a[i] == "--raw") raw = true;
        else if (a[i] == "-q") quiet = true;
        else listFiles(a[i], ".b3d", files);
    }
    if (files.empty()) { fprintf(stderr, "b3d: no input\n"); return 2; }
    if (obj) fs::create_directories(outdir);
    int ok = 0, fail = 0;
    std::map<std::string, int> st;
    for (auto& f : files) {
        Bytes d; B3DModel m;
        try {
            if (!readFile(f, d)) throw ParseError("cannot read");
            parseB3D(d, m);
            ok++;
            { std::vector<const B3DMesh*> ms; collectMeshes(m.root, ms);
              st[std::string("converted-flag=") + (m.hasConverted ? std::to_string(m.converted) : "absent")]++;
              st["handedness=" + m.handed]++; st["unit=" + std::to_string(m.unit) + " scale=" + std::to_string(m.scale)]++;
              st["meshes-per-file=" + std::to_string(ms.size())]++;
              for (auto* me : ms) { st[me->preconverted ? "mesh: preconverted stride44" : "mesh: legacy vertex list"]++; if (me->hasLightmap) st["mesh: with 4 lightmap uv sets"]++; for (auto& g : me->groups) { st["blend=" + g.blend]++; st["surface=" + g.surface]++; st["shader=" + g.shader]++; } }
              for (auto& t : m.materials) { st["mat.blend=" + t.blend]++; st["mat.surface=" + t.surface]++; st["mat.shader=" + t.shader]++; st["mat.mask=" + std::to_string(t.mask)]++; }
              for (auto& t : m.textures) st["texture token " + std::to_string(t.token) + " flags=" + std::to_string(t.flags)]++;
              for (auto& kv : m.boneFlagHist) st["bone flags=" + std::to_string(kv.first)] += kv.second; }
            if (!quiet) { printf("== %s\n", f.string().c_str()); dumpB3D(m, stdout); }
            if (obj) { std::string err; if (!exportOBJ(m, (fs::path(outdir) / f.stem()).string(), raw, err)) fprintf(stderr, "%s: export: %s\n", f.string().c_str(), err.c_str()); }
        } catch (std::exception& e) { fail++; fprintf(stderr, "FAIL %s: %s\n", f.string().c_str(), e.what()); }
    }
    if (files.size() > 1) { for (auto& kv : st) printf("  stat %s : %d\n", kv.first.c_str(), kv.second); }
    printf("b3d: %d ok, %d failed of %zu\n", ok, fail, files.size());
    return fail ? 1 : 0;
}
static int cmdTGA(std::vector<std::string>& a) {
    std::string png; std::vector<fs::path> files; bool quiet = false;
    for (size_t i = 0; i < a.size(); i++) { if (a[i] == "--png" && i + 1 < a.size()) png = a[++i]; else if (a[i] == "-q") quiet = true; else listFiles(a[i], ".tga", files); }
    int ok = 0, fail = 0;
    for (auto& f : files) {
        Bytes d;
        try {
            if (!readFile(f, d)) throw ParseError("cannot read");
            TgaInfo t = parseTgaHeader(d); std::vector<uint8_t> px; int W, H; decodeTga(d, px, W, H);
            int amin = 255, amax = 0; for (size_t i = 3; i < px.size(); i += 4) { amin = std::min<int>(amin, px[i]); amax = std::max<int>(amax, px[i]); }
            if (!quiet) printf("%s: type=%u %ux%u bpp=%u cmap=%u origin=%s alphaBits=%u idLen=%u footer=%d alpha[%d..%d]%s\n", f.filename().string().c_str(), t.imgType, t.w, t.h, t.bpp, t.cmType, (t.desc & 0x20) ? "top" : "bottom", t.desc & 15, t.idLen, t.footer, amin, amax, t.idLen ? (" id='" + t.idStr + "'").c_str() : "");
            if (!png.empty()) writePNG(png, px, W, H);
            ok++;
        } catch (std::exception& e) { fail++; fprintf(stderr, "FAIL %s: %s\n", f.string().c_str(), e.what()); }
    }
    printf("tga: %d ok, %d failed of %zu\n", ok, fail, files.size());
    return fail ? 1 : 0;
}
static const char* kFileInfoNames[] = {"Textures", "Materials", "Skeletons", "VertexDatas", "TriTopologies", "Meshes", "Models", "TrackGroups", "Animations"};
static int cmdGR2(std::vector<std::string>& a) {
    std::vector<fs::path> files; bool quiet = false, useDll = true;
    for (size_t i = 0; i < a.size(); i++) { if (a[i] == "--dll" && i + 1 < a.size()) g_dll = a[++i]; else if (a[i] == "--nodll") useDll = false; else if (a[i] == "-q") quiet = true; else listFiles(a[i], ".gr2", files); }
#ifdef _WIN32
    GrannyDll dll; bool haveDll = false;
    if (useDll) {
        if (g_dll.empty()) g_dll = "F:/_work/F3_Demo/granny2.dll";
        haveDll = dll.load(g_dll);
        if (!haveDll && !quiet) fprintf(stderr, "note: granny2.dll not loadable (%s) - needs a 32-bit (Win32) build; reporting container info only\n", g_dll.c_str());
    }
#endif
    int ok = 0, fail = 0, dllok = 0;
    for (auto& f : files) {
        Bytes d; Gr2File g;
        try {
            if (!readFile(f, d)) throw ParseError("cannot read");
            parseGr2(d, g);
            if (g.fileSize != d.size()) throw ParseError("header file size mismatch");
            ok++;
            if (!quiet) {
                printf("== %s\n  version=%u headerSize=%u headerFormat=%u size=%u crc=%08x tag=%08x rootType=(%u,0x%x) rootNode=(%u,0x%x)\n", f.filename().string().c_str(), g.version, g.headerSize, g.headerFormat, g.fileSize, g.crc, g.tag, g.rootTypeSec, g.rootTypeOff, g.rootNodeSec, g.rootNodeOff);
                for (size_t i = 0; i < g.secs.size(); i++) { auto& s = g.secs[i]; printf("  section %zu: compression=%u(%s) off=%u clen=%u dlen=%u align=%u first16=%u first8=%u relocs=%u@%u marshal=%u@%u\n", i, s.comp, s.comp == 0 ? "none" : s.comp == 1 ? "Oodle0" : s.comp == 2 ? "Oodle1" : "?", s.off, s.clen, s.dlen, s.align, s.f16, s.f8, s.relCnt, s.relOff, s.marCnt, s.marOff); }
            }
#ifdef _WIN32
            if (haveDll) {
                void* gf = dll.ReadEntireFile(f.string().c_str());
                if (gf) {
                    const uint8_t* fi = (const uint8_t*)dll.GetFileInfo(gf);
                    if (fi) {
                        dllok++;
                        if (!quiet) {
                            printf("  fileinfo (via granny2.dll):");
                            for (int k = 0; k < 9; k++) { uint32_t c; memcpy(&c, fi + 16 + 8 * k, 4); printf(" %s=%u", kFileInfoNames[k], c); }
                            printf("\n");
                            if (getenv("MODELINFO_FI")) { printf("  fi words:"); for (int k = 0; k < 40; k++) { uint32_t c; memcpy(&c, fi + 4 * k, 4); printf(" %x", c); } printf("\n"); }
                        }
                    }
                } else if (!quiet) printf("  granny2.dll could not read the file\n");
            }
#endif
        } catch (std::exception& e) { fail++; fprintf(stderr, "FAIL %s: %s\n", f.string().c_str(), e.what()); }
    }
    printf("gr2: container parsed %d ok, %d failed of %zu", ok, fail, files.size());
#ifdef _WIN32
    if (haveDll) printf("; granny2.dll fileinfo %d", dllok);
#endif
    printf("\n");
    return fail ? 1 : 0;
}
static void usage() {
    puts("modelinfo - Van Buren (F3.exe) 3D asset tool\n"
         "  modelinfo b3d <file|dir>... [--obj <outdir>] [--raw] [-q]   dump B3D, optionally export .obj/.mtl\n"
         "  modelinfo tga <file|dir>... [--png out.png] [-q]            TGA info / decode / PNG export\n"
         "  modelinfo gr2 <file|dir>... [--dll granny2.dll] [--nodll] [-q]   Granny2 container inspector\n"
         "  modelinfo scan <extracted-dir>                              parse-rate summary for b3d/tga/gr2");
}
int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 2; }
    std::string cmd = argv[1]; std::vector<std::string> a(argv + 2, argv + argc);
    if (cmd == "b3d") return cmdB3D(a);
    if (cmd == "tga") return cmdTGA(a);
    if (cmd == "gr2") return cmdGR2(a);
    if (cmd == "scan") {
        if (a.empty()) { usage(); return 2; }
        std::vector<std::string> x{a[0], "-q"}; int rc = 0;
        rc |= cmdB3D(x); rc |= cmdTGA(x); std::vector<std::string> y{a[0], "-q"}; rc |= cmdGR2(y); return rc;
    }
    usage(); return 2;
}
