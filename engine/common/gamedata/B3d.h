#pragma once
// "B3D 1.1 " mesh parser (see docs/formats/models.md). The file's single mesh is flattened into
// one vertex pool + material groups. Coordinates are left as stored: left-handed, Y up.
#include <cstdint>
#include <string>
#include <vector>

namespace f3::gd {

struct B3dGroup {
    std::string name, shader, blend, surface;
    std::string texture;           // diffuse TGA resource name (".tga" suffix, case-insensitive), may be empty
    std::vector<uint32_t> idx;     // triangle list into B3dModel vertex pool
};

struct B3dBone {
    std::string name;
    uint8_t flags = 0;
    float pos[3] = {}, axisAngle[4] = {};
};

struct B3dModel {
    std::vector<float> pos, nrm;   // 3 floats per vertex
    std::vector<float> uv0, uv1;   // 2 floats per vertex (uv1 = lightmap uv, preconverted layout only)
    std::vector<uint8_t> col;      // RGBA8 per vertex (0x7f = neutral, engine modulates x2)
    std::vector<B3dGroup> groups;
    std::vector<B3dBone> bones;
    // Skinning (critters): up to 4 influences per vertex; index = position in `bones`, weights sum to ~1.
    // Empty when the model has no skin data. See docs/formats/models.md ("bone weights").
    std::vector<uint8_t> boneIdx;  // 4 per vertex
    std::vector<float> boneW;      // 4 per vertex
    bool HasSkin() const { return !boneW.empty(); }
    std::vector<std::string> textures;  // top-level texture list (file names)
    float bmin[3] = {}, bmax[3] = {};
    bool preconverted = false;
    size_t VertexCount() const { return pos.size() / 3; }
};

// Returns false (and fills err) on any inconsistency, like modelinfo's strict parser.
bool ParseB3d(const uint8_t* data, size_t size, B3dModel& out, std::string* err = nullptr);

}  // namespace f3::gd
