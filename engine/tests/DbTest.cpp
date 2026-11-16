// Unit-style tests of the persistence layer (SQLite store, schema migrations, import of the old JSON saves, export, corrupt files).
// No server, no network, no game data: everything happens in a throw-away temp directory.
//   f3_dbtest             exit code 0 = all passed
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>
#include "Account.h"
#include "Migrate.h"
#include "Store.h"

using namespace f3;
using json = nlohmann::json;
namespace fs = std::filesystem;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond, ...) do { ++g_checks; if (!(cond)) { ++g_failed; std::printf("  FAIL %s:%d  ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static fs::path g_root;

static fs::path Fresh(const std::string& name) {
    fs::path p = g_root / name;
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p);
    return p;
}
static void Put(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << text;
}
static std::string Slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}
static bool Has(const std::vector<std::string>& v, const std::string& needle) {
    for (const std::string& s : v) if (s.find(needle) != std::string::npos) return true;
    return false;
}

// ---- the character document as Persist.cpp: CharacterToJson writes it (all the keys the server saves)
static json MakeCharacter(const std::string& name, const std::string& map, int x, int y, int level, int xp) {
    json j;
    j["version"] = 1;
    j["name"] = name;
    j["saved_at"] = 1700000000;
    j["map"] = map;
    j["x"] = x; j["y"] = y; j["dir"] = 3;
    j["hp"] = 31; j["max_hp"] = 40; j["dead"] = false;
    j["level"] = level; j["xp"] = xp;
    j["special"] = {5, 6, 7, 4, 5, 8, 9};
    j["skill_points"] = 12;
    j["skill_ranks"] = {{"small_guns", 20}, {"lockpick", 5}};
    j["unspent_special"] = 0;
    j["tags"] = 5; j["traits"] = 3; j["perk_points"] = 1;
    j["perks"] = {{"toughness", 2}};
    j["created"] = true;
    j["proto"] = "Default_PC.CRT";
    j["inventory"] = json::array({{{"proto", "Pistol.WEA"}, {"count", 1}, {"slot", 1}}, {{"proto", "Ammo9.AMO"}, {"count", 30}, {"slot", 0}}});
    j["vars"] = {{"steps", 123.0}};
    j["quests"] = json::array({{{"id", "q1"}, {"title", "First"}, {"text", "Do it"}, {"state", 0}, {"updated", 1700000100}},
                               {{"id", "q2"}, {"title", "Second"}, {"text", "Done"}, {"state", 1}, {"updated", 1700000200}}});
    j["friends"] = json::array({"Bob", "Carol"});
    j["gvars"] = {{"quest_x", 2.0}};
    return j;
}

static void Reg(AccountStore& as, const std::string& name, const std::string& pw) {
    const auto r = as.Register(name, pw, "127.0.0.1", json{{"new", true}, {"name", name}});
    CHECK(r == AccountStore::Result::Ok, "register %s", name.c_str());
}

// ============================================================================================================ schema + db layer

static void TestOpenAndSchema() {
    std::printf("open + schema\n");
    const fs::path dir = Fresh("schema");
    Store st;
    std::string err;
    CHECK(st.Open(dir / "f3.db", &err), "open fresh: %s", err.c_str());
    CHECK(st.Raw().SchemaVersion() == StoreMigrations().back().version, "schema_version %d", st.Raw().SchemaVersion());
    CHECK(st.Raw().JournalMode() == "wal", "journal mode %s", st.Raw().JournalMode().c_str());
    CHECK(st.Raw().ScalarInt("PRAGMA foreign_keys") == 1, "foreign keys on");
    const DbInfo info = st.Info();
    CHECK(info.tables.size() >= 8 && info.schemaVersion == st.Raw().SchemaVersion(), "info lists the tables");
    CHECK(st.IsEmpty(), "fresh db is empty");
    st.Close();
    CHECK(st.Open(dir / "f3.db", &err), "reopen: %s", err.c_str());
    CHECK(st.Raw().Migrate(StoreMigrations()) == 0, "reopen applies no migration again");
    // a later program version: one more migration is applied once, a backup of the old file is kept
    st.Raw().Close();
    Db db;
    CHECK(db.Open(dir / "f3.db", Db::Options{}, &err), "db open: %s", err.c_str());
    std::vector<DbMigration> m2 = StoreMigrations();
    m2.push_back({m2.back().version + 1, "test column", "ALTER TABLE accounts ADD COLUMN note TEXT;"});
    CHECK(db.Migrate(m2) == 1, "second schema version applied");
    CHECK(db.SchemaVersion() == m2.back().version, "version bumped");
    bool bak = false;
    for (auto& e : fs::directory_iterator(dir)) if (e.path().filename().string().find(".pre-v") != std::string::npos) bak = true;
    CHECK(bak, "pre-migration backup written");
    db.Close();
    // an OLDER program must refuse a newer database (and not touch it)
    CHECK(!st.Open(dir / "f3.db", &err) && err.find("newer") != std::string::npos, "older program refuses a newer schema: %s", err.c_str());
    // a failing migration rolls back completely
    const fs::path dir2 = Fresh("schema_fail");
    CHECK(db.Open(dir2 / "x.db", Db::Options{}, &err), "open x");
    bool threw = false;
    try { db.Migrate({{1, "bad", "CREATE TABLE a(x); CREATE TABLE a(y);"}}); } catch (const DbError&) { threw = true; }
    CHECK(threw && db.SchemaVersion() == 0 && db.ScalarInt("SELECT count(*) FROM sqlite_master WHERE name='a'") == 0, "failed migration leaves nothing behind");
    db.Close();
}

static void TestTransactions() {
    std::printf("transactions + threading\n");
    const fs::path dir = Fresh("tx");
    Store st;
    std::string err;
    CHECK(st.Open(dir / "f3.db", &err), "open");
    {   // rolled back: nothing visible
        Store::Tx tx(st);
        st.SaveWorldObjects("m", {{"door@1,1", true, false, 0, 0, "", ""}});
        CHECK(st.WorldObjectCount("m") == 1, "visible inside the transaction");
    }
    CHECK(st.WorldObjectCount("m") == 0, "rolled back without Commit");
    {   // nested: inner rollback keeps the outer work
        Store::Tx outer(st);
        st.SaveWorldObjects("a", {{"d@1,1", false, false, 0, 0, "", ""}});
        {
            Store::Tx inner(st);
            st.SaveWorldObjects("b", {{"d@2,2", false, false, 0, 0, "", ""}});
        }
        outer.Commit();
    }
    CHECK(st.WorldObjectCount("a") == 1 && st.WorldObjectCount("b") == 0, "inner savepoint rolled back, outer committed");
    // a save of an unknown account fails as a whole
    CHECK(!st.SaveCharacter("nobody", 0, MakeCharacter("nobody", "m", 1, 1, 1, 0)), "character of an unknown account is refused");
    // another thread must not touch the connection
    bool ok = true;
    std::string why;
    std::thread t([&] { ok = st.AccountExists("x"); why = st.LastError(); });
    t.join();
    CHECK(!ok && why.find("thread") != std::string::npos, "use from another thread is a clear error: %s", why.c_str());
    CHECK(st.IsOpen() && st.AccountCount() == 0, "store still usable on its own thread");
}

// ============================================================================================================ round trips

// UTF-8 (Cyrillic) names, chat text and quest texts: stored and read back byte for byte, account names fold case across Latin and Cyrillic.
static void TestUtf8() {
    std::printf("utf-8\n");
    const fs::path dir = Fresh("utf8");
    std::string err;
    const std::string stalker = u8"\u0421\u0442\u0430\u043b\u043a\u0435\u0440";      // Stalker
    const std::string stalkerUp = u8"\u0421\u0422\u0410\u041b\u041a\u0415\u0420";    // STALKER (upper case)
    const std::string stalkerLow = u8"\u0441\u0442\u0430\u043b\u043a\u0435\u0440";   // stalker (lower case)
    Store st;
    CHECK(st.Open(dir / "f3.db", &err), "open");
    AccountStore as;
    as.Init(&st, 1000);
    Reg(as, stalker, "secret1");
    CHECK(as.Exists(stalkerUp) && as.Exists(stalkerLow), "Cyrillic account names are case-insensitive");
    CHECK(as.Register(stalkerLow, "other", "1.1.1.1", json::object()) == AccountStore::Result::NameTaken, "name taken across letter case (Cyrillic)");
    CHECK(as.Register(u8"\u0418\u0433\u0440 ok", "secret1", "1.1.1.1", json::object()) == AccountStore::Result::InvalidName, "a space is not a name character");
    json ch;
    CHECK(as.Login(stalkerUp, "secret1", "2.2.2.2", &ch) == AccountStore::Result::Ok && ch.value("name", "") == stalker, "login with another letter case returns the stored spelling");
    json full = MakeCharacter(stalker, "00_03_Tutorial_Junktown", 1, 2, 3, 4);
    full["quests"] = json::array({{{"id", "q1"}, {"title", u8"\u041f\u0435\u0440\u0432\u044b\u0435 \u0448\u0430\u0433\u0438"}, {"text", u8"\u041f\u043e\u0433\u043e\u0432\u043e\u0440\u0438\u0442\u0435 \u0441 \u043c\u0435\u0441\u0442\u043d\u044b\u043c\u0438"}, {"state", 0}, {"updated", 1}}});
    full["friends"] = json::array({u8"\u0414\u0440\u0443\u0433"});
    CHECK(as.SaveCharacter(stalker, 0, full), "save a character with Cyrillic texts");
    json back;
    CHECK(st.LoadCharacter(stalkerLow, 0, &back) == Store::Load::Ok && back == full, "Cyrillic character document round trips exactly");
    CHECK(st.Raw().ScalarText("SELECT name FROM characters LIMIT 1") == stalker, "the name column holds UTF-8: %s", st.Raw().ScalarText("SELECT name FROM characters LIMIT 1").c_str());
    const std::string mailText = u8"\u041f\u0440\u0438\u0432\u0435\u0442 \u0438\u0437 \u043f\u0443\u0441\u0442\u043e\u0448\u0438";
    const uint32_t id = st.MailAdd(AccountStore::Key(stalkerUp), u8"\u0414\u0440\u0443\u0433", mailText, 1700000000);
    auto box = st.MailList(AccountStore::Key(stalkerUp));
    CHECK(id && box.size() == 1 && box[0].text == mailText && box[0].from == u8"\u0414\u0440\u0443\u0433", "mail text and sender in Cyrillic round trip");
    CHECK(!st.WhoHasFriend(u8"\u0434\u0440\u0443\u0433").empty() && st.WhoHasFriend(u8"\u0434\u0440\u0443\u0433")[0] == stalker, "friend lookup folds Cyrillic case");
}

static void TestRoundTrip() {
    std::printf("round trip\n");
    const fs::path dir = Fresh("rt");
    std::string err;
    {
        Store st;
        CHECK(st.Open(dir / "f3.db", &err), "open");
        AccountStore as;
        as.Init(&st, 1000);
        CHECK(!as.Exists("Alice"), "no account yet");
        Reg(as, "Alice", "secret1");
        Reg(as, "Bob", "secret2");
        CHECK(as.Register("alice", "other", "1.1.1.1", json::object()) == AccountStore::Result::NameTaken, "name taken case-insensitively");
        CHECK(as.Register("A", "secret1", "1.1.1.1", json::object()) == AccountStore::Result::InvalidName, "invalid name");
        CHECK(as.Register("Dave", "x", "1.1.1.1", json::object()) == AccountStore::Result::WeakPassword, "weak password");
        CHECK(as.Exists("ALICE"), "exists");

        json ch;
        CHECK(as.Login("alice", "wrong", "2.2.2.2", &ch) == AccountStore::Result::BadCredentials, "wrong password");
        CHECK(as.Login("nobody", "secret1", "2.2.2.2", &ch) == AccountStore::Result::BadCredentials, "unknown account");
        CHECK(as.Login("ALICE", "secret1", "2.2.2.2", &ch) == AccountStore::Result::Ok, "login (case-insensitive)");
        CHECK(ch.value("new", false) && ch.value("name", "") == "Alice", "a new account logs in with its placeholder character");
        auto acc = st.GetAccount("alice");
        CHECK(acc && acc->name == "Alice" && acc->iterations == 1000 && acc->salt.size() == 16 && acc->hash.size() == 32 && acc->lastLogin > 0 && acc->created > 0,
              "account row: name, pbkdf2 parameters, created, last_login");

        const json full = MakeCharacter("Alice", "00_03_Tutorial_Junktown", 120, 80, 4, 2500);
        CHECK(as.SaveCharacter("Alice", 0, full), "save character");
        json back;
        CHECK(st.LoadCharacter("alice", 0, &back) == Store::Load::Ok && back == full, "character document round trips exactly (%s)", back.dump().substr(0, 80).c_str());
        CHECK(as.Login("Alice", "secret1", "2.2.2.2", &ch) == AccountStore::Result::Ok && ch == full, "login returns the saved document");
        // searchable columns
        CHECK(st.Raw().ScalarInt("SELECT level FROM characters WHERE name='Alice'") == 4 && st.Raw().ScalarInt("SELECT xp FROM characters WHERE name='Alice'") == 2500, "level / xp columns");
        CHECK(st.Raw().ScalarText("SELECT map FROM characters WHERE name='Alice'") == "00_03_Tutorial_Junktown" && st.Raw().ScalarInt("SELECT x FROM characters WHERE name='Alice'") == 120 &&
              st.Raw().ScalarInt("SELECT y FROM characters WHERE name='Alice'") == 80 && st.Raw().ScalarInt("SELECT last_seen FROM characters WHERE name='Alice'") == 1700000000, "map / pos / last_seen columns");
        CHECK(st.Raw().ScalarInt("SELECT count(*) FROM quests") == 2 && st.Raw().ScalarInt("SELECT count(*) FROM friends") == 2, "quests and friends are rows");
        CHECK(st.Raw().ScalarText("SELECT data FROM characters WHERE name='Alice'").find("quests") == std::string::npos, "the blob holds no quests / friends");
        CHECK(st.WhoHasFriend("bob").size() == 1 && st.WhoHasFriend("BOB")[0] == "Alice" && st.WhoHasFriend("zed").empty(), "who lists a player as friend");
        // update shrinks the lists
        json upd = full;
        upd["level"] = 5;
        upd["friends"] = json::array({"Carol"});
        upd["quests"] = json::array({full["quests"][0]});
        upd["quests"][0]["state"] = 2;
        CHECK(as.SaveCharacter("alice", 0, upd), "second save");
        CHECK(st.LoadCharacter("alice", 0, &back) == Store::Load::Ok && back == upd, "update replaces the sub tables");
        CHECK(st.Raw().ScalarInt("SELECT count(*) FROM characters") == 2, "one character row per account (upsert)");

        // mail
        const uint32_t m1 = st.MailAdd("alice", "Bob", "hello", 1700000300), m2 = st.MailAdd("alice", "Bob", "second", 1700000400);
        CHECK(m1 && m2 && m2 > m1 && st.MailAdd("nobody", "x", "y", 1) == 0, "mail ids increase, unknown recipient refused");
        CHECK(st.MailCount("alice") == 2 && st.MailUnread("alice") == 2 && st.MailCount("bob") == 0, "mail counts per account");
        CHECK(st.MailMarkRead("alice", m1) == 1 && st.MailMarkRead("alice", m1) == 0 && st.MailMarkRead("alice", 9999) == -1 && st.MailMarkRead("bob", m2) == -1, "mark read: changed / already / unknown / not yours");
        auto box = st.MailList("alice");
        CHECK(box.size() == 2 && box[0].id == m1 && box[0].read && box[0].from == "Bob" && box[0].text == "hello" && box[0].time == 1700000300 && !box[1].read && st.MailUnread("alice") == 1, "mail list");
        CHECK(!st.MailDelete("bob", m1) && st.MailDelete("alice", m1) && st.MailCount("alice") == 1, "delete only own mail");

        // world objects
        std::vector<WorldObjRow> rows = {{"Door1.DOR@10,10", true, false, 1, 20, "key7", ""}, {"Chest.CON@5,6", false, true, 0, 30, "", R"([{"proto":"Ammo9.AMO","count":3}])"}};
        CHECK(st.SaveWorldObjects("town", rows), "save world objects");
        auto w = st.LoadWorldObjects("town");
        CHECK(w.size() == 2 && w[0].key == "Chest.CON@5,6" && w[0].locked && w[0].dc == 30 && w[0].contents == rows[1].contents && w[1].open && w[1].flags == 1 && w[1].lockKey == "key7" && w[1].contents.empty(), "world objects round trip");
        rows.pop_back();
        CHECK(st.SaveWorldObjects("town", rows) && st.LoadWorldObjects("town").size() == 1 && st.LoadWorldObjects("other").empty(), "a save replaces the rows of that map only");

        // events
        CHECK(st.LogEvent("login", "Alice", {{"ip", "1.2.3.4"}}) && st.LogEvent("kill", "Alice", {{"victim", "Rat"}}) && st.LogEvent("login", "Bob"), "log events");
        auto ev = st.RecentEvents(10);
        CHECK(ev.size() == 3 && ev[0].actor == "Bob" && ev[2].kind == "login" && json::parse(ev[2].data)["ip"] == "1.2.3.4", "events newest first with their json");
        CHECK(st.RecentEvents(10, "login").size() == 2 && st.RecentEvents(10, "", "alice").size() == 2 && st.RecentEvents(1).size() == 1 && st.EventCount() == 3, "event filters");
    }
    {   // everything survives a restart
        Store st;
        CHECK(st.Open(dir / "f3.db", &err), "reopen: %s", err.c_str());
        json back;
        CHECK(st.LoadCharacter("alice", 0, &back) == Store::Load::Ok && back["level"] == 5 && back["friends"].size() == 1, "character after restart");
        CHECK(st.MailCount("alice") == 1 && st.LoadWorldObjects("town").size() == 1 && st.EventCount() == 3, "mail / world / events after restart");
        AccountStore as;
        as.Init(&st, 1000);
        json ch;
        CHECK(as.Login("Alice", "secret1", "3.3.3.3", &ch) == AccountStore::Result::Ok, "password verifies after restart");
        // the character row of an account with unreadable data must refuse the login, never hand out a fresh character
        st.Raw().Exec("UPDATE characters SET data='{broken' WHERE name='Alice'");
        CHECK(as.Login("Alice", "secret1", "3.3.3.3", &ch) == AccountStore::Result::Error, "unreadable character blob refuses the login");
        CHECK(st.Raw().ScalarText("SELECT data FROM characters WHERE name='Alice'") == "{broken", "and the row is left as it was");
    }
}

// ============================================================================================================ JSON import

// Old format fixtures: accounts/acc_<key>.json, mail/<key>.json, world/<map>.json (what the previous server wrote)
static void MakeOldAccount(const fs::path& saves, const std::string& name, const std::string& pw, const json& chars) {
    const auto salt = crypto::RandomBytes(16);
    json j;
    j["name"] = name;
    j["algo"] = "pbkdf2-sha256";
    j["iterations"] = 1000;
    j["salt"] = crypto::Hex(salt);
    j["hash"] = crypto::Hex(crypto::Pbkdf2Sha256(pw, salt, 1000));
    j["characters"] = chars;
    Put(saves / "accounts" / ("acc_" + AccountStore::Key(name) + ".json"), j.dump(1));
}

static fs::path MakeOldSaves(const std::string& name) {
    const fs::path saves = Fresh(name);
    MakeOldAccount(saves, "Alice", "secret1", json::array({MakeCharacter("Alice", "town", 11, 22, 3, 900)}));
    MakeOldAccount(saves, "Bob", "secret2", json::array({json{{"new", true}, {"name", "Bob"}}}));
    Put(saves / "accounts" / "acc_alice.json.bak", "{old backup, must be ignored}");
    Put(saves / "accounts" / "acc_broken.json", "{ this is not json");
    Put(saves / "accounts" / "acc_nosalt.json", R"({"name":"Nosalt","characters":[]})");
    Put(saves / "mail" / "alice.json", json::array({{{"id", 1}, {"from", "Bob"}, {"text", "first"}, {"time", 1700000001}, {"read", true}},
                                                      {{"id", 2}, {"from", "Bob"}, {"text", "second"}, {"time", 1700000002}, {"read", false}},
                                                      {{"id", 0}, {"from", "x"}, {"text", "dropped like the old loader did"}, {"time", 1}, {"read", false}}}).dump());
    Put(saves / "mail" / "ghost.json", json::array({{{"id", 1}, {"from", "Bob"}, {"text", "to nobody"}, {"time", 5}, {"read", false}}}).dump());
    json doc = {{"version", 1}, {"map", "town"}, {"objects", json::array({{{"key", "Door1.DOR@10,10"}, {"open", true}, {"locked", false}, {"flags", 1}, {"dc", 20}, {"lock_key", "k"}},
                                                                          {{"key", "Chest.CON@5,6"}, {"open", false}, {"locked", true}, {"flags", 0}, {"dc", 30}, {"lock_key", ""},
                                                                           {"contents", json::array({{{"proto", "Ammo9.AMO"}, {"count", 3}}})}}})}};
    Put(saves / "world" / "town.json", doc.dump(1));
    Put(saves / "world" / "bad.json", "[1,2");
    return saves;
}

static void TestMigration() {
    std::printf("json migration\n");
    const fs::path saves = MakeOldSaves("mig");
    const std::string aliceFile = Slurp(saves / "accounts" / "acc_alice.json");
    std::string err;
    Store st;
    CHECK(st.Open(saves / "f3.db", &err), "open: %s", err.c_str());
    MigrateReport rep;
    CHECK(MigrateJsonSaves(st, saves, false, rep, &err), "migrate: %s", err.c_str());
    CHECK(rep.legacyFound && rep.imported && rep.accounts == 2 && rep.characters == 2 && rep.mail == 2 && rep.worldMaps == 1 && rep.worldObjects == 2,
          "counts: %d accounts %d chars %d mail %d maps %d objs", rep.accounts, rep.characters, rep.mail, rep.worldMaps, rep.worldObjects);
    CHECK(Has(rep.problems, "acc_broken.json") && Has(rep.problems, "acc_nosalt.json") && Has(rep.problems, "ghost.json") && Has(rep.problems, "bad.json"),
          "every unreadable file / orphan mailbox is reported with its name");
    // old folders set aside, never deleted
    CHECK(!fs::exists(saves / "accounts") && !fs::exists(saves / "mail") && !fs::exists(saves / "world"), "old folders are renamed");
    CHECK(Slurp(saves / "accounts.migrated" / "acc_alice.json") == aliceFile && fs::exists(saves / "accounts.migrated" / "acc_broken.json") &&
          fs::exists(saves / "accounts.migrated" / "acc_alice.json.bak") && fs::exists(saves / "mail.migrated" / "ghost.json") && fs::exists(saves / "world.migrated" / "bad.json"),
          "the backup holds every original file untouched, also the ones that could not be imported");
    // the data is usable
    AccountStore as;
    as.Init(&st, 1000);
    json ch;
    CHECK(as.Login("alice", "secret1", "1.1.1.1", &ch) == AccountStore::Result::Ok, "the old password hash still verifies");
    CHECK(ch == MakeCharacter("Alice", "town", 11, 22, 3, 900), "character document identical to the old file");
    CHECK(as.Login("BOB", "secret2", "1.1.1.1", &ch) == AccountStore::Result::Ok && ch.value("new", false), "never played account stays 'new'");
    auto box = st.MailList("alice");
    CHECK(box.size() == 2 && box[0].text == "first" && box[0].read && box[0].time == 1700000001 && box[1].text == "second" && !box[1].read, "mail imported in order with the read flag");
    auto w = st.LoadWorldObjects("town");
    CHECK(w.size() == 2 && w[1].open && w[1].lockKey == "k" && w[0].locked && w[0].contents.find("Ammo9.AMO") != std::string::npos, "world objects imported");
    CHECK(st.Meta("json_migrated_at").has_value() && !st.RecentEvents(5, "migrate_json").empty(), "marker + audit event written");
    CHECK(st.Raw().ScalarInt("SELECT count(*) FROM accounts WHERE created > 1000000000") == 2, "created time taken from the file");

    // idempotent: nothing to do now
    const int64_t before = st.Raw().ScalarInt("SELECT count(*) FROM characters") + st.MailCount("alice") + st.WorldObjectCount();
    MigrateReport again;
    CHECK(MigrateJsonSaves(st, saves, false, again, &err) && !again.legacyFound && !again.imported, "second run: no old folders, nothing happens");
    CHECK(MigrateJsonSaves(st, saves, true, again, &err) && !again.legacyFound, "--migrate-json with no old folders: nothing happens");
    // put the backup back and merge: existing accounts are skipped, nothing is duplicated
    fs::copy(saves / "accounts.migrated", saves / "accounts", fs::copy_options::recursive);
    fs::copy(saves / "mail.migrated", saves / "mail", fs::copy_options::recursive);
    fs::copy(saves / "world.migrated", saves / "world", fs::copy_options::recursive);
    CHECK(MigrateJsonSaves(st, saves, true, again, &err) && again.imported && again.accounts == 0 && again.mail == 0 && again.worldMaps == 0 && again.accountsSkipped >= 2,
          "--migrate-json on the same data again: everything skipped (accounts %d skipped)", again.accountsSkipped);
    const int64_t after = st.Raw().ScalarInt("SELECT count(*) FROM characters") + st.MailCount("alice") + st.WorldObjectCount();
    CHECK(before == after, "no duplicates after the repeated import");
    CHECK(fs::exists(saves / "accounts.migrated.1"), "second backup folder gets a numbered name instead of overwriting the first");
    CHECK(fs::exists(saves / "accounts.migrated" / "acc_alice.json"), "first backup still there");

    // a database that already holds data but was never migrated: old folders at a normal start are left alone (not imported, not renamed)
    {
        const fs::path s3 = MakeOldSaves("mig_nonempty");
        Store st3;
        CHECK(st3.Open(s3 / "f3.db", &err), "open 3");
        AccountStore as3;
        as3.Init(&st3, 1000);
        Reg(as3, "Zed", "secret9");
        MigrateReport r3;
        CHECK(MigrateJsonSaves(st3, s3, false, r3, &err) && r3.legacyFound && !r3.imported && r3.accounts == 0 && !st3.AccountExists("alice"), "non-empty database + old folders at start: nothing imported");
        CHECK(fs::exists(s3 / "accounts" / "acc_alice.json") && fs::exists(s3 / "mail") && fs::exists(s3 / "world"), "and the old folders stay where they are");
        CHECK(MigrateJsonSaves(st3, s3, true, r3, &err) && r3.imported && r3.accounts == 2 && st3.AccountExists("alice") && st3.AccountExists("zed"), "--migrate-json merges them");
    }

    // --migrate-json merges NEW data into a live database (an account that is missing)
    MakeOldAccount(saves, "Carol", "secret3", json::array({MakeCharacter("Carol", "town", 1, 1, 1, 0)}));
    MigrateReport merge;
    CHECK(MigrateJsonSaves(st, saves, true, merge, &err) && merge.accounts == 1 && st.AccountExists("carol"), "merge adds the missing account");

    // crash between commit and rename: the marker makes the next start only finish the rename
    MakeOldAccount(saves, "Dave", "secret4", json::array());
    MigrateReport fin;
    CHECK(MigrateJsonSaves(st, saves, false, fin, &err) && !fin.imported && !fs::exists(saves / "accounts") && !st.AccountExists("dave"),
          "marker set + folder present: renamed only, no second import");
}

static void TestMigrationRoundTripExport() {
    std::printf("export-json round trip\n");
    const fs::path saves = MakeOldSaves("exp");
    std::string err;
    Store st;
    CHECK(st.Open(saves / "f3.db", &err), "open");
    MigrateReport rep;
    CHECK(MigrateJsonSaves(st, saves, false, rep, &err), "import");
    const fs::path out = g_root / "exp_out";
    std::error_code ec;
    fs::remove_all(out, ec);
    CHECK(ExportJson(st, out, &err), "export: %s", err.c_str());
    CHECK(fs::exists(out / "accounts" / "acc_alice.json") && fs::exists(out / "mail" / "alice.json") && fs::exists(out / "world" / "town.json"), "files written in the old layout");
    // the exported folder imports into a fresh database and yields the same data
    const fs::path saves2 = Fresh("exp2");
    fs::copy(out, saves2, fs::copy_options::recursive);
    Store st2;
    CHECK(st2.Open(saves2 / "f3.db", &err), "open 2");
    MigrateReport rep2;
    CHECK(MigrateJsonSaves(st2, saves2, false, rep2, &err) && rep2.accounts == 2 && rep2.mail == 2 && rep2.worldObjects == 2 && rep2.problems.empty(), "re-import of the export: %zu problems", rep2.problems.size());
    json a, b;
    CHECK(st.LoadCharacter("alice", 0, &a) == Store::Load::Ok && st2.LoadCharacter("alice", 0, &b) == Store::Load::Ok && a == b, "character equal");
    CHECK(st.GetAccount("alice")->hash == st2.GetAccount("alice")->hash && st.GetAccount("alice")->salt == st2.GetAccount("alice")->salt, "password hash equal");
    auto m1 = st.MailList("alice"), m2 = st2.MailList("alice");
    CHECK(m1.size() == m2.size() && m1[1].text == m2[1].text && m1[0].read == m2[0].read, "mail equal");
    // dump never prints secrets
    std::ostringstream os;
    CHECK(DumpDb(st, os, "", &err), "dump");
    const std::string d = os.str();
    const auto acc = st.GetAccount("alice");
    CHECK(d.find("## accounts (2 rows)") != std::string::npos && d.find("## mail (2 rows)") != std::string::npos && d.find(crypto::Hex(acc->hash)) == std::string::npos &&
          d.find("(blob 32 bytes)") != std::string::npos && d.find("\"small_guns\"") != std::string::npos, "dump shows rows, hides hash / salt, expands the character JSON");
    std::ostringstream os2;
    CHECK(DumpDb(st, os2, "mail", &err) && os2.str().find("## accounts") == std::string::npos && !DumpDb(st, os2, "nope", &err), "dump of one table / unknown table");
}

// ============================================================================================================ corrupt files

static void TestCorrupt() {
    std::printf("corrupt database\n");
    std::string err;
    {   // not a database at all
        const fs::path dir = Fresh("bad1");
        const std::string junk = "This is definitely not a SQLite database file, just some text that somebody saved here by accident.\n";
        Put(dir / "f3.db", junk);
        Store st;
        CHECK(!st.Open(dir / "f3.db", &err), "garbage file refused");
        CHECK(err.find("NOT modified") != std::string::npos && err.find("f3.db") != std::string::npos, "clear message naming the file: %s", err.c_str());
        CHECK(Slurp(dir / "f3.db") == junk, "garbage file is byte-identical afterwards (not replaced, not 'repaired')");
        CHECK(!st.IsOpen(), "store is not open");
    }
    // a valid database with its pages destroyed / truncated
    const fs::path dir = Fresh("bad2");
    {
        Store st;
        CHECK(st.Open(dir / "f3.db", &err), "open");
        AccountStore as;
        as.Init(&st, 1000);
        for (int i = 0; i < 40; ++i) {
            const std::string n = "Player" + std::to_string(i);
            Reg(as, n, "password");
            as.SaveCharacter(n, 0, MakeCharacter(n, "town", i, i, 1 + i % 7, i * 10));
            st.MailAdd(AccountStore::Key(n), "Admin", std::string(300, 'x'), 1);
        }
    }   // closed: checkpointed into the main file
    const fs::path db = dir / "f3.db";
    const std::string good = Slurp(db);
    CHECK(good.size() > 8192, "database file has several pages (%zu bytes)", good.size());
    {
        std::string bad = good;
        for (size_t i = 4096; i < bad.size(); ++i) bad[i] = static_cast<char>(0xA5);
        Put(dir / "damaged.db", bad);
        Store st;
        CHECK(!st.Open(dir / "damaged.db", &err) && !err.empty(), "database with destroyed pages refused: %s", err.c_str());
        CHECK(Slurp(dir / "damaged.db") == bad, "damaged file untouched");
    }
    {
        std::string bad = good.substr(0, 4096 + 1000);
        Put(dir / "trunc.db", bad);
        Store st;
        CHECK(!st.Open(dir / "trunc.db", &err) && !err.empty(), "truncated database refused: %s", err.c_str());
        CHECK(Slurp(dir / "trunc.db") == bad, "truncated file untouched");
    }
    {   // the intact copy still opens and holds everything
        Store st;
        CHECK(st.Open(db, &err), "intact database opens: %s", err.c_str());
        CHECK(st.AccountCount() == 40 && st.MailCount("player7") == 1, "no data lost");
    }
}

static void TestBackup() {
    std::printf("backup\n");
    const fs::path dir = Fresh("bak");
    std::string err;
    Store st;
    CHECK(st.Open(dir / "f3.db", &err), "open");
    AccountStore as;
    as.Init(&st, 1000);
    Reg(as, "Alice", "secret1");
    CHECK(st.Backup(dir / "copy.db"), "backup while open");
    CHECK(!st.Backup(dir / "copy.db"), "never overwrites an existing backup");
    Store st2;
    CHECK(st2.Open(dir / "copy.db", &err) && st2.AccountExists("alice"), "backup opens and holds the account");
}

int main() {
    g_root = fs::temp_directory_path() / ("f3_dbtest_" + std::to_string(static_cast<long long>(std::time(nullptr))));
    fs::create_directories(g_root);
    TestOpenAndSchema();
    TestTransactions();
    TestRoundTrip();
    TestUtf8();
    TestMigration();
    TestMigrationRoundTripExport();
    TestCorrupt();
    TestBackup();
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    std::error_code ec;
    if (!g_failed) fs::remove_all(g_root, ec);   // keep the files of a failing run for inspection
    else std::printf("kept %s\n", g_root.string().c_str());
    return g_failed ? 1 : 0;
}
