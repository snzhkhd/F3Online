#include <unordered_map>
#include "MapView.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdlib>
#include <cstring>
#include <raymath.h>
#include <set>
#include <rlgl.h>
#include "common/log.h"

namespace f3 {

float MapView::modelScale = 1.0f;

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
    ox_ = oz_ = 0;
    pickTris_.clear();
    pickCells_.clear();
    hf_.clear();
    layerOff_.clear();
    layerY_.clear();
    modalFloor_ = 0.0f;
    hfW_ = hfH_ = 0;
    loaded_ = false;
}

Vector3 MapView::TileToWorld(float tx, float ty, int tw, int th, float y) const {
    float cw = tw > 0 && grid_.w > 0 ? grid_.w * 0.5f / tw : 0.5f;
    float ch = th > 0 && grid_.h > 0 ? grid_.h * 0.5f / th : 0.5f;
    return {ox_ + (tx + 0.5f) * cw, y, -(oz_ + (ty + 0.5f) * ch)};
}

bool MapView::WorldToTile(const Vector3& w, int tw, int th, int& tx, int& ty) const {
    float cw = tw > 0 && grid_.w > 0 ? grid_.w * 0.5f / tw : 0.5f;
    float ch = th > 0 && grid_.h > 0 ? grid_.h * 0.5f / th : 0.5f;
    tx = static_cast<int>(std::floor((w.x - ox_) / cw));
    ty = static_cast<int>(std::floor((-w.z - oz_) / ch));
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
    // The mapdata grid starts at the min corner of the tre root bounding box (see docs/formats/maps.md section 2).
    ox_ = grid_.w > 0 ? tre.bmin[0] : 0.0f;
    oz_ = grid_.w > 0 ? tre.bmin[2] : 0.0f;
    if (grid_.w > 0 && (std::fabs((tre.bmax[0] - tre.bmin[0]) * 2 - grid_.w) > 0.01f || std::fabs((tre.bmax[2] - tre.bmin[2]) * 2 - grid_.h) > 0.01f))
        log::Warn("map %s: grid %dx%d does not match the tre extent %gx%g units (stale test map); grid origin is a guess", treName_.c_str(), grid_.w, grid_.h,
                  tre.bmax[0] - tre.bmin[0], tre.bmax[2] - tre.bmin[2]);
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
        bmin_.x = std::min(bmin_.x, ox_); bmax_.x = std::max(bmax_.x, ox_ + grid_.w * 0.5f);
        bmin_.z = std::min(bmin_.z, -(oz_ + grid_.h * 0.5f)); bmax_.z = std::max(bmax_.z, -oz_);
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

    BuildHeightField();

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
        const float sc = modelScale * m->unitScale;
        Matrix s = MatrixScale(sc, sc, sc);
        if (e.type == "DOR" && rigs_) {
            const std::string mn = assets.EntityModelName(e.tmpl, e.type);
            if (SkinModel* sm = rigs_->Model(mn)) {
                if (const Clip* c = rigs_->GetNamed(*sm, mn + "_Open_In_01")) {
                    inst.rig = std::make_unique<SkinnedInstance>(sm);
                    inst.openClip = c;
                    inst.rig->Pose(c, 0.0f, nullptr, 0.0f, 1.0f);
                    inst.posedT = 0;
                }
            }
        }
        inst.isDoor = e.type == "DOR";
        inst.sinkH = (m->bmax.y - m->bmin.y) * sc + 0.15f;
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
        for (size_t i = 0; i < entities_.size(); ++i) {
            const MapEntityInstance& e = entities_[i];
            if (e.sink >= 0.999f) continue;   // fully open skeleton-less door: below the floor
            const Matrix tr = e.sink > 0 ? MatrixMultiply(e.transform, MatrixTranslate(0, -e.sinkH * e.sink, 0)) : e.transform;
            DrawModel(assets, e.rig ? e.rig->asset() : *e.model, tr, static_cast<int>(i) == tintIdx_ ? tintCol_ : WHITE, e.skin, opt, true);
            ++entitiesDrawn_;
        }
    }
    tintIdx_ = -1;
    rlDisableDepthMask();
    drawBatches(true);
    rlEnableDepthMask();
}

void MapView::SetEntityTint(const Vector3& p, Color tint) {
    int best = -1;
    float bd = 2.5f;
    for (size_t i = 0; i < entities_.size(); ++i) {
        const MapEntityInstance& e = entities_[i];
        if (e.src.type != "DOR" && e.src.type != "CON" && e.src.type != "USE") continue;
        const float dx = e.src.pos[0] - p.x, dz = -e.src.pos[2] - p.z;
        const float d = std::sqrt(dx * dx + dz * dz);
        if (d < bd) { bd = d; best = static_cast<int>(i); }
    }
    tintIdx_ = best;
    tintCol_ = tint;
}

