#pragma once
// Shared basic types: ids, tile coordinates, directions.
// MVP uses a square tile grid with 8 directions. The original game (and FOnline)
// use hex maps; TilePos/Dir are the only place that needs to change for that
// (neighbour offsets, distance, pathfinding step set).
#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace f3 {

using CritterId = uint32_t;
using ItemId = uint32_t;
using MapId = uint16_t;

struct TilePos {
    int16_t x = 0, y = 0;
    bool operator==(const TilePos& o) const { return x == o.x && y == o.y; }
    bool operator!=(const TilePos& o) const { return !(*this == o); }
};

enum class Dir : uint8_t { N = 0, NE, E, SE, S, SW, W, NW };

inline constexpr int kDirDx[8] = {0, 1, 1, 1, 0, -1, -1, -1};
inline constexpr int kDirDy[8] = {-1, -1, 0, 1, 1, 1, 0, -1};

// Chebyshev distance (8-connected grid).
inline int TileDist(TilePos a, TilePos b) {
    return std::max(std::abs(a.x - b.x), std::abs(a.y - b.y));
}

inline Dir DirFromDelta(int dx, int dy, Dir fallback = Dir::S) {
    dx = (dx > 0) - (dx < 0);
    dy = (dy > 0) - (dy < 0);
    for (int i = 0; i < 8; ++i)
        if (kDirDx[i] == dx && kDirDy[i] == dy) return static_cast<Dir>(i);
    return fallback;
}

// Lock flags of doors and containers (Item::lockFlags, ObjectInfo.flags). Original GDOR / GCON only carry locked, key and lock DC;
// the flags are F3Online additions (docs/research/doors.md).
enum LockFlag : uint8_t {
    kLockNoOpen = 1,    // scenery: cannot be opened at all (not even with a key or by picking)
    kLockNoPick = 2,    // the lock cannot be picked (Security skill) but a key / script still opens it
    kLockJammed = 4,    // stuck: refuses to open until a script clears the flag
    kLockKeyOnly = 8,   // a key is required: implies NoPick
};

enum class CritterKind : uint8_t { Player = 0, Npc = 1 };

}  // namespace f3
