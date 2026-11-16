# Persistence: the SQLite store

The server keeps all live state that must survive a restart in **one SQLite database**, `<saves>/f3.db` (`--saves DIR`, default `<repo>/saves`,
gitignored; never commit a database). It replaces the JSON files of the first persistence layer (`saves/accounts`, `saves/mail`, `saves/world`).
Gameplay is unchanged: the server still builds the same character document and applies it the same way; only where it is stored differs.

Code: `engine/server/Db.*` (connection, statements, transactions, migrations), `Store.*` (the tables and their API), `Migrate.*` (JSON import,
export, dump, command line tools), `Account.*` (`AccountStore` on top of `Store`), `Persist.cpp` / `Pip.cpp` / `Doors.cpp` (the users).
Tests: `f3_dbtest` (`engine/tests/DbTest.cpp`) and part 0b of `smoke_test.ps1`.

## 1. Dependency

SQLite 3.45.3 amalgamation, fetched at configure time like the other dependencies (`engine/cmake/Dependencies.cmake`, pinned URL + SHA256,
built as the static library `sqlite3_static`; compile definitions: `SQLITE_DQS=0`, `SQLITE_OMIT_LOAD_EXTENSION`, `SQLITE_THREADSAFE=1`,
`SQLITE_DEFAULT_FOREIGN_KEYS=1`). **A configure of a build directory that has not fetched it yet needs network access once**
(`https://www.sqlite.org/2024/sqlite-amalgamation-3450300.zip`, 2.7 MB). Offline: unpack that zip somewhere and configure with
`-DFETCHCONTENT_SOURCE_DIR_SQLITE3=<dir containing sqlite3.c>`.

## 2. Threading model

One connection, **one thread**: the server's main loop (network events, ticks, console commands, autosave). The console reader thread only
queues text (`Server::PostCommand`); `dbinfo` / `events` run on the loop. `Db` remembers the thread that opened it and every call checks it:
use from another thread throws `DbError` (logged by `Store`, which returns failure), so a mistake is a loud error and never a data race.
Other processes (the tools below) may read a live database at the same time (WAL); a second *writer* waits up to 3 s for the lock.
Only the CPU-heavy part of a login (PBKDF2) is not special: it runs on the loop as before.

## 3. Connection settings and atomicity

`journal_mode=WAL`, `synchronous=NORMAL`, `foreign_keys=ON`, `busy_timeout=3000`, a prepared-statement cache (statements are prepared once per
SQL text and reused, bound by position).

* Every multi-step write is one transaction: a character save (row + friends + quests), account registration (account + first character),
  a mail send, the door / container state of a map, the whole autosave (`SaveAllPlayers`: one `BEGIN IMMEDIATE ... COMMIT` with a savepoint per
  player) and the whole world save (`SaveWorldState`: all dirty maps in one commit). Transactions nest as savepoints. A failure or an
  exception rolls back; the maps stay flagged dirty and are retried.
* Compared with the old "write temp file, rename": a commit is atomic and, in WAL mode, a killed process or a crash never leaves a half
  written state; the old `.bak` copy is replaced by `f3.db.pre-v<N>-v<M>.bak` (made before a schema upgrade) and by explicit `--db-backup`.
  With `synchronous=NORMAL` a *power loss or OS crash* can lose the last few commits (never corrupt the file); a process kill (what the smoke
  tests do) loses nothing. Switch to `FULL` in `Db::Open` if that matters.
* Autosave cost: the interval is unchanged (`--save-interval`, 60 s). One transaction, one WAL append, no fsync per commit; the WAL is
  checkpointed automatically and truncated on a clean shutdown.

## 4. Schema (version 1, `Store.cpp`)

Migrations live in code (`StoreMigrations()`); the table `schema_version(version, name, applied_at)` records what was applied. Opening runs the
pending ones in order, each in its own transaction; before the first upgrade of a non-empty file a copy `f3.db.pre-v<from>-v<to>.bak` is made.
A database written by a **newer** program is refused with a clear error and not touched.

| table | columns | notes |
|---|---|---|
| `meta` | `key`, `value` | `json_migrated_at` once the old JSON saves were imported |
| `accounts` | `id`, `acc_key` (lower case, unique), `name`, `algo`, `iterations`, `salt` BLOB, `hash` BLOB, `created`, `last_login`, `flags` | PBKDF2-HMAC-SHA256 as before; `last_login` is set on every successful login; `flags` bit 0 reserved for bans |
| `characters` | `id`, `account_id` -> accounts, `slot` (unique per account, only 0 used), `name`, `level`, `xp`, `map`, `x`, `y`, `hp`, `max_hp`, `dead`, `created`, `last_seen`, `data` JSON | `data` holds the bulky rest of the character document: SPECIAL, skill ranks, perks, traits, inventory, vars, gvars. Indexed by name (NOCASE) and map |
| `friends` | `character_id`, `ord`, `friend`, `friend_key` | primary key `(character_id, friend_key)`; index on `friend_key` ("who lists X") |
| `quests` | `character_id`, `quest_id`, `ord`, `title`, `body`, `state`, `updated` | state 0 active / 1 completed / 2 failed |
| `mail` | `id` (database wide), `recipient_id` -> accounts, `sender`, `body`, `sent_at`, `is_read` | index `(recipient_id, is_read)` for the unread count. The ids shown to players (`/inbox`, Pip-Boy) are these ids; they are no longer 1..n per mailbox |
| `world_objects` | `map`, `obj_key` (`<proto key>@x,y`), `is_open`, `locked`, `flags`, `dc`, `lock_key`, `contents` JSON | doors and containers; a save replaces all rows of a map atomically |
| `events` | `id`, `ts`, `kind`, `actor`, `data` JSON | audit log, indices on `(kind, id)` and `(actor, id)` |

