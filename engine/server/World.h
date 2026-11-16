#pragma once
// Server-side data model: Item, Critter, Map, World. No networking, no scripting.
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "common/types.h"

struct _ENetPeer;

namespace f3 {

// Item inside an inventory or container. pid indexes the DataStore prototypes.
struct InvItem {
    ItemId id = 0;
    uint16_t pid = 0;
    uint16_t count = 1;
    bool equipped = false;   // == (slot != 0)
    uint8_t slot = 0;        // EquipSlot (protocol_game.h): 0 none, 1 body, 2 head, 3 hand1 (active weapon), 4 hand2
};

enum class ObjKind : uint8_t { Ground = 0, Door = 1, Container = 2, Usable = 3 };

// Anything standing on the map that is not a critter: ground items, doors, containers, usables.
struct Item {
    ItemId id = 0;
    uint16_t pid = 0;
    std::string name;
    MapId mapId = 0;
    TilePos pos;
    ObjKind kind = ObjKind::Ground;
    uint16_t count = 1;
    // Door / container state (Doors.cpp). States: Open (open), Closed (!open && !locked), Locked (!open && locked).
    // A closed door blocks its footprint cells (pathing, movement, line of sight); critters that may operate it open it on the way.
    bool open = false;
    bool locked = false;               // locked: opens with the key (or a pick / script), keeps `open` false
    uint8_t lockFlags = 0;             // LockFlag bits (types.h)
    int lockDC = 0;                    // lock difficulty (original GDOR / GCON v1 lockDC); 0 = GameConfig::LockDefaultDC
    std::string keyId;                 // key item: prototype key without extension ("KeysTest") or full key; empty = no key
    std::vector<InvItem> contents;     // containers
    std::vector<TilePos> footprint;    // doors: the 0x1b doorway cells of the grid (blocked while the door is closed)
    std::string script;                // original AMX script name (informational / AMX host)
};

enum class PendingType : uint8_t { None, Pickup, Use, Take, Loot, Put };
struct Pending {
    PendingType type = PendingType::None;
    ItemId id = 0;       // map object
    ItemId sub = 0;      // inventory item inside the object (Take), 0 = all; Put: the item of the actor
    bool critter = false;  // `id` is a critter (corpse), not a map object
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

    // ---- gameplay state (see Game.cpp)
    int protoPid = -1;          // DataStore prototype, -1 = none (test content)
    int special[7] = {5, 5, 5, 5, 5, 5, 5};  // S P E C I A L
    int level = 1;
    int hp = 30, maxHp = 30;
    int team = 1;               // players are team 1
    bool dead = false;
    bool isStatic = false;      // props that are critter-class in the data (barrels, windows): never move
    uint64_t deadSince = 0;
    std::vector<InvItem> inventory;
    CritterId target = 0;       // combat target (0 = none)
    Pending pending;
    CritterId talkTarget = 0;   // NPC the player is walking to in order to talk (Server::TalkTo)
    // A walk request that arrived while the critter was busy (breaktime of an attack / item action): the auto attack is
    // cancelled at once and the walk starts the moment the critter is free (FOnline would drop it; clicking in the
    // middle of an attack animation must not make walking impossible).
    struct QueuedMove {
        bool has = false, f = false, run = false;   // f: MoveToF (float target), else a cell
        float x = 0, y = 0;
        uint64_t atMs = 0;
    } qmove;
    uint64_t regionLockUntilMs = 0;
    uint64_t nextRepathTick = 0;
    int inRegion = -1;          // index of the trigger region the player stands in
    std::string script, dialog; // original AMX script names (informational)
    std::string instName;       // instance name from the map data (the original entity displayName): key of the _DEAD_<name> death counter
    uint32_t attacksDone = 0;   // attacks performed by TickCombat (the AMX A_Attack action completes after one)

    // ---- continuous position + real-time state (Move.cpp, Timing.cpp). `pos` is always floor(fx), floor(fy).
    float fx = 0.5f, fy = 0.5f;  // position in cell units, cell (cx,cy) has its centre at (cx+0.5, cy+0.5)
    struct Pt { float x = 0, y = 0; };
    struct Mover {
        bool active = false;
        std::vector<Pt> pts;     // polyline, pts[0] = start
        size_t seg = 0;          // current segment pts[seg] -> pts[seg+1]
        float segDone = 0;       // distance travelled on the current segment (cells)
        uint64_t lastMs = 0;     // when fx/fy were last advanced
        float speed = 0;         // cells / s
        bool run = false;
    } mv;
    bool IsMoving() const { return mv.active; }
    void SnapToCell() { fx = pos.x + 0.5f; fy = pos.y + 0.5f; }
    uint64_t waitEndMs = 0;      // breaktime: actions rejected until this server time (ms)
    uint64_t battleEndMs = 0;    // battle flag (TimeoutBattle)
    int ap = -1;                 // 1/100 AP, -1 = full (initialised lazily by Server::SyncAp)
    uint64_t apRegenMs = 0;      // last regeneration timestamp
    uint64_t nextThinkMs = 0;    // cr:think_due
    uint32_t caps = 0;           // players: gnet::kCap* bits sent by the client
    int msgBudget = 10;         // request rate limiter (players)

