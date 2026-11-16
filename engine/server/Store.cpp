#include "Store.h"
#include "common/utf8.h"
#include <algorithm>
#include <cctype>
#include <ctime>
#include "common/log.h"

namespace f3 {

using json = nlohmann::json;
namespace fs = std::filesystem;

// ------------------------------------------------------------------ schema (migrations in code)

static const char* kSchemaV1 = R"SQL(
CREATE TABLE meta(
  key   TEXT PRIMARY KEY,
  value TEXT NOT NULL
) WITHOUT ROWID;

CREATE TABLE accounts(
  id          INTEGER PRIMARY KEY,
  acc_key     TEXT NOT NULL UNIQUE,               -- lower case name
  name        TEXT NOT NULL,                      -- spelling at registration
  algo        TEXT NOT NULL DEFAULT 'pbkdf2-sha256',
  iterations  INTEGER NOT NULL,
  salt        BLOB NOT NULL,
  hash        BLOB NOT NULL,
  created     INTEGER NOT NULL,                   -- unix seconds
  last_login  INTEGER,                            -- unix seconds, NULL = never
  flags       INTEGER NOT NULL DEFAULT 0          -- bit 0 banned, others reserved
);

CREATE TABLE characters(
  id          INTEGER PRIMARY KEY,
  account_id  INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
  slot        INTEGER NOT NULL,                   -- only slot 0 is used today
  name        TEXT NOT NULL,
  level       INTEGER NOT NULL DEFAULT 1,
  xp          INTEGER NOT NULL DEFAULT 0,
  map         TEXT,
  x           INTEGER,
  y           INTEGER,
  hp          INTEGER,
  max_hp      INTEGER,
  dead        INTEGER NOT NULL DEFAULT 0,
  created     INTEGER NOT NULL DEFAULT 1,         -- character creation finished
  last_seen   INTEGER,                            -- unix seconds of the last save
  data        TEXT NOT NULL,                      -- JSON: everything else (SPECIAL, skills, perks, inventory, vars, gvars ...), no quests / friends
  UNIQUE(account_id, slot)
);
CREATE INDEX characters_name ON characters(name COLLATE NOCASE);
CREATE INDEX characters_map  ON characters(map);

CREATE TABLE friends(
  character_id INTEGER NOT NULL REFERENCES characters(id) ON DELETE CASCADE,
  ord          INTEGER NOT NULL,
  friend       TEXT NOT NULL,                     -- display spelling
  friend_key   TEXT NOT NULL,                     -- lower case
  PRIMARY KEY(character_id, friend_key)
) WITHOUT ROWID;
CREATE INDEX friends_by_friend ON friends(friend_key);

CREATE TABLE quests(
  character_id INTEGER NOT NULL REFERENCES characters(id) ON DELETE CASCADE,
  quest_id     TEXT NOT NULL,
  ord          INTEGER NOT NULL,
  title        TEXT NOT NULL,
  body         TEXT NOT NULL,
  state        INTEGER NOT NULL,                  -- 0 active, 1 completed, 2 failed
  updated      INTEGER NOT NULL,
  PRIMARY KEY(character_id, quest_id)
) WITHOUT ROWID;
CREATE INDEX quests_by_state ON quests(quest_id, state);

CREATE TABLE mail(
  id           INTEGER PRIMARY KEY,
  recipient_id INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
  sender       TEXT NOT NULL,
  body         TEXT NOT NULL,
  sent_at      INTEGER NOT NULL,
  is_read      INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX mail_inbox ON mail(recipient_id, is_read);

CREATE TABLE world_objects(
  map       TEXT NOT NULL,
  obj_key   TEXT NOT NULL,                        -- "<proto key>@x,y"
  is_open   INTEGER NOT NULL DEFAULT 0,
  locked    INTEGER NOT NULL DEFAULT 0,
  flags     INTEGER NOT NULL DEFAULT 0,
  dc        INTEGER NOT NULL DEFAULT 0,
  lock_key  TEXT NOT NULL DEFAULT '',
  contents  TEXT,                                 -- JSON [{proto,count}] for containers
  PRIMARY KEY(map, obj_key)
) WITHOUT ROWID;

CREATE TABLE events(
  id    INTEGER PRIMARY KEY,
  ts    INTEGER NOT NULL,                         -- unix seconds
  kind  TEXT NOT NULL,                            -- login, logout, register, kick, level_up, kill, mail_sent, ...
  actor TEXT NOT NULL DEFAULT '',
  data  TEXT NOT NULL DEFAULT '{}'                -- JSON
);
CREATE INDEX events_kind  ON events(kind, id);
CREATE INDEX events_actor ON events(actor COLLATE NOCASE, id);
)SQL";

// v2: runtime state of scripted map objects (data/objects): one JSON document per (map, object key), see docs/object-scripts.md
const char* const kSchemaV2 = R"SQL(
CREATE TABLE object_state(
  map       TEXT NOT NULL,
  obj_key   TEXT NOT NULL,                        -- instance name, or "<proto key>@x,y"
  data      TEXT NOT NULL,                        -- JSON {"used":0|1,"flags":{name:number}}
  PRIMARY KEY(map, obj_key)
) WITHOUT ROWID;
)SQL";

