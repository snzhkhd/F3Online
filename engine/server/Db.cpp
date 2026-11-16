#include "Db.h"
#include <sqlite3.h>
#include <system_error>
#include "common/log.h"

namespace f3 {

namespace fs = std::filesystem;

// ------------------------------------------------------------------ Stmt

Stmt::Stmt(Stmt&& o) noexcept : db_(o.db_), st_(o.st_), entry_(o.entry_) { o.st_ = nullptr; o.entry_ = nullptr; }

Stmt::~Stmt() {
    if (!st_) return;
    if (entry_) {
        sqlite3_reset(st_);
        sqlite3_clear_bindings(st_);
        static_cast<Db::Entry*>(entry_)->busy = false;
    } else {
        sqlite3_finalize(st_);
    }
}

Stmt& Stmt::BindInt(int i, int64_t v) { if (int rc = sqlite3_bind_int64(st_, i, v)) db_->Fail("bind", rc); return *this; }
Stmt& Stmt::BindReal(int i, double v) { if (int rc = sqlite3_bind_double(st_, i, v)) db_->Fail("bind", rc); return *this; }
Stmt& Stmt::BindText(int i, const std::string& v) {
    if (int rc = sqlite3_bind_text64(st_, i, v.data(), v.size(), SQLITE_TRANSIENT, SQLITE_UTF8)) db_->Fail("bind", rc);
    return *this;
}
Stmt& Stmt::BindBlob(int i, const std::vector<uint8_t>& v) {
    if (int rc = sqlite3_bind_blob64(st_, i, v.empty() ? static_cast<const void*>("") : static_cast<const void*>(v.data()), v.size(), SQLITE_TRANSIENT)) db_->Fail("bind", rc);
    return *this;
}
Stmt& Stmt::BindNull(int i) { if (int rc = sqlite3_bind_null(st_, i)) db_->Fail("bind", rc); return *this; }

bool Stmt::Step() {
    db_->CheckThread();
    const int rc = sqlite3_step(st_);
    if (rc == SQLITE_ROW) return true;
    sqlite3_reset(st_);   // keeps the bindings; a statement that finished must be reset before it can be bound again
    if (rc == SQLITE_DONE) return false;
    db_->Fail(std::string("step '") + sqlite3_sql(st_) + "'", rc);
}

void Stmt::Run() { while (Step()) {} }
int Stmt::ColCount() const { return sqlite3_column_count(st_); }
std::string Stmt::ColName(int c) const { const char* n = sqlite3_column_name(st_, c); return n ? n : ""; }
int Stmt::ColType(int c) const { return sqlite3_column_type(st_, c); }
bool Stmt::ColIsNull(int c) const { return sqlite3_column_type(st_, c) == SQLITE_NULL; }
int64_t Stmt::ColInt(int c) const { return sqlite3_column_int64(st_, c); }
double Stmt::ColReal(int c) const { return sqlite3_column_double(st_, c); }
std::string Stmt::ColText(int c) const {
    const unsigned char* p = sqlite3_column_text(st_, c);
    return p ? std::string(reinterpret_cast<const char*>(p), static_cast<size_t>(sqlite3_column_bytes(st_, c))) : std::string();
}
std::vector<uint8_t> Stmt::ColBlob(int c) const {
    const void* p = sqlite3_column_blob(st_, c);
    const int n = sqlite3_column_bytes(st_, c);
    return p ? std::vector<uint8_t>(static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n) : std::vector<uint8_t>();
}

// ------------------------------------------------------------------ Db

void Db::Fail(const std::string& what, int rc) const {
    std::string m = what;
    if (db_) {
        m += ": ";
        m += sqlite3_errmsg(db_);
        if (rc < 0) rc = sqlite3_extended_errcode(db_);
    }
    throw DbError(m, rc < 0 ? 0 : rc);
}

void Db::CheckThread() const {
    if (owner_ != std::this_thread::get_id())
        throw DbError("database used from a thread other than the one that opened it (see the threading model in Db.h)");
}

bool Db::Open(const fs::path& path, const Options& opt, std::string* err) {
    auto fail = [&](const std::string& m) {
        if (err) *err = m;
        if (db_) { sqlite3_close(db_); db_ = nullptr; }
        return false;
    };
    Close();
    path_ = path;
    std::error_code ec;
    const bool exists = fs::exists(path, ec);
    if (!exists && (!opt.create || opt.readOnly)) return fail("database " + path.string() + " does not exist");
    if (!exists) fs::create_directories(path.parent_path(), ec);
    const int flags = opt.readOnly ? SQLITE_OPEN_READONLY : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    int rc = sqlite3_open_v2(path.u8string().c_str(), &db_, flags, nullptr);
    if (rc != SQLITE_OK) {
        const std::string m = db_ ? sqlite3_errmsg(db_) : sqlite3_errstr(rc);
        return fail("cannot open database " + path.string() + ": " + m);
    }
    owner_ = std::this_thread::get_id();
    sqlite3_busy_timeout(db_, 3000);
    try {
        // The first statement that reads the file reports a file that is not a database / is damaged; nothing has been written yet.
        const std::string qc = opt.quickCheck ? ScalarText("PRAGMA quick_check(1)") : std::string("ok");
        if (qc != "ok") throw DbError("quick_check: " + qc);
        if (!opt.readOnly) {
            const std::string jm = ScalarText("PRAGMA journal_mode=WAL");
            if (jm != "wal") log::Warn("database %s: WAL not available (journal mode '%s'); saves stay atomic but slower", path.string().c_str(), jm.c_str());
            Exec("PRAGMA synchronous=NORMAL");
        }
        Exec("PRAGMA foreign_keys=ON");
    } catch (const DbError& e) {
        const std::string m = e.what();
        cache_.clear();
        return fail("database " + path.string() + " is damaged or not a SQLite database (" + m +
                    "). The file was NOT modified; restore a backup (<saves>/f3.db.*.bak, a --db-backup copy or --export-json output) "
                    "or move the file away to start fresh.");
    }
    return true;
}

void Db::Close() {
    if (!db_) return;
    for (auto& [sql, e] : cache_) if (e.st) sqlite3_finalize(e.st);
    cache_.clear();
    sqlite3_wal_checkpoint_v2(db_, nullptr, SQLITE_CHECKPOINT_TRUNCATE, nullptr, nullptr);
    sqlite3_close(db_);
    db_ = nullptr;
    txDepth_ = 0;
}

void Db::Exec(const std::string& sql) {
    CheckThread();
    if (!db_) throw DbError("database is not open");
    char* msg = nullptr;
    const int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &msg);
    if (rc != SQLITE_OK) {
        std::string m = msg ? msg : sqlite3_errmsg(db_);
        sqlite3_free(msg);
        throw DbError("exec: " + m, rc);
    }
}

