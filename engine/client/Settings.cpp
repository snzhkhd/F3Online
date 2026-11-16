#include "Settings.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace fs = std::filesystem;

namespace f3 {

std::vector<std::string> Utf8Args(int argc, char** argv) {
    std::vector<std::string> out;
#ifdef _WIN32
    // main()'s argv is in the ANSI code page: Cyrillic names would arrive as invalid UTF-8; the wide command line is exact
    int wc = 0;
    if (LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wc)) {
        for (int i = 0; i < wc; ++i) {
            const int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
            std::string a(n > 1 ? static_cast<size_t>(n) - 1 : 0, 'x');
            if (n > 1) WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, a.data(), n, nullptr, nullptr);
            out.push_back(a);
        }
        LocalFree(wargv);
        if (static_cast<int>(out.size()) == argc) return out;
        out.clear();
    }
#endif
    for (int i = 0; i < argc; ++i) out.push_back(argv[i]);
    return out;
}

namespace {
std::string Norm(const fs::path& p) {
    std::error_code ec;
    fs::path c = fs::weakly_canonical(p, ec);
    std::string s = (ec ? p : c).generic_string();
    while (s.size() > 3 && s.back() == '/') s.pop_back();
    return s;
}
std::string Trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
}  // namespace

std::string ExeDir() {
#ifdef _WIN32
    char buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameA(nullptr, buf, sizeof buf);
    if (n > 0 && n < sizeof buf) return Norm(fs::path(buf).parent_path());
#endif
    std::error_code ec;
    return Norm(fs::current_path(ec));
}

std::vector<std::string> SearchRoots() {
    std::vector<std::string> out;
    std::set<std::string> seen;
    std::error_code ec;
    for (std::string start : {Norm(fs::current_path(ec)), ExeDir()}) {
        fs::path p = start;
        for (int i = 0; i < 7 && !p.empty(); ++i) {
            std::string s = Norm(p);
            if (seen.insert(s).second) out.push_back(s);
            fs::path parent = p.parent_path();
            if (parent == p) break;
            p = parent;
        }
    }
    return out;
}

bool IsGameDir(const std::string& dir) {
    if (dir.empty()) return false;
    std::error_code ec;
    return fs::exists(fs::path(dir) / "resource.rht", ec) && fs::is_directory(fs::path(dir) / "data", ec);
}

std::string ConfigPath() { return (fs::path(ExeDir()) / "client.cfg").generic_string(); }

Config LoadConfig() {
    Config c;
    std::error_code ec;
    std::vector<std::string> files;
    files.push_back((fs::path(Norm(fs::current_path(ec))) / "client.cfg").generic_string());  // lower priority
    files.push_back(ConfigPath());
    for (const auto& f : files) {
        std::ifstream in(f);
        std::string line;
        while (std::getline(in, line)) {
            line = Trim(line);
            if (line.empty() || line[0] == '#' || line[0] == ';') continue;
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            c[Trim(line.substr(0, eq))] = Trim(line.substr(eq + 1));
        }
    }
    return c;
}

bool SaveConfig(const Config& c) {
    std::ofstream out(ConfigPath(), std::ios::trunc);
    if (!out) return false;
    out << "# F3Online client settings (written automatically)\n";
    for (const auto& [k, v] : c) out << k << "=" << v << "\n";
    return out.good();
}

GameDirResult DetectGameDir(const std::string& cliArg) {
    GameDirResult r;
    Config cfg = LoadConfig();
    auto consider = [&](const std::string& dir, const std::string& source) {
        if (!r.dir.empty() || dir.empty()) return;
        r.tried.push_back(dir + " (" + source + ")");
        if (IsGameDir(dir)) { r.dir = Norm(dir); r.source = source; }
    };
    consider(cliArg, "--gamedir");
    if (const char* env = std::getenv("F3_GAMEDIR")) consider(env, "F3_GAMEDIR");
    if (auto it = cfg.find("gamedir"); it != cfg.end()) consider(it->second, "client.cfg");
    consider("F:/_work/F3_Demo", "auto-detected");
    for (const std::string& root : SearchRoots()) {
        consider(root + "/../F3_Demo", "auto-detected");
        consider(root + "/F3_Demo", "auto-detected");
        consider(root, "auto-detected");
    }
    for (const char* d : {"C:/Games/Van Buren", "C:/Games/F3_Demo", "D:/F3_Demo", "D:/Games/F3_Demo", "C:/F3_Demo"}) consider(d, "auto-detected");
    if (!r.dir.empty() && r.source != "client.cfg") {  // persist what we found / were told
        Config c = LoadConfig();
        c["gamedir"] = r.dir;
        SaveConfig(c);
    }
    return r;
}

}  // namespace f3
