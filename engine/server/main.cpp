// f3_server: headless authoritative server.
//   f3_server [--port N] [--scripts DIR] [--data DIR | --no-data] [--seed N] [--tickrate N] [--run-seconds N] [--dev-kit]
// --data defaults to the nearest gamedata/server (output of f3_dataimport) next to the binary or in a parent of the cwd.
//   --content DIR (default: nearest data/), --lenient, --validate-data (check data/ and exit), --dev-commands (chat /reload; implied by --dev-kit)
//   --saves DIR (holds f3.db), --save-interval SEC
//   database tools (no network, exit when done): --migrate-json | --export-json DIR | --db-dump [TABLE] | --db-backup FILE  (docs/persistence.md)
// Console commands on stdin: reload [data|scripts|all] | status | dbinfo | events [n] [kind] | say <text> | quit
#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include "Migrate.h"
#include "Server.h"
#include "common/log.h"

namespace {
f3::Server* g_server = nullptr;
void OnSignal(int) { if (g_server) g_server->RequestStop(); }

// Look for a scripts/ directory next to the binary, in parents, or in the cwd.
std::filesystem::path FindScripts(const char* argv0) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path exeDir = fs::absolute(fs::path(argv0), ec).parent_path();
    for (fs::path d : {fs::current_path(ec), exeDir}) {
        for (int i = 0; i < 6 && !d.empty(); ++i, d = d.parent_path()) {
            if (fs::exists(d / "scripts" / "main.lua", ec)) return d / "scripts";
            if (d == d.root_path()) break;
        }
    }
    return "scripts";
}

std::filesystem::path FindData(const char* argv0) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path exeDir = fs::absolute(fs::path(argv0), ec).parent_path();
    for (fs::path d : {fs::current_path(ec), exeDir}) {
        for (int i = 0; i < 6 && !d.empty(); ++i, d = d.parent_path()) {
            if (fs::exists(d / "gamedata" / "server" / "manifest.json", ec)) return d / "gamedata" / "server";
            if (d == d.root_path()) break;
        }
    }
    return {};
}
}  // namespace

int main(int argc, char** argv) {
    f3::Server::Config cfg;
    std::string dbTool, dbToolArg;
    bool scriptsGiven = false, dataGiven = false, contentGiven = false, validateOnly = false, checkLocale = false, strictLocale = false;
    std::string srcDir;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) { std::cerr << "missing value for " << name << "\n"; std::exit(2); }
            return argv[++i];
        };
        if (a == "--port") cfg.port = static_cast<uint16_t>(std::atoi(next("--port")));
        else if (a == "--scripts") { cfg.scriptsDir = next("--scripts"); scriptsGiven = true; }
        else if (a == "--data") { cfg.dataDir = next("--data"); dataGiven = true; }
        else if (a == "--no-data") cfg.noData = true;
        else if (a == "--content") { cfg.contentDir = next("--content"); contentGiven = true; }
        else if (a == "--lenient") cfg.lenient = true;
        else if (a == "--validate-data") validateOnly = true;
        else if (a == "--dev-commands") cfg.devCommands = true;
        else if (a == "--lang") cfg.lang = next("--lang");
        else if (a == "--locale") cfg.localeDir = next("--locale");
        else if (a == "--check-locale") checkLocale = true;
        else if (a == "--strict") strictLocale = true;
        else if (a == "--src") srcDir = next("--src");
        else if (a == "--seed") cfg.seed = static_cast<uint32_t>(std::strtoul(next("--seed"), nullptr, 10));
        else if (a == "--tickrate") cfg.tickRate = std::max(1, std::atoi(next("--tickrate")));
        else if (a == "--dev-kit") cfg.devKit = true;
        else if (a == "--allow-guest") cfg.allowGuest = true;
        else if (a == "--saves") cfg.savesDir = next("--saves");
        else if (a == "--save-interval") cfg.saveIntervalSec = std::atoi(next("--save-interval"));
        else if (a == "--pbkdf2-iterations") cfg.pbkdf2Iterations = std::atoi(next("--pbkdf2-iterations"));
        else if (a == "--run-seconds") cfg.maxRunSeconds = std::atoi(next("--run-seconds"));
        else if (a == "--migrate-json") dbTool = "migrate-json";
        else if (a == "--export-json") { dbTool = "export-json"; dbToolArg = next("--export-json"); }
        else if (a == "--db-backup") { dbTool = "db-backup"; dbToolArg = next("--db-backup"); }
        else if (a == "--db-dump") { dbTool = "db-dump"; if (i + 1 < argc && std::strncmp(argv[i + 1], "--", 2) != 0) dbToolArg = argv[++i]; }
        else if (a == "--bind") cfg.bindHost = next("--bind");
        else if (a == "--local") cfg.bindHost = "127.0.0.1";
        else { std::cerr << "usage: f3_server [--lang CODE] [--locale DIR] [--check-locale [--strict] [--src DIR]] [--content DIR] [--lenient] [--validate-data] [--dev-commands] [--port N] [--bind ADDR | --local] [--scripts DIR] [--data DIR | --no-data] [--tickrate N] [--run-seconds N] [--dev-kit] [--allow-guest] [--saves DIR] [--save-interval SEC]\n"; return 2; }
    }
    if (!scriptsGiven) cfg.scriptsDir = FindScripts(argv[0]);
    if (!dbTool.empty()) {   // database tools: only need the saves folder
        if (cfg.savesDir.empty()) cfg.savesDir = std::filesystem::absolute(cfg.scriptsDir).parent_path() / "saves";
        return f3::RunDbTool(dbTool, cfg.savesDir, dbToolArg);
    }
    if (!dataGiven && !cfg.noData) cfg.dataDir = FindData(argv[0]);
    if (!contentGiven) cfg.contentDir = f3::FindContentDir(argv[0]);
    if (checkLocale) {   // --check-locale: coverage of data/locale (missing / extra / unused keys, placeholders), exit 0 / 1 (2 = no locale)
        if (cfg.contentDir.empty()) { std::cerr << "check-locale: no data/ folder found (use --content DIR)" << std::endl; return 2; }
        return f3::RunCheckLocale(cfg.contentDir, cfg.localeDir, srcDir, std::filesystem::absolute(argv[0]), strictLocale);
    }
    if (validateOnly) {   // --validate-data: check data/ (against gamedata when present), print the report, exit 0 / 1 (2 = gamedata unusable)
        if (cfg.contentDir.empty()) { std::cerr << "validate-data: no data/ folder found (use --content DIR)" << std::endl; return 2; }
        return f3::RunValidateData(cfg.noData ? std::filesystem::path() : cfg.dataDir, cfg.contentDir, cfg.lenient);
    }
    if (cfg.contentDir.empty()) f3::log::Info("no data/ folder found: built-in content defaults (see docs/data-format.md)");
    else f3::log::Info("content: %s", std::filesystem::absolute(cfg.contentDir).string().c_str());
    if (cfg.dataDir.empty()) f3::log::Info("no gamedata/server found: running with the built-in test map (see docs/server-data.md)");
    else f3::log::Info("data: %s", std::filesystem::absolute(cfg.dataDir).string().c_str());
    f3::log::Info("scripts: %s", std::filesystem::absolute(cfg.scriptsDir).string().c_str());

    f3::Server server(cfg);
    g_server = &server;
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    if (!server.Init()) return 1;

    std::thread console([&server] {
        std::string line;
        while (std::getline(std::cin, line)) server.PostCommand(line);
    });
    console.detach();  // blocked in getline; process exit tears it down

    server.Run();
    g_server = nullptr;
    return 0;
}
