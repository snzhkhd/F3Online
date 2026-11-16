// Content editor core (docs/editor.md): request handler, path confinement, token / host checks, record listing, atomic saves, validation of candidates.
#include "EditorCore.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <fstream>
#include <sstream>
#include <system_error>
#include "DataFormat.h"
#include "common/protocol_game.h"
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace f3::editor {

namespace fs = std::filesystem;

namespace {

std::string Low(std::string s) { for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }

int CurrentPid() {
#ifdef _WIN32
    return _getpid();
#else
    return static_cast<int>(getpid());
#endif
}

bool StartsWith(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }

const KindInfo* FindKind(const std::string& id) {
    for (const KindInfo& k : Kinds()) if (id == k.id) return &k;
    return nullptr;
}

bool ValidLang(const std::string& l) {
    if (l.size() < 2 || l.size() > 3) return false;
    for (unsigned char c : l) if (!(c >= 'a' && c <= 'z')) return false;
    return true;
}

bool IsLocaleKind(const std::string& kind, std::string* lang = nullptr) {
    if (!StartsWith(kind, "locale/")) return false;
    const std::string l = kind.substr(7);
    if (!ValidLang(l)) return false;
    if (lang) *lang = l;
    return true;
}

bool ReservedDeviceName(const std::string& file) {
    std::string stem = Low(file.substr(0, file.find('.')));
    while (!stem.empty() && stem.back() == ' ') stem.pop_back();
    static const char* const names[] = {"con", "prn", "aux", "nul"};
    for (const char* n : names) if (stem == n) return true;
    if (stem.size() == 4 && (StartsWith(stem, "com") || StartsWith(stem, "lpt")) && stem[3] >= '0' && stem[3] <= '9') return true;
    return false;
}

// "data/perks/perks.json: records[3].x: message" -> {file, path, message}
json OneMessage(const std::string& raw) {
    json m = json::object();
    m["raw"] = raw;
    const size_t a = raw.find(": ");
    if (a == std::string::npos) { m["file"] = ""; m["path"] = ""; m["message"] = raw; return m; }
    const size_t b = raw.find(": ", a + 2);
    m["file"] = raw.substr(0, a);
    if (b == std::string::npos) { m["path"] = ""; m["message"] = raw.substr(a + 2); }
    else { m["path"] = raw.substr(a + 2, b - a - 2); m["message"] = raw.substr(b + 2); }
    return m;
}

std::string ClsOfRecord(const ordered_json& rec, const BaseProto* b) {
    if (b) {
        if (b->cls == "weapon" || b->cls == "armor" || b->cls == "ammo") return b->cls;
        if (b->cls == "misc") return "kit";
        if (b->cls == "critter") return "npc";
        return "other";
    }
    if (rec.contains("weapon")) return "weapon";
    if (rec.contains("armor") || rec.contains("head_armor")) return "armor";
    if (rec.contains("ammo")) return "ammo";
    return "other";
}

std::string GetStr(const ordered_json& j, const char* k) {
    auto it = j.find(k);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------- security primitives

bool Api::IsLoopbackAddr(const std::string& a) {
    if (a == "::1") return true;
    std::string s = a;
    if (StartsWith(Low(s), "::ffff:")) s = s.substr(7);
    if (s.size() < 5 || s.compare(0, 4, "127.") != 0) return false;
    int dots = 0;
    for (char c : s) { if (c == '.') ++dots; else if (!std::isdigit(static_cast<unsigned char>(c))) return false; }
    return dots == 3;
}

bool Api::HostHeaderOk(const std::string& hostHeader) const {
    const std::string h = Low(hostHeader);
    for (const char* name : {"127.0.0.1", "localhost", "[::1]"}) {
        if (opt_.port == 0) { if (h == name || StartsWith(h, std::string(name) + ":")) return true; }
        else if (h == std::string(name) + ":" + std::to_string(opt_.port)) return true;
    }
    return false;
}

bool Api::TokenEquals(const std::string& a, const std::string& b) {
    unsigned diff = static_cast<unsigned>(a.size() ^ b.size());
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    return diff == 0;
}

bool Api::ValidFileName(const std::string& f) {
    if (f.size() < 6 || f.size() > 80) return false;                     // ".json" plus at least one character
    if (f[0] == '.') return false;
    if (f.find("..") != std::string::npos) return false;
    for (unsigned char c : f) if (!(std::isalnum(c) || c == '_' || c == '.' || c == '-')) return false;   // no separators, drive colons, spaces
    if (Low(f.substr(f.size() - 5)) != ".json") return false;
    if (f.size() == 5) return false;
    if (ReservedDeviceName(f)) return false;
    return true;
}

bool Api::ResolveFile(const std::string& kind, const std::string& file, fs::path& out, std::string& why) const {
    std::string dir;
    if (FindKind(kind)) dir = kind;
    else if (IsLocaleKind(kind)) dir = kind;               // "locale/<lang>"
    else { why = "unknown kind '" + kind + "'"; return false; }
    if (!ValidFileName(file)) { why = "invalid file name '" + file + "' (letters, digits, _ . - and the .json extension only)"; return false; }
    if (opt_.contentDir.empty()) { why = "no data folder is configured"; return false; }
    std::error_code ec;
    const fs::path root = fs::weakly_canonical(opt_.contentDir, ec);
    if (ec) { why = "cannot resolve the data folder"; return false; }
    const fs::path p = fs::weakly_canonical(root / dir / file, ec);
    if (ec) { why = "cannot resolve the path"; return false; }
    // must be exactly root/<dir>/<file>: same parent, no escape (symlinks, junctions or case tricks resolve to something else and are refused)
    const fs::path want = (root / dir).lexically_normal();
    if (p.parent_path().lexically_normal() != fs::weakly_canonical(want, ec).lexically_normal() || p.filename().string() != file) { why = "path leaves the data folder"; return false; }
    const std::string rs = root.generic_string(), ps = p.generic_string();
    if (ps.size() <= rs.size() + 1 || ps.compare(0, rs.size(), rs) != 0 || ps[rs.size()] != '/') { why = "path leaves the data folder"; return false; }
    out = p;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------- construction / dispatch

Api::Api(Options opt, Host host) : opt_(std::move(opt)), host_(std::move(host)) {
    for (size_t i = 0; i < host_.base.size(); ++i) baseIndex_[Low(host_.base[i].key)] = i;
}

Response Api::Ok(const json& j) const {
    Response r;
    r.body = j.dump();
    return r;
}

Response Api::Error(int status, const std::string& code, const std::string& msg, json extra) const {
    Response r;
    r.status = status;
    extra["ok"] = false;
    extra["code"] = code;
    extra["error"] = msg;
    r.body = extra.dump();
    return r;
}

Response Api::Handle(const Request& req) {
    try {
        // 1. only loopback peers, only our own Host names (DNS rebinding), whatever the route
        if (!IsLoopbackAddr(req.remoteAddr)) return Error(403, "forbidden", "only loopback clients are served");
        auto h = req.headers.find("host");
        if (h == req.headers.end() || !HostHeaderOk(h->second)) return Error(403, "bad_host", "unexpected Host header (use http://127.0.0.1:<port>/)");
        return Route(req);
    } catch (const std::exception& e) {
        return Error(500, "internal", std::string("internal error: ") + e.what());
    } catch (...) {
        return Error(500, "internal", "internal error");
    }
}

Response Api::Route(const Request& req) {
    const std::string& p = req.path;
    if (!StartsWith(p, "/api/")) {
        if (req.method != "GET" && req.method != "HEAD") return Error(405, "method", "GET only");
        if (p == "/" || p == "/index.html") {   // the page itself needs the token (?token=...), which the page then keeps in sessionStorage
            auto q = req.query.find("token");
            if (q == req.query.end() || !TokenEquals(q->second, opt_.token)) return Error(401, "token", "missing or wrong token: open the URL printed by f3_server (http://127.0.0.1:<port>/?token=...)");
        }
        return Static(p);
    }
    // 2. the API: custom header with the per-run token; browsers cannot add it to cross-site requests without a CORS preflight, which we never grant
    auto t = req.headers.find("x-f3-token");
    if (t == req.headers.end() || !TokenEquals(t->second, opt_.token)) return Error(401, "token", "missing or wrong X-F3-Token header");
    const bool mutating = req.method == "PUT" || req.method == "POST" || req.method == "DELETE";
    if (mutating) {
        auto ct = req.headers.find("content-type");
        if (ct == req.headers.end() || Low(ct->second).find("application/json") == std::string::npos) return Error(415, "content_type", "Content-Type: application/json required");
    }
    const std::string ep = p.substr(5);
    auto need = [&](const char* method) { return req.method == method; };
    if (ep == "kinds") return need("GET") ? KindsResp() : Error(405, "method", "GET");
    if (ep == "list") return need("GET") ? List(req) : Error(405, "method", "GET");
    if (ep == "get") return need("GET") ? Get(req) : Error(405, "method", "GET");
    if (ep == "put") return need("PUT") ? Put(req) : Error(405, "method", "PUT");
    if (ep == "delete") return need("DELETE") ? Delete(req) : Error(405, "method", "DELETE");
    if (ep == "validate") return need("POST") ? Validate(req) : Error(405, "method", "POST");
    if (ep == "format") {
        if (!need("POST")) return Error(405, "method", "POST");
        ordered_json b = ordered_json::parse(req.body, nullptr, false);
        if (b.is_discarded() || !b.contains("doc")) return Error(400, "bad_request", "body must be {\"doc\": ...}");
        return Ok(json{{"ok", true}, {"text", EmitJsonText(b["doc"])}});
    }
    if (ep == "reload") return need("POST") ? Reload(req) : Error(405, "method", "POST");
    if (ep == "schema") {
        if (!need("GET")) return Error(405, "method", "GET");
        return Ok(BuildSchema());
    }
    if (ep == "locale-status") return need("GET") ? LocaleStatusResp() : Error(405, "method", "GET");
    if (ep == "refs") return need("GET") ? Refs() : Error(405, "method", "GET");
    if (ep == "stf") return need("GET") ? StfLookup(req) : Error(405, "method", "GET");
    return Error(404, "not_found", "unknown endpoint /api/" + ep);
}

Response Api::Static(const std::string& path) {
    if (opt_.uiDir.empty()) return Error(404, "no_ui", "the editor front end (tools/editor) was not found; the API is available under /api/");
    std::string name = path == "/" ? "index.html" : path.substr(1);
    if (name.empty() || name.size() > 64 || name.find('/') != std::string::npos || name.find('\\') != std::string::npos || name[0] == '.' || name.find("..") != std::string::npos) return Error(404, "not_found", "no such file");
    for (unsigned char c : name) if (!(std::isalnum(c) || c == '_' || c == '.' || c == '-')) return Error(404, "not_found", "no such file");
    const size_t dot = name.rfind('.');
    const std::string ext = dot == std::string::npos ? "" : Low(name.substr(dot));
    static const std::map<std::string, std::string> types = {
        {".html", "text/html; charset=utf-8"}, {".css", "text/css; charset=utf-8"}, {".js", "text/javascript; charset=utf-8"},
        {".svg", "image/svg+xml"}, {".json", "application/json; charset=utf-8"}, {".ico", "image/x-icon"}, {".png", "image/png"}, {".txt", "text/plain; charset=utf-8"}};
    auto ty = types.find(ext);
    if (ty == types.end()) return Error(404, "not_found", "no such file");
    std::ifstream f(opt_.uiDir / name, std::ios::binary);
    if (!f) return Error(404, "not_found", "no such file");
    std::stringstream ss;
    ss << f.rdbuf();
    Response r;
    r.contentType = ty->second;
    r.body = ss.str();
    return r;
}

// ---------------------------------------------------------------------------------------------------------------- files

std::string Api::Etag(const std::string& text) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : text) { h ^= c; h *= 1099511628211ull; }
    char b[24];
    std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(h));
    return b;
}

std::vector<std::string> Api::ListFiles(const std::string& kind, bool includeDrafts) const {
    std::vector<std::string> v;
    std::error_code ec;
    const fs::path dir = opt_.contentDir / kind;
    if (!fs::is_directory(dir, ec)) return v;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file()) continue;
        const std::string n = e.path().filename().string();
        if (!ValidFileName(n)) continue;
        if (!includeDrafts && n[0] == '_') continue;
        v.push_back(n);
    }
    std::sort(v.begin(), v.end());
    return v;
}

