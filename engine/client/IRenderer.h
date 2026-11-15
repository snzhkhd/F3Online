#pragma once
// Rendering abstraction. The game loop (client/main.cpp) only talks to IRenderer, so the
// raylib renderer can be replaced without touching networking or game logic.
//
// The renderer owns the window, input polling and the camera; it receives a read-only
// snapshot of the replicated world (SceneView) plus UI state (UiView) each frame, and reports clicks on its own
// widgets back as UiCommands.
#include <memory>
#include <string>
#include <vector>
#include "ClientCore.h"

namespace f3 {

// FOnline-style cursor. Only Move, Examine, Attack and Skill are chosen by the player (UiState::mode): RMB toggles
// Move <-> Examine, key A enters Attack, a skill picked in the character window (or the S key) enters Skill (UiState::skill is the
// skill, a click on a critter / object / door / container sends UseSkill with that target), RMB or Esc leaves them. Use, Talk and Pickup are never selected by hand: with the
// Move cursor they appear by themselves while the mouse hovers a usable object / an NPC with a dialogue / a loose item
// (see ResolveCursor).
enum class CursorMode : int { Move = 0, Examine, Attack, Use, Talk, Pickup, Skill, Count };
inline const char* CursorModeName(CursorMode m) {
    switch (m) {
        case CursorMode::Move: return "Move";
        case CursorMode::Examine: return "Examine";
        case CursorMode::Attack: return "Attack";
        case CursorMode::Use: return "Use";
        case CursorMode::Talk: return "Talk";
        case CursorMode::Pickup: return "Pickup";
        case CursorMode::Skill: return "Skill";
        default: return "?";
    }
}

struct FrameInput {
    bool mouseLeftPressed = false;
    bool mouseRightClick = false;     // right button released without dragging (camera orbit uses drags)
    int mouseX = 0, mouseY = 0;
    float wheel = 0;
    std::string typed;  // UTF-8 characters typed this frame
    bool enter = false, backspace = false, escape = false;
    bool shift = false, ctrl = false;
    bool keyAttack = false;           // A: attack cursor
    bool keyInventory = false;        // I
    bool keyChar = false;             // C
    bool keyExamine = false;          // X
    bool keyHand = false;             // B: switch the active hand
    bool keyStop = false;             // Tab: stop attacking
    bool keySkill = false;            // S: next skill cursor (Shift+S: previous)
    int keyNumber = -1;               // top-row 1..9 -> 0..8, 0 -> 9: skill hotkey (Alt = use on yourself)
    bool alt = false;
};

struct SceneCritter {
    const ClientCritter* critter = nullptr;
    float x = 0, y = 0;  // interpolated tile position
    CritterPose pose;    // motion at `now`
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
    double now = 0;                  // ClientCore clock
    const ProtoDb* protos = nullptr;
    uint64_t mapSerial = 0;
};

// Options for the asset renderer. With an empty gameDir (or a failed open) the placeholder renderer is used.
struct RendererOptions {
    std::string gameDir;            // original install (resource.rht + data\*.grp)
    std::string screenshotPath;     // write a PNG of frame `screenshotFrame`
    int screenshotFrame = 30;
    bool exitAfterScreenshot = false;
    // multiple timed screenshots (seconds after start): prefix_N.png, logged with the frame timestamp
    std::string shotPrefix;
    std::vector<double> shotTimes;
    // optional initial camera / view overrides (viewer and tests)
    bool hasCam = false;  float camAzimuth = 45, camElevation = 45, camFov = 12;
    bool hasTarget = false;  float targetX = 0, targetZ = 0;   // original world x/z
    float clipY = 1e9f;             // roof cut-away height
    bool noEntities = false;
    bool noOrigUi = false;          // keep the plain raylib HUD even when the original interface art is available
    bool showGrid = false;          // start with the walk grid overlay on (the G key toggles it)
    float modelScale = -1;          // <0: default
    bool network = false;           // network mode: dynamic entities come from the server, not from the emap
    const ProtoDb* protos = nullptr;
};

// Login screen (shown before connecting). Layout is shared by the renderer (drawing) and main (hit tests).
struct LoginView {
    std::string name, password, error, status, server;
    int focus = 0;           // 0 name, 1 password
    bool busy = false;       // connecting / waiting for the server
    int hover = -1;          // 0 login button, 1 register button
};
struct LoginLayout {
    int px, py, pw, ph;      // panel
    int nameX, nameY, passX, passY, fieldW, fieldH;
    int loginX, regX, btnY, btnW, btnH;
    LoginLayout(int sw, int sh) {
        pw = 440; ph = 345; px = (sw - pw) / 2; py = (sh - ph) / 2;
        fieldW = pw - 80; fieldH = 36;
        nameX = passX = px + 40; nameY = py + 108; passY = py + 173;
        btnW = (fieldW - 16) / 2; btnH = 40; btnY = py + 238;
        loginX = px + 40; regX = loginX + btnW + 16;
    }
    static bool In(int x, int y, int rx, int ry, int rw, int rh) { return x >= rx && y >= ry && x < rx + rw && y < ry + rh; }
};

struct UiCommand {
    enum Type { SetMode, ToggleInventory, ToggleChar, Equip, Drop, Take, Put, CloseContainer, DialogChoose, DialogClose,
                CharCreate, SpendSkill, UseSkill, SwapHands, Quit, SelectSkill } type = SetMode;   // SpendSkill/UseSkill: a = skill index, b = points; CharCreate sends UiState::draft; SelectSkill: a = skill (enters the skill cursor, closes the sheet); Quit: leave the game (Cancel on the creation screen)
    uint32_t a = 0, b = 0, c = 0;   // Equip: a = inventory id, b = on, c = EquipSlot (0 = automatic)
};

struct CharDraft {            // character creation in progress (client side until "Create" is pressed)
    bool init = false;
    int special[7] = {5, 5, 5, 5, 5, 5, 5};
    unsigned tagMask = 0;
    unsigned traitMask = 0;
};

struct UiState {
    CharDraft draft;
    CursorMode mode = CursorMode::Move;   // chosen by the player: Move, Examine, Attack or Skill
    int skill = 0;                        // the skill of the Skill cursor (index into kSkillNames)
    bool inventory = false;
    bool charSheet = false;
};

struct UiView {
    const std::deque<ChatLine>* chat = nullptr;
    bool chatInputActive = false;
    std::string chatInput;
    std::string status;  // top-left line
    std::string banner;  // prominent warning (e.g. game data missing)
    double now = 0;
    const ClientCore* core = nullptr;   // HUD data (null in viewer mode)
    UiState* state = nullptr;
    std::vector<UiCommand>* commands = nullptr;
};

struct PickResult {
    enum Kind { None, Ground, Critter, Item } kind = None;
    CritterId critter = 0;
    ItemId item = 0;
    float fx = 0, fy = 0;  // ground position, tile units (tile centre = integer)
    TilePos tile;
};

// The cursor the game shows (and acts with) for the thing under the mouse, given the cursor the player selected.
// Attack and Examine are sticky and apply to everything; the Move cursor adapts to the target:
// loose item -> Pickup, door / container / usable -> Use, NPC with a dialogue -> Talk, other critters -> Examine.
inline CursorMode ResolveCursor(const ClientCore& core, CursorMode manual, const PickResult& pr) {
    if (manual == CursorMode::Attack && pr.kind == PickResult::Critter && pr.critter != core.selfId()) {
        // a corpse is never attacked: it is lootable (NPC) or only examinable (player), whatever the previous mode was
        const ClientCritter* c = core.Find(pr.critter);
        if (c && c->dead) return c->kind == CritterKind::Npc ? CursorMode::Use : CursorMode::Examine;
    }
    if (manual == CursorMode::Attack || manual == CursorMode::Examine || manual == CursorMode::Skill) return manual;
    if (pr.kind == PickResult::Item) {
        auto it = core.items().find(pr.item);
        if (it == core.items().end()) return CursorMode::Examine;
        return it->second.kind == gnet::ObjectKind::Ground ? CursorMode::Pickup : CursorMode::Use;
    }
    if (pr.kind == PickResult::Critter) {
        if (pr.critter == core.selfId()) return CursorMode::Move;
        const ClientCritter* c = core.Find(pr.critter);
        if (!c) return CursorMode::Examine;
        if (c->dead) return c->kind == CritterKind::Npc ? CursorMode::Use : CursorMode::Examine;   // an NPC corpse is a container: click = loot
        if (c->kind == CritterKind::Npc && c->pid >= 0) {
            const ProtoInfo* p = core.protos().ByPid(static_cast<uint32_t>(c->pid));
            if (p && p->hasDialog) return CursorMode::Talk;
        }
        return CursorMode::Examine;
    }
    return CursorMode::Move;
}

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
    virtual void DrawLogin(const LoginView& /*v*/) {}
    virtual void WindowSize(int& w, int& h) const { w = 1280; h = 720; }
    // Maps a screen position to the tile under it for the last drawn scene.
    virtual bool ScreenToTile(int sx, int sy, TilePos& out) const = 0;
    // What is under the cursor (critter, item or ground) in the last drawn scene.
    virtual bool Pick(int sx, int sy, PickResult& out) const = 0;
    // True if the point is over UI (so clicks should not become move orders).
    virtual bool IsOverUi(int sx, int sy) const = 0;

    // --- original-asset rendering (optional; the placeholder renderer ignores these) ---
    virtual bool HasAssets() const { return false; }
    // True when the last DrawScene used the 3D asset path (false = flat placeholder).
    virtual bool Is3D() const { return false; }
    // Loads a map of the original game by name (tre/emap base name). Viewer mode and network mode both use this.
    virtual bool LoadOriginalMap(const std::string& /*name*/, std::string* /*err*/ = nullptr) { return false; }
    virtual std::string AssetInfo() const { return {}; }
    virtual std::string AssetError() const { return {}; }
};

std::unique_ptr<IRenderer> CreateRaylibRenderer(const RendererOptions& opt = {});

}  // namespace f3
