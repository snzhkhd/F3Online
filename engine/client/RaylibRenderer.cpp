// Placeholder renderer: flat grid, coloured boxes. Only file that includes raylib.
#include <raylib.h>
#include <raymath.h>
#include <rlgl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include "GameAssets.h"
#include "IRenderer.h"
#include "MapView.h"
#include "common/log.h"

namespace f3 {

namespace {
Color Unpack(uint32_t rgba) {
    return Color{static_cast<unsigned char>(rgba >> 24), static_cast<unsigned char>(rgba >> 16),
                 static_cast<unsigned char>(rgba >> 8), static_cast<unsigned char>(rgba)};
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
                log::Warn("asset renderer disabled (%s); using placeholder", err.c_str());
                assets_.reset();
            } else {
                log::Info("game data: %s (%zu resources)", opt_.gameDir.c_str(), assets_->data().resources().size());
                if (opt_.modelScale > 0) MapView::modelScale = opt_.modelScale;
                dopt_.clipY = opt_.clipY;
                dopt_.entities = !opt_.noEntities;
            }
        }
        return true;
    }
    void Shutdown() override {
        map_.Unload();
        if (assets_) assets_->Unload();
        assets_.reset();
        if (IsWindowReady()) CloseWindow();
    }
    bool ShouldClose() override { return WindowShouldClose() || (shotDone_ && opt_.exitAfterScreenshot); }

    bool HasAssets() const override { return assets_ != nullptr; }
    std::string AssetInfo() const override { return map_.loaded() ? map_.info() : std::string(); }
    bool LoadOriginalMap(const std::string& name, std::string* err) override {
        if (!assets_) { if (err) *err = "no game data"; return false; }
        auto t0 = GetTime();
        if (!map_.Load(*assets_, name, err)) return false;
        log::Info("loaded %s (%.2fs); %zu textures, %zu models cached", map_.info().c_str(), GetTime() - t0,
                  assets_->textureCount(), assets_->modelCount());
        loadedName_ = name;
        // initial camera
        cam_ = OrbitCam{};
        cam_.target = map_.startTarget();
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
        if (in.wheel != 0) {
            if (map_.loaded()) cam_.fov = std::clamp(cam_.fov * (in.wheel > 0 ? 0.9f : 1.1f), 3.0f, 40.0f);
            else zoom_ = std::clamp(zoom_ * (in.wheel > 0 ? 1.1f : 0.9f), 0.4f, 3.0f);
        }
        return in;
    }

    void BeginFrame() override { BeginDrawing(); ClearBackground(Color{18, 20, 24, 255}); }
    void EndFrame() override {
        ++frame_;
        if (!opt_.screenshotPath.empty() && !shotDone_ && frame_ >= opt_.screenshotFrame) {
            Image img = LoadImageFromScreen();
            ExportImage(img, opt_.screenshotPath.c_str());
            UnloadImage(img);
            log::Info("screenshot written: %s", opt_.screenshotPath.c_str());
            shotDone_ = true;
        }
        EndDrawing();
    }

    void DrawScene(const SceneView& s) override {
        active3d_ = false;
        if (assets_) {
            if (!s.viewer && s.map && s.map->name != attemptedName_) {
                attemptedName_ = s.map->name;
                std::string err;
                if (!LoadOriginalMap(s.map->name, &err))
                    log::Warn("map '%s' not in game data (%s): placeholder view", s.map->name.c_str(), err.c_str());
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
            DrawPoly(Vector2{cx, cy}, 4, r * 1.6f, 45.0f, Color{240, 210, 70, 255});
        }

        for (const SceneCritter& sc : s.critters) {
            Vector2 p = TileToScreen(sc.x, sc.y);
            float pad = ts * 0.15f;
            Rectangle r{p.x + pad, p.y + pad, ts - 2 * pad, ts - 2 * pad};
            DrawRectangleRec(r, Unpack(sc.critter->color));
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

    void DrawUi(const UiView& ui) override {
        const int sh = GetScreenHeight();
        DrawText(ui.status.c_str(), 10, 8, 16, RAYWHITE);
        if (active3d_) {
            for (const Label& l : labels_) {
                Vector2 sp = GetWorldToScreen(l.pos, lastCam_);
                int tw = MeasureText(l.text.c_str(), 12);
                DrawText(l.text.c_str(), static_cast<int>(sp.x) - tw / 2, static_cast<int>(sp.y) - 6, 12, l.color);
            }
            char b[320];
            std::snprintf(b, sizeof b, "%d fps | az %.0f el %.0f fov %.1f | %s", GetFPS(), cam_.azimuth, cam_.elevation,
                          cam_.fov, map_.info().c_str());
            DrawText(b, 10, 28, 12, Color{200, 200, 200, 255});
            if (hoverValid_) {
                std::snprintf(b, sizeof b, "cursor: x %.1f y %.1f z %.1f  cell (%d, %d)  [original axes]", hover_.x, hover_.y,
                              -hover_.z, static_cast<int>(std::floor(hover_.x * 2)), static_cast<int>(std::floor(-hover_.z * 2)));
                DrawText(b, 10, 44, 12, Color{200, 200, 140, 255});
            }
            DrawText("RMB orbit | MMB/WASD pan | wheel zoom | Q/E rotate | R/F tilt | PgUp/PgDn roof cut | G grid | H props | L lightmap | F12 shot",
                     10, 60, 12, Color{150, 150, 150, 255});
        } else {
            DrawText("LMB move | Enter chat | wheel zoom", 10, 28, 12, Color{180, 180, 180, 255});
        }

        Rectangle panel = ChatPanel();
        DrawRectangleRec(panel, Color{0, 0, 0, 140});
        if (ui.chat) {
            const int lineH = 16, maxLines = static_cast<int>(panel.height / lineH);
            int count = static_cast<int>(ui.chat->size());
            int start = std::max(0, count - maxLines);
            int y = static_cast<int>(panel.y) + 2;
            for (int i = start; i < count; ++i, y += lineH) {
                const ChatLine& l = (*ui.chat)[i];
                std::string line = l.name.empty() ? l.text : l.name + ": " + l.text;
                Color c = l.channel == ChatChannel::System ? Color{140, 220, 255, 255} : RAYWHITE;
                DrawText(line.c_str(), static_cast<int>(panel.x) + 6, y, 14, c);
            }
        }
        Rectangle in{panel.x, panel.y + panel.height + 4, panel.width, 24};
        DrawRectangleRec(in, ui.chatInputActive ? Color{30, 30, 60, 220} : Color{0, 0, 0, 90});
        std::string text = ui.chatInputActive ? ui.chatInput + ((static_cast<int>(ui.now * 2) & 1) ? "_" : "") : "Press Enter to chat";
        DrawText(text.c_str(), static_cast<int>(in.x) + 6, static_cast<int>(in.y) + 4, 16,
                 ui.chatInputActive ? WHITE : Color{150, 150, 150, 255});
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
    bool IsOverUi(int sx, int sy) const override {
        Rectangle p = ChatPanel();
        p.height += 30;
        return CheckCollisionPointRec(Vector2{static_cast<float>(sx), static_cast<float>(sy)}, p);
    }

private:
    struct Label { Vector3 pos; std::string text; Color color; };

    bool PickAt(int sx, int sy, Vector3& hit) const {
        Ray r = GetScreenToWorldRay(Vector2{static_cast<float>(sx), static_cast<float>(sy)}, lastCam_);
        return map_.PickGround(r, hit);
    }

    void HandleCameraInput(const SceneView& s, float dt) {
        if (IsKeyPressed(KEY_F12)) TakeScreenshot("f3_screenshot.png");
        if (!s.allowCameraKeys) return;
        Vector2 md = GetMouseDelta();
        if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
            cam_.azimuth -= md.x * 0.3f;
            cam_.elevation = std::clamp(cam_.elevation + md.y * 0.3f, 15.0f, 89.0f);
        }
        const float az = cam_.azimuth * DEG2RAD;
        const Vector3 fwd{-std::sin(az), 0, -std::cos(az)}, right{std::cos(az), 0, -std::sin(az)};
        Vector3 move{0, 0, 0};
        const float speed = cam_.fov * 3.5f * (IsKeyDown(KEY_LEFT_SHIFT) ? 3.0f : 1.0f);
        if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP)) move = Vector3Add(move, fwd);
        if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN)) move = Vector3Subtract(move, fwd);
        if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) move = Vector3Add(move, right);
        if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT)) move = Vector3Subtract(move, right);
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
        if (IsKeyPressed(KEY_PAGE_UP)) dopt_.clipY = std::min(dopt_.clipY, 60.0f) + 1.0f;
        if (IsKeyPressed(KEY_PAGE_DOWN)) dopt_.clipY = std::min(dopt_.clipY, 60.0f) - 1.0f;
        if (IsKeyPressed(KEY_END)) dopt_.clipY = 1e9f;
    }

    void DrawScene3D(const SceneView& s) {
        const ClientMap* nm = s.viewer ? nullptr : s.map;
        const int tw = nm ? nm->width : map_.gridW(), th = nm ? nm->height : map_.gridH();
        lastTileW_ = tw; lastTileH_ = th;
        follow_ = s.hasSelf && nm;
        const float dt = GetFrameTime();
        HandleCameraInput(s, dt);
        if (follow_) {
            Vector3 w = map_.TileToWorld(s.cameraX, s.cameraY, tw, th);
            Vector3 goal = Vector3Add(w, panOffset_);
            cam_.target = Vector3Lerp(cam_.target, goal, std::min(1.0f, dt * 10.0f));
        }
        lastCam_ = cam_.Build();
        labels_.clear();

        rlSetClipPlanes(30.0, 1500.0);
        BeginMode3D(lastCam_);
        map_.Draw(*assets_, dopt_);

        // hover / picking
        Vector2 mp = GetMousePosition();
        hoverValid_ = !IsOverUi(static_cast<int>(mp.x), static_cast<int>(mp.y)) &&
                      PickAt(static_cast<int>(mp.x), static_cast<int>(mp.y), hover_);
        const float cw = tw > 0 && map_.gridW() > 0 ? map_.gridW() * 0.5f / tw : 0.5f;
        const float ch = th > 0 && map_.gridH() > 0 ? map_.gridH() * 0.5f / th : 0.5f;
        if (hoverValid_) {
            int hx, hy;
            if (map_.WorldToTile(hover_, tw, th, hx, hy)) {
                if (showGrid_) {
                    for (int dy = -12; dy <= 12; ++dy)
                        for (int dx = -12; dx <= 12; ++dx) {
                            int cx = hx + dx, cy = hy + dy;
                            if (cx < 0 || cy < 0 || (tw > 0 && cx >= tw) || (th > 0 && cy >= th)) continue;
                            bool blocked = nm ? nm->IsBlocked(cx, cy) : map_.Blocked(cx, cy);
                            Vector3 c = map_.TileToWorld(static_cast<float>(cx), static_cast<float>(cy), tw, th, hover_.y + 0.04f);
                            DrawPlane(c, Vector2{cw * 0.94f, ch * 0.94f}, blocked ? Color{220, 40, 40, 110} : Color{60, 220, 90, 40});
                        }
                }
                Vector3 c = map_.TileToWorld(static_cast<float>(hx), static_cast<float>(hy), tw, th, hover_.y + 0.06f);
                DrawPlane(c, Vector2{cw, ch}, Color{255, 255, 255, 120});
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
            const ModelAsset* human = assets_->Model("HumMNor");
            for (const SceneCritter& sc : s.critters) {
                Vector3 w = map_.TileToWorld(sc.x, sc.y, tw, th);
                int d = static_cast<int>(sc.critter->dir);
                float yaw = std::atan2(static_cast<float>(kDirDx[d]), -static_cast<float>(kDirDy[d]));
                Color c = Color{static_cast<unsigned char>(sc.critter->color >> 24), static_cast<unsigned char>(sc.critter->color >> 16),
                                static_cast<unsigned char>(sc.critter->color >> 8), 255};
                Color tint = Color{static_cast<unsigned char>(128 + c.r / 2), static_cast<unsigned char>(128 + c.g / 2),
                                   static_cast<unsigned char>(128 + c.b / 2), 255};
                if (human) {
                    const float k = MapView::modelScale;
                    Matrix m = MatrixMultiply(MatrixMultiply(MatrixScale(k, k, k), MatrixRotateY(yaw)), MatrixTranslate(w.x, w.y, w.z));
                    MapView::DrawModel(*assets_, *human, m, tint, nullptr, dopt_, true);
                } else {
                    DrawCylinder(w, 0.35f, 0.35f, 1.8f, 10, c);
                }
                labels_.push_back({Vector3{w.x, w.y + 2.3f, w.z}, sc.critter->name,
                                   sc.critter->kind == CritterKind::Npc ? Color{255, 220, 140, 255} : RAYWHITE});
            }
            for (const ClientItem* it : s.items) {
                Vector3 w = map_.TileToWorld(static_cast<float>(it->pos.x), static_cast<float>(it->pos.y), tw, th, 0.25f);
                DrawCube(w, 0.4f, 0.4f, 0.4f, Color{240, 210, 70, 255});
            }
        }
        EndMode3D();
    }

    float TileSize() const { return 32.0f * zoom_; }
    Rectangle ChatPanel() const {
        float h = 150;
        return Rectangle{8, GetScreenHeight() - h - 36.0f, 520, h};
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
    MapView map_;
    DrawOptions dopt_;
    OrbitCam cam_;
    Camera3D lastCam_{};
    Vector3 panOffset_{0, 0, 0};
    Vector3 hover_{0, 0, 0};
    bool hoverValid_ = false, active3d_ = false, follow_ = false, showGrid_ = false, shotDone_ = false;
    int lastTileW_ = 0, lastTileH_ = 0, frame_ = 0;
    std::string attemptedName_, loadedName_;
    std::vector<Label> labels_;
};

std::unique_ptr<IRenderer> CreateRaylibRenderer(const RendererOptions& opt) { return std::make_unique<RaylibRenderer>(opt); }

}  // namespace f3
