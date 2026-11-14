#include "CritterRig.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include "common/log.h"
#include "gamedata/B3d.h"

namespace f3 {

namespace {

constexpr float kSkelToModel = 10.0f;  // Granny skeleton units -> b3d units

std::string Lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void Mul4(const float* a, const float* b, float* c) {  // row-major, row-vector convention (c = a * b)
    for (int r = 0; r < 4; ++r)
        for (int col = 0; col < 4; ++col) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a[r * 4 + k] * b[k * 4 + col];
            c[r * 4 + col] = s;
        }
}

Mesh BuildDynamicMesh(const SkinPartSrc& p) {
    Mesh m{};
    const size_t nv = p.pos.size() / 3;
    m.vertexCount = static_cast<int>(nv);
    m.triangleCount = static_cast<int>(p.idx.size() / 3);
    m.vertices = static_cast<float*>(MemAlloc(static_cast<unsigned>(nv * 3 * sizeof(float))));
    m.normals = static_cast<float*>(MemAlloc(static_cast<unsigned>(nv * 3 * sizeof(float))));
    m.texcoords = static_cast<float*>(MemAlloc(static_cast<unsigned>(nv * 2 * sizeof(float))));
    m.colors = static_cast<unsigned char*>(MemAlloc(static_cast<unsigned>(nv * 4)));
    m.indices = static_cast<unsigned short*>(MemAlloc(static_cast<unsigned>(p.idx.size() * sizeof(unsigned short))));
    for (size_t i = 0; i < nv; ++i) {
        m.vertices[i * 3 + 0] = p.pos[i * 3 + 0];
        m.vertices[i * 3 + 1] = p.pos[i * 3 + 1];
        m.vertices[i * 3 + 2] = -p.pos[i * 3 + 2];
        m.normals[i * 3 + 0] = p.nrm[i * 3 + 0];
        m.normals[i * 3 + 1] = p.nrm[i * 3 + 1];
        m.normals[i * 3 + 2] = -p.nrm[i * 3 + 2];
        m.texcoords[i * 2 + 0] = p.uv0.empty() ? 0 : p.uv0[i * 2 + 0];
        m.texcoords[i * 2 + 1] = p.uv0.empty() ? 0 : p.uv0[i * 2 + 1];
        for (int k = 0; k < 4; ++k) m.colors[i * 4 + k] = p.col.empty() ? 255 : p.col[i * 4 + k];
    }
    for (size_t t = 0; t + 2 < p.idx.size(); t += 3) {
        m.indices[t + 0] = static_cast<unsigned short>(p.idx[t + 0]);
        m.indices[t + 1] = static_cast<unsigned short>(p.idx[t + 2]);
        m.indices[t + 2] = static_cast<unsigned short>(p.idx[t + 1]);
    }
    UploadMesh(&m, true);  // keeps the CPU copies: they are the staging arrays for UpdateMeshBuffer
    return m;
}

}  // namespace

const char* RigLibrary::ClipKey(ClipKind k) {
    switch (k) {
        case ClipKind::Idle: return "idle";
        case ClipKind::Walk: return "walk";
        case ClipKind::Run: return "run";
        case ClipKind::Attack: return "attack";
        case ClipKind::Hit: return "hit";
        case ClipKind::Death: return "death";
        default: return "?";
    }
}

SkinnedInstance::SkinnedInstance(const SkinModel* m) : model_(m) {
    inst_.bmin = Vector3{-3, 0, -1};
    inst_.bmax = Vector3{3, m->height, 1};
    for (const SkinPartSrc& sp : m->parts) {
        DrawPart dp;
        dp.mesh = BuildDynamicMesh(sp);
        dp.tex = sp.tex;
        dp.flags = sp.flags;
        dp.skin = sp.skinTex;
        dp.texName = sp.texName;
        inst_.parts.push_back(std::move(dp));
    }
    palette_.assign(m->boneMap.size() * 16, 0.0f);
}

