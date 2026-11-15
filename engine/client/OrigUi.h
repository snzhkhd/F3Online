#pragma once
// Original-art HUD: the action bar and the inventory window drawn from the original game's Interface\*.gui layouts and
// TGA textures (read from the game install at run time, never copied into the repository). See docs/formats/gui.md.
// When the layouts, textures or fonts are missing Load() fails and the renderer keeps its plain raylib HUD.
#include <climits>
#include <string>
#include <unordered_map>
#include <vector>
#include <raylib.h>
#include "IRenderer.h"
#include "gamedata/GameData.h"
#include "gamedata/GuiFile.h"
#include "gamedata/Stf.h"

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
    // Container.gui (256x176: Slot0..9, TakeAll, Done, Name) as the loot window of a container / corpse (core.hasContainer()); drawn
    // over the inventory window, which shows the player's side. Drag items between the two, double-click a slot to take it.
    bool HasLoot() const { return ok_ && haveLoot_; }
    void DrawLoot(const UiView& ui, std::vector<Rectangle>& rects);
    // Skill panel (key U): the usable skills with their values; a click selects the skill cursor (UiCommand::SelectSkill). No original
    // layout exists for it (no Skilldex .gui in the demo), so it is drawn in the style of the action bar (dark plate, amber gothic text).
    void DrawSkillPanel(const UiView& ui, std::vector<Rectangle>& rects);
    void DrawOverlay();   // tooltip, after all windows
    // Floating speech: word-wrapped outlined text (gothic font), centred on cx, the block's bottom edge at bottomY. Returns the block height.
    float DrawSpeech(const std::string& text, float cx, float bottomY, Color c, float alpha, float maxW = 230);
    // Cursor art (TGA): loose file in extracted/Interface (searched upwards from the cwd and the exe; the custom "Cursor" lives
    // there), else the game resource of that name. nullptr if neither exists. usedPath gets the source for the log.
    const Texture2D* CursorTexture(const std::string& name, std::string* usedPath = nullptr);
    // CharacterGeneration.gui (1024x768 layout, drawn full screen): `creating` = the creation screen shown before the world
    // (UiState::draft), otherwise the character window opened with C (sheet values, skill points, skill cursor selection).
    bool HasCharScreen() const { return ok_ && haveChar_; }
    void DrawCharScreen(const UiView& ui, bool creating, std::vector<Rectangle>& rects);
    // True while an item is being dragged (the world must not react to the mouse).
    bool Dragging() const { return drag_.active || lootDrag_.active; }
    // True while the action bar is on screen: PageUp / PageDown scroll its chat (the viewer's roof-cut keys must stand back).
    bool ChatOwnsPaging() const { return ok_ && GetTime() - lastBarTime_ < 0.25; }

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
    gd::GuiFile bar_, inv_, loot_;
    bool haveLoot_ = false;
    Rectangle lootRect_{0, 0, 0, 0};                              // screen rect of the loot window while it is drawn
    int lootScroll_ = 0;
    struct LootDrag { bool active = false, pressed = false; uint32_t id = 0; int side = 0; Vector2 start{}; } lootDrag_;
    double lootClickTime_ = 0;
    uint32_t lootClickId_ = 0;
    gd::GuiTree char_;                                            // CharacterGeneration.gui, exact tree
    std::unordered_map<std::string, const gd::GuiNode*> charIdx_;   // widget name -> node (first one)
    std::vector<const gd::GuiNode*> charFlat_;                     // all nodes, file order
    gd::Stf stf_;                                                  // English.stf (label / tooltip ids of the layouts)
    bool haveChar_ = false;
    int charTab_ = 0;                                              // 0 derived stats, 1 damage thresholds
    std::string charHint_;
    double charHintUntil_ = 0;
    // odometer-style number change on the character screens: the shown value of every digit window / number label is tracked by key,
    // a change starts a ~250 ms roll (increase = the old digit leaves upwards, the new one comes from below; decrease = reverse)
    struct RollSt { int shown = INT_MIN, from = 0; double t0 = -10; int dir = 1; };
    struct Roll { int from = 0, to = 0, dir = 1; float p = 1; bool rolling() const { return p < 1 && from != to; } };
    std::unordered_map<std::string, RollSt> rollers_;
    Roll RollOf(const std::string& key, int v);
    Font fallouty_{}, gothic_{};
    bool haveFonts_ = false;
    bool ok_ = false;

    struct Drag { bool active = false, pressed = false; uint32_t id = 0; int from = 0; Vector2 start{}; } drag_;
    int scrollRows_ = 0;
    // chat log in the message screen: wrapped history cache + scroll position (lines above the bottom)
    std::vector<std::pair<std::string, Color>> wrapped_;
    uint64_t wrapCounter_ = ~0ull;
    float wrapWidth_ = 0;
    int chatScroll_ = 0;
    bool wasTyping_ = false;
    double lastBarTime_ = -1;
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
