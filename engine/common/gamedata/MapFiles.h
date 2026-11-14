#pragma once
// Parsers for the per-map files of the original game (docs/formats/maps.md):
//   .emap     entity instances, entry points, regions (dynamic content)
//   .mapdata  RLE collision / surface grid (2 cells per world unit)
//   .tre      baked static level: lights, textures, SoA vertex buffers, per-material/lightmap index batches
//   .een      entity templates (only the model + skin are needed by the renderer)
// World space is left-handed, Y up (x/z ground plane); conversion to the renderer's space is the caller's job.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace f3::gd {

struct MapEntity {
    std::string name;        // "<template>.<TYPE>"
    std::string tmpl, type;  // "Critters00_TMCpl_Armstrong", "CRT"
    std::string instance;    // EEOV instance name
    std::string skin;        // EEOV texture override (may be empty)
    float pos[3] = {};       // world x, y(up), z
    float yaw = 0;           // radians about Y
    uint32_t team = 0;
    std::vector<std::string> inventory;
};

struct EntryPoint {
    uint8_t id = 0;
    std::vector<std::array<float, 3>> pos;
    std::vector<float> yaw;
};

struct Emap {
    std::string treName;     // without the legacy ".8"
    std::string automapName;
    std::vector<MapEntity> entities;
    std::vector<EntryPoint> entryPoints;
    float cam[4] = {};
};
bool ParseEmap(const uint8_t* data, size_t size, Emap& out, std::string* err = nullptr);

struct MapGrid {
    int w = 0, h = 0;                 // cells, 2 per world unit
    std::vector<uint8_t> cells;       // row-major, row = world z * 2
    static constexpr uint8_t kBlocked = 0x2f;
    bool Blocked(int x, int y) const {
        return x < 0 || y < 0 || x >= w || y >= h || cells[static_cast<size_t>(y) * w + x] == kBlocked;
    }
};
bool ParseMapData(const uint8_t* data, size_t size, MapGrid& out, std::string* err = nullptr);

struct EntityTemplate {
    std::string model;       // b3d resource name
    std::string skin;        // default skin texture (EEOV)
};
bool ParseEntityTemplate(const uint8_t* data, size_t size, EntityTemplate& out, std::string* err = nullptr);

struct TreVertexBuffer {
    uint32_t count = 0;
    std::vector<float> xyz, nrm, txuv, lmuv;  // 3,3,2,2 floats per vertex
    std::vector<uint8_t> rgba;                // 4 per vertex (D3DCOLOR converted; 0x7f neutral, x2 modulate)
};

struct TreLight {
    uint32_t type = 0;       // 0 directional, 2 point
    float color[3] = {1, 1, 1}, pos[3] = {}, dir[3] = {};
};

// One draw batch: triangles sharing vertex buffer, material, diffuse texture and lightmap page.
struct TreBatch {
    int vb = 0;
    int material = 0;        // index into Tre::materials (shader technique names)
    int texture = -1;        // index into Tre::textures, -1 = none
    int lightmap = -1;       // index into Tre::lightmaps, -1 = none
    std::vector<uint16_t> idx;
};

struct Tre {
    int gridW = 0, gridH = 0;
    float bmin[3] = {}, bmax[3] = {};
    std::vector<TreLight> lights;
    std::vector<std::string> materials;   // shader technique per material id, e.g. "SK_BASE_LM_2X"
    std::vector<std::string> textures;    // diffuse TGA names (as stored, ".tga")
    std::vector<std::string> lightmaps;   // lightmap pages ("<Map>_<n>.ctx" -> Maps/<Map>_<n>.tga)
    std::vector<TreVertexBuffer> vbs;
    std::vector<TreBatch> batches;
    int badSubRecords = 0;                // index records that did not decode (skipped)
    size_t TriangleCount() const { size_t n = 0; for (auto& b : batches) n += b.idx.size() / 3; return n; }
};
bool ParseTre(const uint8_t* data, size_t size, Tre& out, std::string* err = nullptr);

}  // namespace f3::gd
