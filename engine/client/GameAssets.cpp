#include "GameAssets.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <rlgl.h>
#include "Shaders.h"
#include "common/log.h"
#include "gamedata/B3d.h"
#include "gamedata/Tga.h"

namespace f3 {

static constexpr int kMaxMaterialMaps = 12;  // raylib MAX_MATERIAL_MAPS

namespace {
std::string Upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
bool StartsWithNoCase(const std::string& s, const char* p) {
    size_t n = std::strlen(p);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(p[i]))) return false;
    return true;
}
uint32_t TypeToResType(const std::string& t) {
    if (t == "CRT") return gd::kResCrt;
    if (t == "CON") return gd::kResCon;
    if (t == "USE") return gd::kResUse;
    if (t == "DOR") return gd::kResDor;
    if (t == "ITM") return gd::kResItm;
    if (t == "WEA") return gd::kResWea;
    if (t == "ARM") return gd::kResArm;
    if (t == "AMO") return gd::kResAmo;
    return 0;
}
}  // namespace

MaterialFlags FlagsFromShader(const std::string& shader, const std::string& blend) {
    MaterialFlags f;
    std::string s = Upper(shader), b = Upper(blend);
    f.noTexture = s.rfind("NO_TEXTURE", 0) == 0;
    f.lightmap = s.find("_LM_") != std::string::npos;
    f.vertexAlpha = s.find("VERTEX_ALPHA") != std::string::npos;
    f.imageAlpha = s.find("IMAGE_ALPHA") != std::string::npos;
    f.additive = b == "ADD";
    f.blend = b == "ALPHABLEND" || f.vertexAlpha;
    return f;
}

GameAssets::~GameAssets() { Unload(); }

bool GameAssets::Open(const std::string& gameDir, std::string* err) { return data_.Open(gameDir, err); }

bool GameAssets::InitGpu() {
    if (gpu_) return true;
    shader_ = LoadShaderFromMemory(kVertexShader, kFragmentShader);
    if (shader_.id == 0 || shader_.id == rlGetShaderIdDefault()) { log::Error("asset shader failed to compile"); return false; }
    shader_.locs[SHADER_LOC_MATRIX_MODEL] = GetShaderLocation(shader_, "matModel");
    locs.useTex = GetShaderLocation(shader_, "uUseTex");
    locs.useLM = GetShaderLocation(shader_, "uUseLM");
    locs.alphaMode = GetShaderLocation(shader_, "uAlphaMode");
    locs.lit = GetShaderLocation(shader_, "uLit");
    locs.tint = GetShaderLocation(shader_, "uTint");
    locs.clipY = GetShaderLocation(shader_, "uClipY");
    locs.lmGain = GetShaderLocation(shader_, "uLmGain");
    mat_ = Material{};
    mat_.shader = shader_;
    mat_.maps = static_cast<MaterialMap*>(MemAlloc(kMaxMaterialMaps * sizeof(MaterialMap)));
    std::memset(mat_.maps, 0, kMaxMaterialMaps * sizeof(MaterialMap));
    for (int i = 0; i < kMaxMaterialMaps; ++i) mat_.maps[i].color = WHITE;
    gpu_ = true;
    return true;
}

void GameAssets::Unload() {
    for (auto& [k, m] : models_) if (m) for (auto& p : m->parts) UnloadMesh(p.mesh);
    models_.clear();
    for (auto& [k, t] : textures_) if (t && t->id) UnloadTexture(*t);
    textures_.clear();
    if (gpu_) { MemFree(mat_.maps); mat_ = Material{}; UnloadShader(shader_); gpu_ = false; }
}

