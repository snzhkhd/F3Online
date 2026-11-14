// gr2verify: checks the native Oodle0 decoder against the original granny2.dll (needs a 32-bit build on Windows).
//   gr2verify <dir-or-file>... [--dll path/to/granny2.dll]
// Without --dll (or on non-Win32 builds) only the native decode success rate is reported.
#include <algorithm>
#include <cmath>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "gr2.h"

#if defined(_WIN32) && !defined(_WIN64)
#include <windows.h>
typedef void(__stdcall* DecompressFn)(int, int, const void*, unsigned, unsigned, unsigned, void*);
#define HAVE_DLL 1
#endif

namespace fs = std::filesystem;

#ifdef HAVE_DLL
// --pose <mesh.gr2> <anim.gr2> : compare sampled local-pose transforms against granny2.dll's own animation sampler.
static bool g_quiet = false;
static int poseCompare(HMODULE h, const std::string& meshPath, const std::string& animPath, double* worstOut = nullptr) {
    typedef void*(__stdcall * ReadFn)(const char*);
    typedef void*(__stdcall * InfoFn)(void*);
    typedef void*(__stdcall * InstFn)(void*);
    typedef void*(__stdcall * PlayFn)(float, void*, void*);
    typedef void*(__stdcall * NewPoseFn)(int);
    typedef void(__stdcall * SampleFn)(void*, int, int, void*);
    typedef float*(__stdcall * GetTrFn)(void*, int);
    typedef void(__stdcall * SetClockFn)(void*, float);
    auto rd = (ReadFn)GetProcAddress(h, "_GrannyReadEntireFile@4");
    auto info = (InfoFn)GetProcAddress(h, "_GrannyGetFileInfo@4");
    auto inst = (InstFn)GetProcAddress(h, "_GrannyInstantiateModel@4");
    auto play = (PlayFn)GetProcAddress(h, "_GrannyPlayControlledAnimation@12");
    auto newPose = (NewPoseFn)GetProcAddress(h, "_GrannyNewLocalPose@4");
    auto sample = (SampleFn)GetProcAddress(h, "_GrannySampleModelAnimations@16");
    auto getTr = (GetTrFn)GetProcAddress(h, "_GrannyGetLocalPoseTransform@8");
    auto setClock = (SetClockFn)GetProcAddress(h, "_GrannySetControlRawLocalClock@8");
    if (!rd || !info || !inst || !play || !newPose || !sample || !getTr || !setClock) {
        std::fprintf(stderr, "missing DLL exports\n");
        return 2;
    }
    void* fm = rd(meshPath.c_str());
    void* fa = rd(animPath.c_str());
    if (!fm || !fa) {
        std::fprintf(stderr, "DLL could not read the files\n");
        return 2;
    }
    auto at = [](void* base, int off) { return *(uint8_t**)((uint8_t*)base + off); };
    uint8_t* im = (uint8_t*)info(fm);
    uint8_t* ia = (uint8_t*)info(fa);
    // file_info: models count @64 / array-of-pointers @68; animations count @80 / @84
    void* model = ((void**)at(im, 68))[0];
    void* anim = ((void**)at(ia, 84))[0];
    gr2::File mf, af;
    gr2::Scene ms, as;
    std::string err;
    if (!mf.loadFile(meshPath, &err) || !af.loadFile(animPath, &err) || !gr2::loadScene(mf, ms) || !gr2::loadScene(af, as) ||
        ms.skeletons.empty() || as.animations.empty()) {
        std::fprintf(stderr, "native load failed\n");
        return 2;
    }
    const gr2::Skeleton& sk = ms.skeletons[0];
    const gr2::Animation& an = as.animations[0];
    // The DLL binds an animation to a model by comparing track group name and model name. Pick the track group that covers
    // most bones and give the in-memory model that group's name (any skeleton can then serve as oracle).
    size_t bestG = 0;
    {
        int bestCov = -1;
        for (size_t gi = 0; gi < an.trackGroups.size(); gi++) {
            int cov = 0;
            for (auto& t : an.trackGroups[gi].transformTracks)
                for (auto& b : sk.bones) cov += b.name == t.name;
            if (cov > bestCov) {
                bestCov = cov;
                bestG = gi;
            }
        }
        uint8_t* tgArr = at(ia, 76);
        if (tgArr) {
            void* tgp = ((void**)tgArr)[bestG];
            if (tgp) *(void**)model = *(void**)tgp;
        }
    }
    bool dbg = std::getenv("GR2_VERBOSE") != nullptr;
    if (dbg) std::fprintf(stderr, "model %p anim %p\n", model, anim);
    void* mi = inst(model);
    if (dbg) std::fprintf(stderr, "instance %p\n", mi);
    void* ctl = play(0.0f, anim, mi);
    if (dbg) std::fprintf(stderr, "control %p\n", ctl);
    if (!ctl) {  // the DLL could not bind the animation to this model (track group / model name mismatch)
        if (worstOut) *worstOut = -1;
        if (!g_quiet) std::printf("DLL could not bind %s to %s\n", animPath.c_str(), meshPath.c_str());
        return 3;
    }
    void* pose = newPose((int)sk.bones.size());
    double worst = 0;
    int cmp = 0;
    for (int k = 0; k <= 8; k++) {
        float t = an.duration * k / 8.0f;
        if (k == 8) t = an.duration;
        setClock(ctl, t);
        sample(mi, 0, (int)sk.bones.size(), pose);
        for (size_t b = 0; b < sk.bones.size(); b++) {
            float* tr = getTr(pose, (int)b);  // flags, pos[3], quat[4], scaleShear[9]
            const gr2::TransformTrack* track = nullptr;
            for (auto& x : an.trackGroups[bestG].transformTracks)
                if (x.name == sk.bones[b].name) track = &x;
            gr2::Transform mine = track ? gr2::sampleTrack(*track, t, sk.bones[b].local) : sk.bones[b].local;
            uint32_t fl;
            std::memcpy(&fl, tr, 4);
            double d = 0;
            for (int i = 0; i < 3; i++) d = std::fmax(d, std::fabs(tr[1 + i] - mine.position[i]));
            {  // q and -q are the same rotation: the DLL may flip the sign (quaternion neighbourhood handling)
                double dp = 0, dn = 0;
                for (int i = 0; i < 4; i++) {
                    dp = std::fmax(dp, std::fabs(tr[4 + i] - mine.orientation[i]));
                    dn = std::fmax(dn, std::fabs(tr[4 + i] + mine.orientation[i]));
                }
                d = std::fmax(d, std::getenv("GR2_STRICT_SIGN") ? dp : std::fmin(dp, dn));
            }
            for (int i = 0; i < 9; i++) d = std::fmax(d, std::fabs(tr[8 + i] - mine.scaleShear[i]));
            cmp++;
            if (d > worst) worst = d;
            if (d > 1e-4 && !g_quiet)
                std::printf("DIFF t=%g bone %zu %s: max abs diff %g (dll q %g %g %g %g, mine %g %g %g %g)\n", t, b,
                            sk.bones[b].name.c_str(), d, tr[4], tr[5], tr[6], tr[7], mine.orientation[0],
                            mine.orientation[1], mine.orientation[2], mine.orientation[3]);
        }
    }
    if (worstOut) *worstOut = worst;
    if (!g_quiet) std::printf("pose compare: %d bone samples, worst abs diff %g\n", cmp, worst);
    return worst > 1e-4 ? 1 : 0;
}
#endif

