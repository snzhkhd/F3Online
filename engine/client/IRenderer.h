#pragma once
// Rendering abstraction. The game loop (client/main.cpp) only talks to IRenderer, so the
// raylib placeholder renderer can later be replaced by one that loads the original
// game's assets (TGA/DDS tiles, GR2 models) without touching networking or game logic.
//
// The renderer owns the window, input polling and the camera; it receives a read-only
// snapshot of the replicated world (SceneView) plus UI state (UiView) each frame.
#include <memory>
#include <string>
#include <vector>
#include "ClientCore.h"

namespace f3 {

struct FrameInput {
    bool mouseLeftPressed = false;
    int mouseX = 0, mouseY = 0;
    float wheel = 0;
    std::string typed;  // UTF-8 characters typed this frame
    bool enter = false, backspace = false, escape = false;
};

struct SceneCritter {
    const ClientCritter* critter = nullptr;
    float x = 0, y = 0;  // interpolated tile position
    bool isSelf = false;
};

struct SceneView {
    const ClientMap* map = nullptr;
    std::vector<SceneCritter> critters;
    std::vector<const ClientItem*> items;
    float cameraX = 0, cameraY = 0;  // tile units, centre of view (renderer may smooth further)
    bool hasSelf = false;            // cameraX/Y is the controlled critter (renderer follows it)
    bool viewer = false;             // map viewer mode: no network, the renderer draws its own loaded map
    bool allowCameraKeys = true;     // false while the chat box has the keyboard
};

// Options for the asset renderer. With an empty gameDir (or a failed open) the placeholder renderer is used.
struct RendererOptions {
    std::string gameDir;            // original install (resource.rht + data\*.grp)
    std::string screenshotPath;     // write a PNG of frame `screenshotFrame`
    int screenshotFrame = 30;
    bool exitAfterScreenshot = false;
    // optional initial camera / view overrides (viewer and tests)
    bool hasCam = false;  float camAzimuth = 45, camElevation = 45, camFov = 12;
    bool hasTarget = false;  float targetX = 0, targetZ = 0;   // original world x/z
    float clipY = 1e9f;             // roof cut-away height
    bool noEntities = false;
    float modelScale = -1;          // <0: default
};

struct UiView {
    const std::deque<ChatLine>* chat = nullptr;
    bool chatInputActive = false;
    std::string chatInput;
    std::string status;  // top-left line
    double now = 0;
};

class IRenderer {
public:
    virtual ~IRenderer() = default;
    virtual bool Init(int width, int height, const char* title) = 0;
    virtual void Shutdown() = 0;
    virtual bool ShouldClose() = 0;
    virtual FrameInput PollInput() = 0;
    virtual void BeginFrame() = 0;
    virtual void DrawScene(const SceneView& scene) = 0;
    virtual void DrawUi(const UiView& ui) = 0;
    virtual void EndFrame() = 0;
    // Maps a screen position to the tile under it for the last drawn scene.
    virtual bool ScreenToTile(int sx, int sy, TilePos& out) const = 0;
    // True if the point is over UI (so clicks should not become move orders).
    virtual bool IsOverUi(int sx, int sy) const = 0;

    // --- original-asset rendering (optional; the placeholder renderer ignores these) ---
    virtual bool HasAssets() const { return false; }
    // Loads a map of the original game by name (tre/emap base name). Viewer mode and network mode both use this.
    virtual bool LoadOriginalMap(const std::string& /*name*/, std::string* /*err*/ = nullptr) { return false; }
    virtual std::string AssetInfo() const { return {}; }
};

std::unique_ptr<IRenderer> CreateRaylibRenderer(const RendererOptions& opt = {});

}  // namespace f3
