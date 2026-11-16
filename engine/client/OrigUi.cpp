#include "OrigUi.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "Fonts.h"
#include "TextUtil.h"
#include "common/Localization.h"
#include "common/log.h"
#include "common/utf8.h"
#include "gamedata/Tga.h"

namespace f3 {
namespace {

const Color kAmber{232, 196, 84, 255};
const Color kAmberDim{176, 148, 70, 255};

std::string StripExt(std::string s) {
    size_t d = s.rfind('.');
    if (d != std::string::npos && s.size() - d <= 5) s.resize(d);
    return s;
}

std::string ModeName(int type) {
    switch (type) {
        case 1: return loc::T("mode.burst");
        case 2: return loc::T("mode.single");
        case 6: return loc::T("mode.thrust");
        case 8: return loc::T("mode.swing");
        case 9: return loc::T("mode.throw");
        default: return loc::T("mode.attack");
    }
}

// original F3 shows whole AP per attack; the data's mode "ap" word is in tenths (guess: 40 -> 4AP)
int ModeAp(int ap) { return std::max(1, (ap + 5) / 10); }

bool In(Rectangle r, Vector2 p) { return CheckCollisionPointRec(p, r); }

const ProtoInfo* ProtoOf(const ClientCore& core, const gnet::InvEntry& e) { return core.protos().ByPid(e.pid); }

// EquipSlot an item may be put into (0 = not equippable); `slot` is also the preferred one for the item class
bool FitsSlot(const ProtoInfo* p, int slot) {
    if (!p) return false;
    if (p->cls == "weapon") return slot == gnet::SlotHand1 || slot == gnet::SlotHand2;
    if (p->cls == "armor") return slot == (p->headArmor ? gnet::SlotHead : gnet::SlotBody);
    return false;
}
bool Equippable(const ProtoInfo* p) { return p && (p->cls == "weapon" || p->cls == "armor"); }

std::string ItemText(const gnet::InvEntry& e, const ProtoInfo* p) {
    std::string t;
    if (p && !p->desc.empty()) t += p->desc + "\n";
    t += loc::T("item.weight", {{"n", e.weight * e.count}});
    if (p && p->cls == "weapon") {
        t += "\n" + loc::T("item.weapon", {{"min", static_cast<int>(p->dmgMin)}, {"max", static_cast<int>(p->dmgMax)}, {"range", p->range}});
        if (!p->modes.empty()) {
            t += "\n" + loc::T("item.modes");
            for (auto& m : p->modes) t += " " + ModeName(m.type);
        }
    } else if (p && p->cls == "armor") {
        t += "\n" + loc::T("item.armor", {{"dt", p->armor[0] / 2}, {"ac", p->armor[1] / 4}});
    } else if (p && p->cls == "ammo") {
        t += "\n" + loc::T("item.ammo");
    }
    return t;
}

}  // namespace

// --------------------------------------------------------------------------------------------------------- loading

bool OrigUi::Load(gd::GameData& data, const std::string& gameDir, std::string* err) {
    Unload();
    data_ = &data;
    auto fail = [&](const std::string& m) { if (err) *err = m; Unload(); return false; };
    std::vector<uint8_t> buf;
    if (!data.Read("Actionbar", gd::kResGui, buf) || !gd::ParseGui(buf.data(), buf.size(), bar_)) return fail("Actionbar.gui missing");
    if (!data.Read("Inventory", gd::kResGui, buf) || !gd::ParseGui(buf.data(), buf.size(), inv_)) return fail("Inventory.gui missing");
    haveLoot_ = data.Read("Container", gd::kResGui, buf) && gd::ParseGui(buf.data(), buf.size(), loot_) && loot_.Find("Slot9") >= 0 && loot_.Find("TakeAll") >= 0;
    if (!haveLoot_) log::Warn("original UI: Container.gui unavailable, the raylib container window stays");
    if (!Texture("f3_actionbar.tga") || !Texture("f3_inventory.tga")) return fail("interface textures missing");
    const std::string fd = gameDir + "/Fonts/";
    fallouty_ = LoadFontEx((fd + "r_fallouty.ttf").c_str(), 32, nullptr, 0);
    gothic_ = LoadFontEx((fd + "TT0807M_.TTF").c_str(), 32, nullptr, 0);
    haveFonts_ = fallouty_.texture.id != 0 && gothic_.texture.id != 0;
    if (!haveFonts_) return fail("fonts missing (Fonts\\r_fallouty.ttf, TT0807M_.TTF)");
    SetTextureFilter(fallouty_.texture, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(gothic_.texture, TEXTURE_FILTER_BILINEAR);
    haveDlg_ = data.Read("Dialog", gd::kResGui, buf) && gd::ParseGuiTree(buf.data(), buf.size(), dlg_) && dlg_.Find("SpeakerWin") && dlg_.Find("ListenerWin") &&
               dlg_.Find("SpeakerTextLbl") && dlg_.Find("ListenerTextLbl");
    if (!haveDlg_) log::Warn("original UI: Dialog.gui unavailable, the raylib dialogue box stays");
    havePip_ = Texture("f3_pipboy.tga") != nullptr;
    if (!havePip_) log::Warn("original UI: f3_pipboy.tga unavailable, no Pip-Boy window");
    // the character screen is optional: without it the renderer keeps its raylib character window
    haveChar_ = false;
    charIdx_.clear(); charFlat_.clear(); char_ = gd::GuiTree();
    if (data.Read("CharacterGeneration", gd::kResGui, buf) && gd::ParseGuiTree(buf.data(), buf.size(), char_) &&
        stf_.Load(gameDir + "/English.stf") && Texture("f3_chargen.tga")) {
        std::vector<const gd::GuiNode*> st;
        for (auto it = char_.roots.rbegin(); it != char_.roots.rend(); ++it) st.push_back(&*it);
        while (!st.empty()) {   // depth first, file order
            const gd::GuiNode* n = st.back(); st.pop_back();
            charFlat_.push_back(n);
            charIdx_.emplace(n->name, n);
            for (auto it = n->children.rbegin(); it != n->children.rend(); ++it) st.push_back(&*it);
        }
        haveChar_ = true;
    } else {
        log::Warn("original UI: CharacterGeneration.gui / English.stf / f3_chargen.tga unavailable, the raylib character window stays");
    }
    ok_ = true;
    log::Info("original UI: action bar (%zu widgets) and inventory (%zu widgets) loaded", bar_.widgets.size(), inv_.widgets.size());
    return true;
}

const Texture2D* OrigUi::CursorTexture(const std::string& name, std::string* usedPath) {
    const std::string key = "cursor:" + name;
    auto it = tex_.find(key);
    if (it != tex_.end()) return it->second.ok ? &it->second.t : nullptr;
    Tex t;
    std::string used;
    std::vector<uint8_t> buf;
    gd::Image img;
    auto upload = [&]() {
        Image ri{img.rgba.data(), img.w, img.h, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
        t.t = LoadTextureFromImage(ri);
        t.ok = t.t.id != 0;
        if (t.ok) SetTextureFilter(t.t, TEXTURE_FILTER_BILINEAR);
    };
    std::vector<std::string> bases = {"extracted/Interface/"};
    for (int up = 1; up <= 4; ++up) { std::string b; for (int i = 0; i < up; ++i) b += "../"; bases.push_back(b + "extracted/Interface/"); }
    const std::string exe = GetApplicationDirectory();
    for (int up = 0; up <= 4; ++up) { std::string b = exe; for (int i = 0; i < up; ++i) b += "../"; bases.push_back(b + "extracted/Interface/"); }
    for (const std::string& b : bases) {
        const std::string path = b + name + ".tga";
        if (!FileExists(path.c_str())) continue;
        int n = 0;
        unsigned char* d = LoadFileData(path.c_str(), &n);
        if (!d) continue;
        const bool good = gd::DecodeTga(d, static_cast<size_t>(n), img);
        UnloadFileData(d);
        if (good) { upload(); if (t.ok) { used = path; break; } }
    }
    if (!t.ok && data_ && data_->Read(name, gd::kResTga, buf) && gd::DecodeTga(buf.data(), buf.size(), img)) { upload(); if (t.ok) used = "game data " + name; }
    if (usedPath) *usedPath = used;
    log::Info("cursor art '%s': %s", name.c_str(), t.ok ? used.c_str() : "not found");
    return tex_.emplace(key, t).first->second.ok ? &tex_[key].t : nullptr;
}

void OrigUi::Unload() {
    for (auto& [k, t] : tex_) if (t.ok) UnloadTexture(t.t);
    tex_.clear();
    if (haveFonts_) { UnloadFont(fallouty_); UnloadFont(gothic_); }
    haveFonts_ = false;
    ok_ = false;
}

const Texture2D* OrigUi::Texture(const std::string& name) {
    if (name.empty() || !data_) return nullptr;
    auto it = tex_.find(name);
    if (it != tex_.end()) return it->second.ok ? &it->second.t : nullptr;
    Tex t;
    std::vector<uint8_t> buf;
    gd::Image img;
    if (data_->Read(name, gd::kResTga, buf) && gd::DecodeTga(buf.data(), buf.size(), img)) {
        Image ri{img.rgba.data(), img.w, img.h, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
        t.t = LoadTextureFromImage(ri);
        t.ok = t.t.id != 0;
        if (t.ok) SetTextureFilter(t.t, TEXTURE_FILTER_BILINEAR);
    }
    return tex_.emplace(name, t).first->second.ok ? &tex_[name].t : nullptr;
}

// ----------------------------------------------------------------------------------------------------- primitives

const Font& OrigUi::Pick(const Font& f, const std::string& s, float size) const {
    // the original TTFs have Latin glyphs only: other languages / Cyrillic strings use the replacement fonts. The wide display font is
    // for headings only (fallouty_ at 19 px and up), everything smaller uses the condensed body font
    return fonts::For(&f == &fallouty_ && size >= 19 ? fonts::Role::Head : fonts::Role::Body, s, f);
}

float OrigUi::FitSize(const Font& f, const std::string& s, float size, float maxW) const {
    if (maxW <= 0 || &Pick(f, s, size) == &f) return size;   // the original (Latin) fonts keep the layout's size
    float k = size;
    while (k > size * 0.62f && TextW(f, s, k) > maxW) k -= 0.5f;
    return k;
}

std::string OrigUi::StfT(int id) const { return loc::TOr("stf." + std::to_string(id), stf_.Get(id)); }

float OrigUi::TextW(const Font& f, const std::string& s, float size) const { return MeasureTextEx(Pick(f, s, size), s.c_str(), size, 1).x; }

void OrigUi::Text(const Font& f, const std::string& s, float x, float y, float size, Color c, int align, float w) {
    if (s.empty()) return;
    if (align == 1) x += (w - TextW(f, s, size)) / 2;
    else if (align == 2) x += w - TextW(f, s, size);
    DrawTextEx(Pick(f, s, size), s.c_str(), Vector2{std::floor(x), std::floor(y)}, size, 1, c);
}

OrigUi::Roll OrigUi::RollOf(const std::string& key, int v) {
    const double now = GetTime();
    RollSt& r = rollers_[key];
    if (r.shown == INT_MIN) { r.shown = r.from = v; r.t0 = -10; }
    if (v != r.shown) { r.dir = v > r.shown ? 1 : -1; r.from = r.shown; r.shown = v; r.t0 = now; }
    float p = static_cast<float>(std::clamp((now - r.t0) / 0.25, 0.0, 1.0));
    p = p * p * (3 - 2 * p);
    if (now - r.t0 >= 0.25) p = 1;
    return Roll{r.from, r.shown, r.dir, p};
}

void OrigUi::DrawSkillPanel(const UiView& ui, std::vector<Rectangle>& rects) {
    if (!ok_ || !haveFonts_ || !ui.state || !ui.state->skillPanel || !ui.core || !ui.commands) return;
    const float sw = static_cast<float>(GetScreenWidth()), sh_ = static_cast<float>(GetScreenHeight());
    const float s = std::min(sw / 1024.0f, sh_ / 768.0f);
    const bool have = ui.core->hasSheet();
    const float rowH = 26 * s, w = 250 * s, hdr = 34 * s;
    const float h = hdr + rowH * kUsableSkillCount + 24 * s;
    const Rectangle r{std::floor(sw / 2 - w / 2), std::floor(sh_ - BarHeight() - h - 8 * s), w, h};
    rects.push_back(r);
    DrawRectangleRec(r, Color{8, 8, 6, 235});
    DrawRectangleLinesEx(r, 2, Color{120, 100, 44, 255});
    DrawRectangleLinesEx(Rectangle{r.x + 4, r.y + 4, r.width - 8, r.height - 8}, 1, Color{60, 50, 24, 255});
    Text(fallouty_, loc::T("skillpanel.title"), r.x, r.y + 8 * s, 20 * s, kAmber, 1, r.width);
    const Vector2 mp = GetMousePosition();
    for (int i = 0; i < kUsableSkillCount; ++i) {
        const int sk = kUsableSkills[i];
        const Rectangle row{r.x + 10 * s, r.y + hdr + i * rowH, r.width - 20 * s, rowH};
        const bool hot = CheckCollisionPointRec(mp, row);
        const bool cur = ui.state->mode == CursorMode::Skill && ui.state->skill == sk;
        if (hot || cur) DrawRectangleRec(row, Color{70, 58, 22, hot ? 200u : 120u});
        const Color c = hot ? Color{255, 235, 140, 255} : kAmber;
        Text(gothic_, std::to_string(i + 1) + "  " + txt::SkillLabel(kSkillNames[sk]), row.x + 6 * s, row.y + 3 * s, 17 * s, c);
        if (have) DrawNumber("uSkill" + std::to_string(sk), fallouty_, row.x, row.y + 4 * s, 15 * s, Color{120, 235, 90, 255}, 2, row.width - 6 * s, "", ui.core->sheet().skill[sk]);
        if (hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) ui.commands->push_back(UiCommand{UiCommand::SelectSkill, static_cast<uint32_t>(sk), 0, 0});
    }
    Text(gothic_, loc::T("skillpanel.close"), r.x, r.y + r.height - 20 * s, 12 * s, kAmberDim, 1, r.width);
}

void OrigUi::DrawDialog(const UiView& ui, std::vector<Rectangle>& rects) {
    if (!HasDialog() || !haveFonts_ || !ui.core || !ui.commands) return;
    const ClientCore::DialogState& d = ui.core->dialog();
    if (!d.open) return;
    // layout rectangles are absolute (layout units, origin = top-left of SpeakerWin); the block is scaled up for legibility (the original
    // used 8 pt Arial) and centred at the top of the screen
    const gd::GuiNode* spW = dlg_.Find("SpeakerWin");
    const gd::GuiNode* liW = dlg_.Find("ListenerWin");
    const gd::GuiNode* spT = dlg_.Find("SpeakerTextLbl");
    const gd::GuiNode* spN = dlg_.Find("SpeakerNameLbl");
    const gd::GuiNode* liN = dlg_.Find("ListenerNameLbl");
    const gd::GuiNode* liT = dlg_.Find("ListenerTextLbl");
    const float sw = static_cast<float>(GetScreenWidth()), sh = static_cast<float>(GetScreenHeight());
    const float k = std::min(sw / 1024.0f, sh / 768.0f) * 1.5f;
    const float W = spW->w() * k;
    const float ox = std::floor(sw / 2 - W / 2), oy = std::floor(24 * k / 1.5f);
    const float fs = 12 * k, lh = fs + 4 * k / 1.5f;
    auto X = [&](int lx) { return ox + (lx - spW->rect[0]) * k; };
    auto Y = [&](int ly, float extra = 0) { return oy + (ly - spW->rect[1]) * k + extra; };
    auto wrap = [&](const std::string& text, float maxW) {
        return txt::Wrap(text, maxW, [&](const std::string& s) { return TextW(gothic_, s, fs); });
    };
    const Vector2 mp = GetMousePosition();
    const Color plate{8, 8, 6, 240}, edge{120, 100, 44, 255}, inner{60, 50, 24, 255};
    auto frame = [&](Rectangle r) {
        DrawRectangleRec(r, plate);
        DrawRectangleLinesEx(r, 2, edge);
        DrawRectangleLinesEx(Rectangle{r.x + 3, r.y + 3, r.width - 6, r.height - 6}, 1, inner);
        rects.push_back(r);
    };

    // ---- speaker: name + text (the model area is not drawn, so the text uses the whole width right of the picture)
    const float tx = X(spT->rect[0]), textW = W - (spT->rect[0] - spW->rect[0]) * k - 8 * k;
    const std::vector<std::string> tl = wrap(d.text, textW);
    const int minLines = 3;   // the layout's text label holds ~4 lines of 8 pt Arial; the window grows with the text instead
    const int maxLines = std::max(minLines, static_cast<int>((sh * 0.42f) / lh));
    const int shown = std::clamp(static_cast<int>(tl.size()), minLines, maxLines);
    if (d.text != dlgText_) { dlgText_ = d.text; dlgScroll_ = 0; }
    const int overflow = std::max(0, static_cast<int>(tl.size()) - shown);
    const float spH = (spT->rect[1] - spW->rect[1]) * k + shown * lh + 10 * k;
    const Rectangle spR{ox, oy, W, spH};
    if (overflow > 0 && CheckCollisionPointRec(mp, spR)) dlgScroll_ -= static_cast<int>(std::lround(GetMouseWheelMove()));
    dlgScroll_ = std::clamp(dlgScroll_, 0, overflow);
    frame(spR);
    Text(fallouty_, d.npcName, X(spN->rect[0]), Y(spN->rect[1]), 14 * k, kAmber);
    // picture slot (SpeakerPic): initial of the speaker on a dark tile
    {
        const gd::GuiNode* pic = dlg_.Find("SpeakerPic");
        if (pic) {
            const Rectangle pr{X(pic->rect[0]), Y(pic->rect[1]), pic->w() * k, pic->h() * k};
            DrawRectangleRec(pr, Color{24, 20, 10, 255});
            DrawRectangleLinesEx(pr, 1, inner);
            if (!d.npcName.empty()) Text(fallouty_, utf8::Truncate(d.npcName, 1), pr.x, pr.y + pr.height * 0.18f, pr.height * 0.6f, kAmberDim, 1, pr.width);
        }
    }
    for (int i = 0; i < shown && dlgScroll_ + i < static_cast<int>(tl.size()); ++i)
        Text(gothic_, tl[dlgScroll_ + i], tx, Y(spT->rect[1]) + i * lh, fs, Color{235, 235, 225, 255});
    if (overflow > 0) {
        if (dlgScroll_ > 0) Text(gothic_, "^", spR.x, spR.y + 6 * k, fs, kAmber, 2, spR.width - 10 * k);
        if (dlgScroll_ < overflow) Text(gothic_, "v", spR.x, spR.y + spR.height - lh - 4 * k, fs, kAmber, 2, spR.width - 10 * k);
    }

    // ---- listener: the player's name and the numbered answers (click or keys 1..9)
    const float liY = oy + spH - 2 * k;
    const float repX = X(liT->rect[0]), repW = W - (liT->rect[0] - liW->rect[0]) * k - 8 * k;
    struct Rep { std::vector<std::string> lines; float h = 0; };
    std::vector<Rep> reps(d.replies.size());
    float repH = 0;
    for (size_t i = 0; i < d.replies.size(); ++i) {
        reps[i].lines = wrap((i < 9 ? std::to_string(i + 1) + ". " : std::string("   ")) + d.replies[i], repW - 12 * k);
        reps[i].h = reps[i].lines.size() * lh + 4 * k;
        repH += reps[i].h;
    }
    if (reps.empty()) repH = lh;
    const float liHead = (liT->rect[1] - liW->rect[1]) * k;
    const float liH = liHead + repH + lh + 8 * k;
    const Rectangle liR{ox, liY, W, liH};
    frame(liR);
    Text(fallouty_, ui.core->playerName(), X(liN->rect[0]), liY + (liN->rect[1] - liW->rect[1]) * k, 14 * k, Color{120, 235, 90, 255});
    if (const gd::GuiNode* pic = dlg_.Find("ListenerPic")) {
        const Rectangle pr{X(pic->rect[0]), liY + (pic->rect[1] - liW->rect[1]) * k, pic->w() * k, pic->h() * k};
        DrawRectangleRec(pr, Color{12, 22, 10, 255});
        DrawRectangleLinesEx(pr, 1, inner);
        const std::string& pn = ui.core->playerName();
        if (!pn.empty()) Text(fallouty_, pn.substr(0, 1), pr.x, pr.y + pr.height * 0.18f, pr.height * 0.6f, Color{70, 150, 56, 255}, 1, pr.width);
    }
    float y = liY + liHead;
    for (size_t i = 0; i < reps.size(); ++i) {
        const Rectangle row{repX - 4 * k, y, repW + 8 * k, reps[i].h};
        const bool hot = CheckCollisionPointRec(mp, row);
        if (hot) DrawRectangleRec(row, Color{70, 58, 22, 200});
        for (size_t l = 0; l < reps[i].lines.size(); ++l)
            Text(gothic_, reps[i].lines[l], repX + (l ? 12 * k : 0), y + 2 * k + l * lh, fs, hot ? Color{255, 235, 140, 255} : kAmber);
        if (hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) ui.commands->push_back(UiCommand{UiCommand::DialogChoose, static_cast<uint32_t>(i), 0, 0});
        y += reps[i].h;
    }
    if (reps.empty()) Text(gothic_, "...", repX, y, fs, kAmberDim);
    // leave
    const std::string leave = loc::T("dlg.end");
    const float lw = TextW(gothic_, leave, fs * 0.85f);
    const Rectangle lr{liR.x + liR.width - lw - 14 * k, liR.y + liR.height - lh - 4 * k, lw + 8 * k, lh};
    const bool lhot = CheckCollisionPointRec(mp, lr);
    Text(gothic_, leave, lr.x + 4 * k, lr.y, fs * 0.85f, lhot ? Color{255, 235, 140, 255} : kAmberDim);
    if (lhot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) ui.commands->push_back(UiCommand{UiCommand::DialogClose, 0, 0, 0});
}

void OrigUi::DrawDigitCell(const Roll& ro, int div, bool pad, Rectangle r) {
    auto dig = [&](int val) { return pad || val >= div || div == 1 ? (std::max(val, 0) / div) % 10 : -1; };
    auto pic = [&](int d, Rectangle q) { if (d >= 0) Blit(Texture("f3_digit_" + std::to_string(d) + ".tga"), q); };
    r = Rectangle{std::floor(r.x + 0.5f), std::floor(r.y + 0.5f), std::floor(r.width + 0.5f), std::floor(r.height + 0.5f)};
    const int fd = dig(ro.from), td = dig(ro.to);
    if (!ro.rolling() || fd == td) { pic(td, r); return; }
    // the clip is exactly the cell: neighbours never see the sliding picture, and the cell background (the digit pictures are opaque) is
    // redrawn by whichever picture covers it
    BeginScissorMode(static_cast<int>(r.x), static_cast<int>(r.y), static_cast<int>(r.width), static_cast<int>(r.height));
    const float off = std::floor(ro.dir * ro.p * r.height + 0.5f);
    pic(fd, Rectangle{r.x, r.y - off, r.width, r.height});
    pic(td, Rectangle{r.x, r.y + (ro.dir > 0 ? r.height : -r.height) - off, r.width, r.height});
    EndScissorMode();
}

void OrigUi::DrawDigits(const std::string& key, Rectangle r, int v, int n, bool pad) {
    int maxV = 1;
    for (int i = 0; i < n; ++i) maxV *= 10;
    const Roll ro = RollOf(key, std::clamp(v, 0, maxV - 1));
    const float cw = r.width / n;
    float w = cw, h = r.height;
    if (w * 3 > h * 2) w = h * 2 / 3; else h = w * 3 / 2;      // keep the 16:24 aspect of the picture
    int div = maxV / 10;
    for (int i = 0; i < n; ++i, div /= 10) {
        const float x0 = std::floor(r.x + cw * i + 0.5f), x1 = std::floor(r.x + cw * (i + 1) + 0.5f);   // cells tile without gaps
        DrawDigitCell(ro, div, pad, Rectangle{x0 + std::floor((x1 - x0 - w) / 2 + 0.5f), r.y + std::floor((r.height - h) / 2 + 0.5f), std::floor(w + 0.5f), std::floor(h + 0.5f)});
    }
}

void OrigUi::DrawNumber(const std::string& key, const Font& f, float x, float y, float size, Color c, int align, float w, const std::string& pre, int v, const std::string& post) {
    const Roll ro = RollOf(key, v);
    const std::string to = std::to_string(ro.to);
    if (!ro.rolling()) { Text(f, pre + to + post, x, y, size, c, align, w); return; }
    const std::string from = std::to_string(ro.from);
    const float wp = TextW(f, pre, size), wn = std::max(TextW(f, from, size), TextW(f, to, size)), wq = TextW(f, post, size);
    float x0 = x;
    if (align == 1) x0 += (w - (wp + wn + wq)) / 2; else if (align == 2) x0 += w - (wp + wn + wq);
    x0 = std::floor(x0);
    Text(f, pre, x0, y, size, c);
    Text(f, post, x0 + wp + wn, y, size, c);
    const float h = size * 1.25f;
    BeginScissorMode(static_cast<int>(std::floor(x0 + wp)), static_cast<int>(std::floor(y)), static_cast<int>(std::ceil(wn)) + 1, static_cast<int>(std::ceil(h)));
    Text(f, from, x0 + wp, y - ro.dir * ro.p * h, size, c, 2, wn);
    Text(f, to, x0 + wp, y + ro.dir * (1 - ro.p) * h, size, c, 2, wn);
    EndScissorMode();
}

void OrigUi::DrawNumberText(const std::string& key, const Font& f, float x, float y, float size, Color c, int align, float w, const std::string& text) {
    size_t a = 0;
    while (a < text.size() && !(text[a] >= '0' && text[a] <= '9')) ++a;
    size_t b = a;
    while (b < text.size() && text[b] >= '0' && text[b] <= '9') ++b;
    if (a == b || b - a > 9) { Text(f, text, x, y, size, c, align, w); return; }
    DrawNumber(key, f, x, y, size, c, align, w, text.substr(0, a), std::atoi(text.substr(a, b - a).c_str()), text.substr(b));
}

float OrigUi::DrawSpeech(const std::string& text, float cx, float bottomY, Color c, float alpha, float maxW) {
    const Font& f = haveFonts_ ? gothic_ : GetFontDefault();
    const float fs = 16;
    const std::vector<std::string> lines = txt::Wrap(text, maxW, [&](const std::string& s) { return TextW(f, s, fs); }, false);
    const float lh = fs + 1;
    const float h = lh * lines.size();
    const unsigned char a = static_cast<unsigned char>(std::clamp(alpha, 0.0f, 1.0f) * 255);
    Color col = c; col.a = a;
    Color ol{0, 0, 0, a};
    float y = bottomY - h;
    for (const std::string& l : lines) {
        const float x = cx - TextW(f, l, fs) / 2;
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                if (dx || dy) Text(f, l, x + dx, y + dy, fs, ol);
        Text(f, l, x, y, fs, col);
        y += lh;
    }
    return h;
}

void OrigUi::Blit(const Texture2D* t, Rectangle dst, Color tint) {
    if (!t) return;
    DrawTexturePro(*t, Rectangle{0, 0, static_cast<float>(t->width), static_cast<float>(t->height)}, dst, Vector2{0, 0}, 0, tint);
}

bool OrigUi::Find(const gd::GuiFile& g, const std::string& name, int from, int* idx) const {
    int i = g.Find(name, from);
    if (idx) *idx = i;
    return i >= 0;
}

Rectangle OrigUi::LRect(const gd::GuiFile& g, const std::string& name, int from, int* idx) const {
    int i = g.Find(name, from);
    if (idx) *idx = i;
    if (i < 0) return Rectangle{0, 0, 0, 0};
    const auto& w = g.widgets[i];
    return Rectangle{static_cast<float>(w.rect[0]), static_cast<float>(w.rect[1]), static_cast<float>(w.w()), static_cast<float>(w.h())};
}

void OrigUi::Tooltip(const std::string& title, const std::string& body) {
    tipTitle_ = title;
    tipBody_ = body;
    tipOn_ = true;
}

float OrigUi::BarHeight() const {
    if (!ok_) return 0;
    return 164.0f * std::min(1.0f, GetScreenWidth() / 1024.0f);
}

// -------------------------------------------------------------------------------------------------------- action bar

void OrigUi::DrawActionBar(const UiView& ui, std::vector<Rectangle>& rects) {
    if (!ok_ || !ui.core || !ui.state || !ui.commands) return;
    const ClientCore& core = *ui.core;
    UiState& st = *ui.state;
    const float sw = static_cast<float>(GetScreenWidth()), sh = static_cast<float>(GetScreenHeight());
    const float s = std::min(1.0f, sw / 1024.0f);
    const float ox = std::floor((sw - 1024 * s) / 2), oy = sh - 164 * s;
    auto P = [&](float lx, float ly, float lw, float lh) { return Rectangle{ox + lx * s, oy + ly * s, lw * s, lh * s}; };
    const Vector2 mp = GetMousePosition();
    const bool click = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    rects.push_back(P(0, 0, 1024, 164));
    Blit(Texture("f3_actionbar.tga"), P(0, 0, 1024, 164));

    // ---- weapon slots: left = hand 1 (active weapon), right = hand 2 (spare); clicking the spare one (or key B) switches hands, even if it is empty
    for (int i = 0; i < 2; ++i) {
        const std::string n = std::string("0") + static_cast<char>('1' + i);
        int bi;
        Rectangle b = LRect(bar_, "btnWeapon" + n, 0, &bi);
        if (bi < 0) continue;
        const int slot = i == 0 ? gnet::SlotHand1 : gnet::SlotHand2;
        const gnet::InvEntry* item = nullptr;
        for (auto& e : core.inventory()) if (e.equipped == slot) item = &e;
        const ProtoInfo* p = item ? ProtoOf(core, *item) : nullptr;
        const bool active = i == 0;
        Rectangle br = P(b.x, b.y, b.width, b.height);
        Blit(Texture(active ? "f3_actionbar_weap_down.tga" : "f3_actionbar_weap_up.tga"), br);
        if (p && !p->iconAct.empty()) {
            const Texture2D* ic = Texture(p->iconAct);
            if (ic) {
                Rectangle ir = P(b.x + 12, b.y + 6, 160, 80);
                const float k = std::min(ir.width / ic->width, ir.height / ic->height);
                Blit(ic, Rectangle{ir.x + (ir.width - ic->width * k) / 2, ir.y + (ir.height - ic->height * k) / 2, ic->width * k, ic->height * k});
            }
        }
        std::string mode, ap;
        if (p && p->cls == "weapon") {
            if (!p->modes.empty()) { mode = ModeName(p->modes[0].type); ap = "AP"; }
            else { mode = loc::T("mode.melee"); }
        } else if (!item && i == 0) {
            mode = loc::T("mode.unarmed");
        }
        const float fs = 22 * s;
        Text(gothic_, mode, b.x * s + ox + 8 * s, b.y * s + oy + 4 * s, fs, kAmber, 2, 164 * s - 16 * s);
        if (!ap.empty()) DrawNumber("wAp" + std::to_string(i), gothic_, b.x * s + ox + 8 * s, b.y * s + oy + 56 * s, fs, kAmber, 0, 0, "", ModeAp(p->modes[0].ap), loc::T("hud.ap_unit"));
        if (p && p->cls == "weapon" && p->range > 1) {
            bool ammo = false;
            for (auto& e : core.inventory()) if (const ProtoInfo* q = ProtoOf(core, e)) if (q->cls == "ammo") ammo = true;
            const Texture2D* at = Texture(ammo ? "f3_actionbar_ammo_on.tga" : "f3_actionbar_ammo_off.tga");
            if (at) Blit(at, P(b.x + 160, b.y + 74, at->width * 1.3f, at->height * 1.3f));
        }
        if (In(br, mp)) {
            if (item) Tooltip(item->name, ItemText(*item, p));
            else Tooltip(loc::T(active ? "hand.empty_active" : "hand.empty_spare"), loc::T(active ? "hand.fists_tip" : "hand.switch_tip"));
            // clicking the inactive hand makes it the active one, also when it is empty (= holster the weapon)
            if (click && !active) ui.commands->push_back(UiCommand{UiCommand::SwapHands, 0, 0, 0});
        }
    }

    // ---- digit boxes: health / action points / armor class
    const ClientCritter* me = core.self();
    int hp = core.hasStats() ? core.stats().hp : (me ? me->hp : 0);
    int maxHp = core.hasStats() ? core.stats().maxHp : (me ? me->maxHp : 0);
    const float maxAp = core.waitKnown() ? core.MaxAp() : (core.hasStats() ? core.stats().sequence : 0.0f);
    const float ap = core.CurrentAp();
    const int ac = core.hasStats() ? core.stats().armorClass : 0;
    // the original digit pictures (f3_digit_N.tga) with the odometer roller, like the creation screen
    DrawDigits("barHp", P(314, 30, 42, 18), hp, 3);
    DrawDigits("barAp", P(314, 79, 42, 18), static_cast<int>(std::lround(ap)), 3);
    DrawDigits("barAc", P(314, 131, 42, 18), ac, 3);

    // ---- player frame (name, health, AP)
    {
        Rectangle pf = P(380, 0, 104, 36);
        Blit(Texture("f3_actionbar_PC_down.tga"), pf);
        Text(gothic_, me ? me->name : "", pf.x + 6 * s, pf.y + 3 * s, 15 * s, kAmber);
        const float hf = maxHp > 0 ? std::clamp(static_cast<float>(hp) / maxHp, 0.f, 1.f) : 0.f;
        const float af = maxAp > 0.5f ? std::clamp(ap / maxAp, 0.f, 1.f) : 0.f;
        Blit(Texture("f3_actionbar_fill_yellow.tga"), P(380 + 8, 19, 88 * hf, 4));
        Blit(Texture("f3_actionbar_fill_blue.tga"), P(380 + 8, 28, 88 * af, 4));
    }

    // ---- AP cells and counter
    {
        const int n = std::clamp(static_cast<int>(std::lround(maxAp)), 0, 21);
        for (int i = 0; i < n; ++i) {
            const float f = ap - i;
            const char* t = f >= 1.0f ? "f3_actionbar_AP_green.tga" : f > 0.05f ? "f3_actionbar_AP_blue.tga" : "f3_actionbar_AP_grey.tga";
            Blit(Texture(t), P(412 + 10 * i, 44, 9, 9));
        }
        Blit(Texture("f3_actionbar_AP_box.tga"), P(624, 42, 24, 14));
        DrawDigits("barApBox", P(625, 42, 22, 13), static_cast<int>(std::lround(ap)), 2);
    }

    // ---- buttons
    struct Btn { const char* name; const char* label; UiCommand::Type cmd; bool on; };
    const Btn btns[] = {{"btnInventory", "hud.inventory", UiCommand::ToggleInventory, st.inventory},
                        {"btnCharacter", "hud.character", UiCommand::ToggleChar, st.charSheet},
                        {"btnOptions", "hud.options", UiCommand::SetMode, false}};
    for (auto& bt : btns) {
        int bi;
        Rectangle b = LRect(bar_, bt.name, 0, &bi);
        if (bi < 0) continue;
        Rectangle r = P(b.x, b.y, b.width, b.height);
        const bool hot = In(r, mp);
        Blit(Texture(bt.on || (hot && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) ? "f3_button_down.tga" : "f3_button_up.tga"), r);
        { const std::string bl = loc::T(bt.label); Text(gothic_, bl, r.x + 42 * s, r.y + 8 * s, FitSize(gothic_, bl, 18 * s, r.width - 46 * s), hot ? Color{255, 230, 130, 255} : kAmber); }
        if (hot && click && bt.cmd != UiCommand::SetMode) ui.commands->push_back(UiCommand{bt.cmd, 0, 0});
    }

    // ---- message area (left dark screen): chat log (newest at the bottom) + input line; break bar and busy text below
    {
        const float fs = 14 * s;
        Rectangle area = P(16, 18, 262, 106);
        const float lineH = fs + 2 * s;
        const Rectangle inR = P(16, 108, 262, 16);   // input / hint line (bottom of the area)
        const int maxLines = std::max(1, static_cast<int>((inR.y - area.y - 2) / lineH));
        const float maxW = area.width - 4 * s;
        // wrap the whole history (cached until a line arrives or the width changes)
        const uint64_t counter = ui.core ? ui.core->chatCounter() : 0;
        if (counter != wrapCounter_ || std::fabs(maxW - wrapWidth_) > 0.5f) {
            const size_t oldN = wrapped_.size();
            wrapped_.clear();
            if (ui.chat) {
                for (const ChatLine& l : *ui.chat) {
                    std::string line = l.name.empty() ? l.text : l.name + ": " + l.text;
                    const Color c = l.channel == ChatChannel::System ? Color{140, 220, 255, 255} : Color{225, 205, 150, 255};
                    for (std::string& ln : txt::Wrap(line, maxW, [&](const std::string& s2) { return TextW(gothic_, s2, fs); }, false)) wrapped_.emplace_back(std::move(ln), c);
                }
            }
            // a user who scrolled up keeps looking at the same lines when new ones arrive; at the bottom the view follows
            if (chatScroll_ > 0 && wrapWidth_ == maxW && wrapped_.size() > oldN) chatScroll_ += static_cast<int>(wrapped_.size() - oldN);
            wrapCounter_ = counter;
            wrapWidth_ = maxW;
        }
        const std::vector<std::pair<std::string, Color>>& wrapped = wrapped_;
        const int maxScroll = std::max(0, static_cast<int>(wrapped.size()) - maxLines);
        lastBarTime_ = GetTime();
        // scrolling: mouse wheel while hovering the message screen, PageUp / PageDown
        if (In(P(16, 18, 262, 124), mp)) chatScroll_ += static_cast<int>(std::lround(GetMouseWheelMove() * 3));
        if (IsKeyPressed(KEY_PAGE_UP) || IsKeyPressedRepeat(KEY_PAGE_UP)) chatScroll_ += std::max(1, maxLines - 1);
        if (IsKeyPressed(KEY_PAGE_DOWN) || IsKeyPressedRepeat(KEY_PAGE_DOWN)) chatScroll_ -= std::max(1, maxLines - 1);
        if (wasTyping_ && !ui.chatInputActive) chatScroll_ = 0;   // the message was sent (or dropped): look at the newest lines again
        wasTyping_ = ui.chatInputActive;
        chatScroll_ = std::clamp(chatScroll_, 0, maxScroll);
        const int first = std::max(0, static_cast<int>(wrapped.size()) - maxLines - chatScroll_);
        const int last = std::min(static_cast<int>(wrapped.size()), first + maxLines);
        const int shown = last - first;
        float y = inR.y - 2 * s - shown * lineH;   // newest visible line sits right above the input line
        for (int i = first; i < last; ++i, y += lineH) Text(gothic_, wrapped[i].first, area.x + 2 * s, y, fs, wrapped[i].second);
        if (maxScroll > 0) {   // scroll indicator: thin track + thumb on the right edge, "more" while lines below are hidden
            const float th = inR.y - area.y - 2;
            const Rectangle tr{area.x + area.width - 3 * s, area.y, 2 * s, th};
            DrawRectangleRec(tr, Color{70, 60, 30, 120});
            const float frac = static_cast<float>(maxLines) / static_cast<float>(wrapped.size());
            const float thumbH = std::max(8.0f * s, th * frac);
            const float pos = maxScroll > 0 ? static_cast<float>(maxScroll - chatScroll_) / maxScroll : 1.0f;
            DrawRectangleRec(Rectangle{tr.x, tr.y + (th - thumbH) * pos, tr.width, thumbH}, chatScroll_ > 0 ? Color{255, 220, 120, 230} : Color{170, 150, 80, 180});
            if (chatScroll_ > 0) Text(gothic_, loc::T("chat.more"), area.x + area.width - 6 * s, inR.y - 12 * s, 10 * s, kAmberDim, 2);
        }
        if (ui.chatInputActive) {
            DrawRectangleRec(inR, Color{30, 26, 8, 200});
            const std::string t = ui.chatInput + ((static_cast<int>(ui.now * 2) & 1) ? "_" : "");
            const float tw = TextW(gothic_, t, fs);
            Text(gothic_, t, inR.x + 2 * s - std::max(0.0f, tw - (inR.width - 8 * s)), inR.y, fs, WHITE);
        } else {
            Text(gothic_, loc::T("chat.hint"), inR.x + 2 * s, inR.y, fs, Color{110, 95, 55, 255});
        }
        // cursor mode (RMB toggles Move/Examine, A = Attack): small unobtrusive label
        {
            const std::string mn = st.mode == CursorMode::Attack ? loc::T("cursor.attack") : st.mode == CursorMode::Examine ? loc::T("cursor.examine") :
                                   st.mode == CursorMode::Skill ? txt::SkillLabel(kSkillNames[std::clamp(st.skill, 0, kSkillCount - 1)]) : loc::T("cursor.move");
            Text(gothic_, mn, P(16, 18, 0, 0).x, P(16, 18, 0, 0).y, 12 * s, kAmberDim, 2, 262 * s);
        }
        // break time: red bar while actions are locked
        Rectangle bt = P(16, 127, 262, 14);
        const bool locked = core.IsBreak();
        DrawRectangleRec(bt, Color{20, 6, 6, 200});
        char b[64];
        if (locked) {
            const double rem = core.BreakRemaining(), tot = std::max(0.05, core.BreakTotal());
            DrawRectangleRec(Rectangle{bt.x, bt.y, bt.width * static_cast<float>(std::clamp(rem / tot, 0.0, 1.0)), bt.height}, Color{210, 30, 30, 255});
            std::snprintf(b, sizeof b, "%.1f", rem);
            Text(gothic_, loc::T("hud.break", {{"s", b}}), bt.x + 6, bt.y - 1 * s, 14 * s, WHITE);
        } else {
            Text(gothic_, loc::T(core.InBattle() ? "hud.battle_ready" : "hud.ready"), bt.x + 6, bt.y - 1 * s, 14 * s, Color{120, 170, 120, 255});
        }
        DrawRectangleLinesEx(bt, 1, locked ? Color{200, 60, 60, 255} : Color{80, 60, 60, 255});
        if (core.waitKnown() && core.lastResult() != gnet::ActionResult::Ok && core.lastResult() != gnet::ActionResult::None && ui.now - core.lastResultTime() < 1.5) {
            const bool mv = core.lastAction() == static_cast<uint8_t>(GMsgRt::C2S_MoveToF) || core.lastAction() == static_cast<uint8_t>(Msg::C2S_MoveTo);
            const std::string why = loc::T(core.lastResult() == gnet::ActionResult::Busy ? (mv ? "hud.busy_walk" : "hud.busy") : core.lastResult() == gnet::ActionResult::NoAp ? "hud.noap" :
                                           core.lastResult() == gnet::ActionResult::Dead ? "hud.dead" : "hud.invalid");
            Text(gothic_, why, P(16, 108, 0, 0).x + 150 * s, P(16, 108, 0, 0).y, 14 * s, Color{255, 120, 100, 255});
        }
    }
}

// ---------------------------------------------------------------------------------------------------------- inventory

void OrigUi::DrawInventory(const UiView& ui, std::vector<Rectangle>& rects) {
    if (!ok_ || !ui.core || !ui.commands) return;
    const ClientCore& core = *ui.core;
    const float sw = static_cast<float>(GetScreenWidth()), sh = static_cast<float>(GetScreenHeight());
    const float bar = BarHeight();
    const float s = std::clamp(std::min(sw / 1024.0f, (sh - bar - 4) / 604.0f), 0.3f, 1.0f);
    const float ox = std::floor((sw - 1024 * s) / 2), oy = std::max(0.0f, std::floor((sh - bar - 604 * s) / 2));
    auto P = [&](float lx, float ly, float lw, float lh) { return Rectangle{ox + lx * s, oy + ly * s, lw * s, lh * s}; };
    const Rectangle win = P(0, 0, 1024, 604);
    const Vector2 mpReal = GetMousePosition();
    const Vector2 mp = mpReal;
    rects.push_back(win);
    Blit(Texture("f3_inventory.tga"), win);

    const auto& all = core.inventory();
    // ---- backpack list: carried (not equipped) items, sorted
    std::vector<const gnet::InvEntry*> list;
    for (auto& e : all) if (!e.equipped) list.push_back(&e);
    auto rank = [&](const gnet::InvEntry* e) {
        const ProtoInfo* p = ProtoOf(core, *e);
        const std::string c = p ? p->cls : "";
        return c == "weapon" ? 0 : c == "armor" ? 1 : c == "ammo" ? 2 : 3;
    };
    std::stable_sort(list.begin(), list.end(), [&](const gnet::InvEntry* a, const gnet::InvEntry* b) {
        if (sortMode_ == 1) return a->weight * a->count > b->weight * b->count;
        if (sortMode_ == 2) return a->name < b->name;
        return rank(a) < rank(b);
    });
    const int cols = 3, rows = 6;
    const int totalRows = (static_cast<int>(list.size()) + cols - 1) / cols;
    scrollRows_ = std::clamp(scrollRows_, 0, std::max(0, totalRows - rows));
    const Rectangle bp = P(68, 96, 192, 384);
    if (In(bp, mp)) {
        const float w = GetMouseWheelMove();
        if (w != 0) scrollRows_ = std::clamp(scrollRows_ - static_cast<int>(w), 0, std::max(0, totalRows - rows));
    }
    {
        int bi;
        Rectangle up = LRect(inv_, "btnBackpackUp", 0, &bi), dn = LRect(inv_, "btnBackpackDown");
        if (bi >= 0 && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            if (In(P(up.x, up.y, up.width, up.height), mp)) scrollRows_ = std::max(0, scrollRows_ - 1);
            if (In(P(dn.x, dn.y, dn.width, dn.height), mp)) scrollRows_ = std::min(std::max(0, totalRows - rows), scrollRows_ + 1);
        }
    }

    struct Region { Rectangle r; int slot; const gnet::InvEntry* item; };   // slot 0 = backpack cell
    std::vector<Region> regions;
    for (int rr = 0; rr < rows; ++rr)
        for (int cc = 0; cc < cols; ++cc) {
            const int idx = (scrollRows_ + rr) * cols + cc;
            regions.push_back({P(68 + cc * 64, 96 + rr * 64, 60, 60), 0, idx < static_cast<int>(list.size()) ? list[idx] : nullptr});
        }
    struct SlotDef { int slot; const char* label; float hy; float ey; const char* nm; };
    const SlotDef sdefs[] = {{gnet::SlotHead, "inv.slot.head", 72, 164, "Head"}, {gnet::SlotBody, "inv.slot.body", 212, 216, "Body"},
                             {gnet::SlotHand1, "inv.slot.hand1", 348, 268, "Hand1"}, {gnet::SlotHand2, "inv.slot.hand2", 488, 320, "Hand2"}};
    for (auto& d : sdefs) {
        const gnet::InvEntry* it = nullptr;
        for (auto& e : all) if (e.equipped == d.slot) it = &e;
        Rectangle hr = LRect(inv_, std::string("pic") + d.nm + "SlotHighlight");
        if (hr.width <= 0) hr = Rectangle{316, d.hy, 116, 76};
        regions.push_back({P(hr.x, hr.y, hr.width, hr.height), d.slot, it});
    }

    // ---- icons, labels
    const Color white{255, 255, 255, 255};
    for (auto& rg : regions) {
        const bool hot = In(rg.r, mp);
        if (rg.slot == 0) {
            if (hot) DrawRectangleRec(rg.r, Color{255, 255, 255, 28});
        } else if (hot) {
            DrawRectangleRec(rg.r, Color{255, 255, 255, 24});
        }
        if (rg.item && !(drag_.active && drag_.id == rg.item->id)) {
            const ProtoInfo* p = ProtoOf(core, *rg.item);
            const Texture2D* ic = p ? Texture(p->icon) : nullptr;
            Rectangle ir = rg.slot == 0 ? Rectangle{rg.r.x + 4 * s, rg.r.y + 4 * s, 52 * s, 52 * s}
                                        : Rectangle{rg.r.x + 20 * s, rg.r.y, 76 * s, 76 * s};
            if (ic) Blit(ic, ir);
            else Text(gothic_, rg.item->name.substr(0, 8), ir.x, ir.y + ir.height / 2 - 8 * s, 14 * s, kAmber, 1, ir.width);
            if (rg.item->count > 1) {
                DrawNumber("cnt" + std::to_string(rg.item->id), fallouty_, ir.x, ir.y + ir.height - 14 * s, 14 * s, white, 2, ir.width, "", rg.item->count);
            }
        }
    }
    const float labelY[] = {32, 170, 308, 448};
    for (int i = 0; i < 4; ++i) Text(gothic_, loc::T(sdefs[i].label), P(312, labelY[i], 124, 24).x, P(312, labelY[i], 124, 24).y + 2 * s, 17 * s, kAmber, 1, 124 * s);

    // ---- while dragging: show which slots accept the item
    const gnet::InvEntry* dragItem = nullptr;
    if (drag_.active)
        for (auto& e : all) if (e.id == drag_.id) dragItem = &e;
    if (dragItem) {
        const ProtoInfo* dp = ProtoOf(core, *dragItem);
        for (auto& rg : regions)
            if (rg.slot != 0 && FitsSlot(dp, rg.slot)) DrawRectangleLinesEx(rg.r, 2, Color{120, 235, 90, 220});
    }

    // ---- readouts
    Text(gothic_, loc::T("inv.weight"), P(68, 60, 0, 0).x, P(68, 60, 0, 0).y - 2 * s, 15 * s, kAmber);
    if (core.hasStats()) {
        const bool over = core.stats().carried > core.stats().carryWeight;
        {
            std::string pre, post;
            txt::SplitNum(loc::T("inv.weight_cur", {{"max", core.stats().carryWeight}}), pre, post);
            DrawNumber("invWeight", fallouty_, P(152, 60, 0, 0).x, P(152, 60, 0, 0).y, 14 * s, over ? Color{255, 90, 70, 255} : Color{120, 235, 90, 255}, 0, 0, pre,
                       static_cast<int>(core.stats().carried), post);
        }
        const char* sp[] = {"special_short.strength", "special_short.perception", "special_short.endurance", "special_short.charisma", "special_short.intelligence", "special_short.agility", "special_short.luck"};
        for (int i = 0; i < 7; ++i) {
            { const std::string sl = loc::T(sp[i]); Text(gothic_, sl, P(468, 68 + 12 * i, 0, 0).x, P(468, 68 + 12 * i, 0, 0).y - 3 * s, FitSize(gothic_, sl, 14 * s, 27 * s), kAmber); }
            DrawNumber("invSp" + std::to_string(i), fallouty_, P(496, 68 + 12 * i, 0, 0).x, P(496, 68 + 12 * i, 0, 0).y, 13 * s, Color{120, 235, 90, 255}, 0, 0, "", core.stats().special[i]);
        }
        {
            const Color g{120, 235, 90, 255};
            std::string pre, post;
            txt::SplitNum(loc::T("inv.hp", {{"max", core.stats().maxHp}}), pre, post);
            DrawNumber("invHp", fallouty_, P(560, 68, 0, 0).x, P(560, 68, 0, 0).y, 13 * s, g, 0, 0, pre, core.stats().hp, post);
            txt::SplitNum(loc::T("inv.ac"), pre, post);
            DrawNumber("invAc", fallouty_, P(560, 82, 0, 0).x, P(560, 82, 0, 0).y, 13 * s, g, 0, 0, pre, core.stats().armorClass, post);
            txt::SplitNum(loc::T("inv.level"), pre, post);
            DrawNumber("invLvl", fallouty_, P(560, 96, 0, 0).x, P(560, 96, 0, 0).y, 13 * s, g, 0, 0, pre, core.stats().level, post);
        }
    }
    if (core.self()) Text(fallouty_, core.self()->name, P(464, 32, 0, 0).x, P(464, 32, 0, 0).y, 18 * s, kAmber);
    for (auto& d : sdefs) {
        const gnet::InvEntry* it = nullptr;
        for (auto& e : all) if (e.equipped == d.slot) it = &e;
        const float x = 468, y = d.ey;
        const std::string slotLbl = loc::T(d.label) + ":";
        Text(gothic_, slotLbl, P(x, y + 2, 0, 0).x, P(x, y + 2, 0, 0).y, 14 * s, kAmberDim);
        const float vx = P(x, y, 0, 0).x + std::max(56 * s, TextW(gothic_, slotLbl, 14 * s) + 6 * s);   // the value follows the label (translations are longer)
        if (!it) { Text(fallouty_, loc::T("ui.empty"), vx, P(x, y, 0, 0).y, 13 * s, kAmberDim); continue; }
        const ProtoInfo* p = ProtoOf(core, *it);
        Text(fallouty_, it->name, vx, P(x, y, 0, 0).y, 13 * s, Color{120, 235, 90, 255});
        std::string b;
        if (p && p->cls == "weapon") {
            int ammo = 0;
            for (auto& e : all) if (const ProtoInfo* q = ProtoOf(core, e)) if (q->cls == "ammo") ammo += e.count;
            b = loc::T("inv.weapon_line", {{"min", static_cast<int>(p->dmgMin)}, {"max", static_cast<int>(p->dmgMax)}, {"range", p->range}, {"ammo", ammo}});
        } else if (p && p->cls == "armor") {
            b = loc::T("inv.armor_line", {{"dt", p->armor[0] / 2}, {"ac", p->armor[1] / 4}});
        }
        Text(fallouty_, b, vx, P(x, y + 16, 0, 0).y, FitSize(fallouty_, b, 12 * s, 262 * s - (vx - P(x, y, 0, 0).x)), kAmber);
    }
    // sort selector (bottom left field) and drop target
    {
        const char* names[] = {"inv.sort.type", "inv.sort.weight", "inv.sort.name"};
        Rectangle sr = P(48, 540, 172, 24);
        Text(gothic_, loc::T(names[sortMode_]), sr.x, sr.y + 2 * s, 16 * s, kAmber, 1, sr.width);
        if (In(sr, mp) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) sortMode_ = (sortMode_ + 1) % 3;
    }
    const Rectangle dropR = P(244, 528, 48, 48);

    // ---- interaction
    const Region* hover = nullptr;
    for (auto& rg : regions) if (In(rg.r, mp)) hover = &rg;
    const bool overWin = In(win, mp);
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && hover && hover->item) {
        drag_.pressed = true; drag_.active = false; drag_.id = hover->item->id; drag_.from = hover->slot; drag_.start = mp;
    }
    if (drag_.pressed && !drag_.active && IsMouseButtonDown(MOUSE_BUTTON_LEFT) &&
        std::hypot(mp.x - drag_.start.x, mp.y - drag_.start.y) > 6) drag_.active = true;
    if (drag_.pressed && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        const gnet::InvEntry* di = nullptr;
        for (auto& e : all) if (e.id == drag_.id) di = &e;
        if (di) {
            const ProtoInfo* dp = ProtoOf(core, *di);
            if (!drag_.active) {   // plain click: double click equips / unequips
                const double now = ui.now;
                if (lastClickId_ == di->id && now - lastClickTime_ < 0.45) {
                    if (di->equipped) ui.commands->push_back(UiCommand{UiCommand::Equip, di->id, 0, 0});
                    else if (Equippable(dp)) ui.commands->push_back(UiCommand{UiCommand::Equip, di->id, 1, 0});
                    lastClickId_ = 0;
                } else {
                    lastClickId_ = di->id; lastClickTime_ = now;
                }
            } else if (hover && hover->slot != 0 && FitsSlot(dp, hover->slot)) {
                if (di->equipped != hover->slot) ui.commands->push_back(UiCommand{UiCommand::Equip, di->id, 1, static_cast<uint32_t>(hover->slot)});
            } else if (In(dropR, mp) || (!overWin && !In(Rectangle{0, sh - bar, sw, bar}, mp))) {
                ui.commands->push_back(UiCommand{UiCommand::Drop, di->id, 0});
            } else if (In(P(68, 96, 192, 384), mp) && di->equipped) {
                ui.commands->push_back(UiCommand{UiCommand::Equip, di->id, 0, 0});
            }
        }
        drag_ = Drag{};
    }
    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT) && !drag_.pressed) drag_.active = false;
    if (hover && hover->item) {
        const ProtoInfo* p = ProtoOf(core, *hover->item);
        // no floating tooltip here: hovering fills the description box below
        if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {   // examine
            examineId_ = hover->item->id;
            examineName_ = hover->item->name;
            examineText_ = ItemText(*hover->item, p);
        }
    }

    // ---- description box (examined item)
    if (examineId_) {
        bool still = false;
        for (auto& e : all) if (e.id == examineId_) still = true;
        if (!still) { examineId_ = 0; examineText_.clear(); }
    }
    if (examineId_ || hover) {
        const gnet::InvEntry* di = nullptr;
        if (hover && hover->item) di = hover->item;
        std::string title = di ? di->name : examineName_;
        std::string body = di ? ItemText(*di, ProtoOf(core, *di)) : examineText_;
        Rectangle dr = P(460, 400, 468, 172);
        Text(gothic_, title, dr.x, dr.y, 20 * s, kAmber);
        float y = dr.y + 26 * s;
        size_t pos = 0;
        while (pos <= body.size() && y < dr.y + dr.height - 12 * s) {
            size_t e = body.find('\n', pos);
            if (e == std::string::npos) e = body.size();
            std::string ln = body.substr(pos, e - pos), cur;
            size_t q = 0;
            while (q < ln.size()) {   // word wrap
                size_t sp = ln.find(' ', q);
                if (sp == std::string::npos) sp = ln.size();
                std::string w = ln.substr(q, sp - q);
                if (!cur.empty() && TextW(fallouty_, cur + " " + w, 14 * s) > dr.width) {
                    Text(fallouty_, cur, dr.x, y, 14 * s, Color{120, 235, 90, 255});
                    y += 17 * s; cur = w;
                } else {
                    cur = cur.empty() ? w : cur + " " + w;
                }
                q = sp + 1;
            }
            Text(fallouty_, cur, dr.x, y, 14 * s, Color{120, 235, 90, 255});
            y += 17 * s;
            pos = e + 1;
        }
    }

    // ---- dragged item and tooltip last
    if (drag_.active && dragItem) {
        const ProtoInfo* p = ProtoOf(core, *dragItem);
        if (const Texture2D* ic = p ? Texture(p->icon) : nullptr) Blit(ic, Rectangle{mp.x - 32 * s, mp.y - 32 * s, 64 * s, 64 * s}, Color{255, 255, 255, 200});
    }
}

// ------------------------------------------------------------------------------------------------------------- loot window

// Two panels side by side, each cut from the inventory art (f3_inventory.tga: the backpack part, 236x472 of the 800x472 texture,
// plus the texture's right frame strip so that every panel has a closed frame): LEFT = the player's backpack (the same list,
// weight readout, sort field, trash and scroll arrows as the inventory window, no equipment), RIGHT = the target's items with
// its name, weight, scroll arrows and the Done / Take All buttons (Container.gui) in the bottom field.
void OrigUi::DrawLoot(const UiView& ui, std::vector<Rectangle>& rects) {
    if (!ok_ || !haveLoot_ || !ui.core || !ui.commands) return;
    const ClientCore& core = *ui.core;
    if (!core.hasContainer()) { lootRect_ = Rectangle{0, 0, 0, 0}; lootDrag_ = LootDrag{}; return; }
    const float sw = static_cast<float>(GetScreenWidth()), sh = static_cast<float>(GetScreenHeight());
    const float bar = BarHeight();
    const float kK = 800.0f / 1024.0f;                       // texture px per layout unit
    const float kPanelW = (236 + 26) / kK;                   // one panel in layout units (335.4)
    const float kPanelH = 604;
    const float s = std::clamp(std::min(sw / 1024.0f, (sh - bar - 4) / 604.0f), 0.3f, 1.0f);
    const float wx = std::floor((sw - 2 * kPanelW * s) / 2), wy = std::max(0.0f, std::floor((sh - bar - kPanelH * s) / 2));
    auto PP = [&](int panel, float lx, float ly, float lw, float lh) { return Rectangle{wx + (panel * kPanelW + lx) * s, wy + ly * s, lw * s, lh * s}; };
    const Rectangle win{wx, wy, 2 * kPanelW * s, kPanelH * s};
    const Rectangle panelR[2] = {PP(0, 0, 0, kPanelW, kPanelH), PP(1, 0, 0, kPanelW, kPanelH)};
    lootRect_ = win;
    rects.push_back(win);
    const Vector2 mp = GetMousePosition();
    const bool pressed = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    const bool corpse = core.containerIsCorpse();
    const Color green{120, 235, 90, 255};
    const Color white{255, 255, 255, 255};

    // ---- panel art
    if (const Texture2D* t = Texture("f3_inventory.tga")) {
        for (int p = 0; p < 2; ++p) {
            DrawTexturePro(*t, Rectangle{0, 0, 236, 472}, PP(p, 0, 0, 236 / kK, kPanelH), Vector2{0, 0}, 0, WHITE);
            DrawTexturePro(*t, Rectangle{774, 0, 26, 472}, PP(p, 236 / kK, 0, 26 / kK, kPanelH), Vector2{0, 0}, 0, WHITE);
        }
    }

    // ---- the two item lists
    std::vector<const gnet::InvEntry*> mine, theirs;
    for (auto& e : core.inventory()) if (!e.equipped) mine.push_back(&e);
    auto rank = [&](const gnet::InvEntry* e) {
        const ProtoInfo* p = ProtoOf(core, *e);
        const std::string c = p ? p->cls : "";
        return c == "weapon" ? 0 : c == "armor" ? 1 : c == "ammo" ? 2 : 3;
    };
    std::stable_sort(mine.begin(), mine.end(), [&](const gnet::InvEntry* a, const gnet::InvEntry* b) {
        if (sortMode_ == 1) return a->weight * a->count > b->weight * b->count;
        if (sortMode_ == 2) return a->name < b->name;
        return rank(a) < rank(b);
    });
    for (auto& e : core.container()) theirs.push_back(&e);
    const int cols = 3, rows = 6;
    auto rowsOf = [&](const std::vector<const gnet::InvEntry*>& l) { return (static_cast<int>(l.size()) + cols - 1) / cols; };
    scrollRows_ = std::clamp(scrollRows_, 0, std::max(0, rowsOf(mine) - rows));
    lootScroll_ = std::clamp(lootScroll_, 0, std::max(0, rowsOf(theirs) - rows));
    int* scr[2] = {&scrollRows_, &lootScroll_};
    const std::vector<const gnet::InvEntry*>* lists[2] = {&mine, &theirs};
    for (int p = 0; p < 2; ++p) {
        const int maxScroll = std::max(0, rowsOf(*lists[p]) - rows);
        if (In(PP(p, 68, 96, 192, 384), mp)) {
            const float w = GetMouseWheelMove();
            if (w != 0) *scr[p] = std::clamp(*scr[p] - static_cast<int>(w), 0, maxScroll);
        }
        if (pressed) {
            if (In(PP(p, 260, 56, 24, 24), mp)) *scr[p] = std::max(0, *scr[p] - 1);
            if (In(PP(p, 260, 84, 24, 24), mp)) *scr[p] = std::min(maxScroll, *scr[p] + 1);
        }
    }

    struct Cell { Rectangle r; const gnet::InvEntry* item; int panel; };
    std::vector<Cell> cells;
    for (int p = 0; p < 2; ++p)
        for (int rr = 0; rr < rows; ++rr)
            for (int cc = 0; cc < cols; ++cc) {
                const int idx = (*scr[p] + rr) * cols + cc;
                cells.push_back({PP(p, 68 + cc * 64, 96 + rr * 64, 60, 60), idx < static_cast<int>(lists[p]->size()) ? (*lists[p])[idx] : nullptr, p});
            }
    const Cell* hover = nullptr;
    for (auto& c : cells) if (In(c.r, mp)) hover = &c;
    for (auto& c : cells) {
        if (hover == &c) DrawRectangleRec(c.r, Color{255, 255, 255, 28});
        if (!c.item || (lootDrag_.active && lootDrag_.id == c.item->id)) continue;
        const ProtoInfo* p = ProtoOf(core, *c.item);
        const Texture2D* ic = p ? Texture(p->icon) : nullptr;
        const Rectangle ir{c.r.x + 4 * s, c.r.y + 4 * s, 52 * s, 52 * s};
        if (ic) Blit(ic, ir);
        else Text(gothic_, c.item->name.substr(0, 8), ir.x, ir.y + ir.height / 2 - 8 * s, 14 * s, kAmber, 1, ir.width);
        if (c.item->count > 1) DrawNumber("cnt" + std::to_string(c.item->id), fallouty_, ir.x, ir.y + ir.height - 14 * s, 14 * s, white, 2, ir.width, "", c.item->count);
    }

    // ---- headers: left = "WEIGHT x / limit lb" (as in the inventory), right = the target's name and weight
    Text(gothic_, loc::T("inv.weight"), PP(0, 68, 60, 0, 0).x, PP(0, 68, 60, 0, 0).y - 2 * s, 15 * s, kAmber);
    if (core.hasStats()) {
        const bool over = core.stats().carried > core.stats().carryWeight;
        std::string pre, post;
        txt::SplitNum(loc::T("inv.weight_cur", {{"max", core.stats().carryWeight}}), pre, post);
        DrawNumber("lootMyWeight", fallouty_, PP(0, 152, 60, 0, 0).x, PP(0, 152, 60, 0, 0).y, 14 * s, over ? Color{255, 90, 70, 255} : green, 0, 0, pre,
                   static_cast<int>(core.stats().carried), post);
    }
    {
        std::string nm = core.containerName();
        if (nm.empty()) nm = loc::T(corpse ? "loot.corpse" : "loot.container");
        float fs = 20 * s;
        const Rectangle nr = PP(1, 40, 20, 232, 26);
        while (fs > 10 * s && TextW(gothic_, nm, fs) > nr.width) fs -= 1;
        Text(gothic_, nm, nr.x, nr.y, fs, kAmber, 1, nr.width);
        int contW = 0;
        for (auto& e : core.container()) contW += e.weight * e.count;
        Text(gothic_, loc::T("inv.weight"), PP(1, 68, 60, 0, 0).x, PP(1, 68, 60, 0, 0).y - 2 * s, 15 * s, kAmber);
        {
            std::string pre, post;
            txt::SplitNum(loc::T("inv.weight_lb"), pre, post);
            DrawNumber("lootWeight", fallouty_, PP(1, 152, 60, 0, 0).x, PP(1, 152, 60, 0, 0).y, 14 * s, green, 0, 0, pre, contW, post);
        }
        if (core.container().empty()) Text(gothic_, loc::T("ui.empty"), PP(1, 68, 270, 192, 20).x, PP(1, 68, 270, 192, 20).y, 18 * s, kAmberDim, 1, 192 * s);
    }

    // ---- left bottom field: sort selector; trash = drop to the world
    {
        const char* names[] = {"inv.sort.type", "inv.sort.weight", "inv.sort.name"};
        const Rectangle sr = PP(0, 48, 540, 172, 24);
        Text(gothic_, loc::T(names[sortMode_]), sr.x, sr.y + 2 * s, 16 * s, kAmber, 1, sr.width);
        if (pressed && In(sr, mp)) sortMode_ = (sortMode_ + 1) % 3;
    }
    const Rectangle trashR = PP(0, 244, 528, 48, 48);

    // ---- right bottom: Done / Take All (the buttons of Container.gui, neutral art f3_chargen_age_* stretched as nine patches)
    auto button = [&](const Rectangle& r, const std::string& label, bool enabled) {
        const bool hot = enabled && In(r, mp);
        const bool down = hot && IsMouseButtonDown(MOUSE_BUTTON_LEFT);
        if (const Texture2D* t = Texture(down ? "f3_chargen_age_down.tga" : "f3_chargen_age_up.tga")) {
            NPatchInfo np{Rectangle{0, 0, static_cast<float>(t->width), static_cast<float>(t->height)}, 14, 12, 14, 12, NPATCH_NINE_PATCH};
            DrawTextureNPatch(*t, np, r, Vector2{0, 0}, 0, enabled ? WHITE : Color{150, 150, 150, 255});
        }
        if (hot) DrawRectangleRec(r, Color{255, 255, 255, 24});
        Text(gothic_, label, r.x, r.y + (r.height - 18 * s) / 2, 18 * s, enabled ? kAmber : kAmberDim, 1, r.width);
        return hot && pressed;
    };
    const std::string lblAll = !StfT(1713).empty() ? StfT(1713) : loc::T("loot.take_all");
    const std::string lblDone = !StfT(75).empty() ? StfT(75) : loc::T("loot.done");
    DrawRectangleRec(PP(1, 38, 526, 258, 54), Color{34, 32, 24, 255});   // hides the sort field / trash art of the panel under the buttons
    const Rectangle doneR = PP(1, 44, 536, 116, 34), allR = PP(1, 166, 536, 124, 34);
    if (button(allR, lblAll, !core.container().empty())) ui.commands->push_back(UiCommand{UiCommand::Take, core.containerOwner(), 0, 0});
    if (button(doneR, lblDone, true)) ui.commands->push_back(UiCommand{UiCommand::CloseContainer, 0, 0, 0});

    // ---- drag and drop / double click between the panels (Put is only offered for containers, never for corpses)
    const gnet::InvEntry* dragItem = nullptr;
    if (lootDrag_.active) for (auto* e : *lists[lootDrag_.side]) if (e->id == lootDrag_.id) dragItem = e;
    if (dragItem) {   // show where the item may go
        if (lootDrag_.side == 0 && !corpse) DrawRectangleLinesEx(panelR[1], 2, Color{120, 235, 90, 200});
        if (lootDrag_.side == 1) DrawRectangleLinesEx(panelR[0], 2, Color{120, 235, 90, 200});
    }
    if (pressed && hover && hover->item) { lootDrag_.pressed = true; lootDrag_.active = false; lootDrag_.id = hover->item->id; lootDrag_.side = hover->panel; lootDrag_.start = mp; }
    if (lootDrag_.pressed && !lootDrag_.active && IsMouseButtonDown(MOUSE_BUTTON_LEFT) &&
        std::hypot(mp.x - lootDrag_.start.x, mp.y - lootDrag_.start.y) > 6) lootDrag_.active = true;
    if (lootDrag_.pressed && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        const gnet::InvEntry* di = nullptr;
        for (auto* e : *lists[lootDrag_.side]) if (e->id == lootDrag_.id) di = e;
        if (di) {
            const bool mineSide = lootDrag_.side == 0;
            auto move = [&]() {
                if (mineSide) { if (!corpse) ui.commands->push_back(UiCommand{UiCommand::Put, di->id, 0, 0}); }
                else ui.commands->push_back(UiCommand{UiCommand::Take, core.containerOwner(), di->id, 0});
            };
            if (!lootDrag_.active) {
                if (lootClickId_ == di->id && ui.now - lootClickTime_ < 0.45) { move(); lootClickId_ = 0; }
                else { lootClickId_ = di->id; lootClickTime_ = ui.now; }
            } else if (In(panelR[mineSide ? 1 : 0], mp)) {
                move();
            } else if (mineSide && In(trashR, mp)) {
                ui.commands->push_back(UiCommand{UiCommand::Drop, di->id, 0});
            }
        }
        lootDrag_ = LootDrag{};
    }
    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT) && !lootDrag_.pressed) lootDrag_.active = false;
    if (hover && hover->item && !lootDrag_.active) Tooltip(hover->item->name, ItemText(*hover->item, ProtoOf(core, *hover->item)));
    if (dragItem) if (const ProtoInfo* p = ProtoOf(core, *dragItem)) if (const Texture2D* ic = Texture(p->icon))
        Blit(ic, Rectangle{mp.x - 32 * s, mp.y - 32 * s, 64 * s, 64 * s}, Color{255, 255, 255, 200});
}

