// f3_client: raylib game client.
//   f3_client [--host H] [--port N] [--name NAME] [--gamedir DIR]
//   f3_client --viewmap MAP --gamedir DIR   (map viewer without a server)
// Game loop talks to IRenderer only; see IRenderer.h.
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include "ClientCore.h"
#include "IRenderer.h"
#include "common/log.h"

using namespace f3;

static const char* kUsage =
    "usage: f3_client [--host H] [--port N] [--name NAME] [--gamedir DIR]\n"
    "       f3_client --viewmap MAP --gamedir DIR       map viewer, no server needed\n"
    "viewer/test options: --shot FILE.png [--shot-frame N] [--shot-exit]  --cam AZ,EL,FOV  --target X,Z\n"
    "                     --clipy Y  --noentities  --modelscale S  --size W,H\n"
    "The game directory is the original install (resource.rht + data\\*.grp); F3_GAMEDIR sets the default.\n";

int main(int argc, char** argv) {
    std::string host = "127.0.0.1", name = "Player", viewMap;
    uint16_t port = kDefaultPort;
    RendererOptions ropt;
    if (const char* env = std::getenv("F3_GAMEDIR")) ropt.gameDir = env;
    int winW = 1280, winH = 720;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--host") host = next();
        else if (a == "--port") port = static_cast<uint16_t>(std::atoi(next()));
        else if (a == "--name") name = next();
        else if (a == "--gamedir") ropt.gameDir = next();
        else if (a == "--viewmap") viewMap = next();
        else if (a == "--shot") ropt.screenshotPath = next();
        else if (a == "--shot-frame") ropt.screenshotFrame = std::atoi(next());
        else if (a == "--shot-exit") ropt.exitAfterScreenshot = true;
        else if (a == "--cam") {
            ropt.hasCam = std::sscanf(next(), "%f,%f,%f", &ropt.camAzimuth, &ropt.camElevation, &ropt.camFov) == 3;
        } else if (a == "--target") {
            ropt.hasTarget = std::sscanf(next(), "%f,%f", &ropt.targetX, &ropt.targetZ) == 2;
        } else if (a == "--clipy") ropt.clipY = static_cast<float>(std::atof(next()));
        else if (a == "--noentities") ropt.noEntities = true;
        else if (a == "--modelscale") ropt.modelScale = static_cast<float>(std::atof(next()));
        else if (a == "--size") std::sscanf(next(), "%d,%d", &winW, &winH);
        else { std::cerr << kUsage; return 2; }
    }
    if (!viewMap.empty() && ropt.gameDir.empty()) { std::cerr << "--viewmap needs --gamedir (or F3_GAMEDIR)\n"; return 2; }

    std::unique_ptr<IRenderer> renderer = CreateRaylibRenderer(ropt);
    if (!renderer->Init(winW, winH, "F3Online")) { log::Error("cannot open window"); return 1; }

    // ---- map viewer: no network, the renderer draws the original map by itself ----
    if (!viewMap.empty()) {
        std::string err;
        if (!renderer->HasAssets() || !renderer->LoadOriginalMap(viewMap, &err)) {
            log::Error("cannot load map '%s': %s", viewMap.c_str(), err.c_str());
            renderer->Shutdown();
            return 1;
        }
        while (!renderer->ShouldClose()) {
            renderer->PollInput();
            SceneView scene;
            scene.viewer = true;
            UiView ui;
            ui.status = "map viewer: " + viewMap;
            renderer->BeginFrame();
            renderer->DrawScene(scene);
            renderer->DrawUi(ui);
            renderer->EndFrame();
        }
        renderer->Shutdown();
        return 0;
    }

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
            if (sc.isSelf) { scene.cameraX = sc.x; scene.cameraY = sc.y; scene.hasSelf = true; }
            scene.critters.push_back(sc);
        }
        scene.allowCameraKeys = !typing;
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
