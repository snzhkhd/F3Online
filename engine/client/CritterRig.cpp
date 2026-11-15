#include "CritterRig.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include "common/log.h"
#include "gamedata/B3d.h"
#include "gamedata/Tga.h"

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
        case ClipKind::Equip: return "equip";
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
        dp.skin = false;
        dp.texName = sp.texName;
        inst_.parts.push_back(std::move(dp));
    }
    palette_.assign((m->sk.bones.size() + 1) * 16, 0.0f);
    float* idm = &palette_[m->sk.bones.size() * 16];
    idm[0] = idm[5] = idm[10] = idm[15] = 1.0f;
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
    world_ = world;
    const size_t nb = sk.bones.size();
    for (size_t j = 0; j < nb; ++j) Mul4(&m.invWorld[j * 16], &world[j * 16], &palette_[j * 16]);
    const float ss = m.skelScale;
    const float inv = 1.0f / ss;
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
            mesh.vertices[v * 3 + 0] = ox * ss;
            mesh.vertices[v * 3 + 1] = oy * ss;
            mesh.vertices[v * 3 + 2] = -oz * ss;
            mesh.normals[v * 3 + 0] = onx * nl;
            mesh.normals[v * 3 + 1] = ony * nl;
            mesh.normals[v * 3 + 2] = -onz * nl;
        }
        UpdateMeshBuffer(mesh, 0, mesh.vertices, static_cast<int>(nv * 3 * sizeof(float)), 0);
        UpdateMeshBuffer(mesh, 2, mesh.normals, static_cast<int>(nv * 3 * sizeof(float)), 0);
    }
}

bool SkinnedInstance::BoneWorld(const char* name, float* out16) const {
    const gr2::Skeleton& sk = model_->sk;
    for (size_t i = 0; i < sk.bones.size(); ++i)
        if (Lower(sk.bones[i].name) == Lower(name) && world_.size() >= (i + 1) * 16) {
            for (int k = 0; k < 16; ++k) out16[k] = world_[i * 16 + k];
            return true;
        }
    return false;
}

void SkinnedInstance::BonePositions(std::vector<Vector3>& out) const {
    out.clear();
    const float ss = model_->skelScale;
    for (size_t i = 0; (i + 1) * 16 <= world_.size(); ++i)
        out.push_back(Vector3{world_[i * 16 + 12] * ss, world_[i * 16 + 13] * ss, -world_[i * 16 + 14] * ss});
}

std::string Appearance::Key() const {
    std::string k = Lower(model) + "|" + Lower(skin);
    for (const auto& w : worn) k += "|" + Lower(w.first) + ":" + Lower(w.second);
    return k;
}

struct RigLibrary::Core {
    bool ok = false;
    std::string name, animPrefix;
    gd::B3dModel b3d;
    gr2::Skeleton sk;
    std::vector<float> invWorld;
    ClipSet* clips = nullptr;
};

