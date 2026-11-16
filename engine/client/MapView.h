#pragma once
// A map of the original game loaded for rendering: baked tre geometry (batched per material / texture /
// lightmap), emap entity instances drawn as static-pose B3D models, and the mapdata walk grid.
//
// Space conventions (renderer space, right-handed, Y up): X = original X, Y = original Y, Z = -original Z.
// The walk grid has 2 cells per world unit and starts at the tre root bbox min (ox, oz): a cell (cx, cy) covers original x in
// [ox+cx/2, ox+cx/2+0.5), original z in [oz+cy/2, oz+cy/2+0.5), i.e. renderer z in (-(oz+(cy+1)/2), -(oz+cy/2)].
#include <memory>
#include <string>
#include <vector>
#include <raylib.h>
#include "GameAssets.h"
#include "CritterRig.h"
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
    // doors without a skeleton / open clip (Overseer door, generator room, ...): the slab sinks into the floor like the original
    // Open_In clip does, sink runs 0 (closed) .. 1 (fully below the floor, not drawn)
    float sink = 0, sinkH = 0;
    bool isDoor = false;
    // doors with a skeleton: CPU-skinned instance posed with "<model>_Open_In_01" (the door slab drops into the floor); doorT runs
    // 0 (closed) .. clip duration (open), moving towards the server state at the clip's own speed
    std::unique_ptr<SkinnedInstance> rig;
    const Clip* openClip = nullptr;
    float doorT = 0, posedT = -1;
    bool doorOpen = false;
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
    // Door entities. FindDoor = index of the DOR entity nearest to a renderer-space position (-1 = none within 2.5 units);
    // SetDoorState moves it towards open / closed (rigged doors play Open_In_01, others sink into the floor); instant = no animation
    // (a door that is already open when it comes into view).
    int FindDoor(const Vector3& worldPos) const;
    void SetDoorState(int door, bool open, bool instant);
    float DoorProgress(int door) const;   // 0 closed .. 1 open (animation position), for tests / logging
    void SetRigs(RigLibrary* rigs) { rigs_ = rigs; }   // optional, before Load: skeletal doors
    void Update(float dt);                              // door animation
    // Colour-modulate the baked map entity (door / container / usable) nearest to `worldPos` in the next Draw() only (object highlight).
    void SetEntityTint(const Vector3& worldPos, Color tint);

    void Draw(GameAssets& assets, const DrawOptions& opt);
    // Draws one model instance with the asset shader (used for network critters/items as well).
    static void DrawModel(GameAssets& assets, const ModelAsset& model, const Matrix& transform, Color tint,
                          const Texture2D* skin, const DrawOptions& opt, bool lit = true);

    // Ray against upward-facing level geometry. hit in renderer space.
    bool PickGround(const Ray& ray, Vector3& hit) const;

    // Walkable ground surface height under a renderer-space (x, z): a smoothed, continuous height field precomputed
    // from the up-facing tre triangles (the server map data has no height). 0 when the map has no geometry.
    float GroundHeight(float x, float z) const;
    bool hasHeightField() const { return hfW_ > 0; }

    // Grid (2 cells per unit). Cell centre in renderer space, y = 0.
    int gridW() const { return grid_.w; }
    int gridH() const { return grid_.h; }
    bool hasGrid() const { return grid_.w > 0; }
    bool Blocked(int cx, int cy) const { return grid_.Blocked(cx, cy); }
    Vector3 CellCenter(int cx, int cy, float y = 0) const { return {ox_ + (cx + 0.5f) * 0.5f, y, -(oz_ + (cy + 0.5f) * 0.5f)}; }
    // World position (original x, z) of the min corner of cell (0,0): the min corner of the tre root bbox.
    float gridOriginX() const { return ox_; }
    float gridOriginZ() const { return oz_; }
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

    float ox_ = 0, oz_ = 0;   // grid origin, original world units
    RigLibrary* rigs_ = nullptr;
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
    int tintIdx_ = -1;
    Color tintCol_ = WHITE;
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

    // ground height field (see GroundHeight)
    void BuildHeightField();
    std::vector<float> hf_;
    int hfW_ = 0, hfH_ = 0;
    float hfOx_ = 0, hfOz_ = 0;
    static constexpr float kHfStep = 0.25f;
};

}  // namespace f3
