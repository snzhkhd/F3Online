#pragma once
// Server-side data model: Item, Critter, Map, World. No networking, no scripting.
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "common/types.h"

struct _ENetPeer;

namespace f3 {

struct Item {
    ItemId id = 0;
    uint16_t pid = 0;
    std::string name;
    MapId mapId = 0;
    TilePos pos;
};

struct Critter {
    CritterId id = 0;
    std::string name;
    CritterKind kind = CritterKind::Player;
    MapId mapId = 0;
    TilePos pos;
    Dir dir = Dir::S;
    uint32_t color = 0xFFFFFFFF;

    _ENetPeer* peer = nullptr;  // players only
    std::string npcType;        // npcs only: key in Lua npc_types

    std::deque<TilePos> path;
    int moveCooldown = 0;       // ticks until next step is allowed
    int msgBudget = 10;         // request rate limiter (players)

    // Visibility state, what this (player) critter's client currently knows about.
    std::unordered_set<CritterId> visCritters;
    std::unordered_set<ItemId> visItems;

    std::unordered_map<std::string, double> vars;  // script-visible numeric state

    bool IsNpc() const { return kind == CritterKind::Npc; }
    bool IsPlayer() const { return kind == CritterKind::Player; }
};

struct Map {
    MapId id = 0;
    std::string name;
    int width = 0, height = 0;
    std::vector<uint8_t> blocked;  // width*height
    TilePos spawn;
    std::vector<Critter*> critters;
    std::unordered_map<ItemId, Item> items;
    bool dirty = true;  // visibility needs recompute

    bool InBounds(int x, int y) const { return x >= 0 && y >= 0 && x < width && y < height; }
    bool IsBlocked(int x, int y) const { return !InBounds(x, y) || blocked[static_cast<size_t>(y) * width + x] != 0; }
    Critter* CritterAt(TilePos p) const;
    bool IsFree(TilePos p) const { return !IsBlocked(p.x, p.y) && !CritterAt(p); }
    // Nearest free tile (spiral search) around p; returns false if none found within radius.
    bool FindFreeNear(TilePos p, int radius, TilePos& out) const;
};

class World {
public:
    Map* CreateMap(const std::string& name, int w, int h, std::vector<uint8_t> blocked, TilePos spawn);
    Map* GetMap(MapId id);
    Map* FindMapByName(const std::string& name);
    Map* FirstMap();

    Critter* CreateCritter(const std::string& name, CritterKind kind);  // not placed on a map yet
    Critter* GetCritter(CritterId id);
    Critter* FindPlayerByName(const std::string& name);
    void DestroyCritter(CritterId id);

    ItemId NextItemId() { return nextItemId_++; }

    std::unordered_map<CritterId, std::unique_ptr<Critter>>& Critters() { return critters_; }
    std::vector<std::unique_ptr<Map>>& Maps() { return maps_; }

private:
    std::vector<std::unique_ptr<Map>> maps_;
    std::unordered_map<CritterId, std::unique_ptr<Critter>> critters_;
    CritterId nextCritterId_ = 1;
    ItemId nextItemId_ = 1;
};

// A* on the 8-connected grid; diagonal steps may not cut blocked corners. Critters are
// ignored (they are handled at step time). Returns false if unreachable or too far.
bool FindPath(const Map& map, TilePos from, TilePos to, std::deque<TilePos>& out, int maxNodes = 8192);

}  // namespace f3
