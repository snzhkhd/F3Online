// f3_client: raylib game client.
//   f3_client [--host H] [--port N] [--name NAME]
// Game loop talks to IRenderer only; see IRenderer.h.
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include "ClientCore.h"
#include "IRenderer.h"
#include "common/log.h"

using namespace f3;

int main(int argc, char** argv) {
    std::string host = "127.0.0.1", name = "Player";
    uint16_t port = kDefaultPort;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--host") host = next();
        else if (a == "--port") port = static_cast<uint16_t>(std::atoi(next()));
        else if (a == "--name") name = next();
        else { std::cerr << "usage: f3_client [--host H] [--port N] [--name NAME]\n"; return 2; }
    }

    std::unique_ptr<IRenderer> renderer = CreateRaylibRenderer();
    if (!renderer->Init(1280, 720, "F3Online")) { log::Error("cannot open window"); return 1; }

    ClientCore core;
    if (!core.Connect(host, port, name)) log::Error("connect: %s", core.failReason().c_str());

    bool typing = false;
    std::string input;
    while (!renderer->ShouldClose()) {
        core.Poll();
        FrameInput in = renderer->PollInput();

        // chat input handling
        if (typing) {
            input += in.typed;
            if (input.size() > kMaxChatLen) input.resize(kMaxChatLen);
            if (in.backspace) {
                while (!input.empty() && (static_cast<unsigned char>(input.back()) & 0xC0) == 0x80) input.pop_back();
                if (!input.empty()) input.pop_back();
            }
            if (in.enter) { core.SendChat(input); input.clear(); typing = false; }
            else if (in.escape) { input.clear(); typing = false; }
        } else if (in.enter) {
            typing = true;
        }

        // click to move
        if (!typing && in.mouseLeftPressed && core.state() == ClientCore::State::InGame && core.hasMap() &&
            !renderer->IsOverUi(in.mouseX, in.mouseY)) {
            TilePos t;
            if (renderer->ScreenToTile(in.mouseX, in.mouseY, t) && !core.map().IsBlocked(t.x, t.y)) core.RequestMove(t);
        }

        // snapshot for the renderer
        SceneView scene;
        scene.map = core.hasMap() ? &core.map() : nullptr;
        for (auto& [id, c] : core.critters()) {
            SceneCritter sc;
            sc.critter = &c;
            core.RenderPos(c, sc.x, sc.y);
            sc.isSelf = (id == core.selfId());
            if (sc.isSelf) { scene.cameraX = sc.x; scene.cameraY = sc.y; }
            scene.critters.push_back(sc);
        }
        for (auto& [id, it] : core.items()) scene.items.push_back(&it);

        UiView ui;
        ui.chat = &core.chat();
        ui.chatInputActive = typing;
        ui.chatInput = input;
        ui.now = core.Now();
        switch (core.state()) {
            case ClientCore::State::Connecting: ui.status = "Connecting to " + host + "..."; break;
            case ClientCore::State::LoggingIn: ui.status = "Logging in..."; break;
            case ClientCore::State::InGame: {
                const ClientCritter* me = core.self();
                ui.status = core.map().name + (me ? "  (" + std::to_string(me->pos.x) + "," + std::to_string(me->pos.y) + ")" : "");
                break;
            }
            default: ui.status = "Disconnected: " + core.failReason(); break;
        }

        renderer->BeginFrame();
        renderer->DrawScene(scene);
        renderer->DrawUi(ui);
        renderer->EndFrame();
    }
    core.Disconnect();
    renderer->Shutdown();
    return 0;
}
