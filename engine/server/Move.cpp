// Continuous, path-based movement. The walk grid (0.5 world unit cells) is only used for collision and pathfinding:
// positions are floats in cell units. A move is an A* path on the grid, string-pulled into a few straight segments
// (every segment validated against the grid with a clearance radius), sent to clients as ONE MovePath message with a
// start time and a speed; clients interpolate. The server integrates the same polyline every tick and re-validates every
// cell it enters (walls, closed doors, other critters, AP cost per cell), so it stays authoritative.
#include <algorithm>
#include <cmath>
#include <functional>
#include "Scripting.h"
#include "Server.h"
#include "common/log.h"

namespace f3 {

namespace {
constexpr size_t kMaxPathPts = 200;
constexpr int kSmoothLookahead = 48;

inline bool BlockedAt(const Map& m, float x, float y) {
    return m.IsBlocked(static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)));
}

// Is the straight segment a->b walkable for a point with clearance r (cells)? Samples the centre line and two parallel
// lines at +-r, and forbids squeezing through the corner between two diagonal blocked cells.
bool SegmentClear(const Map& m, float ax, float ay, float bx, float by, float r) {
    float dx = bx - ax, dy = by - ay;
    float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-4f) return !BlockedAt(m, ax, ay);
    float nx = -dy / len, ny = dx / len;
    int n = static_cast<int>(std::ceil(len / 0.2f)) + 1;
    int pcx = static_cast<int>(std::floor(ax)), pcy = static_cast<int>(std::floor(ay));
    for (int i = 0; i <= n; ++i) {
        float t = static_cast<float>(i) / n;
        float px = ax + dx * t, py = ay + dy * t;
        int cx = static_cast<int>(std::floor(px)), cy = static_cast<int>(std::floor(py));
        if (m.IsBlocked(cx, cy)) return false;
        if (cx != pcx && cy != pcy && (m.IsBlocked(cx, pcy) || m.IsBlocked(pcx, cy))) return false;
        pcx = cx; pcy = cy;
        if (r > 0) {
            if (BlockedAt(m, px + nx * r, py + ny * r) || BlockedAt(m, px - nx * r, py - ny * r)) return false;
        }
    }
    return true;
}

// String pulling: from each point jump to the farthest later point that is directly walkable (with clearance);
// adjacent points (consecutive grid cells) are always accepted, they are valid by construction of the A*.
std::vector<Critter::Pt> Smooth(const Map& m, const std::vector<Critter::Pt>& in, float r) {
    std::vector<Critter::Pt> out;
    if (in.size() < 3) return in;
    out.push_back(in[0]);
    size_t i = 0;
    while (i + 1 < in.size()) {
        size_t j = std::min(in.size() - 1, i + kSmoothLookahead);
        for (; j > i + 1; --j)
            if (SegmentClear(m, in[i].x, in[i].y, in[j].x, in[j].y, r)) break;
        out.push_back(in[j]);
        i = j;
    }
    return out;
}

Dir DirFromVec(float dx, float dy, Dir fallback) {
    if (std::fabs(dx) < 1e-4f && std::fabs(dy) < 1e-4f) return fallback;
    float ang = std::atan2(dx, -dy);  // 0 = north (y-), clockwise
    int step = static_cast<int>(std::lround(ang / (3.14159265f / 4)));
    return static_cast<Dir>(((step % 8) + 8) % 8);
}
}  // namespace

bool Server::DefaultRun(const Critter& c) const {
    bool run = c.IsPlayer() ? rt_.RtAlwaysRun : (c.target != 0);
    if (run && !rt_.RunOnCombat && NowMs() < c.battleEndMs) run = false;
    return run;
}

// Legacy per-cell CritterMove for clients without the continuous-move capability (snap=1 goes to everybody).
void Server::SendLegacyMove(const Critter& c, uint8_t snap) {
    Map* m = world_.GetMap(c.mapId);
    if (!m) return;
    ByteBuffer buf = net::Pack(net::CritterMove{c.id, c.pos.x, c.pos.y, c.dir, snap});
    for (Critter* p : m->critters)
        if (p->IsPlayer() && (p == &c || p->visCritters.count(c.id)) && (snap || !(p->caps & gnet::kCapContinuousMove))) Send(*p, buf);
}

void Server::SendCritterStop(const Critter& c, uint8_t reason, Critter* onlyTo) {
    Map* m = world_.GetMap(c.mapId);
    if (!m) return;
    gnet::CritterStop st;
    st.id = c.id; st.x = c.fx; st.y = c.fy; st.dir = static_cast<uint8_t>(c.dir); st.reason = reason;
    ByteBuffer buf = gnet::PackG(st);
    if (onlyTo) { if (onlyTo->caps & gnet::kCapContinuousMove) Send(*onlyTo, buf); return; }
    for (Critter* p : m->critters)
        if (p->IsPlayer() && (p->caps & gnet::kCapContinuousMove) && (p == &c || p->visCritters.count(c.id))) Send(*p, buf);
}