const std::vector<DbMigration>& StoreMigrations() {
    static const std::vector<DbMigration> m = {
        {1, "initial schema", kSchemaV1},
        {2, "object_state (scripted map objects)", kSchemaV2},
    };
    return m;
}

// ------------------------------------------------------------------ helpers

namespace {

std::string Lower(const std::string& s) { return utf8::Lower(s); }
std::string JStr(const json& j, const char* k, const std::string& def = std::string()) {
    auto it = j.find(k);
    return it != j.end() && it->is_string() ? it->get<std::string>() : def;
}
int64_t JInt(const json& j, const char* k, int64_t def = 0) {
    auto it = j.find(k);
    return it != j.end() && it->is_number() ? it->get<int64_t>() : def;
}
bool JBool(const json& j, const char* k, bool def) {
    auto it = j.find(k);
    return it != j.end() && it->is_boolean() ? it->get<bool>() : def;
}
int64_t Now() { return static_cast<int64_t>(std::time(nullptr)); }

}  // namespace

void Store::Fail(const char* what, const char* msg) {
    lastError_ = std::string(what) + ": " + msg;
    log::Error("database: %s", lastError_.c_str());
}

bool Store::Open(const fs::path& dbPath, std::string* err, bool readOnly) {
    Db::Options o;
    o.readOnly = readOnly;
    o.create = !readOnly;
    if (!db_.Open(dbPath, o, err)) return false;
    try {
        if (readOnly) {
            const int v = db_.SchemaVersion();
            const int latest = StoreMigrations().back().version;
            if (v > latest) throw DbError("database schema version " + std::to_string(v) + " is newer than this program supports (" + std::to_string(latest) + ")");
        } else {
            db_.Migrate(StoreMigrations());
        }
    } catch (const std::exception& e) {
        if (err) *err = "database " + dbPath.string() + ": " + e.what();
        db_.Close();
        return false;
    }
    return true;
}

Store::Tx::Tx(Store& s) : s_(s) {
    try { tx_ = std::make_unique<Db::Tx>(s.db_); ok_ = true; } catch (const std::exception& e) { s.Fail("begin", e.what()); }
}
bool Store::Tx::Commit() {
    if (!ok_) return false;
    try { tx_->Commit(); return true; } catch (const std::exception& e) { s_.Fail("commit", e.what()); ok_ = false; return false; }
}

// ------------------------------------------------------------------ accounts

int64_t Store::AccountId(const std::string& key) {
    auto it = accIds_.find(key);
    if (it != accIds_.end()) return it->second;
    Stmt s = db_.Prepare("SELECT id FROM accounts WHERE acc_key=?");
    s.BindText(1, key);
    if (!s.Step()) return 0;
    return accIds_[key] = s.ColInt(0);
}

