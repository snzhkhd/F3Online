#pragma once
// Cell classes of the original .mapdata grid (docs/formats/maps.md section 2) and the collision rule derived from them.
// Shared by the server (walkability), the client map viewer and the tools.
#include <cstdint>

namespace f3::gd {

constexpr uint8_t kCellWall = 0x2f;       // wall / void: always solid
constexpr uint8_t kCellProp = 0x2c;       // footprint of a static scenery prop (tables, shelves, vehicles): solid
constexpr uint8_t kCellDoor = 0x1b;       // doorway of a door entity: solid only while the door is closed AND locked (server rule)

// Classes that sit exclusively on wall / fence geometry (their share of wall-footprint cells is 20..70 %, of all cells < 1 %):
// fence, rail and low-wall variants. Treated as solid. 0x16 (cave rim, 7 %) and 0x26 (door frame posts, 16 %) are not: critters
// and entry points stand on 0x26, and 0x16 is too common in caves to be a wall.
inline bool CellSolid(uint8_t v) {
    switch (v) {
        case kCellWall: case kCellProp:
        case 0x1f: case 0x2a: case 0x21: case 0x1a: case 0x17: case 0x0c: case 0x22:
            return true;
        default: return false;
    }
}

}  // namespace f3::gd