// MovePath for the remaining route of a moving critter (pts[0] = its position right now).
void Server::SendMovePath(Critter& c, Critter* onlyTo) {
    Map* m = world_.GetMap(c.mapId);
    if (!m || !c.mv.active) return;
    gnet::MovePath mp;
    uint64_t now = NowMs();
    mp.id = c.id; mp.nowMs = Ms32(now); mp.startMs = Ms32(now); mp.speed = c.mv.speed; mp.run = c.mv.run ? 1 : 0;
    mp.pts.push_back({c.fx, c.fy});
    for (size_t i = c.mv.seg + 1; i < c.mv.pts.size(); ++i) mp.pts.push_back({c.mv.pts[i].x, c.mv.pts[i].y});
    ByteBuffer buf = gnet::PackG(mp);
    if (onlyTo) { if (onlyTo->caps & gnet::kCapContinuousMove) Send(*onlyTo, buf); return; }
    for (Critter* p : m->critters)
        if (p->IsPlayer() && (p->caps & gnet::kCapContinuousMove) && (p == &c || p->visCritters.count(c.id))) Send(*p, buf);
}

// A client announced continuous-move support after login: give it exact positions and running paths of what it already sees.
void Server::SendCapsResync(Critter& player) {
    Map* m = world_.GetMap(player.mapId);
    if (!m) return;
    SendCritterStop(player, 3, &player);
    if (player.mv.active) SendMovePath(player, &player);
    for (Critter* c : m->critters) {
        if (c == &player || !player.visCritters.count(c->id)) continue;
        SendCritterStop(*c, 3, &player);
        if (c->mv.active) SendMovePath(*c, &player);
    }
}

void Server::StopMove(Critter& c, uint8_t reason) {
    if (!c.mv.active) return;
    c.mv.active = false;
    c.mv.pts.clear();
    SendCritterStop(c, reason);
}

bool Server::MoveCritterTo(Critter& c, TilePos target, int run) {
    bool r = run < 0 ? DefaultRun(c) : run != 0;
    return StartMove(c, target.x + 0.5f, target.y + 0.5f, r, false);
}

bool Server::StartMove(Critter& c, float tx, float ty, bool run, bool exact) {
    Map* m = world_.GetMap(c.mapId);
    if (!m || c.dead || c.isStatic) return false;
    if (!std::isfinite(tx) || !std::isfinite(ty)) return false;
    tx = std::clamp(tx, 0.0f, static_cast<float>(m->width) - 0.001f);
    ty = std::clamp(ty, 0.0f, static_cast<float>(m->height) - 0.001f);
    TilePos target{static_cast<int16_t>(std::floor(tx)), static_cast<int16_t>(std::floor(ty))};
    if (m->IsBlocked(target.x, target.y) && !DoorPassable(c, *m, target.y * m->width + target.x)) return false;
    if (run && !rt_.RunOnCombat && NowMs() < c.battleEndMs) run = false;

    // closed doors this critter may operate (unlocked, or it carries the key) do not stop the pathfinder: it opens them on the way
    // (OpenDoorsAhead); locked, jammed and scenery doors block like walls
    std::function<bool(int)> doorPass = [&](int cell) { return DoorPassable(c, *m, cell); };
    std::deque<TilePos> cells;
    if (!FindPath(*m, c.pos, target, cells, 120000, &doorPass)) return false;
    Critter* occupant = m->CritterAt(target);
    bool occupied = occupant && occupant != &c;
    if (occupied) {
        if (cells.empty()) return false;
        cells.pop_back();  // stop next to it
        exact = false;
    }

    std::vector<Critter::Pt> pts;
    pts.push_back({c.fx, c.fy});
    for (TilePos t : cells) pts.push_back({t.x + 0.5f, t.y + 0.5f});
    if (!occupied) {
        // final point: the exact click point if the last hop stays on free cells, else the cell centre
        Critter::Pt last = pts.back();
        if (exact && SegmentClear(*m, last.x, last.y, tx, ty, 0)) pts.push_back({tx, ty});
        else if (cells.empty()) pts.push_back({target.x + 0.5f, target.y + 0.5f});
    }
    float r = static_cast<float>(std::clamp(rt_.MoveClearance, 0.0, 0.45));
    std::vector<Critter::Pt> sm = Smooth(*m, pts, r);
    if (sm.size() > kMaxPathPts) sm.resize(kMaxPathPts);
    std::vector<Critter::Pt> fin;  // drop zero-length hops
    for (auto& p : sm)
        if (fin.empty() || std::fabs(p.x - fin.back().x) > 1e-3f || std::fabs(p.y - fin.back().y) > 1e-3f) fin.push_back(p);
    if (fin.size() < 2) return false;

    c.mv.active = true;
    c.mv.pts = std::move(fin);
    c.mv.seg = 0;
    c.mv.segDone = 0;
    c.mv.lastMs = NowMs();
    c.mv.run = run;
    c.mv.speed = static_cast<float>(run ? rt_.RunSpeed : rt_.WalkSpeed);
    c.dir = DirFromVec(c.mv.pts[1].x - c.mv.pts[0].x, c.mv.pts[1].y - c.mv.pts[0].y, c.dir);
    m->dirty = true;
    SendMovePath(c);
    OpenDoorsAhead(c, *m);
    return true;
}

