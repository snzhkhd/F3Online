#include "Migrate.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <iostream>
#include <fstream>
#include <ostream>
#include <set>
#include "Account.h"
#include "common/log.h"

namespace f3 {

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

const char* const kLegacyDirs[] = {"accounts", "mail", "world"};

int64_t FileTimeSec(const fs::path& p) {
    std::error_code ec;
    auto ft = fs::last_write_time(p, ec);
    if (ec) return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count());
}

// *.json files of a folder, sorted by name (".bak" / ".tmp" leftovers of the old atomic writer are ignored)
std::vector<fs::path> JsonFiles(const fs::path& dir) {
    std::vector<fs::path> v;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && it->path().extension() == ".json") v.push_back(it->path());
    std::sort(v.begin(), v.end());
    return v;
}

bool ReadJson(const fs::path& p, json& out, std::string& why) {
    std::ifstream f(p, std::ios::binary);
    if (!f) { why = "cannot open"; return false; }
    out = json::parse(f, nullptr, false);
    if (out.is_discarded()) { why = "not valid JSON"; return false; }
    return true;
}

std::string SafeMapFile(std::string s) {
    for (char& ch : s)
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_' && ch != '-' && ch != '.') ch = '_';
    return s;
}

std::string StrOr(const json& j, const char* k, const std::string& d = std::string()) {
    auto it = j.find(k);
    return it != j.end() && it->is_string() ? it->get<std::string>() : d;
}
int64_t IntOr(const json& j, const char* k, int64_t d = 0) {
    auto it = j.find(k);
    return it != j.end() && it->is_number() ? it->get<int64_t>() : d;
}
bool BoolOr(const json& j, const char* k, bool d = false) {
    auto it = j.find(k);
    return it != j.end() && it->is_boolean() ? it->get<bool>() : d;
}

void RenameLegacy(const fs::path& saves, MigrateReport& rep) {
    for (const char* n : kLegacyDirs) {
        const fs::path p = saves / n;
        std::error_code ec;
        if (!fs::is_directory(p, ec)) continue;
        fs::path target;
        for (int i = 0;; ++i) {
            target = saves / (std::string(n) + ".migrated" + (i ? "." + std::to_string(i) : std::string()));
            if (!fs::exists(target, ec)) break;
        }
        fs::rename(p, target, ec);
        if (ec) {
            rep.problems.push_back(std::string("cannot rename ") + p.string() + ": " + ec.message() + " (the data is in the database; the folder stays and is retried at the next start)");
            log::Error("migration: cannot rename %s: %s", p.string().c_str(), ec.message().c_str());
        } else {
            rep.renamed.push_back(p.string() + " -> " + target.string());
            log::Info("migration: old saves %s kept as %s", p.string().c_str(), target.string().c_str());
        }
    }
}

}  // namespace

bool HasLegacyJsonSaves(const fs::path& saves) {
    std::error_code ec;
    for (const char* n : kLegacyDirs) if (fs::is_directory(saves / n, ec)) return true;
    return false;
}

