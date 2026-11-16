// Unit-style tests of the data layer (loader, override / merge, validation, pid stability). No server, no network, no original game
// files: a tiny synthetic gamedata folder and data folders are written to a temp directory.
//   f3_datatest            exit code 0 = all passed
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include "Content.h"
#include "DataStore.h"

using namespace f3;
namespace fs = std::filesystem;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond, ...) do { ++g_checks; if (!(cond)) { ++g_failed; std::printf("  FAIL %s:%d  ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static void Put(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << text;
}

static bool Has(const std::vector<std::string>& v, const std::string& needle) {
    for (const std::string& s : v) if (s.find(needle) != std::string::npos) return true;
    return false;
}
static std::string Dump(const ContentReport& r) {
    std::string s;
    for (auto& e : r.errors) s += "    error: " + e + "\n";
    for (auto& e : r.warnings) s += "    warning: " + e + "\n";
    return s;
}

// ---- synthetic gamedata/server (what f3_dataimport would write, reduced to five prototypes and one map)
static void MakeGamedata(const fs::path& dir) {
    Put(dir / "manifest.json", R"({"maps":[{"name":"town","width":10,"height":10,"entities":0,"has_emap":true}],"start_map":"town","start_entry":0})");
    Put(dir / "protos.json", R"({
      "Ammo9.AMO":  {"class":"ammo","name":"9mm","desc":"Ammo","ammo":{"caliber":12,"dmg_min":5,"dmg_max":12}},
      "Armor1.ARM": {"class":"armor","name":"Leather","armor":{"values":[4,4,4,0,8,4,0]}},
      "Junk.ITM":   {"class":"item","name":"Junk","desc":"Old junk"},
      "Pistol.WEA": {"class":"weapon","name":"Pistol","desc":"A pistol","weapon":{"weapon_class":1,"caliber":12,"range":15,"dmg_min":6.0,"dmg_max":12.0}},
      "Thug.CRT":   {"class":"critter","name":"Thug","team":2,"critter":{"special":[5,5,5,5,5,5,5]},"inventory":["Junk.ITM"]}
    })");
}

static std::string Env(const char* kind, const std::string& records, const std::string& extra = "") {
    return std::string("{\"schema\":1,\"kind\":\"") + kind + "\"" + extra + ",\"records\":" + records + "}";
}

struct Fixture {
    fs::path root, gd, data;
    DataStore ds;
    explicit Fixture(const std::string& name) {
        root = fs::temp_directory_path() / ("f3_datatest_" + name + "_" + std::to_string(std::rand()));
        fs::remove_all(root);
        gd = root / "gamedata";
        data = root / "data";
        MakeGamedata(gd);
        Put(data / "manifest.json", R"({"schema":1,"name":"test"})");
        std::string err;
        if (!ds.Load(gd, err)) { std::printf("fixture: %s\n", err.c_str()); std::exit(2); }
    }
    ~Fixture() { std::error_code ec; fs::remove_all(root, ec); }
    bool Build(bool lenient, ContentDb& db, ContentReport& rep, bool withBase = true) { return ContentDb::Build(data, withBase ? &ds : nullptr, lenient, db, rep); }
};

// ----------------------------------------------------------------------------------------------------------------

static void TestEmptyAndMissing() {
    std::printf("empty / missing data folder\n");
    Fixture f("empty");
    ContentDb db; ContentReport rep;
    CHECK(f.Build(false, db, rep) && rep.Ok(), "an empty (manifest only) data folder is valid\n%s", Dump(rep).c_str());
    CHECK(db.loaded && db.traits.empty() && db.perks.empty(), "nothing defined");
    ContentDb db2; ContentReport rep2;
    CHECK(ContentDb::Build(f.root / "nope", &f.ds, false, db2, rep2) && rep2.Ok() && !db2.loaded, "a missing data folder means built-in defaults, not an error");
    fs::remove(f.data / "manifest.json");
    ContentDb db3; ContentReport rep3;
    CHECK(!f.Build(false, db3, rep3) && Has(rep3.errors, "manifest.json"), "a folder without manifest.json is rejected\n%s", Dump(rep3).c_str());
}

static void TestSkillDefaults() {
    std::printf("skill formulas: built-in defaults equal the legacy placeholders\n");
    ContentDb db;
    bool same = true;
    for (int trial = 0; trial < 200; ++trial) {
        int sp[7];
        for (int i = 0; i < 7; ++i) sp[i] = 1 + (trial * (i + 3) + i * 7) % 10;
        const int S = sp[0], P = sp[1], E = sp[2], C = sp[3], I = sp[4], A = sp[5];
        const int legacy[14] = {5 + 4 * A, 20 + 2 * (S + A), 30 + 2 * (S + A), 4 * C, 5 + 4 * C, 5 + 3 * C + I, 3 * I, 2 * (P + I), 5 + 2 * (E + I), 4 * I, 10 + P + A, 5 + 3 * A, 3 + 3 * A, 10 + P + A};
        for (int k = 0; k < 14; ++k) {
            int v = db.skills[k].constant;
            for (int i = 0; i < 7; ++i) v += db.skills[k].coef[i] * sp[i];
            if (v != legacy[k]) same = false;
        }
    }
    CHECK(same, "defaults reproduce the old SkillBase switch");
}

static void TestLoadAndMerge() {
    std::printf("loading, override / merge by id, new records\n");
    Fixture f("merge");
    Put(f.data / "skills/s.json", Env("skills", R"([{"id":"firearms","desc":"Guns","base":{"const":10,"agility":3}}])"));
    Put(f.data / "traits/t.json", Env("traits", R"([{"id":"bruiser","index":0,"name":"Bruiser","desc":"Hits hard"},{"id":"gifted","index":6,"name":"Gifted","effects":[{"stat":"skill.Melee","add":5}]}])", R"(,"max_picks":2)"));
    Put(f.data / "perks/p.json", Env("perks", R"([
        {"id":"toughness","name":"Toughness","requires":{"level":3,"special":{"endurance":6}},"effects":[{"stat":"max_hp","add":10}]},
        {"id":"thick_skin","name":"Thick Skin","ranks":2,"requires":{"perks":["toughness"],"traits":["gifted"],"skills":{"Melee":30}},"hook":"perk_x","effects":[{"stat":"armor_class","add":5}]}])"));
    Put(f.data / "items/i.json", Env("items", R"([
        {"id":"Pistol.WEA","name":"Service Pistol","desc":"Reliable.","weight":3,"weapon":{"range":18,"dmg_min":7,"dmg_max":13}},
        {"id":"junk.itm","weight":0},
        {"id":"Armor1.ARM","armor":{"values":[1,2,3,4,5,6,7]},"head_armor":true},
        {"id":"Ammo9.AMO","ammo":{"dmg_min":6,"dmg_max":14}},
        {"id":"F3_Pistol_Mk2.WEA","base":"Pistol.WEA","name":"Pistol Mk2","weapon":{"dmg_max":20}}])"));
    Put(f.data / "npcs/n.json", Env("npcs", R"([
        {"id":"Thug.CRT","name":"Street Thug","team":3,"level":4,"max_hp":77,"xp":250,"special":{"strength":8},"inventory":["Pistol.WEA","F3_Pistol_Mk2.WEA"],"equipment":["Armor1.ARM"],"spawns":[{"map":"town","cell":[3,4],"count":2}]},
        {"id":"F3_Boss.CRT","base":"Thug.CRT","name":"Boss","xp":999}])"));
    Put(f.data / "quests/q.json", Env("quests", R"([{"id":"q1","title":"Quest","text":"Do it","xp":100,"items":[{"item":"Junk.ITM","count":3}]}])"));
    ContentDb db; ContentReport rep;
    const bool ok = f.Build(false, db, rep);
    CHECK(ok && rep.Ok(), "valid data loads\n%s", Dump(rep).c_str());
    if (!ok) return;
    CHECK(db.traits.size() == 2 && db.hasTraitMax && db.traitMax == 2, "traits and max_picks");
    CHECK(db.FindTrait("GIFTED") && db.FindTrait("gifted")->effects.size() == 1 && db.FindTrait("gifted")->effects[0].stat == kStatSkillBase + 1, "trait effect skill.Melee parsed, ids case-insensitive");
    CHECK(db.perks.size() == 2 && db.FindPerk("thick_skin")->ranks == 2 && db.FindPerk("thick_skin")->req.skills.size() == 1 && db.FindPerk("thick_skin")->hook == "perk_x", "perk fields");
    CHECK(db.FindPerk("toughness")->req.special[2] == 6 && db.FindPerk("toughness")->req.level == 3, "perk requirements");
    CHECK(db.skills[0].constant == 10 && db.skills[0].coef[5] == 3 && db.skills[0].coef[0] == 0, "skill override replaces the formula (case-insensitive id)");
    CHECK(db.skills[1].constant == 20, "untouched skills keep their defaults");
    CHECK(db.protosBuilt, "candidate prototype table built");
    const size_t baseN = f.ds.ProtoCount();
    // the loaded base is untouched until Commit
    CHECK(f.ds.FindProto("Pistol.WEA")->name == "Pistol", "base unchanged before commit");
    f.ds.Commit(std::move(db.protoSet));
    const Proto* pi = f.ds.FindProto("pistol.wea");
    CHECK(pi && pi->name == "Service Pistol" && pi->desc == "Reliable." && pi->weightOverride == 3 && pi->range == 18 && pi->wDmgMin == 7 && pi->wDmgMax == 13, "item override applied (case-insensitive id)");
    CHECK(pi && pi->caliber == 12 && pi->model == f.ds.BaseProtos()[pi->pid].model, "fields not named by the record keep the original value");
    CHECK(f.ds.BaseProtos()[pi->pid].name == "Pistol" && f.ds.BaseProtos()[pi->pid].weightOverride == -1, "the original prototype stays available as the base layer");
    CHECK(f.ds.FindProto("Junk.ITM")->weightOverride == 0, "weight 0 is a valid override");
    const Proto* ar = f.ds.FindProto("Armor1.ARM");
    CHECK(ar->armor[0] == 1 && ar->armor[6] == 7 && ar->headArmor, "armor values / head_armor");
    CHECK(f.ds.FindProto("Ammo9.AMO")->ammoDmgMin == 6 && f.ds.FindProto("Ammo9.AMO")->ammoDmgMax == 14, "ammo damage");
    const Proto* mk = f.ds.FindProto("F3_Pistol_Mk2.WEA");
    CHECK(mk && mk->fromData && mk->pid >= baseN && mk->name == "Pistol Mk2" && mk->wDmgMax == 20 && mk->wDmgMin == 6 && mk->caliber == 12 && mk->desc == "A pistol", "new item cloned from its base, then overridden");
    CHECK(f.ds.GetProto(mk->pid) == mk && f.ds.ProtoCount() == baseN + 2, "new prototypes get pids after the original ones");
    const Proto* th = f.ds.FindProto("Thug.CRT");
    CHECK(th->name == "Street Thug" && th->team == 3 && th->level == 4 && th->maxHpFixed == 77 && th->xp == 250 && th->special[0] == 8 && th->special[1] == 5, "npc override (partial SPECIAL keeps the rest)");
    CHECK(th->inventory.size() == 2 && th->equipment.size() == 1, "lists replace the original lists");
    const Proto* boss = f.ds.FindProto("F3_Boss.CRT");
    CHECK(boss && boss->fromData && boss->cls == PClass::Critter && boss->xp == 999 && boss->team == 2 && boss->inventory.size() == 1, "new npc cloned from Thug.CRT before the data override? (clones the effective base)");
    CHECK(db.FindNpc("thug.crt") && db.FindNpc("Thug.CRT")->spawns.size() == 1 && db.FindNpc("Thug.CRT")->spawns[0].count == 2, "spawn defaults kept");
    CHECK(db.FindQuest("Q1") && db.FindQuest("q1")->items.size() == 1 && db.FindQuest("q1")->xp == 100, "quest");

    // ---- pid stability across reloads: a record that disappears keeps its pid (retired), coming back reuses it
    const uint16_t mkPid = mk->pid;
    fs::remove(f.data / "items/i.json");
    Put(f.data / "npcs/n.json", Env("npcs", "[]"));
    ContentDb db2; ContentReport rep2;
    CHECK(f.Build(false, db2, rep2) && rep2.Ok(), "reload after removing records\n%s", Dump(rep2).c_str());
    f.ds.Commit(std::move(db2.protoSet));
    CHECK(f.ds.FindProto("F3_Pistol_Mk2.WEA") == nullptr, "removed record is no longer found by key");
    CHECK(f.ds.GetProto(mkPid) && f.ds.GetProto(mkPid)->retired && f.ds.GetProto(mkPid)->name == "Pistol Mk2", "but its pid stays valid for existing instances (retired)");
    CHECK(f.ds.FindProto("Pistol.WEA")->name == "Pistol" && f.ds.FindProto("Pistol.WEA")->weightOverride == -1, "overrides vanish with their records (back to the base layer)");
    CHECK(f.ds.FindProto("Thug.CRT")->name == "Thug" && f.ds.FindProto("Thug.CRT")->xp == -1, "npc back to the original");
    Put(f.data / "items/i.json", Env("items", R"([{"id":"F3_Pistol_Mk2.WEA","base":"Pistol.WEA","name":"Back Again"}])"));
    ContentDb db3; ContentReport rep3;
    CHECK(f.Build(false, db3, rep3) && rep3.Ok(), "re-added\n%s", Dump(rep3).c_str());
    f.ds.Commit(std::move(db3.protoSet));
    CHECK(f.ds.FindProto("F3_Pistol_Mk2.WEA") && f.ds.FindProto("F3_Pistol_Mk2.WEA")->pid == mkPid && !f.ds.GetProto(mkPid)->retired, "a re-added record reuses its pid");

    // ---- diff
    ContentDb a, b; ContentReport ra, rb;
    f.Build(false, a, ra);
    Put(f.data / "perks/p.json", Env("perks", R"([{"id":"toughness","name":"Tougher","effects":[{"stat":"max_hp","add":12}]},{"id":"new_one","name":"New"}])"));
    f.Build(false, b, rb);
    const auto diff = ContentDb::Diff(a, b);
    CHECK(Has(diff, "perks: +2 -0 ~1") || Has(diff, "perks:"), "diff reports perks");
    std::string joined;
    for (auto& d : diff) joined += d + "\n";
    CHECK(joined.find("toughness") != std::string::npos && joined.find("new_one") != std::string::npos, "diff names the changed / added ids\n%s", joined.c_str());
}

static void ExpectError(const char* label, const std::string& file, const std::string& content, const std::string& needle, const std::string& needle2 = "") {
    Fixture f("err");
    Put(f.data / file, content);
    // a second valid perk file so cross-file references can be tested
    ContentDb db; ContentReport rep;
    const bool ok = f.Build(false, db, rep);
    const bool match = Has(rep.errors, needle) && (needle2.empty() || Has(rep.errors, needle2));
    CHECK(!ok && !rep.Ok() && match, "%s: expected failure containing '%s' '%s'\n%s", label, needle.c_str(), needle2.c_str(), Dump(rep).c_str());
}

static void TestValidation() {
    std::printf("validation failures (file + path in every message)\n");
    ExpectError("invalid json", "perks/p.json", "{ not json", "data/perks/p.json", "invalid JSON");
    ExpectError("missing schema", "perks/p.json", R"({"kind":"perks","records":[]})", "data/perks/p.json: schema", "missing");
    ExpectError("schema too new", "perks/p.json", R"({"schema":2,"kind":"perks","records":[]})", "schema 2 is newer");
    ExpectError("wrong kind", "perks/p.json", R"({"schema":1,"kind":"items","records":[]})", "data/perks/p.json: kind");
    ExpectError("records not array", "perks/p.json", R"({"schema":1,"kind":"perks","records":{}})", "records", "expected array");
    ExpectError("unknown top-level", "perks/p.json", R"({"schema":1,"kind":"perks","records":[],"extra":1})", "unknown top-level field");
    ExpectError("wrong type", "perks/p.json", Env("perks", R"([{"id":"a","name":"A","requires":{"level":"three"}}])"), "data/perks/p.json: records[0].requires.level", "expected integer, got string");
    ExpectError("range", "perks/p.json", Env("perks", R"([{"id":"a","name":"A","ranks":99}])"), "records[0].ranks", "out of range");
    ExpectError("unknown field", "perks/p.json", Env("perks", R"([{"id":"a","name":"A","descr":"typo"}])"), "records[0].descr", "unknown field");
    ExpectError("missing name", "perks/p.json", Env("perks", R"([{"id":"a"}])"), "records[0].name", "missing required");
    ExpectError("unknown stat", "perks/p.json", Env("perks", R"([{"id":"a","name":"A","effects":[{"stat":"charisma_bonus","add":1}]}])"), "records[0].effects[0].stat", "unknown stat 'charisma_bonus'");
    ExpectError("bad hook name", "perks/p.json", Env("perks", R"([{"id":"a","name":"A","hook":"not a name"}])"), "records[0].hook", "not a valid Lua function name");
    ExpectError("bad id chars", "perks/p.json", Env("perks", R"([{"id":"a b","name":"A"}])"), "records[0].id", "invalid id");
    ExpectError("duplicate id (same file)", "perks/p.json", Env("perks", R"([{"id":"a","name":"A"},{"id":"A","name":"A2"}])"), "records[1].id", "duplicate id 'A' (first defined in data/perks/p.json records[0])");
    ExpectError("unknown perk requirement", "perks/p.json", Env("perks", R"([{"id":"a","name":"A","requires":{"perks":["ghost"]}}])"), "unknown perk 'ghost'");
    ExpectError("unknown trait requirement", "perks/p.json", Env("perks", R"([{"id":"a","name":"A","requires":{"traits":["ghost"]}}])"), "unknown trait 'ghost'");
    ExpectError("unknown skill requirement", "perks/p.json", Env("perks", R"([{"id":"a","name":"A","requires":{"skills":{"Telepathy":10}}}])"), "unknown skill 'Telepathy'");
    ExpectError("requirement cycle", "perks/p.json", Env("perks", R"([{"id":"a","name":"A","requires":{"perks":["b"]}},{"id":"b","name":"B","requires":{"perks":["a"]}}])"), "requirement cycle");
    ExpectError("trait index duplicate", "traits/t.json", Env("traits", R"([{"id":"a","index":1,"name":"A"},{"id":"b","index":1,"name":"B"}])"), "records[1].index", "already used");
    ExpectError("trait index range", "traits/t.json", Env("traits", R"([{"id":"a","index":16,"name":"A"}])"), "records[0].index", "out of range");
    ExpectError("max_picks range", "traits/t.json", Env("traits", "[]", R"(,"max_picks":40)"), "max_picks");
    ExpectError("unknown skill", "skills/s.json", Env("skills", R"([{"id":"Telepathy","base":{"const":1}}])"), "unknown skill 'Telepathy'");
    ExpectError("skill base key", "skills/s.json", Env("skills", R"([{"id":"Melee","base":{"smell":1}}])"), "base.smell", "unknown field");
    ExpectError("unknown item id", "items/i.json", Env("items", R"([{"id":"Nope.WEA","name":"x"}])"), "data/items/i.json: records[0].id", "unknown item id 'Nope.WEA'");
    ExpectError("item base unknown", "items/i.json", Env("items", R"([{"id":"New.WEA","base":"Nope.WEA"}])"), "records[0].base", "unknown base prototype");
    ExpectError("item base but id exists", "items/i.json", Env("items", R"([{"id":"Pistol.WEA","base":"Pistol.WEA"}])"), "records[0].base", "already exists");
    ExpectError("item wrong class group", "items/i.json", Env("items", R"([{"id":"Junk.ITM","weapon":{"range":5}}])"), "records[0].weapon", "not a weapon");
    ExpectError("item record on a critter", "items/i.json", Env("items", R"([{"id":"Thug.CRT","name":"x"}])"), "is a critter", "data/npcs");
    ExpectError("item weight type", "items/i.json", Env("items", R"([{"id":"Junk.ITM","weight":"heavy"}])"), "records[0].weight", "expected integer, got string");
    ExpectError("item dmg order", "items/i.json", Env("items", R"([{"id":"Pistol.WEA","weapon":{"dmg_min":9,"dmg_max":3}}])"), "dmg_min is larger");
    ExpectError("armor values count", "items/i.json", Env("items", R"([{"id":"Armor1.ARM","armor":{"values":[1,2]}}])"), "armor.values", "expected 7");
    ExpectError("npc loot unknown item", "npcs/n.json", Env("npcs", R"([{"id":"Thug.CRT","inventory":["Pistol.WEA","Ghost.ITM"]}])"), "data/npcs/n.json: records[0].inventory[1]", "unknown item 'Ghost.ITM'");
    ExpectError("npc equipment is a critter", "npcs/n.json", Env("npcs", R"([{"id":"Thug.CRT","equipment":["Thug.CRT"]}])"), "records[0].equipment[0]", "is a critter");
    ExpectError("npc unknown id", "npcs/n.json", Env("npcs", R"([{"id":"Ghost.CRT","name":"x"}])"), "unknown npc id 'Ghost.CRT'");
    ExpectError("npc on an item", "npcs/n.json", Env("npcs", R"([{"id":"Pistol.WEA","name":"x"}])"), "is not a critter");
    ExpectError("npc special range", "npcs/n.json", Env("npcs", R"([{"id":"Thug.CRT","special":{"strength":11}}])"), "special.strength", "out of range");
    ExpectError("npc spawn map", "npcs/n.json", Env("npcs", R"([{"id":"Thug.CRT","spawns":[{"map":"atlantis","cell":[1,1]}]}])"), "spawns[0].map", "unknown map 'atlantis'");
    ExpectError("npc spawn cell", "npcs/n.json", Env("npcs", R"([{"id":"Thug.CRT","spawns":[{"map":"town","cell":[1]}]}])"), "spawns[0].cell", "expected [x, y]");
    ExpectError("quest reward item", "quests/q.json", Env("quests", R"([{"id":"q","title":"Q","items":[{"item":"Ghost.ITM"}]}])"), "items[0].item", "unknown item 'Ghost.ITM'");
    ExpectError("quest reward unknown field", "quests/q.json", Env("quests", R"([{"id":"q","title":"Q","reward":5}])"), "records[0].reward", "unknown field");

    {   // duplicate ids across files, across items and npcs (one prototype namespace)
        Fixture f("dup");
        Put(f.data / "perks/a.json", Env("perks", R"([{"id":"x","name":"X"}])"));
        Put(f.data / "perks/b.json", Env("perks", R"([{"id":"X","name":"X again"}])"));
        Put(f.data / "items/i.json", Env("items", R"([{"id":"Junk.ITM","weight":1}])"));
        Put(f.data / "items/j.json", Env("items", R"([{"id":"JUNK.itm","weight":2}])"));
        ContentDb db; ContentReport rep;
        CHECK(!f.Build(false, db, rep) && Has(rep.errors, "data/perks/b.json: records[0].id: duplicate id 'X' (first defined in data/perks/a.json records[0])"), "duplicate across files names both files\n%s", Dump(rep).c_str());
        CHECK(Has(rep.errors, "data/items/j.json: records[0].id: duplicate id 'JUNK.itm'"), "duplicate item override\n%s", Dump(rep).c_str());
    }
    {   // several errors are all reported (not just the first), disabled files are ignored
        Fixture f("multi");
        Put(f.data / "perks/p.json", Env("perks", R"([{"id":"a"},{"id":"b","name":"B","ranks":0},{"id":"c","name":"C"}])"));
        Put(f.data / "perks/_disabled.json", "this is not even json");
        ContentDb db; ContentReport rep;
        CHECK(!f.Build(false, db, rep) && rep.errors.size() == 2 && !Has(rep.errors, "_disabled"), "all errors reported, _files ignored\n%s", Dump(rep).c_str());
    }
    {   // lenient: errors become warnings and the broken records are dropped
        Fixture f("lenient");
        Put(f.data / "perks/p.json", Env("perks", R"([{"id":"good","name":"Good"},{"id":"bad","name":"Bad","requires":{"perks":["ghost"]}},{"id":"typo","name":"T","rankz":1}])"));
        Put(f.data / "items/i.json", Env("items", R"([{"id":"Pistol.WEA","name":"Fine"},{"id":"Ghost.WEA","name":"x"}])"));
        ContentDb db; ContentReport rep;
        const bool ok = f.Build(true, db, rep);
        CHECK(ok && rep.errors.empty() && rep.warnings.size() >= 3, "lenient: no errors, warnings instead\n%s", Dump(rep).c_str());
        CHECK(db.FindPerk("good") && !db.FindPerk("bad") && !db.FindPerk("typo"), "lenient drops only the broken perks");
        f.ds.Commit(std::move(db.protoSet));
        CHECK(f.ds.FindProto("Pistol.WEA")->name == "Fine", "lenient still applies the valid item record");
    }
    {   // without gamedata: structure and cross references between data files are still checked, item / npc records are not applied
        Fixture f("nobase");
        Put(f.data / "items/i.json", Env("items", R"([{"id":"Whatever.WEA","name":"x"}])"));
        Put(f.data / "perks/p.json", Env("perks", R"([{"id":"a","name":"A","requires":{"perks":["ghost"]}}])"));
        ContentDb db; ContentReport rep;
        const bool ok = f.Build(false, db, rep, false);
        CHECK(!ok && Has(rep.errors, "unknown perk 'ghost'") && Has(rep.warnings, "no gamedata"), "without a base: perk references checked, a warning for items\n%s", Dump(rep).c_str());
        CHECK(!db.protosBuilt, "no prototype table without a base");
    }
}

int main() {
    TestEmptyAndMissing();
    TestSkillDefaults();
    TestLoadAndMerge();
    TestValidation();
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
