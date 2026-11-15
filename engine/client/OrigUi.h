#pragma once
// Original-art HUD: the action bar and the inventory window drawn from the original game's Interface\*.gui layouts and
// TGA textures (read from the game install at run time, never copied into the repository). See docs/formats/gui.md.
// When the layouts, textures or fonts are missing Load() fails and the renderer keeps its plain raylib HUD.
#include <string>
#include <unordered_map>
#include <vector>
#include <raylib.h>
#include "IRenderer.h"
#include "gamedata/GameData.h"
#include "gamedata/GuiFile.h"

namespace f3 {

class OrigUi {
public:
    ~OrigUi() { Unload(); }
    // Needs the window (GPU). `gameDir` is the install root (Fonts\ lives there, layouts/textures come from `data`).
    bool Load(gd::GameData& data, const std::string& gameDir, std::string* err = nullptr);
    void Unload();
    bool ok() const { return ok_; }

    // Height of the action bar on screen (chat and other overlays sit above it).
    float BarHeight() const;
    // Draw (and handle the clicks of) the windows; occupied screen rects are appended to `rects`.
    void DrawActionBar(const UiView& ui, std::vector<Rectangle>& rects);
    void DrawInventory(const UiView& ui, std::vector<Rectangle>& rects);
    void DrawOverlay();   // tooltip, after all windows
    // True while an item is being dragged (the world must not react to the mouse).
    bool Dragging() const { return drag_.active; }

private:
    struct Tex { Texture2D t{}; bool ok = false; };
    const Texture2D* Texture(const std::string& name);
    void Text(const Font& f, const std::string& s, float x, float y, float size, Color c, int align = 0, float w = 0);  // align 0 left 1 centre 2 right
    float TextW(const Font& f, const std::string& s, float size) const;
    void Blit(const Texture2D* t, Rectangle dst, Color tint = WHITE);
    bool Find(const gd::GuiFile& g, const std::string& name, int from, int* idx) const;
    // widget rectangle in layout units, relative to its parent
    Rectangle LRect(const gd::GuiFile& g, const std::string& name, int from = 0, int* idx = nullptr) const;
    void Tooltip(const std::string& title, const std::string& body);

    gd::GameData* data_ = nullptr;
    std::unordered_map<std::string, Tex> tex_;
    gd::GuiFile bar_, inv_;
    Font fallouty_{}, gothic_{};
    bool haveFonts_ = false;
    bool ok_ = false;

    struct Drag { bool active = false, pressed = false; uint32_t id = 0; int from = 0; Vector2 start{}; } drag_;
    int scrollRows_ = 0;
    int sortMode_ = 0;            // 0 type, 1 weight, 2 name
    uint32_t examineId_ = 0;
    std::string examineName_, examineText_;
    double lastClickTime_ = 0;
    uint32_t lastClickId_ = 0;
    // tooltip requested during the frame, drawn last
    std::string tipTitle_, tipBody_;
    bool tipOn_ = false;
};

}  // namespace f3