int MapView::FindDoor(const Vector3& p) const {
    int best = -1;
    float bd = 2.5f;
    for (size_t i = 0; i < entities_.size(); ++i) {
        const MapEntityInstance& e = entities_[i];
        if (!e.isDoor) continue;
        const float dx = e.src.pos[0] - p.x, dz = -e.src.pos[2] - p.z;
        const float d = std::sqrt(dx * dx + dz * dz);
        if (d < bd) { bd = d; best = static_cast<int>(i); }
    }
    return best;
}

void MapView::SetDoorState(int door, bool open, bool instant) {
    if (door < 0 || door >= static_cast<int>(entities_.size())) return;
    MapEntityInstance& e = entities_[static_cast<size_t>(door)];
    e.doorOpen = open;
    if (!instant) return;
    if (e.rig && e.openClip) {
        e.doorT = open ? e.openClip->duration : 0.0f;
        if (e.doorT != e.posedT) { e.rig->Pose(e.openClip, e.doorT, nullptr, 0.0f, 1.0f); e.posedT = e.doorT; }
    } else {
        e.sink = open ? 1.0f : 0.0f;
    }
}

float MapView::DoorProgress(int door) const {
    if (door < 0 || door >= static_cast<int>(entities_.size())) return 0;
    const MapEntityInstance& e = entities_[static_cast<size_t>(door)];
    if (e.rig && e.openClip && e.openClip->duration > 0) return e.doorT / e.openClip->duration;
    return e.sink;
}

void MapView::Update(float dt) {
    for (MapEntityInstance& e : entities_) {
        if (!e.isDoor) continue;
        if (!e.rig || !e.openClip) {   // no clip: the slab sinks into the floor in 0.9 s (like the Open_In clip of the rigged doors)
            const float goal = e.doorOpen ? 1.0f : 0.0f;
            if (e.sink != goal) e.sink = e.sink < goal ? std::min(goal, e.sink + dt / 0.9f) : std::max(goal, e.sink - dt / 0.9f);
            continue;
        }
        const float dur = e.openClip->duration;
        const float goal = e.doorOpen ? dur : 0.0f;
        if (e.doorT == goal) continue;
        e.doorT = e.doorT < goal ? std::min(goal, e.doorT + dt) : std::max(goal, e.doorT - dt);
        if (e.doorT != e.posedT) { e.rig->Pose(e.openClip, e.doorT, nullptr, 0.0f, 1.0f); e.posedT = e.doorT; }
    }
}