namespace {

// Slot of each appearance part inside the 512x512 critter texture atlas (pixels: x, y, size). The b3d uv0 of every critter
// mesh already addresses the atlas: the body skin is the top-left 256x256 quadrant, outfits, hair, ... have their own slots.
struct Slot { int x, y, size; };
bool SlotOf(const std::string& part, Slot& s) {
    const std::string p = Lower(part.substr(0, 3));
    if (p == "skn") s = {0, 0, 256};
    else if (p == "bod") s = {0, 256, 256};
    else if (p == "sho") s = {256, 0, 128};
    else if (p == "hea") s = {384, 0, 128};
    else if (p == "han") s = {384, 128, 128};
    else if (p == "fee") s = {384, 256, 128};
    else if (p == "bac") s = {256, 256, 128};
    else if (p == "hai") s = {256, 128, 64};
    else if (p == "pon") s = {288, 224, 32};
    else if (p == "bea") s = {256, 192, 32};
    else if (p == "mus") s = {256, 224, 32};
    else if (p == "eye") s = {320, 192, 64};
    else if (p == "van") s = {320, 128, 64};
    else return false;
    return true;
}

void Blit(std::vector<uint8_t>& atlas, const gd::Image& im, const Slot& s) {
    if (im.w <= 0 || im.h <= 0) return;
    for (int y = 0; y < s.size; ++y)
        for (int x = 0; x < s.size; ++x) {
            // box sample of the source area covered by this destination pixel (downscale) or nearest (upscale)
            const int x0 = x * im.w / s.size, x1 = std::max(x0 + 1, (x + 1) * im.w / s.size);
            const int y0 = y * im.h / s.size, y1 = std::max(y0 + 1, (y + 1) * im.h / s.size);
            int acc[4] = {0, 0, 0, 0}, n = 0;
            for (int sy = y0; sy < y1 && sy < im.h; ++sy)
                for (int sx = x0; sx < x1 && sx < im.w; ++sx) {
                    const uint8_t* q = &im.rgba[(static_cast<size_t>(sy) * im.w + sx) * 4];
                    for (int c = 0; c < 4; ++c) acc[c] += q[c];
                    ++n;
                }
            uint8_t* d = &atlas[(static_cast<size_t>(s.y + y) * 512 + s.x + x) * 4];
            for (int c = 0; c < 4; ++c) d[c] = static_cast<uint8_t>(n ? acc[c] / n : 0);
            if (!im.hasAlpha) d[3] = 255;
        }
}

}  // namespace

RigLibrary::RigLibrary(GameAssets& a) : assets_(a) {}

RigLibrary::~RigLibrary() {
    for (auto& [k, m] : models_)
        if (m && m->atlas.id) UnloadTexture(m->atlas);
}

RigLibrary::Core* RigLibrary::GetCore(const std::string& name) {
    const std::string key = Lower(name);
    auto it = cores_.find(key);
    if (it != cores_.end()) return it->second->ok ? it->second.get() : nullptr;
    auto core = std::make_unique<Core>();
    Core* c = core.get();
    cores_[key] = std::move(core);
    c->name = name;
    gd::GameData& data = assets_.data();
    std::vector<uint8_t> buf, gb;
    std::string err;
    if (!data.Read(name, gd::kResB3d, buf) || !gd::ParseB3d(buf.data(), buf.size(), c->b3d, &err) || !c->b3d.HasSkin() || c->b3d.bones.empty())
        return nullptr;
    gr2::File f;
    gr2::Scene sc;
    if (!data.Read(name, gd::kResMeshGr2, gb) || !f.load(gb.data(), gb.size(), &err) || !gr2::loadScene(f, sc, &err) || sc.skeletons.empty()) {
        log::Warn("rig %s: skeleton: %s", name.c_str(), err.c_str());
        return nullptr;
    }
    c->sk = sc.skeletons[0];
    c->invWorld.resize(c->sk.bones.size() * 16);
    for (size_t i = 0; i < c->sk.bones.size(); ++i)
        std::memcpy(&c->invWorld[i * 16], c->sk.bones[i].inverseWorld, 16 * sizeof(float));
    int mapped = 0;
    for (const auto& bb : c->b3d.bones)
        for (const auto& sb : c->sk.bones)
            if (Lower(sb.name) == Lower(bb.name)) { ++mapped; break; }
    if (mapped < static_cast<int>(c->b3d.bones.size()) / 2) { log::Warn("rig %s: only %d/%zu bones matched", name.c_str(), mapped, c->b3d.bones.size()); return nullptr; }
    // animation prefix: HumMNor -> HumM, HumFWir -> HumF, CR_Dog -> CR_Dog
    if (name.size() >= 4 && Lower(name.substr(0, 3)) == "hum") c->animPrefix = name.substr(0, 4);
    else c->animPrefix = name;
    auto& cs = clipSets_[key];
    if (!cs) cs = std::make_unique<ClipSet>();
    c->clips = cs.get();
    c->ok = true;
    return c;
}