bool MigrateJsonSaves(Store& store, const fs::path& saves, bool force, MigrateReport& rep, std::string* err) {
    rep = MigrateReport{};
    rep.legacyFound = HasLegacyJsonSaves(saves);
    if (!rep.legacyFound) return true;
    if (!force) {
        if (store.Meta("json_migrated_at").has_value()) {   // committed earlier, the rename did not happen (crash): finish it
            log::Info("migration: the database already holds the old JSON saves; setting the old folders aside");
            RenameLegacy(saves, rep);
            return true;
        }
        if (!store.IsEmpty()) {
            log::Warn("old JSON saves found in %s but the database already holds data: they are NOT imported (run f3_server --migrate-json to merge them)", saves.string().c_str());
            return true;
        }
    }
    log::Info("migration: importing the old JSON saves of %s into %s", saves.string().c_str(), store.Path().string().c_str());

    Store::Tx tx(store);
    if (!tx.Ok()) { if (err) *err = store.LastError(); return false; }
    auto problem = [&](const std::string& m) { rep.problems.push_back(m); log::Warn("migration: %s", m.c_str()); };
    std::set<std::string> importedKeys;

    // ---- accounts (+ characters, friends, quests)
    std::error_code ec;
    if (fs::is_directory(saves / "accounts", ec))
        for (const fs::path& p : JsonFiles(saves / "accounts")) {
            const std::string fn = p.filename().string();
            json j;
            std::string why;
            if (!ReadJson(p, j, why) || !j.is_object()) { ++rep.accountsSkipped; problem("accounts/" + fn + ": " + (why.empty() ? "not an object" : why)); continue; }
            const std::string name = StrOr(j, "name");
            const std::vector<uint8_t> salt = crypto::FromHex(StrOr(j, "salt")), hash = crypto::FromHex(StrOr(j, "hash"));
            if (!AccountStore::ValidName(name) || salt.empty() || hash.empty()) {
                ++rep.accountsSkipped;
                problem("accounts/" + fn + ": missing / invalid name, salt or hash");
                continue;
            }
            const std::string key = AccountStore::Key(name);
            if (store.AccountExists(key)) {
                ++rep.accountsSkipped;
                if (importedKeys.count(key)) problem("accounts/" + fn + ": duplicate of an account imported from another file (case-insensitive name)");
                continue;
            }
            AccountRow a;
            a.key = key;
            a.name = name;
            a.algo = StrOr(j, "algo", "pbkdf2-sha256");
            a.iterations = static_cast<int>(IntOr(j, "iterations", 10000));
            a.salt = salt;
            a.hash = hash;
            a.created = FileTimeSec(p);
            const json chars = j.contains("characters") && j["characters"].is_array() ? j["characters"] : json::array();
            const json first = !chars.empty() && chars[0].is_object() ? chars[0] : json{{"new", true}, {"name", name}};
            if (!store.CreateAccount(a, first)) { ++rep.accountsSkipped; problem("accounts/" + fn + ": database refused it (" + store.LastError() + ")"); continue; }
            ++rep.accounts;
            ++rep.characters;
            importedKeys.insert(key);
            for (size_t i = 1; i < chars.size(); ++i)
                if (chars[i].is_object() && store.SaveCharacter(key, static_cast<int>(i), chars[i])) ++rep.characters;
        }

    // ---- mail
    if (fs::is_directory(saves / "mail", ec))
        for (const fs::path& p : JsonFiles(saves / "mail")) {
            const std::string key = AccountStore::Key(p.stem().string());
            json j;
            std::string why;
            if (!ReadJson(p, j, why) || !j.is_array()) { ++rep.mailSkipped; problem("mail/" + p.filename().string() + ": " + (why.empty() ? "not an array" : why)); continue; }
            if (!importedKeys.count(key)) {
                rep.mailSkipped += static_cast<int>(j.size());
                if (!store.AccountExists(key)) problem("mail/" + p.filename().string() + ": no such account, " + std::to_string(j.size()) + " message(s) not imported");
                continue;
            }
            std::vector<json> msgs;
            for (const json& m : j) if (m.is_object() && IntOr(m, "id", 0) > 0) msgs.push_back(m);
            std::stable_sort(msgs.begin(), msgs.end(), [](const json& a, const json& b) { return IntOr(a, "id") < IntOr(b, "id"); });
            for (const json& m : msgs) {
                const uint32_t id = store.MailAdd(key, StrOr(m, "from"), StrOr(m, "text"), IntOr(m, "time", 0));
                if (!id) { ++rep.mailSkipped; problem("mail/" + p.filename().string() + ": message " + std::to_string(IntOr(m, "id")) + " refused by the database"); continue; }
                if (BoolOr(m, "read")) store.MailMarkRead(key, id);
                ++rep.mail;
            }
        }

    // ---- world objects
    if (fs::is_directory(saves / "world", ec))
        for (const fs::path& p : JsonFiles(saves / "world")) {
            json doc;
            std::string why;
            if (!ReadJson(p, doc, why) || !doc.is_object() || !doc.contains("objects") || !doc["objects"].is_array()) {
                ++rep.worldMapsSkipped;
                problem("world/" + p.filename().string() + ": " + (why.empty() ? "no objects array" : why));
                continue;
            }
            const std::string map = StrOr(doc, "map", p.stem().string());
            if (store.WorldObjectCount(map) > 0) { ++rep.worldMapsSkipped; continue; }
            std::vector<WorldObjRow> rows;
            for (const json& o : doc["objects"]) {
                if (!o.is_object() || StrOr(o, "key").empty()) continue;
                WorldObjRow r;
                r.key = StrOr(o, "key");
                r.open = BoolOr(o, "open");
                r.locked = BoolOr(o, "locked");
                r.flags = static_cast<int>(IntOr(o, "flags"));
                r.dc = static_cast<int>(IntOr(o, "dc"));
                r.lockKey = StrOr(o, "lock_key");
                if (o.contains("contents") && o["contents"].is_array()) r.contents = o["contents"].dump(-1, ' ', false, json::error_handler_t::replace);
                rows.push_back(std::move(r));
            }
            if (!store.SaveWorldObjects(map, rows)) { ++rep.worldMapsSkipped; problem("world/" + p.filename().string() + ": database refused it"); continue; }
            ++rep.worldMaps;
            rep.worldObjects += static_cast<int>(rows.size());
        }

    store.SetMeta("json_migrated_at", std::to_string(static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count())));
    store.LogEvent("migrate_json", "server",
                   {{"accounts", rep.accounts}, {"characters", rep.characters}, {"mail", rep.mail}, {"world_maps", rep.worldMaps}, {"world_objects", rep.worldObjects},
                    {"accounts_skipped", rep.accountsSkipped}, {"mail_skipped", rep.mailSkipped}, {"problems", static_cast<int>(rep.problems.size())}, {"forced", force}});
    if (!tx.Commit()) {
        store.ResetCaches();
        if (err) *err = "committing the import failed: " + store.LastError();
        return false;
    }
    rep.imported = true;
    log::Info("migration: %d account(s), %d character(s), %d mail(s), %d map(s) with %d object(s) imported; %zu problem(s)", rep.accounts, rep.characters,
              rep.mail, rep.worldMaps, rep.worldObjects, rep.problems.size());
    RenameLegacy(saves, rep);
    return true;
}

