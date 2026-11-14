#include "MapFiles.h"

#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>
#include <tuple>

namespace f3::gd {

namespace {

struct Err : std::runtime_error { using std::runtime_error::runtime_error; };

struct Rd {
    const uint8_t* b; size_t n; size_t o = 0;
    Rd(const uint8_t* p, size_t len) : b(p), n(len) {}
    void need(size_t k) const { if (o + k > n) throw Err("read past end"); }
    size_t left() const { return n - o; }
    bool eof() const { return o >= n; }
    uint8_t r8() { need(1); return b[o++]; }
    uint16_t r16() { need(2); uint16_t v = static_cast<uint16_t>(b[o] | (b[o + 1] << 8)); o += 2; return v; }
    uint32_t r32() { need(4); uint32_t v = b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (static_cast<uint32_t>(b[o + 3]) << 24); o += 4; return v; }
    float rf() { uint32_t v = r32(); float f; std::memcpy(&f, &v, 4); return f; }
    std::string str() { uint16_t l = r16(); need(l); std::string s(reinterpret_cast<const char*>(b) + o, l); o += l; return s; }
    Rd sub(size_t len) { need(len); Rd r(b + o, len); o += len; return r; }
    void skip(size_t k) { need(k); o += k; }
    std::string tag() { need(4); std::string s(reinterpret_cast<const char*>(b) + o, 4); o += 4; return s; }
};

struct Eeov { std::string inst, skin; uint32_t team = 0; std::vector<std::string> inv; };

Eeov ParseEeov(Rd& r) {
    Eeov e;
    if (r.tag() != "EEOV") throw Err("EEOV tag expected");
    uint32_t ver = r.r32();
    uint32_t size = r.r32();
    if (size < 12 || (ver != 1 && ver != 2)) throw Err("EEOV header");
    Rd b = r.sub(size - 12);
    e.inst = b.str();
    b.skip(3);
    e.team = b.r32();
    b.r32();
    b.str(); b.str();
    uint32_t np = b.r32();
    if (np > 4096) throw Err("EEOV pair count");
    for (uint32_t i = 0; i < np; ++i) { b.str(); b.r32(); }
    b.r8(); b.rf();
    e.skin = b.str();
    uint8_t nf = b.r8();
    for (uint8_t i = 0; i < nf; ++i) b.str();
    b.r8(); b.r8();
    if (ver >= 2) {
        uint32_t ni = b.r32();
        if (ni > 4096) throw Err("EEOV inventory count");
        for (uint32_t i = 0; i < ni; ++i) e.inv.push_back(b.str());
    }
    return e;
}

}  // namespace

bool ParseEmap(const uint8_t* data, size_t size, Emap& out, std::string* err) {
    try {
        out = Emap{};
        Rd r(data, size);
        if (r.tag() != "EMAP") throw Err("not an EMAP file");
        uint32_t ver = r.r32(), hdrSize = r.r32();
        r.r32();
        if (ver != 5) throw Err("EMAP version");
        out.treName = r.str(); r.str(); out.automapName = r.str();
        if (out.treName.size() > 2 && out.treName.compare(out.treName.size() - 2, 2, ".8") == 0) out.treName.resize(out.treName.size() - 2);
        if (hdrSize < r.o || hdrSize > size) throw Err("EMAP header size");
        r.o = hdrSize;
        while (!r.eof()) {
            std::string tag = r.tag();
            r.r32();
            uint32_t sz = r.r32();
            if (sz < 12) throw Err("chunk size");
            Rd b = r.sub(sz - 12);
            if (tag == "EME2") {
                MapEntity e;
                e.name = b.str();
                size_t dot = e.name.rfind('.');
                if (dot == std::string::npos) throw Err("EME2 template without type suffix");
                e.tmpl = e.name.substr(0, dot); e.type = e.name.substr(dot + 1);
                for (int k = 0; k < 3; ++k) e.pos[k] = b.rf();
                e.yaw = b.rf(); b.rf(); b.rf();
                b.r8();
                Eeov ov = ParseEeov(b);
                e.instance = ov.inst; e.skin = ov.skin; e.team = ov.team; e.inventory = std::move(ov.inv);
                out.entities.push_back(std::move(e));
            } else if (tag == "EMEP") {
                EntryPoint ep;
                ep.id = b.r8();
                size_t n = b.left() / 16;
                for (size_t k = 0; k < n; ++k) { std::array<float, 3> p; for (int q = 0; q < 3; ++q) p[q] = b.rf(); ep.pos.push_back(p); }
                for (size_t k = 0; k < n; ++k) ep.yaw.push_back(b.rf());
                out.entryPoints.push_back(std::move(ep));
            } else if (tag == "ECAM") {
                for (int k = 0; k < 4; ++k) out.cam[k] = b.rf();
            }
        }
        return true;
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return false;
    }
}

bool ParseMapData(const uint8_t* data, size_t size, MapGrid& out, std::string* err) {
    try {
        out = MapGrid{};
        Rd r(data, size);
        uint32_t w = r.r32(), h = r.r32();
        if (w == 0 || h == 0 || static_cast<uint64_t>(w) * h > (1u << 26)) throw Err("bad grid size");
        out.w = static_cast<int>(w); out.h = static_cast<int>(h);
        out.cells.assign(static_cast<size_t>(w) * h, 0);
        for (uint32_t y = 0; y < h; ++y) {
            uint32_t x = 0;
            while (x < w) {
                uint32_t run = r.r32();
                uint8_t v = r.r8();
                if (run == 0 || x + run > w) throw Err("bad run");
                std::memset(&out.cells[static_cast<size_t>(y) * w + x], v, run);
                x += run;
            }
        }
        return true;
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return false;
    }
}

bool ParseEntityTemplate(const uint8_t* data, size_t size, EntityTemplate& out, std::string* err) {
    try {
        out = EntityTemplate{};
        Rd r(data, size);
        while (!r.eof()) {
            std::string tag = r.tag();
            r.r32();
            uint32_t sz = r.r32();
            if (sz < 12) throw Err("chunk size");
            Rd b = r.sub(sz - 12);
            if (tag == "EEN2") {
                out.model = b.str(); b.str(); b.str();
                b.skip(5);
                Eeov ov = ParseEeov(b);
                out.skin = ov.skin;
                return true;
            }
        }
        throw Err("no EEN2 chunk");
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return false;
    }
}

// ------------------------------------------------------------------------------------------------
// .tre
// ------------------------------------------------------------------------------------------------
namespace {

// Index stream (IDXS chunk, u16 words), see docs/formats/maps.md section 4:
//   group  = 0x8000|materialId, len, <sub-records>
//   sub    = 0x4000|(lightmapPage+1) (0 = no lightmap), len, <runs>
//   run    = textureIndex+1 (0 = none), count (multiple of 3), count x vertex index (absolute in the vertex buffer),
//            plus one 0 pad word when count is odd (every record is a whole number of 32-bit words)
struct Run { uint16_t kind; size_t begin, count; };
bool ParseRuns(const uint16_t* u, size_t q, size_t e2, uint32_t vcount, std::vector<Run>& runs) {
    while (q < e2) {
        if (q + 2 > e2) return false;
        uint16_t kind = u[q];
        size_t cnt = u[q + 1];
        if (cnt % 3 != 0 || q + 2 + cnt > e2) return false;
        for (size_t i = 0; i < cnt; ++i) if (u[q + 2 + i] >= vcount) return false;
        runs.push_back({kind, q + 2, cnt});
        q += 2 + cnt;
        if (cnt & 1) { if (q >= e2 || u[q] != 0) return false; ++q; }
    }
    return true;
}

}  // namespace

bool ParseTre(const uint8_t* data, size_t size, Tre& out, std::string* err) {
    try {
        out = Tre{};
        Rd r(data, size);
        if (r.tag() != "8TRE") throw Err("not a TRE file");
        uint32_t total = r.r32();
        if (static_cast<size_t>(total) + 8 > size) throw Err("TRE size beyond file");
        Rd body(data + 8, total);
        size_t treeOff = 0, treeLen = 0;
        while (!body.eof()) {
            std::string tag = body.tag();
            uint32_t sz = body.r32();
            size_t chunkStart = body.o;
            Rd c = body.sub(sz);
            if (tag == "LGTD") {
                while (!c.eof()) {
                    std::string st = c.tag(); uint32_t ss = c.r32(); Rd s = c.sub(ss);
                    if (st == "LGTR" && ss >= 52) {
                        TreLight l;
                        l.type = s.r32(); s.r32();
                        for (int k = 0; k < 3; ++k) l.color[k] = s.rf();
                        for (int k = 0; k < 3; ++k) l.pos[k] = s.rf();
                        for (int k = 0; k < 3; ++k) l.dir[k] = s.rf();
                        out.lights.push_back(l);
                    }
                }
            } else if (tag == "LVLD") {
                std::string st = c.tag(); uint32_t ss = c.r32(); Rd s = c.sub(ss);
                if (st == "INFO") { out.gridW = static_cast<int>(s.r32()); out.gridH = static_cast<int>(s.r32()); }
            } else if (tag == "MATD") {
                while (!c.eof()) {
                    std::string st = c.tag(); uint32_t ss = c.r32(); Rd s = c.sub(ss);
                    if (st == "MATR" && ss >= 36) {
                        s.r32();
                        std::string nm(reinterpret_cast<const char*>(s.b) + s.o, 32);
                        nm.resize(nm.find('\0') == std::string::npos ? nm.size() : nm.find('\0'));
                        out.materials.push_back(nm);
                    }
                }
            } else if (tag == "TXTD") {
                c.r32(); c.r32();
                while (!c.eof()) {
                    if (c.tag() != "TXTR") throw Err("TXTR expected");
                    uint32_t l = c.r32(); Rd s = c.sub(l);
                    std::string nm(reinterpret_cast<const char*>(s.b), l);
                    nm.resize(nm.find('\0') == std::string::npos ? nm.size() : nm.find('\0'));
                    bool ctx = nm.size() > 4 && nm.compare(nm.size() - 4, 4, ".ctx") == 0;
                    (ctx ? out.lightmaps : out.textures).push_back(nm);
                }
            } else if (tag == "VTXD") {
                uint32_t nbuf = c.r32();
                if (nbuf > 64) throw Err("absurd vertex buffer count");
                for (uint32_t bi = 0; bi < nbuf; ++bi) {
                    if (c.tag() != "VTXB") throw Err("VTXB expected");
                    uint32_t vs = c.r32(); Rd vb = c.sub(vs);
                    TreVertexBuffer v;
                    v.count = vb.r32();
                    while (!vb.eof()) {
                        std::string st = vb.tag(); uint32_t ss = vb.r32(); Rd s = vb.sub(ss);
                        auto floats = [&](std::vector<float>& o, uint32_t per) {
                            if (ss != static_cast<uint64_t>(v.count) * per * 4) throw Err("vertex stream size");
                            o.resize(static_cast<size_t>(v.count) * per);
                            std::memcpy(o.data(), s.b, ss);
                        };
                        if (st == "XYZ ") floats(v.xyz, 3);
                        else if (st == "NRML") floats(v.nrm, 3);
                        else if (st == "TXUV") floats(v.txuv, 2);
                        else if (st == "LMUV") floats(v.lmuv, 2);
                        else if (st == "DIFU") {
                            if (ss != static_cast<uint64_t>(v.count) * 4) throw Err("DIFU size");
                            v.rgba.resize(ss);
                            for (uint32_t i = 0; i < v.count; ++i) {  // D3DCOLOR BGRA -> RGBA
                                v.rgba[i * 4 + 0] = s.b[i * 4 + 2]; v.rgba[i * 4 + 1] = s.b[i * 4 + 1];
                                v.rgba[i * 4 + 2] = s.b[i * 4 + 0]; v.rgba[i * 4 + 3] = s.b[i * 4 + 3];
                            }
                        }
                    }
                    if (v.xyz.empty()) throw Err("vertex buffer without XYZ");
                    out.vbs.push_back(std::move(v));
                }
            } else if (tag == "TREE") {
                treeOff = 8 + chunkStart; treeLen = size - treeOff;  // declared size is a few bytes short; geometry may spill past it
            }
        }
        if (out.vbs.empty() || !treeLen) throw Err("TRE without geometry");

        // Flat walk of the TREE chunk. Container sizes (NODE/LEAF/IDXD) are not reliable, so only the
        // leaf chunks (INFO/IDXS/FLGS) are stepped by size; anything else resyncs on the next NODE/LEAF tag.
        std::map<std::tuple<int, int, int, int>, size_t> batchIndex;
        const uint8_t* t = data + treeOff;
        size_t p = 0;
        int curVb = 0;
        bool haveBox = false;
        auto u32at = [&](size_t o) { uint32_t v; std::memcpy(&v, t + o, 4); return v; };
        while (p + 8 <= treeLen) {
            const char* tg = reinterpret_cast<const char*>(t + p);
            uint32_t s = u32at(p + 4);
            auto is = [&](const char* x) { return std::memcmp(tg, x, 4) == 0; };
            if (is("NODE") || is("LEAF") || is("IDXD")) { p += 8; continue; }
            if (is("INFO")) {
                if (s >= 28 && p + 8 + s <= treeLen) {
                    if (!haveBox) { std::memcpy(out.bmin, t + p + 8, 12); std::memcpy(out.bmax, t + p + 20, 12); haveBox = true; }
                    curVb = static_cast<int>(u32at(p + 8 + 24));
                }
                p += 8 + s; continue;
            }
            if (is("FLGS")) { p += 8 + s; continue; }
            if (is("IDXS")) {
                if (p + 8 + s > treeLen || curVb < 0 || curVb >= static_cast<int>(out.vbs.size())) throw Err("bad IDXS: vb=" + std::to_string(curVb) + " nvb=" + std::to_string(out.vbs.size()) + " p=" + std::to_string(p) + " s=" + std::to_string(s) + " treeLen=" + std::to_string(treeLen));
                size_t nw = s / 2;
                std::vector<uint16_t> u(nw);
                std::memcpy(u.data(), t + p + 8, nw * 2);
                const uint32_t vcount = out.vbs[curVb].count;
                size_t i = 0;
                while (i + 2 <= nw) {
                    uint16_t gtag = u[i], glen = u[i + 1];
                    if (!(gtag & 0x8000) || i + 2 + glen > nw) { ++out.badSubRecords; break; }
                    int mat = gtag & 0x7fff;
                    size_t j = i + 2, gend = i + 2 + glen;
                    while (j + 2 <= gend) {
                        uint16_t stag = u[j], slen = u[j + 1];
                        if ((stag & 0xC000) != 0x4000 || j + 2 + slen > gend) { ++out.badSubRecords; break; }
                        int lm = (stag & 0x3fff) - 1;
                        std::vector<Run> runs;
                        if (ParseRuns(u.data(), j + 2, j + 2 + slen, vcount, runs)) {
                            for (auto& run : runs) {
                                int tex = static_cast<int>(run.kind) - 1;
                                auto key = std::make_tuple(curVb, mat, tex, lm);
                                auto it = batchIndex.find(key);
                                if (it == batchIndex.end()) {
                                    it = batchIndex.emplace(key, out.batches.size()).first;
                                    TreBatch b; b.vb = curVb; b.material = mat; b.texture = tex; b.lightmap = lm;
                                    out.batches.push_back(std::move(b));
                                }
                                auto& idx = out.batches[it->second].idx;
                                idx.insert(idx.end(), u.begin() + run.begin, u.begin() + run.begin + run.count);
                            }
                        } else {
                            ++out.badSubRecords;
                        }
                        j += 2 + slen;
                    }
                    i = gend;
                }
                p += 8 + s; continue;
            }
            // unknown bytes: resync on the next node tag
            size_t q = p + 1;
            for (; q + 4 <= treeLen; ++q)
                if (std::memcmp(t + q, "NODE", 4) == 0 || std::memcmp(t + q, "LEAF", 4) == 0) break;
            if (q + 4 > treeLen) break;
            p = q;
        }
        return true;
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return false;
    }
}

}  // namespace f3::gd
