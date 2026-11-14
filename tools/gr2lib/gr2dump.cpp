// gr2dump: inspect Granny .gr2 files (skeleton / animation / mesh summary, JSON export, generic type tree, statistics).
//   gr2dump <file.gr2>                       summary
//   gr2dump <file.gr2> --json out.json [--fps N] [--no-samples]
//   gr2dump <file.gr2> --tree [maxItems]     generic type-tree dump of the root object
//   gr2dump <file.gr2> --sample <track> <t>  sample one transform track at time t
//   gr2dump --scan <dir>                     parse every .gr2 below dir and print success rates / curve statistics
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <string>

#include "gr2.h"

using namespace gr2;
namespace fs = std::filesystem;

static void printTree(const Value& v, int ind, int maxItems) {
    std::string pad(ind * 2, ' ');
    switch (v.kind) {
        case Value::Null: std::puts("null"); break;
        case Value::Int: std::printf("%lld\n", (long long)v.i); break;
        case Value::Real: std::printf("%g\n", v.r); break;
        case Value::String: std::printf("\"%s\"\n", v.s.c_str()); break;
        case Value::Transform:
            std::printf("transform[");
            for (size_t i = 0; i < v.a.size(); i++) std::printf("%g ", v.a[i].num());
            std::puts("]");
            break;
        case Value::Variant: std::puts("variant:"); printTree(v.m[0].second, ind, maxItems); break;
        case Value::Struct:
            std::puts("{");
            for (auto& kv : v.m) {
                std::printf("%s  %s: ", pad.c_str(), kv.first.c_str());
                printTree(kv.second, ind + 1, maxItems);
            }
            std::printf("%s}\n", pad.c_str());
            break;
        case Value::Array: {
            bool scalar = !v.a.empty() && (v.a[0].kind == Value::Int || v.a[0].kind == Value::Real);
            if (scalar) {
                std::printf("[%zu: ", v.a.size());
                for (size_t i = 0; i < v.a.size() && i < 16; i++) std::printf("%g ", v.a[i].num());
                std::puts(v.a.size() > 16 ? "...]" : "]");
                break;
            }
            std::printf("[%zu items]\n", v.a.size());
            for (size_t i = 0; i < v.a.size() && (int)i < maxItems; i++) {
                std::printf("%s  [%zu] ", pad.c_str(), i);
                printTree(v.a[i], ind + 1, maxItems);
            }
            break;
        }
    }
}

static void summary(const File& f, const Scene& s) {
    std::printf("version %u, %zu sections, root type @%u\n", f.version, f.sections.size(), f.rootType);
    for (size_t i = 0; i < f.sections.size(); i++) {
        auto& x = f.sections[i];
        if (x.decompressedSize || x.compressedSize)
            std::printf("  section %zu: compression %u, %u -> %u bytes, %u relocations\n", i, x.compression,
                        x.compressedSize, x.decompressedSize, x.relocCount);
    }
    std::printf("source: %s\n", s.fromFileName.c_str());
    for (auto& sk : s.skeletons) {
        std::printf("skeleton \"%s\": %zu bones\n", sk.name.c_str(), sk.bones.size());
        for (size_t i = 0; i < sk.bones.size(); i++) {
            auto& b = sk.bones[i];
            std::printf("  [%2zu] %-24s parent %2d  pos(%.4f %.4f %.4f) quat(%.4f %.4f %.4f %.4f)\n", i, b.name.c_str(),
                        b.parent, b.local.position[0], b.local.position[1], b.local.position[2],
                        b.local.orientation[0], b.local.orientation[1], b.local.orientation[2], b.local.orientation[3]);
        }
    }
    for (auto& m : s.meshes) {
        std::printf("mesh \"%s\": %u vertices, %u indices (%u triangles), %zu groups, %zu bound bones; components:",
                    m.name.c_str(), m.vertexCount, m.indexCount, m.indexCount / 3, m.groups.size(), m.boundBones.size());
        for (auto& c : m.vertexComponents) std::printf(" %s", c.c_str());
        std::puts("");
    }
    for (auto& a : s.animations) {
        std::printf("animation \"%s\": duration %g s, timestep %g, %zu track group(s)\n", a.name.c_str(), a.duration,
                    a.timeStep, a.trackGroups.size());
        for (auto& g : a.trackGroups) {
            size_t withP = 0, withQ = 0, withS = 0;
            std::set<int> degs;
            for (auto& t : g.transformTracks) {
                withP += !t.position.empty();
                withQ += !t.orientation.empty();
                withS += !t.scaleShear.empty();
                degs.insert(t.position.degree);
                degs.insert(t.orientation.degree);
            }
            std::printf("  group \"%s\": %zu transform tracks (position %zu, orientation %zu, scale/shear %zu), degrees:",
                        g.name.c_str(), g.transformTracks.size(), withP, withQ, withS);
            for (int d : degs) std::printf(" %d", d);
            std::puts("");
        }
    }
}