bool Store::AccountExists(const std::string& key) {
    try { return AccountId(key) != 0; } catch (const std::exception& e) { Fail("account lookup", e.what()); return false; }
}

std::optional<AccountRow> Store::GetAccount(const std::string& key) {
    try {
        Stmt s = db_.Prepare("SELECT id, acc_key, name, algo, iterations, salt, hash, created, COALESCE(last_login,0), flags FROM accounts WHERE acc_key=?");
        s.BindText(1, key);
        if (!s.Step()) return std::nullopt;
        AccountRow a;
        a.id = s.ColInt(0); a.key = s.ColText(1); a.name = s.ColText(2); a.algo = s.ColText(3);
        a.iterations = static_cast<int>(s.ColInt(4)); a.salt = s.ColBlob(5); a.hash = s.ColBlob(6);
        a.created = s.ColInt(7); a.lastLogin = s.ColInt(8); a.flags = s.ColInt(9);
        return a;
    } catch (const std::exception& e) { Fail("account load", e.what()); return std::nullopt; }
}

bool Store::CreateAccount(const AccountRow& a, const json& firstCharacter) {
    try {
        Db::Tx tx(db_);
        {
            Stmt s = db_.Prepare("INSERT INTO accounts(acc_key, name, algo, iterations, salt, hash, created, flags) VALUES(?,?,?,?,?,?,?,?)");
            s.BindText(1, a.key).BindText(2, a.name).BindText(3, a.algo).BindInt(4, a.iterations).BindBlob(5, a.salt).BindBlob(6, a.hash)
                .BindInt(7, a.created ? a.created : Now()).BindInt(8, a.flags);
            s.Run();
        }
        accIds_.erase(a.key);
        if (!SaveCharacter(a.key, 0, firstCharacter)) throw DbError("first character: " + lastError_);
        tx.Commit();
        return true;
    } catch (const std::exception& e) {
        accIds_.erase(a.key);
        Fail("account create", e.what());
        return false;
    }
}

void Store::TouchLogin(const std::string& key) {
    try {
        Stmt s = db_.Prepare("UPDATE accounts SET last_login=? WHERE acc_key=?");
        s.BindInt(1, Now()).BindText(2, key).Run();
    } catch (const std::exception& e) { Fail("last_login", e.what()); }
}

int64_t Store::AccountCount() {
    try { return db_.ScalarInt("SELECT count(*) FROM accounts"); } catch (const std::exception& e) { Fail("count", e.what()); return 0; }
}

// ------------------------------------------------------------------ characters