Api::FileInfo Api::ReadFile(const std::string& kind, const std::string& file) const {
    FileInfo fi;
    fi.kind = kind;
    fi.file = file;
    fs::path p;
    std::string why;
    if (!ResolveFile(kind, file, p, why)) { fi.error = why; return fi; }
    std::ifstream f(p, std::ios::binary);
    if (!f) { fi.error = "cannot open file"; return fi; }
    std::stringstream ss;
    ss << f.rdbuf();
    fi.text = ss.str();
    fi.etag = Etag(fi.text);
    ordered_json strict = ordered_json::parse(fi.text, nullptr, false, false);
    ordered_json lax = strict.is_discarded() ? ordered_json::parse(fi.text, nullptr, false, true) : strict;
    if (lax.is_discarded()) {
        try { (void)ordered_json::parse(fi.text, nullptr, true, true); } catch (const std::exception& e) { fi.error = e.what(); }
        if (fi.error.empty()) fi.error = "invalid JSON";
        return fi;
    }
    fi.hasComments = strict.is_discarded();
    fi.doc = std::move(lax);
    fi.ok = true;
    fi.generated = fi.doc.is_object() && fi.doc.contains("generator");
    return fi;
}

// ---------------------------------------------------------------------------------------------------------------- validation of candidates

ContentReport Api::RunValidate(const Candidate* c) const {
    static std::atomic<int> counter{0};
    std::error_code ec;
    const fs::path tmpRoot = fs::temp_directory_path(ec) / ("f3editor_" + std::to_string(CurrentPid()) + "_" + std::to_string(++counter));
    const fs::path dir = tmpRoot / "data";   // the folder name appears in every message: data/<kind>/<file>: path: message
    ContentReport rep;
    fs::create_directories(dir, ec);
    if (ec) { rep.Error("data", "$", "cannot create a temporary folder for validation: " + ec.message()); return rep; }
    // copy everything the validator reads (the locale folder is not part of it)
    for (const auto& e : fs::directory_iterator(opt_.contentDir, ec)) {
        const std::string n = e.path().filename().string();
        if (e.is_regular_file()) { fs::copy_file(e.path(), dir / n, fs::copy_options::overwrite_existing, ec); continue; }
        if (!e.is_directory() || n == "locale") continue;
        fs::create_directories(dir / n, ec);
        for (const auto& fe : fs::directory_iterator(e.path(), ec))
            if (fe.is_regular_file()) fs::copy_file(fe.path(), dir / n / fe.path().filename(), fs::copy_options::overwrite_existing, ec);
    }
    if (c) {
        const fs::path target = dir / c->kind / c->file;
        fs::create_directories(target.parent_path(), ec);
        if (c->remove) fs::remove(target, ec);
        else if (c->text) { std::ofstream f(target, std::ios::binary); f << *c->text; }
    }
    if (host_.validate) rep = host_.validate(dir);
    else {
        ContentDb db;
        ContentDb::Build(dir, nullptr, false, db, rep);
    }
    fs::remove_all(tmpRoot, ec);
    return rep;
}