SkinnedInstance::~SkinnedInstance() {
    for (DrawPart& p : inst_.parts) UnloadMesh(p.mesh);
}

void SkinnedInstance::Pose(const Clip* a, float ta, const Clip* b, float tb, float blendToA) {
    const SkinModel& m = *model_;
    const gr2::Skeleton& sk = m.sk;
    std::vector<gr2::Transform> pose;
    if (a && a->valid) pose = gr2::samplePose(sk, a->tg, ta);
    else { pose.reserve(sk.bones.size()); for (const auto& bn : sk.bones) pose.push_back(bn.local); }
    if (b && b->valid && blendToA < 0.999f) {
        std::vector<gr2::Transform> pb = gr2::samplePose(sk, b->tg, tb);
        const float k = std::clamp(blendToA, 0.0f, 1.0f);  // weight of A (the new clip)
        for (size_t i = 0; i < pose.size(); ++i) {
            gr2::Transform& o = pose[i];
            const gr2::Transform& q = pb[i];
            for (int c = 0; c < 3; ++c) o.position[c] = q.position[c] + (o.position[c] - q.position[c]) * k;
            float dot = 0;
            for (int c = 0; c < 4; ++c) dot += o.orientation[c] * q.orientation[c];
            const float sgn = dot < 0 ? -1.0f : 1.0f;
            float len = 0;
            for (int c = 0; c < 4; ++c) { o.orientation[c] = q.orientation[c] * sgn + (o.orientation[c] - q.orientation[c] * sgn) * k; len += o.orientation[c] * o.orientation[c]; }
            len = 1.0f / std::sqrt(std::max(len, 1e-12f));
            for (int c = 0; c < 4; ++c) o.orientation[c] *= len;
        }
    }
    std::vector<float> world = gr2::worldMatrices(sk, &pose);
    for (size_t i = 0; i < m.boneMap.size(); ++i) {
        float* P = &palette_[i * 16];
        const int j = m.boneMap[i];
        if (j < 0) { std::memset(P, 0, 16 * sizeof(float)); P[0] = P[5] = P[10] = P[15] = 1; continue; }
        Mul4(&m.invWorld[static_cast<size_t>(j) * 16], &world[static_cast<size_t>(j) * 16], P);
    }
    const float inv = 1.0f / kSkelToModel;
    for (size_t pi = 0; pi < m.parts.size(); ++pi) {
        const SkinPartSrc& sp = m.parts[pi];
        Mesh& mesh = inst_.parts[pi].mesh;
        const size_t nv = sp.pos.size() / 3;
        for (size_t v = 0; v < nv; ++v) {
            const float px = sp.pos[v * 3] * inv, py = sp.pos[v * 3 + 1] * inv, pz = sp.pos[v * 3 + 2] * inv;
            const float nx = sp.nrm[v * 3], ny = sp.nrm[v * 3 + 1], nz = sp.nrm[v * 3 + 2];
            float ox = 0, oy = 0, oz = 0, onx = 0, ony = 0, onz = 0;
            for (int k = 0; k < 4; ++k) {
                const float w = sp.bw[v * 4 + k];
                if (w <= 0) continue;
                const float* M = &palette_[static_cast<size_t>(sp.bidx[v * 4 + k]) * 16];
                ox += w * (px * M[0] + py * M[4] + pz * M[8] + M[12]);
                oy += w * (px * M[1] + py * M[5] + pz * M[9] + M[13]);
                oz += w * (px * M[2] + py * M[6] + pz * M[10] + M[14]);
                onx += w * (nx * M[0] + ny * M[4] + nz * M[8]);
                ony += w * (nx * M[1] + ny * M[5] + nz * M[9]);
                onz += w * (nx * M[2] + ny * M[6] + nz * M[10]);
            }
            const float nl = 1.0f / std::max(1e-6f, std::sqrt(onx * onx + ony * ony + onz * onz));
            mesh.vertices[v * 3 + 0] = ox * kSkelToModel;
            mesh.vertices[v * 3 + 1] = oy * kSkelToModel;
            mesh.vertices[v * 3 + 2] = -oz * kSkelToModel;
            mesh.normals[v * 3 + 0] = onx * nl;
            mesh.normals[v * 3 + 1] = ony * nl;
            mesh.normals[v * 3 + 2] = -onz * nl;
        }
        UpdateMeshBuffer(mesh, 0, mesh.vertices, static_cast<int>(nv * 3 * sizeof(float)), 0);
        UpdateMeshBuffer(mesh, 2, mesh.normals, static_cast<int>(nv * 3 * sizeof(float)), 0);
    }
}

