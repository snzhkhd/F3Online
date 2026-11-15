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

}  // namespace f3::gd
