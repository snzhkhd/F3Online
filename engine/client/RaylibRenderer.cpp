// Placeholder renderer: flat grid, coloured boxes. Only file that includes raylib.
#include <raylib.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include "IRenderer.h"

namespace f3 {

namespace {
Color Unpack(uint32_t rgba) {
    return Color{static_cast<unsigned char>(rgba >> 24), static_cast<unsigned char>(rgba >> 16),
                 static_cast<unsigned char>(rgba >> 8), static_cast<unsigned char>(rgba)};
}
}  // namespace

class RaylibRenderer final : public IRenderer {
public:
    bool Init(int w, int h, const char* title) override {
        SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
        InitWindow(w, h, title);
        SetTargetFPS(60);
        SetExitKey(0);  // Esc is ours
        return IsWindowReady();
    }
    void Shutdown() override { if (IsWindowReady()) CloseWindow(); }
    bool ShouldClose() override { return WindowShouldClose(); }

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
        if (in.wheel != 0) zoom_ = std::clamp(zoom_ * (in.wheel > 0 ? 1.1f : 0.9f), 0.4f, 3.0f);
        return in;
    }

    void BeginFrame() override { BeginDrawing(); ClearBackground(Color{18, 20, 24, 255}); }
    void EndFrame() override { EndDrawing(); }

    void DrawScene(const SceneView& s) override {
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
        DrawText("LMB move | Enter chat | wheel zoom", 10, 28, 12, Color{180, 180, 180, 255});

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
};

std::unique_ptr<IRenderer> CreateRaylibRenderer() { return std::make_unique<RaylibRenderer>(); }

}  // namespace f3
