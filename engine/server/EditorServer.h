#pragma once
// The loopback-only HTTP front of the content editor (docs/editor.md): binds 127.0.0.1, owns the per-run token, and connects the
// HTTP-free core (EditorCore) to the running Server through Server::PostTask (the HTTP thread never touches game state).
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace f3 {

class Server;

struct EditorConfig {
    uint16_t port = 4041;
    std::string bind = "127.0.0.1";          // anything that is not a loopback address is refused
    std::filesystem::path uiDir;             // tools/editor; empty = search next to the binary / in the parents of the cwd
};

// True for 127.0.0.0/8, "localhost" and "::1" (the only addresses the editor may bind).
bool EditorBindAllowed(const std::string& host);

class EditorServer {
public:
    EditorServer(Server& server, EditorConfig cfg);
    ~EditorServer();
    // Takes the snapshot of the base layer (call on the server thread before Server::Run), binds, starts the HTTP thread, prints the URL with the token.
    bool Start(std::string& err);
    void Stop();
    const std::string& Token() const;
    int Port() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace f3