bool Store::SaveCharacter(const std::string& accountKey, int slot, const json& ch) {
    try {
        const int64_t acc = AccountId(accountKey);
        if (!acc) { lastError_ = "unknown account " + accountKey; return false; }
        json data = ch.is_object() ? ch : json::object();
        json quests = data.contains("quests") ? data["quests"] : json::array();
        json friends = data.contains("friends") ? data["friends"] : json::array();
        data.erase("quests");
        data.erase("friends");
        Db::Tx tx(db_);
        {
            Stmt s = db_.Prepare(
                "INSERT INTO characters(account_id, slot, name, level, xp, map, x, y, hp, max_hp, dead, created, last_seen, data) "
                "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
                "ON CONFLICT(account_id, slot) DO UPDATE SET name=excluded.name, level=excluded.level, xp=excluded.xp, map=excluded.map, "
                "x=excluded.x, y=excluded.y, hp=excluded.hp, max_hp=excluded.max_hp, dead=excluded.dead, created=excluded.created, "
                "last_seen=excluded.last_seen, data=excluded.data");
            s.BindInt(1, acc).BindInt(2, slot).BindText(3, JStr(data, "name", JStr(data, "account_name", accountKey)));
            s.BindInt(4, std::max<int64_t>(1, JInt(data, "level", 1))).BindInt(5, JInt(data, "xp", 0));
            if (data.contains("map") && data["map"].is_string()) s.BindText(6, data["map"].get<std::string>()); else s.BindNull(6);
            if (data.contains("x") && data["x"].is_number()) s.BindInt(7, JInt(data, "x")); else s.BindNull(7);
            if (data.contains("y") && data["y"].is_number()) s.BindInt(8, JInt(data, "y")); else s.BindNull(8);
            if (data.contains("hp") && data["hp"].is_number()) s.BindInt(9, JInt(data, "hp")); else s.BindNull(9);
            if (data.contains("max_hp") && data["max_hp"].is_number()) s.BindInt(10, JInt(data, "max_hp")); else s.BindNull(10);
            s.BindInt(11, JBool(data, "dead", false) ? 1 : 0).BindInt(12, JBool(data, "created", true) ? 1 : 0);
            if (data.contains("saved_at") && data["saved_at"].is_number()) s.BindInt(13, JInt(data, "saved_at")); else s.BindNull(13);
            s.BindText(14, data.dump(-1, ' ', false, json::error_handler_t::replace));
            s.Run();
        }
        int64_t cid = 0;
        {
            Stmt s = db_.Prepare("SELECT id FROM characters WHERE account_id=? AND slot=?");
            s.BindInt(1, acc).BindInt(2, slot);
            if (!s.Step()) throw DbError("character row vanished");
            cid = s.ColInt(0);
        }
        { Stmt s = db_.Prepare("DELETE FROM friends WHERE character_id=?"); s.BindInt(1, cid).Run(); }
        { Stmt s = db_.Prepare("DELETE FROM quests WHERE character_id=?"); s.BindInt(1, cid).Run(); }
        if (friends.is_array()) {
            int ord = 0;
            Stmt s = db_.Prepare("INSERT OR IGNORE INTO friends(character_id, ord, friend, friend_key) VALUES(?,?,?,?)");
            for (const json& f : friends) {
                if (!f.is_string()) continue;
                const std::string n = f.get<std::string>();
                s.BindInt(1, cid).BindInt(2, ord++).BindText(3, n).BindText(4, Lower(n)).Run();
            }
        }
        if (quests.is_array()) {
            int ord = 0;
            Stmt s = db_.Prepare("INSERT OR IGNORE INTO quests(character_id, quest_id, ord, title, body, state, updated) VALUES(?,?,?,?,?,?,?)");
            for (const json& q : quests) {
                if (!q.is_object() || JStr(q, "id").empty()) continue;
                s.BindInt(1, cid).BindText(2, JStr(q, "id")).BindInt(3, ord++).BindText(4, JStr(q, "title", JStr(q, "id"))).BindText(5, JStr(q, "text"))
                    .BindInt(6, JInt(q, "state", 0)).BindInt(7, JInt(q, "updated", 0)).Run();
            }
        }
        tx.Commit();
        return true;
    } catch (const std::exception& e) { Fail("character save", e.what()); return false; }
}

Store::Load Store::LoadCharacter(const std::string& accountKey, int slot, json* out) {
    try {
        const int64_t acc = AccountId(accountKey);
        if (!acc) return Load::Missing;
        int64_t cid = 0;
        json j;
        {
            Stmt s = db_.Prepare("SELECT id, data FROM characters WHERE account_id=? AND slot=?");
            s.BindInt(1, acc).BindInt(2, slot);
            if (!s.Step()) return Load::Missing;
            cid = s.ColInt(0);
            j = json::parse(s.ColText(1), nullptr, false);
        }
        if (!j.is_object()) throw DbError("character data of '" + accountKey + "' is not valid JSON (row left untouched)");
        {
            Stmt s = db_.Prepare("SELECT friend FROM friends WHERE character_id=? ORDER BY ord");
            s.BindInt(1, cid);
            json arr = json::array();
            while (s.Step()) arr.push_back(s.ColText(0));
            if (!arr.empty()) j["friends"] = std::move(arr);
        }
        {
            Stmt s = db_.Prepare("SELECT quest_id, title, body, state, updated FROM quests WHERE character_id=? ORDER BY ord");
            s.BindInt(1, cid);
            json arr = json::array();
            while (s.Step()) arr.push_back({{"id", s.ColText(0)}, {"title", s.ColText(1)}, {"text", s.ColText(2)}, {"state", s.ColInt(3)}, {"updated", s.ColInt(4)}});
            if (!arr.empty()) j["quests"] = std::move(arr);
        }
        if (out) *out = std::move(j);
        return Load::Ok;
    } catch (const std::exception& e) { Fail("character load", e.what()); return Load::Error; }
}

