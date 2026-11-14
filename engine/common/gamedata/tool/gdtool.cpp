// f3_gdtool: checks the gamedata library against an original install.
//   f3_gdtool <gamedir> scan          parse every b3d / tga / tre / emap / mapdata, print failures + totals
//   f3_gdtool <gamedir> map <Name>    stats for one map (tre base name)
//   f3_gdtool <gamedir> b3d <Name>    bounds / groups of one model
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include "gamedata/B3d.h"
#include "gamedata/GameData.h"
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
    if (argc < 4) return 2;
    std::string name = argv[3];
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
        return 0;
    }
    return 2;
}
