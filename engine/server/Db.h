#pragma once
// Thin RAII wrapper around SQLite (amalgamation, see engine/cmake/Dependencies.cmake): connection with WAL + foreign keys, cached
// prepared statements, nested transactions (BEGIN IMMEDIATE / SAVEPOINT) and a code-side migration runner (table schema_version).
//
// THREADING MODEL: one connection, used by exactly one thread, the one that called Open() (the server's main loop: network handlers,
// ticks, console commands, which arrive via Server::PostCommand and run on the loop). Every entry point checks the calling thread and throws
// DbError otherwise, so a misuse is a loud error and never a data race. No locks are taken by this layer.
//
// Errors are exceptions (DbError); Store (Store.h) catches them at its API boundary and returns bool / empty results, logging the reason.
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace f3 {

struct DbError : std::runtime_error {
    int code;   // sqlite result code, 0 = not a sqlite error
    explicit DbError(const std::string& m, int c = 0) : std::runtime_error(m), code(c) {}
};

class Db;

// A prepared statement checked out of the Db cache. Bind (1-based), Step / Run, read columns (0-based). Resets itself when destroyed.
class Stmt {
public:
    Stmt(Stmt&& o) noexcept;
    Stmt& operator=(Stmt&&) = delete;
    Stmt(const Stmt&) = delete;
    ~Stmt();
    Stmt& BindInt(int i, int64_t v);
    Stmt& BindReal(int i, double v);
    Stmt& BindText(int i, const std::string& v);
    Stmt& BindBlob(int i, const std::vector<uint8_t>& v);
    Stmt& BindNull(int i);
    bool Step();   // true = a row is available, false = done; throws DbError
    void Run();    // steps to completion (INSERT / UPDATE / DELETE)
    int ColCount() const;
    std::string ColName(int c) const;
    int ColType(int c) const;   // SQLITE_INTEGER 1, FLOAT 2, TEXT 3, BLOB 4, NULL 5
    bool ColIsNull(int c) const;
    int64_t ColInt(int c) const;
    double ColReal(int c) const;
    std::string ColText(int c) const;
    std::vector<uint8_t> ColBlob(int c) const;
private:
    friend class Db;
    Stmt(Db* db, sqlite3_stmt* st, void* entry) : db_(db), st_(st), entry_(entry) {}
    Db* db_;
    sqlite3_stmt* st_;
    void* entry_;   // Db::Entry of the statement cache, nullptr = private statement (finalized on destruction)
};

struct DbMigration {
    int version;            // strictly increasing, starting at 1
    const char* name;
    const char* sql;        // one or more statements, run in one transaction together with the schema_version row
};

class Db {
public:
    struct Options {
        bool readOnly = false;     // tools (--db-dump): no WAL switch, no writes
        bool create = true;        // create the file when missing (false: error)
        bool quickCheck = true;    // PRAGMA quick_check at open (detects damaged files before anything is read or written)
    };
    Db() = default;
    ~Db() { Close(); }
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;

    // Opens (and, unless readOnly, switches to WAL + foreign keys + synchronous=NORMAL). On failure returns false with a human readable `err`;
    // the file is never modified, moved or recreated when it is damaged.
    bool Open(const std::filesystem::path& path, const Options& opt, std::string* err);
    void Close();                       // checkpoints the WAL (TRUNCATE) and closes
    bool IsOpen() const { return db_ != nullptr; }
    const std::filesystem::path& Path() const { return path_; }

    void Exec(const std::string& sql);                  // may hold several statements
    Stmt Prepare(const std::string& sql);               // cached per SQL text
    int64_t ScalarInt(const std::string& sql);          // first column of the first row (0 when none)
    std::string ScalarText(const std::string& sql);
    int64_t LastInsertRowId() const;
    int Changes() const;
    std::string JournalMode();
    int SchemaVersion();                                // highest applied migration, 0 for a fresh file

    // Applies the pending migrations in order (each in its own transaction). Refuses a file written by a newer program. Before the first
    // migration of a NON-EMPTY database a copy "<file>.pre-v<from>-v<to>.bak" is made (VACUUM INTO). Returns the number applied.
    int Migrate(const std::vector<DbMigration>& migrations);
    // Consistent copy of the live database to `dest` (VACUUM INTO; fails if dest exists).
    void BackupTo(const std::filesystem::path& dest);

    // Nested transactions: the outermost is BEGIN IMMEDIATE ... COMMIT, inner ones are SAVEPOINTs. Destruction without Commit rolls back.
    class Tx {
    public:
        explicit Tx(Db& db);
        ~Tx();
        void Commit();
    private:
        Db& db_;
        int depth_;
        bool done_ = false;
    };

    void CheckThread() const;
private:
    friend class Stmt;
    friend class Tx;
    [[noreturn]] void Fail(const std::string& what, int rc = -1) const;
    sqlite3* db_ = nullptr;
    std::filesystem::path path_;
    std::thread::id owner_;
    int txDepth_ = 0;
    struct Entry { sqlite3_stmt* st = nullptr; bool busy = false; };
    std::unordered_map<std::string, Entry> cache_;
};

}  // namespace f3