SkinModel* RigLibrary::Model(const std::string& name) {
    const std::string key = Lower(name);
    auto it = models_.find(key);
    if (it != models_.end()) return it->second->ok ? it->second.get() : nullptr;
    auto sm = std::make_unique<SkinModel>();
    sm->name = name;
    SkinModel* out = sm.get();
    models_[key] = std::move(sm);

    gd::GameData& data = assets_.data();
    std::vector<uint8_t> buf;
    gd::B3dModel b3d;
    std::string err;
    if (!data.Read(name, gd::kResB3d, buf) || !gd::ParseB3d(buf.data(), buf.size(), b3d, &err) || !b3d.HasSkin() || b3d.bones.empty()) {
        return nullptr;
    }
    std::vector<uint8_t> gb;
    gr2::File f;
    gr2::Scene sc;
    if (!data.Read(name, gd::kResMeshGr2, gb) || !f.load(gb.data(), gb.size(), &err) || !gr2::loadScene(f, sc, &err) || sc.skeletons.empty()) {
        log::Warn("rig %s: skeleton: %s", name.c_str(), err.c_str());
        return nullptr;
    }
    out->sk = sc.skeletons[0];
    out->invWorld.resize(out->sk.bones.size() * 16);
    for (size_t i = 0; i < out->sk.bones.size(); ++i)
        std::memcpy(&out->invWorld[i * 16], out->sk.bones[i].inverseWorld, 16 * sizeof(float));
    out->boneMap.assign(b3d.bones.size(), -1);
    int mapped = 0;
    for (size_t i = 0; i < b3d.bones.size(); ++i) {
        const std::string bn = Lower(b3d.bones[i].name);
        for (size_t j = 0; j < out->sk.bones.size(); ++j)
            if (Lower(out->sk.bones[j].name) == bn) { out->boneMap[i] = static_cast<int>(j); ++mapped; break; }
    }
    if (mapped < static_cast<int>(b3d.bones.size()) / 2) { log::Warn("rig %s: only %d/%zu bones matched", name.c_str(), mapped, b3d.bones.size()); return nullptr; }

    // animation prefix: HumMNor -> HumM, HumFWir -> HumF, CR_Dog -> CR_Dog
    if (name.size() >= 4 && Lower(name.substr(0, 3)) == "hum") out->animPrefix = name.substr(0, 4);
    else out->animPrefix = name;

    // parts: one per material group, vertices deduplicated per group
    const size_t nv = b3d.VertexCount();
    std::vector<int32_t> remap(nv);
    for (const auto& g : b3d.groups) {
        if (g.idx.empty()) continue;
        std::fill(remap.begin(), remap.end(), -1);
        SkinPartSrc sp;
        for (uint32_t v : g.idx) {
            if (remap[v] < 0) {
                remap[v] = static_cast<int32_t>(sp.pos.size() / 3);
                sp.pos.insert(sp.pos.end(), &b3d.pos[v * 3], &b3d.pos[v * 3] + 3);
                sp.nrm.insert(sp.nrm.end(), &b3d.nrm[v * 3], &b3d.nrm[v * 3] + 3);
                sp.uv0.insert(sp.uv0.end(), &b3d.uv0[v * 2], &b3d.uv0[v * 2] + 2);
                sp.col.insert(sp.col.end(), &b3d.col[v * 4], &b3d.col[v * 4] + 4);
                sp.bidx.insert(sp.bidx.end(), &b3d.boneIdx[v * 4], &b3d.boneIdx[v * 4] + 4);
                sp.bw.insert(sp.bw.end(), &b3d.boneW[v * 4], &b3d.boneW[v * 4] + 4);
            }
            sp.idx.push_back(static_cast<uint32_t>(remap[v]));
        }
        if (sp.pos.size() / 3 > 65535) continue;
        sp.flags = FlagsFromShader(g.shader, g.blend);
        sp.texName = g.texture;
        sp.skinTex = g.texture.size() >= 5 && Lower(g.texture.substr(0, 5)) == "skin_";
        if (!sp.flags.noTexture && !g.texture.empty()) sp.tex = assets_.Texture(g.texture);
        out->parts.push_back(std::move(sp));
    }
    out->height = b3d.bmax[1];
    out->ok = !out->parts.empty();
    return out->ok ? out : nullptr;
}