std::vector<std::string> Store::WhoHasFriend(const std::string& name) {
    std::vector<std::string> r;
    try {
        Stmt s = db_.Prepare("SELECT a.name FROM friends f JOIN characters c ON c.id=f.character_id JOIN accounts a ON a.id=c.account_id WHERE f.friend_key=? ORDER BY a.acc_key");
        s.BindText(1, Lower(name));
        while (s.Step()) r.push_back(s.ColText(0));
    } catch (const std::exception& e) { Fail("friends lookup", e.what()); }
    return r;
}

// ------------------------------------------------------------------ mail

uint32_t Store::MailAdd(const std::string& toKey, const std::string& from, const std::string& text, int64_t time) {
    try {
        const int64_t acc = AccountId(toKey);
        if (!acc) return 0;
        Stmt s = db_.Prepare("INSERT INTO mail(recipient_id, sender, body, sent_at, is_read) VALUES(?,?,?,?,0)");
        s.BindInt(1, acc).BindText(2, from).BindText(3, text).BindInt(4, time).Run();
        return static_cast<uint32_t>(db_.LastInsertRowId());
    } catch (const std::exception& e) { Fail("mail add", e.what()); return 0; }
}

std::vector<MailRow> Store::MailList(const std::string& key) {
    std::vector<MailRow> r;
    try {
        const int64_t acc = AccountId(key);
        if (!acc) return r;
        Stmt s = db_.Prepare("SELECT id, sender, body, sent_at, is_read FROM mail WHERE recipient_id=? ORDER BY id");
        s.BindInt(1, acc);
        while (s.Step()) {
            MailRow m;
            m.id = static_cast<uint32_t>(s.ColInt(0)); m.from = s.ColText(1); m.text = s.ColText(2); m.time = s.ColInt(3); m.read = s.ColInt(4) != 0;
            r.push_back(std::move(m));
        }
    } catch (const std::exception& e) { Fail("mail list", e.what()); }
    return r;
}

int Store::MailCount(const std::string& key) {
    try {
        const int64_t acc = AccountId(key);
        if (!acc) return 0;
        Stmt s = db_.Prepare("SELECT count(*) FROM mail WHERE recipient_id=?");
        s.BindInt(1, acc);
        return s.Step() ? static_cast<int>(s.ColInt(0)) : 0;
    } catch (const std::exception& e) { Fail("mail count", e.what()); return 0; }
}

int Store::MailUnread(const std::string& key) {
    try {
        const int64_t acc = AccountId(key);
        if (!acc) return 0;
        Stmt s = db_.Prepare("SELECT count(*) FROM mail WHERE recipient_id=? AND is_read=0");
        s.BindInt(1, acc);
        return s.Step() ? static_cast<int>(s.ColInt(0)) : 0;
    } catch (const std::exception& e) { Fail("mail unread", e.what()); return 0; }
}

int Store::MailMarkRead(const std::string& key, uint32_t id) {
    try {
        const int64_t acc = AccountId(key);
        if (!acc) return -1;
        { Stmt s = db_.Prepare("UPDATE mail SET is_read=1 WHERE id=? AND recipient_id=? AND is_read=0"); s.BindInt(1, id).BindInt(2, acc).Run(); }
        if (db_.Changes() > 0) return 1;
        Stmt s = db_.Prepare("SELECT 1 FROM mail WHERE id=? AND recipient_id=?");
        s.BindInt(1, id).BindInt(2, acc);
        return s.Step() ? 0 : -1;
    } catch (const std::exception& e) { Fail("mail read", e.what()); return -1; }
}

