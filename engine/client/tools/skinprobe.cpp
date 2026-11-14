// f3_skinprobe: dev tool. skinprobe <gamedir> <model> [animName...]
// Prints the b3d bone list, the Granny skeleton, per-bone skin vertex bounds and the root motion of animations.
#include <algorithm>
#include <cstdio>
#include "gamedata/GameData.h"
#include "gamedata/B3d.h"
#include "gr2/gr2.h"
using namespace f3;

int main(int argc, char** argv) {
    if (argc < 3) { std::puts("usage: f3_skinprobe <gamedir> <model> [anim...]"); return 2; }
    gd::GameData g;
    std::string err;
    if (!g.Open(argv[1], &err)) { std::puts(err.c_str()); return 1; }
    std::vector<uint8_t> b;
    g.Read(argv[2], gd::kResB3d, b);
    gd::B3dModel m;
    if (!gd::ParseB3d(b.data(), b.size(), m, &err)) { std::puts(err.c_str()); return 1; }
    std::printf("verts %zu skin %d bones %zu\n", m.VertexCount(), (int)m.HasSkin(), m.bones.size());
    for (size_t i = 0; i < m.bones.size(); i++) {
        auto& bb = m.bones[i];
        std::printf("%2zu %-14s p(%.3f %.3f %.3f)\n", i, bb.name.c_str(), bb.pos[0], bb.pos[1], bb.pos[2]);
    }
    std::vector<uint8_t> gb;
    g.Read(argv[2], gd::kResMeshGr2, gb);
    gr2::File f;
    if (!f.load(gb.data(), gb.size(), &err)) { std::puts(err.c_str()); return 1; }
    gr2::Scene sc;
    gr2::loadScene(f, sc, &err);
    auto& sk = sc.skeletons[0];
    auto w = gr2::worldMatrices(sk);
    std::printf("skeleton %s %zu bones\n", sk.name.c_str(), sk.bones.size());
    for (size_t i = 0; i < sk.bones.size(); i++)
        std::printf("%2zu %-14s par %2d world(%.3f %.3f %.3f)\n", i, sk.bones[i].name.c_str(), sk.bones[i].parent, w[i * 16 + 12], w[i * 16 + 13], w[i * 16 + 14]);
    for (size_t bi = 0; bi < m.bones.size() && m.HasSkin(); bi++) {
        float mn[3] = {1e9f, 1e9f, 1e9f}, mx[3] = {-1e9f, -1e9f, -1e9f};
        int n = 0;
        for (size_t v = 0; v < m.VertexCount(); v++)
            for (int k = 0; k < 4; k++)
                if (m.boneIdx[v * 4 + k] == bi && m.boneW[v * 4 + k] > 0.5f) {
                    n++;
                    for (int a = 0; a < 3; a++) { mn[a] = std::min(mn[a], m.pos[v * 3 + a]); mx[a] = std::max(mx[a], m.pos[v * 3 + a]); }
                }
        if (n) std::printf("bone %zu %s verts %d bbox (%.2f %.2f %.2f)-(%.2f %.2f %.2f)\n", bi, m.bones[bi].name.c_str(), n, mn[0], mn[1], mn[2], mx[0], mx[1], mx[2]);
    }
    for (int a = 3; a < argc; a++) {
        std::vector<uint8_t> ab;
        if (!g.Read(argv[a], gd::kResAnimGr2, ab)) { std::printf("no anim %s\n", argv[a]); continue; }
        gr2::File af;
        if (!af.load(ab.data(), ab.size(), &err)) { std::puts(err.c_str()); continue; }
        gr2::Scene as;
        gr2::loadScene(af, as, &err);
        if (as.animations.empty()) { std::puts("no animation"); continue; }
        auto& an = as.animations[0];
        std::printf("anim %s dur %.3f groups %zu\n", argv[a], an.duration, an.trackGroups.size());
        for (auto& tg : an.trackGroups) {
            std::printf(" group %s tracks %zu accumflags %u\n", tg.name.c_str(), tg.transformTracks.size(), tg.flags);
            for (float t : {0.f, an.duration * 0.5f, an.duration}) {
                auto pose = gr2::samplePose(sk, tg, t);
                auto ww = gr2::worldMatrices(sk, &pose);
                std::printf("  t=%.2f Root(%.3f %.3f %.3f) Head(%.3f %.3f %.3f) RHand(%.3f %.3f %.3f)\n", t, ww[1 * 16 + 12], ww[1 * 16 + 13], ww[1 * 16 + 14],
                            ww[20 * 16 + 12], ww[20 * 16 + 13], ww[20 * 16 + 14], ww[12 * 16 + 12], ww[12 * 16 + 13], ww[12 * 16 + 14]);
            }
            // foot travel over the cycle (skeleton units): the stance foot slides back at the body speed of a walk cycle
            for (size_t bi = 0; bi < sk.bones.size(); ++bi) {
                const std::string& bn = sk.bones[bi].name;
                if (bn.find("Foot") == std::string::npos && bn.find("foot") == std::string::npos) continue;
                float mn[3] = {1e9f, 1e9f, 1e9f}, mx[3] = {-1e9f, -1e9f, -1e9f};
                for (int s = 0; s <= 60; ++s) {
                    auto pose = gr2::samplePose(sk, tg, an.duration * s / 60.0f);
                    auto ww = gr2::worldMatrices(sk, &pose);
                    for (int c = 0; c < 3; ++c) { mn[c] = std::min(mn[c], ww[bi * 16 + 12 + c]); mx[c] = std::max(mx[c], ww[bi * 16 + 12 + c]); }
                }
                std::printf("  foot %-10s range x %.3f y %.3f z %.3f (b3d units x10)  -> walking speed ~ %.2f b3d units/s\n", bn.c_str(), mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2],
                            10.0f * std::max(mx[0] - mn[0], mx[2] - mn[2]) / (0.6f * an.duration));
            }
            break;
        }
    }
    return 0;
}
