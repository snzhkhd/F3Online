#include "B3d.h"

#include <cstring>
#include <stdexcept>

namespace f3::gd {

namespace {

struct ParseError : std::runtime_error { using std::runtime_error::runtime_error; };

struct Reader {
    const uint8_t* d; size_t n; size_t p = 0;
    Reader(const uint8_t* data, size_t size) : d(data), n(size) {}
    void need(size_t k) const { if (p + k > n) throw ParseError("unexpected EOF at " + std::to_string(p)); }
    uint8_t u8() { need(1); return d[p++]; }
    uint16_t u16() { need(2); uint16_t v; std::memcpy(&v, d + p, 2); p += 2; return v; }
    uint32_t u32() { need(4); uint32_t v; std::memcpy(&v, d + p, 4); p += 4; return v; }
    float f32() { need(4); float v; std::memcpy(&v, d + p, 4); p += 4; return v; }
    void skip(size_t k) { need(k); p += k; }
    std::string str() { uint16_t l = u16(); need(l); std::string s(reinterpret_cast<const char*>(d) + p, l); p += l; return s; }
    void fl(float* o, int c) { for (int i = 0; i < c; ++i) o[i] = f32(); }
};

struct Material { std::string shader, name, blend, surface; };

struct State {
    uint8_t converted = 0;
    std::vector<Material> materials;
    B3dModel* out = nullptr;
    bool haveBounds = false;
};

uint8_t ToByte(float f) { f = f < 0 ? 0 : (f > 1 ? 1 : f); return static_cast<uint8_t>(f * 255.0f + 0.5f); }

void ReadMesh(Reader& r, State& st) {
    B3dModel& m = *st.out;
    r.str();  // mesh name (always empty)
    uint8_t has = st.converted ? r.u8() : 0;
    const size_t vbase = m.VertexCount();
    if (has) {
        m.preconverted = true;
        uint32_t cnt = r.u32(), stride = r.u32();
        if (stride != 44) throw ParseError("unsupported vertex stride");
        if (static_cast<uint64_t>(cnt) * stride > r.n - r.p) throw ParseError("vertex buffer overruns file");
        const uint8_t* vb = r.d + r.p;
        r.skip(static_cast<size_t>(cnt) * stride);
        for (uint32_t i = 0; i < cnt; ++i) {
            const uint8_t* v = vb + static_cast<size_t>(i) * 44;
            float f[3];
            std::memcpy(f, v, 12); m.pos.insert(m.pos.end(), f, f + 3);
            std::memcpy(f, v + 12, 12); m.nrm.insert(m.nrm.end(), f, f + 3);
            m.col.push_back(v[26]); m.col.push_back(v[25]); m.col.push_back(v[24]); m.col.push_back(v[27]);  // BGRA -> RGBA
            std::memcpy(f, v + 28, 8); m.uv0.insert(m.uv0.end(), f, f + 2);
            std::memcpy(f, v + 36, 8); m.uv1.insert(m.uv1.end(), f, f + 2);
        }
        float bmin[3], bmax[3];
        r.fl(bmin, 3); r.fl(bmax, 3);
        if (!st.haveBounds) { std::memcpy(m.bmin, bmin, 12); std::memcpy(m.bmax, bmax, 12); st.haveBounds = true; }
        if (r.u8()) {  // 4 lightmap uv sets: skipped
            if (r.u32() != 4) throw ParseError("lightmap set count != 4");
            r.skip(static_cast<size_t>(4) * cnt * 8);
        }
        uint32_t G = r.u32();
        if (G > 4096) throw ParseError("absurd group count");
        const size_t g0 = m.groups.size();
        m.groups.resize(g0 + G);
        for (uint32_t gi = 0; gi < G; ++gi) {
            B3dGroup& g = m.groups[g0 + gi];
            g.name = r.str(); g.shader = r.str(); r.skip(64 + 4);
            g.blend = r.str(); g.surface = r.str();
            r.skip(3);
            r.skip(4); r.skip(1); r.skip(4); r.skip(1); r.skip(8); r.skip(1); r.skip(8); r.skip(1); r.skip(12); r.skip(1); r.skip(16);
            uint32_t k = r.u32();
            if (k > 64) throw ParseError("absurd texture count");
            if (k) g.texture = r.str();
            if (k > 1) r.str();
            uint32_t ne = r.u32();
            if (ne > 4096) throw ParseError("absurd extra count");
            for (uint32_t i = 0; i < ne; ++i) r.str();
        }
        for (uint32_t gi = 0; gi < G; ++gi) r.str();
        std::vector<uint32_t> ic(G), tc(G);
        for (auto& x : ic) x = r.u32();
        for (auto& x : tc) x = r.u32();
        for (uint32_t gi = 0; gi < G; ++gi) {
            if (static_cast<uint64_t>(ic[gi]) * 2 > r.n - r.p) throw ParseError("index list overruns file");
            auto& idx = m.groups[g0 + gi].idx;
            idx.resize(ic[gi]);
            for (auto& x : idx) { x = static_cast<uint32_t>(r.u16()) + static_cast<uint32_t>(vbase); if (x - vbase >= cnt) throw ParseError("index out of range"); }
        }
    } else {
        uint32_t nv = r.u32();
        if (static_cast<uint64_t>(nv) * 48 > r.n - r.p) throw ParseError("vertex list overruns file");
        for (uint32_t i = 0; i < nv; ++i) {
            float p[3], n[3], c[4];
            r.fl(p, 3); r.fl(n, 3); r.fl(c, 4);
            m.pos.insert(m.pos.end(), p, p + 3);
            m.nrm.insert(m.nrm.end(), n, n + 3);
            for (int k = 0; k < 4; ++k) m.col.push_back(ToByte(c[k]));
            uint32_t nuv = r.u32();
            if (nuv > 8) throw ParseError("absurd uv count");
            float uv[2][2] = {{0, 0}, {0, 0}};
            for (uint32_t k = 0; k < nuv; ++k) { float t[2]; r.fl(t, 2); if (k < 2) { uv[k][0] = t[0]; uv[k][1] = t[1]; } }
            m.uv0.insert(m.uv0.end(), uv[0], uv[0] + 2);
            m.uv1.insert(m.uv1.end(), uv[1], uv[1] + 2);
            uint32_t nw = r.u32();
            if (nw > 64) throw ParseError("absurd weight count");
            r.skip(static_cast<size_t>(nw) * 8);
        }
        uint32_t ng = r.u32();
        if (ng > 4096) throw ParseError("absurd group count");
        for (uint32_t gi = 0; gi < ng; ++gi) {
            B3dGroup g;
            int32_t mi = static_cast<int32_t>(r.u32());
            g.name = r.str();
            uint32_t a = r.u32();
            if (a > 4096) throw ParseError("absurd list");
            for (uint32_t i = 0; i < a; ++i) { std::string s = r.str(); if (i == 0) g.texture = s; }
            uint32_t b = r.u32();
            if (b > 4096) throw ParseError("absurd list");
            for (uint32_t i = 0; i < b; ++i) r.str();
            if (r.u8() != 0xc9) throw ParseError("unknown primitive type");
            uint32_t n = r.u32();
            if (static_cast<uint64_t>(n) * 5 > r.n - r.p) throw ParseError("face list overruns file");
            if (n % 3) throw ParseError("index count not multiple of 3");
            g.idx.resize(n);
            for (auto& x : g.idx) { x = r.u32(); if (x >= nv) throw ParseError("index out of range"); x += static_cast<uint32_t>(vbase); }
            r.skip(n);
            if (mi >= 0 && static_cast<size_t>(mi) < st.materials.size()) {
                g.shader = st.materials[mi].shader; g.blend = st.materials[mi].blend; g.surface = st.materials[mi].surface;
            }
            m.groups.push_back(std::move(g));
        }
        if (!st.haveBounds && nv) {
            for (int k = 0; k < 3; ++k) { m.bmin[k] = 1e30f; m.bmax[k] = -1e30f; }
            for (size_t i = vbase; i < m.VertexCount(); ++i)
                for (int k = 0; k < 3; ++k) {
                    if (m.pos[i * 3 + k] < m.bmin[k]) m.bmin[k] = m.pos[i * 3 + k];
                    if (m.pos[i * 3 + k] > m.bmax[k]) m.bmax[k] = m.pos[i * 3 + k];
                }
            st.haveBounds = true;
        }
    }
}

void ReadNode(Reader& r, State& st) {
    for (;;) {
        uint8_t t = r.u8();
        switch (t) {
            case 0x0b: return;
            case 0x0c: r.str(); break;
            case 0x0a: ReadNode(r, st); break;
            case 0x0f: ReadMesh(r, st); break;
            case 0x1b: { uint32_t n = r.u32(); if (n > 4096) throw ParseError("absurd list"); for (uint32_t i = 0; i < n; ++i) r.str(); break; }
            default: throw ParseError("unknown node token");
        }
    }
}

}  // namespace

bool ParseB3d(const uint8_t* data, size_t size, B3dModel& out, std::string* err) {
    try {
        out = B3dModel{};
        if (size < 9 || std::memcmp(data, "B3D 1.1 ", 8) != 0) throw ParseError("not a 'B3D 1.1 ' file");
        Reader r(data, size);
        r.p = 8;
        State st;
        st.out = &out;
        for (;;) {
            uint8_t t = r.u8();
            switch (t) {
                case 0x01: goto done;
                case 0x03: case 0x04: r.f32(); break;
                case 0x05: r.str(); break;
                case 0x06: r.skip(12); break;
                case 0x07: {
                    Material mt;
                    mt.shader = r.str(); mt.name = r.str();
                    r.skip(64 + 8);
                    mt.blend = r.str(); mt.surface = r.str();
                    uint32_t mask = r.u32();
                    r.skip(8 * !!(mask & 1) + 8 * !!(mask & 2) + 16 * !!(mask & 4) + 16 * !!(mask & 8) + 24 * !!(mask & 16) + 20 * !!(mask & 32));
                    r.u32();
                    st.materials.push_back(std::move(mt));
                    break;
                }
                case 0x08: case 0x09: {
                    r.u8(); r.str();
                    std::string file = r.str();
                    r.u32(); r.u32();
                    out.textures.push_back(std::move(file));
                    break;
                }
                case 0x0a: ReadNode(r, st); break;
                case 0x0e: {
                    B3dBone b;
                    b.name = r.str(); r.str(); b.flags = r.u8();
                    if (b.flags & 1) r.fl(b.pos, 3);
                    if (b.flags & 2) r.fl(b.axisAngle, 4);
                    if (b.flags & 4) r.skip(12);
                    out.bones.push_back(std::move(b));
                    break;
                }
                case 0x16: r.u32(); break;
                case 0x1c: st.converted = r.u8(); break;
                default: throw ParseError("unknown top-level token");
            }
        }
    done:
        // Legacy groups with no texture of their own fall back to the first model texture.
        for (auto& g : out.groups) if (g.texture.empty() && !out.textures.empty()) g.texture = out.textures[0];
        return true;
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return false;
    }
}

}  // namespace f3::gd
