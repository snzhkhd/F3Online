// f3_server --check-locale: coverage report of data/locale (see docs/localization.md); also the structured status behind the editor's locale tab.
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include "Content.h"
#include "EditorCore.h"
#include "common/Localization.h"
#include "common/protocol_game.h"

namespace f3 {

namespace {

// English base text of every locale key whose text lives in data/*.json (the translations are keys by convention, docs/localization.md).
std::map<std::string, std::string> DataKeyTexts(const ContentDb& db) {
    std::map<std::string, std::string> m;
    for (const PerkDef& p : db.perks) { m["perk." + p.id + ".name"] = p.name; if (!p.desc.empty()) m["perk." + p.id + ".desc"] = p.desc; }
    for (const TraitDef& t : db.traits) { m["trait." + t.id + ".name"] = t.name; if (!t.desc.empty()) m["trait." + t.id + ".desc"] = t.desc; }
    for (const QuestDef& q : db.quests) { m["quest." + q.id + ".title"] = q.title; if (!q.text.empty()) m["quest." + q.id + ".text"] = q.text; }
    for (const DialogDef& d : db.dialogs)
        for (const DlgNode& n : d.nodes) {
            if (n.stf == 0) m["dialog." + d.id + "." + n.id + ".text"] = n.text;
            for (size_t k = 0; k < n.answers.size(); ++k)
                if (n.answers[k].stf == 0) m["dialog." + d.id + "." + n.id + "." + (n.answers[k].id.empty() ? "a" + std::to_string(k + 1) : n.answers[k].id)] = n.answers[k].text;
        }
    for (int i = 0; i < kSkillCount; ++i) {
        m[std::string("skill.") + kSkillNames[i] + ".name"] = kSkillNames[i];
        if (!db.skills[i].desc.empty()) m[std::string("skill.") + kSkillNames[i] + ".desc"] = db.skills[i].desc;
    }
    return m;
}

}  // namespace

int RunCheckLocale(const std::filesystem::path& contentDir, const std::filesystem::path& localeDirIn, const std::string& srcDirArg, const std::filesystem::path& exePath, bool strict) {
    namespace fs = std::filesystem;
    fs::path localeDir = localeDirIn.empty() ? contentDir / "locale" : localeDirIn;
    if (!fs::exists(localeDir)) localeDir = loc::Locale::FindDir(exePath);
    loc::Locale l;
    std::string err;
    if (!localeDir.empty()) l.Load(localeDir, &err);
    if (!l.Loaded()) {
        std::cout << "check-locale: cannot load data/locale: " << (err.empty() ? "folder not found" : err) << "\n";
        return 2;
    }
    if (!err.empty()) std::cout << "check-locale: " << err;
    // the data records whose English base text lives in data/*.json: their translations are keyed by id
    ContentDb db;
    ContentReport rep;
    ContentDb::Build(contentDir, nullptr, true, db, rep);
    loc::CheckOptions o;
    o.strict = strict;
    for (const auto& [k, v] : DataKeyTexts(db)) o.dataKeys.insert(k);
    // sources: --src DIR, else the engine/ and scripts/ folders next to data/
    if (!srcDirArg.empty()) o.srcDir = srcDirArg;
    else {
        const fs::path root = fs::absolute(contentDir).parent_path();
        if (fs::exists(root / "engine")) o.srcDir = root / "engine";
        if (fs::exists(root / "scripts")) o.extraSrcDirs.push_back(root / "scripts");
    }
    const loc::CheckResult r = loc::Check(l, o);
    std::cout << "locale dir: " << localeDir.generic_string() << "\n" << r.text;
    return r.errors ? 1 : 0;
}

namespace editor {

using nlohmann::json;

// Same rules as --check-locale (missing / extra / placeholder mismatch against en, data keys included), but per key and with the file each text lives in.
json LocaleStatus(const std::filesystem::path& contentDir) {
    namespace fs = std::filesystem;
    json out = json::object();
    const fs::path root = contentDir / "locale";
    std::error_code ec;
    if (!fs::is_directory(root, ec)) { out["ok"] = false; out["error"] = "no data/locale folder"; return out; }
    ContentDb db;
    ContentReport rep;
    ContentDb::Build(contentDir, nullptr, true, db, rep);
    const std::map<std::string, std::string> dataTexts = DataKeyTexts(db);

    struct Cell { std::string text, file; };
    std::map<std::string, std::map<std::string, Cell>> tables;   // lang -> key -> text, file
    std::vector<std::string> langs, problems;
    for (const auto& de : fs::directory_iterator(root, ec)) {
        if (!de.is_directory()) continue;
        const std::string lang = de.path().filename().string();
        langs.push_back(lang);
        std::vector<fs::path> files;
        for (const auto& fe : fs::directory_iterator(de.path(), ec)) {
            const std::string fn = fe.path().filename().string();
            if (fe.is_regular_file() && fe.path().extension() == ".json" && fn[0] != '_' && fn[0] != '.') files.push_back(fe.path());
        }
        std::sort(files.begin(), files.end());
        for (const fs::path& f : files) {
            std::ifstream in(f, std::ios::binary);
            const std::string fn = f.filename().string();
            try {
                const json j = json::parse(in, nullptr, true, true);
                if (!j.is_object()) { problems.push_back("locale/" + lang + "/" + fn + ": not a JSON object"); continue; }
                for (auto it = j.begin(); it != j.end(); ++it) {
                    if (it.value().is_string()) tables[lang][it.key()] = Cell{it.value().get<std::string>(), fn};
                    else problems.push_back("locale/" + lang + "/" + fn + ": " + it.key() + ": not a string");
                }
            } catch (const std::exception& e) { problems.push_back("locale/" + lang + "/" + fn + ": " + e.what()); }
        }
    }
    std::sort(langs.begin(), langs.end());
    out["ok"] = true;
    out["langs"] = langs;
    out["problems"] = problems;
    std::set<std::string> keys;
    for (const auto& [lang, t] : tables) for (const auto& [k, c] : t) keys.insert(k);
    for (const auto& [k, v] : dataTexts) keys.insert(k);
    json entries = json::array();
    std::map<std::string, std::array<int, 4>> sum;   // lang -> total, missing, extra, badPlaceholders
    for (const std::string& lang : langs) if (lang != "en") sum[lang] = {0, 0, 0, 0};
    for (const std::string& k : keys) {
        json e = json::object();
        e["key"] = k;
        const std::string ns = k.substr(0, k.find('.'));
        const bool isData = dataTexts.count(k) != 0, isStf = ns == "stf";
        e["group"] = isStf ? "stf" : isData ? "data" : ns;
        if (isData) e["dataText"] = dataTexts.at(k);
        const auto enT = tables.find("en");
        const Cell* en = nullptr;
        if (enT != tables.end()) { auto it = enT->second.find(k); if (it != enT->second.end()) en = &it->second; }
        if (en) e["en"] = json{{"text", en->text}, {"file", en->file}};
        json tr = json::object();
        for (const std::string& lang : langs) {
            if (lang == "en") continue;
            const auto lt = tables.find(lang);
            const Cell* c = nullptr;
            if (lt != tables.end()) { auto it = lt->second.find(k); if (it != lt->second.end()) c = &it->second; }
            auto& s = sum[lang];
            const bool inBase = en || isData;
            if (inBase) ++s[0];
            json cell = json::object();
            if (c) { cell["text"] = c->text; cell["file"] = c->file; }
            std::string status = "ok";
            if (inBase && !c) { status = "missing"; ++s[1]; }
            else if (c && !inBase && !isStf) { status = "extra"; ++s[2]; }
            else if (c && en && loc::Locale::Placeholders(en->text) != loc::Locale::Placeholders(c->text)) { status = "placeholders"; ++s[3]; }
            cell["status"] = status;
            tr[lang] = cell;
        }
        e["tr"] = tr;
        entries.push_back(std::move(e));
    }
    out["entries"] = entries;
    json summary = json::object();
    for (const auto& [lang, s] : sum) summary[lang] = json{{"total", s[0]}, {"missing", s[1]}, {"extra", s[2]}, {"placeholders", s[3]}};
    out["summary"] = summary;
    // the text report of --check-locale (without the source scan)
    loc::Locale l;
    std::string err;
    l.Load(root, &err);
    loc::CheckOptions o;
    for (const auto& [k, v] : dataTexts) o.dataKeys.insert(k);
    out["report"] = l.Loaded() ? loc::Check(l, o).text : std::string("cannot load data/locale: ") + err;
    return out;
}

}  // namespace editor

}  // namespace f3