int main(int argc, char** argv) {
    std::vector<std::string> inputs;
    std::string poseMesh, poseAnim, poseAll;
    std::string dllPath = "F:/_work/F3_Demo/granny2.dll";
    bool useDll = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--dll" && i + 1 < argc) {
            dllPath = argv[++i];
            useDll = true;
        } else if (a == "--pose-all" && i + 1 < argc) {
            poseAll = argv[++i];
        } else if (a == "--pose" && i + 2 < argc) {
            poseMesh = argv[++i];
            poseAnim = argv[++i];
        } else
            inputs.push_back(a);
    }
    if (!poseAll.empty()) {
#ifdef HAVE_DLL
        HMODULE h = LoadLibraryA(dllPath.c_str());
        if (!h) {
            std::fprintf(stderr, "cannot load %s\n", dllPath.c_str());
            return 2;
        }
        g_quiet = true;
        int n = 0, good = 0, nomesh = 0, unbound = 0;
        double worstAll = 0;
        std::map<std::string, gr2::Scene> meshCache;
        for (auto& e : fs::recursive_directory_iterator(poseAll)) {
            std::string name = e.path().filename().string();
            if (!e.is_regular_file() || name.size() < 9 || name.substr(name.size() - 9) != ".anim.gr2") continue;
            // candidate skeletons: every *.mesh.gr2 in the same directory whose bones cover all track names
            // (prefix matches first); the first one the DLL can bind the animation to is used
            gr2::File af;
            gr2::Scene as;
            if (!af.loadFile(e.path().string()) || !gr2::loadScene(af, as) || as.animations.empty()) continue;
            std::vector<std::pair<int, std::string>> cands;
            for (auto& m : fs::directory_iterator(e.path().parent_path())) {
                std::string mn = m.path().filename().string();
                if (mn.size() < 9 || mn.substr(mn.size() - 9) != ".mesh.gr2") continue;
                std::string mp = m.path().string();
                if (!meshCache.count(mp)) {
                    gr2::File mf;
                    gr2::Scene ms;
                    if (mf.loadFile(mp) && gr2::loadScene(mf, ms)) meshCache[mp] = ms;
                    else meshCache[mp] = gr2::Scene();
                }
                const gr2::Scene& ms = meshCache[mp];
                if (ms.skeletons.empty() || ms.models.empty()) continue;
                int found = 0, total = 0;
                for (auto& g : as.animations[0].trackGroups)
                    for (auto& t : g.transformTracks) {
                        bool f1 = false;
                        for (auto& b : ms.skeletons[0].bones) f1 = f1 || b.name == t.name;
                        found += f1;
                        total++;
                    }
                if (found == 0) continue;
                std::string stem = mn.substr(0, mn.size() - 9);
                int pri = name.compare(0, stem.size() + 1, stem + "_") == 0 ? 1000 + (int)stem.size() : 0;
                cands.push_back({-(pri + 100 * found / (total ? total : 1)), mp});
            }
            std::sort(cands.begin(), cands.end());
            if (cands.empty()) {
                nomesh++;
                continue;
            }
            double w = 0;
            int r = 3;
            if (std::getenv("GR2_VERBOSE")) std::fprintf(stderr, "%s\n", e.path().string().c_str());
            int tries = 0;
            for (auto& cd : cands) {
                if (++tries > 6) break;
                r = poseCompare(h, cd.second, e.path().string(), &w);
                if (r != 3) break;
            }
            if (r == 3) {
                unbound++;
                continue;
            }
            n++;
            if (r == 0) good++;
            else std::printf("MISMATCH %s (worst %g)\n", e.path().string().c_str(), w);
            if (w > worstAll) worstAll = w;
        }
        std::printf("pose compare vs granny2.dll: %d clips compared, %d within 1e-4, %d anim files without a matching skeleton file, %d not bindable by the DLL, worst abs diff %g\n", n, good, nomesh, unbound, worstAll);
        return good == n ? 0 : 1;
#else
        std::fprintf(stderr, "--pose-all needs a 32-bit Windows build\n");
        return 2;
#endif
    }
    if (!poseMesh.empty()) {
#ifdef HAVE_DLL
        HMODULE h = LoadLibraryA(dllPath.c_str());
        if (!h) {
            std::fprintf(stderr, "cannot load %s\n", dllPath.c_str());
            return 2;
        }
        return poseCompare(h, poseMesh, poseAnim);
#else
        std::fprintf(stderr, "--pose needs a 32-bit Windows build\n");
        return 2;
#endif
    }
    if (inputs.empty()) {
        std::fprintf(stderr, "usage: gr2verify <dir|file>... [--dll granny2.dll]\n");
        return 2;
    }
