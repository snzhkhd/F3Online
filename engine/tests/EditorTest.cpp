// Unit-style tests of the content editor core (docs/editor.md): request handling, token / Host / loopback checks, path confinement,
// listing, atomic saves with stable formatting, draft / generated-file / conflict handling, delete safety, locale files, and the check that
// the served schema and the real validator agree (boundary values of every field). No server, no sockets, no original game files.
//   f3_editortest          exit code 0 = all passed
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <memory>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include "DataFormat.h"
#include "EditorCore.h"

using namespace f3;
using namespace f3::editor;
namespace fs = std::filesystem;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond, ...) do { ++g_checks; if (!(cond)) { ++g_failed; std::printf("  FAIL %s:%d  ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static void Put(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << text;
}
static std::string Slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    return s;
}

static const int kPort = 47123;
static const char* kToken = "0123456789abcdef0123456789abcdef";

static Request Req(const std::string& method, const std::string& path, std::map<std::string, std::string> query = {}, const std::string& body = "", bool token = true) {
    Request r;
    r.method = method;
    r.path = path;
    r.query = std::move(query);
    r.body = body;
    r.remoteAddr = "127.0.0.1";
    r.headers["host"] = "127.0.0.1:" + std::to_string(kPort);
    if (token) r.headers["x-f3-token"] = kToken;
    if (method != "GET") r.headers["content-type"] = "application/json";
    return r;
}
static json J(const Response& r) { return json::parse(r.body, nullptr, false); }

struct Fixture {
    fs::path root, data, ui;
    std::unique_ptr<Api> api;
    Host host;
    explicit Fixture(const std::string& name, bool withHost = false) {
        root = fs::temp_directory_path() / ("f3_editortest_" + name + "_" + std::to_string(std::rand()));
        fs::remove_all(root);
        data = root / "data";
        ui = root / "ui";
        Put(data / "manifest.json", R"({"schema":1,"name":"t"})");
        Put(data / "perks/perks.json", "{\n  \"schema\": 1,\n  \"kind\": \"perks\",\n  \"records\": [\n    {\n      \"id\": \"toughness\",\n      \"name\": \"Toughness\",\n      \"ranks\": 2\n    }\n  ]\n}\n");
        Put(data / "quests/quests.json", "{\"schema\":1,\"kind\":\"quests\",\"records\":[{\"id\":\"q1\",\"title\":\"First\"}]}");
        Put(data / "dialogs/d.json", R"({"schema":1,"kind":"dialogs","records":[{"id":"d1","nodes":[{"id":"a","text":"Hi","answers":[{"id":"x","text":"Quest","do":[{"quest":"q1","state":"add"}]}]}]}]})");
        Put(data / "items/gen.json", "{\n  \"schema\": 1,\n  \"kind\": \"items\",\n  \"generator\": \"f3_server --export-data\",\n  \"note\": \"n\",\n  \"records\": [\n    { \"id\": \"Pistol.WEA\", \"name\": \"Pistol\" }\n  ]\n}\n");
        Put(data / "locale/en/ui.json", "{\n  \"ui.a\": \"A {n}\",\n  \"ui.c\": \"C\"\n}\n");
        Put(data / "locale/ru/ui.json", "{\n  \"ui.a\": \"\xD0\x90 {n}\"\n}\n");
        Put(ui / "index.html", "<html>ui</html>");
        Put(ui / "app.js", "var x=1;");
        Put(root / "secret.txt", "secret");
        if (withHost) {
            BaseProto b;
            b.key = "Pistol.WEA"; b.name = "Base Pistol"; b.cls = "weapon";
            b.record = ordered_json::parse(R"({"id":"Pistol.WEA","name":"Base Pistol","weapon":{"range":10}})");
            host.base.push_back(b);
            BaseProto n;
            n.key = "Thug.CRT"; n.name = "Thug"; n.cls = "critter";
            n.record = ordered_json::parse(R"({"id":"Thug.CRT","name":"Thug","team":2})");
            host.base.push_back(n);
            host.hasGamedata = true;
            host.maps = {"town"};
            host.stf = [](int id) { return id == 7 ? std::string("Original line") : std::string(); };
            host.reload = [](const std::string&) { return std::vector<std::string>{"data reloaded: 1 perks", "  perks: +0 -0 ~1 (changed: toughness)"}; };
        }
        Options o;
        o.contentDir = data;
        o.uiDir = ui;
        o.token = kToken;
        o.port = kPort;
        api = std::make_unique<Api>(o, host);
    }
    ~Fixture() { std::error_code ec; fs::remove_all(root, ec); }
};

static std::string PerksDoc(const std::string& recs) { return "{\"schema\":1,\"kind\":\"perks\",\"records\":" + recs + "}"; }
static std::string PutBody(const std::string& kind, const std::string& file, const std::string& doc, const std::string& extra = "") {
    return "{\"kind\":\"" + kind + "\",\"file\":\"" + file + "\",\"doc\":" + doc + extra + "}";
}

// ------------------------------------------------------------------------------------------------------------------------------------ security
static void TestSecurity() {
    std::printf("security primitives\n");
    for (const char* a : {"127.0.0.1", "127.1.2.3", "::1", "::ffff:127.0.0.1"}) CHECK(Api::IsLoopbackAddr(a), "%s is loopback", a);
    for (const char* a : {"", "0.0.0.0", "192.168.1.5", "10.0.0.1", "128.0.0.1", "::", "fe80::1", "127.0.0", "127.0.0.1.5x", "localhost", "evil.com"}) CHECK(!Api::IsLoopbackAddr(a), "'%s' is not loopback", a);
    CHECK(Api::TokenEquals("abc", "abc") && !Api::TokenEquals("abc", "abd") && !Api::TokenEquals("abc", "abcd") && !Api::TokenEquals("", "x"), "token compare");
    Fixture f("sec");
    CHECK(f.api->HostHeaderOk("127.0.0.1:47123") && f.api->HostHeaderOk("localhost:47123") && f.api->HostHeaderOk("[::1]:47123") && f.api->HostHeaderOk("LOCALHOST:47123"), "own host names accepted");
    CHECK(!f.api->HostHeaderOk("evil.example:47123") && !f.api->HostHeaderOk("127.0.0.1:80") && !f.api->HostHeaderOk("127.0.0.1") && !f.api->HostHeaderOk("localhost.evil.com:47123") && !f.api->HostHeaderOk(""), "foreign Host headers refused (DNS rebinding)");

    for (const char* n : {"perks.json", "a-b_c.1.json", "_draft_x.json", "X.JSON"}) CHECK(Api::ValidFileName(n), "valid name %s", n);
    for (const char* n : {"", ".json", "x.txt", "../x.json", "a/b.json", "a\\b.json", ".hidden.json", "x..json", "x.json.exe", "c:x.json", "x.json:ads", "con.json", "NUL.json", "com1.json", "lpt9.json", "a b.json", "x\n.json", "x.json "})
        CHECK(!Api::ValidFileName(n), "invalid name '%s'", n);
    CHECK(!Api::ValidFileName(std::string(100, 'a') + ".json"), "overlong name refused");

    fs::path p;
    std::string why;
    CHECK(f.api->ResolveFile("perks", "perks.json", p, why) && p.filename() == "perks.json", "plain resolve");
    CHECK(f.api->ResolveFile("locale/ru", "ui.json", p, why), "locale resolve");
    CHECK(f.api->ResolveFile("locale/de", "new.json", p, why), "a new language folder may be created");
    for (const char* k : {"../items", "..", "", "manifest", "locale", "locale/", "locale/..", "locale/EN", "locale/e", "locale/en/x", "locale/../perks", "items/../..", "C:/Windows", "/etc", "perks/", "gamedata", "saves"})
        CHECK(!f.api->ResolveFile(k, "x.json", p, why), "kind '%s' refused", k);
    for (const char* n : {"../../secret.json", "..\\x.json", "/abs.json", "C:\\x.json", "perks/../x.json", "%2e%2e/x.json"})
        CHECK(!f.api->ResolveFile("perks", n, p, why), "file '%s' refused", n);
    CHECK(!f.api->ResolveFile("manifest", "manifest.json", p, why), "manifest.json is not an editable kind");
}

static void TestRequestGuards() {
    std::printf("request guards\n");
    Fixture f("guards", true);
    CHECK(f.api->Handle(Req("GET", "/api/kinds")).status == 200, "valid token");
    CHECK(f.api->Handle(Req("GET", "/api/kinds", {}, "", false)).status == 401, "no token -> 401");
    Request r = Req("GET", "/api/kinds");
    r.headers["x-f3-token"] = "wrong";
    CHECK(f.api->Handle(r).status == 401, "wrong token -> 401");
    r.headers["x-f3-token"] = std::string(kToken).substr(0, 31);
    CHECK(f.api->Handle(r).status == 401, "prefix of the token -> 401");
    r = Req("GET", "/api/kinds", {{"token", kToken}}, "", false);   // the token in the query string is NOT accepted for the API
    CHECK(f.api->Handle(r).status == 401, "query token is not an API credential");
    r = Req("GET", "/api/kinds");
    r.remoteAddr = "192.168.1.20";
    CHECK(f.api->Handle(r).status == 403, "non-loopback peer -> 403");
    r.remoteAddr = "";
    CHECK(f.api->Handle(r).status == 403, "unknown peer -> 403");
    r = Req("GET", "/api/kinds");
    r.headers["host"] = "evil.example";
    CHECK(f.api->Handle(r).status == 403, "foreign Host -> 403");
    r.headers.erase("host");
    CHECK(f.api->Handle(r).status == 403, "no Host -> 403");
    r = Req("PUT", "/api/put", {}, PutBody("perks", "perks.json", PerksDoc("[]")));
    r.headers["content-type"] = "text/plain";
    CHECK(f.api->Handle(r).status == 415, "form-style content type refused");
    r.headers.erase("content-type");
    CHECK(f.api->Handle(r).status == 415, "no content type refused");
    CHECK(f.api->Handle(Req("GET", "/api/put")).status == 405, "wrong method");
    CHECK(f.api->Handle(Req("GET", "/api/nothing")).status == 404, "unknown endpoint");
    // pages
    CHECK(f.api->Handle(Req("GET", "/", {}, "", false)).status == 401, "index without token -> 401");
    CHECK(f.api->Handle(Req("GET", "/", {{"token", "nope"}}, "", false)).status == 401, "index with wrong token -> 401");
    Response idx = f.api->Handle(Req("GET", "/", {{"token", kToken}}, "", false));
    CHECK(idx.status == 200 && idx.body.find("ui") != std::string::npos && idx.contentType.find("text/html") == 0, "index with the token");
    CHECK(f.api->Handle(Req("GET", "/app.js", {}, "", false)).status == 200, "static assets need no token (plain repo files)");
    for (const char* p : {"/../secret.txt", "/..%2fsecret.txt", "/.hidden", "/a/b.js", "/..\\secret.txt", "/secret.txt", "/nofile.js", "/app.js/"})
        CHECK(f.api->Handle(Req("GET", p, {}, "", false)).status == 404, "static path %s -> 404", p);
}

// ------------------------------------------------------------------------------------------------------------------------------------ api
static void TestReadEndpoints() {
    std::printf("kinds / list / get / refs / stf\n");
    Fixture f("read", true);
    json k = J(f.api->Handle(Req("GET", "/api/kinds")));
    CHECK(k["ok"] == true && k["hasGamedata"] == true && k["kinds"].size() == 8, "kinds");
    json langs = k["langs"];
    CHECK(langs.size() == 2 && langs[0] == "en" && langs[1] == "ru", "locale languages");
    bool genFlag = false;
    for (auto& kk : k["kinds"]) if (kk["id"] == "items") for (auto& fl : kk["files"]) if (fl["file"] == "gen.json" && fl["generated"] == true) genFlag = true;
    CHECK(genFlag, "generated file flagged in kinds");

    json l = J(f.api->Handle(Req("GET", "/api/list", {{"kind", "items"}})));
    CHECK(l["ok"] == true && l["hasBase"] == true, "list items");
    bool gen = false, base = false;
    for (auto& r : l["rows"]) {
        if (r["id"] == "Pistol.WEA" && r["origin"] == "generated" && r["cls"] == "weapon" && r["override"] == true) gen = true;
    }
    CHECK(gen, "generated row with class from the base prototype");
    l = J(f.api->Handle(Req("GET", "/api/list", {{"kind", "npcs"}})));
    for (auto& r : l["rows"]) if (r["id"] == "Thug.CRT" && r["origin"] == "base" && r["file"].is_null()) base = true;
    CHECK(base, "original NPC listed as base");
    l = J(f.api->Handle(Req("GET", "/api/list", {{"kind", "perks"}})));
    CHECK(l["rows"].size() == 1 && l["rows"][0]["origin"] == "hand" && l["rows"][0]["name"] == "Toughness", "hand authored perk row");
    CHECK(f.api->Handle(Req("GET", "/api/list", {{"kind", "locale/en"}})).status == 400, "list needs a content kind");

    json g = J(f.api->Handle(Req("GET", "/api/get", {{"kind", "items"}, {"id", "pistol.wea"}})));
    CHECK(g["ok"] == true && g["origin"] == "generated" && g["base"]["mode"] == "override" && g["base"]["record"]["weapon"]["range"] == 10, "get by id (case-insensitive) with the base record");
    g = J(f.api->Handle(Req("GET", "/api/get", {{"kind", "npcs"}, {"id", "Thug.CRT"}})));
    CHECK(g["ok"] == true && g["baseOnly"] == true && g["record"].is_null(), "base-only record");
    CHECK(f.api->Handle(Req("GET", "/api/get", {{"kind", "perks"}, {"id", "nope"}})).status == 404, "unknown record");
    g = J(f.api->Handle(Req("GET", "/api/get", {{"kind", "perks"}, {"file", "perks.json"}})));
    CHECK(g["ok"] == true && g["parsed"] == true && g["doc"]["records"][0]["id"] == "toughness" && !g["etag"].get<std::string>().empty(), "get by file");
    CHECK(f.api->Handle(Req("GET", "/api/get", {{"kind", "perks"}, {"file", "../manifest.json"}})).status == 400, "get: traversal refused");
    CHECK(f.api->Handle(Req("GET", "/api/get", {{"kind", "perks"}, {"file", "missing.json"}})).status == 404, "get: missing file");
    json refs = J(f.api->Handle(Req("GET", "/api/refs")));
    bool hasQuest = false, hasThug = false;
    for (auto& q : refs["quests"]) if (q == "q1") hasQuest = true;
    for (auto& n : refs["npcs"]) if (n["id"] == "Thug.CRT") hasThug = true;
    CHECK(hasQuest && hasThug && refs["perks"][0] == "toughness", "reference pickers");
    json stf = J(f.api->Handle(Req("GET", "/api/stf", {{"id", "7"}})));
    CHECK(stf["available"] == true && stf["text"] == "Original line", "stf preview");
    CHECK(J(f.api->Handle(Req("GET", "/api/stf", {{"id", "x"}}))).value("ok", true) == false, "stf bad id");
    json sch = J(f.api->Handle(Req("GET", "/api/schema")));
    CHECK(sch["kinds"].contains("dialogs") && sch["enums"]["skills"].size() == 14 && sch["kinds"].contains("objects") && sch["steps"]["kinds"].size() == 15, "schema served");
}

static void TestSaveFlow() {
    std::printf("save / validate / draft / generated / conflict\n");
    Fixture f("save", true);
    const fs::path perks = f.data / "perks/perks.json";
    const std::string before = Slurp(perks);
    json g = J(f.api->Handle(Req("GET", "/api/get", {{"kind", "perks"}, {"file", "perks.json"}})));
    const std::string etag = g["etag"];
    // 1. valid save: stable formatting, atomic, no stray temp file
    json doc = g["doc"];
    doc["records"][0]["ranks"] = 3;
    doc["records"].push_back(json{{"id", "tough2"}, {"name", "Tough 2"}, {"requires", {{"level", 4}, {"perks", json::array({"toughness"})}}}});
    json pr = J(f.api->Handle(Req("PUT", "/api/put", {}, PutBody("perks", "perks.json", doc.dump(), ",\"etag\":\"" + etag + "\""))));
    CHECK(pr["ok"] == true && pr["written"] == true, "valid save: %s", pr.dump().c_str());
    const std::string after = Slurp(perks);
    CHECK(after.find("\"ranks\": 3") != std::string::npos && after.find("tough2") != std::string::npos, "file written");
    CHECK(after == pr["text"].get<std::string>(), "response text is the file text");
    // 1b. byte-identical to the shared emitter (the exporter's)
    CHECK(after == EmitJsonText(ordered_json::parse(after)), "saved text is the canonical form");
    int tmpLeft = 0;
    for (auto& e : fs::directory_iterator(f.data / "perks")) if (e.path().filename().string().find(".tmp") != std::string::npos) ++tmpLeft;
    CHECK(tmpLeft == 0, "no temp file left behind");
    // 2. stale etag -> 409, nothing written
    json c = J(f.api->Handle(Req("PUT", "/api/put", {}, PutBody("perks", "perks.json", doc.dump(), ",\"etag\":\"" + etag + "\""))));
    CHECK(c["ok"] == false && c["code"] == "conflict", "stale etag -> conflict");
    // 3. invalid: not written, error points at file + path
    const std::string cur = Slurp(perks);
    json bad = doc;
    bad["records"][0]["ranks"] = 99;
    Response br = f.api->Handle(Req("PUT", "/api/put", {}, PutBody("perks", "perks.json", bad.dump())));
    json bj = J(br);
    CHECK(br.status == 422 && bj["code"] == "invalid" && bj["errors"][0]["file"] == "data/perks/perks.json" && bj["errors"][0]["path"] == "records[0].ranks", "invalid save refused with file / path: %s", br.body.c_str());
    CHECK(Slurp(perks) == cur, "an invalid save writes nothing");
    // validate endpoint on a draft document
    json vj = J(f.api->Handle(Req("POST", "/api/validate", {}, "{\"kind\":\"perks\",\"file\":\"perks.json\",\"doc\":" + bad.dump() + "}")));
    CHECK(vj["ok"] == false && vj["errors"][0]["path"] == "records[0].ranks" && vj["own"] == "data/perks/perks.json:", "validate a draft doc");
    CHECK(J(f.api->Handle(Req("POST", "/api/validate", {}, ""))).value("ok", false), "validate the folder on disk");
    // 4. draft: never touches the live file; the loader ignores the _draft_ copy
    json dj = J(f.api->Handle(Req("PUT", "/api/put", {}, PutBody("perks", "perks.json", bad.dump(), ",\"draft\":true"))));
    CHECK(dj["ok"] == true && dj["draft"] == true && dj["file"] == "_draft_perks.json" && fs::exists(f.data / "perks/_draft_perks.json") && Slurp(perks) == cur, "draft saved aside");
    ContentDb db;
    ContentReport rep;
    ContentDb::Build(f.data, nullptr, false, db, rep);
    CHECK(rep.Ok(), "a draft file does not break the data folder");
    // 5. a change that breaks another file is refused (delete the quest the dialog uses)
    json dq = J(f.api->Handle(Req("DELETE", "/api/delete", {}, "{\"kind\":\"quests\",\"file\":\"quests.json\",\"id\":\"q1\"}")));
    CHECK(dq["ok"] == false && dq["code"] == "invalid" && dq["errors"].size() >= 1, "deleting a referenced quest is refused: %s", dq.dump().c_str());
    CHECK(Slurp(f.data / "quests/quests.json").find("q1") != std::string::npos, "quest still there");
    // 6. record delete that is fine
    json dp = J(f.api->Handle(Req("DELETE", "/api/delete", {}, "{\"kind\":\"perks\",\"file\":\"perks.json\",\"id\":\"TOUGH2\"}")));
    CHECK(dp["ok"] == true && Slurp(perks).find("tough2") == std::string::npos, "record delete (case-insensitive id)");
    // 7. generated file: warns, take over removes the key
    json gen = J(f.api->Handle(Req("GET", "/api/get", {{"kind", "items"}, {"file", "gen.json"}})));
    CHECK(gen["generated"] == true && gen["generator"] == "f3_server --export-data", "generated flag on get");
    json gdoc = gen["doc"];
    gdoc["records"][0]["name"] = "Edited";
    json g1 = J(f.api->Handle(Req("PUT", "/api/put", {}, PutBody("items", "gen.json", gdoc.dump()))));
    CHECK(g1["ok"] == false && g1["code"] == "generated", "saving a generated file needs acknowledgement");
    CHECK(Slurp(f.data / "items/gen.json").find("Edited") == std::string::npos, "not written without acknowledgement");
    json g2 = J(f.api->Handle(Req("PUT", "/api/put", {}, PutBody("items", "gen.json", gdoc.dump(), ",\"takeover\":true"))));
    CHECK(g2["ok"] == true && g2["tookOver"] == true && Slurp(f.data / "items/gen.json").find("generator") == std::string::npos && Slurp(f.data / "items/gen.json").find("Edited") != std::string::npos, "take over: generator key removed");
    // 8. delete a file: confirm needed, moved aside not destroyed
    CHECK(f.api->Handle(Req("DELETE", "/api/delete", {}, "{\"kind\":\"items\",\"file\":\"gen.json\"}")).status == 400, "file delete needs confirm");
    json df = J(f.api->Handle(Req("DELETE", "/api/delete", {}, "{\"kind\":\"items\",\"file\":\"gen.json\",\"confirm\":true}")));
    CHECK(df["ok"] == true && df["movedTo"] == "_deleted_gen.json" && !fs::exists(f.data / "items/gen.json") && fs::exists(f.data / "items/_deleted_gen.json"), "file moved aside");
    // 9. new file
    json nf = J(f.api->Handle(Req("PUT", "/api/put", {}, PutBody("quests", "more.json", "{\"schema\":1,\"kind\":\"quests\",\"records\":[{\"id\":\"q2\",\"title\":\"Two\"}]}"))));
    CHECK(nf["ok"] == true && fs::exists(f.data / "quests/more.json"), "new file created");
    // 10. wrong kind in the envelope, duplicate ids across files
    json wk = J(f.api->Handle(Req("PUT", "/api/put", {}, PutBody("quests", "wrong.json", "{\"schema\":1,\"kind\":\"perks\",\"records\":[]}"))));
    CHECK(wk["ok"] == false && !fs::exists(f.data / "quests/wrong.json"), "envelope kind mismatch refused");
    json dup = J(f.api->Handle(Req("PUT", "/api/put", {}, PutBody("quests", "dup.json", "{\"schema\":1,\"kind\":\"quests\",\"records\":[{\"id\":\"q2\",\"title\":\"Dup\"}]}"))));
    CHECK(dup["ok"] == false && !fs::exists(f.data / "quests/dup.json"), "duplicate id across files refused");
    // 11. confinement: attempts to write elsewhere
    for (auto kf : std::vector<std::pair<const char*, const char*>>{{"perks", "../escape.json"}, {"..", "x.json"}, {"perks", "..\\escape.json"}, {"manifest", "manifest.json"}, {"locale/..", "x.json"}, {"perks", "con.json"}, {"perks", "x.txt"}}) {
        Response r = f.api->Handle(Req("PUT", "/api/put", {}, PutBody(kf.first, kf.second, PerksDoc("[]"))));
        CHECK(r.status == 400, "write to %s/%s refused (status %d)", kf.first, kf.second, r.status);
    }
    CHECK(!fs::exists(f.root / "escape.json") && !fs::exists(f.data / "escape.json") && !fs::exists(f.data / "x.json"), "nothing written outside the kind folders");
    CHECK(Slurp(f.data / "manifest.json").find("\"name\":\"t\"") != std::string::npos, "manifest untouched");
    // 12. format endpoint = shared emitter
    json fm = J(f.api->Handle(Req("POST", "/api/format", {}, "{\"doc\":{\"a\":[1,2],\"b\":{\"c\":1.0}}}")));
    CHECK(fm["text"] == "{\n  \"a\": [1, 2],\n  \"b\": {\n    \"c\": 1\n  }\n}\n", "format endpoint: %s", fm.dump().c_str());
    // 13. reload through the host, and without one
    json rl = J(f.api->Handle(Req("POST", "/api/reload", {}, "{}")));
    CHECK(rl["ok"] == true && rl["lines"].size() == 2 && rl["report"].get<std::string>().find("changed: toughness") != std::string::npos, "reload report");
    Fixture f2("noserver");
    CHECK(f2.api->Handle(Req("POST", "/api/reload", {}, "{}")).status == 501, "reload without a server");
}

static void TestLocale() {
    std::printf("locale files and status\n");
    Fixture f("loc");
    json st = J(f.api->Handle(Req("GET", "/api/locale-status")));
    CHECK(st["ok"] == true && st["langs"].size() == 2, "locale status");
    bool missing = false, perkKey = false;
    for (auto& e : st["entries"]) {
        if (e["key"] == "ui.c" && e["tr"]["ru"]["status"] == "missing") missing = true;
        if (e["key"] == "perk.toughness.name" && e["group"] == "data" && e["dataText"] == "Toughness" && e["tr"]["ru"]["status"] == "missing") perkKey = true;
    }
    CHECK(missing, "ui.c is missing in ru");
    CHECK(perkKey, "data key perk.toughness.name is missing in ru, English taken from data/perks");
    CHECK(st["summary"]["ru"]["missing"].get<int>() >= 2 && !st["report"].get<std::string>().empty(), "summary and the --check-locale report");
    // save: keeps the sorted order, rejects non-strings and bad keys
    json g = J(f.api->Handle(Req("GET", "/api/get", {{"kind", "locale/ru"}, {"file", "ui.json"}})));
    json d = g["doc"];
    d["ui.c"] = "\xD0\xA6";
    d["ui.b"] = "\xD0\x91";
    json pr = J(f.api->Handle(Req("PUT", "/api/put", {}, PutBody("locale/ru", "ui.json", d.dump(-1, ' ', false)))));
    CHECK(pr["ok"] == true, "locale save");
    const std::string t = Slurp(f.data / "locale/ru/ui.json");
    CHECK(t.find("ui.a") < t.find("ui.b") && t.find("ui.b") < t.find("ui.c"), "key-sorted file stays sorted");
    CHECK(t.find("\xD0\xA6") != std::string::npos, "UTF-8 kept as is");
    CHECK(f.api->Handle(Req("PUT", "/api/put", {}, PutBody("locale/ru", "ui.json", "{\"k\":5}"))).status == 422, "non-string value refused");
    CHECK(f.api->Handle(Req("PUT", "/api/put", {}, PutBody("locale/ru", "ui.json", "[1]"))).status == 422, "non-object refused");
    CHECK(f.api->Handle(Req("PUT", "/api/put", {}, PutBody("locale/ru", "../ui.json", "{}"))).status == 400, "locale traversal refused");
    json nl = J(f.api->Handle(Req("PUT", "/api/put", {}, PutBody("locale/de", "ui.json", "{\"ui.a\":\"A\"}"))));
    CHECK(nl["ok"] == true && fs::exists(f.data / "locale/de/ui.json"), "a new language file");
}

// ------------------------------------------------------------------------------------------------------------------------------------ emitter
static void TestEmitterOnRepoData(const char* argv0) {
    std::printf("canonical form of the shipped data files\n");
    const fs::path content = FindContentDir(argv0);
    if (content.empty()) { std::printf("  (no data/ folder found, skipped)\n"); return; }
    int checked = 0;
    for (const char* kind : {"items", "npcs", "skills"}) {
        for (auto& e : fs::directory_iterator(content / kind)) {
            if (e.path().extension() != ".json") continue;
            const std::string text = Slurp(e.path());
            if (text.find("\"generator\"") == std::string::npos) continue;   // generated by the exporter: must be byte-identical to the shared emitter
            ordered_json j = ordered_json::parse(text);
            CHECK(EmitJsonText(j) == text, "%s is not in canonical form (the editor would rewrite it)", e.path().filename().string().c_str());
            ++checked;
        }
    }
    std::printf("  %d generated file(s) round-trip byte for byte\n", checked);
}

// ------------------------------------------------------------------------------------------------------------------------------------ schema vs. validator
static bool HasErr(const ContentReport& r, const std::string& needle) {
    for (const std::string& e : r.errors) if (e.find(needle) != std::string::npos) return true;
    return false;
}
static std::string Dump(const ContentReport& r) { std::string s; for (auto& e : r.errors) s += "\n      " + e; return s; }

struct Probe {
    fs::path root;
    Probe() {
        root = fs::temp_directory_path() / ("f3_editortest_schema_" + std::to_string(std::rand()));
        fs::remove_all(root);
        Put(root / "manifest.json", R"({"schema":1})");
        Put(root / "quests/q.json", R"({"schema":1,"kind":"quests","records":[{"id":"q1","title":"One"}]})");
    }
    ~Probe() { std::error_code ec; fs::remove_all(root, ec); }
    ContentReport Run(const std::string& kind, const json& record, const json& envelopeExtra = json::object()) {
        for (const char* d : {"items", "npcs", "perks", "traits", "skills", "quests", "dialogs"}) fs::remove_all(root / d);
        if (kind != "quests") Put(root / "quests/q.json", R"({"schema":1,"kind":"quests","records":[{"id":"q1","title":"One"}]})");   // dialogs refer to it
        json doc = json::object();
        doc["schema"] = 1;
        doc["kind"] = kind;
        for (auto it = envelopeExtra.begin(); it != envelopeExtra.end(); ++it) doc[it.key()] = it.value();
        doc["records"] = json::array({record});
        Put(root / kind / "t.json", doc.dump());
        ContentDb db;
        ContentReport rep;
        ContentDb::Build(root, nullptr, false, db, rep);
        return rep;
    }
};

static json SampleValue(const json& fld, bool forBoundary = false);

static json SampleObject(const json& fields) {
    json o = json::object();
    bool hasText = false, hasStf = false;
    for (const json& f : fields) {
        if (f["key"] == "text") hasText = true;
        if (f["key"] == "stf") hasStf = true;
    }
    for (const json& f : fields) {
        const std::string k = f["key"];
        if (f.value("required", false)) o[k] = SampleValue(f);
    }
    (void)hasStf;
    if (hasText && !o.contains("text")) o["text"] = "x";
    return o;
}

static json SampleValue(const json& f, bool) {
    const std::string t = f["type"];
    if (t == "string") { if (f.contains("enum")) return f["enum"][0]; return f.contains("ref") && f["ref"] == "quest" ? "q1" : "abc"; }
    if (t == "enum") return f["enum"][0];
    if (t == "text") return "x";
    if (t == "int" || t == "intenum") return f.contains("min") ? f["min"] : json(1);
    if (t == "number") return f.contains("min") ? f["min"] : json(1);
    if (t == "bool") return true;
    if (t == "object") return SampleObject(f["fields"]);
    if (t == "cell") return json::array({0, 0});
    if (t == "intmap") return json::object();
    if (t == "stringlist") return json::array();
    if (t == "list") {
        json a = json::array();
        if (f.contains("fixedLen")) for (int i = 0; i < f["fixedLen"].get<int>(); ++i) a.push_back(SampleValue(f["item"]));
        else if (f.value("minCount", 0) > 0) { json e = SampleValue(f["item"]); if (e.is_object() && e.contains("id")) e["id"] = "n0"; a.push_back(e); }
        return a;
    }
    return json();
}

// Walks every field of a kind, sets it to its boundary values in an otherwise valid record and asks the real validator.
static void ProbeFields(Probe& pr, const std::string& kind, const json& rootFields, const json& envExtra, json base) {
    // path: a list of steps (object key or list index) from the record down to the field
    struct Step { std::string key; int index = -1; };
    int probes = 0;
    std::function<void(const json&, std::vector<Step>)> walk;
    auto setAt = [](json& rec, const std::vector<Step>& path, const json& v) {
        json* cur = &rec;
        for (const Step& s : path) cur = s.index >= 0 ? &(*cur)[s.index] : &(*cur)[s.key];
        *cur = v;
    };
    auto pathStr = [](const std::vector<Step>& path) {
        std::string p = "records[0]";
        for (const Step& s : path) p += s.index >= 0 ? "[" + std::to_string(s.index) + "]" : "." + s.key;
        return p;
    };
    // `fieldsHere` describe the object located at `where` (already present in `base`)
    std::function<void(const json&, const std::vector<Step>&)> visit = [&](const json& fields, const std::vector<Step>& where) {
        for (const json& f : fields) {
            const std::string key = f["key"], type = f["type"];
            std::vector<Step> path = where;
            path.push_back({key, -1});
            const std::string ps = pathStr(path);
            auto probe = [&](const json& v, bool expectOk, const char* what) {
                json rec = base;
                // make sure the parents exist
                json* cur = &rec;
                for (const Step& s : where) cur = s.index >= 0 ? &(*cur)[s.index] : &(*cur)[s.key];
                (*cur)[key] = v;
                if (key == "dmg_min" && expectOk) (*cur)["dmg_max"] = 10000;          // cross rule of the validator: dmg_min <= dmg_max
                if (key == "stf") cur->erase("text");                                   // cross rule: exactly one of text / stf
                const ContentReport rep = pr.Run(kind, rec, envExtra);
                ++probes;
                if (expectOk) CHECK(rep.Ok(), "%s %s %s should be accepted by the validator%s", kind.c_str(), ps.c_str(), what, Dump(rep).c_str());
                else CHECK(!rep.Ok() && HasErr(rep, ps), "%s %s %s should be rejected at that path%s", kind.c_str(), ps.c_str(), what, Dump(rep).c_str());
            };
            if (type == "int" || type == "intenum" || type == "number") {
                if (f.contains("min")) { probe(f["min"], true, "min"); probe(type == "number" ? json(f["min"].get<double>() - 1) : json(f["min"].get<int>() - 1), false, "min-1"); }
                if (f.contains("max")) { probe(f["max"], true, "max"); probe(type == "number" ? json(f["max"].get<double>() + 1) : json(f["max"].get<int>() + 1), false, "max+1"); }
                probe("text", false, "wrong type");
            } else if (type == "string" || type == "text") {
                if (f.contains("enum")) {
                    for (const json& v : f["enum"]) probe(v, true, "enum value");
                    probe("NoSuchValue", false, "unknown enum value");
                } else if (f.contains("maxLen")) {
                    const size_t n = f["maxLen"].get<size_t>();
                    if (!f.contains("ref")) { probe(std::string(n, 'a'), true, "maxLen"); }
                    probe(std::string(n + 1, 'a'), false, "maxLen+1");
                }
                probe(5, false, "wrong type");
            } else if (type == "bool") {
                probe(true, true, "true"); probe("yes", false, "wrong type");
            } else if (type == "object") {
                json rec = base;
                json* cur = &rec;
                for (const Step& s : where) cur = s.index >= 0 ? &(*cur)[s.index] : &(*cur)[s.key];
                (*cur)[key] = json::object();
                std::vector<Step> sub = path;
                json saved = base;
                base = rec;   // descend with the empty object in place
                visit(f["fields"], sub);
                // unknown field inside this object
                json r2 = base;
                json* c2 = &r2;
                for (const Step& s : sub) c2 = s.index >= 0 ? &(*c2)[s.index] : &(*c2)[s.key];
                (*c2)["zz_unknown"] = 1;
                const ContentReport rep = pr.Run(kind, r2, envExtra);
                CHECK(HasErr(rep, ps + ".zz_unknown"), "%s %s rejects unknown fields%s", kind.c_str(), ps.c_str(), Dump(rep).c_str());
                base = saved;
            } else if (type == "intmap") {
                const std::string k0 = f["keys"][0];
                json v = json::object();
                v[k0] = f["min"];
                probe(v, true, "intmap min");
                v[k0] = f["max"];
                probe(v, true, "intmap max");
                v[k0] = f["min"].get<int>() - 1;
                { json rec = base; json* cur = &rec; for (const Step& s : where) cur = s.index >= 0 ? &(*cur)[s.index] : &(*cur)[s.key]; (*cur)[key] = v; const ContentReport rep = pr.Run(kind, rec, envExtra); CHECK(HasErr(rep, ps + "." + k0), "%s %s.%s min-1 rejected%s", kind.c_str(), ps.c_str(), k0.c_str(), Dump(rep).c_str()); }
                v[k0] = f["max"].get<int>() + 1;
                { json rec = base; json* cur = &rec; for (const Step& s : where) cur = s.index >= 0 ? &(*cur)[s.index] : &(*cur)[s.key]; (*cur)[key] = v; const ContentReport rep = pr.Run(kind, rec, envExtra); CHECK(HasErr(rep, ps + "." + k0), "%s %s.%s max+1 rejected%s", kind.c_str(), ps.c_str(), k0.c_str(), Dump(rep).c_str()); }
                for (const json& sk : f["keys"]) { json m = json::object(); m[sk.get<std::string>()] = f["min"]; probe(m, true, "every key of the enum is a valid key"); }
                { json m = json::object(); m["NoSuchSkill"] = f["min"]; probe(m, false, "unknown key"); }
            } else if (type == "cell") {
                probe(json::array({0, 0}), true, "cell");
                probe(json::array({-1, 0}), false, "negative cell");
                probe(json::array({1}), false, "short cell");
            } else if (type == "stringlist") {
                if (f["ref"] == "item" && f.contains("maxCount")) {   // item references are only resolved against game data: the count limit is structural
                    json a = json::array();
                    for (int i = 0; i < f["maxCount"].get<int>(); ++i) a.push_back("A.ITM");
                    probe(a, true, "maxCount entries");
                    a.push_back("A.ITM");
                    probe(a, false, "maxCount+1 entries");
                }
                probe(json::array({5}), false, "non-string entry");
            } else if (type == "list") {
                const json& item = f["item"];
                const std::string it = item["type"];
                if (f.contains("fixedLen")) {
                    json a = json::array();
                    for (int i = 0; i < f["fixedLen"].get<int>(); ++i) a.push_back(SampleValue(item));
                    probe(a, true, "fixedLen entries");
                    a.push_back(SampleValue(item));
                    probe(a, false, "fixedLen+1 entries");
                    // element boundaries
                    for (const char* lim : {"min", "max"}) {
                        json b2 = json::array();
                        for (int i = 0; i < f["fixedLen"].get<int>(); ++i) b2.push_back(SampleValue(item));
                        b2[0] = item[lim];
                        probe(b2, true, lim);
                    }
                    json lo = json::array();
                    for (int i = 0; i < f["fixedLen"].get<int>(); ++i) lo.push_back(SampleValue(item));
                    lo[0] = item["min"].get<int>() - 1;
                    probe(lo, false, "element min-1");
                    json hi = lo; hi[0] = item["max"].get<int>() + 1;
                    probe(hi, false, "element max+1");
                } else if (it == "object") {
                    auto mk = [&](int n) {
                        json a = json::array();
                        for (int i = 0; i < n; ++i) { json e = SampleObject(item["fields"]); for (const json& sf : item["fields"]) if (sf["key"] == "id") e["id"] = "n" + std::to_string(i); a.push_back(e); }
                        return a;
                    };
                    if (f.contains("maxCount")) {
                        const int n = f["maxCount"].get<int>();
                        probe(mk(n), true, "maxCount entries");
                        probe(mk(n + 1), false, "maxCount+1 entries");
                    }
                    // fields of an element: descend into element 0
                    json rec = base;
                    json* cur = &rec;
                    for (const Step& s : where) cur = s.index >= 0 ? &(*cur)[s.index] : &(*cur)[s.key];
                    (*cur)[key] = mk(1);
                    json saved = base;
                    base = rec;
                    std::vector<Step> sub = path;
                    sub.push_back({"", 0});
                    visit(item["fields"], sub);
                    base = saved;
                }
            }
            // condition / effects / dlgstart: covered by TestConditionsAndEffects
            (void)setAt; (void)walk;
        }
    };
    visit(rootFields, {});
    std::printf("  %-8s %d boundary probes\n", kind.c_str(), probes);
}

static void TestSchemaMatchesValidator() {
    std::printf("schema vs. validator (every field, boundary values)\n");
    const json schema = BuildSchema();
    CHECK(schema["contentSchema"] == kContentSchema, "schema carries the envelope version");
    CHECK(schema["enums"]["skills"].size() == kSkillSlots, "14 skills");
    Probe pr;
    for (const std::string& kind : schema["kindOrder"].get<std::vector<std::string>>()) {
        const json& k = schema["kinds"][kind];
        json base = SampleObject(k["fields"]);
        if (kind == "dialogs") base["nodes"] = json::array({json{{"id", "a"}, {"text", "x"}}});
        if (kind == "objects") base["object"] = "Obj_000";   // a binding needs an instance name or a cell
        const ContentReport rep = pr.Run(kind, base);
        CHECK(rep.Ok(), "%s: the minimal record built from the schema validates%s", kind.c_str(), Dump(rep).c_str());
        json env = json::object();
        if (kind == "traits") env["max_picks"] = 2;
        ProbeFields(pr, kind, k["fields"], env, base);
        // envelope fields
        if (k.contains("envelope"))
            for (const json& ef : k["envelope"]) {
                const std::string ek = ef["key"];
                json lo = json::object(), hi = json::object(), ok1 = json::object(), ok2 = json::object();
                lo[ek] = ef["min"].get<int>() - 1; hi[ek] = ef["max"].get<int>() + 1; ok1[ek] = ef["min"]; ok2[ek] = ef["max"];
                CHECK(pr.Run(kind, base, ok1).Ok() && pr.Run(kind, base, ok2).Ok(), "envelope %s min / max accepted", ek.c_str());
                CHECK(HasErr(pr.Run(kind, base, lo), ek) && HasErr(pr.Run(kind, base, hi), ek), "envelope %s out of range rejected", ek.c_str());
            }
        // unknown top-level record field
        json r2 = base;
        r2["zz_unknown"] = 1;
        CHECK(HasErr(pr.Run(kind, r2, env), "zz_unknown"), "%s: unknown record field rejected", kind.c_str());
    }
    // a record's id for the skills kind: every enum value is a skill, other names are not
    CHECK(!HasErr(pr.Run("skills", json{{"id", "Firearms"}}), "id"), "skills: enum ids");
}

static void TestConditionsAndEffects() {
    std::printf("dialog conditions and effects of the schema vs. the validator\n");
    const json schema = BuildSchema();
    Probe pr;
    auto dialogWith = [&](const json& cond, const json& effect) {
        json answer = json{{"id", "a"}, {"text", "x"}};
        if (!cond.is_null()) answer["if"] = cond;
        if (!effect.is_null()) answer["do"] = json::array({effect});
        return json{{"id", "d"}, {"nodes", json::array({json{{"id", "n"}, {"text", "x"}, {"answers", json::array({answer})}}})}};
    };
    const json& ck = schema["conditions"]["kinds"];
    int n = 0;
    for (const json& c : ck) {
        const std::string id = c["id"];
        std::vector<std::string> samples;
        const json& name = c["name"];
        std::vector<std::string> names;
        if (name.contains("enum")) for (const json& v : name["enum"]) names.push_back(v);
        else names.push_back(id == "quest" ? "q1" : id == "lua" ? "my_fn" : id == "item" ? "Junk.ITM" : "my_flag");
        for (const std::string& nm : names) {
            std::string s = id + ":" + nm;
            const std::string op = c["op"];
            if (op == "required") s += ">=" + std::to_string(c["value"]["min"].get<int>());
            else if (op == "eq-ne") s += "==" + std::string(c["value"]["enum"][0]);
            else if (op == "optional") { s += ">=" + std::to_string(c["value"]["max"].get<int>()); }
            const ContentReport rep = pr.Run("dialogs", dialogWith(s, json()));
            ++n;
            CHECK(rep.Ok(), "condition '%s' from the schema is accepted%s", s.c_str(), Dump(rep).c_str());
            if (op == "required") {
                const ContentReport hi = pr.Run("dialogs", dialogWith(id + ":" + nm + ">=" + std::to_string(c["value"]["max"].get<int>() + 1), json()));
                CHECK(!hi.Ok(), "condition '%s' above max is rejected", s.c_str());
            }
        }
        if (c["op"] == "eq-ne") for (const json& st : c["value"]["enum"]) CHECK(pr.Run("dialogs", dialogWith("quest:q1!=" + st.get<std::string>(), json())).Ok(), "quest state %s", st.get<std::string>().c_str());
    }
    for (const std::string& op : schema["conditions"]["ops"].get<std::vector<std::string>>())
        CHECK(pr.Run("dialogs", dialogWith("skill:Barter" + op + "40", json())).Ok(), "operator %s", op.c_str());
    CHECK(pr.Run("dialogs", dialogWith(json::array({"flag:a", json{{"any", json::array({"flag:b", json{{"not", "flag:c"}}})}}}), json())).Ok(), "combinators all / any / not");
    CHECK(!pr.Run("dialogs", dialogWith("bogus:x", json())).Ok(), "unknown condition kind rejected");
    for (const json& e : schema["effects"]["kinds"]) {
        json fx = json::object();
        for (const json& f : e["fields"]) {
            const std::string t = f["type"];
            const std::string key = f["key"];
            if (t == "const") fx[key] = f["value"];
            else if (!f.value("required", false)) continue;
            else if (t == "cell") fx[key] = json::array({1, 2});
            else if (t == "enum") fx[key] = f["enum"][0];
            else if (t == "int") fx[key] = f["min"];
            else fx[key] = key == "quest" ? "q1" : key == "lua" ? "fn" : "Junk.ITM";
        }
        const ContentReport rep = pr.Run("dialogs", dialogWith(json(), fx));
        ++n;
        CHECK(rep.Ok(), "effect '%s' from the schema is accepted%s", std::string(e["id"]).c_str(), Dump(rep).c_str());
    }
    for (const char* act : {"add", "done", "fail", "reopen"}) CHECK(pr.Run("dialogs", dialogWith(json(), json{{"quest", "q1"}, {"state", act}})).Ok(), "quest action %s", act);
    CHECK(!pr.Run("dialogs", dialogWith(json(), json{{"quest", "q1"}, {"state", "explode"}})).Ok(), "unknown quest action rejected");
    CHECK(!pr.Run("dialogs", dialogWith(json(), json{{"teleport", true}})).Ok(), "unknown effect rejected");
    CHECK(!pr.Run("dialogs", dialogWith(json(), json{{"end", true}, {"xp", 5}})).Ok(), "two actions in one effect rejected");
    std::printf("  %d condition / effect samples\n", n);
}

static void TestObjectScripts() {
    std::printf("object scripts: object picker endpoint, script status / create, objects kind save\n");
    Fixture f("objs", true);
    int calls = 0;
    std::string lastMap, lastQuery;
    int lastLimit = 0;
    f.host.objects = [&](const std::string& map, const std::string& q, int limit) {
        ++calls; lastMap = map; lastQuery = q; lastLimit = limit;
        if (map.empty()) return std::string(R"({"maps":[{"name":"town","width":10,"height":10,"objects":2}]})");
        if (map == "nowhere") return std::string(R"({"error":"map 'nowhere' is not loaded"})");
        return std::string(R"({"map":"town","width":10,"height":10,"total":1,"objects":[{"id":5,"name":"Comp_000","display":"Computer","proto":"X.USE","model":"m","kind":"usable","x":3,"y":4,"binding":""}],"unresolved":[]})");
    };
    Options o = f.api->options();
    o.scriptsDir = f.root / "scripts";
    f.api = std::make_unique<Api>(o, f.host);
    // the picker endpoint
    json m = J(f.api->Handle(Req("GET", "/api/objects")));
    CHECK(m["ok"] == true && m["maps"][0]["name"] == "town", "maps list: %s", m.dump().c_str());
    json l = J(f.api->Handle(Req("GET", "/api/objects", {{"map", "town"}, {"q", "comp"}, {"limit", "50"}})));
    CHECK(l["ok"] == true && l["objects"][0]["name"] == "Comp_000" && lastMap == "town" && lastQuery == "comp" && lastLimit == 50, "object list passes map / query / limit through: %s", l.dump().c_str());
    CHECK(J(f.api->Handle(Req("GET", "/api/objects", {{"map", "nowhere"}})))["ok"] == false, "an unknown map is reported");
    CHECK(f.api->Handle(Req("GET", "/api/objects", {{"limit", "x"}})).status == 400, "bad limit");
    CHECK(f.api->Handle(Req("GET", "/api/objects", {}, "", false)).status == 401, "the picker needs the token");
    CHECK(f.api->Handle(Req("POST", "/api/objects")).status == 405, "GET only");
    {
        Fixture g("objs_nohost");   // no server behind the editor
        CHECK(g.api->Handle(Req("GET", "/api/objects")).status == 503, "without a server the picker answers 503");
    }
    // script status / create
    json st = J(f.api->Handle(Req("GET", "/api/objscript", {{"path", "objects/t1.lua"}})));
    CHECK(st["ok"] == true && st["exists"] == false && st["path"] == "objects/t1.lua", "missing script: %s", st.dump().c_str());
    Response cr = f.api->Handle(Req("POST", "/api/objscript/create", {}, R"({"path":"objects/t1.lua"})"));
    json cj = J(cr);
    CHECK(cr.status == 200 && cj["created"] == true && cj["exists"] == true && cj["valid"] == true, "create from the template: %s", cr.body.c_str());
    CHECK(fs::exists(f.root / "scripts/objects/t1.lua"), "the file was written below scripts/objects");
    bool onUse = false;
    for (auto& h : cj["hooks"]) if (h == "on_use") onUse = true;
    CHECK(onUse, "the template defines on_use (hooks: %s)", cj["hooks"].dump().c_str());
    CHECK(cj["vscode"].get<std::string>().rfind("vscode://file/", 0) == 0 && cj["abs"].get<std::string>().find("t1.lua") != std::string::npos, "VS Code hint and absolute path: %s", cj["vscode"].dump().c_str());
    const std::string before = Slurp(f.root / "scripts/objects/t1.lua");
    Put(f.root / "scripts/objects/t1.lua", before + "\n-- my edit\n");
    cj = J(f.api->Handle(Req("POST", "/api/objscript/create", {}, R"({"path":"objects/t1.lua"})")));
    CHECK(cj["created"] == false && Slurp(f.root / "scripts/objects/t1.lua").find("my edit") != std::string::npos, "an existing file is never overwritten");
    Put(f.root / "scripts/objects/bad.lua", "function on_use(player, obj\n  return true\nend\n");
    st = J(f.api->Handle(Req("GET", "/api/objscript", {{"path", "objects/bad.lua"}})));
    CHECK(st["exists"] == true && st["valid"] == false && !st["error"].get<std::string>().empty(), "a syntax error is reported by the Lua loader: %s", st.dump().c_str());
    Put(f.root / "scripts/objects/two.lua", "function on_skill(p, o, s) return false end\nfunction on_look(p, o) end\nfunction helper() end\n");
    st = J(f.api->Handle(Req("GET", "/api/objscript", {{"path", "objects/two.lua"}})));
    CHECK(st["valid"] == true && st["hooks"].size() == 2 && st["hooks"][0] == "on_skill" && st["hooks"][1] == "on_look", "hooks found: %s", st.dump().c_str());
    Put(f.root / "scripts/objects/top.lua", "undefined_api_call()\nfunction on_use() end\n");
    st = J(f.api->Handle(Req("GET", "/api/objscript", {{"path", "objects/top.lua"}})));
    CHECK(st["valid"] == true && !st["warning"].get<std::string>().empty(), "top-level code that needs the server API is only a warning: %s", st.dump().c_str());
    for (const char* bad : {"../x.lua", "objects/../x.lua", "objects/a/b.lua", "objects/x.txt", "x.lua", "objects/.hid.lua", "objects/con.lua", "objects/a b.lua", "objects//x.lua", "/objects/x.lua", "objects/x.lua "}) {
        CHECK(f.api->Handle(Req("GET", "/api/objscript", {{"path", bad}})).status == 400, "status refuses path '%s'", bad);
        CHECK(f.api->Handle(Req("POST", "/api/objscript/create", {}, std::string("{\"path\":\"") + bad + "\"}")).status == 400, "create refuses path '%s'", bad);
    }
    CHECK(!fs::exists(f.root / "x.lua") && !fs::exists(f.root / "scripts/x.lua"), "nothing was written outside scripts/objects");
    CHECK(f.api->Handle(Req("POST", "/api/objscript/create", {}, "{}")).status == 400, "create needs a path");
    CHECK(f.api->Handle(Req("POST", "/api/objscript/create", {}, R"({"path":"objects/y.lua"})", false)).status == 401, "create needs the token");
    // the objects kind goes through the normal save flow with the real validator
    {
        Fixture g("objs_save", true);
        const std::string good = R"({"schema":1,"kind":"objects","records":[{"id":"o1","map":"town","object":"Comp_000","on_use":[{"dialog":"d1"}]}]})";
        Response ok = g.api->Handle(Req("PUT", "/api/put", {}, PutBody("objects", "o.json", good)));
        CHECK(ok.status == 200 && fs::exists(g.data / "objects/o.json"), "an objects file saves: %s", ok.body.c_str());
        const std::string badDoc = R"({"schema":1,"kind":"objects","records":[{"id":"o9","map":"town","object":"Other_000","on_use":[{"dialog":"nope"}]}]})";
        Response br = g.api->Handle(Req("PUT", "/api/put", {}, PutBody("objects", "o2.json", badDoc)));
        CHECK(br.status == 422 && br.body.find("unknown dialog") != std::string::npos && !fs::exists(g.data / "objects/o2.json"), "a dangling dialog reference is refused with its path: %s", br.body.c_str());
        json kinds = J(g.api->Handle(Req("GET", "/api/kinds")));
        bool listed = false;
        for (auto& k : kinds["kinds"]) if (k["id"] == "objects" && k["records"] == 1) listed = true;
        CHECK(listed, "the kind list shows the objects kind with its record count");
        json refs = J(g.api->Handle(Req("GET", "/api/refs")));
        bool hasObj = false;
        for (auto& o2 : refs["objects"]) if (o2 == "o1") hasObj = true;
        CHECK(hasObj, "refs list the binding ids");
    }
}

static void TestSavePreservesFieldOrder() {
    std::printf("save keeps the file's field order\n");
    Fixture f("order", true);
    const fs::path file = f.data / "perks/order.json";
    Put(file, "{\n  \"schema\": 1,\n  \"kind\": \"perks\",\n  \"records\": [\n    {\n      \"name\": \"Zed\",\n      \"ranks\": 2,\n      \"id\": \"zed_perk\"\n    }\n  ]\n}\n");
    const std::string before = Slurp(file);
    Response gr = f.api->Handle(Req("GET", "/api/get", {{"kind", "perks"}, {"file", "order.json"}}));
    ordered_json g = ordered_json::parse(gr.body);
    ordered_json doc = g["doc"];
    doc["records"][0]["name"] = "Zed Two";
    Response pr = f.api->Handle(Req("PUT", "/api/put", {}, PutBody("perks", "order.json", doc.dump(), ",\"etag\":\"" + g["etag"].get<std::string>() + "\"")));
    CHECK(J(pr)["ok"] == true, "order file saved: %s", pr.body.c_str());
    const std::string after = Slurp(file);
    auto lines = [](const std::string& t) { std::vector<std::string> v; std::stringstream ss(t); std::string l; while (std::getline(ss, l)) v.push_back(l); return v; };
    const auto lb = lines(before), la = lines(after);
    int diff = 0;
    CHECK(lb.size() == la.size(), "line count unchanged (%zu vs %zu)\n%s", lb.size(), la.size(), after.c_str());
    for (size_t i = 0; i < lb.size() && i < la.size(); ++i) if (lb[i] != la[i]) { ++diff; CHECK(la[i].find("Zed Two") != std::string::npos, "unexpected changed line: %s", la[i].c_str()); }
    CHECK(diff == 1, "exactly one line changed (%d)", diff);
}

int main(int argc, char** argv) {
    std::srand(static_cast<unsigned>(std::time(nullptr)));
    TestSecurity();
    TestRequestGuards();
    TestReadEndpoints();
    TestSaveFlow();
    TestSavePreservesFieldOrder();
    TestLocale();
    TestEmitterOnRepoData(argc > 0 ? argv[0] : "");
    TestSchemaMatchesValidator();
    TestConditionsAndEffects();
    TestObjectScripts();
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
