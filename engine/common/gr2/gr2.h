// gr2lib: dependency-free reader for Granny 2.x (.gr2, file format version 6, little-endian 32-bit) files as shipped
// with the Van Buren tech demo. See docs/formats/granny.md.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "oodle0.h"

namespace gr2 {

// ---- container -------------------------------------------------------------------------------------------
struct SectionInfo {
    uint32_t compression = 0;  // 0 none, 1 Oodle0, 2 Oodle1
    uint32_t dataOffset = 0, compressedSize = 0, decompressedSize = 0, alignment = 0;
    uint32_t first16bit = 0, first8bit = 0;
    uint32_t relocOffset = 0, relocCount = 0, marshallOffset = 0, marshallCount = 0;
    uint32_t base = 0;  // offset of the section's decompressed data in File::mem
};

// Member types of the Granny type system (granny_member_type).
enum MemberType : int32_t {
    kEnd = 0, kInline, kReference, kReferenceToArray, kArrayOfReferences, kVariantReference, kUnsupported,
    kReferenceToVariantArray, kString, kTransform, kReal32, kInt8, kUInt8, kBinormalInt8, kNormalUInt8, kInt16,
    kUInt16, kBinormalInt16, kNormalUInt16, kInt32, kUInt32, kReal16, kEmptyReference
};

// A loaded file. All section data is decompressed into one flat buffer `mem` (offset 0 is reserved as null) and every
// relocated pointer is rewritten to an offset into `mem`, i.e. the file behaves like a 32-bit address space.
class File {
public:
    bool load(const uint8_t* data, size_t size, std::string* err = nullptr);
    bool loadFile(const std::string& path, std::string* err = nullptr);

    uint32_t version = 0, fileSize = 0, crc = 0, tag = 0;
    std::vector<SectionInfo> sections;
    uint32_t rootType = 0, rootObject = 0;  // offsets into mem
    std::vector<uint8_t> mem;

    uint32_t u32(uint32_t off) const;
    int32_t i32(uint32_t off) const { return (int32_t)u32(off); }
    float f32(uint32_t off) const;
    uint16_t u16(uint32_t off) const;
    uint8_t u8(uint32_t off) const;
    std::string cstr(uint32_t off) const;  // NUL-terminated string at off ("" for 0 / out of range)
    bool valid(uint32_t off, uint32_t len = 1) const { return off != 0 && (uint64_t)off + len <= mem.size(); }