json Api::Messages(const std::vector<std::string>& raw) {
    json a = json::array();
    for (const std::string& r : raw) a.push_back(OneMessage(r));
    return a;
}


Response Api::Validate(const Request& req) {
    ordered_json b = req.body.empty() ? ordered_json::object() : ordered_json::parse(req.body, nullptr, false);
    if (b.is_discarded() || !b.is_object()) return Error(400, "bad_request", "body must be a JSON object");
    std::lock_guard<std::mutex> lk(fileMutex_);
    ContentReport rep;
    std::string own;
    if (b.contains("doc")) {   // a draft that is not saved: replace that one file in a throw-away copy
        const std::string kind = GetStr(b, "kind"), file = GetStr(b, "file");
        fs::path p;
        std::string why;
        if (!ResolveFile(kind, file, p, why)) return Error(400, "bad_path", why);
        if (IsLocaleKind(kind)) return Error(400, "bad_kind", "locale files are not validated by the content validator (the locale tab checks them)");
        const std::string text = EmitJsonText(b["doc"]);
        Candidate c{kind, file, &text, false};
        rep = RunValidate(&c);
        own = "data/" + kind + "/" + file + ":";
    } else {
        rep = RunValidate(nullptr);
    }
    json out = json::object();
    out["ok"] = rep.errors.empty();
    out["errors"] = Messages(rep.errors);
    out["warnings"] = Messages(rep.warnings);
    out["own"] = own;   // messages whose "file" starts with this belong to the file being edited
    return Ok(out);
}

