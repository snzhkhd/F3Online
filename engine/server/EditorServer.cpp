// Content editor HTTP front (docs/editor.md). httplib.h must come before anything that pulls in winsock / windows.h.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <httplib.h>
#include "EditorServer.h"
#include <atomic>
#include <future>
#include <random>
#include <thread>
#include "DataFormat.h"
#include "EditorCore.h"
#include "Server.h"
#include "common/log.h"

namespace f3 {

namespace fs = std::filesystem;

bool EditorBindAllowed(const std::string& host) {
    if (host == "localhost" || host == "::1") return true;
    return editor::Api::IsLoopbackAddr(host);
}

namespace {

std::string RandomToken() {
    std::random_device rd;   // non-deterministic on MSVC, libstdc++ and libc++
    std::string t;
    char b[9];
    for (int i = 0; i < 4; ++i) { std::snprintf(b, sizeof b, "%08x", static_cast<unsigned>(rd())); t += b; }
    return t;
}

fs::path FindUiDir(const fs::path& hint) {
    std::error_code ec;
    if (!hint.empty()) return fs::exists(hint / "index.html", ec) ? hint : fs::path();
    // tools/editor next to the binary or in a parent of the cwd / the binary
    for (fs::path d : {fs::current_path(ec)}) {
        for (int i = 0; i < 6 && !d.empty(); ++i, d = d.parent_path()) {
            if (fs::exists(d / "tools" / "editor" / "index.html", ec)) return d / "tools" / "editor";
            if (d == d.root_path()) break;
        }
    }
    return {};
}

}  // namespace

struct EditorServer::Impl {
    Server& server;
    EditorConfig cfg;
    std::string token = RandomToken();
    std::unique_ptr<editor::Api> api;
    httplib::Server http;
    std::thread thread;
    int port = 0;
    bool running = false;
    std::atomic<bool> stopping{false};
    explicit Impl(Server& s, EditorConfig c) : server(s), cfg(std::move(c)) {}

    // Runs fn on the server thread (Server::PostTask) and waits for the result; the HTTP thread never reads game state itself.
    template <class T, class F>
    bool OnServer(F fn, T& out, int timeoutMs) {
        auto prom = std::make_shared<std::promise<T>>();
        std::future<T> fut = prom->get_future();
        server.PostTask([prom, fn] {
            try { prom->set_value(fn()); } catch (...) { try { prom->set_exception(std::current_exception()); } catch (...) {} }
        });
        for (int waited = 0; fut.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready; waited += 100)
            if (waited >= timeoutMs || stopping) return false;   // the main loop is gone or stuck: answer the HTTP request instead of hanging
        out = fut.get();
        return true;
    }

    editor::Host MakeHost() {
        editor::Host h;
        const DataStore& d = server.Data();   // read here, before the HTTP thread exists: the base layer is a snapshot
        h.hasGamedata = d.Loaded();
        if (d.Loaded()) {
            for (const Proto& p : d.BaseProtos()) {
                editor::BaseProto b;
                b.key = p.key;
                if (p.cls == PClass::Critter) {
                    if (p.isStatic) continue;
                    b.cls = "critter";
                    b.name = p.ReadableName();
                    b.record = ExportNpcRecord(p);
                } else if (p.cls == PClass::Weapon || p.cls == PClass::Armor || p.cls == PClass::Ammo || p.cls == PClass::Item) {
                    b.cls = p.cls == PClass::Weapon ? "weapon" : p.cls == PClass::Armor ? "armor" : p.cls == PClass::Ammo ? "ammo" : "misc";
                    b.name = p.DisplayName();
                    b.record = ExportItemRecord(p);
                } else continue;
                h.base.push_back(std::move(b));
            }
            h.maps = d.MapNames();
        }
        Server* s = &server;
        const int to = 20000;
        h.validate = [this, s, to](const fs::path& dir) {
            ContentReport rep;
            const bool done = OnServer<ContentReport>([s, dir] {
                ContentReport r;
                ContentDb db;
                ContentDb::Build(dir, s->Data().Loaded() ? &s->Data() : nullptr, false, db, r);   // strict whatever --lenient says: the editor must see every error
                return r;
            }, rep, to);
            if (!done) rep.Error("data", "$", "validation timed out (the server main loop did not answer in time)");
            return rep;
        };
        h.reload = [this, s, to](const std::string& what) {
            std::string text;
            if (!OnServer<std::string>([s, what] { return s->ReloadAll(what); }, text, to)) return std::vector<std::string>{"data reload FAILED: the server main loop did not answer in time"};
            std::vector<std::string> lines;
            std::istringstream ls(text);
            for (std::string l; std::getline(ls, l);) lines.push_back(l);
            return lines;
        };
        h.stf = [this, s, to](int id) {
            std::string t;
            OnServer<std::string>([s, id] { return s->Data().String(id); }, t, to);
            return t;
        };
        h.stfSearch = [this, s, to](const std::string& needle, size_t limit) {
            std::vector<std::pair<int, std::string>> v;
            OnServer<std::vector<std::pair<int, std::string>>>([s, needle, limit] { return s->Data().SearchStrings(needle, limit); }, v, to);
            return v;
        };
        return h;
    }