Stmt Db::Prepare(const std::string& sql) {
    CheckThread();
    if (!db_) throw DbError("database is not open");
    Entry& e = cache_[sql];
    if (!e.st) {
        const int rc = sqlite3_prepare_v3(db_, sql.c_str(), static_cast<int>(sql.size()), SQLITE_PREPARE_PERSISTENT, &e.st, nullptr);
        if (rc != SQLITE_OK) { cache_.erase(sql); Fail("prepare '" + sql + "'", rc); }
    }
    if (!e.busy) { e.busy = true; return Stmt(this, e.st, &e); }
    sqlite3_stmt* priv = nullptr;   // the cached one is checked out (re-entrant use): a private statement
    const int rc = sqlite3_prepare_v2(db_, sql.c_str(), static_cast<int>(sql.size()), &priv, nullptr);
    if (rc != SQLITE_OK) Fail("prepare '" + sql + "'", rc);
    return Stmt(this, priv, nullptr);
}

int64_t Db::ScalarInt(const std::string& sql) {
    Stmt s = Prepare(sql);
    return s.Step() ? s.ColInt(0) : 0;
}

std::string Db::ScalarText(const std::string& sql) {
    Stmt s = Prepare(sql);
    return s.Step() ? s.ColText(0) : std::string();
}

int64_t Db::LastInsertRowId() const { return sqlite3_last_insert_rowid(db_); }
int Db::Changes() const { return sqlite3_changes(db_); }
std::string Db::JournalMode() { return ScalarText("PRAGMA journal_mode"); }

int Db::SchemaVersion() {
    if (ScalarInt("SELECT count(*) FROM sqlite_master WHERE type='table' AND name='schema_version'") == 0) return 0;
    return static_cast<int>(ScalarInt("SELECT COALESCE(MAX(version),0) FROM schema_version"));
}

void Db::BackupTo(const fs::path& dest) {
    CheckThread();
    const std::string q = dest.u8string();
    std::string esc;
    for (char c : q) { if (c == '\'') esc += '\''; esc += c; }
    Exec("VACUUM INTO '" + esc + "'");
}

int Db::Migrate(const std::vector<DbMigration>& ms) {
    CheckThread();
    // The version table is only created together with the first migration, so a file without it is "fresh" (version 0) and a file with it
    // from an older program is upgraded.
    const int cur = SchemaVersion();
    const int latest = ms.empty() ? 0 : ms.back().version;
    if (cur > latest)
        throw DbError("database schema version " + std::to_string(cur) + " is newer than this server supports (" + std::to_string(latest) +
                      "): refusing to open it with an older program");
    int applied = 0;
    for (const DbMigration& m : ms) {
        if (m.version <= cur) continue;
        if (applied == 0 && cur > 0 && !path_.empty()) {   // upgrading existing data: keep a pre-migration copy
            fs::path bak = path_;
            bak += ".pre-v" + std::to_string(cur) + "-v" + std::to_string(latest) + ".bak";
            std::error_code ec;
            if (!fs::exists(bak, ec)) {
                BackupTo(bak);
                log::Info("database: backup before migration written to %s", bak.string().c_str());
            }
        }
        Tx tx(*this);
        Exec("CREATE TABLE IF NOT EXISTS schema_version(version INTEGER PRIMARY KEY, name TEXT NOT NULL, applied_at INTEGER NOT NULL)");
        Exec(m.sql);
        {
            Stmt s = Prepare("INSERT INTO schema_version(version, name, applied_at) VALUES(?,?,strftime('%s','now'))");
            s.BindInt(1, m.version).BindText(2, m.name).Run();
        }
        tx.Commit();
        log::Info("database: applied migration %d (%s)", m.version, m.name);
        ++applied;
    }
    return applied;
}

// ------------------------------------------------------------------ Tx

Db::Tx::Tx(Db& db) : db_(db), depth_(db.txDepth_) {
    if (depth_ == 0) db_.Exec("BEGIN IMMEDIATE");
    else db_.Exec("SAVEPOINT s" + std::to_string(depth_));
    ++db_.txDepth_;
}

void Db::Tx::Commit() {
    if (done_) return;
    if (depth_ == 0) db_.Exec("COMMIT");
    else db_.Exec("RELEASE s" + std::to_string(depth_));
    done_ = true;
    --db_.txDepth_;
}

Db::Tx::~Tx() {
    if (done_) return;
    done_ = true;
    --db_.txDepth_;
    try {
        if (depth_ == 0) db_.Exec("ROLLBACK");
        else db_.Exec("ROLLBACK TO s" + std::to_string(depth_) + "; RELEASE s" + std::to_string(depth_));
    } catch (...) {}
}

}  // namespace f3