Response Api::Reload(const Request& req) {
    (void)req;
    if (!host_.reload) return Error(501, "no_server", "reload is not available (no running server attached)");
    const std::vector<std::string> lines = host_.reload("data");   // runs ReloadAll("data") on the server thread
    json out = json::object();
    bool ok = !lines.empty();
    std::string rep;
    for (const std::string& l : lines) { if (l.find("data reload FAILED") != std::string::npos || l.find("no data/ folder") != std::string::npos) ok = false; rep += l + "\n"; }
    out["ok"] = ok;
    out["lines"] = lines;
    out["report"] = rep;
    return Ok(out);
}

Response Api::StfLookup(const Request& req) {
    json out = json::object();
    out["available"] = host_.hasGamedata && host_.stf != nullptr;
    if (!out["available"].get<bool>()) { out["ok"] = true; out["text"] = ""; out["results"] = json::array(); return Ok(out); }
    auto q = req.query.find("q");
    if (q != req.query.end() && host_.stfSearch) {
        json arr = json::array();
        for (const auto& [id, text] : host_.stfSearch(q->second, 40)) arr.push_back(json{{"id", id}, {"text", text}});
        out["ok"] = true;
        out["results"] = arr;
        return Ok(out);
    }
    auto id = req.query.find("id");
    if (id == req.query.end()) return Error(400, "bad_request", "id or q required");
    int n = 0;
    try { n = std::stoi(id->second); } catch (...) { return Error(400, "bad_request", "id must be an integer"); }
    out["ok"] = true;
    out["id"] = n;
    out["text"] = host_.stf(n);
    return Ok(out);
}

Response Api::LocaleStatusResp() {
    std::lock_guard<std::mutex> lk(fileMutex_);
    return Ok(LocaleStatus(opt_.contentDir));
}

// ---------------------------------------------------------------------------------------------------------------- kinds / list / get

namespace {

struct RecFile { std::string file; bool generated = false; ordered_json recs; };

}  // namespace

Response Api::KindsResp() {
    json out = json::object();
    out["ok"] = true;
    out["app"] = "f3_editor";
    out["api"] = 1;
    out["contentSchema"] = kContentSchema;
    out["hasGamedata"] = host_.hasGamedata;
    out["canReload"] = host_.reload != nullptr;
    out["dataDir"] = fs::absolute(opt_.contentDir).generic_string();
    json kinds = json::array();
    std::lock_guard<std::mutex> lk(fileMutex_);
    for (const KindInfo& ki : editor::Kinds()) {
        json k = json::object();
        k["id"] = ki.id;
        k["label"] = ki.label;
        k["group"] = ki.group;
        json files = json::array();
        size_t total = 0;
        for (const std::string& f : ListFiles(ki.id, true)) {
            const FileInfo fi = ReadFile(ki.id, f);
            json e = json::object();
            e["file"] = f;
            e["generated"] = fi.generated;
            e["draft"] = f[0] == '_';
            e["ok"] = fi.ok;
            if (!fi.ok) e["error"] = fi.error;
            size_t n = 0;
            if (fi.ok && fi.doc.is_object() && fi.doc.contains("records") && fi.doc["records"].is_array()) n = fi.doc["records"].size();
            e["records"] = n;
            if (f[0] != '_') total += n;
            files.push_back(e);
        }
        k["files"] = files;
        k["records"] = total;
        kinds.push_back(k);
    }
    out["kinds"] = kinds;
    json loc = json::object();
    std::error_code ec;
    json langs = json::array();
    if (fs::is_directory(opt_.contentDir / "locale", ec))
        for (const auto& e : fs::directory_iterator(opt_.contentDir / "locale", ec))
            if (e.is_directory() && ValidLang(e.path().filename().string())) {
                const std::string lang = e.path().filename().string();
                langs.push_back(lang);
                json files = json::array();
                for (const std::string& f : ListFiles("locale/" + lang, true)) files.push_back(f);
                loc[lang] = files;
            }
    out["locale"] = loc;
    std::vector<std::string> ls;
    for (const auto& l : langs) ls.push_back(l.get<std::string>());
    std::sort(ls.begin(), ls.end());
    out["langs"] = ls;
    return Ok(out);
}

