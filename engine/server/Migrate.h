#pragma once
// Import of the old JSON saves (<saves>/accounts, <saves>/mail, <saves>/world) into the SQLite store, plus the debugging / backup tools
// (--migrate-json, --export-json, --db-dump, --db-backup). See docs/persistence.md.
#include <filesystem>
#include <iosfwd>
#include <string>
#include <vector>
#include "Store.h"

namespace f3 {

struct MigrateReport {
    bool legacyFound = false;     // at least one of accounts/, mail/, world/ existed
    bool imported = false;        // an import transaction was committed
    int accounts = 0, characters = 0, mail = 0, worldMaps = 0, worldObjects = 0;
    int accountsSkipped = 0;      // already in the database (force mode) or unreadable
    int mailSkipped = 0, worldMapsSkipped = 0;
    std::vector<std::string> problems;   // every skipped / unreadable file, with the reason
    std::vector<std::string> renamed;    // "old -> backup" of the folders that were set aside
};

// True when <saves>/accounts, mail or world exists as a folder.
bool HasLegacyJsonSaves(const std::filesystem::path& saves);

// force = false (server start): imports only into an EMPTY database that was never migrated; a database that already holds data keeps it
// and the old folders are left alone (a warning is logged). force = true (--migrate-json): merges whatever the old folders hold that the
// database lacks (accounts by name, mail only of accounts imported now, world state of maps without rows).
// After a committed import the old folders are renamed to <name>.migrated (.migrated.1, ... when taken); nothing is ever deleted.
// Safe to call again: with no old folders it does nothing; after a crash between commit and rename it only finishes the rename.
// Returns false (err set) only when the database could not be written; unreadable files are reported in `problems` and set aside with the rest.
bool MigrateJsonSaves(Store& store, const std::filesystem::path& saves, bool force, MigrateReport& rep, std::string* err);

// Writes accounts/acc_<key>.json, mail/<key>.json and world/<map>.json in the OLD format below `dir` (a backup the old server and the
// importer both read). The account files contain the password hashes: treat the output like the database.
bool ExportJson(Store& store, const std::filesystem::path& dir, std::string* err);

// Prints schema version, counts and every row of every table (or only `table`) as JSON lines; blobs (salt, hash) are not printed.
bool DumpDb(Store& store, std::ostream& os, const std::string& table, std::string* err);

// Command line tools of f3_server (they never start the network): tool = "migrate-json" | "export-json" | "db-dump" | "db-backup".
// `arg`: target folder (export-json), file (db-backup), optional table (db-dump). Returns the process exit code (0 ok, 1 error).
int RunDbTool(const std::string& tool, const std::filesystem::path& saves, const std::string& arg);

}  // namespace f3
