#include "OrigUi.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "common/log.h"
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

const char* ModeName(int type) {
    switch (type) {
        case 1: return "BURST";
        case 2: return "SINGLE";
        case 6: return "THRUST";
        case 8: return "SWING";
        case 9: return "THROW";
        default: return "ATTACK";
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
    char b[160];
    std::snprintf(b, sizeof b, "Weight: %d lb", e.weight * e.count);
    t += b;
    if (p && p->cls == "weapon") {
        std::snprintf(b, sizeof b, "\nDamage: %d-%d   Range: %d", static_cast<int>(p->dmgMin), static_cast<int>(p->dmgMax), p->range);
        t += b;
        if (!p->modes.empty()) {
            t += "\nModes:";
            for (auto& m : p->modes) t += std::string(" ") + ModeName(m.type);
        }
    } else if (p && p->cls == "armor") {
        std::snprintf(b, sizeof b, "\nDamage threshold: %d   Armor class: %d", p->armor[0] / 2, p->armor[1] / 4);
        t += b;
    } else if (p && p->cls == "ammo") {
        t += "\nAmmunition";
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
    if (!Texture("f3_actionbar.tga") || !Texture("f3_inventory.tga")) return fail("interface textures missing");
    const std::string fd = gameDir + "/Fonts/";
    fallouty_ = LoadFontEx((fd + "r_fallouty.ttf").c_str(), 32, nullptr, 0);
    gothic_ = LoadFontEx((fd + "TT0807M_.TTF").c_str(), 32, nullptr, 0);
    haveFonts_ = fallouty_.texture.id != 0 && gothic_.texture.id != 0;
    if (!haveFonts_) return fail("fonts missing (Fonts\\r_fallouty.ttf, TT0807M_.TTF)");
    SetTextureFilter(fallouty_.texture, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(gothic_.texture, TEXTURE_FILTER_BILINEAR);
    ok_ = true;
    log::Info("original UI: action bar (%zu widgets) and inventory (%zu widgets) loaded", bar_.widgets.size(), inv_.widgets.size());
    return true;
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

float OrigUi::TextW(const Font& f, const std::string& s, float size) const { return MeasureTextEx(f, s.c_str(), size, 1).x; }

void OrigUi::Text(const Font& f, const std::string& s, float x, float y, float size, Color c, int align, float w) {
    if (s.empty()) return;
    if (align == 1) x += (w - TextW(f, s, size)) / 2;
    else if (align == 2) x += w - TextW(f, s, size);
    DrawTextEx(f, s.c_str(), Vector2{std::floor(x), std::floor(y)}, size, 1, c);
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
            if (!p->modes.empty()) { mode = ModeName(p->modes[0].type); ap = std::to_string(ModeAp(p->modes[0].ap)) + "AP"; }
            else { mode = "MELEE"; }
        } else if (!item && i == 0) {
            mode = "UNARMED";
        }
        const float fs = 22 * s;
        Text(gothic_, mode, b.x * s + ox + 8 * s, b.y * s + oy + 4 * s, fs, kAmber, 2, 164 * s - 16 * s);
        Text(gothic_, ap, b.x * s + ox + 8 * s, b.y * s + oy + 56 * s, fs, kAmber);
        if (p && p->cls == "weapon" && p->range > 1) {
            bool ammo = false;
            for (auto& e : core.inventory()) if (const ProtoInfo* q = ProtoOf(core, e)) if (q->cls == "ammo") ammo = true;
            const Texture2D* at = Texture(ammo ? "f3_actionbar_ammo_on.tga" : "f3_actionbar_ammo_off.tga");
            if (at) Blit(at, P(b.x + 160, b.y + 74, at->width * 1.3f, at->height * 1.3f));
        }
        if (In(br, mp)) {
            if (item) Tooltip(item->name, ItemText(*item, p));
            else Tooltip(active ? "Active hand (empty)" : "Spare hand (empty)", active ? "Fists. Press B or click the other hand to switch." : "Click here or press B to switch hands.");
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
    auto digits = [&](float ly, int v) {
        char b[8];
        std::snprintf(b, sizeof b, "%03d", std::clamp(v, 0, 999));
        for (int d = 0; d < 3; ++d) {
            Rectangle r = P(314 + 14 * d, ly, 14, 18);
            Text(fallouty_, std::string(1, b[d]), r.x, r.y + 1 * s, 20 * s, Color{120, 235, 90, 255}, 1, r.width);
        }
    };
    digits(30, hp);
    digits(79, static_cast<int>(std::lround(ap)));
    digits(131, ac);

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
        char b[16];
        std::snprintf(b, sizeof b, "%d", static_cast<int>(std::lround(ap)));
        Text(fallouty_, b, P(625, 42, 24, 14).x, P(625, 42, 24, 14).y, 14 * s, Color{120, 235, 90, 255}, 1, 24 * s);
    }

    // ---- buttons
    struct Btn { const char* name; const char* label; UiCommand::Type cmd; bool on; };
    const Btn btns[] = {{"btnInventory", "Inventory", UiCommand::ToggleInventory, st.inventory},
                        {"btnCharacter", "Character", UiCommand::ToggleChar, st.charSheet},
                        {"btnOptions", "Options", UiCommand::SetMode, false}};
    for (auto& bt : btns) {
        int bi;
        Rectangle b = LRect(bar_, bt.name, 0, &bi);
        if (bi < 0) continue;
        Rectangle r = P(b.x, b.y, b.width, b.height);
        const bool hot = In(r, mp);
        Blit(Texture(bt.on || (hot && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) ? "f3_button_down.tga" : "f3_button_up.tga"), r);
        Text(gothic_, bt.label, r.x + 42 * s, r.y + 8 * s, 18 * s, hot ? Color{255, 230, 130, 255} : kAmber);
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
                    std::string cur;
                    size_t pos = 0;
                    while (pos < line.size()) {
                        size_t sp = line.find(' ', pos);
                        std::string word = line.substr(pos, sp == std::string::npos ? std::string::npos : sp - pos + 1);
                        pos = sp == std::string::npos ? line.size() : sp + 1;
                        while (TextW(gothic_, word, fs) > maxW && word.size() > 1) {
                            size_t n = word.size();
                            while (n > 1 && TextW(gothic_, word.substr(0, n), fs) > maxW) --n;
                            if (!cur.empty()) { wrapped_.emplace_back(cur, c); cur.clear(); }
                            wrapped_.emplace_back(word.substr(0, n), c);
                            word = word.substr(n);
                        }
                        if (!cur.empty() && TextW(gothic_, cur + word, fs) > maxW) { wrapped_.emplace_back(cur, c); cur.clear(); }
                        cur += word;
                    }
                    if (!cur.empty()) wrapped_.emplace_back(cur, c);
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
            if (chatScroll_ > 0) Text(gothic_, "more below", area.x + area.width - 6 * s, inR.y - 12 * s, 10 * s, kAmberDim, 2);
        }
        if (ui.chatInputActive) {
            DrawRectangleRec(inR, Color{30, 26, 8, 200});
            const std::string t = ui.chatInput + ((static_cast<int>(ui.now * 2) & 1) ? "_" : "");
            const float tw = TextW(gothic_, t, fs);
            Text(gothic_, t, inR.x + 2 * s - std::max(0.0f, tw - (inR.width - 8 * s)), inR.y, fs, WHITE);
        } else {
            Text(gothic_, "Enter: chat", inR.x + 2 * s, inR.y, fs, Color{110, 95, 55, 255});
        }
        // cursor mode (RMB toggles Move/Examine, A = Attack): small unobtrusive label
        {
            const char* mn = st.mode == CursorMode::Attack ? "Attack" : st.mode == CursorMode::Examine ? "Examine" : "Move";
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
            std::snprintf(b, sizeof b, "BREAK  %.1fs", rem);
            Text(gothic_, b, bt.x + 6, bt.y - 1 * s, 14 * s, WHITE);
        } else {
            Text(gothic_, core.InBattle() ? "IN BATTLE - ready" : "ready", bt.x + 6, bt.y - 1 * s, 14 * s, Color{120, 170, 120, 255});
        }
        DrawRectangleLinesEx(bt, 1, locked ? Color{200, 60, 60, 255} : Color{80, 60, 60, 255});
        if (core.waitKnown() && core.lastResult() != gnet::ActionResult::Ok && core.lastResult() != gnet::ActionResult::None && ui.now - core.lastResultTime() < 1.5) {
            const bool mv = core.lastAction() == static_cast<uint8_t>(GMsgRt::C2S_MoveToF) || core.lastAction() == static_cast<uint8_t>(Msg::C2S_MoveTo);
            const char* why = core.lastResult() == gnet::ActionResult::Busy ? (mv ? "busy - walking as soon as ready" : "busy") : core.lastResult() == gnet::ActionResult::NoAp ? "not enough AP" :
                              core.lastResult() == gnet::ActionResult::Dead ? "dead" : "invalid";
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
    const Vector2 mp = GetMousePosition();
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
    const SlotDef sdefs[] = {{gnet::SlotHead, "HEAD", 72, 164, "Head"}, {gnet::SlotBody, "BODY", 212, 216, "Body"},
                             {gnet::SlotHand1, "HAND 1", 348, 268, "Hand1"}, {gnet::SlotHand2, "HAND 2", 488, 320, "Hand2"}};
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
                const std::string c = std::to_string(rg.item->count);
                Text(fallouty_, c, ir.x, ir.y + ir.height - 14 * s, 14 * s, white, 2, ir.width);
            }
        }
    }
    const float labelY[] = {32, 170, 308, 448};
    for (int i = 0; i < 4; ++i) Text(gothic_, sdefs[i].label, P(312, labelY[i], 124, 24).x, P(312, labelY[i], 124, 24).y + 2 * s, 17 * s, kAmber, 1, 124 * s);

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
    Text(gothic_, "WEIGHT", P(68, 60, 0, 0).x, P(68, 60, 0, 0).y - 2 * s, 15 * s, kAmber);
    if (core.hasStats()) {
        char b[48];
        std::snprintf(b, sizeof b, "%u / %u lb", core.stats().carried, core.stats().carryWeight);
        const bool over = core.stats().carried > core.stats().carryWeight;
        Text(fallouty_, b, P(152, 60, 0, 0).x, P(152, 60, 0, 0).y, 14 * s, over ? Color{255, 90, 70, 255} : Color{120, 235, 90, 255});
        const char* sp[] = {"ST", "PE", "EN", "CH", "IN", "AG", "LK"};
        for (int i = 0; i < 7; ++i) {
            Text(gothic_, sp[i], P(468, 68 + 12 * i, 0, 0).x, P(468, 68 + 12 * i, 0, 0).y - 3 * s, 14 * s, kAmber);
            Text(fallouty_, std::to_string(core.stats().special[i]), P(496, 68 + 12 * i, 0, 0).x, P(496, 68 + 12 * i, 0, 0).y, 13 * s, Color{120, 235, 90, 255});
        }
        std::snprintf(b, sizeof b, "HP %d / %d", core.stats().hp, core.stats().maxHp);
        Text(fallouty_, b, P(560, 68, 0, 0).x, P(560, 68, 0, 0).y, 13 * s, Color{120, 235, 90, 255});
        std::snprintf(b, sizeof b, "Armor class %d", core.stats().armorClass);
        Text(fallouty_, b, P(560, 82, 0, 0).x, P(560, 82, 0, 0).y, 13 * s, Color{120, 235, 90, 255});
        std::snprintf(b, sizeof b, "Level %d", core.stats().level);
        Text(fallouty_, b, P(560, 96, 0, 0).x, P(560, 96, 0, 0).y, 13 * s, Color{120, 235, 90, 255});
    }
    if (core.self()) Text(fallouty_, core.self()->name, P(464, 32, 0, 0).x, P(464, 32, 0, 0).y, 18 * s, kAmber);
    for (auto& d : sdefs) {
        const gnet::InvEntry* it = nullptr;
        for (auto& e : all) if (e.equipped == d.slot) it = &e;
        const float x = 468, y = d.ey;
        Text(gothic_, std::string(d.label) + ":", P(x, y + 2, 0, 0).x, P(x, y + 2, 0, 0).y, 14 * s, kAmberDim);
        if (!it) { Text(fallouty_, "(empty)", P(x + 56, y, 0, 0).x, P(x + 56, y, 0, 0).y, 13 * s, kAmberDim); continue; }
        const ProtoInfo* p = ProtoOf(core, *it);
        Text(fallouty_, it->name, P(x + 56, y, 0, 0).x, P(x + 56, y, 0, 0).y, 13 * s, Color{120, 235, 90, 255});
        char b[96] = "";
        if (p && p->cls == "weapon") {
            int ammo = 0;
            for (auto& e : all) if (const ProtoInfo* q = ProtoOf(core, e)) if (q->cls == "ammo") ammo += e.count;
            std::snprintf(b, sizeof b, "Damage %d-%d    Range %d    Ammo %d", static_cast<int>(p->dmgMin), static_cast<int>(p->dmgMax), p->range, ammo);
        } else if (p && p->cls == "armor") {
            std::snprintf(b, sizeof b, "Threshold %d    Armor class %d", p->armor[0] / 2, p->armor[1] / 4);
        }
        Text(fallouty_, b, P(x + 56, y + 16, 0, 0).x, P(x + 56, y + 16, 0, 0).y, 12 * s, kAmber);
    }
    // sort selector (bottom left field) and drop target
    {
        const char* names[] = {"Sort: type", "Sort: weight", "Sort: name"};
        Rectangle sr = P(48, 540, 172, 24);
        Text(gothic_, names[sortMode_], sr.x, sr.y + 2 * s, 16 * s, kAmber, 1, sr.width);
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

void OrigUi::DrawOverlay() {
    if (ok_ && tipOn_ && !drag_.active) {
        const float s = 1.0f, fs = 16;
        const float sw = static_cast<float>(GetScreenWidth()), sh = static_cast<float>(GetScreenHeight());
        const Vector2 mp = GetMousePosition();
        std::vector<std::string> lines{tipTitle_};
        size_t pos = 0;
        while (pos <= tipBody_.size()) {
            size_t e = tipBody_.find('\n', pos);
            if (e == std::string::npos) e = tipBody_.size();
            lines.push_back(tipBody_.substr(pos, e - pos));
            pos = e + 1;
        }
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
