// High-level extraction (skeletons, animations, meshes), curve sampling and JSON export.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "gr2.h"

namespace gr2 {

// ---- curve sampling (granny2.dll FUN_1000fef0 / FUN_1000adc0 / FUN_1000aeb0 / SampleBSpline kernels) ------------
namespace {

// FUN_1000adc0: index i in [1, n-1] of the first knot >= t (lower bound).
int findKnot(const std::vector<float>& k, float t) {
    int n = (int)k.size();
    int i = (int)(std::lower_bound(k.begin(), k.end(), t) - k.begin());
    if (i < 1) i = 1;
    if (i > n - 1) i = n - 1;
    return i;
}

}  // namespace

bool Curve::sample(float t, float* out) const {
    int n = (int)knots.size();
    if (n == 0 || dim <= 0 || controls.size() < (size_t)n * dim) return false;
    if (n == 1) {  // constant
        for (int c = 0; c < dim; c++) out[c] = controls[c];
        return true;
    }
    if (t < knots.front()) t = knots.front();
    if (t > knots.back()) t = knots.back();
    int idx = findKnot(knots, t);
    int d = degree;
    if (d < 0 || d > 2) return false;
    // knot / control accessors with end clamping (the original pads the ends by replicating the first / last entry)
    auto K = [&](int i) { return knots[std::min(std::max(i, 0), n - 1)]; };
    auto P = [&](int i, int c) { return controls[(size_t)std::min(std::max(i, 0), n - 1) * dim + c]; };
    if (d == 0) {
        for (int c = 0; c < dim; c++) out[c] = P(idx, c);
        return true;
    }
    if (d == 1) {
        float u = (t - K(idx - 1)) / (K(idx) - K(idx - 1));
        for (int c = 0; c < dim; c++) out[c] = u * P(idx, c) + (1.0f - u) * P(idx - 1, c);
    } else {
        float k1 = K(idx - 1), k2 = K(idx - 2);
        float u1 = (t - k1) / (K(idx) - k1);
        float u2 = (t - k2) / (K(idx) - k2);
        float a = (u1 + u2) - u2 * u1;
        float b = ((t - k1) / (K(idx + 1) - k1)) * u1;
        for (int c = 0; c < dim; c++) out[c] = b * P(idx, c) + (a - b) * P(idx - 1, c) + (1.0f - a) * P(idx - 2, c);
    }
    if (dim == 4) {  // "4n" kernels: first-order renormalisation (3 - |q|^2) / 2
        float s = (3.0f - (out[3] * out[3] + out[2] * out[2] + out[1] * out[1] + out[0] * out[0])) * 0.5f;
        for (int c = 0; c < 4; c++) out[c] *= s;
    }
    return true;
}

Transform sampleTrack(const TransformTrack& t, float time, const Transform& base) {
    Transform r = base;
    float v[9];
    if (t.position.sample(time, v)) std::memcpy(r.position, v, 12);
    if (t.orientation.sample(time, v)) std::memcpy(r.orientation, v, 16);
    if (t.scaleShear.sample(time, v)) std::memcpy(r.scaleShear, v, 36);
    return r;
}

void transformToMatrix(const Transform& t, float m[16]) {
    float x = t.orientation[0], y = t.orientation[1], z = t.orientation[2], w = t.orientation[3];
    // column-vector rotation matrix of the quaternion; the row-vector form is its transpose
    float r[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                     {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                     {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
    float rt[3][3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) rt[i][j] = r[j][i];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            float v = 0;
            for (int k = 0; k < 3; k++) v += t.scaleShear[i * 3 + k] * rt[k][j];
            m[i * 4 + j] = v;
        }
    for (int i = 0; i < 3; i++) m[i * 4 + 3] = 0;
    m[12] = t.position[0];
    m[13] = t.position[1];
    m[14] = t.position[2];
    m[15] = 1;
}

std::vector<float> worldMatrices(const Skeleton& sk, const std::vector<Transform>* pose) {
    std::vector<float> w(sk.bones.size() * 16);
    for (size_t i = 0; i < sk.bones.size(); i++) {
        float l[16];
        transformToMatrix(pose && i < pose->size() ? (*pose)[i] : sk.bones[i].local, l);
        int p = sk.bones[i].parent;
        if (p < 0 || (size_t)p >= i) {
            std::memcpy(&w[i * 16], l, sizeof l);
            continue;
        }
        const float* pm = &w[(size_t)p * 16];
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++) {
                float v = 0;
                for (int k = 0; k < 4; k++) v += l[r * 4 + k] * pm[k * 4 + c];
                w[i * 16 + r * 4 + c] = v;
            }
    }
    return w;
}

std::vector<Transform> samplePose(const Skeleton& sk, const TrackGroup& tg, float t) {
    std::vector<Transform> pose(sk.bones.size());
    for (size_t i = 0; i < sk.bones.size(); i++) {
        pose[i] = sk.bones[i].local;
        for (auto& tt : tg.transformTracks)
            if (tt.name == sk.bones[i].name) {
                pose[i] = sampleTrack(tt, t, sk.bones[i].local);
                break;
            }
    }
    return pose;
}

// ---- Value -> model helpers -----------------------------------------------------------------------------------
namespace {

const Value* member(const Value& v, const char* name) { return v.get(name); }
std::string strOf(const Value& v, const char* name) {
    const Value* m = member(v, name);
    return m && m->kind == Value::String ? m->s : std::string();
}
double numOf(const Value& v, const char* name, double def = 0) {
    const Value* m = member(v, name);
    return m && (m->kind == Value::Int || m->kind == Value::Real) ? m->num() : def;
}
std::vector<float> floats(const Value* a) {
    std::vector<float> r;
    if (!a || a->kind != Value::Array) return r;
    r.reserve(a->a.size());
    for (auto& e : a->a) {
        if (e.kind == Value::Struct && e.m.size() == 1) r.push_back((float)e.m[0].second.num());
        else r.push_back((float)e.num());
    }
    return r;
}
void fixedFloats(const Value* a, float* out, size_t n) {
    if (!a || a->kind != Value::Array) return;
    for (size_t i = 0; i < n && i < a->a.size(); i++) out[i] = (float)a->a[i].num();
}
Transform toTransform(const Value* v) {
    Transform t;
    if (!v || v->kind != Value::Transform || v->a.size() < 17) return t;
    t.flags = (uint32_t)v->a[0].i;
    for (int i = 0; i < 3; i++) t.position[i] = (float)v->a[1 + i].r;
    for (int i = 0; i < 4; i++) t.orientation[i] = (float)v->a[4 + i].r;
    for (int i = 0; i < 9; i++) t.scaleShear[i] = (float)v->a[8 + i].r;
    return t;
}
Curve toCurve(const Value* v, int dim) {
    Curve c;
    c.dim = dim;
    if (!v || v->kind != Value::Struct) return c;
    c.degree = (int)numOf(*v, "Degree");
    c.knots = floats(member(*v, "Knots"));
    c.controls = floats(member(*v, "Controls"));
    return c;
}
TrackGroup toTrackGroup(const Value& g) {
    TrackGroup tg;
    tg.name = strOf(g, "Name");
    tg.flags = (uint32_t)numOf(g, "AccumulationFlags");
    tg.initialPlacement = toTransform(member(g, "InitialPlacement"));
    fixedFloats(member(g, "LoopTranslation"), tg.loopTranslation, 3);
    if (const Value* tts = member(g, "TransformTracks")) {
        for (auto& t : tts->a) {
            TransformTrack tt;
            tt.name = strOf(t, "Name");
            tt.flags = (uint32_t)numOf(t, "Flags");
            tt.position = toCurve(member(t, "PositionCurve"), 3);
            tt.orientation = toCurve(member(t, "OrientationCurve"), 4);
            tt.scaleShear = toCurve(member(t, "ScaleShearCurve"), 9);
            tg.transformTracks.push_back(std::move(tt));
        }
    }
    if (const Value* vts = member(g, "ScalarTracks")) {
        for (auto& t : vts->a) {
            VectorTrack vt;
            vt.name = strOf(t, "Name");
            vt.curve = toCurve(member(t, "ValueCurve"), 1);
            tg.vectorTracks.push_back(std::move(vt));
        }
    }
    return tg;
}

}  // namespace

bool loadScene(const File& f, Scene& out, std::string* err) {
    Walker w(f);
    Value root = w.readRoot();
    if (root.kind != Value::Struct) {
        if (err) *err = "root object unreadable";
        return false;
    }
    out = Scene();
    out.fromFileName = strOf(root, "FromFileName");
    if (const Value* sks = root.get("Skeletons")) {
        for (auto& s : sks->a) {
            Skeleton sk;
            sk.name = strOf(s, "Name");
            if (const Value* bones = s.get("Bones")) {
                for (auto& b : bones->a) {
                    Bone bone;
                    bone.name = strOf(b, "Name");
                    bone.parent = (int)numOf(b, "ParentIndex", -1);
                    bone.local = toTransform(b.get("Transform"));
                    fixedFloats(b.get("InverseWorldTransform"), bone.inverseWorld, 16);
                    sk.bones.push_back(std::move(bone));
                }
            }
            out.skeletons.push_back(std::move(sk));
        }
    }
    if (const Value* tgs = root.get("TrackGroups"))
        for (auto& g : tgs->a) out.trackGroups.push_back(toTrackGroup(g));
    if (const Value* ans = root.get("Animations")) {
        for (auto& a : ans->a) {
            Animation an;
            an.name = strOf(a, "Name");
            an.duration = (float)numOf(a, "Duration");
            an.timeStep = (float)numOf(a, "TimeStep");
            an.oversampling = (float)numOf(a, "Oversampling");
            if (const Value* tgs2 = a.get("TrackGroups"))
                for (auto& g : tgs2->a) an.trackGroups.push_back(toTrackGroup(g));
            out.animations.push_back(std::move(an));
        }
    }
    if (const Value* ms = root.get("Meshes")) {
        for (auto& m : ms->a) {
            Mesh mesh;
            mesh.name = strOf(m, "Name");
            if (const Value* vd = m.get("PrimaryVertexData")) {
                if (const Value* vs = vd->get("Vertices")) {
                    mesh.vertexCount = (uint32_t)vs->a.size();
                    if (!vs->a.empty())
                        for (auto& kv : vs->a[0].m) mesh.vertexComponents.push_back(kv.first);
                    for (auto& v : vs->a) {
                        float tmp[4] = {0, 0, 0, 0};
                        if (const Value* p = v.get("Position")) {
                            fixedFloats(p, tmp, 3);
                            mesh.positions.insert(mesh.positions.end(), tmp, tmp + 3);
                        }
                        std::memset(tmp, 0, sizeof tmp);
                        if (const Value* p = v.get("Normal")) {
                            fixedFloats(p, tmp, 3);
                            mesh.normals.insert(mesh.normals.end(), tmp, tmp + 3);
                        }
                        std::memset(tmp, 0, sizeof tmp);
                        if (const Value* p = v.get("TextureCoordinates0")) {
                            fixedFloats(p, tmp, 2);
                            mesh.uv0.insert(mesh.uv0.end(), tmp, tmp + 2);
                        }
                        std::memset(tmp, 0, sizeof tmp);
                        if (const Value* p = v.get("BoneWeights")) {
                            fixedFloats(p, tmp, 4);
                            mesh.boneWeights.insert(mesh.boneWeights.end(), tmp, tmp + 4);
                        }
                        if (const Value* p = v.get("BoneIndices")) {
                            for (size_t i = 0; i < 4; i++)
                                mesh.boneIndices.push_back(i < p->a.size() ? (uint8_t)p->a[i].i : 0);
                        }
                    }
                }
            }
            if (const Value* tp = m.get("PrimaryTopology")) {
                // some exports leave the topology uninitialised: every field reads 0xBAADF00D (debug-heap fill pattern)
                if (const Value* gs0 = tp->get("Groups"))
                    for (auto& g : gs0->a)
                        if ((uint32_t)numOf(g, "MaterialIndex") == 0xBAADF00Du) mesh.topologyValid = false;
                if (mesh.topologyValid) {
                    const Value* i32 = tp->get("Indices");
                    const Value* i16 = tp->get("Indices16");
                    if (i32 && !i32->a.empty())
                        for (auto& e : i32->a) mesh.indices.push_back((uint32_t)e.i);
                    else if (i16)
                        for (auto& e : i16->a) mesh.indices.push_back((uint32_t)e.i & 0xffffu);
                    mesh.indexCount = (uint32_t)mesh.indices.size();
                    if (const Value* gs = tp->get("Groups"))
                        for (auto& g : gs->a) {
                            Mesh::Group gr;
                            gr.materialIndex = (int)numOf(g, "MaterialIndex");
                            gr.triFirst = (int)numOf(g, "TriFirst");
                            gr.triCount = (int)numOf(g, "TriCount");
                            mesh.groups.push_back(gr);
                        }
                } else {
                    const Value* i32 = tp->get("Indices");
                    const Value* i16 = tp->get("Indices16");
                    mesh.indexCount = (uint32_t)((i32 ? i32->a.size() : 0) + (i16 ? i16->a.size() : 0));
                }
            }
            if (const Value* bb = m.get("BoneBindings"))
                for (auto& b : bb->a) mesh.boundBones.push_back(strOf(b, "BoneName"));
            out.meshes.push_back(std::move(mesh));
        }
    }
    if (const Value* mds = root.get("Models")) {
        for (auto& m : mds->a) {
            Model md;
            md.name = strOf(m, "Name");
            md.initialPlacement = toTransform(m.get("InitialPlacement"));
            if (const Value* sk = m.get("Skeleton")) {
                std::string sn = strOf(*sk, "Name");
                for (size_t i = 0; i < out.skeletons.size(); i++)
                    if (out.skeletons[i].name == sn) md.skeleton = (int)i;
            }
            if (const Value* mb = m.get("MeshBindings")) {
                for (auto& b : mb->a) {
                    if (const Value* mm = b.get("Mesh")) {
                        std::string mn = strOf(*mm, "Name");
                        for (size_t i = 0; i < out.meshes.size(); i++)
                            if (out.meshes[i].name == mn) md.meshes.push_back((int)i);
                    }
                }
            }
            out.models.push_back(std::move(md));
        }
    }
    return true;
}

// ---- JSON ----------------------------------------------------------------------------------------------------
namespace {
void jstr(std::string& o, const std::string& s) {
    o += '"';
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            o += '\\';
            o += (char)c;
        } else if (c < 0x20) {
            char b[8];
            std::snprintf(b, sizeof b, "\\u%04x", c);
            o += b;
        } else
            o += (char)c;
    }
    o += '"';
}
void jnum(std::string& o, double v) {
    char b[40];
    if (!std::isfinite(v)) v = 0;
    std::snprintf(b, sizeof b, "%.7g", v);
    o += b;
}
void jvec(std::string& o, const float* p, int n) {
    o += '[';
    for (int i = 0; i < n; i++) {
        if (i) o += ',';
        jnum(o, p[i]);
    }
    o += ']';
}
void jtransform(std::string& o, const Transform& t) {
    o += "{\"flags\":" + std::to_string(t.flags) + ",\"position\":";
    jvec(o, t.position, 3);
    o += ",\"orientation\":";
    jvec(o, t.orientation, 4);
    o += ",\"scaleShear\":";
    jvec(o, t.scaleShear, 9);
    o += '}';
}
}  // namespace