// Ground height field: every up-facing triangle is rasterised onto a regular XZ sample grid; per sample the lowest
// layer is the walkable floor (roof tops and upper storeys lie above it, ceilings face down and are not collected).
// Samples without geometry are filled from their neighbours, then the field is smoothed with a small blur so that the
// bilinear lookup is continuous and free of single-triangle bumps (rocks, props baked into the level).
void MapView::BuildHeightField() {
    hf_.clear();
    hfW_ = hfH_ = 0;
    if (pickTris_.empty()) return;
    hfOx_ = bmin_.x; hfOz_ = bmin_.z;
    hfW_ = static_cast<int>((bmax_.x - bmin_.x) / kHfStep) + 2;
    hfH_ = static_cast<int>((bmax_.z - bmin_.z) / kHfStep) + 2;
    const size_t n = static_cast<size_t>(hfW_) * hfH_;
    const float kNone = 1e30f;
    std::vector<float> lo(n, kNone);
    std::vector<float> hi(n, -kNone);
    std::vector<std::vector<float>> all(n);   // every floor layer per sample (see GroundHeight)
    for (const PickTri& t : pickTris_) {
        const float* v = t.v;
        const float x0 = std::min({v[0], v[3], v[6]}), x1 = std::max({v[0], v[3], v[6]});
        const float z0 = std::min({v[2], v[5], v[8]}), z1 = std::max({v[2], v[5], v[8]});
        const int i0 = std::max(0, static_cast<int>(std::ceil((x0 - hfOx_) / kHfStep)));
        const int i1 = std::min(hfW_ - 1, static_cast<int>(std::floor((x1 - hfOx_) / kHfStep)));
        const int j0 = std::max(0, static_cast<int>(std::ceil((z0 - hfOz_) / kHfStep)));
        const int j1 = std::min(hfH_ - 1, static_cast<int>(std::floor((z1 - hfOz_) / kHfStep)));
        const float det = (v[5] - v[8]) * (v[0] - v[6]) + (v[6] - v[3]) * (v[2] - v[8]);
        if (std::fabs(det) < 1e-9f) continue;
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i) {
                const float px = hfOx_ + i * kHfStep, pz = hfOz_ + j * kHfStep;
                const float l1 = ((v[5] - v[8]) * (px - v[6]) + (v[6] - v[3]) * (pz - v[8])) / det;
                const float l2 = ((v[8] - v[2]) * (px - v[6]) + (v[0] - v[6]) * (pz - v[8])) / det;
                const float l3 = 1.0f - l1 - l2;
                if (l1 < -1e-4f || l2 < -1e-4f || l3 < -1e-4f) continue;
                const float y = l1 * v[1] + l2 * v[4] + l3 * v[7];
                const size_t k = static_cast<size_t>(j) * hfW_ + i;
                lo[k] = std::min(lo[k], y);
                hi[k] = std::max(hi[k], y);
                all[k].push_back(y);
            }
    }
    // layer table (CSR): distinct heights per sample, ascending, 0.15 apart at least
    layerOff_.assign(n + 1, 0);
    layerY_.clear();
    for (size_t k = 0; k < n; ++k) {
        auto& v = all[k];
        std::sort(v.begin(), v.end());
        float last = -kNone;
        for (float y : v) if (y - last > 0.15f) { layerY_.push_back(y); last = y; }
        layerOff_[k + 1] = static_cast<uint32_t>(layerY_.size());
        std::vector<float>().swap(v);
    }
    {   // the most common floor height (half-unit bins): where a critter without history is assumed to stand
        std::unordered_map<int, size_t> hist;
        for (float y : layerY_) ++hist[static_cast<int>(std::floor(y * 2.0f))];
        size_t best = 0;
        for (auto& kv : hist) if (kv.second > best) { best = kv.second; modalFloor_ = (kv.first + 0.5f) * 0.5f; }
    }
    hf_.assign(n, 0.0f);
    std::vector<uint8_t> have(n, 0);
    size_t covered = 0, multi = 0;
    for (size_t k = 0; k < n; ++k)
        if (lo[k] < kNone) { hf_[k] = lo[k]; have[k] = 1; ++covered; if (hi[k] - lo[k] > 1.0f) ++multi; }
    // fill holes: repeated 4-neighbour propagation (bounded)
    for (int pass = 0; pass < 64; ++pass) {
        bool any = false;
        std::vector<float> nv = hf_;
        std::vector<uint8_t> nh = have;
        for (int j = 0; j < hfH_; ++j)
            for (int i = 0; i < hfW_; ++i) {
                const size_t k = static_cast<size_t>(j) * hfW_ + i;
                if (have[k]) continue;
                float s = 0; int c = 0;
                const int di[4] = {1, -1, 0, 0}, dj[4] = {0, 0, 1, -1};
                for (int d = 0; d < 4; ++d) {
                    const int ii = i + di[d], jj = j + dj[d];
                    if (ii < 0 || jj < 0 || ii >= hfW_ || jj >= hfH_) continue;
                    const size_t kk = static_cast<size_t>(jj) * hfW_ + ii;
                    if (have[kk]) { s += hf_[kk]; ++c; }
                }
                if (c) { nv[k] = s / c; nh[k] = 1; any = true; }
            }
        hf_.swap(nv); have.swap(nh);
        if (!any) break;
    }
    // thin walls have no floor under them: their top would be the lowest layer. A 9x9 median (2.25 units) removes every
    // feature narrower than about 4 samples while keeping real steps and slopes; flat areas are skipped for speed.
    std::vector<float> tmp(n);
    {
        tmp = hf_;
        const int R = 4;
        std::vector<float> win;
        win.reserve((2 * R + 1) * (2 * R + 1));
        auto at = [&](int i, int j) { return hf_[static_cast<size_t>(std::clamp(j, 0, hfH_ - 1)) * hfW_ + std::clamp(i, 0, hfW_ - 1)]; };
        for (int j = 0; j < hfH_; ++j)
            for (int i = 0; i < hfW_; ++i) {
                const float c = at(i, j);
                if (std::fabs(at(i - R, j) - c) < 0.05f && std::fabs(at(i + R, j) - c) < 0.05f && std::fabs(at(i, j - R) - c) < 0.05f &&
                    std::fabs(at(i, j + R) - c) < 0.05f && std::fabs(at(i - R, j - R) - c) < 0.05f && std::fabs(at(i + R, j + R) - c) < 0.05f)
                    continue;
                win.clear();
                for (int dj = -R; dj <= R; ++dj)
                    for (int di = -R; di <= R; ++di) win.push_back(at(i + di, j + dj));
                std::nth_element(win.begin(), win.begin() + win.size() / 2, win.end());
                tmp[static_cast<size_t>(j) * hfW_ + i] = win[win.size() / 2];
            }
        hf_ = tmp;
    }
    // smoothing: two passes of a separable [1 2 1] blur (about +-0.5 unit footprint)
    for (int pass = 0; pass < 2; ++pass) {
        for (int j = 0; j < hfH_; ++j)
            for (int i = 0; i < hfW_; ++i) {
                const float a = hf_[static_cast<size_t>(j) * hfW_ + std::max(0, i - 1)], b = hf_[static_cast<size_t>(j) * hfW_ + i];
                const float c = hf_[static_cast<size_t>(j) * hfW_ + std::min(hfW_ - 1, i + 1)];
                tmp[static_cast<size_t>(j) * hfW_ + i] = (a + 2 * b + c) * 0.25f;
            }
        for (int j = 0; j < hfH_; ++j)
            for (int i = 0; i < hfW_; ++i) {
                const float a = tmp[static_cast<size_t>(std::max(0, j - 1)) * hfW_ + i], b = tmp[static_cast<size_t>(j) * hfW_ + i];
                const float c = tmp[static_cast<size_t>(std::min(hfH_ - 1, j + 1)) * hfW_ + i];
                hf_[static_cast<size_t>(j) * hfW_ + i] = (a + 2 * b + c) * 0.25f;
            }
    }
    if (const char* dump = std::getenv("F3_DUMP_HF")) {   // debug: grey-scale PGM of the height field (-5..25 units)
        if (FILE* f = std::fopen(dump, "wb")) {
            std::fprintf(f, "P5 %d %d 255 ", hfW_, hfH_);
            for (int j = hfH_ - 1; j >= 0; --j)
                for (int i = 0; i < hfW_; ++i) std::fputc(static_cast<int>(std::clamp((hf_[static_cast<size_t>(j) * hfW_ + i] + 5.0f) * 8.5f, 0.0f, 255.0f)), f);
            std::fclose(f);
        }
    }
    log::Info("ground height field: %dx%d samples, %.1f%% covered, %.1f%% multi-layer", hfW_, hfH_, 100.0 * covered / n, 100.0 * multi / n);
}

