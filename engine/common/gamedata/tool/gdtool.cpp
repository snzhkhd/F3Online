// f3_gdtool: checks the gamedata library against an original install.
//   f3_gdtool <gamedir> scan          parse every b3d / tga / tre / emap / mapdata, print failures + totals
//   f3_gdtool <gamedir> map <Name>    stats for one map (tre base name)
//   f3_gdtool <gamedir> b3d <Name>    bounds / groups of one model
//   f3_gdtool <gamedir> guilist       all .gui layouts: parse result and widget statistics
//   f3_gdtool <gamedir> guitree <Name> exact widget tree of one layout (full parser)
//   f3_gdtool <gamedir> gui <Name>    widget list of one Interface\*.gui layout
#include <chrono>
#include <cstdio>
#include <functional>
#include <cstring>
#include <string>
#include "gamedata/B3d.h"
#include "gamedata/GameData.h"
#include "gamedata/GuiFile.h"
#include "gamedata/MapFiles.h"
#include "gamedata/Tga.h"

using namespace f3::gd;

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: f3_gdtool <gamedir> scan|map <Name>|b3d <Name>\n"); return 2; }
    GameData gd;
    std::string err;
    if (!gd.Open(argv[1], &err)) { std::fprintf(stderr, "open: %s\n", err.c_str()); return 1; }
    std::string cmd = argv[2];
    std::vector<uint8_t> buf;
    if (cmd == "scan") {
        int ok[5] = {}, bad[5] = {};
        const char* nm[5] = {"b3d", "tga", "tre", "emap", "mapdata"};
        size_t tris = 0; int badSubs = 0;
        auto t0 = std::chrono::steady_clock::now();
        for (const ResourceInfo* r : gd.List(kResB3d)) {
            gd.Read(*r, buf); B3dModel m; std::string e;
            if (ParseB3d(buf.data(), buf.size(), m, &e)) ++ok[0]; else { ++bad[0]; std::printf("b3d FAIL %s/%s: %s\n", r->grp.c_str(), r->name.c_str(), e.c_str()); }
        }
        for (const ResourceInfo* r : gd.List(kResTga)) {
            gd.Read(*r, buf); Image im;
            if (DecodeTga(buf.data(), buf.size(), im)) ++ok[1]; else { ++bad[1]; }
        }
        for (const ResourceInfo* r : gd.List(kResTre)) {
            gd.Read(*r, buf); Tre t; std::string e;
            if (ParseTre(buf.data(), buf.size(), t, &e)) { ++ok[2]; tris += t.TriangleCount(); badSubs += t.badSubRecords;
                std::printf("tre %-34s vbs=%zu batches=%zu tris=%zu mats=%zu tex=%zu lm=%zu bad=%d\n", r->name.c_str(), t.vbs.size(), t.batches.size(), t.TriangleCount(), t.materials.size(), t.textures.size(), t.lightmaps.size(), t.badSubRecords); }
            else { ++bad[2]; std::printf("tre FAIL %s: %s\n", r->name.c_str(), e.c_str()); }
        }
        for (const ResourceInfo* r : gd.List(kResEmap)) {
            gd.Read(*r, buf); Emap m; std::string e;
            if (ParseEmap(buf.data(), buf.size(), m, &e)) ++ok[3]; else { ++bad[3]; std::printf("emap FAIL %s: %s\n", r->name.c_str(), e.c_str()); }
        }
        for (const ResourceInfo* r : gd.List(kResMapData)) {
            gd.Read(*r, buf); MapGrid m; std::string e;
            if (ParseMapData(buf.data(), buf.size(), m, &e)) ++ok[4]; else { ++bad[4]; std::printf("mapdata FAIL %s: %s\n", r->name.c_str(), e.c_str()); }
        }
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        for (int i = 0; i < 5; ++i) std::printf("%-8s ok=%d fail=%d\n", nm[i], ok[i], bad[i]);
        std::printf("tre triangles total=%zu undecoded sub-records=%d  (%.1fs)\n", tris, badSubs, s);
        return 0;
    }
    if (cmd == "dumpall" && argc >= 4) {  // analysis dump: <out>/<tre>.tris (f32 x9 + nrm y), .grid, .ents (text)
        std::string out = argv[3];
        for (const ResourceInfo* r : gd.List(kResTre)) {
            gd.Read(*r, buf); Tre t;
            if (!ParseTre(buf.data(), buf.size(), t, &err)) continue;
            std::string base = out + "/" + r->name;
            if (FILE* f = std::fopen((base + ".tris").c_str(), "wb")) {
                for (const TreBatch& b : t.batches) {
                    const TreVertexBuffer& vb = t.vbs[b.vb];
                    for (size_t i = 0; i + 2 < b.idx.size(); i += 3) {
                        float rec[13];
                        float ny = 0, nx = 0, nz = 0;
                        for (int k = 0; k < 3; ++k) {
                            for (int c = 0; c < 3; ++c) rec[k * 3 + c] = vb.xyz[b.idx[i + k] * 3 + c];
                            if (!vb.nrm.empty()) { nx += vb.nrm[b.idx[i + k] * 3]; ny += vb.nrm[b.idx[i + k] * 3 + 1]; nz += vb.nrm[b.idx[i + k] * 3 + 2]; }
                        }
                        rec[9] = nx / 3; rec[10] = ny / 3; rec[11] = nz / 3; rec[12] = static_cast<float>(b.material);
                        std::fwrite(rec, 4, 13, f);
                    }
                }
                std::fclose(f);
            }
            if (const ResourceInfo* m = gd.Find(r->name, kResMapData)) {
                std::vector<uint8_t> mb; MapGrid g;
                if (gd.Read(*m, mb) && ParseMapData(mb.data(), mb.size(), g)) if (FILE* f = std::fopen((base + ".grid").c_str(), "wb")) {
                    int hdr[2] = {g.w, g.h}; std::fwrite(hdr, 4, 2, f); std::fwrite(g.cells.data(), 1, g.cells.size(), f); std::fclose(f);
                }
            }
            for (const ResourceInfo* e : gd.List(kResEmap)) {
                std::vector<uint8_t> eb; Emap em;
                if (!gd.Read(*e, eb) || !ParseEmap(eb.data(), eb.size(), em) || ToLower(em.treName) != ToLower(r->name)) continue;
                if (FILE* f = std::fopen((base + ".ents").c_str(), "w")) {
                    std::fprintf(f, "bbox %g %g %g %g %g %g\n", t.bmin[0], t.bmin[1], t.bmin[2], t.bmax[0], t.bmax[1], t.bmax[2]);
                    for (const auto& en : em.entities) std::fprintf(f, "E %s %s %g %g %g %g\n", en.type.c_str(), en.tmpl.c_str(), en.pos[0], en.pos[1], en.pos[2], en.yaw);
                    for (const auto& ep : em.entryPoints) for (size_t i = 0; i < ep.pos.size(); ++i) std::fprintf(f, "P %d %g %g %g %g\n", ep.id, ep.pos[i][0], ep.pos[i][1], ep.pos[i][2], ep.yaw[i]);
                    std::fclose(f);
                }
                break;
            }
            std::printf("dumped %s\n", r->name.c_str());
        }
        return 0;
    }
    if (cmd == "guilist") {  // every Interface .gui: result of the heuristic reader and of the full tree reader
        for (const ResourceInfo* r : gd.List(kResGui)) {
            GuiFile g;
            GuiTree t;
            std::string err;
            const bool have = gd.Read(r->name, kResGui, buf);
            const bool okOld = have && ParseGui(buf.data(), buf.size(), g);
            const bool okTree = have && ParseGuiTree(buf.data(), buf.size(), t, &err);
            size_t kinds[5] = {0, 0, 0, 0, 0};
            std::function<void(const std::vector<GuiNode>&)> walk = [&](const std::vector<GuiNode>& v) { for (auto& n : v) { if (n.type < 5) ++kinds[n.type]; walk(n.children); } };
            if (okTree) walk(t.roots);
            std::printf("%-22s v%u %-6s tree=%4zu (win %zu btn %zu pic %zu lbl %zu edit %zu) heuristic=%-4s %s %zu bytes\n", r->name.c_str(), t.version,
                        okTree ? "OK" : "FAIL", okTree ? t.Count() : 0, kinds[0], kinds[1], kinds[2], kinds[3], kinds[4], okOld ? std::to_string(g.widgets.size()).c_str() : "FAIL",
                        err.c_str(), buf.size());
        }
        return 0;
    }
    if (cmd == "guitree" && argc >= 4) {  // exact widget tree of one layout: name, type, rect, texture, text id, tooltip ids
        std::string nm = argv[3];
        GuiTree t;
        std::string err;
        if (!gd.Read(nm, kResGui, buf) || !ParseGuiTree(buf.data(), buf.size(), t, &err)) { std::fprintf(stderr, "guitree %s: %s\n", nm.c_str(), err.c_str()); return 1; }
        static const char* tn[] = {"win", "btn", "pic", "lbl", "edt"};
        std::function<void(const GuiNode&, int)> dump = [&](const GuiNode& n, int d) {
            std::printf("%*s%s %s (%d,%d,%d,%d)%s%s tex=%s", d * 2, "", tn[n.type < 5 ? n.type : 0], n.name.c_str(), n.rect[0], n.rect[1], n.rect[2], n.rect[3], n.hidden ? " hidden" : "", n.skin.empty() ? "" : (" skin=" + n.skin).c_str(), n.picture().c_str());
            if (n.type >= 3) std::printf(" str=%u font=%s/%u col=%08X al=%u", n.strRef, n.fontName.c_str(), n.fontSize, n.textColor, n.alignment);
            if (n.tipTitleRef || n.tipTextRef) std::printf(" tip=%u/%u", n.tipTitleRef, n.tipTextRef);
            if (!n.tipImage.empty()) std::printf(" tipimg=%s", n.tipImage.c_str());
            std::printf("\n");
            for (auto& c : n.children) dump(c, d + 1);
        };
        std::printf("%s v%u hidden=%d roots=%zu total=%zu %s\n", t.name.c_str(), t.version, t.hidden, t.roots.size(), t.Count(), err.c_str());
        for (auto& n : t.roots) dump(n, 0);
        return 0;
    }
    if (argc < 4) return 2;
    std::string name = argv[3];
    if (cmd == "gui") {
        GuiFile g;
        if (!gd.Read(name, kResGui, buf) || !ParseGui(buf.data(), buf.size(), g)) { std::fprintf(stderr, "gui %s: not found / bad\n", name.c_str()); return 1; }
        std::printf("%s: %zu widgets\n", g.name.c_str(), g.widgets.size());
        for (auto& w : g.widgets)
            std::printf("%-28s %d%d%d (%d,%d,%d,%d) a=%d pic=%s font=%s\n", w.name.c_str(), w.flags[0], w.flags[1], w.flags[2], w.rect[0], w.rect[1], w.rect[2], w.rect[3], w.alpha, w.picture().c_str(), w.font().c_str());
        return 0;
    }
    if (cmd == "emap") {  // entity list of <Name>.emap
        if (!gd.Read(name, kResEmap, buf)) { std::fprintf(stderr, "no emap %s\n", name.c_str()); return 1; }
        Emap m;
        if (!ParseEmap(buf.data(), buf.size(), m, &err)) { std::fprintf(stderr, "emap: %s\n", err.c_str()); return 1; }
        for (const auto& e : m.entities)
            std::printf("%-6s %-50s pos %.2f %.2f %.2f yaw %.2f skin=%s\n", e.type.c_str(), e.tmpl.c_str(), e.pos[0], e.pos[1], e.pos[2], e.yaw, e.skin.c_str());
        return 0;
    }
    if (cmd == "map") {
        if (!gd.Read(name, kResTre, buf)) { std::fprintf(stderr, "no tre %s\n", name.c_str()); return 1; }
        Tre t;
        if (!ParseTre(buf.data(), buf.size(), t, &err)) { std::fprintf(stderr, "tre: %s\n", err.c_str()); return 1; }
        std::printf("grid %dx%d bbox (%g %g %g)-(%g %g %g) vbs=%zu batches=%zu tris=%zu bad=%d\n", t.gridW, t.gridH,
                    t.bmin[0], t.bmin[1], t.bmin[2], t.bmax[0], t.bmax[1], t.bmax[2], t.vbs.size(), t.batches.size(), t.TriangleCount(), t.badSubRecords);
        for (size_t i = 0; i < t.textures.size(); ++i) std::printf("  tex[%zu] %s %s\n", i, t.textures[i].c_str(), gd.Find(t.textures[i], kResTga) ? "ok" : "MISSING");
        for (size_t i = 0; i < t.lightmaps.size(); ++i) std::printf("  lm[%zu] %s %s\n", i, t.lightmaps[i].c_str(), gd.Find(t.lightmaps[i], kResTga, "Maps") ? "ok" : "MISSING");
        return 0;
    }
    if (cmd == "b3d") {
        if (!gd.Read(name, kResB3d, buf)) { std::fprintf(stderr, "no b3d %s\n", name.c_str()); return 1; }
        B3dModel m;
        if (!ParseB3d(buf.data(), buf.size(), m, &err)) { std::fprintf(stderr, "b3d: %s\n", err.c_str()); return 1; }
        std::printf("verts=%zu groups=%zu bbox (%g %g %g)-(%g %g %g)\n", m.VertexCount(), m.groups.size(), m.bmin[0], m.bmin[1], m.bmin[2], m.bmax[0], m.bmax[1], m.bmax[2]);
        for (auto& g : m.groups) std::printf("  %s shader=%s blend=%s tex=%s tris=%zu\n", g.name.c_str(), g.shader.c_str(), g.blend.c_str(), g.texture.c_str(), g.idx.size() / 3);
        { std::printf("  bones=%zu hides:", m.bones.size()); for (auto& h : m.hides) std::printf(" %s", h.c_str()); std::printf(" skin=%d pre=%d %c", (int)m.HasSkin(), (int)m.preconverted, 10); }
        if (getenv("F3_B3D_EXTRA")) for (auto& g : m.groups) { std::printf("    %s extra:", g.name.c_str()); for (uint8_t b : g.extra) std::printf(" %02x", b); std::printf(" %c", 10); }
        if (argc >= 5) {
            FILE* f = std::fopen(argv[4], "w");
            for (size_t gi = 0; gi < m.groups.size(); ++gi)
                for (size_t t = 0; t + 2 < m.groups[gi].idx.size(); t += 3)
                    for (int set = 0; set < 2; ++set) {
                        const auto& uv = set ? m.uv1 : m.uv0;
                        if (uv.empty()) continue;
                        std::fprintf(f, "%zu %d", gi, set);
                        for (int k = 0; k < 3; ++k) { uint32_t v = m.groups[gi].idx[t + k]; std::fprintf(f, " %g %g", uv[v * 2], uv[v * 2 + 1]); }
                        std::fputc(10, f);
                    }
            std::fclose(f);
        }
        return 0;
    }
    return 2;
}
