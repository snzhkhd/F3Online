#pragma once
// A map of the original game loaded for rendering: baked tre geometry (batched per material / texture /
// lightmap), emap entity instances drawn as static-pose B3D models, and the mapdata walk grid.
//
// Space conventions (renderer space, right-handed, Y up): X = original X, Y = original Y, Z = -original Z.
// The walk grid has 2 cells per world unit; a cell (cx, cy) covers original x in [cx/2, cx/2+0.5),
// original z in [cy/2, cy/2+0.5), i.e. renderer z in (-(cy+1)/2, -cy/2].
#include <memory>
#include <string>
#include <vector>
#include <raylib.h>
#include "GameAssets.h"
#include "ProtoDb.h"
#include "gamedata/MapFiles.h"

namespace f3 {

struct MapBatch {
    Mesh mesh{};
    const Texture2D* tex = nullptr;
    const Texture2D* lightmap = nullptr;
    MaterialFlags flags;
    int triangles = 0;
};

struct MapEntityInstance {
    const ModelAsset* model = nullptr;
    Matrix transform{};
    const Texture2D* skin = nullptr;
    gd::MapEntity src;
    bool hidden = false;   // e.g. an opened door
};

struct DrawOptions {
    float clipY = 1e9f;        // geometry above this height is cut away (roofs of interiors)
    bool entities = true;
    float lmGain = 2.0f;       // lightmap modulation (original "2X")
    bool wireframe = false;
};

class MapView {
public:
    ~MapView() { Unload(); }
    // Accepts a tre base name ("04_Tutorial_Vault") or an emap name ("00_04_Tutorial_Vault", optional ".map").
    bool Load(GameAssets& assets, const std::string& name, std::string* err = nullptr);
    void Unload();
    bool loaded() const { return loaded_; }
    const std::string& name() const { return name_; }

    // Network mode: critters and items are owned by the server and drawn by the renderer, so the emap instances of
    // moving critters and loose items are not baked into the scene (doors, containers, static props stay).
    // Must be called before Load.
    void SetNetworkMode(const ProtoDb* protos) { network_ = true; protos_ = protos; }
    // Hides / shows the door entity nearest to a renderer-space position (door models have no open pose).
    void SetDoorOpen(const Vector3& worldPos, bool open);

    void Draw(GameAssets& assets, const DrawOptions& opt);
    // Draws one model instance with the asset shader (used for network critters/items as well).
    static void DrawModel(GameAssets& assets, const ModelAsset& model, const Matrix& transform, Color tint,
                          const Texture2D* skin, const DrawOptions& opt, bool lit = true);

    // Ray against upward-facing level geometry. hit in renderer space.
    bool PickGround(const Ray& ray, Vector3& hit) const;

    // Grid (2 cells per unit). Cell centre in renderer space, y = 0.
    int gridW() const { return grid_.w; }
    int gridH() const { return grid_.h; }
    bool hasGrid() const { return grid_.w > 0; }
    bool Blocked(int cx, int cy) const { return grid_.Blocked(cx, cy); }
    Vector3 CellCenter(int cx, int cy, float y = 0) const { return {(cx + 0.5f) * 0.5f, y, -(cy + 0.5f) * 0.5f}; }
    // Conversion between a server tile and a world position: the map spans gridW/2 x gridH/2 units, a server
    // map of tw x th tiles divides that evenly (tw == gridW means the usual 0.5-unit tiles).
    Vector3 TileToWorld(float tx, float ty, int tw, int th, float y = 0) const;
    bool WorldToTile(const Vector3& w, int tw, int th, int& tx, int& ty) const;

    Vector3 boundsMin() const { return bmin_; }
    Vector3 boundsMax() const { return bmax_; }
    Vector3 startTarget() const { return start_; }
    float startYaw() const { return startYaw_; }
    const gd::Emap& emap() const { return emap_; }
    bool hasEmap() const { return hasEmap_; }

    // stats
    size_t triangleCount() const { return tris_; }
    size_t batchCount() const { return batches_.size(); }
    size_t entityCount() const { return entities_.size(); }
    size_t entityDrawn() const { return entitiesDrawn_; }
    int missingEntities() const { return missing_; }
    std::string info() const;

    static float modelScale;   // original model units -> world units (see docs/client-rendering.md)

private:
    bool BuildTre(GameAssets& assets, const gd::Tre& tre);
    void BuildEntities(GameAssets& assets);

    bool loaded_ = false;
    bool network_ = false;
    const ProtoDb* protos_ = nullptr;
    std::string name_, treName_;
    gd::Emap emap_;
    bool hasEmap_ = false;
    gd::MapGrid grid_;
    std::vector<MapBatch> batches_;
    std::vector<MapEntityInstance> entities_;
    size_t entitiesDrawn_ = 0;
    int missing_ = 0;
    size_t tris_ = 0;
    int badSubRecords_ = 0;
    Vector3 bmin_{0, 0, 0}, bmax_{0, 0, 0}, start_{0, 0, 0};
    float startYaw_ = 0;

    // floor picking: upward facing triangles bucketed on an XZ grid
    struct PickTri { float v[9]; };
    std::vector<PickTri> pickTris_;
    std::vector<std::vector<uint32_t>> pickCells_;
    int pickW_ = 0, pickH_ = 0;
    float pickOx_ = 0, pickOz_ = 0;
    static constexpr float kPickCell = 4.0f;
};

}  // namespace f3
