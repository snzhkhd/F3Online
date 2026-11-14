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
};

std::unique_ptr<IRenderer> CreateRaylibRenderer();

}  // namespace f3
