#pragma once
// Server-side game data: prototypes and map definitions produced by tools/dataimport
// (gamedata/server). Pure data, no gameplay logic. See docs/server-data.md.
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace f3 {

enum class PClass : uint8_t { Critter, Weapon, Armor, Ammo, Item, Door, Container, Usable };

struct AttackModeDef {
    int type = 0;  // 2 = aimed/single shot, 1 = burst, 6/8/9 = melee/unarmed styles (observed, semantics guessed)
    int ap = 10;   // raw "cost" word; the server converts it to a cooldown (see Game.cpp)
    int dmgMin = 0, dmgMax = 0;
};

struct Proto {
    uint16_t pid = 0;  // index in key-sorted order of protos.json (stable, shared with clients)
    std::string key;   // "<template>.<TYPE>"
    PClass cls = PClass::Item;
    std::string model, name, desc, name2, script, dialog;
    int nameId = 0, descId = 0;
    int team = 0;
    bool isStatic = false;  // barrels, windows (critter-class props)
    // doors / containers: GDOR / GCON game record = u8 open, u8 locked, u16 len + key resource, i32 lockDC, i32 randomRange (+ GCON string)
    bool doorLocked = false, doorOpen = false;
    int lockDC = 0, lockRange = 0;
    std::string keyId;
    std::vector<std::string> inventory;  // EEOV default inventory
    // critter
    int special[7] = {5, 5, 5, 5, 5, 5, 5};
    std::vector<std::pair<int, int>> derived;  // raw derived-stat overrides (idx, value)
    std::vector<AttackModeDef> modes;
    std::vector<std::string> equipment;
    // weapon
    int weaponClass = 0, caliber = -1, range = 1;
    float wDmgMin = 0, wDmgMax = 0;
    // armor (7 raw words, treated as per-damage-type threshold, see Game.cpp)
    int armor[7] = {0, 0, 0, 0, 0, 0, 0};
    // ammo
    int ammoCaliber = -1, ammoDmgMin = 0, ammoDmgMax = 0;
    int weight = 1;
    bool headArmor = false;  // armor worn on the head (helmets) instead of the body
    // ---- data layer (data/*.json, Content.cpp); defaults keep the behaviour of the plain original prototype
    int weightOverride = -1;   // >= 0: unit weight from data/items (wins over config.lua ItemWeights)
    int level = 1;             // critters: starting level (HP formula)
    int maxHpFixed = 0;        // critters: > 0 fixes the maximum HP
    int xp = -1;               // critters: >= 0 experience for killing it (replaces XpByProto / the formula)
    bool fromData = false;     // record added by data/ (no original prototype)
    bool retired = false;      // record removed by a later reload; the pid stays valid for existing instances

    const std::string& DisplayName() const { return !name.empty() ? name : (!name2.empty() ? name2 : key); }
    // Name for people: the English name, else the key made readable ("Critters00_TMCommunist_Insurgent_Shiv.CRT" ->
    // "TMCommunist Insurgent Shiv"). Never used as a lookup key (the client maps critter names back to prototypes).
    std::string ReadableName() const {
        if (!name.empty()) return name;
        if (!name2.empty()) return name2;
        std::string s = key;
        const size_t d = s.rfind('.');
        if (d != std::string::npos && s.size() - d <= 5) s.resize(d);
        size_t u = s.find('_');
        if (u != std::string::npos && u > 0 && u < 14 && s.compare(0, 8, "Critters") == 0) s.erase(0, u + 1);
        for (char& ch : s) if (ch == '_') ch = ' ';
        return s;
    }
};

struct EntryPointDef {
    struct Slot { float x = 0, z = 0, yaw = 0; };
    int id = 0;
    std::vector<Slot> slots;
};

struct RegionDef {
    int kind = 0;  // 0 transition, 1 script trigger, 6 volume
    std::vector<std::pair<float, float>> pts;  // world units (x, z)
    float y = 0;
    std::string script, targetMap;
    int targetEntry = 0;
};

struct EntityDef {
    std::string name, proto, type, script, dialog, skin;
    float x = 0, y = 0, z = 0, yaw = 0;
    int team = 0;
    std::vector<std::string> inventory;
};

struct MapDef {
    std::string name;
    int width = 0, height = 0;  // cells, 2 per world unit
    float originX = 0, originZ = 0;  // world position of the min corner of cell (0,0); cell = floor(2 * (world - origin))
    std::vector<uint8_t> walk;  // raw mapdata class byte per cell, 0x2f = blocked
    std::vector<EntryPointDef> entries;
    std::vector<RegionDef> regions;
    std::vector<EntityDef> entities;
    std::vector<std::vector<std::pair<float, float>>> paths;
    std::vector<std::string> pathNames;  // parallel to paths (EPTH name)
};

struct ProtoSet;
struct ContentReport;

class DataStore {
public:
    // dir = gamedata/server. Returns false (with err) if manifest or protos are missing/invalid.
    bool Load(const std::filesystem::path& dir, std::string& err);
    bool Loaded() const { return loaded_; }

    const std::vector<std::string>& MapNames() const { return mapNames_; }
    bool LoadMap(const std::string& name, MapDef& out, std::string& err) const;

    const Proto* FindProto(const std::string& key) const;  // case-insensitive
    const Proto* GetProto(uint16_t pid) const { return pid < protos_.size() ? &protos_[pid] : nullptr; }
    size_t ProtoCount() const { return protos_.size(); }
    // data layer: the original prototypes (base) stay untouched; the effective table is base + data/ records (Content.cpp)
    const std::deque<Proto>& BaseProtos() const { return base_; }
    const std::deque<Proto>& AllProtos() const { return protos_; }
    void Commit(ProtoSet&& set);   // makes a candidate table (ContentDb::protoSet) the effective one; addresses of existing entries stay valid

    const std::string& StartMap() const { return startMap_; }
    int StartEntry() const { return startEntry_; }
    std::string String(int id) const;  // English.stf string by 1-based id, "" if unknown
    const std::map<std::string, double>& Globals() const { return globals_; }
    const std::filesystem::path& Dir() const { return dir_; }

    static std::string Lower(std::string s);

private:
    bool loaded_ = false;
    std::filesystem::path dir_;
    std::vector<std::string> mapNames_;
    std::deque<Proto> protos_;   // effective table; base entries first (same pids as ever), data records appended
    std::deque<Proto> base_;     // pristine copy of the original prototypes
    std::unordered_map<std::string, uint16_t> byKey_;  // lowercase key
    std::unordered_map<int, std::string> strings_;
    std::map<std::string, double> globals_;
    std::string startMap_;
    int startEntry_ = 0;
};

}  // namespace f3
