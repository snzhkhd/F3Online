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
const std::vector<std::string>& SpecialKeyNames();   // strength .. luck, the keys of every SPECIAL block in data/ (editor schema)
const std::vector<std::string>& DialogStatNames();   // the names a dialog condition `stat:<name>` accepts (editor schema)

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
    bool placeholder = false;      // numbers are placeholders (the demo has no perk data); documentation only
    int ranks = 1;
    PerkReq req;
    std::vector<StatMod> effects;  // per rank
};

struct QuestDef {
    std::string id, title, text, file;
    int xp = 0;
    std::vector<std::pair<std::string, int>> items;   // reward: proto key, count
};

// ---- data dialogs (data/dialogs, docs/dialogs.md): graph of nodes with text, answers, conditions and effects
struct DlgCond {
    enum Kind { Skill, Stat, Item, Quest, Flag, Lua, All, Any, Not };
    enum Op { Ge, Le, Eq, Ne, Gt, Lt };
    Kind kind = All;
    std::string name;                // skill / stat / item key / quest id / flag name / Lua function
    Op op = Ge;
    int value = 0;                   // quest: -1 none, 0 active, 1 done, 2 failed
    bool neg = false;                // leading '!'
    std::vector<DlgCond> kids;       // All / Any / Not
};
struct DlgEffect {
    enum Kind { Give, Take, Quest, Flag, Xp, Trade, Attack, End, Door, Lua };
    Kind kind = End;
    std::string s;                   // item key / quest id / flag name / Lua function
    int n = 1;                       // count / flag value / xp
    int x = 0, y = 0;                // Door: cell on the speaker's map
    std::string action;              // Quest: add | done | fail | reopen; Door: open | close | lock | unlock
};
struct DlgAnswer {
    std::string id, text;            // text: our own English text; empty with stf != 0 = original English.stf string
    int stf = 0;
    bool hasCond = false;
    DlgCond cond;
    std::vector<DlgEffect> effects;
    std::string gotoNode;            // node id, empty / "end" = the conversation ends
};
struct DlgNode {
    std::string id, text;
    int stf = 0;
    std::vector<DlgEffect> onEnter;
    std::vector<DlgAnswer> answers;
};
struct DlgRoute { bool hasCond = false; DlgCond cond; std::string gotoNode; };
struct DialogDef {
    std::string id, speaker, file;
    std::vector<DlgRoute> start;     // first route whose condition holds picks the first node
    std::vector<DlgNode> nodes;
    const DlgNode* FindNode(const std::string& nodeId) const;
};

// ---- object scripts (data/objects, docs/object-scripts.md): behaviour bound to a map object
struct ObjStep {
    enum Kind { Dialog, Door, SkillCheck, Effect, Message, Teleport, Play, Call, DefaultUse, Original, End };
    Kind kind = End;
    bool hasCond = false;
    DlgCond cond;                    // "if": the step is skipped unless it holds
    std::string s;                   // Dialog: dialog id; Door: target instance name; Message: text; Teleport: map name; Play: effect name; Call: Lua function
    std::string key;                 // Message: optional locale key
    std::string action;              // Door: open | close | toggle | lock | unlock
    int x = -1, y = -1;              // Door: target cell (when no name); Teleport: target cell
    int entry = -1;                  // Teleport: entry point of the map
    std::string skill;               // SkillCheck: skill name
    int difficulty = 0, xp = 0;      // SkillCheck: needed skill value (roll: chance = skill - difficulty), xp on success
    bool roll = false;               // SkillCheck: roll the percent chance instead of a plain threshold
    std::vector<ObjStep> success, failure;
    DlgEffect fx;                    // Effect: give / take / quest / flag / xp
};
struct ObjectDef {
    std::string id, file;
    std::string map, object;         // binding: map name + instance name (EntityDef.name of the original map data)
    std::string proto;               // fallback binding: prototype key + cell
    int cx = -1, cy = -1;
    std::string label;               // use cursor label ("Use" when empty)
    std::string name, desc;          // overrides (locale keys object.<id>.name / object.<id>.desc)
    std::string script;              // scripts/<script>
    bool usable = true;
    int oneShot = 0;                 // 0 no, 1 once for the world, 2 once per player
    int cooldown = 0;                // seconds
    bool hasHidden = false;
    DlgCond hiddenUntil;             // hidden from a player until the condition holds
    int range = 0;                   // tiles, 0 = DoorOpenRange
    std::vector<ObjStep> onUse, onLook;
    std::vector<std::pair<std::string, std::vector<ObjStep>>> skills;   // skill name -> steps (C2S_UseSkill on the object)
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
    std::optional<std::string> model, heldModel;
    std::optional<int> caliber, ammoCaliber, ammoArmorAdj, ammoAccuracyAdj;
    std::optional<std::vector<AttackModeDef>> modes;   // weapon.modes (replaces the attack modes as a whole)
};

struct NpcSpawn { std::string map; int x = 0, y = 0; int count = 1; };

struct NpcRec {
    std::string file, path, id, base;
    std::optional<std::string> name, script, dialog, model;
    std::optional<int> team, level, maxHp, xp, respawn;
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
    std::vector<DialogDef> dialogs;
    std::vector<ObjectDef> objects;
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
    const DialogDef* FindDialog(const std::string& id) const;
    const ObjectDef* FindObject(const std::string& id) const;

    // "perks: +1 -0 ~2 (added: a; changed: b, c)" lines for kinds that differ, empty when nothing changed.
    static std::vector<std::string> Diff(const ContentDb& before, const ContentDb& after);
    std::string Summary() const;               // "6 traits, 8 perks, ..."
};

// f3_server --validate-data: loads gamedata (when given) and the content folder, prints the report, returns the exit code.
int RunCheckLocale(const std::filesystem::path& contentDir, const std::filesystem::path& localeDir, const std::string& srcDir, const std::filesystem::path& exePath, bool strict);
// f3_server --export-data: writes data/items, data/npcs and data/skills from the original data (see docs/data-format.md, "Exporter").
int RunExportData(const std::filesystem::path& gamedataDir, const std::filesystem::path& contentDir, const std::filesystem::path& outDir, bool force, bool check);
int RunValidateData(const std::filesystem::path& gamedataDir, const std::filesystem::path& contentDir, bool lenient);

std::filesystem::path FindContentDir(const std::filesystem::path& exePath);

}  // namespace f3