bool Store::MailDelete(const std::string& key, uint32_t id) {
    try {
        const int64_t acc = AccountId(key);
        if (!acc) return false;
        Stmt s = db_.Prepare("DELETE FROM mail WHERE id=? AND recipient_id=?");
        s.BindInt(1, id).BindInt(2, acc).Run();
        return db_.Changes() > 0;
    } catch (const std::exception& e) { Fail("mail delete", e.what()); return false; }
}

// ------------------------------------------------------------------ world objects

bool Store::SaveWorldObjects(const std::string& map, const std::vector<WorldObjRow>& rows) {
    try {
        Db::Tx tx(db_);
        { Stmt s = db_.Prepare("DELETE FROM world_objects WHERE map=?"); s.BindText(1, map).Run(); }
        Stmt s = db_.Prepare("INSERT OR REPLACE INTO world_objects(map, obj_key, is_open, locked, flags, dc, lock_key, contents) VALUES(?,?,?,?,?,?,?,?)");
        for (const WorldObjRow& r : rows) {
            s.BindText(1, map).BindText(2, r.key).BindInt(3, r.open).BindInt(4, r.locked).BindInt(5, r.flags).BindInt(6, r.dc).BindText(7, r.lockKey);
            if (r.contents.empty()) s.BindNull(8); else s.BindText(8, r.contents);
            s.Run();
        }
        tx.Commit();
        return true;
    } catch (const std::exception& e) { Fail("world save", e.what()); return false; }
}

std::vector<WorldObjRow> Store::LoadWorldObjects(const std::string& map) {
    std::vector<WorldObjRow> r;
    try {
        Stmt s = db_.Prepare("SELECT obj_key, is_open, locked, flags, dc, lock_key, COALESCE(contents,'') FROM world_objects WHERE map=? ORDER BY obj_key");
        s.BindText(1, map);
        while (s.Step()) {
            WorldObjRow o;
            o.key = s.ColText(0); o.open = s.ColInt(1) != 0; o.locked = s.ColInt(2) != 0; o.flags = static_cast<int>(s.ColInt(3));
            o.dc = static_cast<int>(s.ColInt(4)); o.lockKey = s.ColText(5); o.contents = s.ColText(6);
            r.push_back(std::move(o));
        }
    } catch (const std::exception& e) { Fail("world load", e.what()); }
    return r;
}

bool Store::SaveObjectState(const std::string& map, const std::vector<ObjStateRow>& rows) {
    try {
        Db::Tx tx(db_);
        { Stmt s = db_.Prepare("DELETE FROM object_state WHERE map=?"); s.BindText(1, map).Run(); }
        Stmt s = db_.Prepare("INSERT OR REPLACE INTO object_state(map, obj_key, data) VALUES(?,?,?)");
        for (const ObjStateRow& r : rows) { s.BindText(1, map).BindText(2, r.key).BindText(3, r.data); s.Run(); }
        tx.Commit();
        return true;
    } catch (const std::exception& e) { Fail("object state save", e.what()); return false; }
}

std::vector<ObjStateRow> Store::LoadObjectState(const std::string& map) {
    std::vector<ObjStateRow> r;
    try {
        Stmt s = db_.Prepare("SELECT obj_key, data FROM object_state WHERE map=? ORDER BY obj_key");
        s.BindText(1, map);
        while (s.Step()) r.push_back(ObjStateRow{s.ColText(0), s.ColText(1)});
    } catch (const std::exception& e) { Fail("object state load", e.what()); }
    return r;
}

int64_t Store::WorldObjectCount(const std::string& map) {
    try {
        if (map.empty()) return db_.ScalarInt("SELECT count(*) FROM world_objects");
        Stmt s = db_.Prepare("SELECT count(*) FROM world_objects WHERE map=?");
        s.BindText(1, map);
        return s.Step() ? s.ColInt(0) : 0;
    } catch (const std::exception& e) { Fail("world count", e.what()); return 0; }
}

// ------------------------------------------------------------------ events

