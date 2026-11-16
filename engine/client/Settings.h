#pragma once
// Client settings: game directory auto-detection and the persisted client.cfg (key=value lines).
//
// Game directory (the original install: resource.rht + data\*.grp) is resolved in this order:
//   1. --gamedir DIR            2. env F3_GAMEDIR           3. client.cfg  gamedir=...  (next to the exe, then cwd)
//   4. well known places: F:\_work\F3_Demo, <root>\..\F3_Demo, <root>\F3_Demo for the exe dir / cwd and their parents
// The first valid one is written back to client.cfg so the next start needs no argument.
#include <map>
#include <string>
#include <vector>

namespace f3 {

// Command line as UTF-8 (on Windows main()'s argv is in the ANSI code page, Cyrillic names would be mangled).
std::vector<std::string> Utf8Args(int argc, char** argv);
std::string ExeDir();
// cwd and exe dir plus their parents (up to 6 levels), without duplicates; forward slashes.
std::vector<std::string> SearchRoots();
bool IsGameDir(const std::string& dir);

struct GameDirResult {
    std::string dir;      // empty if nothing valid was found
    std::string source;   // "--gamedir", "F3_GAMEDIR", "client.cfg", "auto-detected (..)"
    std::vector<std::string> tried;
};
GameDirResult DetectGameDir(const std::string& cliArg);

using Config = std::map<std::string, std::string>;
std::string ConfigPath();       // client.cfg next to the exe
Config LoadConfig();            // merges cwd/client.cfg under exe-dir/client.cfg
bool SaveConfig(const Config& c);

}  // namespace f3
