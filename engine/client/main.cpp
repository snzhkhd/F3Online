// f3_client: raylib game client.
//   f3_client [--host H] [--port N] [--name NAME] [--password PW [--register]] [--guest] [--gamedir DIR]
// Without --password / --guest a login screen (name, password, Login / Register) is shown first; --name only prefills it.
//   f3_client --viewmap MAP [--gamedir DIR]   (map viewer without a server)
// The game directory is auto-detected (see Settings.h); the game loop talks to IRenderer only (see IRenderer.h).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include "ClientCore.h"
#include "IRenderer.h"
#include "Settings.h"
#include "common/log.h"

using namespace f3;

static const char* kUsage =
    "usage: f3_client [--host H] [--port N] [--name NAME] [--password PW [--register]] [--guest] [--gamedir DIR] [--protos FILE|DIR]\n"
    "       f3_client --viewmap MAP [--gamedir DIR]     map viewer, no server needed\n"
    "game data: --gamedir, env F3_GAMEDIR, client.cfg (gamedir=...), else auto-detected (F:\\_work\\F3_Demo, ..\\F3_Demo)\n"
    "options:   --no-assets (flat placeholder view)  --size W,H\n"
    "test:      --shot FILE.png [--shot-frame N] [--shot-exit]   --shots PREFIX --shot-times t0,t1,.. [--shot-exit]\n"
    "           --autowalk (random walks)  --trace FILE.csv  --run-seconds N\n"
    "           --cam AZ,EL,FOV  --target X,Z  --clipy Y  --noentities  --modelscale S\n"
    "controls:  LMB act, RMB click toggles Move/Examine cursor (A = attack cursor, RMB/Esc back) or drag to orbit;\n"
    "           pickup / use / talk cursors switch by themselves over items / usable objects / NPCs,\n"
    "           I inventory, C character, X examine, B switch active hand, Tab stop attack, Enter chat (PgUp/PgDn or wheel over the chat scrolls it),\n"
    "           wheel zoom, Shift+LMB = walk.\n";