void OrigUi::DrawOverlay() {
    if (ok_ && tipOn_ && !drag_.active) {
        const float s = 1.0f, fs = 16;
        const float sw = static_cast<float>(GetScreenWidth()), sh = static_cast<float>(GetScreenHeight());
        const Vector2 mp = GetMousePosition();
        auto wrapTip = [&](const std::string& t) { return txt::Wrap(t, 360.0f, [&](const std::string& s2) { return TextW(gothic_, s2, fs); }); };
        std::vector<std::string> lines = wrapTip(tipTitle_);
        for (std::string& l : wrapTip(tipBody_)) lines.push_back(std::move(l));
        float w = 0;
        for (auto& l : lines) w = std::max(w, TextW(gothic_, l, fs));
        w = std::min(w, 360.0f);
        Rectangle tr{std::min(mp.x + 16, sw - w - 16), std::min(mp.y + 16, sh - (lines.size() * (fs + 2) + 10) - 4), w + 12, lines.size() * (fs + 2) + 8};
        DrawRectangleRec(tr, Color{8, 8, 6, 235});
        DrawRectangleLinesEx(tr, 1, kAmberDim);
        for (size_t i = 0; i < lines.size(); ++i) Text(gothic_, lines[i], tr.x + 6, tr.y + 4 + i * (fs + 2), fs, i == 0 ? kAmber : Color{200, 200, 180, 255});
    }
    tipOn_ = false;
}

}  // namespace f3