Response Api::List(const Request& req) {
    auto kq = req.query.find("kind");
    if (kq == req.query.end() || !FindKind(kq->second)) return Error(400, "bad_kind", "kind must be one of the content kinds");
    const std::string kind = kq->second;
    const bool hasBase = kind == "items" || kind == "npcs";
    std::lock_guard<std::mutex> lk(fileMutex_);
    json rows = json::array(), fileErrors = json::array(), files = json::array();
    std::set<std::string> seen;
    for (const std::string& f : ListFiles(kind, false)) {
        const FileInfo fi = ReadFile(kind, f);
        files.push_back(json{{"file", f}, {"generated", fi.generated}, {"hasComments", fi.hasComments}});
        if (!fi.ok) { fileErrors.push_back(json{{"file", f}, {"error", fi.error}}); continue; }
        if (!fi.doc.is_object() || !fi.doc.contains("records") || !fi.doc["records"].is_array()) { fileErrors.push_back(json{{"file", f}, {"error", "no records array"}}); continue; }
        const ordered_json& recs = fi.doc["records"];
        for (size_t i = 0; i < recs.size(); ++i) {
            const ordered_json& r = recs[i];
            if (!r.is_object()) continue;
            const std::string id = GetStr(r, "id");
            json row = json::object();
            row["id"] = id;
            row["file"] = f;
            row["index"] = i;
            row["origin"] = fi.generated ? "generated" : "hand";
            std::string name = GetStr(r, kind == "quests" ? "title" : "name");
            if (kind == "dialogs") name = GetStr(r, "speaker");
            if (kind == "skills") name = GetStr(r, "desc").substr(0, 60);
            if (hasBase) {
                const BaseProto* b = nullptr;
                auto it = baseIndex_.find(Low(id));
                const bool isOverride = it != baseIndex_.end();
                if (isOverride) b = &host_.base[it->second];
                else { auto bi = baseIndex_.find(Low(GetStr(r, "base"))); if (bi != baseIndex_.end()) b = &host_.base[bi->second]; }
                if (name.empty() && b) name = b->name;
                row["cls"] = kind == "npcs" ? "npc" : ClsOfRecord(r, b);
                row["override"] = isOverride;
            }
            if (kind == "dialogs" && r.contains("nodes") && r["nodes"].is_array()) row["nodes"] = r["nodes"].size();
            row["name"] = name;
            seen.insert(Low(id));
            rows.push_back(row);
        }
    }
    if (hasBase) {   // the original prototypes that no data file touches: shown as "base" (read-only until overridden)
        for (const BaseProto& b : host_.base) {
            const bool isNpc = b.cls == "critter";
            if ((kind == "npcs") != isNpc) continue;
            if (kind == "items" && !(b.cls == "weapon" || b.cls == "armor" || b.cls == "ammo" || b.cls == "misc")) continue;
            if (kind == "items" && Low(b.key).rfind("ds_", 0) == 0) continue;
            if (seen.count(Low(b.key))) continue;
            json row = json::object();
            row["id"] = b.key;
            row["file"] = nullptr;
            row["name"] = b.name;
            row["origin"] = "base";
            row["cls"] = kind == "npcs" ? "npc" : ClsOfRecord(ordered_json::object(), &b);
            row["override"] = true;
            rows.push_back(row);
        }
    }
    json out = json::object();
    out["ok"] = true;
    out["kind"] = kind;
    out["rows"] = rows;
    out["files"] = files;
    out["fileErrors"] = fileErrors;
    out["hasBase"] = hasBase;
    return Ok(out);
}