SkinModel* RigLibrary::Model(const Appearance& app) {
    const std::string key = app.Key();
    auto it = models_.find(key);
    if (it != models_.end()) return it->second->ok ? it->second.get() : nullptr;
    auto sm = std::make_unique<SkinModel>();
    sm->name = app.model;
    SkinModel* out = sm.get();
    models_[key] = std::move(sm);

    Core* core = GetCore(app.model);
    if (!core) return nullptr;
    gd::GameData& data = assets_.data();
    out->sk = core->sk;
    out->invWorld = core->invWorld;
    out->animPrefix = core->animPrefix;
    out->clipSet = core->clips;
    const bool human = Lower(app.model.substr(0, 3)) == "hum";

    // pieces: the base body plus every worn part that exists for this body type
    struct Piece { gd::B3dModel own; const gd::B3dModel* b3d = nullptr; std::string part, tex; };
    std::vector<std::unique_ptr<Piece>> pieces;
    {
        auto base = std::make_unique<Piece>();
        base->b3d = &core->b3d;
        base->part = "skn";
        pieces.push_back(std::move(base));
    }
    std::set<std::string> hidden;
    if (human) {
        for (const auto& w : app.worn) {
            std::vector<uint8_t> pb;
            auto pc = std::make_unique<Piece>();
            std::string err;
            if (!data.Read(app.model + "_" + w.first, gd::kResB3d, pb) || !gd::ParseB3d(pb.data(), pb.size(), pc->own, &err) || !pc->own.HasSkin()) {
                log::Warn("critter part %s_%s: %s", app.model.c_str(), w.first.c_str(), err.empty() ? "not found" : err.c_str());
                continue;
            }
            pc->b3d = &pc->own;
            pc->part = w.first;
            pc->tex = w.second;
            for (const auto& h : pc->own.hides) hidden.insert(Lower(h));
            pieces.push_back(std::move(pc));
        }
    }

    // texture atlas (humans)
    std::vector<uint8_t> atlas;
    if (human) {
        atlas.assign(512 * 512 * 4, 0);
        for (size_t i = 3; i < atlas.size(); i += 4) atlas[i] = 255;
    }
    auto loadImage = [&](const std::string& name, gd::Image& im) {
        std::vector<uint8_t> tb;
        return !name.empty() && data.Read(name, gd::kResTga, tb) && gd::DecodeTga(tb.data(), tb.size(), im);
    };
    bool skinDone = false;
    auto putSkin = [&](const std::string& fallback) {
        if (skinDone || !human) return;
        gd::Image im;
        if (loadImage(app.skin, im) || loadImage(fallback, im)) { Slot s; SlotOf("skn", s); Blit(atlas, im, s); skinDone = true; }
        else log::Warn("critter skin '%s' / '%s' not found", app.skin.c_str(), fallback.c_str());
    };

    const size_t nBones = out->sk.bones.size();
    for (const auto& pcs : pieces) {
        const gd::B3dModel& b3d = *pcs->b3d;
        const bool isBase = pcs->part == "skn";
        // b3d bone index -> skeleton bone
        std::vector<int> boneMap(b3d.bones.size(), -1);
        for (size_t i = 0; i < b3d.bones.size(); ++i) {
            const std::string bn = Lower(b3d.bones[i].name);
            for (size_t j = 0; j < nBones; ++j)
                if (Lower(out->sk.bones[j].name) == bn) { boneMap[i] = static_cast<int>(j); break; }
        }
        bool texPlaced = false;
        const size_t nv = b3d.VertexCount();
        std::vector<int32_t> remap(nv);
        for (const auto& g : b3d.groups) {
            if (g.idx.empty()) continue;
            if (isBase && hidden.count(Lower(g.name))) continue;
            const bool isSkinGroup = g.texture.size() >= 5 && Lower(g.texture.substr(0, 5)) == "skin_";
            if (human) {
                if (isSkinGroup) putSkin(g.texture);
                else if (!texPlaced) {
                    Slot s;
                    gd::Image im;
                    if (SlotOf(pcs->part, s) && (loadImage(pcs->tex, im) || loadImage(g.texture, im))) Blit(atlas, im, s);
                    else log::Warn("critter part %s: texture '%s' / '%s' not usable", pcs->part.c_str(), pcs->tex.c_str(), g.texture.c_str());
                    texPlaced = true;
                }
            }
            std::fill(remap.begin(), remap.end(), -1);
            SkinPartSrc sp;
            for (uint32_t v : g.idx) {
                if (remap[v] < 0) {
                    remap[v] = static_cast<int32_t>(sp.pos.size() / 3);
                    sp.pos.insert(sp.pos.end(), &b3d.pos[v * 3], &b3d.pos[v * 3] + 3);
                    sp.nrm.insert(sp.nrm.end(), &b3d.nrm[v * 3], &b3d.nrm[v * 3] + 3);
                    sp.uv0.insert(sp.uv0.end(), &b3d.uv0[v * 2], &b3d.uv0[v * 2] + 2);
                    sp.col.insert(sp.col.end(), &b3d.col[v * 4], &b3d.col[v * 4] + 4);
                    for (int k = 0; k < 4; ++k) {
                        const float w = b3d.boneW[v * 4 + k];
                        const int bi = boneMap[b3d.boneIdx[v * 4 + k]];
                        sp.bw.push_back(bi >= 0 ? w : 0.0f);
                        sp.bidx.push_back(static_cast<uint16_t>(bi >= 0 ? bi : static_cast<int>(nBones)));
                    }
                }
                sp.idx.push_back(static_cast<uint32_t>(remap[v]));
            }
            if (sp.pos.size() / 3 > 65535) continue;
            sp.flags = FlagsFromShader(g.shader, g.blend);
            sp.texName = g.texture;
            sp.skinTex = false;
            if (!human && !sp.flags.noTexture && !g.texture.empty()) sp.tex = assets_.Texture(g.texture);
            out->parts.push_back(std::move(sp));
        }
    }
    if (human) {
        putSkin(core->b3d.groups.empty() ? std::string() : core->b3d.groups[0].texture);
        Image img{atlas.data(), 512, 512, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
        out->atlas = LoadTextureFromImage(img);
        GenTextureMipmaps(&out->atlas);
        SetTextureFilter(out->atlas, TEXTURE_FILTER_TRILINEAR);
        SetTextureFilter(out->atlas, TEXTURE_FILTER_ANISOTROPIC_8X);
        SetTextureWrap(out->atlas, TEXTURE_WRAP_CLAMP);
        for (auto& p : out->parts) p.tex = &out->atlas;
    }
    out->height = core->b3d.bmax[1];
    out->skelScale = core->b3d.preconverted ? kSkelToModel : 1.0f;
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

const Clip* RigLibrary::GetNamed(SkinModel& m, const std::string& animName) {
    auto it = m.named.find(animName);
    if (it == m.named.end()) {
        Clip c;
        LoadClip(m, animName, c);
        it = m.named.emplace(animName, std::move(c)).first;
    }
    return it->second.valid ? &it->second : nullptr;
}

const Clip* RigLibrary::GetFor(SkinModel& m, ClipKind k, const WeaponAnim& w, bool combat) {
    const std::string& P = m.animPrefix;
    if (P.compare(0, 3, "Hum") != 0) return k == ClipKind::Equip ? nullptr : Get(m, k);
    for (const std::string& n : HumanClipNames(P, static_cast<int>(k), w, combat))
        if (const Clip* c = GetNamed(m, n)) return c;
    return k == ClipKind::Equip ? nullptr : Get(m, k);
}

const Clip* RigLibrary::Get(SkinModel& m, ClipKind k) {
    const int ki = static_cast<int>(k);
    ClipSet& cs = *m.clipSet;
    if (!cs.tried[ki]) {
        cs.tried[ki] = true;
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
        if (k == ClipKind::Equip) return nullptr;
        for (const std::string& c : cands)
            if (LoadClip(m, c, cs.clips[ki])) break;
        if (!cs.clips[ki].valid) log::Warn("rig %s: no '%s' clip", m.name.c_str(), ClipKey(k));
    }
    return cs.clips[ki].valid ? &cs.clips[ki] : nullptr;
}

}  // namespace f3