#ifdef HAVE_DLL
    DecompressFn dll = nullptr;
    if (useDll) {
        HMODULE h = LoadLibraryA(dllPath.c_str());
        if (!h) {
            std::fprintf(stderr, "cannot load %s\n", dllPath.c_str());
            return 2;
        }
        dll = (DecompressFn)GetProcAddress(h, "_GrannyDecompressData@28");
        if (!dll) {
            std::fprintf(stderr, "no _GrannyDecompressData@28\n");
            return 2;
        }
    }
#else
    if (useDll) std::fprintf(stderr, "note: --dll needs a 32-bit Windows build; ignoring\n");
#endif
    std::vector<std::string> files;
    for (auto& in : inputs) {
        if (fs::is_directory(in)) {
            for (auto& e : fs::recursive_directory_iterator(in))
                if (e.is_regular_file() && e.path().extension() == ".gr2") files.push_back(e.path().string());
        } else
            files.push_back(in);
    }
    int ok = 0, bad = 0, cmpOk = 0, cmpBad = 0;
    for (auto& path : files) {
        gr2::File f;
        std::string err;
        if (!f.loadFile(path, &err)) {
            std::printf("FAIL %s: %s\n", path.c_str(), err.c_str());
            bad++;
            continue;
        }
        ok++;
#ifdef HAVE_DLL
        if (dll) {
            for (size_t i = 0; i < f.sections.size(); i++) {
                const auto& s = f.sections[i];
                if (s.compression != 1 || s.decompressedSize == 0) continue;
                std::vector<uint8_t> a, b(s.decompressedSize + 64, 0xAA);
                f.decompressSection((int)i, a);
                // the DLL reads whole 32-bit words: give it a padded copy
                std::vector<uint8_t> src(s.compressedSize + 16, 0);
                std::memcpy(src.data(), f.raw.data() + s.dataOffset, s.compressedSize);
                dll(1, 0, src.data(), s.first16bit, s.first8bit, s.decompressedSize, b.data());
                if (std::memcmp(a.data(), b.data(), s.decompressedSize) == 0)
                    cmpOk++;
                else {
                    cmpBad++;
                    std::printf("MISMATCH %s section %zu\n", path.c_str(), i);
                }
            }
        }
#endif
    }
    std::printf("native decode+parse ok: %d / %zu (failed %d)\n", ok, files.size(), bad);
    if (useDll) std::printf("byte-exact vs granny2.dll: %d sections identical, %d mismatches\n", cmpOk, cmpBad);
    return bad || cmpBad ? 1 : 0;
}