    // Decompress section idx from the original file bytes, without relocation (what granny2.dll's
    // GrannyDecompressData produces); used for verification.
    bool decompressSection(int idx, std::vector<uint8_t>& out) const;
    std::vector<uint8_t> raw;  // the input file bytes
};

// ---- generic type-tree walker ------------------------------------------------------------------------------
struct Value {
    enum Kind { Null, Int, Real, String, Struct, Array, Variant, Transform } kind = Null;
    int64_t i = 0;
    double r = 0;
    std::string s;                                  // String value, or type name for Variant
    std::vector<std::pair<std::string, Value>> m;   // Struct members / Variant (single member "value")
    std::vector<Value> a;                           // Array elements
    int32_t memberType = 0;                         // MemberType the value came from
    const Value* get(const std::string& name) const;
    double num() const { return kind == Real ? r : (double)i; }
};

struct TypeMember {
    int32_t type = 0;
    std::string name;
    uint32_t refType = 0;  // offset of referenced type definition array
    int32_t arrayWidth = 0;
    int32_t extra[3] = {0, 0, 0};
};
struct TypeDef {
    uint32_t offset = 0;
    std::vector<TypeMember> members;
    uint32_t size = 0;  // bytes occupied by one object
    std::string key;    // "name:type,name:type,..." fingerprint
};

class Walker {
public:
    explicit Walker(const File& f) : f_(f) {}
    const TypeDef& type(uint32_t off);
    Value read(uint32_t typeOff, uint32_t objOff, int depth = 0);
    // Reads the root object. Large arrays of scalars are kept as Array values.
    Value readRoot() { return read(f_.rootType, f_.rootObject); }
    uint32_t memberSize(const TypeMember& m);
private:
    const File& f_;
    std::map<uint32_t, TypeDef> cache_;
};

// ---- high level model ------------------------------------------------------------------------------------
struct Transform {
    uint32_t flags = 0;
    float position[3] = {0, 0, 0};
    float orientation[4] = {0, 0, 0, 1};  // x y z w
    float scaleShear[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
};

struct Bone {
    std::string name;
    int parent = -1;
    Transform local;
    float inverseWorld[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};  // 4x4, Granny layout (row-major as stored)
};

struct Skeleton {
    std::string name;
    std::vector<Bone> bones;
};

// One animation curve with `dim` components per sample (1 = scalar, 3 = vec3, 4 = quaternion xyzw, 9 = 3x3 matrix).
struct Curve {
    // File format: Granny 2.2 "granny_curve" = { int32 Degree; real32 Knots[]; real32 Controls[]; } (no variant curve
    // formats exist in these files).
    int dim = 0;                  // 3 position, 4 orientation (xyzw), 9 scale/shear; set by the loader
    int degree = 0;
    std::vector<float> knots;     // empty = curve absent; 1 knot = constant
    std::vector<float> controls;  // knots.size() * dim floats
    bool empty() const { return knots.empty(); }
    // Sample at time t (clamped to the knot range) exactly as granny2.dll does (knot search + SampleBSpline0..2);
    // out must hold `dim` floats. Returns false if the curve is empty or malformed.
    bool sample(float t, float* out) const;
};

struct TransformTrack {
    std::string name;
    uint32_t flags = 0;
    Curve orientation, position, scaleShear;
};
struct VectorTrack {
    std::string name;
    uint32_t key = 0;
    int dimension = 0;
    Curve curve;
};
struct TrackGroup {
    std::string name;
    std::vector<TransformTrack> transformTracks;
    std::vector<VectorTrack> vectorTracks;
    Transform initialPlacement;
    float loopTranslation[3] = {0, 0, 0};
    uint32_t flags = 0;
};
struct Animation {
    std::string name;
    float duration = 0, timeStep = 0, oversampling = 0;
    uint32_t flags = 0;
    std::vector<TrackGroup> trackGroups;
};

struct Mesh {
    std::string name;
    uint32_t vertexCount = 0, indexCount = 0;           // from the vertex data / tri topology
    std::vector<std::string> vertexComponents;          // "Name:type[width]"
    std::vector<float> positions;                       // xyz per vertex (if a Position component exists)
    std::vector<float> normals;
    std::vector<float> uv0;
    std::vector<float> boneWeights;                     // 4 per vertex (if present)
    std::vector<uint8_t> boneIndices;                   // 4 per vertex (if present); index into BoneBindings
    struct Group { int materialIndex = 0, triFirst = 0, triCount = 0; };
    std::vector<Group> groups;
    bool topologyValid = true;                          // false if the file stores uninitialised (0xBAADF00D) topology data
    std::vector<uint32_t> indices;                      // triangle list (empty if !topologyValid)
    std::vector<std::string> boundBones;                // BoneBindings names
};
struct Model {
    std::string name;
    int skeleton = -1;
    Transform initialPlacement;
    std::vector<int> meshes;
};

struct Scene {
    std::string fromFileName;
    std::vector<Skeleton> skeletons;
    std::vector<Animation> animations;
    std::vector<TrackGroup> trackGroups;  // all file-level track groups
    std::vector<Mesh> meshes;
    std::vector<Model> models;
};

// Loads a file and extracts the high level scene. On failure returns false and fills err.
bool loadScene(const File& f, Scene& out, std::string* err = nullptr);

// Evaluate a track at time t into a local transform (identity/missing curves fall back to `base`).
Transform sampleTrack(const TransformTrack& t, float time, const Transform& base);

// Local transform -> 4x4 matrix in Granny's row-vector convention (v' = v * M, translation in the last row, M = ScaleShear * Rot).
void transformToMatrix(const Transform& t, float m[16]);
// World matrices (row-major 4x4 per bone, same convention) for a skeleton; `pose` (one local Transform per bone) defaults to the
// rest pose stored in the skeleton. world[i] = local[i] * world[parent]. Size = 16 * bones.
std::vector<float> worldMatrices(const Skeleton& sk, const std::vector<Transform>* pose = nullptr);
// Local pose of every bone of `sk` at time t: bones with a track in `tg` take the sampled curves, others keep the rest pose.
std::vector<Transform> samplePose(const Skeleton& sk, const TrackGroup& tg, float t);

// Convenience: JSON export of a Scene (skeleton + sampled animation, `fps` samples/second; 0 = animation TimeStep or 30).
std::string sceneToJson(const Scene& s, float fps = 0, bool includeSamples = true);

}  // namespace gr2
