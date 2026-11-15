// raylib renderer: original-asset 3D scene (map, skinned critters, items) with a flat placeholder fallback, plus the
// immediate-mode HUD. Only file (with the asset helpers) that includes raylib.
#include <raylib.h>
#include <raymath.h>
#include <rlgl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include "CritterRig.h"
#include "GameAssets.h"
#include "IRenderer.h"
#include "MapView.h"
#include "CharPreview.h"
#include "OrigUi.h"
#include "common/log.h"

namespace f3 {

namespace {
Color Unpack(uint32_t rgba) {
    return Color{static_cast<unsigned char>(rgba >> 24), static_cast<unsigned char>(rgba >> 16),
                 static_cast<unsigned char>(rgba >> 8), static_cast<unsigned char>(rgba)};
}
float YawFromTileDir(float dx, float dy) { return std::atan2(-dx, dy); }  // model faces original +z (see CritterRig)
float WrapPi(float a) {
    while (a > PI) a -= 2 * PI;
    while (a < -PI) a += 2 * PI;
    return a;
}
}  // namespace

// Orbit camera like the original (sys.ini [Camera]): fixed long distance, narrow FOV (6..15 degrees),
// azimuth/elevation orbit around a ground target.
struct OrbitCam {
    Vector3 target{0, 0, 0};
    float azimuth = 45, elevation = 45;   // degrees
    float fov = 12;                       // vertical, degrees
    float distance = 350;
    Camera3D Build() const {
        float az = azimuth * DEG2RAD, el = elevation * DEG2RAD;
        Camera3D c{};
        c.target = target;
        c.position = Vector3Add(target, Vector3{distance * std::cos(el) * std::sin(az), distance * std::sin(el),
                                                distance * std::cos(el) * std::cos(az)});
        c.up = Vector3{0, 1, 0};
        c.fovy = fov;
        c.projection = CAMERA_PERSPECTIVE;
        return c;
    }
};

class RaylibRenderer final : public IRenderer {
public:
    explicit RaylibRenderer(const RendererOptions& opt) : opt_(opt) {}

    bool Init(int w, int h, const char* title) override {
        SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT | FLAG_MSAA_4X_HINT);
        InitWindow(w, h, title);
        SetTargetFPS(60);
        SetExitKey(0);  // Esc is ours
        if (!IsWindowReady()) return false;
        if (!opt_.gameDir.empty()) {
            assets_ = std::make_unique<GameAssets>();
            std::string err;
            if (!assets_->Open(opt_.gameDir, &err) || !assets_->InitGpu()) {
                assetErr_ = err.empty() ? "cannot initialise the asset renderer" : err;
                log::Warn("asset renderer disabled (%s); using placeholder", assetErr_.c_str());
                assets_.reset();
            } else {
                log::Info("game data: %s (%zu resources)", opt_.gameDir.c_str(), assets_->data().resources().size());
                if (opt_.modelScale > 0) MapView::modelScale = opt_.modelScale;
                dopt_.clipY = opt_.clipY;
                dopt_.entities = !opt_.noEntities;
                rigs_ = std::make_unique<RigLibrary>(*assets_);
                if (!opt_.noOrigUi) {
                    std::string uerr;
                    if (!orig_.Load(assets_->data(), opt_.gameDir, &uerr)) log::Warn("original UI disabled (%s), using the raylib HUD", uerr.c_str());
                }
            }
        }
        if (const char* e = std::getenv("F3_HELD"))   // dev: held weapon transform "scale,rx,ry,rz,ox,oy,oz" (degrees, b3d units)
            std::sscanf(e, "%f,%f,%f,%f,%f,%f,%f", &kHeldModelScale, &kHeldRotX, &kHeldRotY, &kHeldRotZ, &kHeldOffX, &kHeldOffY, &kHeldOffZ);
        showGrid_ = opt_.showGrid;
        startTime_ = GetTime();
        return true;
    }
    void Shutdown() override {
        visuals_.clear();   // GL meshes before the context goes away
        orig_.Unload();
        rigs_.reset();
        map_.Unload();
        if (assets_) assets_->Unload();
        assets_.reset();
        if (IsWindowReady()) CloseWindow();
    }
    bool ShouldClose() override { return WindowShouldClose() || (shotDone_ && opt_.exitAfterScreenshot); }

    bool HasAssets() const override { return assets_ != nullptr; }
    bool Is3D() const override { return active3d_; }
    std::string AssetInfo() const override { return map_.loaded() ? map_.info() : std::string(); }
    std::string AssetError() const override { return assetErr_; }
    bool LoadOriginalMap(const std::string& name, std::string* err) override {
        if (!assets_) { if (err) *err = "no game data"; return false; }
        auto t0 = GetTime();
        visuals_.clear();
        doorState_.clear();
        if (opt_.network) map_.SetNetworkMode(opt_.protos);
        map_.SetRigs(rigs_.get());
        if (!map_.Load(*assets_, name, err)) return false;
        log::Info("loaded %s (%.2fs); %zu textures, %zu models cached", map_.info().c_str(), GetTime() - t0,
                  assets_->textureCount(), assets_->modelCount());
        loadedName_ = name;
        // initial camera
        cam_ = OrbitCam{};
        cam_.target = map_.startTarget();
        if (opt_.network) { cam_.fov = 6.0f; cam_.elevation = 50.0f; }
        if (opt_.hasTarget) cam_.target = Vector3{opt_.targetX, 0, -opt_.targetZ};
        if (opt_.hasCam) { cam_.azimuth = opt_.camAzimuth; cam_.elevation = opt_.camElevation; cam_.fov = opt_.camFov; }
        panOffset_ = Vector3{0, 0, 0};
        return true;
    }

