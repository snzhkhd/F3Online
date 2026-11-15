#pragma once
// Reader for the original engine's interface layout files (Interface\*.gui, "GUI V1.3"). The format is only partly
// understood (see docs/formats/gui.md): this reader recovers, for every widget in file order, its name, flag bytes, skin
// ini, rectangle, alpha, the nine skin fragment textures and any font / sound / text strings. The parent/child tree is
// not recovered; callers address widgets by name (optionally "after" another widget) and apply parent offsets themselves.
#include <cstdint>
#include <string>
#include <vector>

namespace f3::gd {

struct GuiWidget {
    std::string name;
    uint8_t flags[3] = {0, 0, 0};    // [0] 1 = state "down"/hidden variant, [1] 1 = container, [2] 1 = skinned (button/picture/window)
    std::string skin;                // skin ini ("zzmainmenu.ini")
    uint8_t alpha = 255;
    int rect[4] = {0, 0, 0, 0};      // left, top, right, bottom, relative to the parent widget
    std::vector<std::string> frag;   // nine skin fragment textures (UL, U, UR, L, M, R, LL, LO, LR) or empty
    std::vector<std::string> misc;   // other strings in the record: font names, sound files
    int w() const { return rect[2] - rect[0]; }
    int h() const { return rect[3] - rect[1]; }
    const std::string& picture() const { static const std::string e; return frag.size() == 9 ? frag[4] : e; }
    std::string font() const;        // first misc string that is not a file name, or ""
};

struct GuiFile {
    std::string name;                // "Actionbar.INT"
    std::vector<GuiWidget> widgets;
    // First widget called `name` at index >= from (case-sensitive). -1 if none.
    int Find(const std::string& name, int from = 0) const;
};

bool ParseGui(const uint8_t* data, size_t size, GuiFile& out);

// ---------------------------------------------------------------------------------------------------------------------
// Full structural parser (ParseGuiTree). The record layout is the one of the engine's own reader (vbdecomp
// openje/src/gui: Interface::LoadResource, Window/Label/Button/Fragment ::ReadResource), so every widget type and every
// property is decoded and the parent/child tree is exact. See docs/formats/gui.md.
struct GuiFragment {
    uint32_t tileMode = 0, frameW = 0, frameH = 0, frameRate = 0, curFrame = 0, frameCount = 0;
    std::string texture;
    float z = 0;
    uint32_t color = 0;
    int rect[4] = {0, 0, 0, 0};
};

struct GuiNode {
    enum Type { Window = 0, Button = 1, Picture = 2, Label = 3, Edit = 4 };
    uint32_t type = 0;
    std::string name, skin;
    bool hidden = false, frameVisible = false, centerVisible = false;
    uint32_t color = 0;                    // tint, bytes B G R A (A = alpha)
    int rect[4] = {0, 0, 0, 0};            // "rectangle" (first 16 bytes), relative to the parent
    int clientRect[4] = {0, 0, 0, 0};      // client rectangle (second copy)
    uint8_t value60 = 0;
    uint32_t materialModes = 0;
    std::string tipImage;                  // stringCF8: tooltip picture (f3_tooltip_*.tga)
    std::string str0, str1;                // stringCB8 / stringCD8 (usually empty)
    uint32_t tipTextRef = 0, tipTitleRef = 0;   // English.stf ids of the tooltip body / title (valueCD4 / valueCF4)
    GuiFragment frag[9];                   // UL U UR L M R LL LO LR
    std::vector<std::pair<uint32_t, std::string>> strings;   // indexed per-widget strings
    // Button
    uint32_t state = 0;
    bool keyboard = false;
    std::string soundDown, soundUp, soundEnter;
    // Label / Edit
    uint32_t strRef = 0;                   // English.stf id of the text
    std::string fontName;
    uint32_t fontFlags = 0, fontSize = 0, fontCharset = 0, textColor = 0;
    uint8_t textFrame = 0, wrap = 0;
    uint32_t alignment = 0, pad[4] = {0, 0, 0, 0}, valign = 0;
    std::vector<GuiNode> children;

    int w() const { return rect[2] - rect[0]; }
    int h() const { return rect[3] - rect[1]; }
    const std::string& picture() const { return frag[4].texture; }
    const GuiNode* Child(const std::string& n) const;      // direct child by name
    const GuiNode* Find(const std::string& n) const;       // depth-first search below this node
};

struct GuiTree {
    std::string name;
    bool hidden = false;
    uint32_t version = 0;                  // 1..4 (file "GUI V1.0".."V1.3")
    std::vector<GuiNode> roots;
    const GuiNode* Find(const std::string& n) const;       // depth-first, file order
    size_t Count() const;
};

bool ParseGuiTree(const uint8_t* data, size_t size, GuiTree& out, std::string* err = nullptr);

}  // namespace f3::gd
