// f3_server: headless authoritative server.
//   f3_server [--port N] [--scripts DIR] [--data DIR | --no-data] [--tickrate N] [--run-seconds N] [--dev-kit]
// --data defaults to the nearest gamedata/server (output of f3_dataimport) next to the binary or in a parent of the cwd.
// Console commands on stdin: reload | status | say <text> | quit
#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
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
    bool scriptsGiven = false, dataGiven = false;
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
        else if (a == "--tickrate") cfg.tickRate = std::max(1, std::atoi(next("--tickrate")));
        else if (a == "--dev-kit") cfg.devKit = true;
        else if (a == "--run-seconds") cfg.maxRunSeconds = std::atoi(next("--run-seconds"));
        else if (a == "--bind") cfg.bindHost = next("--bind");
        else if (a == "--local") cfg.bindHost = "127.0.0.1";
        else { std::cerr << "usage: f3_server [--port N] [--bind ADDR | --local] [--scripts DIR] [--data DIR | --no-data] [--tickrate N] [--run-seconds N] [--dev-kit]\n"; return 2; }
    }
    if (!scriptsGiven) cfg.scriptsDir = FindScripts(argv[0]);
    if (!dataGiven && !cfg.noData) cfg.dataDir = FindData(argv[0]);
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