Response Api::Get(const Request& req) {
    auto kq = req.query.find("kind");
    if (kq == req.query.end()) return Error(400, "bad_request", "kind required");
    const std::string kind = kq->second;
    if (!FindKind(kind) && !IsLocaleKind(kind)) return Error(400, "bad_kind", "unknown kind '" + kind + "'");
    std::lock_guard<std::mutex> lk(fileMutex_);
    auto fq = req.query.find("file");
    auto iq = req.query.find("id");
    if (fq != req.query.end()) {
        fs::path p;
        std::string why;
        if (!ResolveFile(kind, fq->second, p, why)) return Error(400, "bad_path", why);
        std::error_code ec;
        if (!fs::exists(p, ec)) return Error(404, "not_found", "no such file");
        const FileInfo fi = ReadFile(kind, fq->second);
        json out = json::object();
        out["ok"] = true;
        out["kind"] = kind;
        out["file"] = fq->second;
        out["parsed"] = fi.ok;
        out["generated"] = fi.generated;
        out["hasComments"] = fi.hasComments;
        out["etag"] = fi.etag;
        out["text"] = fi.text;
        if (fi.generated) out["generator"] = fi.doc["generator"];
        if (fi.ok) out["doc"] = fi.doc; else out["error"] = fi.error;
        return Ok(out);
    }
    if (iq == req.query.end() || IsLocaleKind(kind)) return Error(400, "bad_request", "file or id required");
    const std::string want = Low(iq->second);
    const bool hasBase = kind == "items" || kind == "npcs";
    auto baseJson = [&](const BaseProto* b, const char* mode) {
        json j = json::object();
        j["key"] = b->key;
        j["name"] = b->name;
        j["cls"] = b->cls;
        j["mode"] = mode;
        j["record"] = b->record;
        return j;
    };
    for (const std::string& f : ListFiles(kind, false)) {
        const FileInfo fi = ReadFile(kind, f);
        if (!fi.ok || !fi.doc.is_object() || !fi.doc.contains("records") || !fi.doc["records"].is_array()) continue;
        const ordered_json& recs = fi.doc["records"];
        for (size_t i = 0; i < recs.size(); ++i) {
            if (!recs[i].is_object() || Low(GetStr(recs[i], "id")) != want) continue;
            json out = json::object();
            out["ok"] = true;
            out["kind"] = kind;
            out["file"] = f;
            out["index"] = i;
            out["origin"] = fi.generated ? "generated" : "hand";
            out["generated"] = fi.generated;
            out["hasComments"] = fi.hasComments;
            out["etag"] = fi.etag;
            out["record"] = recs[i];
            out["base"] = nullptr;
            if (hasBase) {
                auto it = baseIndex_.find(want);
                if (it != baseIndex_.end()) out["base"] = baseJson(&host_.base[it->second], "override");
                else { auto bi = baseIndex_.find(Low(GetStr(recs[i], "base"))); if (bi != baseIndex_.end()) out["base"] = baseJson(&host_.base[bi->second], "clone"); }
            }
            return Ok(out);
        }
    }
    if (hasBase) {
        auto it = baseIndex_.find(want);
        if (it != baseIndex_.end()) {
            json out = json::object();
            out["ok"] = true;
            out["kind"] = kind;
            out["file"] = nullptr;
            out["origin"] = "base";
            out["baseOnly"] = true;
            out["record"] = nullptr;
            out["base"] = baseJson(&host_.base[it->second], "override");
            return Ok(out);
        }
    }
    return Error(404, "not_found", "no record '" + iq->second + "' in " + kind);
}

// ---------------------------------------------------------------------------------------------------------------- pickers

namespace {

void CollectFlags(const ordered_json& j, std::set<std::string>& out) {
    if (j.is_string()) {
        std::string s = j.get<std::string>();
        size_t b = 0;
        while (b < s.size() && (s[b] == '!' || s[b] == ' ')) ++b;
        if (s.compare(b, 5, "flag:") == 0) {
            std::string n = s.substr(b + 5);
            size_t e = n.find_first_of("<>=! ");
            if (e != std::string::npos) n.resize(e);
            if (!n.empty()) out.insert(n);
        }
    } else if (j.is_array()) {
        for (const auto& e : j) CollectFlags(e, out);
    } else if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (it.key() == "flag" && it.value().is_string()) out.insert(it.value().get<std::string>());
            else CollectFlags(it.value(), out);
        }
    }
}

}  // namespace

Response Api::Refs() {
    std::lock_guard<std::mutex> lk(fileMutex_);
    json items = json::array(), npcs = json::array();
    std::set<std::string> seenItem, seenNpc;
    std::map<std::string, std::set<std::string>> ids;   // kind -> ids
    std::set<std::string> maps, flags;
    auto addRef = [](json& arr, std::set<std::string>& seen, const std::string& id, const std::string& name, const std::string& cls) {
        if (id.empty() || !seen.insert(Low(id)).second) return;
        arr.push_back(json{{"id", id}, {"name", name}, {"cls", cls}});
    };
    for (const KindInfo& ki : editor::Kinds()) {
        const std::string kind = ki.id;
        for (const std::string& f : ListFiles(kind, false)) {
            const FileInfo fi = ReadFile(kind, f);
            if (!fi.ok || !fi.doc.is_object() || !fi.doc.contains("records") || !fi.doc["records"].is_array()) continue;
            for (const ordered_json& r : fi.doc["records"]) {
                if (!r.is_object()) continue;
                const std::string id = GetStr(r, "id");
                if (kind == "items") {
                    auto bi = baseIndex_.find(Low(id));
                    const BaseProto* b = bi != baseIndex_.end() ? &host_.base[bi->second] : nullptr;
                    if (!b) { auto bb = baseIndex_.find(Low(GetStr(r, "base"))); if (bb != baseIndex_.end()) b = &host_.base[bb->second]; }
                    addRef(items, seenItem, id, GetStr(r, "name").empty() && b ? b->name : GetStr(r, "name"), ClsOfRecord(r, b));
                } else if (kind == "npcs") {
                    auto bi = baseIndex_.find(Low(id));
                    addRef(npcs, seenNpc, id, GetStr(r, "name").empty() && bi != baseIndex_.end() ? host_.base[bi->second].name : GetStr(r, "name"), "npc");
                    if (r.contains("spawns") && r["spawns"].is_array()) for (const auto& s : r["spawns"]) if (s.is_object() && !GetStr(s, "map").empty()) maps.insert(GetStr(s, "map"));
                } else {
                    ids[kind].insert(id);
                    if (kind == "dialogs") CollectFlags(r, flags);
                }
            }
        }
    }
    for (const BaseProto& b : host_.base) {
        if (b.cls == "critter") addRef(npcs, seenNpc, b.key, b.name, "npc");
        else if (b.cls == "weapon" || b.cls == "armor" || b.cls == "ammo" || b.cls == "misc") { if (Low(b.key).rfind("ds_", 0) != 0) addRef(items, seenItem, b.key, b.name, b.cls == "misc" ? "kit" : b.cls); }
    }
    for (const std::string& m : host_.maps) maps.insert(m);
    json out = json::object();
    out["ok"] = true;
    out["items"] = items;
    out["npcs"] = npcs;
    for (const char* k : {"perks", "traits", "quests", "dialogs"}) {
        json a = json::array();
        for (const std::string& id : ids[k]) a.push_back(id);
        out[k] = a;
    }
    out["maps"] = maps;
    out["flags"] = flags;
    out["hasGamedata"] = host_.hasGamedata;
    return Ok(out);
}