const Texture2D* GameAssets::Texture(const std::string& name, const char* grpHint) {
    std::string key = gd::ToLower(name);
    if (grpHint) key += std::string("@") + gd::ToLower(grpHint);
    auto it = textures_.find(key);
    if (it != textures_.end()) return it->second && it->second->id ? it->second.get() : nullptr;
    auto tex = std::make_unique<Texture2D>();
    *tex = Texture2D{};
    std::vector<uint8_t> buf;
    gd::Image img;
    if (data_.Read(name, gd::kResTga, buf, grpHint) && gd::DecodeTga(buf.data(), buf.size(), img)) {
        Image ri{img.rgba.data(), img.w, img.h, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
        *tex = LoadTextureFromImage(ri);
        if (tex->id) {
            GenTextureMipmaps(tex.get());
            SetTextureFilter(*tex, TEXTURE_FILTER_TRILINEAR);
            SetTextureFilter(*tex, TEXTURE_FILTER_ANISOTROPIC_8X);
            SetTextureWrap(*tex, TEXTURE_WRAP_REPEAT);
        }
    } else {
        log::Warn("texture not found: %s", name.c_str());
    }
    const Texture2D* r = tex->id ? tex.get() : nullptr;
    textures_[key] = std::move(tex);
    return r;
}

Mesh GameAssets::BuildMesh(const std::vector<float>& pos, const std::vector<float>& nrm, const std::vector<float>& uv0,
                           const std::vector<float>& uv1, const std::vector<uint8_t>& rgba,
                           const std::vector<uint32_t>& idx) {
    Mesh m{};
    const size_t nv = pos.size() / 3;
    m.vertexCount = static_cast<int>(nv);
    m.triangleCount = static_cast<int>(idx.size() / 3);
    m.vertices = static_cast<float*>(MemAlloc(static_cast<unsigned>(nv * 3 * sizeof(float))));
    m.normals = static_cast<float*>(MemAlloc(static_cast<unsigned>(nv * 3 * sizeof(float))));
    m.texcoords = static_cast<float*>(MemAlloc(static_cast<unsigned>(nv * 2 * sizeof(float))));
    m.texcoords2 = static_cast<float*>(MemAlloc(static_cast<unsigned>(nv * 2 * sizeof(float))));
    m.colors = static_cast<unsigned char*>(MemAlloc(static_cast<unsigned>(nv * 4)));
    m.indices = static_cast<unsigned short*>(MemAlloc(static_cast<unsigned>(idx.size() * sizeof(unsigned short))));
    for (size_t i = 0; i < nv; ++i) {
        // left-handed (D3D) -> right-handed: negate z
        m.vertices[i * 3 + 0] = pos[i * 3 + 0];
        m.vertices[i * 3 + 1] = pos[i * 3 + 1];
        m.vertices[i * 3 + 2] = -pos[i * 3 + 2];
        m.normals[i * 3 + 0] = nrm.empty() ? 0 : nrm[i * 3 + 0];
        m.normals[i * 3 + 1] = nrm.empty() ? 1 : nrm[i * 3 + 1];
        m.normals[i * 3 + 2] = nrm.empty() ? 0 : -nrm[i * 3 + 2];
        m.texcoords[i * 2 + 0] = uv0.empty() ? 0 : uv0[i * 2 + 0];
        m.texcoords[i * 2 + 1] = uv0.empty() ? 0 : uv0[i * 2 + 1];
        m.texcoords2[i * 2 + 0] = uv1.empty() ? 0 : uv1[i * 2 + 0];
        m.texcoords2[i * 2 + 1] = uv1.empty() ? 0 : uv1[i * 2 + 1];
        for (int k = 0; k < 4; ++k) m.colors[i * 4 + k] = rgba.empty() ? 255 : rgba[i * 4 + k];
    }
    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
        // flip winding together with the handedness change
        m.indices[t + 0] = static_cast<unsigned short>(idx[t + 0]);
        m.indices[t + 1] = static_cast<unsigned short>(idx[t + 2]);
        m.indices[t + 2] = static_cast<unsigned short>(idx[t + 1]);
    }
    UploadMesh(&m, false);
    // CPU vertex copies are no longer needed once uploaded (UnloadMesh only frees what is non-null).
    MemFree(m.vertices); MemFree(m.normals); MemFree(m.texcoords); MemFree(m.texcoords2); MemFree(m.colors);
    m.vertices = nullptr; m.normals = nullptr; m.texcoords = nullptr; m.texcoords2 = nullptr; m.colors = nullptr;  // indices stay: DrawMesh picks the indexed path from the non-null pointer
    return m;
}

