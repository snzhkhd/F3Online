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
    std::vector<uint8_t> bidx;         // 4 per vertex, b3d bone indices
    std::vector<float> bw;             // 4 per vertex
    std::vector<float> uv0;
    std::vector<uint8_t> col;
    std::vector<uint32_t> idx;
    MaterialFlags flags;
    std::string texName;
    bool skinTex = false;
    const Texture2D* tex = nullptr;
};

// One skinned model (shared between instances).
struct SkinModel {
    std::string name, animPrefix;
    bool ok = false;
    std::vector<SkinPartSrc> parts;
    gr2::Skeleton sk;
    std::vector<float> invWorld;        // 16 per skeleton bone
    std::vector<int> boneMap;           // b3d bone -> skeleton bone or -1
    Clip clips[static_cast<int>(ClipKind::Count)];
    bool clipsTried[static_cast<int>(ClipKind::Count)] = {};
    float height = 6.0f;                // b3d units
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
    std::vector<float> palette_;  // 16 per b3d bone
};

class RigLibrary {
public:
    explicit RigLibrary(GameAssets& a) : assets_(a) {}
    ~RigLibrary() = default;
    // nullptr when the model has no skin data / skeleton.
    SkinModel* Model(const std::string& name);
    // Loads (once) the clip of the given kind for this model; returns nullptr if the game has none.
    const Clip* Get(SkinModel& m, ClipKind k);
    static const char* ClipKey(ClipKind k);

private:
    bool LoadClip(SkinModel& m, const std::string& animName, Clip& out);
    GameAssets& assets_;
    std::unordered_map<std::string, std::unique_ptr<SkinModel>> models_;
};

}  // namespace f3