// ---------------------------------------------------------------------------------------------------------------- save / delete

bool Api::WriteAtomic(const fs::path& p, const std::string& text, std::string& err) const {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = p.parent_path() / ("." + p.filename().string() + ".tmp" + std::to_string(CurrentPid()));
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { err = "cannot create " + tmp.filename().string(); return false; }
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        f.flush();
        if (!f) { err = "write failed"; f.close(); fs::remove(tmp, ec); return false; }
    }
    fs::rename(tmp, p, ec);   // replaces an existing file atomically (MoveFileEx / rename)
    if (ec) { err = "cannot replace the file: " + ec.message(); fs::remove(tmp, ec); return false; }
    return true;
}

Response Api::Put(const Request& req) {
    ordered_json b = ordered_json::parse(req.body, nullptr, false);
    if (b.is_discarded() || !b.is_object() || !b.contains("doc")) return Error(400, "bad_request", "body must be {kind, file, doc, ...}");
    json opts = json::object();
    for (const char* k : {"draft", "takeover", "ackGenerated", "create"}) if (b.contains(k) && b[k].is_boolean()) opts[k] = b[k].get<bool>();
    if (b.contains("etag") && b["etag"].is_string()) opts["etag"] = b["etag"].get<std::string>();
    return Save(GetStr(b, "kind"), GetStr(b, "file"), b["doc"], opts, false);
}

Response Api::Delete(const Request& req) {
    ordered_json b = ordered_json::parse(req.body.empty() ? "{}" : req.body, nullptr, false);
    if (b.is_discarded() || !b.is_object()) return Error(400, "bad_request", "body must be a JSON object");
    json opts = json::object();
    if (b.contains("etag") && b["etag"].is_string()) opts["etag"] = b["etag"].get<std::string>();
    if (b.contains("confirm") && b["confirm"].is_boolean()) opts["confirm"] = b["confirm"].get<bool>();
    if (b.contains("id") && b["id"].is_string()) opts["id"] = b["id"].get<std::string>();
    return Save(GetStr(b, "kind"), GetStr(b, "file"), ordered_json(), opts, true);
}

