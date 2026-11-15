// AMX natives: Pawn float module, geometry (distances, facing), line of sight, fog of war, FindObjects, waypoints, visual effects,
// item counts, death records, plus the NativeTest harness. Semantics follow the Van Buren decompilation (docs/research/vbdecomp-script-natives.md).
//
// Conventions of the original that matter here:
//  * every game native checks the EXACT argument count (VerifyScriptParameters): a mismatch logs and returns 0 (see AmxHost::Argc);
//  * positions are `float[3]` vectors by reference in world units; Distance / PointDistance / GetFacing / GetDirectionToEntity return
//    TRUNCATED INTEGERS (the originals tail-call __ftol2), never float cells;
//  * a facing is an angle in degrees, 0 = +Z, 90 = +X, negated for x < 0 (acos of the normalised z component);
//  * our world has no height data: y is always 0, so every "3D" distance is the 2D distance on the ground plane.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <sstream>
#include "AmxHost.h"
#include "Server.h"
#include "common/log.h"

namespace f3 {

namespace {
AmxCell FBits(float f) { AmxCell c; std::memcpy(&c, &f, 4); return c; }
float FVal(AmxCell c) { float f; std::memcpy(&f, &c, 4); return f; }
// The originals return the low DWORD of (__int64)value (x87 __ftol2): truncation toward zero, NaN -> 0.
AmxCell Trunc(double v) {
    if (!(v == v)) return 0;
    if (v >= 9.2e18) v = 9.2e18;
    if (v <= -9.2e18) v = -9.2e18;
    return static_cast<AmxCell>(static_cast<int64_t>(v));
}
// yaw (degrees) of the unit direction (dx, dz) exactly like the original: acos(z / |d|) * 57.295776, negated for x < 0. Zero length -> NaN -> 0.
double YawOf(float dx, float dz) {
    const double len = std::sqrt(static_cast<double>(dx) * dx + static_cast<double>(dz) * dz);
    if (len == 0.0) return std::nan("");
    const float nz = static_cast<float>(dz / len);
    const double angle = std::acos(static_cast<double>(nz)) * 57.295776f;
    return dx < 0.f ? -angle : angle;
}
// our 8 directions as original yaw degrees (0 = +Z = cell +y = S, 90 = +X = E)
constexpr int kDirYaw[8] = {180, 135, 90, 45, 0, -45, -90, -135};   // N NE E SE S SW W NW
Dir DirFromDegrees(int deg) {
    int step = static_cast<int>(std::lround(static_cast<double>(((deg % 360) + 360) % 360) / 45.0)) & 7;   // 0 S, 1 SE, 2 E, 3 NE, 4 N, 5 NW, 6 W, 7 SW
    static const Dir byStep[8] = {Dir::S, Dir::SE, Dir::E, Dir::NE, Dir::N, Dir::NW, Dir::W, Dir::SW};
    return byStep[step];
}
}  // namespace

// ------------------------------------------------------------------ helpers

bool AmxHost::Argc(const char* native, int got, int want) {
    if (got == want) return true;
    if (argcWarned_.insert(native).second)
        log::Warn("amx: %s() expects %d parameter(s) but received %d (the original returns 0)", native, want, got);
    return false;
}

bool AmxHost::ReadVec(const AmxInstance& m, AmxCell addr, float out[3]) {
    if (addr < 0) return false;
    for (int i = 0; i < 3; ++i) out[i] = FVal(m.Get(addr + 4 * i));
    return true;
}

void AmxHost::WriteVec(AmxInstance& m, AmxCell addr, float x, float y, float z) {
    m.Set(addr, FBits(x)); m.Set(addr + 4, FBits(y)); m.Set(addr + 8, FBits(z));
}

AmxHost::EntRef AmxHost::Resolve(AmxCell h) {
    EntRef r;
    if (h <= 0) return r;
    if (h & 0x40000000) {
        Map* mp = nullptr;
        Item* it = server_.FindObject(static_cast<ItemId>(h & 0x3FFFFFFF), &mp);
        if (!it || !mp) return r;
        r.it = it; r.map = mp->id;
        r.x = (static_cast<float>(it->pos.x) + 0.5f) * 0.5f + mp->originX;
        r.z = (static_cast<float>(it->pos.y) + 0.5f) * 0.5f + mp->originZ;
        return r;
    }
    Critter* c = server_.GetWorld().GetCritter(static_cast<CritterId>(h));
    if (!c) return r;
    const Map* mp = server_.GetWorld().GetMap(c->mapId);
    r.c = c; r.map = c->mapId;
    r.x = c->fx * 0.5f + (mp ? mp->originX : 0.f);   // fx is in cells (2 per world unit); cell (cx,cy) spans origin + [cx/2, (cx+1)/2)
    r.z = c->fy * 0.5f + (mp ? mp->originZ : 0.f);
    return r;
}

MapId AmxHost::OwnerMap(AmxInstance& m) {
    if (m.userData == 0) return 0;
    return Resolve(Handle(m.userData)).map;
}

// Grid line of sight: Bresenham over the walkability grid, endpoints excluded. Approximation of the original DDA over the map's fog/LOS
// height grid (not exported): walls and closed locked doors block, everything else (including low cover) does not.
bool AmxHost::LineClear(const Map& map, float ax, float az, float bx, float bz) const {
    auto cell = [&](float w, float o) { return static_cast<int>(std::floor((w - o) * 2.f)); };
    int x0 = cell(ax, map.originX), y0 = cell(az, map.originZ), x1 = cell(bx, map.originX), y1 = cell(bz, map.originZ);
    const int ex = x1, ey = y1, sx0 = x0, sy0 = y0;
    int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (int guard = 0; guard < 4096; ++guard) {
        if (!(x0 == sx0 && y0 == sy0) && !(x0 == ex && y0 == ey) && map.IsBlocked(x0, y0)) return false;
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
    return true;
}

// IsUnderFOW model: a point is visible when a living player of the map is within the view radius (Server::kViewCells) with a clear line.
bool AmxHost::Visible(const Map& map, float x, float z) const {
    const int cx = static_cast<int>(std::floor((x - map.originX) * 2.f)), cy = static_cast<int>(std::floor((z - map.originZ) * 2.f));
    for (const Critter* p : map.critters) {
        if (!p->IsPlayer() || p->dead) continue;
        if (std::max(std::abs(p->pos.x - cx), std::abs(p->pos.y - cy)) > Server::kViewCells) continue;
        if (LineClear(map, p->fx * 0.5f + map.originX, p->fy * 0.5f + map.originZ, x, z)) return true;
    }
    return false;
}

// ------------------------------------------------------------------ Pawn float module (float.c of the original small-script)

void AmxHost::BindFloatNatives() {
    auto& N = natives_;
    N["float"] = [](AmxInstance&, const AmxCell* a, int) { return FBits(static_cast<float>(a[0])); };
    N["floatadd"] = [](AmxInstance&, const AmxCell* a, int) { return FBits(FVal(a[0]) + FVal(a[1])); };
    N["floatsub"] = [](AmxInstance&, const AmxCell* a, int) { return FBits(FVal(a[0]) - FVal(a[1])); };
    N["floatmul"] = [](AmxInstance&, const AmxCell* a, int) { return FBits(FVal(a[0]) * FVal(a[1])); };
    // plain IEEE division: x/0 = +-inf (0/0 = NaN), no special case
    N["floatdiv"] = [](AmxInstance&, const AmxCell* a, int) { return FBits(FVal(a[0]) / FVal(a[1])); };
    N["floatfract"] = [](AmxInstance&, const AmxCell* a, int) { float f = FVal(a[0]); return FBits(f - static_cast<float>(std::floor(static_cast<double>(f)))); };
    // floatround(f, mode): mode 1 floor, 2 ceil, 3 toward zero, anything else floor(f + .5) (so -2.5 rounds to -2, not -3). The original reads the
    // mode unconditionally; a missing argument behaves like the default mode.
    N["floatround"] = [](AmxInstance&, const AmxCell* a, int n) {
        const double f = FVal(a[0]);
        const AmxCell mode = n > 1 ? a[1] : 0;
        double r;
        switch (mode) {
            case 1: r = std::floor(f); break;
            case 2: r = std::ceil(f); break;
            case 3: r = f >= 0.0 ? std::floor(f) : std::ceil(f); break;
            default: r = std::floor(f + .5); break;
        }
        return static_cast<AmxCell>(static_cast<float>(r));
    };
    // NaN compares unequal and not greater: -1
    N["floatcmp"] = [](AmxInstance&, const AmxCell* a, int) -> AmxCell { float x = FVal(a[0]), y = FVal(a[1]); return x == y ? 0 : (x > y ? 1 : -1); };
}

// ------------------------------------------------------------------ geometry, LOS, search, waypoints, items

void AmxHost::BindSpatialNatives() {
    auto& N = natives_;

    // ---- life state
    // IsDead(entity): unresolved entity -> 1 (original). Map objects have no health model here: alive.
    N["IsDead"] = [this](AmxInstance&, const AmxCell* a, int n) -> AmxCell {
        if (!Argc("IsDead", n, 1)) return 0;
        EntRef e = Resolve(a[0]);
        if (e.c) return e.c->dead ? 1 : 0;
        return e.it ? 0 : 1;
    };
    // HasDied(instanceName$): the COUNTER global `_DEAD_<name>` (the original native RecordDeath adds one per death; here AmxHost::OnDeath
    // does it for every NPC with an instance name). It takes a name string, not an entity.
    N["HasDied"] = [this](AmxInstance& m, const AmxCell* a, int n) -> AmxCell {
        if (!Argc("HasDied", n, 1)) return 0;
        return static_cast<AmxCell>(server_.GetGlobal("_DEAD_" + m.GetString(a[0], 254)));
    };

    // ---- position / distance / facing
    N["GetPosition"] = [this](AmxInstance& m, const AmxCell* a, int n) -> AmxCell {   // (entity, float out[3]) world units, y = 0 (no heights)
        if (!Argc("GetPosition", n, 2)) return 0;
        EntRef e = Resolve(a[0]);
        if (!e || a[1] < 0) return 0;
        WriteVec(m, a[1], e.x, 0.f, e.z);
        return 1;
    };
    // Distance(a, b): truncated integer world units (scripts compare with int constants: `Distance(..) < 25`). Unknown entity -> 0 (original);
    // entities of different maps have unrelated coordinates: 9999.
    N["Distance"] = [this](AmxInstance&, const AmxCell* a, int n) -> AmxCell {
        if (!Argc("Distance", n, 2)) return 0;
        EntRef x = Resolve(a[0]), y = Resolve(a[1]);
        if (!x || !y) return 0;
        if (x.map != y.map) return 9999;
        const double dx = static_cast<double>(x.x) - y.x, dz = static_cast<double>(x.z) - y.z;
        return Trunc(std::sqrt(dx * dx + dz * dz));
    };
    N["PointDistance"] = [this](AmxInstance& m, const AmxCell* a, int n) -> AmxCell {   // (float p1[3], float p2[3]) -> truncated int
        if (!Argc("PointDistance", n, 2)) return 0;
        float p[3], q[3];
        if (!ReadVec(m, a[0], p) || !ReadVec(m, a[1], q)) return 0;
        const double x = static_cast<double>(p[0]) - q[0], y = static_cast<double>(p[1]) - q[1], z = static_cast<double>(p[2]) - q[2];
        return Trunc(std::sqrt(z * z + y * y + x * x));
    };
    // GetFacing(entity): integer degrees. The server only stores 8 directions, so the result is a multiple of 45 (SetFacing(37) reads back 45).
    N["GetFacing"] = [this](AmxInstance&, const AmxCell* a, int n) -> AmxCell {
        if (!Argc("GetFacing", n, 1)) return 0;
        EntRef e = Resolve(a[0]);
        return e.c ? kDirYaw[static_cast<int>(e.c->dir) & 7] : 0;
    };
    N["SetFacing"] = [this](AmxInstance&, const AmxCell* a, int n) -> AmxCell {   // (entity, integer degrees)
        if (!Argc("SetFacing", n, 2)) return 0;
        EntRef e = Resolve(a[0]);
        if (!e.c) return 0;
        e.c->dir = DirFromDegrees(a[1]);
        return 1;
    };
    N["SetFacingTarget"] = [this](AmxInstance&, const AmxCell* a, int n) -> AmxCell {   // (entity, target): face the target
        if (!Argc("SetFacingTarget", n, 2)) return 0;
        EntRef e = Resolve(a[0]), t = Resolve(a[1]);
        if (!e || !t) return 0;
        if (e.c) {
            const double yaw = YawOf(t.x - e.x, t.z - e.z);
            if (yaw == yaw) e.c->dir = DirFromDegrees(static_cast<int>(std::lround(yaw)));
        }
        return 1;
    };
    N["GetDirectionToEntity"] = [this](AmxInstance&, const AmxCell* a, int n) -> AmxCell {   // (entity, target) -> truncated int degrees
        if (!Argc("GetDirectionToEntity", n, 2)) return 0;
        EntRef e = Resolve(a[0]), t = Resolve(a[1]);
        if (!e || !t) return 0;
        return Trunc(YawOf(t.x - e.x, t.z - e.z));
    };

    // ---- line of sight / fog of war
    // GetLOS(a, b): 2 when a sees b, else 0 (the original returns 0/1/2, scripts only test non-zero). Grid approximation, see LineClear.
    N["GetLOS"] = [this](AmxInstance&, const AmxCell* a, int n) -> AmxCell {
        if (!Argc("GetLOS", n, 2)) return 0;
        EntRef s = Resolve(a[0]), t = Resolve(a[1]);
        if (!s || !t || s.map != t.map) return 0;
        const Map* mp = server_.GetWorld().GetMap(s.map);
        return mp && LineClear(*mp, s.x, s.z, t.x, t.z) ? 2 : 0;
    };
    N["GetLOSPoint"] = [this](AmxInstance& m, const AmxCell* a, int n) -> AmxCell {   // (entity, float point[3])
        if (!Argc("GetLOSPoint", n, 2)) return 0;
        EntRef s = Resolve(a[0]);
        float p[3];
        if (!s || !ReadVec(m, a[1], p)) return 0;
        const Map* mp = server_.GetWorld().GetMap(s.map);
        return mp && LineClear(*mp, s.x, s.z, p[0], p[2]) ? 2 : 0;
    };
    // IsUnderFOW(entity): 1 when no living player of the entity's map can see it (view radius + grid line of sight), a player is never under
    // fog for itself. The original reads the client's visible-cell bits; the server has no such state, this is the documented substitute.
    N["IsUnderFOW"] = [this](AmxInstance&, const AmxCell* a, int n) -> AmxCell {
        if (!Argc("IsUnderFOW", n, 1)) return 0;
        EntRef e = Resolve(a[0]);
        if (!e) return 0;
        if (e.c && e.c->IsPlayer() && !e.c->dead) return 0;
        const Map* mp = server_.GetWorld().GetMap(e.map);
        return mp && !Visible(*mp, e.x, e.z) ? 1 : 0;
    };

    // ---- FindObjects(out handle, float pos[3], float radius) -> count; GetObject(handle, index) -> entity; GetType(entity)
    // Entities of the owner's map (the owner itself excluded) whose box touches the circle (XZ plane, height ignored), sorted FARTHEST FIRST
    // (the original comparator returns dist(a) >= dist(b)). Critters are 2x2 cells, map objects use their footprint (doors) or half a unit.
    // The output cell is written only when the count is > 0; the lists live one tick.
    N["FindObjects"] = [this](AmxInstance& m, const AmxCell* a, int n) -> AmxCell {
        if (!Argc("FindObjects", n, 3)) return 0;
        float p[3];
        if (!ReadVec(m, a[1], p)) return 0;
        const float radius = FVal(a[2]);
        const MapId mapId = OwnerMap(m);
        Map* mp = mapId ? server_.GetWorld().GetMap(mapId) : nullptr;
        if (!mp) return 0;
        const AmxCell owner = Handle(m.userData);
        std::vector<std::pair<double, AmxCell>> found;
        auto consider = [&](AmxCell h, float x0, float z0, float x1, float z1, float cx, float cz) {
            const float nx = std::clamp(p[0], x0, x1), nz = std::clamp(p[2], z0, z1);
            const double dx = static_cast<double>(p[0]) - nx, dz = static_cast<double>(p[2]) - nz;
            if (dx * dx + dz * dz > static_cast<double>(radius) * radius) return;
            const double cdx = static_cast<double>(p[0]) - cx, cdz = static_cast<double>(p[2]) - cz;
            found.emplace_back(std::sqrt(cdx * cdx + cdz * cdz), h);
        };
        for (Critter* c : mp->critters) {
            if (Handle(c->id) == owner) continue;
            const float x = c->fx * 0.5f + mp->originX, z = c->fy * 0.5f + mp->originZ;
            consider(static_cast<AmxCell>(c->id), x - 1.f, z - 1.f, x + 1.f, z + 1.f, x, z);
        }
        for (auto& [id, it] : mp->items) {
            const AmxCell h = Handle(static_cast<uint64_t>(id) | kObjectBit);
            if (h == owner) continue;
            float x0, z0, x1, z1;
            if (!it.footprint.empty()) {
                int minx = 1 << 30, miny = 1 << 30, maxx = -(1 << 30), maxy = -(1 << 30);
                for (const TilePos& t : it.footprint) { minx = std::min<int>(minx, t.x); miny = std::min<int>(miny, t.y); maxx = std::max<int>(maxx, t.x); maxy = std::max<int>(maxy, t.y); }
                x0 = minx * 0.5f + mp->originX; x1 = (maxx + 1) * 0.5f + mp->originX;
                z0 = miny * 0.5f + mp->originZ; z1 = (maxy + 1) * 0.5f + mp->originZ;
            } else {
                const float cx = (it.pos.x + 0.5f) * 0.5f + mp->originX, cz = (it.pos.y + 0.5f) * 0.5f + mp->originZ;
                x0 = cx - 0.5f; x1 = cx + 0.5f; z0 = cz - 0.5f; z1 = cz + 0.5f;
            }
            consider(h, x0, z0, x1, z1, (x0 + x1) * 0.5f, (z0 + z1) * 0.5f);
        }
        if (found.empty()) return 0;
        std::stable_sort(found.begin(), found.end(), [](const auto& l, const auto& r) { return l.first > r.first; });
        const AmxCell handle = nextObjectList_++;
        auto& list = objectLists_[handle];
        for (auto& f : found) list.push_back(f.second);
        if (a[0] >= 0) m.Set(a[0], handle);
        return static_cast<AmxCell>(list.size());
    };
    N["GetObject"] = [this](AmxInstance&, const AmxCell* a, int n) -> AmxCell {
        if (!Argc("GetObject", n, 2)) return 0;
        auto it = objectLists_.find(a[0]);
        if (it == objectLists_.end() || a[1] < 0 || static_cast<size_t>(a[1]) >= it->second.size()) return 0;
        return it->second[static_cast<size_t>(a[1])];
    };
    // GetType: entityClass 1 critter, 2 usable, 3 door, 4 container, 5 item, 6 weapon, 7 armor, 9 ammo
    N["GetType"] = [this](AmxInstance&, const AmxCell* a, int n) -> AmxCell {
        if (!Argc("GetType", n, 1)) return 0;
        EntRef e = Resolve(a[0]);
        if (e.c) return 1;
        if (!e.it) return 0;
        switch (e.it->kind) {
            case ObjKind::Usable: return 2;
            case ObjKind::Door: return 3;
            case ObjKind::Container: return 4;
            default: break;
        }
        if (const Proto* p = server_.Data().GetProto(e.it->pid)) {
            if (p->cls == PClass::Weapon) return 6;
            if (p->cls == PClass::Armor) return 7;
            if (p->cls == PClass::Ammo) return 9;
        }
        return 5;
    };

    // ---- map data: waypoint paths (EPTH) and navigation points (EMNP; the demo maps have none)
    auto ownerMapPtr = [this](AmxInstance& m) -> Map* { MapId id = OwnerMap(m); return id ? server_.GetWorld().GetMap(id) : nullptr; };
    auto findPath = [](const Map& mp, const std::string& name) -> int {
        for (size_t i = 0; i < mp.pathNames.size(); ++i) if (mp.pathNames[i] == name) return static_cast<int>(i);
        return -1;
    };
    N["GetWayPointCount"] = [this, ownerMapPtr, findPath](AmxInstance& m, const AmxCell* a, int n) -> AmxCell {   // (name$) -> waypoints of that path, 0 if unknown
        if (!Argc("GetWayPointCount", n, 1)) return 0;
        Map* mp = ownerMapPtr(m);
        if (!mp) return 0;
        int i = findPath(*mp, m.GetString(a[0]));
        return i < 0 ? 0 : static_cast<AmxCell>(mp->pathsWorld[static_cast<size_t>(i)].size());
    };
    N["GetWayPointPosition"] = [this, ownerMapPtr, findPath](AmxInstance& m, const AmxCell* a, int n) -> AmxCell {   // (name$, index, float out[3]) -> 1 / 0
        if (!Argc("GetWayPointPosition", n, 3)) return 0;
        Map* mp = ownerMapPtr(m);
        if (!mp || a[2] < 0) return 0;
        int i = findPath(*mp, m.GetString(a[0]));
        if (i < 0) return 0;
        const auto& pts = mp->pathsWorld[static_cast<size_t>(i)];
        if (a[1] < 0 || static_cast<size_t>(a[1]) >= pts.size()) return 0;
        WriteVec(m, a[2], pts[static_cast<size_t>(a[1])].first, 0.f, pts[static_cast<size_t>(a[1])].second);
        return 1;
    };
    // GetNearestPath(entity, out name$): the path whose FIRST waypoint is nearest to the entity (strictly nearer wins), writes its name
    N["GetNearestPath"] = [this](AmxInstance& m, const AmxCell* a, int n) -> AmxCell {
        if (!Argc("GetNearestPath", n, 2)) return 0;
        EntRef e = Resolve(a[0]);
        Map* mp = e ? server_.GetWorld().GetMap(e.map) : nullptr;
        if (!mp) return 0;
        int best = -1;
        double bestD = 0;
        for (size_t i = 0; i < mp->pathsWorld.size(); ++i) {
            if (mp->pathsWorld[i].empty()) continue;
            const double dx = static_cast<double>(e.x) - mp->pathsWorld[i][0].first, dz = static_cast<double>(e.z) - mp->pathsWorld[i][0].second;
            const double d = std::sqrt(dx * dx + dz * dz);
            if (best < 0 || d < bestD) { best = static_cast<int>(i); bestD = d; }
        }
        if (best < 0) return 0;
        m.SetString(a[1], mp->pathNames[static_cast<size_t>(best)]);
        return 1;
    };
    N["GetNavPointCount"] = [this](AmxInstance&, const AmxCell*, int n) -> AmxCell {   // the EMNP chunk of every demo map is empty
        if (!Argc("GetNavPointCount", n, 0)) return 0;
        return 0;
    };
    N["GetNavPointPosition"] = [this](AmxInstance&, const AmxCell*, int n) -> AmxCell {   // (index, float out[3]): no nav points exist
        if (!Argc("GetNavPointPosition", n, 2)) return 0;
        return 0;
    };

    // ---- visual effect groups: client-only; the server only allocates ids (a no-op for rendering, logged once per effect name)
    auto keyOf = [](AmxCell h) { return (h & 0x40000000) ? ((static_cast<uint64_t>(h) & 0x3FFFFFFFu) | kObjectBit) : static_cast<uint64_t>(h); };
    N["AttachVEG"] = [this, keyOf](AmxInstance& m, const AmxCell* a, int n) -> AmxCell {   // (entity, effect$) -> effect id
        if (!Argc("AttachVEG", n, 2)) return 0;
        if (!Resolve(a[0])) return 0;
        const std::string name = m.GetString(a[1]);
        if (argcWarned_.insert("veg:" + name).second) log::Info("amx: AttachVEG('%s') is not rendered (no effect protocol yet); only the id is tracked", name.c_str());
        const int id = nextVeg_++;
        vegs_[keyOf(a[0])].emplace_back(name, id);
        return id;
    };
    N["RemoveVEG"] = [this, keyOf](AmxInstance&, const AmxCell* a, int n) -> AmxCell {   // (entity, id): always 1 for a known entity
        if (!Argc("RemoveVEG", n, 2)) return 0;
        if (!Resolve(a[0])) return 0;
        auto it = vegs_.find(keyOf(a[0]));
        if (it != vegs_.end()) {
            auto& v = it->second;
            v.erase(std::remove_if(v.begin(), v.end(), [&](const auto& e) { return e.second == a[1]; }), v.end());
        }
        return 1;
    };
    N["GetVEGID"] = [this, keyOf](AmxInstance& m, const AmxCell* a, int n) -> AmxCell {   // (entity, name$) -> id, 0 if not attached
        if (!Argc("GetVEGID", n, 2)) return 0;
        if (!Resolve(a[0])) return 0;
        auto it = vegs_.find(keyOf(a[0]));
        if (it == vegs_.end()) return 0;
        const std::string name = m.GetString(a[1]);
        for (const auto& e : it->second) if (e.first == name) return e.second;
        return 0;
    };

    // ---- items: counts by resource name (the prototype key, e.g. "Ammo9mm.AMO"; case-insensitive)
    auto countItems = [this](const std::vector<InvItem>& inv, const std::string& res) -> AmxCell {
        const std::string want = DataStore::Lower(res);
        AmxCell total = 0;
        for (const InvItem& it : inv)
            if (const Proto* p = server_.Data().GetProto(it.pid))
                if (DataStore::Lower(p->key) == want) total += it.count;
        return total;
    };
    // HasItem(entity, resource$) -> COUNT of that item (not a boolean) in the critter's inventory or the container's contents
    N["HasItem"] = [this, countItems](AmxInstance& m, const AmxCell* a, int n) -> AmxCell {
        if (!Argc("HasItem", n, 2)) return 0;
        EntRef e = Resolve(a[0]);
        if (!e) return 0;
        return countItems(e.c ? e.c->inventory : e.it->contents, m.GetString(a[1]));
    };
    // PartyHasItem(resource$): sum over the party; the multiplayer demo has no party, the protagonist (conversation player / nearest player) is it
    N["PartyHasItem"] = [this, countItems](AmxInstance& m, const AmxCell* a, int n) -> AmxCell {
        if (!Argc("PartyHasItem", n, 1)) return 0;
        Critter* p = Protagonist(m);
        return p ? countItems(p->inventory, m.GetString(a[0])) : 0;
    };
}

// ------------------------------------------------------------------ NativeTest: call natives by name (dev / test only)
//
// program := [owner=<critter id>;] call { ; call }          call := [name=]Native arg...
//   arg: 123 int | f1.5 float | v1,2,3 new float vector | p<critter> / p<critter>,dx,dz  vector of an entity's world position (+offset) |
//        s<text> string | n<critter> instance name of a critter | &name pointer to a named 24 cell buffer | @name value of its first cell
//   "name=" before the native stores the result in that buffer. The reply lists, per call, "Native=<int>[<float>]" and the buffers it was given.
std::string AmxHost::NativeTest(CritterId owner, const std::string& program) {
    if (!available_) return "amx not available";
    static std::string hostScript;
    if (hostScript.empty()) {   // any loadable behaviour script serves as the memory image of the test instance
        std::error_code ec;
        for (auto& e : std::filesystem::directory_iterator(dir_, ec))
            if (e.path().extension() == ".scrb") { hostScript = e.path().stem().string(); break; }
    }
    std::vector<std::string> calls;
    {
        std::stringstream ss(program);
        for (std::string c; std::getline(ss, c, ';');) if (c.find_first_not_of(" \t") != std::string::npos) calls.push_back(c);
    }
    if (!calls.empty() && calls[0].find("owner=") != std::string::npos) {
        owner = static_cast<CritterId>(std::atoi(calls[0].substr(calls[0].find("owner=") + 6).c_str()));
        calls.erase(calls.begin());
    }
    auto inst = NewInstance(hostScript, owner);
    if (!inst) return "cannot create a test instance";
    std::map<std::string, AmxCell> bufs;
    auto buf = [&](const std::string& name) {
        auto it = bufs.find(name);
        if (it == bufs.end()) it = bufs.emplace(name, inst->Alloc(24)).first;
        return it->second;
    };
    std::string out;
    for (const std::string& call : calls) {
        std::istringstream ts(call);
        std::string first;
        ts >> first;
        std::string capture, native = first;
        if (auto eq = first.find('='); eq != std::string::npos) { capture = first.substr(0, eq); native = first.substr(eq + 1); }
        std::vector<AmxCell> args;
        std::vector<std::string> used;
        bool bad = false;
        for (std::string t; ts >> t;) {
            const char k = t[0];
            if (k == 'f' && t.size() > 1) args.push_back(FBits(static_cast<float>(std::atof(t.c_str() + 1))));
            else if (k == 'v' && t.size() > 1) {
                float v[3] = {0, 0, 0};
                std::sscanf(t.c_str() + 1, "%f,%f,%f", &v[0], &v[1], &v[2]);
                AmxCell addr = inst->Alloc(3);
                if (addr < 0) { bad = true; break; }
                WriteVec(*inst, addr, v[0], v[1], v[2]);
                args.push_back(addr);
            } else if (k == 'p' && t.size() > 1) {
                int id = 0; float dx = 0, dz = 0;
                std::sscanf(t.c_str() + 1, "%d,%f,%f", &id, &dx, &dz);
                EntRef e = Resolve(id);
                AmxCell addr = inst->Alloc(3);
                if (!e || addr < 0) { bad = true; break; }
                WriteVec(*inst, addr, e.x + dx, 0.f, e.z + dz);
                args.push_back(addr);
            } else if (k == 's') {
                std::string text = t.substr(1);
                AmxCell addr = inst->Alloc(static_cast<int>(text.size()) + 1);
                if (addr < 0) { bad = true; break; }
                inst->SetString(addr, text, text.size() + 1);
                args.push_back(addr);
            } else if (k == 'n' && t.size() > 1) {
                Critter* c = CritterOf(std::atoi(t.c_str() + 1));
                std::string text = c ? c->instName : std::string();
                AmxCell addr = inst->Alloc(static_cast<int>(text.size()) + 1);
                if (addr < 0) { bad = true; break; }
                inst->SetString(addr, text, text.size() + 1);
                args.push_back(addr);
            } else if (k == '&' && t.size() > 1) { args.push_back(buf(t.substr(1))); used.push_back(t.substr(1)); }
            else if (k == '@' && t.size() > 1) args.push_back(inst->Get(buf(t.substr(1))));
            else if (k == '-' || (k >= '0' && k <= '9')) args.push_back(static_cast<AmxCell>(std::atoll(t.c_str())));
            else { bad = true; break; }
        }
        if (!out.empty()) out += " | ";
        auto it = natives_.find(native);
        if (bad || it == natives_.end()) { out += native + "=<" + (bad ? "bad argument" : "unbound") + ">"; continue; }
        const AmxCell ret = it->second(*inst, args.data(), static_cast<int>(args.size()));
        if (!capture.empty()) inst->Set(buf(capture), ret);
        char tmp[96];
        std::snprintf(tmp, sizeof tmp, "%s=%d[%g]", native.c_str(), ret, static_cast<double>(FVal(ret)));
        out += tmp;
        for (const std::string& u : used) {
            const AmxCell b = buf(u);
            std::string s;
            for (int i = 0; i < 23; ++i) { AmxCell c = inst->Get(b + 4 * i); if (c <= 0 || c > 255) break; s.push_back(static_cast<char>(c)); }
            std::snprintf(tmp, sizeof tmp, " %s:i=%d f=(%g,%g,%g) s='", u.c_str(), inst->Get(b), static_cast<double>(FVal(inst->Get(b))),
                          static_cast<double>(FVal(inst->Get(b + 4))), static_cast<double>(FVal(inst->Get(b + 8))));
            out += tmp + s + "'";
        }
    }
    return out;
}

}  // namespace f3
