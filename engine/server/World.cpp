#include "World.h"

#include <algorithm>
#include <cmath>
#include <queue>

namespace f3 {

Critter* Map::CritterAt(TilePos p) const {
    for (Critter* c : critters)
        if (c->pos == p) return c;
    return nullptr;
}

bool Map::FindFreeNear(TilePos p, int radius, TilePos& out) const {
    for (int r = 0; r <= radius; ++r)
        for (int dy = -r; dy <= r; ++dy)
            for (int dx = -r; dx <= r; ++dx) {
                if (std::max(std::abs(dx), std::abs(dy)) != r) continue;
                TilePos t{static_cast<int16_t>(p.x + dx), static_cast<int16_t>(p.y + dy)};
                if (IsFree(t)) { out = t; return true; }
            }
    return false;
}

Map* World::CreateMap(const std::string& name, int w, int h, std::vector<uint8_t> blocked, TilePos spawn) {
    auto m = std::make_unique<Map>();
    m->id = static_cast<MapId>(maps_.size() + 1);
    m->name = name;
    m->width = w;
    m->height = h;
    m->blocked = std::move(blocked);
    m->blocked.resize(static_cast<size_t>(w) * h, 0);
    m->spawn = spawn;
    maps_.push_back(std::move(m));
    return maps_.back().get();
}

Map* World::GetMap(MapId id) {
    return (id >= 1 && id <= maps_.size()) ? maps_[id - 1].get() : nullptr;
}

Map* World::FindMapByName(const std::string& name) {
    for (auto& m : maps_)
        if (m->name == name) return m.get();
    return nullptr;
}

Map* World::FirstMap() { return maps_.empty() ? nullptr : maps_.front().get(); }

Critter* World::CreateCritter(const std::string& name, CritterKind kind) {
    auto c = std::make_unique<Critter>();
    c->id = nextCritterId_++;
    c->name = name;
    c->kind = kind;
    Critter* p = c.get();
    critters_[p->id] = std::move(c);
    return p;
}

Critter* World::GetCritter(CritterId id) {
    auto it = critters_.find(id);
    return it == critters_.end() ? nullptr : it->second.get();
}

Critter* World::FindPlayerByName(const std::string& name) {
    auto lower = [](std::string s) {
        for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return s;
    };
    std::string n = lower(name);
    for (auto& [id, c] : critters_)
        if (c->IsPlayer() && lower(c->name) == n) return c.get();
    return nullptr;
}

void World::DestroyCritter(CritterId id) { critters_.erase(id); }

bool FindPath(const Map& map, TilePos from, TilePos to, std::deque<TilePos>& out, int maxNodes) {
    out.clear();
    if (from == to) return true;
    if (map.IsBlocked(to.x, to.y) || map.IsBlocked(from.x, from.y)) return false;

    const int W = map.width;
    auto idx = [W](int x, int y) { return y * W + x; };
    const size_t N = static_cast<size_t>(map.width) * map.height;
    std::vector<float> g(N, 1e30f);
    std::vector<int> parent(N, -1);
    std::vector<uint8_t> closed(N, 0);

    struct Node { float f; int i; };
    auto cmp = [](const Node& a, const Node& b) { return a.f > b.f; };
    std::priority_queue<Node, std::vector<Node>, decltype(cmp)> open(cmp);

    auto h = [&](int x, int y) {
        float dx = static_cast<float>(std::abs(x - to.x)), dy = static_cast<float>(std::abs(y - to.y));
        return (dx + dy) + (1.41421356f - 2.0f) * std::min(dx, dy);  // octile
    };

    g[idx(from.x, from.y)] = 0;
    open.push({h(from.x, from.y), idx(from.x, from.y)});
    int expanded = 0;
    const int goal = idx(to.x, to.y);

    while (!open.empty()) {
        Node n = open.top();
        open.pop();
        if (closed[n.i]) continue;
        closed[n.i] = 1;
        if (n.i == goal) break;
        if (++expanded > maxNodes) return false;
        int cx = n.i % W, cy = n.i / W;
        for (int d = 0; d < 8; ++d) {
            int nx = cx + kDirDx[d], ny = cy + kDirDy[d];
            if (map.IsBlocked(nx, ny)) continue;
            bool diag = kDirDx[d] != 0 && kDirDy[d] != 0;
            if (diag && (map.IsBlocked(cx + kDirDx[d], cy) || map.IsBlocked(cx, cy + kDirDy[d]))) continue;
            int ni = idx(nx, ny);
            if (closed[ni]) continue;
            float ng = g[n.i] + (diag ? 1.41421356f : 1.0f);
            if (ng < g[ni]) {
                g[ni] = ng;
                parent[ni] = n.i;
                open.push({ng + h(nx, ny), ni});
            }
        }
    }
    if (parent[goal] < 0) return false;
    for (int i = goal; i != idx(from.x, from.y); i = parent[i])
        out.push_front(TilePos{static_cast<int16_t>(i % W), static_cast<int16_t>(i / W)});
    return true;
}

}  // namespace f3