// ------------------------------------------------------------------ export

bool ExportJson(Store& store, const fs::path& dir, std::string* err) {
    try {
        Db& db = store.Raw();
        std::error_code ec;
        fs::create_directories(dir / "accounts", ec);
        fs::create_directories(dir / "mail", ec);
        fs::create_directories(dir / "world", ec);
        std::vector<std::string> keys;
        {
            Stmt s = db.Prepare("SELECT acc_key FROM accounts ORDER BY acc_key");
            while (s.Step()) keys.push_back(s.ColText(0));
        }
        for (const std::string& key : keys) {
            auto a = store.GetAccount(key);
            if (!a) continue;
            json j;
            j["name"] = a->name;
            j["algo"] = a->algo;
            j["iterations"] = a->iterations;
            j["salt"] = crypto::Hex(a->salt);
            j["hash"] = crypto::Hex(a->hash);
            j["characters"] = json::array();
            std::vector<int> slots;
            {
                Stmt s = db.Prepare("SELECT slot FROM characters WHERE account_id=? ORDER BY slot");
                s.BindInt(1, a->id);
                while (s.Step()) slots.push_back(static_cast<int>(s.ColInt(0)));
            }
            for (int slot : slots) {
                json ch;
                if (store.LoadCharacter(key, slot, &ch) != Store::Load::Ok) throw DbError("character " + key + "/" + std::to_string(slot) + " is unreadable");
                while (static_cast<int>(j["characters"].size()) < slot) j["characters"].push_back(json::object());
                j["characters"].push_back(std::move(ch));
            }
            if (!WriteFileAtomic(dir / "accounts" / ("acc_" + key + ".json"), j.dump(1))) throw DbError("cannot write " + (dir / "accounts" / ("acc_" + key + ".json")).string());
            const std::vector<MailRow> mail = store.MailList(key);
            if (!mail.empty()) {
                json arr = json::array();
                uint32_t n = 0;
                for (const MailRow& m : mail) arr.push_back({{"id", ++n}, {"from", m.from}, {"text", m.text}, {"time", m.time}, {"read", m.read}});
                if (!WriteFileAtomic(dir / "mail" / (key + ".json"), arr.dump())) throw DbError("cannot write mail of " + key);
            }
        }
        std::vector<std::string> maps;
        {
            Stmt s = db.Prepare("SELECT DISTINCT map FROM world_objects ORDER BY map");
            while (s.Step()) maps.push_back(s.ColText(0));
        }
        for (const std::string& map : maps) {
            json objs = json::array();
            for (const WorldObjRow& r : store.LoadWorldObjects(map)) {
                json o = {{"key", r.key}, {"open", r.open}, {"locked", r.locked}, {"flags", r.flags}, {"dc", r.dc}, {"lock_key", r.lockKey}};
                if (!r.contents.empty()) { json c = json::parse(r.contents, nullptr, false); if (c.is_array()) o["contents"] = c; }
                objs.push_back(std::move(o));
            }
            json doc = {{"version", 1}, {"map", map}, {"objects", objs}};
            if (!WriteFileAtomic(dir / "world" / (SafeMapFile(map) + ".json"), doc.dump(1))) throw DbError("cannot write world state of " + map);
        }
        return true;
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return false;
    }
}