    FrameInput PollInput() override {
        FrameInput in;
        Vector2 m = GetMousePosition();
        in.mouseX = static_cast<int>(m.x);
        in.mouseY = static_cast<int>(m.y);
        in.mouseLeftPressed = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
        in.wheel = GetMouseWheelMove();
        for (int cp = GetCharPressed(); cp != 0; cp = GetCharPressed()) {
            int n = 0;
            const char* u = CodepointToUTF8(cp, &n);
            in.typed.append(u, n);
        }
        in.enter = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER);
        in.backspace = IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE);
        in.escape = IsKeyPressed(KEY_ESCAPE);
        in.shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
        in.ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
        in.keyAttack = IsKeyPressed(KEY_A);
        in.keyInventory = IsKeyPressed(KEY_I);
        in.keyChar = IsKeyPressed(KEY_C);
        in.keyExamine = IsKeyPressed(KEY_X);
        in.keyStop = IsKeyPressed(KEY_TAB);
        in.keyHand = IsKeyPressed(KEY_B);
        in.keySkill = IsKeyPressed(KEY_S);
        // right button: a click (toggle cursor mode) unless the mouse was dragged (camera orbit)
        if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) { rmbDown_ = true; rmbTravel_ = 0; }
        if (rmbDown_) {
            Vector2 d = GetMouseDelta();
            rmbTravel_ += std::fabs(d.x) + std::fabs(d.y);
            if (IsMouseButtonReleased(MOUSE_BUTTON_RIGHT)) {
                rmbDown_ = false;
                if (rmbTravel_ < 6.0f) in.mouseRightClick = true;
            }
        }
        if (in.wheel != 0 && !IsOverUi(in.mouseX, in.mouseY)) {
            if (map_.loaded()) cam_.fov = std::clamp(cam_.fov * (in.wheel > 0 ? 0.9f : 1.1f), 1.5f, 40.0f);
            else zoom_ = std::clamp(zoom_ * (in.wheel > 0 ? 1.1f : 0.9f), 0.4f, 3.0f);
        }
        return in;
    }

    void BeginFrame() override { BeginDrawing(); ClearBackground(Color{18, 20, 24, 255}); }
    void WindowSize(int& w, int& h) const override { w = GetScreenWidth(); h = GetScreenHeight(); }
    void DrawLogin(const LoginView& v) override {
        const LoginLayout L(GetScreenWidth(), GetScreenHeight());
        const Color panel{30, 34, 40, 255}, edge{90, 100, 110, 255}, txt{225, 225, 220, 255}, dim{150, 155, 160, 255}, hot{70, 110, 60, 255}, norm{50, 58, 66, 255};
        DrawRectangle(L.px, L.py, L.pw, L.ph, panel);
        DrawRectangleLines(L.px, L.py, L.pw, L.ph, edge);
        DrawText("F3Online", L.px + 40, L.py + 24, 34, txt);
        DrawText(v.server.c_str(), L.px + 40, L.py + 66, 14, dim);
        auto field = [&](const char* label, int x, int y, const std::string& text, bool masked, bool focus) {
            DrawText(label, x, y - 18, 14, dim);
            DrawRectangle(x, y, L.fieldW, L.fieldH, Color{16, 18, 22, 255});
            DrawRectangleLines(x, y, L.fieldW, L.fieldH, focus ? Color{140, 200, 120, 255} : edge);
            std::string shown = masked ? std::string(text.size(), '*') : text;
            if (focus && std::fmod(GetTime(), 1.0) < 0.5) shown += "_";
            DrawText(shown.c_str(), x + 8, y + 8, 20, txt);
        };
        field("Name", L.nameX, L.nameY, v.name, false, v.focus == 0);
        field("Password", L.passX, L.passY, v.password, true, v.focus == 1);
        auto button = [&](const char* label, int x, int idx) {
            DrawRectangle(x, L.btnY, L.btnW, L.btnH, v.hover == idx && !v.busy ? hot : norm);
            DrawRectangleLines(x, L.btnY, L.btnW, L.btnH, edge);
            const int tw = MeasureText(label, 20);
            DrawText(label, x + (L.btnW - tw) / 2, L.btnY + 10, 20, v.busy ? dim : txt);
        };
        button("Login", L.loginX, 0);
        button("Register", L.regX, 1);
        if (!v.error.empty()) DrawText(v.error.c_str(), L.px + 40, L.btnY + L.btnH + 14, 16, Color{240, 110, 90, 255});
        else if (!v.status.empty()) DrawText(v.status.c_str(), L.px + 40, L.btnY + L.btnH + 14, 16, dim);
        DrawText("Tab: next field   Enter: log in   (password never stored locally)", L.px + 40, L.py + L.ph - 24, 11, dim);
    }
    void EndFrame() override {
        ++frame_;
        const double el = GetTime() - startTime_;
        const bool wantShot = (!opt_.screenshotPath.empty() && !shotDone_ && frame_ >= opt_.screenshotFrame) ||
                              (!opt_.shotPrefix.empty() && nextShot_ < opt_.shotTimes.size() && el >= opt_.shotTimes[nextShot_]);
        if (wantShot) rlDrawRenderBatchActive();  // flush the queued 2D (HUD) draws so the screenshot contains them
        if (!opt_.screenshotPath.empty() && !shotDone_ && frame_ >= opt_.screenshotFrame) {
            Image img = LoadImageFromScreen();
            ExportImage(img, opt_.screenshotPath.c_str());
            UnloadImage(img);
            log::Info("screenshot written: %s", opt_.screenshotPath.c_str());
            shotDone_ = true;
        }
        if (!opt_.shotPrefix.empty() && nextShot_ < opt_.shotTimes.size() && el >= opt_.shotTimes[nextShot_]) {
            char name[512];
            std::snprintf(name, sizeof name, "%s_%02zu.png", opt_.shotPrefix.c_str(), nextShot_);
            Image img = LoadImageFromScreen();
            ExportImage(img, name);
            UnloadImage(img);
            log::Info("shot %zu: %s t=%.3f frame=%d", nextShot_, name, el, frame_);
            ++nextShot_;
            if (nextShot_ >= opt_.shotTimes.size() && opt_.exitAfterScreenshot) shotDone_ = true;
        }
        EndDrawing();
    }

    void DrawScene(const SceneView& s) override {
        active3d_ = false;
        sceneNow_ = s.now;
        if (assets_) {
            if (!s.viewer && s.map && (s.map->name != attemptedName_ || s.mapSerial != attemptedSerial_)) {
                attemptedName_ = s.map->name;
                attemptedSerial_ = s.mapSerial;
                std::string err;
                if (!LoadOriginalMap(s.map->name, &err)) {
                    loadedName_.clear();
                    mapErr_ = "map '" + s.map->name + "' not in game data (" + err + ")";
                    log::Warn("%s: placeholder view", mapErr_.c_str());
                } else mapErr_.clear();
            }
            if (map_.loaded() && (s.viewer || (s.map && s.map->name == attemptedName_ && loadedName_ == attemptedName_))) {
                DrawScene3D(s);
                active3d_ = true;
                return;
            }
        }
        DrawScenePlaceholder(s);
    }

    void DrawScenePlaceholder(const SceneView& s) {
        camX_ = s.cameraX; camY_ = s.cameraY;
        const float ts = TileSize();
        const int sw = GetScreenWidth(), sh = GetScreenHeight();
        if (!s.map) return;
        const ClientMap& m = *s.map;
        phCritters_.clear(); phItems_.clear();
        for (const SceneCritter& c : s.critters) phCritters_.push_back({c.critter->id, c.x, c.y, c.isSelf});
        for (const ClientItem* i : s.items) phItems_.push_back({i->id, i->pos});

        int x0 = std::max(0, static_cast<int>(std::floor(camX_ - sw / (2 * ts))) - 1);
        int x1 = std::min(m.width - 1, static_cast<int>(std::ceil(camX_ + sw / (2 * ts))) + 1);
        int y0 = std::max(0, static_cast<int>(std::floor(camY_ - sh / (2 * ts))) - 1);
        int y1 = std::min(m.height - 1, static_cast<int>(std::ceil(camY_ + sh / (2 * ts))) + 1);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                Vector2 p = TileToScreen(static_cast<float>(x), static_cast<float>(y));
                Color c = m.IsBlocked(x, y) ? Color{70, 60, 55, 255}
                                            : (((x + y) & 1) ? Color{44, 52, 40, 255} : Color{48, 57, 44, 255});
                DrawRectangle(static_cast<int>(p.x), static_cast<int>(p.y), static_cast<int>(std::ceil(ts)),
                              static_cast<int>(std::ceil(ts)), c);
            }

        for (const ClientItem* it : s.items) {
            Vector2 p = TileToScreen(static_cast<float>(it->pos.x), static_cast<float>(it->pos.y));
            float cx = p.x + ts / 2, cy = p.y + ts / 2, r = ts * 0.18f;
            Color col = it->kind == gnet::ObjectKind::Ground ? Color{240, 210, 70, 255} : Color{120, 190, 240, 255};
            DrawPoly(Vector2{cx, cy}, 4, r * 1.6f, 45.0f, col);
        }

        for (const SceneCritter& sc : s.critters) {
            Vector2 p = TileToScreen(sc.x, sc.y);
            float pad = ts * 0.15f;
            Rectangle r{p.x + pad, p.y + pad, ts - 2 * pad, ts - 2 * pad};
            Color col = sc.critter->dead ? Color{90, 90, 90, 255} : Unpack(sc.critter->color);
            DrawRectangleRec(r, col);
            DrawRectangleLinesEx(r, sc.isSelf ? 2.5f : 1.0f, sc.isSelf ? WHITE : Color{0, 0, 0, 160});
            // facing marker
            int d = static_cast<int>(sc.critter->dir);
            DrawCircle(static_cast<int>(p.x + ts / 2 + kDirDx[d] * ts * 0.28f),
                       static_cast<int>(p.y + ts / 2 + kDirDy[d] * ts * 0.28f), std::max(2.0f, ts * 0.07f), BLACK);
            const std::string& n = sc.critter->name;
            int tw = MeasureText(n.c_str(), 12);
            DrawText(n.c_str(), static_cast<int>(p.x + ts / 2 - tw / 2), static_cast<int>(p.y - 14),
                     12, sc.critter->kind == CritterKind::Npc ? Color{255, 220, 140, 255} : RAYWHITE);
        }

        // hover marker
        TilePos hov;
        Vector2 mp = GetMousePosition();
        if (ScreenToTile(static_cast<int>(mp.x), static_cast<int>(mp.y), hov) && !IsOverUi((int)mp.x, (int)mp.y) &&
            m.width > 0 && !m.IsBlocked(hov.x, hov.y)) {
            Vector2 p = TileToScreen(hov.x, hov.y);
            DrawRectangleLinesEx(Rectangle{p.x, p.y, ts, ts}, 2, Color{255, 255, 255, 120});
        }
    }

    // ------------------------------------------------------------------------------------------------ HUD
    void DrawUi(const UiView& ui) override {
        const int sw = GetScreenWidth(), sh = GetScreenHeight();
        uiRects_.clear();
        // character creation happens before the world: the creation screen replaces everything (no map, no HUD)
        if (ui.core && ui.state && ui.commands && ui.core->state() == ClientCore::State::InGame && ui.core->hasSheet() && !ui.core->sheet().created &&
            orig_.HasCharScreen()) {
            orig_.DrawCharScreen(ui, true, uiRects_);
            return;
        }
        DrawText(ui.status.c_str(), 10, 8, 16, RAYWHITE);
        if (active3d_) {
            char b[320];
            std::snprintf(b, sizeof b, "%d fps | az %.0f el %.0f fov %.1f | %s", GetFPS(), cam_.azimuth, cam_.elevation,
                          cam_.fov, map_.info().c_str());
            DrawText(b, 10, 28, 12, Color{200, 200, 200, 255});
            if (!ui.core && hoverValid_) {
                std::snprintf(b, sizeof b, "cursor: x %.1f y %.1f z %.1f  cell (%d, %d)  [original axes]", hover_.x, hover_.y,
                              -hover_.z, static_cast<int>(std::floor(hover_.x * 2)), static_cast<int>(std::floor(-hover_.z * 2)));
                DrawText(b, 10, 44, 12, Color{200, 200, 140, 255});
            }
            if (!ui.core)
                DrawText("RMB orbit | MMB/WASD pan | wheel zoom | Q/E rotate | R/F tilt | PgUp/PgDn roof cut | G grid | H props | L lightmap | F12 shot",
                         10, 60, 12, Color{150, 150, 150, 255});
        } else if (ui.core) {
            DrawText("LMB act | RMB cycle cursor mode | Enter chat | wheel zoom", 10, 28, 12, Color{180, 180, 180, 255});
        }

        // overhead labels and floating combat text
        if (active3d_ && ui.core) {
            DrawLabels(ui);
        }

        // chat (with the original-art action bar the chat lives inside the bar's message area instead)
        Rectangle panel = ChatPanel();
        const bool chatInBar = orig_.ok() && ui.core && ui.state && ui.commands && ui.core->state() == ClientCore::State::InGame;
        if (!chatInBar) DrawRectangleRec(panel, Color{0, 0, 0, 140});
        if (ui.chat && !chatInBar) {
            // word-wrap every message to the panel width, show the newest lines that fit
            const int lineH = 16, maxLines = static_cast<int>(panel.height / lineH);
            const int maxW = static_cast<int>(panel.width) - 12;
            std::vector<std::pair<std::string, Color>> wrapped;
            const int count = static_cast<int>(ui.chat->size());
            for (int i = std::max(0, count - 40); i < count; ++i) {
                const ChatLine& l = (*ui.chat)[i];
                std::string line = l.name.empty() ? l.text : l.name + ": " + l.text;
                Color c = l.channel == ChatChannel::System ? Color{140, 220, 255, 255} : RAYWHITE;
                std::string cur;
                size_t pos = 0;
                while (pos < line.size()) {
                    size_t sp = line.find(' ', pos);
                    std::string word = line.substr(pos, sp == std::string::npos ? std::string::npos : sp - pos + 1);
                    pos = sp == std::string::npos ? line.size() : sp + 1;
                    while (MeasureText(word.c_str(), 14) > maxW && word.size() > 1) {  // very long word: hard split
                        size_t n = word.size();
                        while (n > 1 && MeasureText(word.substr(0, n).c_str(), 14) > maxW) --n;
                        if (!cur.empty()) { wrapped.emplace_back(cur, c); cur.clear(); }
                        wrapped.emplace_back(word.substr(0, n), c);
                        word = word.substr(n);
                    }
                    if (!cur.empty() && MeasureText((cur + word).c_str(), 14) > maxW) {
                        wrapped.emplace_back(cur, c);
                        cur.clear();
                    }
                    cur += word;
                }
                if (!cur.empty()) wrapped.emplace_back(cur, c);
            }
            int first = std::max(0, static_cast<int>(wrapped.size()) - maxLines);
            int y = static_cast<int>(panel.y) + 2;
            for (size_t i = first; i < wrapped.size(); ++i, y += lineH)
                DrawText(wrapped[i].first.c_str(), static_cast<int>(panel.x) + 6, y, 14, wrapped[i].second);
        }
        Rectangle in{panel.x, panel.y + panel.height + 4, panel.width, 24};
        if (!chatInBar) DrawRectangleRec(in, ui.chatInputActive ? Color{30, 30, 60, 220} : Color{0, 0, 0, 90});
        std::string text = ui.chatInputActive ? ui.chatInput + ((static_cast<int>(ui.now * 2) & 1) ? "_" : "") : "Press Enter to chat";
        if (!chatInBar) DrawText(text.c_str(), static_cast<int>(in.x) + 6, static_cast<int>(in.y) + 4, 16,
                 ui.chatInputActive ? WHITE : Color{150, 150, 150, 255});

        if (ui.core && ui.state && ui.commands && ui.core->state() == ClientCore::State::InGame) {
            DrawHud(ui);
            DrawCursorInfo(ui);
        }

        if (!ui.banner.empty()) {
            const int fs = 16;
            // wrap on '\n'
            std::vector<std::string> lines;
            size_t p = 0;
            while (p <= ui.banner.size()) {
                size_t e = ui.banner.find('\n', p);
                if (e == std::string::npos) e = ui.banner.size();
                lines.push_back(ui.banner.substr(p, e - p));
                p = e + 1;
            }
            int bw = 0;
            for (auto& l : lines) bw = std::max(bw, MeasureText(l.c_str(), fs));
            Rectangle r{(sw - bw) / 2.0f - 14, 52, static_cast<float>(bw + 28), static_cast<float>(lines.size() * (fs + 4) + 16)};
            DrawRectangleRec(r, Color{90, 20, 20, 230});
            DrawRectangleLinesEx(r, 2, Color{255, 200, 60, 255});
            float y = r.y + 8;
            for (size_t i = 0; i < lines.size(); ++i, y += fs + 4)
                DrawText(lines[i].c_str(), static_cast<int>(r.x) + 14, static_cast<int>(y), fs, i == 0 ? Color{255, 220, 90, 255} : RAYWHITE);
        }
        (void)sh;
    }

    bool ScreenToTile(int sx, int sy, TilePos& out) const override {
        if (active3d_) {
            Vector3 hit;
            if (!PickAt(sx, sy, hit)) return false;
            int tx, ty;
            if (!map_.WorldToTile(hit, lastTileW_, lastTileH_, tx, ty)) return false;
            out.x = static_cast<int16_t>(tx); out.y = static_cast<int16_t>(ty);
            return true;
        }
        const float ts = TileSize();
        float fx = camX_ + (sx - GetScreenWidth() / 2.0f) / ts;
        float fy = camY_ + (sy - GetScreenHeight() / 2.0f) / ts;
        // tile (x,y) occupies [x - 0.5, x + 0.5) around its centre in camera units
        out.x = static_cast<int16_t>(std::floor(fx + 0.5f));
        out.y = static_cast<int16_t>(std::floor(fy + 0.5f));
        return true;
    }

    bool Pick(int sx, int sy, PickResult& out) const override {
        out = PickResult{};
        const Vector2 mp{static_cast<float>(sx), static_cast<float>(sy)};
        if (active3d_) {
            float best = 1e30f;
            for (const PickRect& r : pickRects_) {
                if (!CheckCollisionPointRec(mp, r.rect) || r.depth >= best) continue;
                best = r.depth;
                out.kind = r.isCritter ? PickResult::Critter : PickResult::Item;
                out.critter = r.isCritter ? r.id : 0;
                out.item = r.isCritter ? 0 : r.id;
            }
            Vector3 hit;
            if (PickAt(sx, sy, hit)) {
                int tx, ty;
                const float cw = lastTileW_ > 0 && map_.gridW() > 0 ? map_.gridW() * 0.5f / lastTileW_ : 0.5f;
                const float ch = lastTileH_ > 0 && map_.gridH() > 0 ? map_.gridH() * 0.5f / lastTileH_ : 0.5f;
                out.fx = (hit.x - map_.gridOriginX()) / cw - 0.5f;
                out.fy = (-hit.z - map_.gridOriginZ()) / ch - 0.5f;
                if (map_.WorldToTile(hit, lastTileW_, lastTileH_, tx, ty)) {
                    out.tile = TilePos{static_cast<int16_t>(tx), static_cast<int16_t>(ty)};
                    if (out.kind == PickResult::None) out.kind = PickResult::Ground;
                }
            }
            return out.kind != PickResult::None;
        }
        TilePos t;
        if (!ScreenToTile(sx, sy, t)) return false;
        out.tile = t; out.fx = t.x; out.fy = t.y; out.kind = PickResult::Ground;
        for (const auto& c : phCritters_)
            if (static_cast<int>(std::lround(c.x)) == t.x && static_cast<int>(std::lround(c.y)) == t.y && !c.self) {
                out.kind = PickResult::Critter; out.critter = c.id; return true;
            }
        for (const auto& i : phItems_)
            if (i.pos == t) { out.kind = PickResult::Item; out.item = i.id; return true; }
        return true;
    }

    bool IsOverUi(int sx, int sy) const override {
        Rectangle p = ChatPanel();
        p.height += 30;
        const Vector2 v{static_cast<float>(sx), static_cast<float>(sy)};
        if (!orig_.ok() && CheckCollisionPointRec(v, p)) return true;
        for (const Rectangle& r : uiRects_) if (CheckCollisionPointRec(v, r)) return true;
        return false;
    }