The character document is exactly what `Server::CharacterToJson` produced for the JSON files. `Store::SaveCharacter` splits `quests` and `friends`
out into their tables and fills the searchable columns; `Store::LoadCharacter` puts them back, so `ApplyCharacter` is unchanged. A character whose
`data` cannot be parsed refuses the login (`ServerError`) instead of starting a fresh character over it; the row is left as it is.
All foreign keys cascade on delete (there is no account deletion yet).

### Audit log (`Store::LogEvent(kind, actor, json)`, `Server::Audit`)

| kind | actor | data |
|---|---|---|
| `register`, `login` | account name | `{ip}` |
| `login_failed` | name as typed | `{ip, reason}` (bad credentials and rate limited only) |
| `logout` | character | `{map, level}` |
| `kick` | account | `{reason: "duplicate login", ip}` |
| `character_created` | character | `{level, account}` |
| `level_up` | character | `{level, xp}` |
| `kill` | killer (empty = no killer) | `{victim, victim_is_player, map, x, y}` for every player death and every kill by a player |
| `mail_sent` | sender | `{to, mail_id, length}` (the text is not logged) |
| `migrate_json` | `server` | import counters |

Logging never throws and never blocks gameplay on failure. The table is not pruned automatically (rows are small; trade events will use it later).

## 5. One-time migration from the JSON saves

At start, after the database is opened, `MigrateJsonSaves(force = false)` looks at `<saves>/accounts`, `<saves>/mail` and `<saves>/world`:

* none exist: nothing happens;
* the database is **empty** (no accounts, mail or world rows) and was never migrated: everything is imported in **one transaction** (accounts with
  their hashes and characters, mail in id order with the read flag, world state per map), the marker `json_migrated_at` and an audit event are
  written, the transaction commits, and only then the three folders are renamed to `accounts.migrated`, `mail.migrated`, `world.migrated`
  (`.migrated.1`, `.2` ... if the name is taken). **Nothing is deleted**, also the `.bak` / `.tmp` leftovers and the files that could not be read
  (unparsable JSON, accounts without name / salt / hash, mail of unknown accounts, world files without objects) which are listed in the log
  and counted as problems. `created` of an account is taken from the file's modification time;
* the database holds data but the marker is absent: the old folders are **not** imported and not touched, a warning names `--migrate-json`;
* the marker is set and old folders exist (the process died between commit and rename): only the rename is finished, no second import.

`f3_server --saves DIR --migrate-json` runs the same importer as a tool (no network, exits) with merge semantics, so it is **idempotent**: accounts that
exist are skipped, mail is only imported for accounts imported in that run, a map that already has world rows is skipped; a second run finds no
folders ("nothing to migrate"). To re-import a backup, copy / rename `accounts.migrated` back to `accounts` first. The legacy duplicate-ish cases
(`acc_Foo.json` and `acc_foo.json`) import the first file and report the second.

## 6. Tools and console

| | |
|---|---|
| `f3_server --saves DIR --migrate-json` | import old JSON folders (above) |
| `f3_server --saves DIR --export-json OUT` | write `OUT/accounts/acc_<key>.json`, `OUT/mail/<key>.json`, `OUT/world/<map>.json` in the **old format** (re-importable; the account files contain the password hashes, treat like the database) |
| `f3_server --saves DIR --db-dump [TABLE]` | schema version, every table as JSON lines, character `data` expanded, salt / hash shown as `(blob N bytes)`; opens read-only, safe on a live server |
| `f3_server --saves DIR --db-backup FILE` | consistent copy of the live database (`VACUUM INTO`; refuses to overwrite). Copying `f3.db` alone is **not** a backup while the server runs (the `-wal` file holds the newest commits) |
| console `dbinfo` | path, schema version, journal mode, size, WAL size, pages, row counts per table |
| console `events [n] [kind] [actor]` | the last n (default 20) audit events, newest first |

`--saves DIR` keeps its meaning (the folder that holds the saves; the database is `DIR/f3.db`).

## 7. Damaged databases

`Db::Open` runs `PRAGMA quick_check` before anything is read or written. A file that is not a database, was truncated or has damaged pages
makes the server **refuse to start** (exit code 1) with a message naming the file; the file is never modified, moved, "repaired" or recreated,
so nothing is silently lost. Restore a backup (`f3.db.pre-v*.bak`, a `--db-backup` copy, or `--export-json` output imported with `--migrate-json`
into a fresh `--saves` folder) or move the file away to start empty. A database from a newer program is refused the same way.

## 8. Known gaps

* One writer process only (the server); the tools may read while it runs. No replication.
* Ground items, NPC state and the rest of the live world are still not persisted (as before); only players, mail and doors / containers.
* `synchronous=NORMAL` (see section 3); the audit log is never pruned; no account deletion / ban commands yet (`accounts.flags` is reserved);
  characters of one account other than slot 0 exist in the schema only.
* The importer cannot recover mail ids: they are reassigned in file order (the client only uses them within one snapshot).
* Old-save symptom: an `f3.db` from an earlier build could make every Junktown NPC absent. The cause was the persisted per-player `CleanupJunktown` global (`docs/amx-scripts.md`),
  not the database layout; it is ignored by default now and needs no migration.