std::string sceneToJson(const Scene& s, float fps, bool includeSamples) {
    std::string o = "{\n\"fromFile\":";
    jstr(o, s.fromFileName);
    o += ",\n\"skeletons\":[";
    for (size_t i = 0; i < s.skeletons.size(); i++) {
        const Skeleton& sk = s.skeletons[i];
        o += i ? ",\n{" : "\n{";
        o += "\"name\":";
        jstr(o, sk.name);
        o += ",\"bones\":[";
        for (size_t b = 0; b < sk.bones.size(); b++) {
            const Bone& bn = sk.bones[b];
            o += b ? ",\n  {" : "\n  {";
            o += "\"name\":";
            jstr(o, bn.name);
            o += ",\"parent\":" + std::to_string(bn.parent) + ",\"local\":";
            jtransform(o, bn.local);
            o += ",\"inverseWorld\":";
            jvec(o, bn.inverseWorld, 16);
            o += '}';
        }
        o += "]}";
    }
    o += "],\n\"meshes\":[";
    for (size_t i = 0; i < s.meshes.size(); i++) {
        const Mesh& m = s.meshes[i];
        o += i ? ",\n{" : "\n{";
        o += "\"name\":";
        jstr(o, m.name);
        o += ",\"vertexCount\":" + std::to_string(m.vertexCount) + ",\"indexCount\":" + std::to_string(m.indexCount);
        o += ",\"boneBindings\":[";
        for (size_t b = 0; b < m.boundBones.size(); b++) {
            if (b) o += ',';
            jstr(o, m.boundBones[b]);
        }
        o += "]}";
    }
    o += "],\n\"animations\":[";
    for (size_t ai = 0; ai < s.animations.size(); ai++) {
        const Animation& a = s.animations[ai];
        float rate = fps > 0 ? fps : (a.timeStep > 0 ? 1.0f / a.timeStep : 30.0f);
        int frames = (int)std::floor(a.duration * rate + 0.5f) + 1;
        if (frames < 1) frames = 1;
        o += ai ? ",\n{" : "\n{";
        o += "\"name\":";
        jstr(o, a.name);
        o += ",\"duration\":";
        jnum(o, a.duration);
        o += ",\"timeStep\":";
        jnum(o, a.timeStep);
        o += ",\"sampleRate\":";
        jnum(o, rate);
        o += ",\"frameCount\":" + std::to_string(frames) + ",\"trackGroups\":[";
        for (size_t gi = 0; gi < a.trackGroups.size(); gi++) {
            const TrackGroup& g = a.trackGroups[gi];
            o += gi ? ",\n {" : "\n {";
            o += "\"name\":";
            jstr(o, g.name);
            o += ",\"loopTranslation\":";
            jvec(o, g.loopTranslation, 3);
            o += ",\"tracks\":[";
            for (size_t ti = 0; ti < g.transformTracks.size(); ti++) {
                const TransformTrack& t = g.transformTracks[ti];
                o += ti ? ",\n  {" : "\n  {";
                o += "\"name\":";
                jstr(o, t.name);
                o += ",\"position\":{\"degree\":" + std::to_string(t.position.degree) +
                     ",\"knots\":" + std::to_string(t.position.knots.size()) + "}";
                o += ",\"orientation\":{\"degree\":" + std::to_string(t.orientation.degree) +
                     ",\"knots\":" + std::to_string(t.orientation.knots.size()) + "}";
                o += ",\"scaleShear\":{\"degree\":" + std::to_string(t.scaleShear.degree) +
                     ",\"knots\":" + std::to_string(t.scaleShear.knots.size()) + "}";
                if (includeSamples) {
                    o += ",\"samples\":[";
                    Transform base;
                    for (int fr = 0; fr < frames; fr++) {
                        float tm = std::min(fr / rate, a.duration);
                        Transform r = sampleTrack(t, tm, base);
                        o += fr ? "," : "";
                        o += "{\"t\":";
                        jnum(o, tm);
                        if (!t.position.empty()) {
                            o += ",\"p\":";
                            jvec(o, r.position, 3);
                        }
                        if (!t.orientation.empty()) {
                            o += ",\"q\":";
                            jvec(o, r.orientation, 4);
                        }
                        if (!t.scaleShear.empty()) {
                            o += ",\"s\":";
                            jvec(o, r.scaleShear, 9);
                        }
                        o += '}';
                    }
                    o += ']';
                }
                o += '}';
            }
            o += "]}";
        }
        o += "]}";
    }
    o += "]\n}\n";
    return o;
}

}  // namespace gr2
