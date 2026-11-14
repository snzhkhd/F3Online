#pragma once
// Skinned critters: b3d mesh (4 bone influences per vertex) + Granny skeleton (<model>.mesh.gr2) + animation clips
// (<prefix>_<clip>.anim.gr2). Skinning is done on the CPU into per-instance dynamic meshes (a human is ~2000 vertices).
//
// Units: the b3d vertices are 10x the Granny skeleton units (verified on HumMNor: thigh joint 0.333 vs thigh vertices up
// to 3.13); both are in the same left-handed Y-up space. Instances output renderer space (z negated) in b3d units, so the
// usual MapView::modelScale applies. See docs/client-rendering.md.
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <raylib.h>
#include "GameAssets.h"
#include "gr2/gr2.h"

namespace f3 {

enum class ClipKind { Idle, Walk, Run, Attack, Hit, Death, Count };

struct Clip {
    gr2::TrackGroup tg;
    float duration = 1.0f;
    bool valid = false;
    std::string name;
};

struct SkinPartSrc {
    std::vector<float> pos, nrm;       // bind pose, b3d space (left-handed), 3 per vertex
    std::vector<uint16_t> bidx;        // 4 per vertex, Granny skeleton bone indices (skeleton.size() = identity slot)
    std::vector<float> bw;             // 4 per vertex
    std::vector<float> uv0;
    std::vector<uint8_t> col;
    std::vector<uint32_t> idx;
    MaterialFlags flags;
    std::string texName;
    bool skinTex = false;
    const Texture2D* tex = nullptr;
};

// Everything that makes two critters of the same model look different: skin texture plus worn parts.
struct Appearance {
    std::string model;                                   // base b3d ("HumMNor")
    std::string skin;                                    // replacement for the body skin texture, may be empty
    std::vector<std::pair<std::string, std::string>> worn;  // (part, texture override), see WornPart
    std::string Key() const;
};

// Per base model, shared by all appearances: animation clips (same skeleton).
struct ClipSet {
    Clip clips[static_cast<int>(ClipKind::Count)];
    bool tried[static_cast<int>(ClipKind::Count)] = {};
};

// One skinned model for one appearance (shared between instances).
struct SkinModel {
    std::string name, animPrefix;
    bool ok = false;
    std::vector<SkinPartSrc> parts;
    gr2::Skeleton sk;
    std::vector<float> invWorld;        // 16 per skeleton bone
    ClipSet* clipSet = nullptr;
    Texture2D atlas{};                  // humans: the composed 512x512 critter texture (owned by RigLibrary)
    float height = 6.0f;                // b3d units
    float skelScale = 10.0f;            // b3d units per Granny skeleton unit: 10 for preconverted meshes (critters, JT doors), 1 otherwise
    std::unordered_map<std::string, Clip> named;   // extra clips by resource name (doors: "<model>_Open_In_01")
};

class SkinnedInstance {
public:
    SkinnedInstance(const SkinModel* m);
    ~SkinnedInstance();
    SkinnedInstance(const SkinnedInstance&) = delete;
    SkinnedInstance& operator=(const SkinnedInstance&) = delete;

    // Pose = blend of up to two clip samples. clipB may be null.
    void Pose(const Clip* a, float ta, const Clip* b, float tb, float blendToA);
    const ModelAsset& asset() const { return inst_; }
    const SkinModel* model() const { return model_; }

private:
    const SkinModel* model_;
    ModelAsset inst_;
    std::vector<float> palette_;  // 16 per skeleton bone + one identity matrix
};

class RigLibrary {
public:
    explicit RigLibrary(GameAssets& a);
    ~RigLibrary();
    // nullptr when the model has no skin data / skeleton.
    SkinModel* Model(const std::string& name) { return Model(Appearance{name, "", {}}); }
    SkinModel* Model(const Appearance& app);
    // Loads (once) the clip of the given kind for this model; returns nullptr if the game has none.
    const Clip* Get(SkinModel& m, ClipKind k);
    const Clip* GetNamed(SkinModel& m, const std::string& animName);   // nullptr if the game has no such clip
    static const char* ClipKey(ClipKind k);

private:
    struct Core;   // parsed base b3d + skeleton, shared by appearances of one model
    Core* GetCore(const std::string& name);
    bool LoadClip(SkinModel& m, const std::string& animName, Clip& out);
    GameAssets& assets_;
    std::unordered_map<std::string, std::unique_ptr<Core>> cores_;
    std::unordered_map<std::string, std::unique_ptr<SkinModel>> models_;
    std::unordered_map<std::string, std::unique_ptr<ClipSet>> clipSets_;
};

}  // namespace f3