// ------------------------------------------------------------------ dump

bool DumpDb(Store& store, std::ostream& os, const std::string& table, std::string* err) {
    try {
        Db& db = store.Raw();
        const DbInfo info = store.Info();
        os << "# " << info.path << "  schema v" << info.schemaVersion << "  journal " << info.journalMode << "  " << info.fileBytes << " bytes\n";
        static const char* const kTables[] = {"schema_version", "meta", "accounts", "characters", "friends", "quests", "mail", "world_objects", "events"};
        bool any = false;
        for (const char* t : kTables) {
            if (!table.empty() && table != t) continue;
            any = true;
            Stmt s = db.Prepare(std::string("SELECT * FROM ") + t);
            int64_t n = 0;
            std::string rows;
            while (s.Step()) {
                json row = json::object();
                for (int c = 0; c < s.ColCount(); ++c) {
                    const std::string cn = s.ColName(c);
                    switch (s.ColType(c)) {
                        case 1: row[cn] = s.ColInt(c); break;
                        case 2: row[cn] = s.ColReal(c); break;
                        case 3: {
                            std::string v = s.ColText(c);
                            if (cn == "data" || cn == "contents") { json p = json::parse(v, nullptr, false); if (!p.is_discarded()) { row[cn] = std::move(p); break; } }
                            row[cn] = std::move(v);
                            break;
                        }
                        case 4: row[cn] = "(blob " + std::to_string(s.ColBlob(c).size()) + " bytes)"; break;
                        default: row[cn] = nullptr; break;
                    }
                }
                rows += row.dump(-1, ' ', false, json::error_handler_t::replace) + "\n";
                ++n;
            }
            os << "## " << t << " (" << n << " rows)\n" << rows;
        }
        if (!any) { if (err) *err = "unknown table '" + table + "'"; return false; }
        return true;
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return false;
    }
}

int RunDbTool(const std::string& tool, const fs::path& saves, const std::string& arg) {
    const fs::path dbPath = saves / "f3.db";
    Store store;
    std::string err;
    const bool write = tool == "migrate-json";
    if (!store.Open(dbPath, &err, !write)) { std::cerr << "error: " << err << std::endl; return 1; }
    if (tool == "migrate-json") {
        MigrateReport rep;
        if (!MigrateJsonSaves(store, saves, true, rep, &err)) { std::cerr << "error: " << err << std::endl; return 1; }
        if (!rep.legacyFound) { std::cout << "nothing to migrate: no accounts/, mail/ or world/ folder in " << saves.string() << std::endl; return 0; }
        std::cout << "imported " << rep.accounts << " account(s), " << rep.characters << " character(s), " << rep.mail << " mail(s), " << rep.worldMaps << " map(s) with "
                  << rep.worldObjects << " object(s); skipped " << rep.accountsSkipped << " account(s), " << rep.mailSkipped << " mail(s), " << rep.worldMapsSkipped << " map(s)\n";
        for (const std::string& r : rep.renamed) std::cout << "kept: " << r << "\n";
        for (const std::string& p : rep.problems) std::cout << "problem: " << p << "\n";
        std::cout << std::flush;
        return 0;
    }
    if (tool == "export-json") {
        if (!ExportJson(store, arg, &err)) { std::cerr << "error: " << err << std::endl; return 1; }
        std::cout << "exported to " << arg << " (the account files contain the password hashes)" << std::endl;
        return 0;
    }
    if (tool == "db-dump") {
        if (!DumpDb(store, std::cout, arg, &err)) { std::cerr << "error: " << err << std::endl; return 1; }
        return 0;
    }
    if (tool == "db-backup") {
        if (arg.empty() || !store.Backup(arg)) { std::cerr << "error: " << (arg.empty() ? "no target file" : store.LastError()) << std::endl; return 1; }
        std::cout << "backup written to " << arg << std::endl;
        return 0;
    }
    std::cerr << "unknown tool " << tool << std::endl;
    return 1;
}

}  // namespace f3