bool Store::LogEvent(const std::string& kind, const std::string& actor, const json& data) {
    try {
        Stmt s = db_.Prepare("INSERT INTO events(ts, kind, actor, data) VALUES(?,?,?,?)");
        s.BindInt(1, Now()).BindText(2, kind).BindText(3, actor).BindText(4, data.is_null() ? std::string("{}") : data.dump(-1, ' ', false, json::error_handler_t::replace)).Run();
        return true;
    } catch (const std::exception& e) { Fail("event log", e.what()); return false; }
}

std::vector<EventRow> Store::RecentEvents(int n, const std::string& kind, const std::string& actor) {
    std::vector<EventRow> r;
    try {
        std::string sql = "SELECT id, ts, kind, actor, data FROM events WHERE 1=1";
        if (!kind.empty()) sql += " AND kind=?1";
        if (!actor.empty()) sql += " AND actor=?2 COLLATE NOCASE";
        sql += " ORDER BY id DESC LIMIT ?3";
        Stmt s = db_.Prepare(sql);
        if (!kind.empty()) s.BindText(1, kind);
        if (!actor.empty()) s.BindText(2, actor);
        s.BindInt(3, std::clamp(n, 1, 100000));
        while (s.Step()) r.push_back({s.ColInt(0), s.ColInt(1), s.ColText(2), s.ColText(3), s.ColText(4)});
    } catch (const std::exception& e) { Fail("events", e.what()); }
    return r;
}

int64_t Store::EventCount() {
    try { return db_.ScalarInt("SELECT count(*) FROM events"); } catch (const std::exception& e) { Fail("count", e.what()); return 0; }
}

// ------------------------------------------------------------------ misc

std::optional<std::string> Store::Meta(const std::string& key) {
    try {
        Stmt s = db_.Prepare("SELECT value FROM meta WHERE key=?");
        s.BindText(1, key);
        if (s.Step()) return s.ColText(0);
    } catch (const std::exception& e) { Fail("meta", e.what()); }
    return std::nullopt;
}

bool Store::SetMeta(const std::string& key, const std::string& value) {
    try {
        Stmt s = db_.Prepare("INSERT INTO meta(key, value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value");
        s.BindText(1, key).BindText(2, value).Run();
        return true;
    } catch (const std::exception& e) { Fail("meta", e.what()); return false; }
}

bool Store::IsEmpty() {
    try {
        return db_.ScalarInt("SELECT count(*) FROM accounts") == 0 && db_.ScalarInt("SELECT count(*) FROM mail") == 0 &&
               db_.ScalarInt("SELECT count(*) FROM world_objects") == 0;
    } catch (const std::exception& e) { Fail("empty check", e.what()); return false; }
}

DbInfo Store::Info() {
    DbInfo d;
    d.path = db_.Path().string();
    try {
        d.journalMode = db_.JournalMode();
        d.schemaVersion = db_.SchemaVersion();
        d.pageCount = db_.ScalarInt("PRAGMA page_count");
        d.pageSize = db_.ScalarInt("PRAGMA page_size");
        d.freePages = db_.ScalarInt("PRAGMA freelist_count");
        std::error_code ec;
        const auto sz = fs::file_size(db_.Path(), ec);
        d.fileBytes = ec ? 0 : static_cast<int64_t>(sz);
        fs::path wal = db_.Path();
        wal += "-wal";
        const auto wsz = fs::file_size(wal, ec);
        d.walBytes = ec ? 0 : static_cast<int64_t>(wsz);
        for (const char* t : {"accounts", "characters", "friends", "quests", "mail", "world_objects", "object_state", "events", "meta", "schema_version"})
            d.tables.push_back({t, db_.ScalarInt(std::string("SELECT count(*) FROM ") + t)});
    } catch (const std::exception& e) { Fail("info", e.what()); }
    return d;
}

bool Store::Backup(const fs::path& dest) {
    try { db_.BackupTo(dest); return true; } catch (const std::exception& e) { Fail("backup", e.what()); return false; }
}

}  // namespace f3