private:
    struct Label { Vector3 pos; std::string text; Color color; CritterId id; int hp, maxHp; bool dead; };
    struct PickRect { Rectangle rect; bool isCritter; uint32_t id; float depth; };

    struct AnimState {
        ClipKind cur = ClipKind::Idle, prev = ClipKind::Idle;
        const Clip* curClip = nullptr; const Clip* prevClip = nullptr;   // the clips actually playing (weapon set / stance dependent)
        int setKey = -1;
        double curStart = 0, prevStart = 0, blendStart = -10;
        float yaw = 0;
        bool yawInit = false;
        double lastNow = 0;
        double attackSeen = -1, hitSeen = -1;
    };
    struct Visual {
        std::string model;
        std::unique_ptr<SkinnedInstance> inst;
        const ModelAsset* staticModel = nullptr;   // models without skin data
        AnimState anim;
        const Texture2D* skin = nullptr;
    };

    // Puts a renderer-space position on the walkable ground surface (continuous height field of the level geometry).
    Vector3 OnGround(Vector3 w, float lift = 0.0f) const {
        w.y = map_.GroundHeight(w.x, w.z) + lift;
        return w;
    }

    bool PickAt(int sx, int sy, Vector3& hit) const {
        Ray r = GetScreenToWorldRay(Vector2{static_cast<float>(sx), static_cast<float>(sy)}, lastCam_);
        return map_.PickGround(r, hit);
    }

    void HandleCameraInput(const SceneView& s, float dt) {
        if (IsKeyPressed(KEY_F12)) TakeScreenshot("f3_screenshot.png");
        if (!s.allowCameraKeys) return;
        Vector2 md = GetMouseDelta();
        if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT) && rmbTravel_ >= 6.0f) {
            cam_.azimuth -= md.x * 0.3f;
            cam_.elevation = std::clamp(cam_.elevation + md.y * 0.3f, 15.0f, 89.0f);
        }
        const float az = cam_.azimuth * DEG2RAD;
        const Vector3 fwd{-std::sin(az), 0, -std::cos(az)}, right{std::cos(az), 0, -std::sin(az)};
        Vector3 move{0, 0, 0};
        const float speed = cam_.fov * 3.5f * (IsKeyDown(KEY_LEFT_SHIFT) ? 3.0f : 1.0f);
        const bool wasd = s.viewer;   // in the game A is the attack cursor; WASD pans only in the map viewer
        if ((wasd && IsKeyDown(KEY_W)) || IsKeyDown(KEY_UP)) move = Vector3Add(move, fwd);
        if ((wasd && IsKeyDown(KEY_S)) || IsKeyDown(KEY_DOWN)) move = Vector3Subtract(move, fwd);
        if ((wasd && IsKeyDown(KEY_D)) || IsKeyDown(KEY_RIGHT)) move = Vector3Add(move, right);
        if ((wasd && IsKeyDown(KEY_A)) || IsKeyDown(KEY_LEFT)) move = Vector3Subtract(move, right);
        move = Vector3Scale(move, speed * dt);
        if (IsMouseButtonDown(MOUSE_BUTTON_MIDDLE)) {
            float k = cam_.fov * 0.0105f;
            move = Vector3Add(move, Vector3Add(Vector3Scale(right, -md.x * k), Vector3Scale(fwd, md.y * k * 1.2f)));
        }
        if (follow_) panOffset_ = Vector3Add(panOffset_, move);
        else cam_.target = Vector3Add(cam_.target, move);
        if (IsKeyDown(KEY_Q)) cam_.azimuth += 90 * dt;
        if (IsKeyDown(KEY_E)) cam_.azimuth -= 90 * dt;
        if (IsKeyDown(KEY_R)) cam_.elevation = std::min(89.0f, cam_.elevation + 45 * dt);
        if (IsKeyDown(KEY_F)) cam_.elevation = std::max(15.0f, cam_.elevation - 45 * dt);
        if (IsKeyPressed(KEY_SPACE)) panOffset_ = Vector3{0, 0, 0};
        if (IsKeyPressed(KEY_G)) showGrid_ = !showGrid_;
        if (IsKeyPressed(KEY_H)) dopt_.entities = !dopt_.entities;
        if (IsKeyPressed(KEY_L)) dopt_.lmGain = dopt_.lmGain > 1.5f ? 1.0f : 2.0f;
        if (!orig_.ChatOwnsPaging() && IsKeyPressed(KEY_PAGE_UP)) dopt_.clipY = std::min(dopt_.clipY, 60.0f) + 1.0f;
        if (!orig_.ChatOwnsPaging() && IsKeyPressed(KEY_PAGE_DOWN)) dopt_.clipY = std::min(dopt_.clipY, 60.0f) - 1.0f;
        if (IsKeyPressed(KEY_END)) dopt_.clipY = 1e9f;
    }

    // Chooses and advances the animation of a critter; returns the yaw to draw it with.
    void UpdateAnim(Visual& v, const SceneCritter& sc, const SceneView& s, int tw, int th) {
        const ClientCritter& c = *sc.critter;
        AnimState& a = v.anim;
        SkinModel& m = *const_cast<SkinModel*>(v.inst->model());
        const double now = s.now;
        const float dt = a.lastNow > 0 ? static_cast<float>(std::min(0.1, now - a.lastNow)) : 0.016f;
        a.lastNow = now;

        // weapon class and stance choose the clip set (WeaponAnim.h); combat stance for a while after fighting
        const ProtoInfo* wp = (c.hasApp && c.appWeapon != 0xFFFF && s.protos) ? s.protos->ByPid(c.appWeapon) : nullptr;
        const WeaponAnim wa = ClassifyWeapon(wp);
        const bool combat = (c.attackT >= 0 && now - c.attackT < 8.0) || (c.hitT >= 0 && now - c.hitT < 8.0);
        auto clipOf = [&](ClipKind k) -> const Clip* { return rigs_->GetFor(m, k, wa, combat); };
        ClipKind want = ClipKind::Idle;
        double start = now;
        const Clip* equipClip = c.equipT >= 0 ? clipOf(ClipKind::Equip) : nullptr;
        // an attack lasts as long as the server action: the clip is stretched / squeezed to it (see timeOf)
        auto attackRate = [&](const Clip* cl) { return c.attackDur > 0.05 && cl ? static_cast<float>(std::clamp(cl->duration / c.attackDur, 0.5, 2.5)) : 1.0f; };
        if (c.dead) {
            want = ClipKind::Death;
            start = c.deathT >= 0 ? c.deathT : now - 100.0;
        } else if (c.attackT >= 0 && clipOf(ClipKind::Attack) && now - c.attackT < clipOf(ClipKind::Attack)->duration / attackRate(clipOf(ClipKind::Attack))) {
            want = ClipKind::Attack; start = c.attackT;
        } else if (c.hitT >= 0 && clipOf(ClipKind::Hit) && now - c.hitT < clipOf(ClipKind::Hit)->duration) {
            want = ClipKind::Hit; start = c.hitT;
        } else if (equipClip && now - c.equipT < equipClip->duration) {
            want = ClipKind::Equip; start = c.equipT;
        } else if (sc.pose.moving) {
            want = (c.run || sc.pose.speed > 13.5f) ? ClipKind::Run : ClipKind::Walk;
        }
        const bool oneShot = want == ClipKind::Attack || want == ClipKind::Hit || want == ClipKind::Death || want == ClipKind::Equip;
        const int setKey = wa.key() * 2 + (combat ? 1 : 0);
        const bool changed = want != a.cur || setKey != a.setKey || (oneShot && std::fabs(start - a.curStart) > 0.01);
        if (changed) {
            a.prev = a.cur; a.prevStart = a.curStart; a.prevClip = a.curClip;
            a.cur = want; a.setKey = setKey;
            a.curStart = oneShot ? start : now - (static_cast<double>(c.id % 7)) * 0.13;
            a.blendStart = now;
        }
        auto timeOf = [&](const Clip* cl, ClipKind k, double st) -> float {
            if (!cl) return 0;
            float rate = 1.0f;
            if (k == ClipKind::Walk) rate = std::clamp(sc.pose.speed / 9.6f, 0.5f, 1.8f);   // clip stride ~4.8 units/s = 9.6 cells/s
            else if (k == ClipKind::Run) rate = std::clamp(sc.pose.speed / 19.5f, 0.6f, 1.6f);   // ~9.8 units/s
            else if (k == ClipKind::Attack) rate = attackRate(cl);
            float t = static_cast<float>((now - st)) * rate;
            const bool loop = k == ClipKind::Idle || k == ClipKind::Walk || k == ClipKind::Run;
            if (loop) t = std::fmod(std::max(0.0f, t), cl->duration);
            else t = std::clamp(t, 0.0f, cl->duration);
            return t;
        };
        const Clip* ca = clipOf(a.cur);
        const Clip* cb = a.blendStart > -5 ? a.prevClip : nullptr;
        if (!ca) ca = clipOf(ClipKind::Idle);
        a.curClip = ca;
        const float k = static_cast<float>(std::clamp((now - a.blendStart) / 0.18, 0.0, 1.0));
        v.inst->Pose(ca, timeOf(ca, a.cur, a.curStart), k < 1.0f ? cb : nullptr, timeOf(cb, a.prev, a.prevStart), k);

        // facing
        float target;
        if (sc.pose.moving) target = YawFromTileDir(sc.pose.hx, sc.pose.hy);
        else {
            target = YawFromTileDir(static_cast<float>(kDirDx[static_cast<int>(c.dir)]), static_cast<float>(kDirDy[static_cast<int>(c.dir)]));
            if (!c.dead && c.attackT >= 0 && now - c.attackT < 1.2) {
                for (const SceneCritter& o : s.critters)
                    if (o.critter->id == c.attackTarget) {
                        const float dx = o.x - sc.x, dy = o.y - sc.y;
                        if (std::fabs(dx) + std::fabs(dy) > 0.01f) target = YawFromTileDir(dx, dy);
                    }
            }
        }
        if (!a.yawInit) { a.yaw = target; a.yawInit = true; }
        else {
            const float d = WrapPi(target - a.yaw);
            const float maxStep = 12.0f * dt;
            a.yaw = WrapPi(a.yaw + std::clamp(d, -maxStep, maxStep));
        }
        (void)tw; (void)th;
    }

    Rectangle ProjectRect(const Vector3& foot, float height, float width) const {
        Vector2 f = GetWorldToScreen(foot, lastCam_);
        Vector2 h = GetWorldToScreen(Vector3{foot.x, foot.y + height, foot.z}, lastCam_);
        float hpx = std::fabs(f.y - h.y);
        float wpx = std::max(hpx * width, 14.0f);
        return Rectangle{f.x - wpx / 2, std::min(f.y, h.y), wpx, std::max(hpx, 14.0f)};
    }

    void DrawScene3D(const SceneView& s) {
        const ClientMap* nm = s.viewer ? nullptr : s.map;
        const int tw = nm ? nm->width : map_.gridW(), th = nm ? nm->height : map_.gridH();
        lastTileW_ = tw; lastTileH_ = th;
        follow_ = s.hasSelf && nm;
        const float dt = GetFrameTime();
        HandleCameraInput(s, dt);
        if (follow_) {
            Vector3 w = OnGround(map_.TileToWorld(s.cameraX, s.cameraY, tw, th));
            cam_.target = Vector3Add(w, panOffset_);   // exact follow: the critter pose is already smooth
        }
        lastCam_ = cam_.Build();
        labels_.clear();
        std::vector<PickRect> prevRects;
        prevRects.swap(pickRects_);
        // hover from last frame's rectangles
        hoverCrit_ = 0; hoverItem_ = 0;
        {
            const Vector2 mp = GetMousePosition();
            float best = 1e30f;
            for (const PickRect& r : prevRects)
                if (CheckCollisionPointRec(mp, r.rect) && r.depth < best && !IsOverUi((int)mp.x, (int)mp.y)) {
                    best = r.depth;
                    hoverCrit_ = r.isCritter ? r.id : 0;
                    hoverItem_ = r.isCritter ? 0 : r.id;
                }
        }

        rlSetClipPlanes(30.0, 1500.0);
        BeginMode3D(lastCam_);
        map_.Update(GetFrameTime());
        map_.Draw(*assets_, dopt_);

        // hover / picking
        Vector2 mp = GetMousePosition();
        hoverValid_ = !IsOverUi(static_cast<int>(mp.x), static_cast<int>(mp.y)) &&
                      PickAt(static_cast<int>(mp.x), static_cast<int>(mp.y), hover_);
        const float cw = tw > 0 && map_.gridW() > 0 ? map_.gridW() * 0.5f / tw : 0.5f;
        const float ch = th > 0 && map_.gridH() > 0 ? map_.gridH() * 0.5f / th : 0.5f;
        if (showGrid_) {   // 'G': walk grid around the hovered cell, or around the camera target when the mouse is not over the ground
            int gx, gy;
            const bool have = map_.WorldToTile(cam_.target, tw, th, gx, gy);
            if (have) {
                rlDrawRenderBatchActive(); rlDisableDepthTest();   // terrain is bumpy: draw the overlay on top
                const int R = 18;
                for (int dy = -R; dy <= R; ++dy)
                    for (int dx = -R; dx <= R; ++dx) {
                        int cx = gx + dx, cy = gy + dy;
                        if (cx < 0 || cy < 0 || (tw > 0 && cx >= tw) || (th > 0 && cy >= th)) continue;
                        bool blocked = nm ? nm->IsBlocked(cx, cy) : map_.Blocked(cx, cy);
                        Vector3 c = OnGround(map_.TileToWorld(static_cast<float>(cx), static_cast<float>(cy), tw, th), 0.04f);
                        DrawPlane(c, Vector2{cw * 0.94f, ch * 0.94f}, blocked ? Color{220, 40, 40, 110} : Color{60, 220, 90, 40});
                    }
                rlDrawRenderBatchActive(); rlEnableDepthTest();
            }
        }
        if (hoverValid_) {
            int hx, hy;
            if (map_.WorldToTile(hover_, tw, th, hx, hy)) {
                if (!hoverCrit_ && !hoverItem_) {
                    Vector3 c = OnGround(map_.TileToWorld(static_cast<float>(hx), static_cast<float>(hy), tw, th), 0.06f);
                    DrawPlane(c, Vector2{cw, ch}, Color{255, 255, 255, 120});
                }
            }
        }
        if (s.viewer && map_.hasEmap()) {
            for (const gd::EntryPoint& ep : map_.emap().entryPoints)
                for (size_t k = 0; k < ep.pos.size(); ++k) {
                    Vector3 p{ep.pos[k][0], ep.pos[k][1], -ep.pos[k][2]};
                    DrawCylinder(p, 0.15f, 0.15f, 2.2f, 8, k == 0 ? Color{40, 220, 255, 255} : Color{40, 120, 200, 255});
                }
        }

        // network critters and items
        if (nm) {
            std::unordered_set<CritterId> seen;
            for (const SceneCritter& sc : s.critters) DrawCritter(s, sc, tw, th, seen);
            for (auto it = visuals_.begin(); it != visuals_.end();) {
                if (!seen.count(it->first)) it = visuals_.erase(it); else ++it;
            }
            for (const ClientItem* it : s.items) DrawItem(s, *it, tw, th);
        }
        EndMode3D();
    }

    void DrawCritter(const SceneView& s, const SceneCritter& sc, int tw, int th, std::unordered_set<CritterId>& seen) {
        const ClientCritter& c = *sc.critter;
        const ProtoInfo* proto = s.protos ? s.protos->ByPid(static_cast<uint32_t>(std::max(0, c.pid))) : nullptr;
        if (c.pid < 0) proto = nullptr;
        if (proto && proto->isStatic) return;  // barrels, windows: baked into the map
        std::string model = proto && !proto->model.empty() ? proto->model : "HumMNor";
        Vector3 w = OnGround(map_.TileToWorld(sc.x, sc.y, tw, th));
        seen.insert(c.id);

        Visual& v = visuals_[c.id];
        Appearance app;
        app.model = model;
        if (proto) {
            app.skin = proto->skin;
            for (const WornPart& w : proto->worn) {
                // players start from the base body: the prototype's built-in outfit (prison suit) is not worn
                static const char* kOutfit[] = {"Bod", "Fee", "Bac", "Han", "Sho", "Hea"};
                bool outfit = false;
                if (c.kind == CritterKind::Player) for (const char* o : kOutfit) if (w.part.compare(0, 3, o) == 0) outfit = true;
                if (!outfit) app.worn.emplace_back(w.part, w.tex);
            }
        }
        if (c.hasApp && s.protos) {   // equipment sent by the server (armor, helmet): its parts replace parts of the same slot
            for (uint16_t apid : {c.appBody, c.appHead}) {
                const ProtoInfo* ap = apid == 0xFFFF ? nullptr : s.protos->ByPid(apid);
                if (!ap) continue;
                for (const WornPart& w : ap->armorParts) {
                    app.worn.erase(std::remove_if(app.worn.begin(), app.worn.end(), [&](const std::pair<std::string, std::string>& o) { return o.first.compare(0, 3, w.part, 0, 3) == 0; }), app.worn.end());
                    app.worn.emplace_back(w.part, w.tex);
                }
            }
        }
        const std::string appKey = app.Key();
        if (v.model != appKey) {
            v = Visual{};
            v.model = appKey;
            if (SkinModel* sm = rigs_->Model(app)) v.inst = std::make_unique<SkinnedInstance>(sm);
            else v.staticModel = assets_->Model(model);
            const std::string& skinName = proto ? proto->skin : std::string();
            if (!skinName.empty()) v.skin = assets_->Texture(skinName);
        }
        const float k = MapView::modelScale;
        float yaw = 0;
        float height = 1.9f;
        if (v.inst) {
            UpdateAnim(v, sc, s, tw, th);
            yaw = v.anim.yaw;
            height = v.inst->model()->height * k;
        } else if (v.staticModel) {
            yaw = YawFromTileDir(static_cast<float>(kDirDx[static_cast<int>(c.dir)]), static_cast<float>(kDirDy[static_cast<int>(c.dir)]));
        }
        Matrix m = MatrixMultiply(MatrixMultiply(MatrixScale(k, k, k), MatrixRotateY(yaw)), MatrixTranslate(w.x, w.y, w.z));
        Color tint = WHITE;
        if (v.inst) MapView::DrawModel(*assets_, v.inst->asset(), m, tint, v.skin, dopt_, true);
        else if (v.staticModel) MapView::DrawModel(*assets_, *v.staticModel, m, tint, v.skin, dopt_, true);
        else DrawCylinder(w, 1.0f, 1.0f, 6.0f, 10, Unpack(c.color));
        if (v.inst && !c.dead) DrawHeldWeapon(s, c, v, m);

        const bool hovered = hoverCrit_ == c.id;
        if (hovered || sc.isSelf) {
            Color rc = sc.isSelf ? Color{80, 220, 120, 200} : (c.kind == CritterKind::Npc ? Color{255, 90, 70, 220} : Color{255, 220, 80, 220});
            if (!sc.isSelf || hovered) DrawCircle3D(Vector3{w.x, w.y + 0.05f, w.z}, hovered ? 1.7f : 1.4f, Vector3{1, 0, 0}, 90.0f, rc);
        }
        pickRects_.push_back(PickRect{ProjectRect(w, height, 0.45f), true, c.id, Vector3Distance(w, lastCam_.position)});
        labels_.push_back({Vector3{w.x, w.y + height + 0.7f, w.z}, c.name,
                           c.kind == CritterKind::Npc ? Color{255, 220, 140, 255} : RAYWHITE, c.id, c.hp, c.maxHp, c.dead});
        critterWorld_[c.id] = Vector3{w.x, w.y + height * 0.8f, w.z};
    }

    // The active weapon (appearance block) is drawn on the "R Handle_1" bone of the right hand. The bone matrix is in Granny space
    // (row vectors, left-handed, skeleton units); the renderer flips z, so the basis is reflected like the skinned vertices
    // (see SkinnedInstance::Pose). kHeldScale / kHeldRot are tuned by eye against the weapon b3d (docs/formats/models.md).
    void DrawHeldWeapon(const SceneView& s, const ClientCritter& c, Visual& v, const Matrix& body) {
        if (!c.hasApp || c.appWeapon == 0xFFFF || !s.protos) return;
        const ProtoInfo* wp = s.protos->ByPid(c.appWeapon);
        // The hand model (item record field heldModel, "WP_*") is authored in hand space at world scale; the ground model ("GD_WP_*",
        // ProtoInfo::model) is a different, much larger mesh and is only a fallback (tuned by eye with kHeldScale).
        const ModelAsset* wm = wp && !wp->heldModel.empty() ? assets_->Model(wp->heldModel) : nullptr;
        const bool held = wm != nullptr;
        if (!wm) wm = wp && !wp->model.empty() ? assets_->Model(wp->model) : nullptr;
        float h[16];
        if (!wm || !v.inst->BoneWorld("R Handle_1", h)) return;
        const float ss = v.inst->model()->skelScale;
        Matrix g{};
        g.m0 = h[0];  g.m4 = h[4];  g.m8 = -h[8];   g.m12 = h[12] * ss;
        g.m1 = h[1];  g.m5 = h[5];  g.m9 = -h[9];   g.m13 = h[13] * ss;
        g.m2 = -h[2]; g.m6 = -h[6]; g.m10 = h[10];  g.m14 = -h[14] * ss;
        g.m15 = 1.0f;
        const float ks = (held ? kHeldModelScale : kHeldScale) * wm->unitScale;
        Matrix local = MatrixMultiply(MatrixScale(ks, ks, ks), MatrixRotateXYZ(Vector3{kHeldRotX * DEG2RAD, kHeldRotY * DEG2RAD, kHeldRotZ * DEG2RAD}));
        local = MatrixMultiply(local, MatrixTranslate(kHeldOffX, kHeldOffY, kHeldOffZ));
        const Matrix wmat = MatrixMultiply(MatrixMultiply(local, g), body);
        MapView::DrawModel(*assets_, *wm, wmat, WHITE, nullptr, dopt_, true);
    }
    float kHeldModelScale = 1.0f;
    float kHeldScale = 0.5f, kHeldRotX = 0, kHeldRotY = 0, kHeldRotZ = 0, kHeldOffX = 0, kHeldOffY = 0, kHeldOffZ = 0;

    void DrawItem(const SceneView& s, const ClientItem& it, int tw, int th) {
        const ProtoInfo* proto = s.protos ? s.protos->ByPid(it.pid) : nullptr;
        Vector3 w = OnGround(map_.TileToWorld(static_cast<float>(it.pos.x), static_cast<float>(it.pos.y), tw, th));
        bool drawn = false;
        float height = 2.0f;
        if (it.kind == gnet::ObjectKind::Ground) {
            const ModelAsset* ma = proto && !proto->model.empty() ? assets_->Model(proto->model) : nullptr;
            const float k = MapView::modelScale;
            if (ma) {
                const float ks = k * ma->unitScale;
                Matrix m = MatrixMultiply(MatrixScale(ks, ks, ks), MatrixTranslate(w.x, w.y + 0.08f, w.z));
                MapView::DrawModel(*assets_, *ma, m, WHITE, nullptr, dopt_, true);
                drawn = true;
                height = std::max(1.2f, (ma->bmax.y - ma->bmin.y) * ks);
            }
            if (!drawn) DrawCube(Vector3{w.x, w.y + 0.5f, w.z}, 1.0f, 1.0f, 1.0f, Color{240, 210, 70, 255});
        } else {
            // doors, containers and usables are baked into the map; a door has no open pose, so hide it while open
            auto d = doorState_.find(it.id);
            const bool wasOpen = d != doorState_.end() && d->second;
            if (it.kind == gnet::ObjectKind::Door && wasOpen != it.open) { map_.SetDoorOpen(w, it.open); doorState_[it.id] = it.open; }
            else if (d == doorState_.end()) doorState_[it.id] = false;
            height = 4.5f;
        }
        const bool hovered = hoverItem_ == it.id;
        if (hovered) {
            Color rc = it.kind == gnet::ObjectKind::Ground ? Color{255, 220, 80, 230} : Color{120, 200, 255, 230};
            DrawCircle3D(Vector3{w.x, w.y + 0.05f, w.z}, 1.4f, Vector3{1, 0, 0}, 90.0f, rc);
        }
        Rectangle r = ProjectRect(Vector3{w.x, w.y, w.z}, height, 1.0f);
        r.width = std::max(r.width, 26.0f);
        r.height = std::max(r.height, 26.0f);
        pickRects_.push_back(PickRect{r, false, it.id, Vector3Distance(w, lastCam_.position) + 0.5f});
        if (hovered || (it.kind == gnet::ObjectKind::Ground)) {
            labels_.push_back({Vector3{w.x, w.y + 1.6f, w.z}, hovered ? it.name : std::string(), Color{255, 230, 140, 255}, 0, -1, -1, false});
        }
    }

    void DrawLabels(const UiView& ui) {
        for (const Label& l : labels_) {
            if (l.text.empty()) continue;
            Vector2 sp = GetWorldToScreen(l.pos, lastCam_);
            int tw = MeasureText(l.text.c_str(), 12);
            DrawText(l.text.c_str(), static_cast<int>(sp.x) - tw / 2 + 1, static_cast<int>(sp.y) - 5, 12, Color{0, 0, 0, 200});
            DrawText(l.text.c_str(), static_cast<int>(sp.x) - tw / 2, static_cast<int>(sp.y) - 6, 12, l.color);
            if (l.id && l.hp >= 0 && l.maxHp > 0) {
                float f = std::clamp(static_cast<float>(l.hp) / l.maxHp, 0.0f, 1.0f);
                Rectangle b{sp.x - 20, sp.y + 8, 40, 4};
                DrawRectangleRec(b, Color{0, 0, 0, 180});
                DrawRectangleRec(Rectangle{b.x, b.y, b.width * f, b.height}, l.dead ? Color{120, 120, 120, 255} : Color{210, 40, 40, 255});
            }
        }
        for (const Floater& f : ui.core->floaters()) {
            auto it = critterWorld_.find(f.target);
            if (it == critterWorld_.end()) continue;
            const float age = static_cast<float>(ui.now - f.time);
            Vector2 sp = GetWorldToScreen(it->second, lastCam_);
            sp.y -= 30 + age * 40;
            Color col = f.kind == 1 ? Color{200, 200, 200, 255} : (f.kind == 2 ? Color{255, 200, 40, 255} : (f.kind == 3 ? Color{255, 60, 60, 255} : Color{255, 110, 90, 255}));
            col.a = static_cast<unsigned char>(255 * std::clamp(1.6f - age, 0.0f, 1.0f));
            int fs = f.kind == 2 ? 22 : 18;
            int tw = MeasureText(f.text.c_str(), fs);
            DrawText(f.text.c_str(), static_cast<int>(sp.x) - tw / 2 + 1, static_cast<int>(sp.y) + 1, fs, Color{0, 0, 0, col.a});
            DrawText(f.text.c_str(), static_cast<int>(sp.x) - tw / 2, static_cast<int>(sp.y), fs, col);
        }
    }

    // ---------------------------------------------------------------- HUD widgets
    bool Button(Rectangle r, const char* text, bool active, bool enabled = true) {
        Vector2 mp = GetMousePosition();
        const bool hot = enabled && CheckCollisionPointRec(mp, r);
        Color bg = !enabled ? Color{40, 40, 44, 200} : active ? Color{70, 110, 70, 230} : hot ? Color{70, 70, 90, 230} : Color{36, 38, 48, 220};
        DrawRectangleRec(r, bg);
        DrawRectangleLinesEx(r, 1, active ? Color{140, 230, 140, 255} : Color{90, 90, 110, 255});
        int fs = 14;
        int tw = MeasureText(text, fs);
        DrawText(text, static_cast<int>(r.x + (r.width - tw) / 2), static_cast<int>(r.y + (r.height - fs) / 2), fs, enabled ? RAYWHITE : Color{120, 120, 120, 255});
        return hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    }

    static Color ModeColor(CursorMode m) {
        switch (m) {
            case CursorMode::Move: return Color{120, 220, 120, 255};
            case CursorMode::Examine: return Color{190, 190, 255, 255};
            case CursorMode::Attack: return Color{255, 90, 80, 255};
            case CursorMode::Use: return Color{120, 190, 255, 255};
            case CursorMode::Talk: return Color{255, 220, 120, 255};
            default: return Color{230, 160, 255, 255};
        }
    }

    void DrawHud(const UiView& ui) {
        const ClientCore& core = *ui.core;
        UiState& st = *ui.state;
        const int sw = GetScreenWidth(), sh = GetScreenHeight();
        if (orig_.ok()) {   // original art: action bar + inventory window; the other windows stay raylib for now
            orig_.DrawActionBar(ui, uiRects_);
            if (st.inventory && !(core.hasContainer() && orig_.HasLoot())) orig_.DrawInventory(ui, uiRects_);
            if (core.hasContainer() && orig_.HasLoot()) orig_.DrawLoot(ui, uiRects_);
            orig_.DrawOverlay();
            if (core.hasContainer() && !orig_.HasLoot()) DrawContainer(ui);
            if (core.dialog().open) DrawDialog(ui);
            if (st.charSheet) {
                if (orig_.HasCharScreen()) orig_.DrawCharScreen(ui, false, uiRects_);
                else DrawCharSheet(ui);
            }
            return;
        }
        Rectangle hud{sw - 566.0f, sh - 124.0f, 558, 116};
        DrawRectangleRec(hud, Color{10, 12, 16, 210});
        DrawRectangleLinesEx(hud, 1, Color{80, 84, 100, 255});
        uiRects_.push_back(hud);

        // mode buttons
        // only Move, Examine and Attack are manual; Pickup / Use / Talk follow the mouse
        const char* names[] = {"Move", "Examine", "A Attack"};
        for (int i = 0; i < 3; ++i) {
            Rectangle b{hud.x + 8 + i * 74.0f, hud.y + 8, 70, 24};
            if (Button(b, names[i], st.mode == static_cast<CursorMode>(i)))
                ui.commands->push_back(UiCommand{UiCommand::SetMode, static_cast<uint32_t>(i), 0});
        }
        DrawText("auto: pickup / use / talk", static_cast<int>(hud.x) + 8 + 3 * 74, static_cast<int>(hud.y) + 14, 10, Color{140, 140, 150, 255});
        if (Button(Rectangle{hud.x + 420, hud.y + 8, 56, 24}, "I Inv", st.inventory)) ui.commands->push_back(UiCommand{UiCommand::ToggleInventory, 0, 0});
        if (Button(Rectangle{hud.x + 484, hud.y + 8, 56, 24}, "C Char", st.charSheet)) ui.commands->push_back(UiCommand{UiCommand::ToggleChar, 0, 0});

        // HP
        const ClientCritter* me = core.self();
        int hp = core.hasStats() ? core.stats().hp : (me ? me->hp : -1);
        int maxHp = core.hasStats() ? core.stats().maxHp : (me ? me->maxHp : -1);
        Rectangle hpb{hud.x + 8, hud.y + 40, 250, 20};
        DrawRectangleRec(hpb, Color{40, 10, 10, 255});
        if (hp >= 0 && maxHp > 0) {
            float f = std::clamp(static_cast<float>(hp) / maxHp, 0.0f, 1.0f);
            DrawRectangleRec(Rectangle{hpb.x, hpb.y, hpb.width * f, hpb.height}, f > 0.35f ? Color{190, 30, 30, 255} : Color{240, 60, 30, 255});
        }
        DrawRectangleLinesEx(hpb, 1, Color{150, 70, 70, 255});
        char buf[96];
        std::snprintf(buf, sizeof buf, "HP %d / %d", std::max(hp, 0), std::max(maxHp, 0));
        DrawText(buf, static_cast<int>(hpb.x) + 6, static_cast<int>(hpb.y) + 3, 14, RAYWHITE);

        // AP: one cell per AP, partial fill for the regenerating one
        const float maxAp = core.waitKnown() ? core.MaxAp() : (core.hasStats() ? core.stats().sequence : 0.0f);
        const float ap = core.CurrentAp();
        Rectangle apb{hud.x + 270, hud.y + 40, 280, 20};
        DrawRectangleRec(apb, Color{8, 18, 12, 255});
        if (maxAp > 0.5f) {
            const int n = std::max(1, static_cast<int>(std::lround(maxAp)));
            const float cwid = apb.width / n;
            for (int i = 0; i < n; ++i) {
                float f = std::clamp(ap - i, 0.0f, 1.0f);
                Rectangle cell{apb.x + i * cwid + 1, apb.y + 2, cwid - 2, apb.height - 4};
                DrawRectangleRec(cell, Color{20, 34, 24, 255});
                DrawRectangleRec(Rectangle{cell.x, cell.y, cell.width * f, cell.height}, Color{70, 220, 110, 255});
            }
        }
        DrawRectangleLinesEx(apb, 1, Color{60, 130, 80, 255});
        std::snprintf(buf, sizeof buf, "AP %.1f / %.0f", ap, maxAp);
        DrawText(buf, static_cast<int>(apb.x) + 4, static_cast<int>(apb.y) - 14, 12, Color{150, 230, 170, 255});

        // break time (FOnline IntBreakTime): bar + red clock while actions are locked
        Rectangle bt{hud.x + 36, hud.y + 72, 514, 18};
        const bool locked = core.IsBreak();
        DrawRectangleRec(bt, Color{24, 8, 8, 255});
        if (locked) {
            const double rem = core.BreakRemaining(), tot = std::max(0.05, core.BreakTotal());
            const float f = static_cast<float>(std::clamp(rem / tot, 0.0, 1.0));
            DrawRectangleRec(Rectangle{bt.x, bt.y, bt.width * f, bt.height}, Color{220, 30, 30, 255});
            std::snprintf(buf, sizeof buf, "BREAK  %.1fs", rem);
            DrawText(buf, static_cast<int>(bt.x) + 8, static_cast<int>(bt.y) + 2, 14, WHITE);
            // red clock icon
            const Vector2 c{hud.x + 20, hud.y + 81};
            const float pulse = 0.8f + 0.2f * std::sin(static_cast<float>(ui.now) * 10.0f);
            DrawCircleV(c, 11, Color{static_cast<unsigned char>(230 * pulse), 25, 25, 255});
            DrawCircleLines(static_cast<int>(c.x), static_cast<int>(c.y), 11, WHITE);
            const float ang = static_cast<float>(ui.now) * 4.0f;
            DrawLineEx(c, Vector2{c.x + 7 * std::sin(ang), c.y - 7 * std::cos(ang)}, 2, WHITE);
            DrawLineEx(c, Vector2{c.x, c.y - 5}, 2, WHITE);
        } else {
            const Vector2 c{hud.x + 20, hud.y + 81};
            DrawCircleV(c, 11, Color{30, 90, 40, 255});
            DrawCircleLines(static_cast<int>(c.x), static_cast<int>(c.y), 11, Color{120, 200, 130, 255});
            DrawText(core.InBattle() ? "IN BATTLE - ready" : "ready", static_cast<int>(bt.x) + 8, static_cast<int>(bt.y) + 2, 14, Color{110, 170, 120, 255});
        }
        DrawRectangleLinesEx(bt, 1, locked ? Color{200, 60, 60, 255} : Color{80, 60, 60, 255});
        if (core.waitKnown() && core.lastResult() != gnet::ActionResult::Ok && core.lastResult() != gnet::ActionResult::None &&
            ui.now - core.lastResultTime() < 1.5) {
            const bool isMove = core.lastAction() == static_cast<uint8_t>(GMsgRt::C2S_MoveToF) || core.lastAction() == static_cast<uint8_t>(Msg::C2S_MoveTo);
            const char* why = core.lastResult() == gnet::ActionResult::Busy ? (isMove ? "busy - walking as soon as ready" : "busy") : core.lastResult() == gnet::ActionResult::NoAp ? "not enough AP" :
                              core.lastResult() == gnet::ActionResult::Dead ? "dead" : "invalid";
            DrawText(why, static_cast<int>(hud.x) + 8, static_cast<int>(hud.y) - 20, 16, Color{255, 120, 100, 255});
        }
        std::snprintf(buf, sizeof buf, "%s", core.serverTiming() ? "real-time" : "legacy server");
        DrawText(buf, static_cast<int>(hud.x + hud.width) - MeasureText(buf, 10) - 6, static_cast<int>(hud.y + hud.height) - 14, 10, Color{110, 110, 120, 255});

        if (st.inventory) DrawInventory(ui);
        if (core.hasContainer()) DrawContainer(ui);
        if (core.dialog().open) DrawDialog(ui);
        if (st.charSheet) DrawCharSheet(ui);
    }

    void DrawInventory(const UiView& ui) {
        const ClientCore& core = *ui.core;
        const int sw = GetScreenWidth(), sh = GetScreenHeight();
        Rectangle p{sw - 340.0f, 64, 332, static_cast<float>(sh - 64 - 140)};
        DrawRectangleRec(p, Color{10, 12, 16, 225});
        DrawRectangleLinesEx(p, 1, Color{80, 84, 100, 255});
        uiRects_.push_back(p);
        DrawText("Inventory", static_cast<int>(p.x) + 8, static_cast<int>(p.y) + 6, 16, RAYWHITE);
        if (core.hasStats()) {
            char b[64];
            std::snprintf(b, sizeof b, "%u / %u lb", core.stats().carried, core.stats().carryWeight);
            DrawText(b, static_cast<int>(p.x + p.width) - MeasureText(b, 12) - 8, static_cast<int>(p.y) + 9, 12, Color{180, 180, 190, 255});
        }
        float y = p.y + 30;
        for (const gnet::InvEntry& e : core.inventory()) {
            if (y + 26 > p.y + p.height) break;
            Rectangle row{p.x + 6, y, p.width - 12, 24};
            DrawRectangleRec(row, e.equipped ? Color{40, 60, 40, 220} : Color{24, 26, 34, 220});
            std::string label = e.name + (e.count > 1 ? " x" + std::to_string(e.count) : "");
            DrawText(label.c_str(), static_cast<int>(row.x) + 6, static_cast<int>(row.y) + 5, 14, e.equipped ? Color{170, 240, 170, 255} : RAYWHITE);
            if (Button(Rectangle{row.x + row.width - 128, row.y + 2, 60, 20}, e.equipped ? "Unequip" : "Equip", false))
                ui.commands->push_back(UiCommand{UiCommand::Equip, e.id, e.equipped ? 0u : 1u});
            if (Button(Rectangle{row.x + row.width - 62, row.y + 2, 56, 20}, "Drop", false))
                ui.commands->push_back(UiCommand{UiCommand::Drop, e.id, 0});
            y += 27;
        }
        if (core.inventory().empty()) DrawText("(empty)", static_cast<int>(p.x) + 10, static_cast<int>(y) + 4, 14, Color{140, 140, 150, 255});
    }

    // Conversation window: NPC text (word wrapped) and one button per reply (answers go back as DialogChoose commands).
    void DrawDialog(const UiView& ui) {
        const ClientCore::DialogState& d = ui.core->dialog();
        const int sw = GetScreenWidth(), sh = GetScreenHeight();
        const float w = std::min(680.0f, sw - 20.0f);
        const int fs = 16;
        std::vector<std::string> lines;
        {   // greedy word wrap
            std::string cur, word;
            auto flush = [&]() { if (!cur.empty()) lines.push_back(cur); cur.clear(); };
            std::string t = d.text + " ";
            for (char ch : t) {
                if (ch == '\n') { cur += (cur.empty() ? "" : " ") + word; word.clear(); flush(); continue; }
                if (ch != ' ') { word.push_back(ch); continue; }
                std::string tryLine = cur.empty() ? word : cur + " " + word;
                if (!cur.empty() && MeasureText(tryLine.c_str(), fs) > w - 24) { flush(); cur = word; } else cur = tryLine;
                word.clear();
            }
            flush();
        }
        const float textH = 28 + static_cast<float>(lines.size()) * (fs + 4);
        const float h = textH + 12 + static_cast<float>(d.replies.size()) * 28 + 10;
        Rectangle p{(sw - w) / 2, std::max(40.0f, sh - h - 150.0f), w, h};
        DrawRectangleRec(p, Color{12, 12, 18, 240});
        DrawRectangleLinesEx(p, 1, Color{200, 170, 90, 255});
        uiRects_.push_back(p);
        DrawText(d.npcName.c_str(), static_cast<int>(p.x) + 10, static_cast<int>(p.y) + 6, 14, Color{255, 220, 120, 255});
        float y = p.y + 26;
        for (auto& l : lines) { DrawText(l.c_str(), static_cast<int>(p.x) + 12, static_cast<int>(y), fs, RAYWHITE); y += fs + 4; }
        y = p.y + textH + 8;
        for (size_t i = 0; i < d.replies.size(); ++i) {
            std::string label = std::to_string(i + 1) + ". " + d.replies[i];
            while (label.size() > 8 && MeasureText(label.c_str(), 14) > w - 28) label = label.substr(0, label.size() - 4) + "...";
            if (Button(Rectangle{p.x + 8, y, p.width - 16, 24}, label.c_str(), false))
                ui.commands->push_back(UiCommand{UiCommand::DialogChoose, static_cast<uint32_t>(i), 0});
            y += 28;
        }
    }

    void DrawContainer(const UiView& ui) {
        const ClientCore& core = *ui.core;
        const int sw = GetScreenWidth();
        Rectangle p{sw / 2.0f - 170, 90, 340, 300};
        DrawRectangleRec(p, Color{14, 14, 20, 235});
        DrawRectangleLinesEx(p, 1, Color{120, 140, 200, 255});
        uiRects_.push_back(p);
        DrawText("Container", static_cast<int>(p.x) + 8, static_cast<int>(p.y) + 6, 16, RAYWHITE);
        if (Button(Rectangle{p.x + p.width - 70, p.y + 4, 62, 20}, "Close", false)) ui.commands->push_back(UiCommand{UiCommand::CloseContainer, 0, 0});
        if (Button(Rectangle{p.x + p.width - 150, p.y + 4, 74, 20}, "Take all", false, !core.container().empty()))
            ui.commands->push_back(UiCommand{UiCommand::Take, core.containerOwner(), 0});
        float y = p.y + 30;
        for (const gnet::InvEntry& e : core.container()) {
            if (y + 26 > p.y + p.height) break;
            Rectangle row{p.x + 6, y, p.width - 12, 24};
            DrawRectangleRec(row, Color{26, 28, 38, 220});
            std::string label = e.name + (e.count > 1 ? " x" + std::to_string(e.count) : "");
            DrawText(label.c_str(), static_cast<int>(row.x) + 6, static_cast<int>(row.y) + 5, 14, RAYWHITE);
            if (Button(Rectangle{row.x + row.width - 56, row.y + 2, 50, 20}, "Take", false))
                ui.commands->push_back(UiCommand{UiCommand::Take, core.containerOwner(), e.id});
            y += 27;
        }
        if (core.container().empty()) DrawText("(empty)", static_cast<int>(p.x) + 10, static_cast<int>(y) + 4, 14, Color{140, 140, 150, 255});
    }

    void DrawCharSheet(const UiView& ui) {
        const ClientCore& core = *ui.core;
        UiState& st = *ui.state;
        const bool creating = core.hasSheet() && !core.sheet().created;
        Rectangle p{10, 64, 600, creating ? 560.0f : 492.0f};
        DrawRectangleRec(p, Color{10, 12, 16, 235});
        DrawRectangleLinesEx(p, 1, Color{80, 84, 100, 255});
        uiRects_.push_back(p);
        const int px = static_cast<int>(p.x), py = static_cast<int>(p.y);
        DrawText(creating ? "Create your character" : "Character", px + 10, py + 8, 18, RAYWHITE);
        if (!core.hasStats() || !core.hasSheet()) { DrawText("(no data)", px + 10, py + 36, 14, GRAY); return; }
        const gnet::Stats& s = core.stats();
        const gnet::CharSheet& sh = core.sheet();
        static const char* spNames[7] = {"Strength", "Perception", "Endurance", "Charisma", "Intelligence", "Agility", "Luck"};
        const Color txt{220, 220, 230, 255}, dim{150, 155, 165, 255}, gold{210, 200, 120, 255};
        CharDraft& d = st.draft;
        if (creating && !d.init) { for (int i = 0; i < 7; ++i) d.special[i] = s.special[i]; d.tagMask = 0; d.traitMask = 0; d.init = true; }
        if (!creating) d.init = false;
        int used = 0;
        if (creating) for (int i = 0; i < 7; ++i) used += d.special[i];
        const int left = sh.creationTotal - used;
        char b[128];

        // ---- SPECIAL
        int y = py + 40;
        DrawText("SPECIAL", px + 10, y, 14, gold);
        y += 22;
        for (int i = 0; i < 7; ++i, y += 24) {
            DrawText(spNames[i], px + 10, y + 3, 14, txt);
            const int v = creating ? d.special[i] : s.special[i];
            std::snprintf(b, sizeof b, "%d", v);
            DrawText(b, px + 128, y + 3, 14, txt);
            if (creating) {
                if (Button(Rectangle{static_cast<float>(px + 152), static_cast<float>(y), 22, 20}, "-", false, v > 1)) d.special[i] = v - 1;
                if (Button(Rectangle{static_cast<float>(px + 178), static_cast<float>(y), 22, 20}, "+", false, v < 10 && left > 0)) d.special[i] = v + 1;
            }
        }
        if (creating) {
            std::snprintf(b, sizeof b, "Points left: %d", left);
            DrawText(b, px + 10, y + 2, 14, left == 0 ? Color{140, 230, 140, 255} : Color{240, 180, 100, 255});
            y += 22;
        }
        // ---- derived values
        y += 8;
        DrawText("Derived", px + 10, y, 14, gold);
        y += 20;
        auto line = [&](const char* fmt, int a, int b2 = 0) { std::snprintf(b, sizeof b, fmt, a, b2); DrawText(b, px + 10, y, 14, txt); y += 18; };
        if (creating) {   // preview from the draft, same formulas as the server defaults (the server is authoritative)
            line("Hit points %d", 15 + d.special[0] + 2 * d.special[2]);
            line("Action points %d", 5 + d.special[5] / 2);
            line("Carry weight %d", 25 + 25 * d.special[0]);
            line("Armor class %d", d.special[5]);
        } else {
            line("Hit points %d / %d", s.hp, s.maxHp);
            line("Action points %d", sh.maxAp);
            line("Armor class %d", s.armorClass);
            line("Sequence %d", s.sequence);
            line("Carry weight %d / %d", s.carried, s.carryWeight);
            line("Melee damage +%d", s.meleeBonus);
            line("Healing rate %d", sh.healRate);
            line("Critical chance %d%%", sh.critChance);
        }
        if (!creating) {
            y += 6;
            std::snprintf(b, sizeof b, "Level %d", sh.level);
            DrawText(b, px + 10, y, 16, gold); y += 22;
            if (sh.xpNext) std::snprintf(b, sizeof b, "XP %u / %u", sh.xp, sh.xpNext); else std::snprintf(b, sizeof b, "XP %u (max level)", sh.xp);
            DrawText(b, px + 10, y, 14, txt); y += 18;
            std::snprintf(b, sizeof b, "Skill points %d   Perk points %d", sh.skillPoints, sh.perkPoints);
            DrawText(b, px + 10, y, 14, sh.skillPoints ? Color{140, 230, 140, 255} : txt);
        }

        // ---- skills
        const int sx = px + 260;
        int sy = py + 40;
        int tags = 0;
        if (creating) for (int i = 0; i < kSkillCount; ++i) if (d.tagMask & (1u << i)) ++tags;
        DrawText(creating ? "Skills (click to tag)" : "Skills", sx, sy, 14, gold);
        if (!creating) { std::snprintf(b, sizeof b, "points: %d", sh.skillPoints); DrawText(b, sx + 250, sy, 14, dim); }
        else { std::snprintf(b, sizeof b, "tags %d/%d", tags, sh.tagCount); DrawText(b, sx + 250, sy, 14, tags == sh.tagCount ? Color{140, 230, 140, 255} : Color{240, 180, 100, 255}); }
        sy += 22;
        for (int i = 0; i < kSkillCount; ++i, sy += 24) {
            const bool tagged = creating ? (d.tagMask & (1u << i)) != 0 : (sh.tagMask & (1u << i)) != 0;
            const Rectangle row{static_cast<float>(sx), static_cast<float>(sy), 322, 22};
            if (creating) {
                if (Button(row, "", tagged, true)) {
                    if (tagged) d.tagMask &= ~(1u << i);
                    else if (tags < sh.tagCount) d.tagMask |= (1u << i);
                }
            }
            if (!creating && Button(Rectangle{static_cast<float>(sx), static_cast<float>(sy), 196, 22}, "", st.mode == CursorMode::Skill && st.skill == i, true))
                ui.commands->push_back(UiCommand{UiCommand::SelectSkill, static_cast<uint32_t>(i), 0});
            DrawText(tagged ? "*" : " ", sx + 6, sy + 4, 14, gold);
            DrawText(kSkillNames[i], sx + 20, sy + 4, 14, txt);
            int value = sh.skill[i];
            if (creating) {   // preview: base from the draft SPECIAL + tag bonus (server formulas, see Character.cpp)
                int spv[7]; for (int k = 0; k < 7; ++k) spv[k] = d.special[k];
                value = PreviewSkillBase(i, spv);
                if (tagged) value += 20;
            }
            std::snprintf(b, sizeof b, "%d%%", value);
            DrawText(b, sx + 150, sy + 4, 14, txt);
            if (!creating) {
                if (Button(Rectangle{static_cast<float>(sx + 200), static_cast<float>(sy + 1), 24, 20}, "+", false, sh.skillPoints > 0 && value < 100))
                    ui.commands->push_back(UiCommand{UiCommand::SpendSkill, static_cast<uint32_t>(i), 1});
            }
        }
        if (creating) {
            static const char* traitNames[16] = {"Bruiser", "Chem Reliant", "Clean Living", "Fast Shot", "Feral Kid", "Finesse", "Gifted", "Good Natured",
                                                 "Increased Metabolism", "Kamikaze", "Night Person", "One Hander", "One In a Million", "Red Scare", "Skilled", "Small Frame"};
            int nt = 0;
            for (int t = 0; t < 16; ++t) if (d.traitMask & (1u << t)) ++nt;
            std::snprintf(b, sizeof b, "Traits (up to %d)", sh.traitMax);
            DrawText(b, px + 10, py + 408, 14, gold);
            for (int t = 0; t < 16; ++t) {
                const bool on = (d.traitMask & (1u << t)) != 0;
                const Rectangle tb{static_cast<float>(px + 10 + (t % 4) * 146), static_cast<float>(py + 430 + (t / 4) * 24), 142, 22};
                if (Button(tb, traitNames[t], on, on || nt < sh.traitMax)) { if (on) d.traitMask &= ~(1u << t); else d.traitMask |= (1u << t); }
            }
            const bool ready = left == 0 && tags == sh.tagCount;
            if (Button(Rectangle{static_cast<float>(px + 10), static_cast<float>(py + 528), 150, 28}, "Create character", ready, ready))
                ui.commands->push_back(UiCommand{UiCommand::CharCreate, 0, 0});
            DrawText(ready ? "Ready." : "Spend all points and tag the skills.", px + 170, py + 535, 13, dim);
        } else {
            DrawText("C closes this window.  Click a skill to use it, then click a target (S cycles skills, Esc / RMB cancels).", px + 10, py + 472, 12, dim);
        }
    }

    // cursor mode label and tooltip for whatever is under the mouse
    void DrawCursorInfo(const UiView& ui) {
        const Vector2 mp = GetMousePosition();
        if (IsOverUi(static_cast<int>(mp.x), static_cast<int>(mp.y))) return;
        PickResult pr;
        const bool picked = Pick(static_cast<int>(mp.x), static_cast<int>(mp.y), pr);
        const CursorMode mode = picked ? ResolveCursor(*ui.core, ui.state->mode, pr) : ui.state->mode;
        std::string t = mode == CursorMode::Skill ? std::string("[") + kSkillNames[std::clamp(ui.state->skill, 0, kSkillCount - 1)] + "]" : std::string("[") + CursorModeName(mode) + "]";
        if (picked) {
            if (pr.kind == PickResult::Critter) {
                if (const ClientCritter* c = ui.core->Find(pr.critter)) {
                    t += " " + c->name;
                    if (c->hp >= 0 && c->maxHp > 0) t += "  " + std::to_string(c->hp) + "/" + std::to_string(c->maxHp) + " HP";
                    if (c->dead) t += " (dead)";
                }
            } else if (pr.kind == PickResult::Item) {
                auto it = ui.core->items().find(pr.item);
                if (it != ui.core->items().end()) t += " " + it->second.name;
            }
        }
        const int fs = 14;
        const int tw = MeasureText(t.c_str(), fs);
        Rectangle r{mp.x + 16, mp.y + 14, static_cast<float>(tw + 10), 20};
        DrawRectangleRec(r, Color{0, 0, 0, 170});
        DrawText(t.c_str(), static_cast<int>(r.x) + 5, static_cast<int>(r.y) + 3, fs, ModeColor(mode));
    }

    float TileSize() const { return 32.0f * zoom_; }
    Rectangle ChatPanel() const {
        float h = 150;
        return Rectangle{8, GetScreenHeight() - h - 36.0f - (orig_.ok() ? orig_.BarHeight() : 0.0f), 520, h};
    }
    // Top-left corner of a tile in screen space. Tile coords denote tile centres.
    Vector2 TileToScreen(float tx, float ty) const {
        const float ts = TileSize();
        return Vector2{GetScreenWidth() / 2.0f + (tx - 0.5f - camX_) * ts, GetScreenHeight() / 2.0f + (ty - 0.5f - camY_) * ts};
    }

    float zoom_ = 1.0f;
    float camX_ = 0, camY_ = 0;

    RendererOptions opt_;
    std::unique_ptr<GameAssets> assets_;
    std::unique_ptr<RigLibrary> rigs_;
    OrigUi orig_;   // original-art action bar / inventory (falls back to the raylib HUD when assets are missing)
    MapView map_;
    DrawOptions dopt_;
    OrbitCam cam_;
    Camera3D lastCam_{};
    Vector3 panOffset_{0, 0, 0};
    Vector3 hover_{0, 0, 0};
    bool hoverValid_ = false, active3d_ = false, follow_ = false, showGrid_ = false, shotDone_ = false;
    bool rmbDown_ = false;
    float rmbTravel_ = 0;
    int lastTileW_ = 0, lastTileH_ = 0, frame_ = 0;
    size_t nextShot_ = 0;
    double startTime_ = 0, sceneNow_ = 0;
    uint64_t attemptedSerial_ = ~0ull;
    std::string attemptedName_, loadedName_, assetErr_, mapErr_;
    std::vector<Label> labels_;
    std::vector<PickRect> pickRects_;
    std::vector<Rectangle> uiRects_;
    std::unordered_map<CritterId, Visual> visuals_;
    std::unordered_map<CritterId, Vector3> critterWorld_;
    std::unordered_map<ItemId, bool> doorState_;
    CritterId hoverCrit_ = 0;
    ItemId hoverItem_ = 0;
    struct PhCritter { CritterId id; float x, y; bool self; };
    struct PhItem { ItemId id; TilePos pos; };
    std::vector<PhCritter> phCritters_;
    std::vector<PhItem> phItems_;
};

std::unique_ptr<IRenderer> CreateRaylibRenderer(const RendererOptions& opt) { return std::make_unique<RaylibRenderer>(opt); }

}  // namespace f3