    void Serve(const httplib::Request& hreq, httplib::Response& hres) {
        editor::Request r;
        r.method = hreq.method;
        r.path = hreq.path;
        r.body = hreq.body;
        r.remoteAddr = hreq.remote_addr;
        for (const auto& kv : hreq.headers) {
            std::string k = kv.first;
            for (char& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            r.headers[k] = kv.second;
        }
        for (const auto& kv : hreq.params) r.query[kv.first] = kv.second;
        const editor::Response out = api->Handle(r);
        hres.status = out.status;
        hres.set_content(out.body, out.contentType);
        // no CORS headers, ever: cross-origin pages must not be able to read anything
        hres.set_header("Cache-Control", "no-store");
        hres.set_header("X-Content-Type-Options", "nosniff");
        hres.set_header("Referrer-Policy", "no-referrer");
        hres.set_header("X-Frame-Options", "DENY");
        hres.set_header("Content-Security-Policy", "default-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; frame-ancestors 'none'; base-uri 'none'; form-action 'none'");
    }
};

EditorServer::EditorServer(Server& server, EditorConfig cfg) : impl_(std::make_unique<Impl>(server, std::move(cfg))) {}
EditorServer::~EditorServer() { Stop(); }
const std::string& EditorServer::Token() const { return impl_->token; }
int EditorServer::Port() const { return impl_->port; }

bool EditorServer::Start(std::string& err) {
    Impl& m = *impl_;
    if (!EditorBindAllowed(m.cfg.bind)) { err = "refusing to bind the editor to '" + m.cfg.bind + "': only loopback addresses (127.0.0.1, ::1, localhost) are allowed"; return false; }
    const fs::path content = m.server.GetConfig().contentDir;
    if (content.empty()) { err = "the editor needs a data/ folder (none found; use --content DIR)"; return false; }
    editor::Options o;
    o.contentDir = content;
    o.uiDir = FindUiDir(m.cfg.uiDir);
    o.token = m.token;
    o.port = m.cfg.port;
    if (o.uiDir.empty()) log::Warn("editor: tools/editor (the web front end) was not found; only /api is served (use --editor-dir DIR)");
    m.api = std::make_unique<editor::Api>(std::move(o), m.MakeHost());
    m.http.set_payload_max_length(8u * 1024 * 1024);
    m.http.set_read_timeout(10, 0);
    m.http.set_write_timeout(10, 0);
    m.http.new_task_queue = [] { return new httplib::ThreadPool(4); };
    // One catch-all route per method (not set_pre_routing_handler: that runs before the request body is read).
    auto handler = [&m](const httplib::Request& req, httplib::Response& res) { m.Serve(req, res); };
    const char* const any = R"(.*)";
    m.http.Get(any, handler);
    m.http.Post(any, handler);
    m.http.Put(any, handler);
    m.http.Delete(any, handler);
    m.http.Options(any, handler);
    m.http.Patch(any, handler);
    const std::string host = m.cfg.bind == "localhost" ? "127.0.0.1" : m.cfg.bind;
    if (!m.http.bind_to_port(host, m.cfg.port)) { err = "cannot bind the editor to " + host + ":" + std::to_string(m.cfg.port) + " (port in use?)"; return false; }
    m.port = m.cfg.port;
    m.running = true;
    m.thread = std::thread([&m] { m.http.listen_after_bind(); });
    const std::string url = "http://" + std::string(host.find(':') != std::string::npos ? "[" + host + "]" : host) + ":" + std::to_string(m.port) + "/?token=" + m.token;
    log::Info("editor: %s", url.c_str());
    log::Info("editor: loopback only; the token above is required (random per run). Edits data/ in %s", fs::absolute(content).string().c_str());
    return true;
}

void EditorServer::Stop() {
    Impl& m = *impl_;
    if (!m.running) return;
    m.running = false;
    m.stopping = true;
    m.http.stop();
    if (m.thread.joinable()) m.thread.join();
}

}  // namespace f3
