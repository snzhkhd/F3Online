#include "MapView.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <raymath.h>
#include <set>
#include <rlgl.h>
#include "common/log.h"

namespace f3 {

float MapView::modelScale = 0.3f;

namespace {

std::string StripMapSuffix(std::string n) {
    if (n.size() > 4 && gd::ToLower(n.substr(n.size() - 4)) == ".map") n.resize(n.size() - 4);
    return n;
}

void SetPartState(GameAssets& a, const MaterialFlags& f, bool hasTex, bool hasLM, bool lit, Color tint,
                  const DrawOptions& opt) {
    Shader& sh = a.shader();
    const auto& L = a.locs;
    int useTex = hasTex && !f.noTexture ? 1 : 0;
    int useLM = hasLM && f.lightmap ? 1 : 0;
    int alphaMode = 0;
    if (f.imageAlpha && f.vertexAlpha) alphaMode = 3;
    else if (f.imageAlpha) alphaMode = 2;
    else if (f.vertexAlpha) alphaMode = 1;
    else if (f.blend && hasTex) alphaMode = 3;
    int litI = lit ? 1 : 0;
    float t[4] = {tint.r / 255.0f, tint.g / 255.0f, tint.b / 255.0f, tint.a / 255.0f};
    SetShaderValue(sh, L.useTex, &useTex, SHADER_UNIFORM_INT);
    SetShaderValue(sh, L.useLM, &useLM, SHADER_UNIFORM_INT);
    SetShaderValue(sh, L.alphaMode, &alphaMode, SHADER_UNIFORM_INT);
    SetShaderValue(sh, L.lit, &litI, SHADER_UNIFORM_INT);
    SetShaderValue(sh, L.tint, t, SHADER_UNIFORM_VEC4);
    SetShaderValue(sh, L.clipY, &opt.clipY, SHADER_UNIFORM_FLOAT);
    SetShaderValue(sh, L.lmGain, &opt.lmGain, SHADER_UNIFORM_FLOAT);
}

}  // namespace

void MapView::Unload() {
    for (auto& b : batches_) UnloadMesh(b.mesh);
    batches_.clear();
    entities_.clear();
    pickTris_.clear();
    pickCells_.clear();
    loaded_ = false;
}

Vector3 MapView::TileToWorld(float tx, float ty, int tw, int th, float y) const {
    float cw = tw > 0 && grid_.w > 0 ? grid_.w * 0.5f / tw : 0.5f;
    float ch = th > 0 && grid_.h > 0 ? grid_.h * 0.5f / th : 0.5f;
    return {(tx + 0.5f) * cw, y, -(ty + 0.5f) * ch};
}

bool MapView::WorldToTile(const Vector3& w, int tw, int th, int& tx, int& ty) const {
    float cw = tw > 0 && grid_.w > 0 ? grid_.w * 0.5f / tw : 0.5f;
    float ch = th > 0 && grid_.h > 0 ? grid_.h * 0.5f / th : 0.5f;
    tx = static_cast<int>(std::floor(w.x / cw));
    ty = static_cast<int>(std::floor(-w.z / ch));
    return tx >= 0 && ty >= 0 && (tw <= 0 || tx < tw) && (th <= 0 || ty < th);
}

bool MapView::Load(GameAssets& assets, const std::string& nameIn, std::string* err) {
    Unload();
    auto fail = [&](const std::string& m) { if (err) *err = m; return false; };
    gd::GameData& gd = assets.data();
    std::string name = StripMapSuffix(nameIn);
    std::vector<uint8_t> buf;
    hasEmap_ = false;
    emap_ = gd::Emap{};

    // Resolve emap (dynamic content) and tre (geometry) by either name.
    if (const gd::ResourceInfo* r = gd.Find(name, gd::kResEmap)) {
        if (gd.Read(*r, buf) && gd::ParseEmap(buf.data(), buf.size(), emap_)) { hasEmap_ = true; treName_ = emap_.treName; }
    }
    if (!hasEmap_) {
        treName_ = name;
        if (!gd.Find(treName_, gd::kResTre)) return fail("no such map: " + nameIn);
        for (const gd::ResourceInfo* r : gd.List(gd::kResEmap)) {
            gd::Emap e;
            if (gd.Read(*r, buf) && gd::ParseEmap(buf.data(), buf.size(), e) && gd::ToLower(e.treName) == gd::ToLower(treName_)) {
                emap_ = std::move(e); hasEmap_ = true; break;
            }
        }
    }
    const gd::ResourceInfo* treRes = gd.Find(treName_, gd::kResTre);
    if (!treRes) return fail("map has no geometry (.tre): " + treName_);
    name_ = treRes->name;

    if (const gd::ResourceInfo* r = gd.Find(treName_, gd::kResMapData)) {
        std::string e;
        if (!gd.Read(*r, buf) || !gd::ParseMapData(buf.data(), buf.size(), grid_, &e)) log::Warn("mapdata %s: %s", treName_.c_str(), e.c_str());
    } else grid_ = gd::MapGrid{};

    gd::Tre tre;
    std::string e;
    if (!gd.Read(*treRes, buf) || !gd::ParseTre(buf.data(), buf.size(), tre, &e)) return fail("tre: " + e);
    badSubRecords_ = tre.badSubRecords;
    if (!BuildTre(assets, tre)) return fail("no renderable geometry in " + treName_);
    BuildEntities(assets);

    // Start position: first party slot of the first entry point, else the centre of the level.
    start_ = {(tre.bmin[0] + tre.bmax[0]) * 0.5f, 0, -(tre.bmin[2] + tre.bmax[2]) * 0.5f};
    startYaw_ = 0;
    if (hasEmap_ && !emap_.entryPoints.empty() && !emap_.entryPoints[0].pos.empty()) {
        const auto& p = emap_.entryPoints[0].pos[0];
        start_ = {p[0], p[1], -p[2]};
        startYaw_ = emap_.entryPoints[0].yaw.empty() ? 0 : emap_.entryPoints[0].yaw[0];
    }
    loaded_ = true;
    return true;
}

bool MapView::BuildTre(GameAssets& assets, const gd::Tre& tre) {
    // bounds in renderer space
    bmin_ = {tre.bmin[0], tre.bmin[1], -tre.bmax[2]};
    bmax_ = {tre.bmax[0], tre.bmax[1], -tre.bmin[2]};
    if (grid_.w > 0) {
        bmin_.x = std::min(bmin_.x, 0.0f); bmax_.x = std::max(bmax_.x, grid_.w * 0.5f);
        bmin_.z = std::min(bmin_.z, -grid_.h * 0.5f); bmax_.z = std::max(bmax_.z, 0.0f);
    }

    std::vector<int32_t> remap;
    tris_ = 0;
    for (const gd::TreBatch& tb : tre.batches) {
        if (tb.idx.empty()) continue;
        const gd::TreVertexBuffer& vb = tre.vbs[tb.vb];
        remap.assign(vb.count, -1);
        std::vector<float> pos, nrm, uv0, uv1;
        std::vector<uint8_t> col;
        std::vector<uint32_t> idx;
        idx.reserve(tb.idx.size());
        for (uint16_t v : tb.idx) {
            if (remap[v] < 0) {
                remap[v] = static_cast<int32_t>(pos.size() / 3);
                pos.insert(pos.end(), &vb.xyz[v * 3], &vb.xyz[v * 3] + 3);
                if (!vb.nrm.empty()) nrm.insert(nrm.end(), &vb.nrm[v * 3], &vb.nrm[v * 3] + 3);
                if (!vb.txuv.empty()) uv0.insert(uv0.end(), &vb.txuv[v * 2], &vb.txuv[v * 2] + 2);
                if (!vb.lmuv.empty()) uv1.insert(uv1.end(), &vb.lmuv[v * 2], &vb.lmuv[v * 2] + 2);
                if (!vb.rgba.empty()) col.insert(col.end(), &vb.rgba[v * 4], &vb.rgba[v * 4] + 4);
            }
            idx.push_back(static_cast<uint32_t>(remap[v]));
        }
        MapBatch b;
        b.mesh = GameAssets::BuildMesh(pos, nrm, uv0, uv1, col, idx);
        b.triangles = static_cast<int>(idx.size() / 3);
        std::string shader = tb.material >= 0 && tb.material < static_cast<int>(tre.materials.size()) ? tre.materials[tb.material] : "BASE_2X";
        b.flags = FlagsFromShader(shader, "");
        if (tb.texture >= 0 && tb.texture < static_cast<int>(tre.textures.size())) b.tex = assets.Texture(tre.textures[tb.texture]);
        if (tb.lightmap >= 0 && tb.lightmap < static_cast<int>(tre.lightmaps.size())) b.lightmap = assets.Texture(tre.lightmaps[tb.lightmap], "Maps");
        tris_ += b.triangles;

        // collect up-facing triangles for ground picking
        for (size_t t = 0; t + 2 < idx.size(); t += 3) {
            float ny = 0;
            for (int k = 0; k < 3; ++k) ny += nrm.empty() ? 1.0f : nrm[idx[t + k] * 3 + 1];
            if (ny / 3 < 0.5f) continue;
            PickTri pt;
            for (int k = 0; k < 3; ++k) {
                uint32_t i = idx[t + k];
                pt.v[k * 3 + 0] = pos[i * 3 + 0]; pt.v[k * 3 + 1] = pos[i * 3 + 1]; pt.v[k * 3 + 2] = -pos[i * 3 + 2];
            }
            pickTris_.push_back(pt);
        }
        batches_.push_back(std::move(b));
    }
    // blended batches last
    std::stable_sort(batches_.begin(), batches_.end(), [](const MapBatch& a, const MapBatch& b) { return a.flags.blend < b.flags.blend; });

    // pick grid
    if (!pickTris_.empty()) {
        pickOx_ = bmin_.x; pickOz_ = bmin_.z;
        pickW_ = static_cast<int>((bmax_.x - bmin_.x) / kPickCell) + 2;
        pickH_ = static_cast<int>((bmax_.z - bmin_.z) / kPickCell) + 2;
        pickCells_.assign(static_cast<size_t>(pickW_) * pickH_, {});
        for (uint32_t i = 0; i < pickTris_.size(); ++i) {
            const float* v = pickTris_[i].v;
            float x0 = std::min({v[0], v[3], v[6]}), x1 = std::max({v[0], v[3], v[6]});
            float z0 = std::min({v[2], v[5], v[8]}), z1 = std::max({v[2], v[5], v[8]});
            int cx0 = std::clamp(static_cast<int>((x0 - pickOx_) / kPickCell), 0, pickW_ - 1);
            int cx1 = std::clamp(static_cast<int>((x1 - pickOx_) / kPickCell), 0, pickW_ - 1);
            int cz0 = std::clamp(static_cast<int>((z0 - pickOz_) / kPickCell), 0, pickH_ - 1);
            int cz1 = std::clamp(static_cast<int>((z1 - pickOz_) / kPickCell), 0, pickH_ - 1);
            for (int cz = cz0; cz <= cz1; ++cz)
                for (int cx = cx0; cx <= cx1; ++cx) pickCells_[static_cast<size_t>(cz) * pickW_ + cx].push_back(i);
        }
    }
    return !batches_.empty();
}

void MapView::BuildEntities(GameAssets& assets) {
    entities_.clear();
    missing_ = 0;
    if (!hasEmap_) return;
    std::set<std::string> reported;
    for (const gd::MapEntity& e : emap_.entities) {
        if (network_) {
            const ProtoInfo* pi = protos_ ? protos_->ByKey(e.tmpl + "." + e.type) : nullptr;
            bool dynamic;
            if (pi) dynamic = (pi->isCritter() && !pi->isStatic) || pi->cls == "weapon" || pi->cls == "armor" || pi->cls == "ammo" || pi->cls == "item";
            else dynamic = e.type == "CRT" || e.type == "WEA" || e.type == "ARM" || e.type == "AMO" || e.type == "ITM";
            if (dynamic) continue;
        }
        std::string defSkin;
        const ModelAsset* m = assets.EntityModel(e.tmpl, e.type, &defSkin);
        if (!m) {
            ++missing_;
            if (reported.insert(e.name).second) log::Warn("entity %s: no renderable model", e.name.c_str());
            continue;
        }
        MapEntityInstance inst;
        inst.model = m;
        inst.src = e;
        const std::string& skin = e.skin.empty() ? defSkin : e.skin;
        if (!skin.empty()) inst.skin = assets.Texture(skin);
        Matrix s = MatrixScale(modelScale, modelScale, modelScale);
        Matrix r = MatrixRotateY(-e.yaw);
        Matrix t = MatrixTranslate(e.pos[0], e.pos[1], -e.pos[2]);
        inst.transform = MatrixMultiply(MatrixMultiply(s, r), t);
        entities_.push_back(std::move(inst));
    }
}

void MapView::DrawModel(GameAssets& assets, const ModelAsset& model, const Matrix& transform, Color tint,
                        const Texture2D* skin, const DrawOptions& opt, bool lit) {
    Material& mat = assets.material();
    for (const DrawPart& p : model.parts) {
        const Texture2D* tex = (p.skin && skin) ? skin : p.tex;
        mat.maps[MATERIAL_MAP_ALBEDO].texture = tex ? *tex : Texture2D{};
        mat.maps[MATERIAL_MAP_METALNESS].texture = Texture2D{};
        SetPartState(assets, p.flags, tex != nullptr, false, lit, tint, opt);
        DrawMesh(p.mesh, mat, transform);
    }
}

void MapView::Draw(GameAssets& assets, const DrawOptions& opt) {
    if (!loaded_) return;
    Material& mat = assets.material();
    auto drawBatches = [&](bool blended) {
        for (const MapBatch& b : batches_) {
            if (b.flags.blend != blended) continue;
            mat.maps[MATERIAL_MAP_ALBEDO].texture = b.tex ? *b.tex : Texture2D{};
            mat.maps[MATERIAL_MAP_METALNESS].texture = b.lightmap ? *b.lightmap : Texture2D{};
            SetPartState(assets, b.flags, b.tex != nullptr, b.lightmap != nullptr, false, WHITE, opt);
            DrawMesh(b.mesh, mat, MatrixIdentity());
        }
    };
    drawBatches(false);
    entitiesDrawn_ = 0;
    if (opt.entities) {
        for (const MapEntityInstance& e : entities_) {
            if (e.hidden) continue;
            DrawModel(assets, *e.model, e.transform, WHITE, e.skin, opt, true);
            ++entitiesDrawn_;
        }
    }
    rlDisableDepthMask();
    drawBatches(true);
    rlEnableDepthMask();
}

void MapView::SetDoorOpen(const Vector3& p, bool open) {
    MapEntityInstance* best = nullptr;
    float bd = 2.5f;
    for (MapEntityInstance& e : entities_) {
        if (e.src.type != "DOR") continue;
        const float dx = e.src.pos[0] - p.x, dz = -e.src.pos[2] - p.z;
        const float d = std::sqrt(dx * dx + dz * dz);
        if (d < bd) { bd = d; best = &e; }
    }
    if (best) best->hidden = open;
}

bool MapView::PickGround(const Ray& ray, Vector3& hit) const {
    if (pickCells_.empty()) return false;
    float bestT = 1e30f;
    int lastCx = -1000, lastCz = -1000;
    const float step = kPickCell * 0.45f;
    float leaveT = 1e30f;
    for (float t = 0; t < 4000.0f && t < leaveT; t += step) {
        Vector3 p = Vector3Add(ray.position, Vector3Scale(ray.direction, t));
        int cx = static_cast<int>(std::floor((p.x - pickOx_) / kPickCell));
        int cz = static_cast<int>(std::floor((p.z - pickOz_) / kPickCell));
        if (cx < 0 || cz < 0 || cx >= pickW_ || cz >= pickH_) continue;
        if (cx == lastCx && cz == lastCz) continue;
        lastCx = cx; lastCz = cz;
        for (uint32_t id : pickCells_[static_cast<size_t>(cz) * pickW_ + cx]) {
            const float* v = pickTris_[id].v;
            Vector3 a{v[0], v[1], v[2]}, b{v[3], v[4], v[5]}, c{v[6], v[7], v[8]};
            RayCollision rc = GetRayCollisionTriangle(ray, a, b, c);
            if (rc.hit && rc.distance < bestT) bestT = rc.distance;
        }
        if (bestT < 1e29f) leaveT = bestT + kPickCell * 1.5f;
    }
    if (bestT > 1e29f) return false;
    hit = Vector3Add(ray.position, Vector3Scale(ray.direction, bestT));
    return true;
}

std::string MapView::info() const {
    char b[256];
    std::snprintf(b, sizeof b, "%s: %zu tris, %zu batches, %zu/%zu entities%s, grid %dx%d", name_.c_str(), tris_,
                  batches_.size(), entities_.size(), emap_.entities.size(), missing_ ? " (some without models)" : "", grid_.w, grid_.h);
    return b;
}

}  // namespace f3
