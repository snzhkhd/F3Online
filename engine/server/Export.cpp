// f3_server --export-data: writes the original game records that designers may want to tweak (items, shipped NPCs, skill reference formulas)
// as data/*.json. Deterministic: records sorted by id, fixed field order, LF line endings, so a re-run produces no diff unless the original
// data or this tool changed. Rules of the generated files and the list of what is exported live in docs/data-format.md, "Exporter".
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <nlohmann/json.hpp>
#include "Content.h"
#include "DataFormat.h"
#include "GameConfig.h"
#include "common/protocol_game.h"

namespace f3 {

namespace fs = std::filesystem;
using nlohmann::ordered_json;

namespace {

std::string Lw(std::string s) { for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }

std::string Envelope(const char* kind, const char* note, const ordered_json& records) {
    ordered_json h = ordered_json::object();
    h["schema"] = kContentSchema;
    h["kind"] = kind;
    h["generator"] = "f3_server --export-data";
    h["note"] = note;
    h["records"] = records;
    return EmitJsonText(h);
}

struct Writer {
    fs::path outDir;
    bool force = false, check = false;
    int written = 0, same = 0, differs = 0, skipped = 0;

    void Put(const std::string& rel, const std::string& text) {
        const fs::path p = outDir / rel;
        std::string cur;
        bool exists = false;
        {
            std::ifstream f(p, std::ios::binary);
            if (f) { exists = true; std::stringstream ss; ss << f.rdbuf(); cur = ss.str(); }
        }
        cur.erase(std::remove(cur.begin(), cur.end(), '\r'), cur.end());
        if (exists && cur == text) { ++same; std::cout << "unchanged  " << rel << "\n"; return; }
        if (check && exists && !force && cur.find("\"generator\"") == std::string::npos) {   // taken over by hand: the exporter never overwrites it, so it is not "out of date"
            ++skipped;
            std::cout << "skipped    " << rel << " (edited by hand: no \"generator\" key)\n";
            return;
        }
        if (check) { ++differs; std::cout << (exists ? "DIFFERS    " : "MISSING    ") << rel << "\n"; return; }
        if (exists && !force && cur.find("\"generator\"") == std::string::npos) {
            ++skipped;
            std::cout << "skipped    " << rel << " (edited by hand: no \"generator\" key; use --force to overwrite or --export-out DIR to write elsewhere)\n";
            return;
        }
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        std::ofstream f(p, std::ios::binary);
        f << text;
        ++written;
        std::cout << (exists ? "updated    " : "created    ") << rel << "\n";
    }
};

// ---- items
ordered_json ItemRecord(const Proto& p) {
    ordered_json r = ordered_json::object();
    r["id"] = p.key;
    if (!p.name.empty()) r["name"] = p.name;
    if (!p.model.empty()) r["model"] = p.model;
    if (!p.heldModel.empty()) r["held_model"] = p.heldModel;
    if (p.cls == PClass::Weapon) {
        ordered_json w = ordered_json::object();
        if (p.caliber >= 0) w["caliber"] = p.caliber;
        if (p.range >= 1 && (p.wDmgMax > 0 || p.caliber >= 0)) w["range"] = std::min(p.range, 200);
        if (p.wDmgMax > 0) { w["dmg_min"] = p.wDmgMin; w["dmg_max"] = p.wDmgMax; }
        if (!p.modes.empty()) {
            ordered_json ms = ordered_json::array();
            for (const AttackModeDef& m : p.modes) {
                ordered_json e = ordered_json::object();
                e["type"] = m.type;
                e["cost"] = std::max(0, m.ap);
                e["dmg_min"] = m.dmgMin;
                e["dmg_max"] = m.dmgMax;
                if (m.apRef >= 0) e["ap"] = m.apRef;
                if (m.shots > 0) e["shots"] = m.shots;
                e["damage_type"] = m.damageType;
                ms.push_back(e);
            }
            w["modes"] = ms;
        }
        if (!w.empty()) r["weapon"] = w;
    } else if (p.cls == PClass::Armor) {
        ordered_json a = ordered_json::object();
        ordered_json v = ordered_json::array();
        for (int i = 0; i < 7; ++i) v.push_back(p.armor[i]);
        a["values"] = v;
        r["armor"] = a;
        r["head_armor"] = p.headArmor;
    } else if (p.cls == PClass::Ammo) {
        ordered_json a = ordered_json::object();
        if (p.ammoCaliber >= 0) a["caliber"] = p.ammoCaliber;
        a["dmg_min"] = p.ammoDmgMin;
        a["dmg_max"] = p.ammoDmgMax;
        a["armor_adj"] = p.ammoArmorAdj;
        a["accuracy_adj"] = p.ammoAccuracyAdj;
        r["ammo"] = a;
    }
    return r;
}

// ---- npcs
ordered_json NpcRecord(const Proto& p, const std::vector<const EntityDef*>& inst) {
    ordered_json r = ordered_json::object();
    r["id"] = p.key;
    if (!p.name.empty()) r["name"] = p.name;
    if (!p.model.empty()) r["model"] = p.model;
    // proto value; when the template has none and every map instance agrees on one, that value (instances keep their own anyway)
    auto agree = [&](auto get, auto empty) {
        using T = decltype(get(*inst.front()));
        T v{};
        bool first = true, ok = true;
        for (const EntityDef* e : inst) {
            T x = get(*e);
            if (first) { v = x; first = false; } else if (!(v == x)) ok = false;
        }
        return ok && !empty(v) ? v : T{};
    };
    int team = p.team;
    if (team == 0 && !inst.empty()) team = agree([](const EntityDef& e) { return e.team; }, [](int t) { return t == 0; });
    r["team"] = team;
    r["level"] = p.origLevel;
    if (p.origLevel > 1) {   // keep the hit points the server gives today (level 1 formula); the level itself is the original one
        const GameConfig g;
        r["max_hp"] = g.HpBase + p.special[0] + 2 * p.special[2];
    }
    static const char* const kSp[7] = {"strength", "perception", "endurance", "charisma", "intelligence", "agility", "luck"};
    ordered_json sp = ordered_json::object();
    for (int i = 0; i < 7; ++i) if (p.special[i] >= 1 && p.special[i] <= 10) sp[kSp[i]] = p.special[i];   // a template without SPECIAL data keeps the default
    if (!sp.empty()) r["special"] = sp;
    std::string script = p.script, dialog = p.dialog;
    if (script.empty() && !inst.empty()) script = agree([](const EntityDef& e) { return e.script; }, [](const std::string& s) { return s.empty(); });
    if (dialog.empty() && !inst.empty()) dialog = agree([](const EntityDef& e) { return e.dialog; }, [](const std::string& s) { return s.empty(); });
    if (!script.empty()) r["script"] = script;
    if (!dialog.empty()) r["dialog"] = dialog;
    if (!p.equipment.empty()) r["equipment"] = p.equipment;
    std::vector<std::string> inv = p.inventory;
    if (inv.empty() && !inst.empty()) inv = agree([](const EntityDef& e) { return e.inventory; }, [](const std::vector<std::string>& s) { return s.empty(); });
    if (!inv.empty()) r["inventory"] = inv;
    return r;
}

// ---- skills: formulas / descriptions of the current data (designer values are kept) plus the original game's bonus formula as a reference
ordered_json SkillRecords(const ContentDb& db) {
    static const char* const kSp[7] = {"strength", "perception", "endurance", "charisma", "intelligence", "agility", "luck"};
    // docs/research/vbdecomp-findings.md 2.2 (GameCreature::GetTotalSkill): total = stored base + bonus (+20 when tagged); bonus:
    struct O { int c, s, p, e, ch, i, a, l; bool has; };
    static const O orig[kSkillSlots] = {
        {50, 0, 0, 0, 0, 0, 0, 0, true}, {50, 0, 0, 0, 0, 0, 0, 0, true}, {0, 2, 0, 1, 0, 0, 0, 0, true}, {0, 0, 0, 0, 3, 0, 0, 0, true},
        {0, 0, 0, 0, 3, 0, 0, 0, true}, {0, 0, 0, 0, 2, 1, 0, 0, true}, {0, 0, 1, 0, 0, 2, 0, 0, true}, {0, 0, 1, 0, 0, 2, 0, 0, true},
        {0, 0, 2, 1, 0, 0, 0, 0, true}, {0, 0, 0, 0, 0, 3, 0, 0, true}, {0, 0, 2, 0, 0, 0, 1, 0, true}, {0, 0, 2, 0, 0, 0, 1, 0, true},
        {0, 0, 1, 0, 0, 0, 2, 0, true}, {0, 0, 0, 0, 0, 0, 0, 0, false}};
    ordered_json out = ordered_json::array();
    for (int k = 0; k < kSkillSlots; ++k) {
        ordered_json r = ordered_json::object();
        r["id"] = kSkillNames[k];
        if (!db.skills[k].desc.empty()) r["desc"] = db.skills[k].desc;
        ordered_json b = ordered_json::object();
        b["const"] = db.skills[k].constant;
        for (int i = 0; i < kSpecialCount; ++i) if (db.skills[k].coef[i]) b[kSp[i]] = db.skills[k].coef[i];
        r["base"] = b;
        if (orig[k].has) {
            ordered_json o = ordered_json::object();
            const int v[8] = {orig[k].c, orig[k].s, orig[k].p, orig[k].e, orig[k].ch, orig[k].i, orig[k].a, orig[k].l};
            if (v[0]) o["const"] = v[0];
            for (int i = 0; i < 7; ++i) if (v[i + 1]) o[kSp[i]] = v[i + 1];
            r["original"] = o;
        }
        out.push_back(r);
    }
    return out;
}

}  // namespace

// The record the exporter writes for a base prototype (the "base value" the content editor diffs against, docs/editor.md).
ordered_json ExportItemRecord(const Proto& p) { return ItemRecord(p); }
ordered_json ExportNpcRecord(const Proto& p) { return NpcRecord(p, {}); }

int RunExportData(const fs::path& gamedataDir, const fs::path& contentDir, const fs::path& outDirIn, bool force, bool check) {
    if (gamedataDir.empty()) { std::cerr << "export-data: no gamedata/server found (use --data DIR; run f3_dataimport first)\n"; return 2; }
    if (contentDir.empty() && outDirIn.empty()) { std::cerr << "export-data: no data/ folder found (use --content DIR or --export-out DIR)\n"; return 2; }
    DataStore ds;
    std::string err;
    if (!ds.Load(gamedataDir, err)) { std::cerr << "export-data: cannot load gamedata: " << err << "\n"; return 2; }
    Writer w;
    w.outDir = outDirIn.empty() ? contentDir : outDirIn;
    w.force = force;
    w.check = check;

    // the BASE layer only (the original prototypes), never the effective table: the export must not depend on what data/ already says
    std::vector<const Proto*> weapons, armors, ammos, kits;
    for (const Proto& p : ds.BaseProtos()) {
        if (p.cls == PClass::Weapon) weapons.push_back(&p);
        else if (p.cls == PClass::Armor) armors.push_back(&p);
        else if (p.cls == PClass::Ammo) ammos.push_back(&p);
        else if (p.cls == PClass::Item && Lw(p.key).rfind("ds_", 0) != 0) kits.push_back(&p);   // kits, keys, misc items (set dressing "DS_*" is not an item)
    }
    auto byId = [](const Proto* a, const Proto* b) { return Lw(a->key) < Lw(b->key); };
    auto items = [&](std::vector<const Proto*>& v) {
        std::sort(v.begin(), v.end(), byId);
        ordered_json a = ordered_json::array();
        for (const Proto* p : v) a.push_back(ItemRecord(*p));
        return a;
    };
    const char* itemNote = "Generated from the original prototypes by f3_server --export-data. Edit the numbers freely; remove the \"generator\" key to keep the file from being overwritten. Reference-only fields are explained in docs/data-format.md.";
    w.Put("items/weapons.json", Envelope("items", itemNote, items(weapons)));
    w.Put("items/armor.json", Envelope("items", itemNote, items(armors)));
    w.Put("items/ammo.json", Envelope("items", itemNote, items(ammos)));
    w.Put("items/kits.json", Envelope("items", itemNote, items(kits)));

    // NPCs of the shipped maps: every non-static critter prototype placed by at least one map
    std::map<std::string, std::vector<EntityDef>> placed;   // proto key (lowercase) -> instances (all maps)
    int mapsRead = 0;
    for (const std::string& name : ds.MapNames()) {
        MapDef m;
        std::string merr;
        if (!ds.LoadMap(name, m, merr)) { std::cerr << "export-data: map '" << name << "': " << merr << " (skipped)\n"; continue; }
        ++mapsRead;
        for (const EntityDef& e : m.entities) placed[Lw(e.proto)].push_back(e);
    }
    std::vector<const Proto*> npcs;
    for (const Proto& p : ds.BaseProtos())
        if (p.cls == PClass::Critter && !p.isStatic && placed.count(Lw(p.key))) npcs.push_back(&p);
    std::sort(npcs.begin(), npcs.end(), byId);
    ordered_json nrec = ordered_json::array();
    for (const Proto* p : npcs) {
        std::vector<const EntityDef*> inst;
        for (const EntityDef& e : placed[Lw(p->key)]) inst.push_back(&e);
        nrec.push_back(NpcRecord(*p, inst));
    }
    w.Put("npcs/original.json", Envelope("npcs", "Critter templates placed by the shipped maps, from the original data (f3_server --export-data). Map instances keep their own team / script / dialog / inventory; these records are the template defaults (and what data spawns use). Remove the \"generator\" key to keep your edits.", nrec));

    // skills: current formulas + the original bonus formula as reference
    ContentDb cur;
    ContentReport rep;
    if (!contentDir.empty()) ContentDb::Build(contentDir, nullptr, true, cur, rep);
    w.Put("skills/skills.json", Envelope("skills", "Skill base formulas (ours, editable) with the original game's bonus formula as a reference in \"original\" (not applied). Regenerated by f3_server --export-data: formulas and descriptions you changed are kept.", SkillRecords(cur)));

    std::cout << "export-data: " << weapons.size() << " weapons, " << armors.size() << " armor, " << ammos.size() << " ammo, " << kits.size() << " kits / keys, " << npcs.size()
              << " NPC templates (" << mapsRead << " maps read); " << w.written << " written, " << w.same << " unchanged, " << w.skipped << " skipped, " << w.differs << " differ\n";
    if (check && w.differs) { std::cout << "export-data --check: the files in " << w.outDir.generic_string() << " are out of date (run f3_server --export-data)\n"; return 1; }
    return 0;
}

}  // namespace f3