    // Visibility state, what this (player) critter's client currently knows about.
    std::unordered_set<CritterId> visCritters;
    std::unordered_set<ItemId> visItems;

    std::unordered_map<std::string, double> vars;  // script-visible numeric state (persisted for accounts)
    std::unordered_map<std::string, double> gvars; // per player quest / journal / script globals (persisted; overlay on the shared globals table)
    // Pip-Boy (Pip.cpp): quest log (persisted in the character) and friends list (display names, persisted)
    struct QuestEntry { std::string id, title, text; uint8_t state = 0; int64_t updated = 0; };   // state 0 active, 1 completed, 2 failed
    std::vector<QuestEntry> quests;
    std::vector<std::string> friends;

    // ---- account / character progression (Persist.cpp, Character.cpp)
    std::string lang = "en";    // players: UI language the client announced at login (server messages are localized with it)
    std::string account;        // account name when logged in with an account (empty = guest, nothing is saved)
    int xp = 0;
    int skillPoints = 0;        // unspent skill points
    uint16_t tagMask = 0;       // tag skills (bit per kSkillNames index)
    uint16_t traitMask = 0;     // optional traits chosen at creation (bit per data/traits index 0..15); their effects come from the data layer
    int perkPoints = 0;         // earned every PerkRate levels, spent with /perk take <id> (GameContent.cpp)
    std::map<std::string, int> perks;   // perk id (data/perks) -> rank taken
    int maxHpFixed = 0;         // > 0: maximum HP fixed by the data layer (npc record max_hp)
    int unspentSpecial = 0;     // SPECIAL points still to allocate (character creation)
    bool created = true;        // false while the character still has to go through character creation
    std::map<std::string, int> skillRanks;  // points invested per skill (on top of the SPECIAL based base value)

    bool IsNpc() const { return kind == CritterKind::Npc; }
    bool IsPlayer() const { return kind == CritterKind::Player; }
};

struct EntrySlot { TilePos pos; float yaw = 0; };
struct EntryPoint { int id = 0; std::vector<EntrySlot> slots; };

// Region polygon in cell coordinates (world units * 2).
struct Region {
    int kind = 0;  // 0 transition, 1 trigger, 6 volume
    std::vector<std::pair<float, float>> poly;
    float minx = 0, miny = 0, maxx = 0, maxy = 0;
    std::string script, targetMap;
    int targetEntry = 0;
    bool Contains(float x, float y) const;  // x,y in cells
};

struct Map {
    MapId id = 0;
    std::string name;
    int width = 0, height = 0;
    float originX = 0, originZ = 0;  // world position of the min corner of cell (0,0) (original data maps only)
    std::vector<uint8_t> blocked;  // width*height
    std::vector<uint8_t> cls;      // raw mapdata class per cell (original data maps; empty on test maps)
    std::unordered_map<int, ItemId> doorCell;  // cell index -> door object owning that doorway cell
    TilePos spawn;
    std::vector<EntryPoint> entries;
    std::vector<Region> regions;
    std::vector<std::vector<TilePos>> paths;  // waypoint paths (cells)
    std::vector<std::string> pathNames;       // parallel to paths: EPTH name (GetWayPointCount / GetWayPointPosition / GetNearestPath)
    std::vector<std::vector<std::pair<float, float>>> pathsWorld;  // parallel to paths: waypoints as original world (x, z)
    const EntryPoint* FindEntry(int id) const;
    std::vector<Critter*> critters;
    std::unordered_map<ItemId, Item> items;
    bool dirty = true;  // visibility needs recompute
    bool objDirty = false;  // door / container state changed since the last world-state save (Doors.cpp)

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
// `passable(cellIndex)` (optional) lets blocked cells through, used for closed doors the walker may open (cell index = y * width + x).
bool FindPath(const Map& map, TilePos from, TilePos to, std::deque<TilePos>& out, int maxNodes = 120000,
              const std::function<bool(int)>* passable = nullptr);

}  // namespace f3