bool RigLibrary::LoadClip(SkinModel& m, const std::string& animName, Clip& out) {
    std::vector<uint8_t> buf;
    if (!assets_.data().Read(animName, gd::kResAnimGr2, buf)) return false;
    gr2::File f;
    gr2::Scene sc;
    std::string err;
    if (!f.load(buf.data(), buf.size(), &err) || !gr2::loadScene(f, sc, &err) || sc.animations.empty()) return false;
    const gr2::Animation& an = sc.animations[0];
    int best = -1, bestScore = 0;
    for (size_t g = 0; g < an.trackGroups.size(); ++g) {
        const std::string gn = Lower(an.trackGroups[g].name);
        if (gn == "light" || gn == "camera") continue;
        int score = 0;
        for (const auto& t : an.trackGroups[g].transformTracks)
            for (const auto& b : m.sk.bones)
                if (Lower(b.name) == Lower(t.name)) { ++score; break; }
        if (score > bestScore) { bestScore = score; best = static_cast<int>(g); }
    }
    if (best < 0) return false;
    out.tg = an.trackGroups[best];
    out.duration = std::max(0.05f, an.duration);
    out.valid = true;
    out.name = animName;
    return true;
}

const Clip* RigLibrary::Get(SkinModel& m, ClipKind k) {
    const int ki = static_cast<int>(k);
    if (!m.clipsTried[ki]) {
        m.clipsTried[ki] = true;
        const std::string& P = m.animPrefix;
        std::vector<std::string> cands;
        switch (k) {
            case ClipKind::Idle: cands = {P + "_Com_Una_Idle_01", P + "_Def_Una_Idle_01"}; break;
            case ClipKind::Walk: cands = {P + "_Com_Una_Walk_01", P + "_Com_Una_Walk_02"}; break;
            case ClipKind::Run: cands = {P + "_Com_Una_Run_01", P + "_Com_Una_Walk_01"}; break;
            case ClipKind::Attack: cands = {P + "_Com_Una_Punch_Jab_01", P + "_Com_Una_Attack1_01", P + "_Com_Una_Kick_Snap_01"}; break;
            case ClipKind::Hit: cands = {P + "_Com_Una_DmgFr_01"}; break;
            case ClipKind::Death: cands = {P + "_Com_Una_Laydown_ProneB_01", P + "_ProneB_Una_Death_01", P + "_Com_Una_Knock_ProneB_01"}; break;
            default: break;
        }
        for (const std::string& c : cands)
            if (LoadClip(m, c, m.clips[ki])) break;
        if (!m.clips[ki].valid) log::Warn("rig %s: no '%s' clip", m.name.c_str(), ClipKey(k));
    }
    return m.clips[ki].valid ? &m.clips[ki] : nullptr;
}

}  // namespace f3
