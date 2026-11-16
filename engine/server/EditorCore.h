#pragma once
// Content editor, HTTP-free core (docs/editor.md): the request handler behind the loopback-only web editor. It maps a Request to a Response
// and never touches sockets or game state; everything that needs the running server (validation against the base layer, /reload, the
// original English.stf) goes through the `Host` callbacks, which the server implements by posting a task to its own thread
// (Server::PostTask) and waiting for the answer. The same core is exercised by f3_editortest without a server.
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "Content.h"

namespace f3::editor {

using nlohmann::json;
using nlohmann::ordered_json;

struct Request {
    std::string method;                              // "GET", "PUT", "POST", "DELETE"
    std::string path;                                // "/api/list"
    std::map<std::string, std::string> query;        // decoded
    std::map<std::string, std::string> headers;      // lower-case names
    std::string body;
    std::string remoteAddr;                          // peer address as text ("127.0.0.1"); empty = unknown (rejected)
};

struct Response {
    int status = 200;
    std::string contentType = "application/json; charset=utf-8";
    std::string body;
};

// A snapshot of one original (base layer) prototype, taken once before the HTTP thread starts (the base layer never changes at run time).
struct BaseProto {
    std::string key, name, cls;                      // cls: weapon armor ammo misc critter door container usable
    ordered_json record;                             // the exporter's record of the prototype (the base value of the diff view)
};

// Everything the editor needs from the running server. Every callback must be safe to call from the HTTP thread (the server's versions
// post a task to the main loop and block with a timeout); a null callback means "not available" (tests without a server).
struct Host {
    std::vector<BaseProto> base;                     // empty without gamedata
    std::vector<std::string> maps;                   // loaded map names (spawn pickers)
    bool hasGamedata = false;
    std::function<ContentReport(const std::filesystem::path& dir)> validate;   // ContentDb::Build of a candidate folder against the base layer
    std::function<std::vector<std::string>(const std::string& what)> reload;   // "data": the report lines of Server::ReloadAll
    std::function<std::string(int id)> stf;                                    // original English.stf string
    std::function<std::vector<std::pair<int, std::string>>(const std::string& needle, size_t limit)> stfSearch;
    // Object picker (Server::ObjectsJson): map "" = the list of loaded maps, else the objects of that map filtered by `query`. Returns JSON text.
    std::function<std::string(const std::string& map, const std::string& query, int limit)> objects;
};

struct Options {
    std::filesystem::path contentDir;                // data/
    std::filesystem::path uiDir;                     // tools/editor/ (static front end); empty = no UI (API only)
    std::filesystem::path scriptsDir;                // scripts/ (object scripts live in scripts/objects/); empty = the script endpoints are off
    std::string token;                               // required in the X-F3-Token header of every /api call (and ?token= for the first page load)
    int port = 0;                                    // for the Host header check
    int validateTimeoutMs = 20000;
};

class Api {
public:
    Api(Options opt, Host host);
    Response Handle(const Request& req);             // never throws; every response carries no-store / nosniff headers' worth of safety via the caller

    // Security primitives, public for the tests -------------------------------------------------------------------------------
    static bool IsLoopbackAddr(const std::string& addr);                  // 127.0.0.0/8, ::1, ::ffff:127.x
    bool HostHeaderOk(const std::string& hostHeader) const;               // 127.0.0.1:<port> | localhost:<port> | [::1]:<port>
    static bool TokenEquals(const std::string& a, const std::string& b);  // constant time
    // Maps (kind, file) to a path inside data/ or returns false with the reason. kind: content kind or "locale/<lang>"; file: a flat file name ending in .json.
    // Only the known kinds, only *.json, no separators / dots-only / reserved device names; the result is verified to stay below contentDir.
    bool ResolveFile(const std::string& kind, const std::string& file, std::filesystem::path& out, std::string& why) const;
    static bool ValidFileName(const std::string& file);

    const Options& options() const { return opt_; }

private:
    Response Route(const Request& req);
    Response Static(const std::string& path);
    Response KindsResp();
    Response List(const Request& req);
    Response Get(const Request& req);
    Response Put(const Request& req);
    Response Delete(const Request& req);
    Response Validate(const Request& req);
    Response Reload(const Request& req);
    Response Refs();
    Response StfLookup(const Request& req);
    Response LocaleStatusResp();
    Response ObjectsResp(const Request& req);        // GET /api/objects?map=&q=&limit=
    Response ObjScriptStatus(const Request& req);    // GET /api/objscript?path=objects/x.lua
    Response ObjScriptCreate(const Request& req);    // POST /api/objscript/create {path}
    json ObjScriptInfo(const std::string& rel) const;

    struct FileInfo {
        std::string kind, file;
        bool generated = false, hasComments = false, ok = false;
        std::string error, text, etag;
        ordered_json doc;
    };
    FileInfo ReadFile(const std::string& kind, const std::string& file) const;   // parses with comments allowed
    std::vector<std::string> ListFiles(const std::string& kind, bool includeDrafts) const;
    static std::string Etag(const std::string& text);
    // Validation of a candidate: copies data/ to a temp folder (named "data" so messages read data/<kind>/<file>), replaces / removes one
    // file, runs the server's validator on it. Returns structured messages.
    struct Candidate { std::string kind, file; const std::string* text = nullptr; bool remove = false; };
    ContentReport RunValidate(const Candidate* c) const;
    static json Messages(const std::vector<std::string>& raw);
    Response Error(int status, const std::string& code, const std::string& msg, json extra = json::object()) const;
    Response Ok(const json& j) const;
    Response Save(const std::string& kind, const std::string& file, ordered_json doc, const json& opts, bool isDelete);
    bool WriteAtomic(const std::filesystem::path& p, const std::string& text, std::string& err) const;

    Options opt_;
    Host host_;
    std::mutex fileMutex_;                           // one writer at a time (HTTP threads); the server thread only reads files
    std::map<std::string, size_t> baseIndex_;        // lower-case key -> index in host_.base
};

// The schema of the data folder as served by /api/schema: built from the same constants the validator uses (skill names, SPECIAL keys,
// stat names, dialog stats, envelope version) plus the field tables of EditorSchema.cpp, which f3_editortest checks against the validator.
json BuildSchema();

// Per-key translation status of data/locale (LocaleCheck.cpp, the logic of --check-locale).
json LocaleStatus(const std::filesystem::path& contentDir);

// Content kinds the editor knows, in menu order.
struct KindInfo { const char* id; const char* label; const char* group; };
const std::vector<KindInfo>& Kinds();

}  // namespace f3::editor