static int scanDir(const std::string& dir) {
    int total = 0, loaded = 0, sceneOk = 0, withSkel = 0, withAnim = 0, withMesh = 0, badCurve = 0, badParent = 0;
    int nonUnit = 0;
    long tracks = 0, curves = 0, totalVerts = 0, totalTris = 0;
    int badIdx = 0, badW = 0;
    float invErr = 0;
    long totalBones = 0, badBones = 0, loopNonZero = 0;
    std::map<uint32_t, int> flagHist;
    std::set<std::string> badBoneNames;
    std::map<std::string, int> degHist;  // "dim/degree/const"
    std::map<int, int> nSections;
    std::set<std::string> firstFail;
    std::vector<std::string> nonUnitEx, invBad;
    for (auto& e : fs::recursive_directory_iterator(dir)) {
        if (!e.is_regular_file() || e.path().extension() != ".gr2") continue;
        total++;
        File f;
        std::string err;
        if (!f.loadFile(e.path().string(), &err)) {
            if (firstFail.size() < 5) firstFail.insert(e.path().string() + ": " + err);
            continue;
        }
        loaded++;
        Scene sc;
        if (!loadScene(f, sc, &err)) {
            if (firstFail.size() < 5) firstFail.insert(e.path().string() + ": scene: " + err);
            continue;
        }
        sceneOk++;
        withSkel += !sc.skeletons.empty();
        withAnim += !sc.animations.empty();
        withMesh += !sc.meshes.empty();
        for (auto& sk : sc.skeletons) {
            for (size_t i = 0; i < sk.bones.size(); i++)
                if (sk.bones[i].parent >= (int)i || sk.bones[i].parent < -1) badParent++;
            // rest-pose world matrix times stored inverse world matrix must be the identity
            std::vector<float> w = worldMatrices(sk);
            for (size_t i = 0; i < sk.bones.size(); i++) {
                float boneErr = 0;
                for (int r = 0; r < 4; r++)
                    for (int c = 0; c < 4; c++) {
                        float v = 0;
                        for (int k = 0; k < 4; k++) v += w[i * 16 + r * 4 + k] * sk.bones[i].inverseWorld[k * 4 + c];
                        boneErr = std::max(boneErr, std::fabs(v - (r == c ? 1.0f : 0.0f)));
                    }
                totalBones++;
                if (boneErr > 0.02f) {
                    badBones++;
                    badBoneNames.insert(sk.bones[i].name);
                } else
                    invErr = std::max(invErr, boneErr);
            }
        }
        for (auto& m : sc.meshes) {
            uint32_t mx = 0;
            for (uint32_t ix : m.indices) {
                mx = std::max(mx, ix);
                if (ix >= m.vertexCount) badIdx++;
            }
            if (mx >= m.vertexCount && nonUnitEx.size() < 12)
                nonUnitEx.push_back(e.path().filename().string() + " mesh " + m.name + ": vertices " +
                                    std::to_string(m.vertexCount) + " indices " + std::to_string(m.indexCount) + " max index " +
                                    std::to_string(mx));
            for (size_t v = 0; v + 3 < m.boneWeights.size(); v += 4) {
                float s2 = m.boneWeights[v] + m.boneWeights[v + 1] + m.boneWeights[v + 2] + m.boneWeights[v + 3];
                if (std::fabs(s2 - 1.0f) > 0.02f) badW++;
            }
            totalVerts += m.vertexCount;
            totalTris += m.indexCount / 3;
        }
        for (auto& a : sc.animations) {
            for (auto& g : a.trackGroups) {
                flagHist[g.flags]++;
                if (g.loopTranslation[0] != 0 || g.loopTranslation[1] != 0 || g.loopTranslation[2] != 0) loopNonZero++;
                for (auto& t : g.transformTracks) {
                    tracks++;
                    const Curve* cs[3] = {&t.position, &t.orientation, &t.scaleShear};
                    for (const Curve* c : cs) {
                        if (c->empty()) {
                            if (!c->controls.empty()) badCurve++;
                            degHist["dim" + std::to_string(c->dim) + " empty"]++;
                            continue;
                        }
                        curves++;
                        if (c->controls.size() != c->knots.size() * c->dim) badCurve++;
                        degHist["dim" + std::to_string(c->dim) + " degree" + std::to_string(c->degree) +
                                (c->knots.size() == 1 ? " constant" : " animated")]++;
                        if (c->dim == 4) {
                            float q[4];
                            for (float tm : {c->knots.front(), c->knots.back(), 0.5f * (c->knots.front() + c->knots.back())}) {
                                if (c->sample(tm, q)) {
                                    float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
                                    if (std::fabs(n - 1.0f) > 1e-2f) {
                                        nonUnit++;
                                        if (nonUnitEx.size() < 6)
                                            nonUnitEx.push_back(e.path().filename().string() + " track " + t.name + " knots " +
                                                                std::to_string(c->knots.size()) + " |q|=" + std::to_string(n));
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    std::printf("files: %d, container+Oodle0+relocation ok: %d, scene extraction ok: %d\n", total, loaded, sceneOk);
    std::printf("with skeleton: %d, with animation: %d, with mesh: %d\n", withSkel, withAnim, withMesh);
    std::printf("transform tracks: %ld, non-empty curves: %ld, malformed curves: %d, bad bone parents: %d, "
                "non-unit sampled quaternions: %d\n",
                tracks, curves, badCurve, badParent, nonUnit);
    std::printf("skeleton check: max |world * inverseWorld - I| over the remaining bones = %g; meshes: %ld vertices, %ld triangles, %d out-of-range "
                "indices, %d vertices with bone weights not summing to 1\n", invErr, totalVerts, totalTris, badIdx, badW);
    std::printf("track groups by AccumulationFlags:");
    for (auto& kv : flagHist) std::printf(" %u:%d", kv.first, kv.second);
    std::printf("; groups with non-zero LoopTranslation: %ld\n", loopNonZero);
    for (auto& kv : degHist) std::printf("  curve %-28s %d\n", kv.first.c_str(), kv.second);
    for (auto& s : nonUnitEx) std::printf("  non-unit example: %s\n", s.c_str());
    std::printf("  bones with |world*inverseWorld - I| > 0.02: %ld of %ld (names:", badBones, totalBones);
    for (auto& s : badBoneNames) std::printf(" %s", s.c_str());
    std::printf(")\n");
    for (auto& s : firstFail) std::printf("FAIL %s\n", s.c_str());
    return loaded == total && sceneOk == total ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: gr2dump <file.gr2> [--json out.json [--fps N] [--no-samples]] [--tree [maxItems]] "
                     "[--sample <track> <t>]\n       gr2dump --scan <dir>\n");
        return 2;
    }
    if (!std::strcmp(argv[1], "--scan") && argc > 2) return scanDir(argv[2]);
    File f;
    std::string err;
    if (!f.loadFile(argv[1], &err)) {
        std::fprintf(stderr, "%s: %s\n", argv[1], err.c_str());
        return 1;
    }
    std::string jsonOut;
    float fps = 0;
    bool samples = true, tree = false;
    int maxItems = 3;
    std::string sampleTrack_;
    float sampleT = 0;
    bool doSample = false;
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--json" && i + 1 < argc) jsonOut = argv[++i];
        else if (a == "--fps" && i + 1 < argc) fps = (float)std::atof(argv[++i]);
        else if (a == "--no-samples") samples = false;
        else if (a == "--tree") {
            tree = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') maxItems = std::atoi(argv[++i]);
        } else if (a == "--sample" && i + 2 < argc) {
            doSample = true;
            sampleTrack_ = argv[++i];
            sampleT = (float)std::atof(argv[++i]);
        }
    }
    if (tree) {
        Walker w(f);
        printTree(w.readRoot(), 0, maxItems);
        return 0;
    }
    Scene s;
    if (!loadScene(f, s, &err)) {
        std::fprintf(stderr, "scene: %s\n", err.c_str());
        return 1;
    }
    if (doSample) {
        for (auto& a : s.animations)
            for (auto& g : a.trackGroups)
                for (auto& t : g.transformTracks)
                    if (t.name == sampleTrack_) {
                        Transform r = gr2::sampleTrack(t, sampleT, Transform());
                        std::printf("%s @%g: pos(%g %g %g) quat(%g %g %g %g)\n", t.name.c_str(), sampleT, r.position[0],
                                    r.position[1], r.position[2], r.orientation[0], r.orientation[1], r.orientation[2],
                                    r.orientation[3]);
                        return 0;
                    }
        std::fprintf(stderr, "track not found\n");
        return 1;
    }
    if (!jsonOut.empty()) {
        std::string j = sceneToJson(s, fps, samples);
        FILE* o = std::fopen(jsonOut.c_str(), "wb");
        if (!o) {
            std::fprintf(stderr, "cannot write %s\n", jsonOut.c_str());
            return 1;
        }
        std::fwrite(j.data(), 1, j.size(), o);
        std::fclose(o);
        std::printf("wrote %s (%zu bytes)\n", jsonOut.c_str(), j.size());
        return 0;
    }
    summary(f, s);
    return 0;
}
