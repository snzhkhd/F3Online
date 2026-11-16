#pragma once
// Data-driven content layer: designer-authored JSON files in data/ (traits, perks, skills, items, npcs, quests).
// The original prototypes extracted from the game (DataStore, gamedata/server) are the BASE layer; the records here override
// fields of a base record (merge by id) or add new records. Strict validation, see docs/data-format.md.
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "DataStore.h"

namespace f3 {

inline constexpr int kContentSchema = 1;   // schema version understood by this build (files carry "schema": N)
inline constexpr int kSpecialCount = 7;
inline constexpr int kSkillSlots = 14;     // == kSkillCount of protocol_game.h

struct ContentReport {
    std::vector<std::string> errors, warnings;
    bool Ok() const { return errors.empty(); }
    void Error(const std::string& file, const std::string& path, const std::string& msg) { errors.push_back(file + ": " + path + ": " + msg); }
    void Warn(const std::string& file, const std::string& path, const std::string& msg) { warnings.push_back(file + ": " + path + ": " + msg); }
};

// ---- stat modifiers (perk / trait effects). Ids: 0..N named stats, kStatSkillBase + skill index.
enum StatId : int { StMaxHp = 0, StMaxAp, StCarryWeight, StArmorClass, StSequence, StCritChance, StMeleeDamage, StXpGain, StSkillRate, StHealRate, StNamedCount };
inline constexpr int kStatSkillBase = 100;
int StatFromName(const std::string& name);   // "max_hp", "skill.Firearms" (case-insensitive) -> id, -1 unknown
std::string StatName(int id);
const std::vector<std::string>& StatNames();  // all valid stat names (for messages / docs)

struct StatMod { int stat = 0; int add = 0; };

struct TraitDef {
    std::string id, name, desc, hook, file;
    int index = 0;                 // bit in the character's trait mask (0..15), also the creation screen slot
    std::vector<StatMod> effects;
};

struct PerkReq {
    int level = 0;
    int special[kSpecialCount] = {0, 0, 0, 0, 0, 0, 0};
    std::vector<std::pair<int, int>> skills;   // skill index, minimum value
    std::vector<std::string> perks;            // perk ids that must be held
    std::vector<std::string> traits;           // trait ids that must be chosen
};

struct PerkDef {
    std::string id, name, desc, hook, file;
    int ranks = 1;
    PerkReq req;
    std::vector<StatMod> effects;  // per rank
};

struct QuestDef {
    std::string id, title, text, file;
    int xp = 0;
    std::vector<std::pair<std::string, int>> items;   // reward: proto key, count
};

struct SkillDef {   // base value = constant + sum(coef[i] * SPECIAL[i]) (S P E C I A L)
    int constant = 0;
    int coef[kSpecialCount] = {0, 0, 0, 0, 0, 0, 0};
    std::string desc;
};

// ---- records of the layered kinds (applied on top of the base prototypes by DataStore::BuildProtoSet)
struct ItemRec {
    std::string file, path, id, base;
    std::optional<std::string> name, desc;
    std::optional<int> weight;
    std::optional<bool> headArmor;
    std::optional<int> range;
    std::optional<float> dmgMin, dmgMax;
    std::optional<std::vector<int>> armor;       // 7 values
    std::optional<int> ammoDmgMin, ammoDmgMax;
};

struct NpcSpawn { std::string map; int x = 0, y = 0; int count = 1; };

struct NpcRec {
    std::string file, path, id, base;
    std::optional<std::string> name, script, dialog;
    std::optional<int> team, level, maxHp, xp;
    int special[kSpecialCount] = {0, 0, 0, 0, 0, 0, 0};   // 0 = not set
    std::optional<std::vector<std::string>> equipment, inventory;
    std::vector<NpcSpawn> spawns;
};

// The effective prototype table (base + data records). New prototypes get pids after the base ones (pid >= base size) and
// keep them for the life of the process, see DataStore::BuildProtoSet.
struct ProtoSet {
    std::deque<Proto> protos;
    std::unordered_map<std::string, uint16_t> byKey;   // lowercase key -> pid (retired entries excluded)
};

class ContentDb {
public:
    ContentDb();

    // Reads and validates <dir>/manifest.json and the kind folders. `base` may be null (no gamedata): item / npc records are then
    // only checked structurally and not applied. With `base` the candidate prototype table is built into out.protoSet (commit with
    // DataStore::Commit). Returns false when there are errors and !lenient; with lenient, records with errors are dropped and
    // reported as warnings. `lenient` records that fail validation are listed in the warnings prefixed "dropped".
    static bool Build(const std::filesystem::path& dir, const DataStore* base, bool lenient, ContentDb& out, ContentReport& rep);

    int schema = kContentSchema;
    std::filesystem::path dir;
    bool loaded = false;                       // false: no data/ folder (built-in defaults only)
    std::vector<TraitDef> traits;
    std::vector<PerkDef> perks;
    std::vector<QuestDef> quests;
    std::vector<ItemRec> items;
    std::vector<NpcRec> npcs;
    SkillDef skills[kSkillSlots];              // built-in defaults, overridden by data/skills
    bool hasTraitMax = false;
    int traitMax = 2;
    ProtoSet protoSet;                         // only filled by Build with a base
    bool protosBuilt = false;
    std::map<std::string, std::string> fingerprint;   // "kind:id" -> canonical record dump (diff for /reload)
    std::vector<std::string> files;            // relative paths of every file read (sorted)

    const TraitDef* FindTrait(const std::string& id) const;
    const TraitDef* TraitByIndex(int index) const;
    const PerkDef* FindPerk(const std::string& id) const;
    const QuestDef* FindQuest(const std::string& id) const;
    const NpcRec* FindNpc(const std::string& protoKey) const;

    // "perks: +1 -0 ~2 (added: a; changed: b, c)" lines for kinds that differ, empty when nothing changed.
    static std::vector<std::string> Diff(const ContentDb& before, const ContentDb& after);
    std::string Summary() const;               // "6 traits, 8 perks, ..."
};

// f3_server --validate-data: loads gamedata (when given) and the content folder, prints the report, returns the exit code.
int RunValidateData(const std::filesystem::path& gamedataDir, const std::filesystem::path& contentDir, bool lenient);

std::filesystem::path FindContentDir(const std::filesystem::path& exePath);

}  // namespace f3
