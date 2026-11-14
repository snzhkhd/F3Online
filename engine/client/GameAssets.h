#pragma once
// raylib-side asset cache on top of the gamedata library: textures, B3D models, entity templates.
// Everything here is in the renderer's space: right-handed, Y up. The original data is left-handed
// (Direct3D), so z is negated and triangle winding flipped when meshes are built.
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <raylib.h>
#include "gamedata/GameData.h"
#include "gamedata/MapFiles.h"

namespace f3 {

// How a batch of triangles is shaded. Derived from the original shader technique / blend names.
struct MaterialFlags {
    bool noTexture = false;     // vertex colour only
    bool lightmap = false;      // diffuse * lightmap
    bool vertexAlpha = false;   // alpha from vertex colour (blended)
    bool imageAlpha = false;    // alpha from the texture
    bool additive = false;      // ADD blend state
    bool blend = false;         // ALPHABLEND
};
MaterialFlags FlagsFromShader(const std::string& shader, const std::string& blend);

struct DrawPart {
    Mesh mesh{};
    const Texture2D* tex = nullptr;       // diffuse, may be null
    const Texture2D* lightmap = nullptr;  // second texture, may be null
    MaterialFlags flags;
    bool skin = false;                    // texture name starts with "Skin_": replaceable by an entity skin override
    std::string texName;
};

struct ModelAsset {
    std::vector<DrawPart> parts;
    Vector3 bmin{0, 0, 0}, bmax{0, 0, 0};  // renderer space
};

class GameAssets {
public:
    ~GameAssets();
    bool Open(const std::string& gameDir, std::string* err = nullptr);
    bool IsOpen() const { return data_.IsOpen(); }
    gd::GameData& data() { return data_; }
    Shader& shader() { return shader_; }
    Material& material() { return mat_; }   // asset shader + two map slots (diffuse, lightmap)
    struct Locs { int useTex = -1, useLM = -1, alphaMode = -1, lit = -1, tint = -1, clipY = -1, lmGain = -1; } locs;
    bool InitGpu();   // after the window exists: compiles the shader
    void Unload();    // before CloseWindow

    // Texture by resource name (".tga" suffix optional). Returns nullptr when missing. Cached.
    const Texture2D* Texture(const std::string& name, const char* grpHint = nullptr);
    // B3D model by resource name. Returns nullptr when missing/unparsable. Cached (including failures).
    const ModelAsset* Model(const std::string& name);
    // Model for a map entity template ("Critters00_X" + "CRT"). `skin` may be set by the template.
    const ModelAsset* EntityModel(const std::string& tmpl, const std::string& type, std::string* defaultSkin = nullptr);

    size_t textureCount() const { return textures_.size(); }
    size_t modelCount() const { return models_.size(); }

    // Builds a GPU mesh from CPU arrays in original (left-handed) space: z is negated and winding flipped.
    static Mesh BuildMesh(const std::vector<float>& pos, const std::vector<float>& nrm, const std::vector<float>& uv0,
                          const std::vector<float>& uv1, const std::vector<uint8_t>& rgba,
                          const std::vector<uint32_t>& idx);

private:
    gd::GameData data_;
    Shader shader_{};
    Material mat_{};
    bool gpu_ = false;
    std::unordered_map<std::string, std::unique_ptr<Texture2D>> textures_;
    std::unordered_map<std::string, std::unique_ptr<ModelAsset>> models_;
    std::unordered_map<std::string, std::pair<std::string, std::string>> templates_;  // key -> model, skin
};

}  // namespace f3
