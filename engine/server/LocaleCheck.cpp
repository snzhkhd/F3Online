// f3_server --check-locale: coverage report of data/locale (see docs/localization.md).
#include <filesystem>
#include <iostream>
#include "Content.h"
#include "common/Localization.h"
#include "common/protocol_game.h"

namespace f3 {

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
    for (const PerkDef& p : db.perks) { o.dataKeys.insert("perk." + p.id + ".name"); if (!p.desc.empty()) o.dataKeys.insert("perk." + p.id + ".desc"); }
    for (const TraitDef& t : db.traits) { o.dataKeys.insert("trait." + t.id + ".name"); if (!t.desc.empty()) o.dataKeys.insert("trait." + t.id + ".desc"); }
    for (const QuestDef& q : db.quests) { o.dataKeys.insert("quest." + q.id + ".title"); if (!q.text.empty()) o.dataKeys.insert("quest." + q.id + ".text"); }
    for (const DialogDef& d : db.dialogs)
        for (const DlgNode& n : d.nodes) {
            if (n.stf == 0) o.dataKeys.insert("dialog." + d.id + "." + n.id + ".text");
            for (size_t k = 0; k < n.answers.size(); ++k)
                if (n.answers[k].stf == 0) o.dataKeys.insert("dialog." + d.id + "." + n.id + "." + (n.answers[k].id.empty() ? "a" + std::to_string(k + 1) : n.answers[k].id));
        }
    for (int i = 0; i < kSkillCount; ++i) {
        o.dataKeys.insert(std::string("skill.") + kSkillNames[i] + ".name");
        if (!db.skills[i].desc.empty()) o.dataKeys.insert(std::string("skill.") + kSkillNames[i] + ".desc");
    }
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

}  // namespace f3