namespace {

struct Trace {
    std::ofstream csv;
    struct Sample { double t, x, y, speed; bool moving; };
    std::vector<Sample> samples;
};

void SummariseTrace(const std::vector<Trace::Sample>& s) {
    // Smoothness check: for consecutive frames inside one constant-speed segment the drawn displacement must equal
    // speed * frame time. ratio = displacement / (speed * dt); a stepping (tile-hopping) renderer would show ratio 0 on
    // many frames and large values on others. Frames around screenshots (long dt) are included: ratio is dt independent.
    std::vector<double> ratios, disp;
    double maxGap = 0;
    size_t movingFrames = 0;
    for (size_t i = 1; i < s.size(); ++i) {
        const double dt = s[i].t - s[i - 1].t;
        maxGap = std::max(maxGap, dt);
        if (s[i].moving) ++movingFrames;
        if (!s[i].moving || !s[i - 1].moving || dt <= 0 || std::fabs(s[i].speed - s[i - 1].speed) > 1e-3 || s[i].speed < 0.1) continue;
        const double d = std::hypot(s[i].x - s[i - 1].x, s[i].y - s[i - 1].y);
        ratios.push_back(d / (s[i].speed * dt));
        disp.push_back(d);
    }
    if (ratios.size() < 5) { log::Info("trace: only %zu usable moving frames, no summary", ratios.size()); return; }
    double mean = 0;
    for (double v : ratios) mean += v;
    mean /= ratios.size();
    double var = 0, mn = 1e9, mx = 0;
    size_t bad = 0;
    for (double v : ratios) { var += (v - mean) * (v - mean); mn = std::min(mn, v); mx = std::max(mx, v); if (std::fabs(v - 1.0) > 0.1) ++bad; }
    var = std::sqrt(var / ratios.size());
    log::Info("trace: %zu frames, %zu moving, %zu in constant-speed segments; displacement/(speed*dt) mean %.4f sd %.4f min %.3f max %.3f; frames off by >10%%: %zu; max frame gap %.1f ms",
              s.size(), movingFrames, ratios.size(), mean, var, mn, mx, bad, maxGap * 1000.0);
}

bool StraightWalkable(const ClientMap& m, float x0, float y0, float x1, float y1) {
    const float len = std::hypot(x1 - x0, y1 - y0);
    const int n = std::max(1, static_cast<int>(len * 2));
    for (int i = 0; i <= n; ++i) {
        const float u = static_cast<float>(i) / n;
        const int cx = static_cast<int>(std::lround(x0 + (x1 - x0) * u)), cy = static_cast<int>(std::lround(y0 + (y1 - y0) * u));
        if (m.IsBlocked(cx, cy)) return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg = LoadConfig();
    std::string host = cfg.count("host") ? cfg["host"] : "127.0.0.1";
    std::string name = cfg.count("name") ? cfg["name"] : "Player";
    std::string viewMap, protosHint, gameDirArg, password;
    bool doRegister = false, guest = false, autoCreate = false;
    uint16_t port = cfg.count("port") ? static_cast<uint16_t>(std::atoi(cfg["port"].c_str())) : kDefaultPort;
    bool openInv = false, openChar = false, toggleEquip = false;
    std::string draftArg;
    int toggleStep = 0;
    double lastToggle = 0;
    bool noAssets = false, autowalk = false, hostGiven = false, legacy = false, autoattack = false, autopickup = false;
    std::vector<std::string> sayList;
    std::string traceFile;
    double runSeconds = 0;
    RendererOptions ropt;
    int winW = 1280, winH = 720;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--host") { host = next(); hostGiven = true; }
        else if (a == "--port") port = static_cast<uint16_t>(std::atoi(next()));
        else if (a == "--name") name = next();
        else if (a == "--password") password = next();
        else if (a == "--register") doRegister = true;
        else if (a == "--guest") guest = true;
        else if (a == "--autocreate") autoCreate = true;   // scripted tests: create the character with a fixed allocation
        else if (a == "--gamedir") gameDirArg = next();
        else if (a == "--protos") protosHint = next();
        else if (a == "--no-assets") noAssets = true;
        else if (a == "--viewmap") viewMap = next();
        else if (a == "--shot") ropt.screenshotPath = next();
        else if (a == "--shot-frame") ropt.screenshotFrame = std::atoi(next());
        else if (a == "--shot-exit") ropt.exitAfterScreenshot = true;
        else if (a == "--no-origui") ropt.noOrigUi = true;
        else if (a == "--open-inv") openInv = true;
        else if (a == "--open-char") openChar = true;   // testing: the character window is open once the character exists
        else if (a == "--draft") draftArg = next();     // testing: preset the creation screen, "S,P,E,C,I,A,L:tagMask:traitMask"
        else if (a == "--toggle-equip") toggleEquip = true;
        else if (a == "--shots") ropt.shotPrefix = next();
        else if (a == "--shot-times") {
            std::stringstream ss(next());
            std::string t;
            while (std::getline(ss, t, ',')) ropt.shotTimes.push_back(std::atof(t.c_str()));
        } else if (a == "--autowalk") autowalk = true;
        else if (a == "--autoattack") autoattack = true;
        else if (a == "--autopickup") autopickup = true;
        else if (a == "--legacy") legacy = true;
        else if (a == "--say") sayList.push_back(next());
        else if (a == "--trace") traceFile = next();
        else if (a == "--run-seconds") runSeconds = std::atof(next());
        else if (a == "--cam") {
            ropt.hasCam = std::sscanf(next(), "%f,%f,%f", &ropt.camAzimuth, &ropt.camElevation, &ropt.camFov) == 3;
        } else if (a == "--target") {
            ropt.hasTarget = std::sscanf(next(), "%f,%f", &ropt.targetX, &ropt.targetZ) == 2;
        } else if (a == "--clipy") ropt.clipY = static_cast<float>(std::atof(next()));
        else if (a == "--noentities") ropt.noEntities = true;
        else if (a == "--grid") ropt.showGrid = true;
        else if (a == "--modelscale") ropt.modelScale = static_cast<float>(std::atof(next()));
        else if (a == "--size") std::sscanf(next(), "%d,%d", &winW, &winH);
        else { std::cerr << kUsage; return 2; }
    }
    (void)hostGiven;

    // ---- game data directory: argument > env > client.cfg > auto-detect; the result is persisted ----
    GameDirResult gd;
    if (!noAssets) gd = DetectGameDir(gameDirArg);
    ropt.gameDir = gd.dir;
    if (!gd.dir.empty()) log::Info("game data: %s (%s)", gd.dir.c_str(), gd.source.c_str());
    else if (!noAssets) {
        log::Warn("no game data directory found; using the flat placeholder renderer");
        for (const auto& t : gd.tried) log::Warn("  tried %s", t.c_str());
    }
    if (!viewMap.empty() && ropt.gameDir.empty()) {
        std::cerr << "--viewmap needs the game data (set F3_GAMEDIR, client.cfg gamedir=..., or --gamedir DIR)\n";
        return 2;
    }

    ClientCore core;
    if (viewMap.empty()) {
        std::string perr;
        if (!core.protos().Load(protosHint, &perr)) log::Warn("%s: critter/item models fall back to a generic human", perr.c_str());
        ropt.network = true;
        ropt.protos = &core.protos();
    }

    std::unique_ptr<IRenderer> renderer = CreateRaylibRenderer(ropt);
    if (!renderer->Init(winW, winH, "F3Online")) { log::Error("cannot open window"); return 1; }

    // ---- map viewer: no network, the renderer draws the original map by itself ----
    if (!viewMap.empty()) {
        std::string err;
        if (!renderer->HasAssets() || !renderer->LoadOriginalMap(viewMap, &err)) {
            log::Error("cannot load map '%s': %s", viewMap.c_str(), err.empty() ? renderer->AssetError().c_str() : err.c_str());
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

    // remember connection settings for the next start
    {
        Config c = LoadConfig();
        c["host"] = host; c["port"] = std::to_string(port); c["name"] = name;
        SaveConfig(c);
    }
    core.SetLegacyOnly(legacy);
    // login screen unless the command line already says who to be (--password, or --guest for the legacy name-only login)
    bool loginPhase = !guest && password.empty();
    LoginView lv;
    lv.name = name;
    lv.server = "server " + host + ":" + std::to_string(port);
    if (!loginPhase && !core.Connect(host, port, name, guest ? std::string() : password, doRegister)) log::Error("connect: %s", core.failReason().c_str());

    bool typing = false;
    std::string input;
    UiState uiState;
    uiState.inventory = openInv;
    if (!draftArg.empty()) {
        unsigned v[7] = {5, 5, 5, 5, 5, 5, 5}, tm = 0, trm = 0;
        std::sscanf(draftArg.c_str(), "%u,%u,%u,%u,%u,%u,%u:%u:%u", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &tm, &trm);
        for (int i = 0; i < 7; ++i) uiState.draft.special[i] = static_cast<int>(v[i]);
        uiState.draft.tagMask = tm;
        uiState.draft.traitMask = trm;
        uiState.draft.init = true;
    }
    std::vector<UiCommand> commands;
    Trace trace;
    if (!traceFile.empty()) { trace.csv.open(traceFile); trace.csv << "t,x,y,moving,speed\n"; }
    double inGameSince = -1, lastAutoMove = -100, lastAutoAct = -100;
    int autoStep = 0, frame = 0;
    bool quit = false, wasCreating = false;

    auto localNote = [&](const std::string& t) { (void)t; };
    (void)localNote;

    while (!renderer->ShouldClose() && !quit) {
        if (loginPhase) {
            core.Poll();
            FrameInput li = renderer->PollInput();
            if (core.state() == ClientCore::State::InGame) { loginPhase = false; lv.password.clear(); }
            else {
                int sw = 1280, sh = 720;
                renderer->WindowSize(sw, sh);
                const LoginLayout L(sw, sh);
                lv.busy = core.state() == ClientCore::State::Connecting || core.state() == ClientCore::State::LoggingIn;
                if (core.state() == ClientCore::State::Failed && !lv.busy) { lv.error = core.failReason(); lv.status.clear(); }
                lv.hover = -1;
                if (LoginLayout::In(li.mouseX, li.mouseY, L.loginX, L.btnY, L.btnW, L.btnH)) lv.hover = 0;
                else if (LoginLayout::In(li.mouseX, li.mouseY, L.regX, L.btnY, L.btnW, L.btnH)) lv.hover = 1;
                auto submit = [&](bool reg) {
                    if (lv.busy) return;
                    if (lv.name.size() < 2) { lv.error = "enter a name (2-16 letters, digits or _)"; return; }
                    if (lv.password.size() < 4) { lv.error = "enter a password (at least 4 characters)"; return; }
                    lv.error.clear();
                    lv.status = reg ? "Creating the account..." : "Logging in...";
                    Config c = LoadConfig();   // the name is remembered, the password never
                    c["host"] = host; c["port"] = std::to_string(port); c["name"] = lv.name;
                    SaveConfig(c);
                    if (!core.Connect(host, port, lv.name, lv.password, reg)) lv.error = core.failReason();
                };
                if (!lv.busy) {
                    std::string& f = lv.focus == 0 ? lv.name : lv.password;
                    for (char ch : li.typed) {
                        if (lv.focus == 0 ? (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_') && lv.name.size() < kMaxNameLen
                                          : static_cast<unsigned char>(ch) >= 32 && ch != 127 && lv.password.size() < 64)
                            f += ch;
                    }
                    if (li.backspace && !f.empty()) f.pop_back();
                    if (li.keyStop) lv.focus ^= 1;
                    if (li.mouseLeftPressed) {
                        if (LoginLayout::In(li.mouseX, li.mouseY, L.nameX, L.nameY, L.fieldW, L.fieldH)) lv.focus = 0;
                        else if (LoginLayout::In(li.mouseX, li.mouseY, L.passX, L.passY, L.fieldW, L.fieldH)) lv.focus = 1;
                        else if (lv.hover == 0) submit(false);
                        else if (lv.hover == 1) submit(true);
                    }
                    if (li.enter) { if (lv.focus == 0 && !lv.name.empty() && lv.password.empty()) lv.focus = 1; else submit(false); }
                }
                renderer->BeginFrame();
                renderer->DrawLogin(lv);
                renderer->EndFrame();
                ++frame;
                if (runSeconds > 0 && core.Now() > runSeconds + 8) quit = true;
                continue;
            }
        }
        core.Poll();
        FrameInput in = renderer->PollInput();
        const double now = core.Now();
        const bool inGame = core.state() == ClientCore::State::InGame;
        if (inGame && inGameSince < 0) inGameSince = now;
        // character creation happens before the world: the server keeps the session out of the map until a valid CharCreate, the
        // renderer shows the creation screen (no map, no HUD) and the world input below is off
        const bool creating = inGame && core.hasSheet() && !core.sheet().created;
        if (creating) {
            uiState.charSheet = true;   // the raylib fallback draws its creation window while this is set
            uiState.inventory = false;
            wasCreating = true;
            if (autoCreate) {
                static const uint8_t sp[7] = {7, 5, 5, 5, 8, 6, 4};   // sums to 40
                core.CharCreate(sp, (1u << 0) | (1u << 7) | (1u << 9), (1u << 0) | (1u << 3));   // tags Firearms / Medic / Science, traits Bruiser / Fast Shot
                autoCreate = false;
            }
        } else if (wasCreating && inGame) {
            wasCreating = false;
            uiState.charSheet = openChar;   // created: into the world with the window closed
        }

        // ---- chat input ----
        if (typing) {
            input += in.typed;
            if (input.size() > kMaxChatLen) input.resize(kMaxChatLen);
            if (in.backspace) {
                while (!input.empty() && (static_cast<unsigned char>(input.back()) & 0xC0) == 0x80) input.pop_back();
                if (!input.empty()) input.pop_back();
            }
            if (in.enter) { core.SendChat(input); input.clear(); typing = false; }
            else if (in.escape) { input.clear(); typing = false; }
        } else if (in.enter && !creating) {
            typing = true;
        }

        // ---- hotkeys and mouse ----
        if (!typing && inGame && !creating) {
            if (in.keySkill) {   // S: skill cursor (the first press picks the last used skill, further presses cycle, Shift+S backwards)
                if (uiState.mode == CursorMode::Skill) uiState.skill = (uiState.skill + (in.shift ? kSkillCount - 1 : 1)) % kSkillCount;
                uiState.mode = CursorMode::Skill;
                uiState.charSheet = false;
            }
            if (in.keyAttack) uiState.mode = CursorMode::Attack;
            if (in.keyInventory) uiState.inventory = !uiState.inventory;
            if (in.keyChar) uiState.charSheet = !uiState.charSheet;
            if (in.keyStop) core.Attack(0);
            if (in.keyHand) core.SwapHands();
            if (in.escape) {
                if (core.dialog().open) core.DialogClose();
                else if (core.hasContainer()) core.CloseContainer();
                else if (uiState.inventory || uiState.charSheet) uiState.inventory = uiState.charSheet = false;
                else uiState.mode = CursorMode::Move;
            }
            if (in.mouseRightClick && !renderer->IsOverUi(in.mouseX, in.mouseY))   // Move <-> Examine; Attack -> Move
                uiState.mode = uiState.mode == CursorMode::Move ? CursorMode::Examine : CursorMode::Move;

            PickResult pr;
            const bool over = renderer->IsOverUi(in.mouseX, in.mouseY);
            const bool havePick = !over && renderer->Pick(in.mouseX, in.mouseY, pr);
            if (in.keyExamine && havePick) {
                if (pr.kind == PickResult::Critter) core.Examine(pr.critter, true);
                else if (pr.kind == PickResult::Item) core.Examine(pr.item, false);
            }
            if (in.mouseLeftPressed && havePick && core.hasMap()) {
                auto moveTo = [&]() {
                    if (pr.kind == PickResult::None || core.map().IsBlocked(pr.tile.x, pr.tile.y)) return;
                    core.RequestMoveTo(pr.fx, pr.fy, !in.shift);
                };
                const ClientItem* item = nullptr;
                if (pr.kind == PickResult::Item) {
                    auto it = core.items().find(pr.item);
                    if (it != core.items().end()) item = &it->second;
                }
                switch (ResolveCursor(core, uiState.mode, pr)) {
                    case CursorMode::Move:
                        moveTo();
                        break;
                    case CursorMode::Examine:
                        if (pr.kind == PickResult::Critter) core.Examine(pr.critter, true);
                        else if (item) core.Examine(item->id, false);
                        break;
                    case CursorMode::Attack:
                        if (pr.kind == PickResult::Critter && pr.critter != core.selfId()) core.Attack(pr.critter);
                        else moveTo();
                        break;
                    case CursorMode::Use:
                        if (item) core.Use(item->id);
                        break;
                    case CursorMode::Talk:
                        if (pr.critter != core.selfId()) core.Talk(pr.critter);
                        break;
                    case CursorMode::Pickup:
                        if (item) core.PickUp(item->id);
                        break;
                    case CursorMode::Skill:   // the skill on whatever was clicked: yourself, another critter or a map object; one use, then back to Move
                        if (pr.kind == PickResult::Critter) core.UseSkill(uiState.skill, pr.critter == core.selfId() ? 0 : 1, pr.critter == core.selfId() ? 0 : pr.critter);
                        else if (item) core.UseSkill(uiState.skill, 2, item->id);
                        else break;
                        uiState.mode = CursorMode::Move;
                        break;
                    default: break;
                }
            }
        }

        // ---- autowalk (testing): straight walks of ~10-14 tiles in turn, alternating walk and run ----
        if (autowalk && inGame && core.hasMap() && core.self() && now - lastAutoMove > 0.6) {
            const ClientCritter* me = core.self();
            const CritterPose p = core.Pose(*me, now);
            if (!p.moving && !core.IsBreak()) {
                static const int dx[8] = {1, 0, -1, 0, 1, -1, -1, 1}, dy[8] = {0, 1, 0, -1, 1, 1, -1, -1};
                for (int tries = 0; tries < 16; ++tries) {
                    const int k = (autoStep + tries) % 8;
                    const float dist = 8.0f + 2.0f * ((autoStep + tries) % 4);
                    const float tx = p.x + dx[k] * dist, ty = p.y + dy[k] * dist;
                    if (StraightWalkable(core.map(), p.x, p.y, tx, ty)) {
                        core.RequestMoveTo(tx, ty, (autoStep % 2) == 1);
                        log::Info("autowalk: from (%.1f,%.1f) to (%.1f,%.1f) %s", p.x, p.y, tx, ty, (autoStep % 2) ? "run" : "walk");
                        ++autoStep;
                        break;
                    }
                }
                lastAutoMove = now;
            }
        }

        // ---- scripted test actions ----
        if (inGame && inGameSince >= 0) {
            if (!sayList.empty() && now - inGameSince > 1.0) { core.SendChat(sayList.front()); sayList.erase(sayList.begin()); }
            const ClientCritter* me = core.self();
            if (me && (autoattack || autopickup) && now - lastAutoAct > 2.0 && now - inGameSince > 2.5) {
                lastAutoAct = now;
                const CritterPose mp = core.Pose(*me, now);
                if (autoattack) {
                    const ClientCritter* best = nullptr; float bd = 1e9f;
                    for (auto& [id, c] : core.critters()) {
                        if (id == core.selfId() || c.dead) continue;
                        const CritterPose p = core.Pose(c, now);
                        const float d = std::hypot(p.x - mp.x, p.y - mp.y);
                        if (d < bd) { bd = d; best = &c; }
                    }
                    if (best) { log::Info("autoattack: %s at %d,%d (%.1f tiles)", best->name.c_str(), best->pos.x, best->pos.y, bd); core.Attack(best->id); }
                }
                if (autopickup) {
                    const ClientItem* best = nullptr; float bd = 1e9f;
                    for (auto& [id, it] : core.items()) {
                        if (it.kind != gnet::ObjectKind::Ground) continue;
                        const float d = std::hypot(it.pos.x - mp.x, it.pos.y - mp.y);
                        if (d < bd) { bd = d; best = &it; }
                    }
                    if (best) { log::Info("autopickup: %s (%.1f tiles)", best->name.c_str(), bd); core.PickUp(best->id); }
                }
            }
        }

        // ---- --toggle-equip (testing): every 3 s alternately take off / put on the armor and the first weapon ----
        if (toggleEquip && inGame && now - inGameSince > 2.5 && now - lastToggle > 3.0 && !core.inventory().empty()) {
            lastToggle = now;
            const char* want = (toggleStep / 2) % 2 == 0 ? "armor" : "weapon";
            for (const gnet::InvEntry& e : core.inventory()) {
                const ProtoInfo* p = core.protos().ByPid(e.pid);
                if (p && p->cls == want) {
                    log::Info("toggle-equip: %s %s", e.equipped ? "unequip" : "equip", e.name.c_str());
                    core.Equip(e.id, !e.equipped);
                    break;
                }
            }
            ++toggleStep;
        }

        // ---- snapshot for the renderer ----
        SceneView scene;
        scene.now = now;
        scene.protos = &core.protos();
        scene.mapSerial = core.mapSerial();
        scene.map = core.hasMap() ? &core.map() : nullptr;
        scene.critters.reserve(core.critters().size());
        for (auto& [id, c] : core.critters()) {
            SceneCritter sc;
            sc.critter = &c;
            sc.pose = core.Pose(c, now);
            sc.x = sc.pose.x; sc.y = sc.pose.y;
            sc.isSelf = (id == core.selfId());
            if (sc.isSelf) {
                scene.cameraX = sc.x; scene.cameraY = sc.y; scene.hasSelf = true;
                if (trace.csv.is_open()) trace.csv << now << "," << sc.x << "," << sc.y << "," << sc.pose.moving << "," << sc.pose.speed << "\n";
                if (!traceFile.empty()) trace.samples.push_back({now, sc.x, sc.y, sc.pose.speed, sc.pose.moving});
            }
            scene.critters.push_back(sc);
        }
        scene.allowCameraKeys = !typing;
        for (auto& [id, it] : core.items()) scene.items.push_back(&it);

        UiView ui;
        ui.chat = &core.chat();
        ui.chatInputActive = typing;
        ui.chatInput = input;
        ui.now = now;
        ui.core = &core;
        ui.state = &uiState;
        ui.commands = &commands;
        switch (core.state()) {
            case ClientCore::State::Connecting: ui.status = "Connecting to " + host + ":" + std::to_string(port) + "..."; break;
            case ClientCore::State::LoggingIn: ui.status = "Logging in..."; break;
            case ClientCore::State::InGame: {
                const ClientCritter* me = core.self();
                ui.status = core.map().name + (me ? "  (" + std::to_string(me->pos.x) + "," + std::to_string(me->pos.y) + ")" : "");
                break;
            }
            default: ui.status = "Disconnected: " + core.failReason() + "  (server " + host + ":" + std::to_string(port) + ")"; break;
        }
        // loud message when the asset renderer is not in use
        if (!renderer->HasAssets()) {
            if (noAssets) ui.banner = "Flat placeholder view (--no-assets)";
            else if (ropt.gameDir.empty())
                ui.banner = "NO GAME DATA FOUND - showing the flat placeholder map\n"
                            "Put gamedir=<install dir> into client.cfg next to f3_client.exe, set F3_GAMEDIR, or run with --gamedir <dir>\n"
                            "(the install dir contains resource.rht and data\\; auto-detected: F:\\_work\\F3_Demo and ..\\F3_Demo)";
            else
                ui.banner = "GAME DATA UNUSABLE - showing the flat placeholder map\n" + ropt.gameDir + ": " + renderer->AssetError();
        } else if (core.hasMap() && !renderer->Is3D() && frame > 3) {
            ui.banner = "Map '" + core.map().name + "' has no geometry in the game data - showing the flat placeholder map";
        }

        renderer->BeginFrame();
        renderer->DrawScene(scene);
        renderer->DrawUi(ui);
        renderer->EndFrame();
        ++frame;

        // ---- UI commands produced while drawing the HUD ----
        for (const UiCommand& c : commands) {
            switch (c.type) {
                case UiCommand::SetMode: uiState.mode = c.a == 1 ? CursorMode::Examine : c.a == 2 ? CursorMode::Attack : CursorMode::Move; break;
                case UiCommand::ToggleInventory: uiState.inventory = !uiState.inventory; break;
                case UiCommand::ToggleChar: uiState.charSheet = !uiState.charSheet; break;
                case UiCommand::Equip: core.Equip(c.a, c.b != 0, c.c); break;
                case UiCommand::SwapHands: core.SwapHands(); break;
                case UiCommand::Drop: core.Drop(c.a); break;
                case UiCommand::Take: core.Take(c.a, c.b); break;
                case UiCommand::CloseContainer: core.CloseContainer(); break;
                case UiCommand::DialogChoose: core.DialogChoose(static_cast<int>(c.a)); break;
                case UiCommand::DialogClose: core.DialogClose(); break;
                case UiCommand::CharCreate: {
                    uint8_t sp[7];
                    for (int i = 0; i < 7; i++) sp[i] = static_cast<uint8_t>(uiState.draft.special[i]);
                    core.CharCreate(sp, static_cast<uint16_t>(uiState.draft.tagMask), static_cast<uint16_t>(uiState.draft.traitMask));
                    break;
                }
                case UiCommand::SpendSkill: core.SpendSkill(static_cast<int>(c.a), static_cast<int>(c.b)); break;
                case UiCommand::UseSkill: core.UseSkill(static_cast<int>(c.a)); break;
                case UiCommand::SelectSkill:
                    uiState.skill = static_cast<int>(std::min<uint32_t>(c.a, kSkillCount - 1));
                    uiState.mode = CursorMode::Skill;
                    uiState.charSheet = false;
                    break;
                case UiCommand::Quit: quit = true; break;
            }
        }
        commands.clear();

        if (runSeconds > 0 && inGameSince >= 0 && now - inGameSince >= runSeconds) quit = true;
        if (core.state() == ClientCore::State::Failed && runSeconds > 0 && now > 8) quit = true;
    }
    if (!trace.samples.empty()) SummariseTrace(trace.samples);
    core.Disconnect();
    renderer->Shutdown();
    return 0;
}
