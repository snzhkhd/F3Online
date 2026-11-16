#pragma once
// Persistence of the live server state in one SQLite file (<saves>/f3.db): accounts, characters (searchable columns + JSON blob for the
// bulky parts), friends, quests, mail, door / container state per map and an audit log. Schema, atomicity, threading model and
// migration from the old JSON saves: docs/persistence.md.
//
// All calls must come from the thread that opened the store (the server loop); see Db.h. Every method catches database errors, logs them
// and reports failure through its return value, so a failing disk never throws through gameplay code.
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "Db.h"

namespace f3 {

struct AccountRow {
    int64_t id = 0;
    std::string key;                 // lower case, unique
    std::string name;                // spelling at registration
    std::string algo = "pbkdf2-sha256";
    int iterations = 10000;
    std::vector<uint8_t> salt, hash;
    int64_t created = 0;
    int64_t lastLogin = 0;           // 0 = never
    int64_t flags = 0;
};

struct MailRow {
    uint32_t id = 0;                 // database wide, increasing
    std::string from;
    std::string text;
    int64_t time = 0;
    bool read = false;
};

struct WorldObjRow {
    std::string key;                 // "<proto key>@x,y"
    bool open = false, locked = false;
    int flags = 0, dc = 0;
    std::string lockKey;
    std::string contents;            // JSON array text [{proto,count}], empty = not a container
};

struct EventRow {
    int64_t id = 0, ts = 0;
    std::string kind, actor, data;   // data: JSON text
};

struct TableCount { std::string name; int64_t rows = 0; };

struct DbInfo {
    std::string path, journalMode;
    int schemaVersion = 0;
    int64_t fileBytes = 0, walBytes = 0, pageCount = 0, pageSize = 0, freePages = 0;
    std::vector<TableCount> tables;
};

class Store {
public:
    enum class Load { Ok, Missing, Error };

    // Opens (creating if needed) and migrates the schema. readOnly: tools, no migration, no WAL switch. On failure `err` says why and the
    // file is untouched.
    bool Open(const std::filesystem::path& dbPath, std::string* err, bool readOnly = false);
    void Close() { db_.Close(); }
    bool IsOpen() const { return db_.IsOpen(); }
    const std::filesystem::path& Path() const { return db_.Path(); }
    Db& Raw() { return db_; }
    const std::string& LastError() const { return lastError_; }
    void ResetCaches() { accIds_.clear(); }   // after a rolled back transaction that may have cached ids of rows it created

    // ---- accounts
    bool AccountExists(const std::string& key);
    std::optional<AccountRow> GetAccount(const std::string& key);
    // Inserts the account and its first character (slot 0) in one transaction. false: name taken or database error.
    bool CreateAccount(const AccountRow& a, const nlohmann::json& firstCharacter);
    void TouchLogin(const std::string& key);
    int64_t AccountCount();

    // ---- characters. The json is the same document the server always saved (Persist.cpp CharacterToJson); "quests" and "friends" are
    // stored in their own tables and put back on load.
    Load LoadCharacter(const std::string& accountKey, int slot, nlohmann::json* out);
    bool SaveCharacter(const std::string& accountKey, int slot, const nlohmann::json& ch);
    std::vector<std::string> WhoHasFriend(const std::string& name);   // account names whose character lists `name` as a friend

    // ---- mail (per account key)
    uint32_t MailAdd(const std::string& toKey, const std::string& from, const std::string& text, int64_t time);   // 0 = failed
    std::vector<MailRow> MailList(const std::string& key);
    int MailCount(const std::string& key);
    int MailUnread(const std::string& key);
    // 1 = marked read now, 0 = already read, -1 = no such message
    int MailMarkRead(const std::string& key, uint32_t id);
    bool MailDelete(const std::string& key, uint32_t id);

    // ---- door / container state per map: replaces all rows of the map atomically
    bool SaveWorldObjects(const std::string& map, const std::vector<WorldObjRow>& rows);
    std::vector<WorldObjRow> LoadWorldObjects(const std::string& map);
    int64_t WorldObjectCount(const std::string& map = std::string());

    // ---- audit log
    bool LogEvent(const std::string& kind, const std::string& actor, const nlohmann::json& data = nlohmann::json::object());
    std::vector<EventRow> RecentEvents(int n, const std::string& kind = std::string(), const std::string& actor = std::string());   // newest first
    int64_t EventCount();

    // ---- misc
    std::optional<std::string> Meta(const std::string& key);
    bool SetMeta(const std::string& key, const std::string& value);
    bool IsEmpty();                       // no accounts, no mail, no world rows
    DbInfo Info();
    bool Backup(const std::filesystem::path& dest);

    // transaction around several calls (nested calls become savepoints); destruction without Commit rolls back
    class Tx {
    public:
        explicit Tx(Store& s);
        bool Ok() const { return ok_; }
        bool Commit();
    private:
        Store& s_;
        std::unique_ptr<Db::Tx> tx_;
        bool ok_ = false;
    };

    // runs f(), catching database errors (logged, remembered in LastError); returns false on error
    template <class F> bool Guard(const char* what, F&& f) {
        try { f(); return true; } catch (const std::exception& e) { Fail(what, e.what()); return false; }
    }
    void Fail(const char* what, const char* msg);

private:
    int64_t AccountId(const std::string& key);   // 0 = unknown
    Db db_;
    std::string lastError_;
    std::map<std::string, int64_t> accIds_;
};

const std::vector<DbMigration>& StoreMigrations();

}  // namespace f3