Response Api::Save(const std::string& kind, const std::string& file, ordered_json doc, const json& o, bool isDelete) {
    fs::path path;
    std::string why;
    if (!ResolveFile(kind, file, path, why)) return Error(400, "bad_path", why);
    const bool locale = IsLocaleKind(kind);
    const bool draft = !isDelete && o.value("draft", false);
    std::lock_guard<std::mutex> lk(fileMutex_);
    std::error_code ec;
    const bool exists = fs::exists(path, ec);
    FileInfo cur;
    if (exists) cur = ReadFile(kind, file);
    bool fileDelete = false;
    if (isDelete) {
        if (!exists) return Error(404, "not_found", "no such file");
        if (o.contains("id") && !locale) {   // remove one record, then save like any edit
            if (!cur.ok || !cur.doc.is_object() || !cur.doc.contains("records") || !cur.doc["records"].is_array()) return Error(422, "unreadable", "the file cannot be parsed, fix or replace it first");
            doc = cur.doc;
            const std::string want = Low(o["id"].get<std::string>());
            ordered_json keep = ordered_json::array();
            size_t removed = 0;
            for (const auto& r : doc["records"]) { if (r.is_object() && Low(GetStr(r, "id")) == want) ++removed; else keep.push_back(r); }
            if (!removed) return Error(404, "not_found", "no record '" + o["id"].get<std::string>() + "' in this file");
            doc["records"] = keep;
        } else {
            if (!o.value("confirm", false)) return Error(400, "confirm", "deleting a file needs \"confirm\": true");
            fileDelete = true;
        }
    }
    if (!fileDelete) {
        if (locale) {
            if (!doc.is_object()) return Error(422, "bad_doc", "a locale file is a flat JSON object of strings");
            for (auto it = doc.begin(); it != doc.end(); ++it) {
                if (!it.value().is_string()) return Error(422, "bad_doc", "locale value of '" + it.key() + "' is not a string");
                if (it.key().empty() || it.key().size() > 200) return Error(422, "bad_doc", "locale key '" + it.key() + "' is empty or too long");
            }
        } else if (!doc.is_object()) return Error(422, "bad_doc", "a data file is a JSON object {schema, kind, records}");
    }
    if (o.contains("etag") && exists && !draft && o["etag"].get<std::string>() != cur.etag)
        return Error(409, "conflict", "the file changed on disk since it was loaded (another editor, git, or the exporter); reload it before saving", json{{"etag", cur.etag}});
    bool tookOver = false;
    if (!fileDelete && !locale && !draft && exists && cur.generated && doc.contains("generator")) {
        if (o.value("takeover", false)) { doc.erase("generator"); tookOver = true; }
        else if (!o.value("ackGenerated", false))
            return Error(409, "generated", "this file is generated (\"generator\": " + cur.doc["generator"].dump() + ") and the next --export-data overwrites it; save with ackGenerated to keep the key, or take the file over (removes the key)", json{{"generator", cur.doc["generator"]}});
    }
    if (locale && !fileDelete && exists && cur.ok && cur.doc.is_object()) {   // keep a key-sorted locale file sorted (stable diffs)
        std::vector<std::string> keys;
        for (auto it = cur.doc.begin(); it != cur.doc.end(); ++it) keys.push_back(it.key());
        if (std::is_sorted(keys.begin(), keys.end())) {
            std::vector<std::pair<std::string, ordered_json>> items;
            for (auto it = doc.begin(); it != doc.end(); ++it) items.emplace_back(it.key(), it.value());
            std::sort(items.begin(), items.end(), [](const auto& a, const auto& c) { return a.first < c.first; });
            ordered_json sorted = ordered_json::object();
            for (auto& [k, v] : items) sorted[k] = std::move(v);
            doc = std::move(sorted);
        }
    }
    std::string text;
    if (!fileDelete) {
        try { text = EmitJsonText(doc); } catch (const std::exception& e) { return Error(422, "bad_doc", std::string("cannot serialise: ") + e.what()); }
    }
    json out = json::object();
    if (draft) {   // "draft": never replaces the live file; written as _draft_<name>, which the loader ignores (leading underscore)
        const std::string dn = file[0] == '_' ? file : "_draft_" + file;
        fs::path dp;
        if (!ResolveFile(kind, dn, dp, why)) return Error(400, "bad_path", why);
        std::string err;
        if (!WriteAtomic(dp, text, err)) return Error(500, "write_failed", err);
        out["ok"] = true; out["written"] = true; out["draft"] = true; out["file"] = dn; out["kind"] = kind; out["etag"] = Etag(text);
        return Ok(out);
    }
    json warnings = json::array(), otherErrors = json::array();
    if (!locale) {
        const ContentReport base = RunValidate(nullptr);
        Candidate cand{kind, file, fileDelete ? nullptr : &text, fileDelete};
        const ContentReport rep = RunValidate(&cand);
        const std::string own = "data/" + kind + "/" + file + ":";
        std::set<std::string> was(base.errors.begin(), base.errors.end());
        std::vector<std::string> blocking, other;
        for (const std::string& e : rep.errors) {
            if (StartsWith(e, own) || !was.count(e)) blocking.push_back(e);   // errors in this file, or ones this change introduces anywhere
            else other.push_back(e);                                          // already broken elsewhere before this change: not ours to block on
        }
        if (!blocking.empty() && !(fileDelete && blocking.empty()))
            return Error(422, "invalid", std::to_string(blocking.size()) + " validation error(s); nothing was written (save as draft to keep the work)", json{{"errors", Messages(blocking)}, {"warnings", Messages(rep.warnings)}});
        warnings = Messages(rep.warnings);
        otherErrors = Messages(other);
    }
    std::string err;
    if (fileDelete) {   // moved aside, never destroyed: _deleted_<name> is ignored by the loader
        fs::path dst;
        for (int n = 0;; ++n) {
            const std::string name = "_deleted_" + (n ? std::to_string(n) + "_" : std::string()) + file;
            if (!ResolveFile(kind, name, dst, why)) return Error(400, "bad_path", why);
            if (!fs::exists(dst, ec)) break;
            if (n > 99) return Error(500, "write_failed", "too many deleted copies");
        }
        fs::rename(path, dst, ec);
        if (ec) return Error(500, "write_failed", "cannot move the file aside: " + ec.message());
        out["ok"] = true; out["deleted"] = true; out["movedTo"] = dst.filename().string(); out["warnings"] = warnings; out["otherErrors"] = otherErrors;
        return Ok(out);
    }
    if (!WriteAtomic(path, text, err)) return Error(500, "write_failed", err);
    out["ok"] = true;
    out["written"] = true;
    out["kind"] = kind;
    out["file"] = file;
    out["etag"] = Etag(text);
    out["text"] = text;
    out["tookOver"] = tookOver;
    out["warnings"] = warnings;
    out["otherErrors"] = otherErrors;
    out["hadCommentsLost"] = exists && cur.hasComments;
    return Ok(out);
}

}  // namespace f3::editor