float MapView::GroundHeight(float x, float z, float ref) const {
    if (hfW_ <= 0) return 0.0f;
    const float fx = std::clamp((x - hfOx_) / kHfStep, 0.0f, static_cast<float>(hfW_ - 1) - 0.001f);
    const float fz = std::clamp((z - hfOz_) / kHfStep, 0.0f, static_cast<float>(hfH_ - 1) - 0.001f);
    const int i = static_cast<int>(fx), j = static_cast<int>(fz);
    const float tx = fx - i, tz = fz - j;
    const float* r0 = &hf_[static_cast<size_t>(j) * hfW_ + i];
    const float* r1 = r0 + hfW_;
    if (std::isnan(ref) || layerOff_.empty()) {   // no reference height: the (smoothed) lowest layer
        const float a = r0[0] + (r0[1] - r0[0]) * tx, b = r1[0] + (r1[1] - r1[0]) * tx;
        return a + (b - a) * tz;
    }
    // Layer-aware lookup: the floor is the layer closest to the height the critter already stands at (within a step
    // tolerance). A grate or lift cab with deeper geometry below it (shaft bottom, base plane) therefore keeps its
    // critter on top; samples with no layer near `ref` (door gaps, voids) are filled from the other corners.
    const float kTol = 1.2f;
    float sum = 0.0f, wsum = 0.0f;
    for (int c = 0; c < 4; ++c) {
        const int ii = i + (c & 1), jj = j + (c >> 1);
        const size_t k = static_cast<size_t>(jj) * hfW_ + ii;
        const float w = ((c & 1) ? tx : 1.0f - tx) * ((c >> 1) ? tz : 1.0f - tz);
        if (w <= 1e-6f) continue;
        float val = hf_[k];
        bool ok = std::fabs(val - ref) <= kTol;
        if (!ok) {
            float bestD = kTol + 1e-3f;
            for (uint32_t q = layerOff_[k]; q < layerOff_[k + 1]; ++q) {
                const float d = std::fabs(layerY_[q] - ref);
                if (d < bestD) { bestD = d; val = layerY_[q]; ok = true; }
            }
        }
        if (ok) { sum += val * w; wsum += val == val ? w : 0.0f; }
    }
    return wsum > 1e-4f ? sum / wsum : ref;
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