// Integrates the polyline up to `now`, checking every cell entered.
void Server::AdvanceMove(Critter& c, uint64_t now) {
    if (!c.mv.active) return;
    Map* m = world_.GetMap(c.mapId);
    if (!m) { c.mv.active = false; return; }
    if (now <= c.mv.lastMs) return;
    float dist = c.mv.speed * static_cast<float>(now - c.mv.lastMs) / 1000.0f;
    c.mv.lastMs = now;
    CritterId id = c.id;
    int guard = 0;
    while (dist > 1e-5f && c.mv.active && guard++ < 4000) {
        auto& mv = c.mv;
        Critter::Pt a = mv.pts[mv.seg], b = mv.pts[mv.seg + 1];
        float dx = b.x - a.x, dy = b.y - a.y;
        float seglen = std::sqrt(dx * dx + dy * dy);
        float left = seglen - mv.segDone;
        bool segFinished = left <= 1e-5f;
        if (!segFinished) {
            float step = std::min({dist, left, 0.5f});
            float t = (mv.segDone + step) / seglen;
            float nx = a.x + dx * t, ny = a.y + dy * t;
            int ncx = static_cast<int>(std::floor(nx)), ncy = static_cast<int>(std::floor(ny));
            if (ncx != c.pos.x || ncy != c.pos.y) {
                // a closed door in the way is operated first (opened when the critter may, else it blocks like a wall)
                if (m->IsBlocked(ncx, ncy)) OpenDoorAt(c, *m, ncx, ncy, true);
                if (ncx != c.pos.x && ncy != c.pos.y) { OpenDoorAt(c, *m, ncx, c.pos.y, false); OpenDoorAt(c, *m, c.pos.x, ncy, false); }
                bool ok = !m->IsBlocked(ncx, ncy);
                if (ok && ncx != c.pos.x && ncy != c.pos.y && (m->IsBlocked(ncx, c.pos.y) || m->IsBlocked(c.pos.x, ncy))) ok = false;
                if (ok) {
                    Critter* occ = m->CritterAt(TilePos{static_cast<int16_t>(ncx), static_cast<int16_t>(ncy)});
                    if (occ && occ != &c) ok = false;
                }
                if (ok) {
                    int cost = mv.run ? rt_.RtApCostCritterRun : rt_.RtApCostCritterWalk;
                    if (cost > 0 && !SpendAp(c, cost)) ok = false;
                }
                if (!ok) { StopMove(c, 1); return; }
                TilePos from = c.pos;
                c.pos = TilePos{static_cast<int16_t>(ncx), static_cast<int16_t>(ncy)};
                c.fx = nx; c.fy = ny;
                m->dirty = true;
                SendLegacyMove(c, 0);
                OpenDoorsAhead(c, *m);   // doors a few cells ahead start opening while we walk up to them
                CheckRegions(c);  // may move the player to another map
                Critter* cc = world_.GetCritter(id);
                if (!cc || cc->mapId != m->id) return;  // transferred: Teleport reset the movement
                if (scripting_) scripting_->OnCritterMove(*cc, from);
                cc = world_.GetCritter(id);
                if (!cc || cc->mapId != m->id || !cc->mv.active) return;
            } else {
                c.fx = nx; c.fy = ny;
            }
            mv.segDone += step;
            dist -= step;
            segFinished = mv.segDone >= seglen - 1e-5f;
        }
        if (!segFinished) continue;
        mv.seg++;
        mv.segDone = 0;
        if (mv.seg + 1 >= mv.pts.size()) {
            c.fx = mv.pts.back().x; c.fy = mv.pts.back().y;
            c.pos = TilePos{static_cast<int16_t>(std::floor(c.fx)), static_cast<int16_t>(std::floor(c.fy))};
            mv.active = false;
            mv.pts.clear();
            SendCritterStop(c, 0);
            return;
        }
        c.dir = DirFromVec(mv.pts[mv.seg + 1].x - mv.pts[mv.seg].x, mv.pts[mv.seg + 1].y - mv.pts[mv.seg].y, c.dir);
    }
}

}  // namespace f3
