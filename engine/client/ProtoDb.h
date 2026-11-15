#pragma once
// Client-side view of gamedata/server/protos.json (written by f3_dataimport; see docs/server-data.md).
// `pid` values on the wire (ItemAdd.pid, InvEntry.pid, CritterProto.pid) are indices into the KEY-SORTED list, which is
// exactly the iteration order of a JSON object here (std::map). STL + nlohmann only, no raylib.
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace f3 {

// One appearance attachment of a critter: b3d part ("BodPrisonSuit" -> <model>_BodPrisonSuit.b3d) and an optional texture
// override ("BacPrisonSuit_13_LG.dds"; empty = the part's own default texture).
struct WornPart {
    std::string part, tex;
};

struct ProtoInfo {
    std::string key;     // "Critters00_TMBobby.CRT"
    std::string cls;     // critter weapon armor ammo item door container usable
    std::string model;   // b3d resource name ("HumMNor"), may be empty (items: the GROUND model, GD_WP_*)
    std::string heldModel;   // weapons: b3d drawn in the hand (WP_*, authored in hand space, from the item record), may be empty
    std::string name;    // English display name
    std::string skin;    // texture override for "Skin_*" parts, may be empty
    std::vector<WornPart> worn;  // critters: appearance sockets (hair, beard, ...) plus the parts of equipped armor
    bool hasDialog = false;  // critters: the prototype names a dialogue
    bool isStatic = false;  // critter-class props (barrels, windows): drawn by the map, never animated
    // --- inventory UI block: item data for the original-art inventory / action bar ---
    std::string icon, iconAct;   // "BodVaultsuit_INV.dds", "WP_..._ACT.dds" (names of Interface TGAs, extension free), may be empty
    std::string desc;            // English description, may be empty
    bool headArmor = false;      // armor: helmet (head slot) instead of body
    int range = 0, weaponClass = 0;
    float dmgMin = 0, dmgMax = 0;
    struct Mode { int type = 0, ap = 0; };
    std::vector<Mode> modes;     // weapon attack modes (type 1 burst, 2 single, 6 thrust, 8 swing, 9 throw/other: guess)
    int armor[7] = {0, 0, 0, 0, 0, 0, 0};
    std::vector<WornPart> armorParts;   // armor: b3d parts it adds to the wearer (Bod/Fee/Bac/Han/Sho/Hea...)
    bool isCritter() const { return cls == "critter"; }
};

class ProtoDb {
public:
    // Looks for protos.json: explicit path or directory, else gamedata/server relative to the cwd / exe, walking up.
    bool Load(const std::string& hint, std::string* err = nullptr);
    bool loaded() const { return !protos_.empty(); }
    const std::string& path() const { return path_; }
    const ProtoInfo* ByPid(uint32_t pid) const { return pid < protos_.size() ? &protos_[pid] : nullptr; }
    const ProtoInfo* ByKey(const std::string& key) const;  // case-insensitive
    // Display name -> critter prototype (most frequent model wins among prototypes of that name; players -> Default_PC).
    const ProtoInfo* CritterByName(const std::string& name) const;
    const ProtoInfo* PlayerProto() const { return ByKey("Default_PC.CRT"); }
    int PidOf(const ProtoInfo* p) const { return p ? static_cast<int>(p - protos_.data()) : -1; }
    size_t size() const { return protos_.size(); }

    static std::string FindFile(const std::string& hint);

private:
    std::string path_;
    std::vector<ProtoInfo> protos_;
    std::unordered_map<std::string, uint32_t> byKey_;
    std::unordered_map<std::string, uint32_t> byName_;
};

}  // namespace f3