const ModelAsset* GameAssets::Model(const std::string& name) {
    std::string key = gd::ToLower(name);
    auto it = models_.find(key);
    if (it != models_.end()) return it->second.get();
    std::unique_ptr<ModelAsset> asset;
    std::vector<uint8_t> buf;
    gd::B3dModel b3d;
    std::string err;
    if (!data_.Read(name, gd::kResB3d, buf)) {
        log::Warn("model not found: %s", name.c_str());
    } else if (!gd::ParseB3d(buf.data(), buf.size(), b3d, &err)) {
        log::Warn("model %s: %s", name.c_str(), err.c_str());
    } else {
        asset = std::make_unique<ModelAsset>();
        const size_t nv = b3d.VertexCount();
        asset->bmin = {b3d.bmin[0], b3d.bmin[1], -b3d.bmax[2]};
        asset->bmax = {b3d.bmax[0], b3d.bmax[1], -b3d.bmin[2]};
        std::vector<int32_t> remap(nv);
        for (const auto& g : b3d.groups) {
            if (g.idx.empty()) continue;
            std::fill(remap.begin(), remap.end(), -1);
            std::vector<float> pos, nrm, uv0, uv1;
            std::vector<uint8_t> col;
            std::vector<uint32_t> idx;
            idx.reserve(g.idx.size());
            for (uint32_t v : g.idx) {
                if (remap[v] < 0) {
                    remap[v] = static_cast<int32_t>(pos.size() / 3);
                    pos.insert(pos.end(), &b3d.pos[v * 3], &b3d.pos[v * 3] + 3);
                    nrm.insert(nrm.end(), &b3d.nrm[v * 3], &b3d.nrm[v * 3] + 3);
                    uv0.insert(uv0.end(), &b3d.uv0[v * 2], &b3d.uv0[v * 2] + 2);
                    uv1.insert(uv1.end(), &b3d.uv1[v * 2], &b3d.uv1[v * 2] + 2);
                    col.insert(col.end(), &b3d.col[v * 4], &b3d.col[v * 4] + 4);
                }
                idx.push_back(static_cast<uint32_t>(remap[v]));
            }
            if (pos.size() / 3 > 65535) continue;
            DrawPart part;
            part.mesh = BuildMesh(pos, nrm, uv0, uv1, col, idx);
            part.flags = FlagsFromShader(g.shader, g.blend);
            part.texName = g.texture;
            part.skin = StartsWithNoCase(g.texture, "skin_");
            if (!part.flags.noTexture && !g.texture.empty()) part.tex = Texture(g.texture);
            asset->parts.push_back(std::move(part));
        }
    }
    const ModelAsset* r = asset.get();
    models_[key] = std::move(asset);
    return r;
}

const ModelAsset* GameAssets::EntityModel(const std::string& tmpl, const std::string& type, std::string* defaultSkin) {
    const std::string key = gd::ToLower(type + ":" + tmpl);
    auto it = templates_.find(key);
    if (it == templates_.end()) {
        std::pair<std::string, std::string> v;
        std::vector<uint8_t> buf;
        gd::EntityTemplate et;
        uint32_t rt = TypeToResType(type);
        if (rt && data_.Read(tmpl, rt, buf) && gd::ParseEntityTemplate(buf.data(), buf.size(), et)) v = {et.model, et.skin};
        it = templates_.emplace(key, std::move(v)).first;
    }
    if (defaultSkin) *defaultSkin = it->second.second;
    if (it->second.first.empty()) return nullptr;
    return Model(it->second.first);
}

}  // namespace f3
