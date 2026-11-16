// Character creation screen and character window drawn from the original CharacterGeneration.gui (1024x768 layout, exact
// widget tree from ParseGuiTree), the layout's TGA art and the strings of English.stf. See docs/formats/gui.md and
// docs/client-rendering.md ("Character creation screen").
#include "OrigUi.h"
#include <algorithm>
#include <bitset>
#include <cmath>
#include <cstdio>
#include "CharPreview.h"

namespace f3 {
namespace {

constexpr uint32_t kNoRef = 0xFFFFFFFFu;
const char* const kSpecialCode[7] = {"ST", "PE", "EN", "CH", "IN", "AG", "LK"};
const Color kBright{255, 232, 120, 255};

Color Argb(uint32_t c) { return Color{static_cast<unsigned char>((c >> 16) & 255), static_cast<unsigned char>((c >> 8) & 255), static_cast<unsigned char>(c & 255), static_cast<unsigned char>(c >> 24)}; }

std::string Fmt(const char* f, int a, int b = 0) { char t[96]; std::snprintf(t, sizeof t, f, a, b); return t; }

}  // namespace

void OrigUi::DrawCharScreen(const UiView& ui, bool creating, std::vector<Rectangle>& rects) {
    if (!HasCharScreen() || !ui.core || !ui.state || !ui.commands || !ui.core->hasSheet()) return;
    const ClientCore& core = *ui.core;
    UiState& st = *ui.state;
    const gnet::CharSheet& sh = core.sheet();
    const gnet::Stats* stats = core.hasStats() ? &core.stats() : nullptr;
    CharDraft& d = st.draft;
    if (creating) {
        if (!d.init) { for (int i = 0; i < 7; ++i) d.special[i] = std::clamp<int>(sh.special[i], 1, 10); d.tagMask = 0; d.traitMask = 0; d.init = true; }
    } else d.init = false;

    int sp[7];
    for (int i = 0; i < 7; ++i) sp[i] = creating ? d.special[i] : sh.special[i];
    const unsigned tags = creating ? d.tagMask : sh.tagMask;
    const unsigned traits = creating ? d.traitMask : sh.traitMask;
    int used = 0;
    for (int i = 0; i < 7; ++i) used += sp[i];
    const int left = sh.creationTotal - used;
    const int tagCount = static_cast<int>(std::bitset<16>(tags).count());
    const int traitCount = static_cast<int>(std::bitset<16>(traits).count());
    const bool ready = creating && left == 0 && tagCount == sh.tagCount;

    const float sw = static_cast<float>(GetScreenWidth()), sh_ = static_cast<float>(GetScreenHeight());
    const float s = std::min(sw / 1024.0f, sh_ / 768.0f);
    const float ox = std::floor((sw - 1024 * s) / 2), oy = std::floor((sh_ - 768 * s) / 2);
    const Vector2 mp = GetMousePosition();
    const bool click = IsMouseButtonPressed(MOUSE_BUTTON_LEFT), down = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
    const double now = GetTime();

    auto Nd = [&](const std::string& n) -> const gd::GuiNode* { auto it = charIdx_.find(n); return it == charIdx_.end() ? nullptr : it->second; };
    auto Rc = [&](const gd::GuiNode& n) { return Rectangle{ox + n.rect[0] * s, oy + n.rect[1] * s, n.w() * s, n.h() * s}; };
    auto Hit = [&](const gd::GuiNode* n) { return n && CheckCollisionPointRec(mp, Rc(*n)); };
    auto Cmd = [&](UiCommand::Type t, uint32_t a = 0, uint32_t b = 0) { ui.commands->push_back(UiCommand{t, a, b, 0}); };
    auto Hint = [&](const std::string& t) { charHint_ = t; charHintUntil_ = now + 4.0; };

    DrawRectangle(0, 0, static_cast<int>(sw) + 1, static_cast<int>(sh_) + 1, BLACK);
    rects.push_back(Rectangle{0, 0, sw, sh_});
    Blit(Texture("f3_chargen.tga"), Rectangle{ox, oy, 1024 * s, 768 * s});

    // ---- generic pieces
    auto Digits = [&](const std::string& tens, const std::string& ones, int v) {
        v = std::clamp(v, 0, 99);
        const Roll ro = RollOf(tens, v);
        auto one = [&](const std::string& name, int div) {
            if (const gd::GuiNode* n = Nd(name)) DrawDigitCell(ro, div, true, Rc(*n));   // the layout's own digit windows (shared roller cell)
        };
        one(tens, 10);
        one(ones, 1);
    };
    // draw the BtnUp / BtnDown picture children of a button (Down while `on`)
    auto BtnPic = [&](const gd::GuiNode* b, bool on, bool enabled = true) {
        if (!b) return;
        const gd::GuiNode *up = nullptr, *dn = nullptr;
        for (const gd::GuiNode& c : b->children) {
            if (c.name.rfind("BtnUp", 0) == 0) up = &c;
            else if (c.name.rfind("BtnDown", 0) == 0) dn = &c;
        }
        const gd::GuiNode* pic = on && dn ? dn : up;
        if (pic) Blit(Texture(pic->picture()), Rc(*pic), enabled ? WHITE : Color{255, 255, 255, 110});
    };
    auto Lbl = [&](const gd::GuiNode* n, const std::string& txt, Color col, int alignOverride = -1) {
        if (!n || txt.empty()) return;
        const bool fo = n->fontName.find("allouty") != std::string::npos;
        const Font& f = fo ? fallouty_ : gothic_;
        const float px = (fo ? std::max<uint32_t>(10, n->fontSize) * 1.6f : static_cast<float>(n->fontSize)) * s;
        const Rectangle r = Rc(*n);
        Text(f, txt, r.x, r.y + (r.height - px) / 2, px, col, alignOverride >= 0 ? alignOverride : static_cast<int>(n->alignment), r.width);
    };
    // label whose first integer rolls (derived statistics, HP, thresholds)
    auto NumLbl = [&](const gd::GuiNode* n, const std::string& key, const std::string& txt, Color col) {
        if (!n || txt.empty()) return;
        const bool fo = n->fontName.find("allouty") != std::string::npos;
        const Font& f = fo ? fallouty_ : gothic_;
        const float px = (fo ? std::max<uint32_t>(10, n->fontSize) * 1.6f : static_cast<float>(n->fontSize)) * s;
        const Rectangle r = Rc(*n);
        DrawNumberText(key, f, r.x, r.y + (r.height - px) / 2, px, col, static_cast<int>(n->alignment), r.width, txt);
    };
    auto StfLbl = [&](const gd::GuiNode* n, Color col) { if (n) Lbl(n, stf_.Get(n->strRef), col); };

    // ---- tabs (derived statistics / damage thresholds) and the name plate
    if (click && Hit(Nd("btnStats"))) charTab_ = 0;
    if (click && Hit(Nd("btnThresholds"))) charTab_ = 1;
    if (const gd::GuiNode* t = Nd(charTab_ == 0 ? "picTabStats" : "picTabThresholds")) Blit(Texture(t->picture()), Rc(*t));
    if (const gd::GuiNode* n = Nd("btnName")) {
        BtnPic(n, false);
        const Rectangle r = Rc(*n);
        Text(gothic_, core.playerName(), r.x + 8 * s, r.y + 5 * s, 22 * s, kBright, 1, r.width - 16 * s);
    }

    // ---- SPECIAL
    for (int i = 0; i < 7; ++i) {
        const std::string c = kSpecialCode[i];
        Digits("win" + c + "10", "win" + c + "1", sp[i]);
        if (const gd::GuiNode* n = Nd("lbl" + c)) Lbl(n, stf_.Get(40 + std::clamp(sp[i], 1, 10)), Argb(n->textColor));
        const gd::GuiNode* plus = Nd("btn" + c + "+");
        const gd::GuiNode* minus = Nd("btn" + c + "-");
        const bool canPlus = creating && sp[i] < 10 && left > 0, canMinus = creating && sp[i] > 1;
        BtnPic(plus, down && Hit(plus) && canPlus, canPlus || !creating);
        BtnPic(minus, down && Hit(minus) && canMinus, canMinus || !creating);
        if (click && creating) {
            if (Hit(plus) && canPlus) d.special[i] = sp[i] + 1;
            else if (Hit(minus) && canMinus) d.special[i] = sp[i] - 1;
        }
    }
    Digits("winCharPoints10", "winCharPoints1", creating ? left : sh.unspentSpecial);

    // ---- skills (the 13 of the layout plus Traps, which the layout has no row for: one more row below Steal)
    struct Row { Rectangle radio, name, base, total; const gd::GuiNode* btn; };
    auto SkillRow = [&](int i) -> Row {
        const int n = std::min(i + 1, 13);
        const float shift = i == 13 ? 14.0f * s : 0.0f;
        Row r{};
        r.btn = Nd("btnSkill" + std::to_string(n));
        auto get = [&](const std::string& nm) { const gd::GuiNode* g = Nd(nm); Rectangle q = g ? Rc(*g) : Rectangle{}; q.y += shift; return q; };
        r.radio = get("btnSkill" + std::to_string(n));
        r.name = get("lblSkill" + std::to_string(n) + "a");
        r.base = get("lblSkill" + std::to_string(n) + "b");
        r.total = get("lblSkill" + std::to_string(n) + "c");
        return r;
    };
    for (const char* h : {"lblSkillsHeading1", "lblSkillsHeading2", "lblSkillsHeading3", "lblSkillsHeading4", "lblSkillsHeading1a", "lblSkillsHeading1b"})
        if (const gd::GuiNode* n = Nd(h)) Lbl(n, stf_.Get(n->strRef), Argb(n->textColor));
    int hoverSkill = -1;
    const gd::GuiNode* skillFont = Nd("lblSkill1a");
    for (int i = 0; i < kSkillCount; ++i) {
        const Row r = SkillRow(i);
        if (r.radio.width <= 0 || !skillFont) continue;
        const bool tagged = (tags >> i) & 1u;
        const bool rowHot = CheckCollisionPointRec(mp, r.radio) || CheckCollisionPointRec(mp, r.name);
        if (rowHot) hoverSkill = i;
        // radio: down = tag skill
        if (const gd::GuiNode* b = r.btn) {
            const gd::GuiNode *up = nullptr, *dn = nullptr;
            for (const gd::GuiNode& c : b->children) { if (c.name == "BtnUp") up = &c; else if (c.name == "BtnDown") dn = &c; }
            const gd::GuiNode* pic = tagged ? dn : up;
            if (pic) Blit(Texture(pic->picture()), Rectangle{r.radio.x, r.radio.y + (i == 13 ? 0 : (pic->rect[1] - b->rect[1]) * s), r.radio.width, r.radio.height});
        }
        const Color base = Argb(skillFont->textColor);
        const Color col = tagged ? kBright : (rowHot && !creating ? Color{120, 235, 90, 255} : base);
        const float px = 10 * 1.6f * s;
        auto lab = [&](Rectangle q, const std::string& t, int al) { Text(fallouty_, t, q.x, q.y + (q.height - px) / 2, px, col, al, q.width); };
        lab(r.name, kSkillNames[i], 0);
        int bv = PreviewSkillBase(i, sp);
        int tv = creating ? bv + (tagged ? 20 : 0) : sh.skill[i];
        auto labRoll = [&](Rectangle q, const std::string& key, int v) {
            const Roll ro = RollOf(key, v);
            if (!ro.rolling()) { lab(q, std::to_string(v), 1); return; }
            BeginScissorMode(static_cast<int>(q.x), static_cast<int>(q.y - 1), static_cast<int>(std::ceil(q.width)), static_cast<int>(std::ceil(q.height)) + 2);
            Rectangle a = q, b = q;
            a.y -= ro.dir * ro.p * q.height; b.y += ro.dir * (1 - ro.p) * q.height;
            lab(a, std::to_string(ro.from), 1);
            lab(b, std::to_string(ro.to), 1);
            EndScissorMode();
        };
        labRoll(r.base, "skB" + std::to_string(i), bv);
        labRoll(r.total, "skT" + std::to_string(i), tv);
        if (creating) {
            if (click && rowHot) {
                if (tagged) d.tagMask &= ~(1u << i);
                else if (tagCount < sh.tagCount) d.tagMask |= (1u << i);
                else Hint("You can only tag " + std::to_string(sh.tagCount) + " skills.");
            }
        } else {
            // spend a skill point (the tag / rank bonus lives on the server), or use the skill with a click on its name
            const Rectangle pl{r.total.x + r.total.width + 6 * s, r.total.y + 1.5f * s, 18 * s, 11 * s};
            const bool can = sh.skillPoints > 0 && tv < 100;
            if (const Texture2D* t = Texture(can && down && CheckCollisionPointRec(mp, pl) ? "f3_chargen_plus_down.tga" : "f3_chargen_plus_up.tga"))
                Blit(t, pl, can ? WHITE : Color{255, 255, 255, 90});
            if (click && can && CheckCollisionPointRec(mp, pl)) Cmd(UiCommand::SpendSkill, static_cast<uint32_t>(i), 1);
            else if (click && rowHot) Cmd(UiCommand::SelectSkill, static_cast<uint32_t>(i));
        }
    }
    Digits("winTagSkills10", "winTagSkills1", creating ? sh.tagCount - tagCount : 0);

    // ---- traits (two columns of eight; the radio is down = chosen)
    for (int t = 0; t < 16; ++t) {
        const std::string key = std::to_string(t < 8 ? 1 : 2) + std::to_string(t % 8 + 1);
        const gd::GuiNode* b = Nd("btnTrait" + key);
        const gd::GuiNode* l = Nd("lblTrait" + key);
        if (!b || !l) continue;
        const gnet::ContentDefs::Trait* td = core.TraitAt(t);
        if (core.hasContent() && !td) continue;   // the server's trait list has no trait in this slot
        const bool on = (traits >> t) & 1u;
        const bool hot = Hit(b) || Hit(l);
        BtnPic(b, on);
        Lbl(l, td ? td->name : stf_.Get(159 + t), on ? kBright : (hot && creating ? Color{120, 235, 90, 255} : Argb(l->textColor)));
        if (creating && click && hot) {
            if (on) d.traitMask &= ~(1u << t);
            else if (traitCount < sh.traitMax) d.traitMask |= (1u << t);
            else Hint("You may choose up to " + std::to_string(sh.traitMax) + " traits.");
        }
    }

    // ---- derived statistics / thresholds
    if (charTab_ == 0) {
        if (const gd::GuiNode* n = Nd("lblDerivedStats")) StfLbl(n, Argb(n->textColor));
        if (const gd::GuiNode* n = Nd("lblResistances")) StfLbl(n, Argb(n->textColor));
        std::string v[15];
        if (creating) {
            v[1] = std::to_string(PreviewAp(sp)); v[2] = std::to_string(PreviewCarry(sp)) + " lb"; v[3] = std::to_string(2 * sp[1]);
            v[4] = std::to_string(sp[6]) + "%"; v[5] = std::to_string(sp[5]); v[6] = std::to_string(PreviewHealRate(sp));
            v[7] = std::to_string(std::max(0, sp[0] - 5));
        } else if (stats) {
            v[1] = std::to_string(sh.maxAp); v[2] = std::to_string(stats->carryWeight) + " lb"; v[3] = std::to_string(stats->sequence);
            v[4] = std::to_string(sh.critChance) + "%"; v[5] = std::to_string(stats->armorClass); v[6] = std::to_string(sh.healRate);
            v[7] = std::to_string(stats->meleeBonus); v[10] = std::to_string(sh.skillRate);
        }
        for (int n = 1; n <= 14; ++n) {
            const gd::GuiNode* a = Nd("lblDerivedStat" + std::to_string(n) + "a");
            const gd::GuiNode* b = Nd("lblDerivedStat" + std::to_string(n) + "b");
            if (!a || !b) continue;
            StfLbl(a, Argb(a->textColor));
            NumLbl(b, "dst" + std::to_string(n), v[n].empty() ? "-" : v[n], Argb(b->textColor));   // "-": the server does not model this value
        }
    } else if (const gd::GuiNode* w = Nd("winThresholds")) {
        for (const gd::GuiNode& c : w->children) {
            if (c.type != gd::GuiNode::Label) continue;
            const bool value = c.name.size() > 5 && c.name.compare(c.name.size() - 5, 5, "Value") == 0;
            if (value) NumLbl(&c, "thr" + c.name, creating ? "0" : "-", Argb(c.textColor));
            else Lbl(&c, stf_.Get(c.strRef), Argb(c.textColor));
        }
    }
    {   // hit points under the character window
        const gd::GuiNode* h1 = Nd("lblHP1");
        const gd::GuiNode* h2 = Nd("lblHP2");
        StfLbl(h1, h1 ? Argb(h1->textColor) : WHITE);
        if (h2) NumLbl(h2, "charHp", creating ? std::to_string(PreviewMaxHp(sp)) : (stats ? Fmt("%d/%d", stats->hp, stats->maxHp) : "-"), Argb(h2->textColor));
    }

    // ---- information block where the original shows the character model (no model preview in this client)
    // (belongs to the Stats tab: the Thresholds page draws its own table over the same area)
    if (const gd::GuiNode* m = charTab_ == 0 ? Nd("winModel") : nullptr) {
        const Rectangle r = Rc(*m);
        const float px = 17 * s, lh = 22 * s;
        float y = r.y + 14 * s;
        int lineNo = 0;
        auto line = [&](const std::string& t, Color c) { DrawNumberText("info" + std::to_string(lineNo++), fallouty_, r.x + 12 * s, y, px, c, 0, 0, t); y += lh; };
        const Color txt{200, 190, 120, 255};
        line(core.playerName(), kBright);
        y += 6 * s;
        if (creating) {
            line("New character", txt);
            y += 6 * s;
            line(Fmt("Points to spend: %d", left), left == 0 ? Color{120, 235, 90, 255} : kBright);
            line(Fmt("Tag skills: %d of %d", tagCount, sh.tagCount), tagCount == sh.tagCount ? Color{120, 235, 90, 255} : kBright);
            line(Fmt("Traits: %d of %d", traitCount, sh.traitMax), txt);
        } else {
            line(Fmt("Level %d", sh.level), txt);
            line(sh.xpNext ? "XP " + std::to_string(sh.xp) + " / " + std::to_string(sh.xpNext) : "XP " + std::to_string(sh.xp) + " (max)", txt);
            y += 6 * s;
            line(Fmt("Skill points: %d", sh.skillPoints), sh.skillPoints ? Color{120, 235, 90, 255} : txt);
            line(Fmt("Perk points: %d", sh.perkPoints), txt);
            y += 10 * s;
            Text(fallouty_, "Click a skill to use it,", r.x + 12 * s, y, 14 * s, Color{150, 140, 90, 255}); y += 17 * s;
            Text(fallouty_, "then click a target.", r.x + 12 * s, y, 14 * s, Color{150, 140, 90, 255}); y += 17 * s;
            Text(fallouty_, "[+] spends a skill point.", r.x + 12 * s, y, 14 * s, Color{150, 140, 90, 255});
        }
    }

    // ---- Options / Done / Cancel
    struct Btn { const char* win; const char* btn; };
    const Btn btns[3] = {{"winOptions", "btnOptions"}, {"winDone", "btnDone"}, {"winCancel", "btnCancel"}};
    for (int k = 0; k < 3; ++k) {
        const gd::GuiNode* w = Nd(btns[k].win);
        const gd::GuiNode* b = Nd(btns[k].btn);
        if (!w || !b) continue;
        const bool hot = Hit(w) || Hit(b);
        BtnPic(b, hot && down);
        if (k == 1 && creating && !ready) { const Rectangle r = Rc(*w); DrawRectangleRec(r, Color{0, 0, 0, 90}); }
        if (!(click && hot)) continue;
        if (k == 0) Hint("Options are not available in this client.");
        else if (k == 1) {
            if (!creating) Cmd(UiCommand::ToggleChar);
            else if (ready) Cmd(UiCommand::CharCreate);
            else if (left != 0) Hint(left > 0 ? "Spend all character points first (" + std::to_string(left) + " left)." : "Too many SPECIAL points.");
            else Hint("Choose " + std::to_string(sh.tagCount) + " tag skills first.");
        } else Cmd(creating ? UiCommand::Quit : UiCommand::ToggleChar);
    }

    // ---- tooltip on the parchment: title, text and the picture of whatever is under the mouse
    {
        const gd::GuiNode* best = nullptr;
        float bestArea = 1e12f;
        for (const gd::GuiNode* n : charFlat_) {
            if (n->tipTitleRef == kNoRef || n->tipTitleRef == 0 || n->hidden) continue;
            const bool thr = n->name.rfind("lblBody", 0) == 0 || n->name.rfind("lblHead", 0) == 0 || n->name.rfind("lblDT", 0) == 0 || n->name.rfind("lblDamage", 0) == 0;
            const bool der = n->name.rfind("lblDerived", 0) == 0 || n->name.rfind("lblResist", 0) == 0;
            if ((thr && charTab_ != 1) || (der && charTab_ != 0)) continue;
            if (n->name == "btnStats" || n->name == "btnThresholds" || n->name == "btnName") continue;
            const Rectangle r = Rc(*n);
            if (!CheckCollisionPointRec(mp, r)) continue;
            if (r.width * r.height < bestArea) { bestArea = r.width * r.height; best = n; }
        }
        std::string title, body, img;
        if (now < charHintUntil_) title = charHint_;
        else if (best) {
            // the layouts print the pair as "title/text": the first id is the heading ("Strength"), the second the description
            title = stf_.Get(best->tipTitleRef);
            if (best->tipTextRef != kNoRef && best->tipTextRef != best->tipTitleRef) body = stf_.Get(best->tipTextRef);
            img = best->tipImage;
            if (core.hasContent() && (best->name.rfind("lblTrait", 0) == 0 || best->name.rfind("btnTrait", 0) == 0) && best->name.size() >= 10) {
                const int tens = best->name[8] - '0', dig = best->name[9] - '0';   // "lblTrait<1|2><1..8>": slot (tens - 1) * 8 + digit - 1
                if (tens >= 1 && tens <= 2 && dig >= 1 && dig <= 8)
                    if (const gnet::ContentDefs::Trait* td = core.TraitAt((tens - 1) * 8 + dig - 1)) { title = td->name; body = td->desc; img.clear(); }
            }
        } else if (hoverSkill == 13) title = "Traps";
        const Color ink{20, 14, 6, 255};
        if (const gd::GuiNode* n = Nd("lblToolTipTitle")) {
            const Rectangle r = Rc(*n);
            const float px = (now < charHintUntil_ ? 20.0f : 28.0f) * s;
            Text(gothic_, title, r.x, r.y + 2 * s, px, now < charHintUntil_ ? Color{120, 20, 10, 255} : ink, 0, r.width);
        }
        if (!body.empty()) {
            if (const gd::GuiNode* n = Nd("lblToolTip")) {   // word wrap into the text area
                const Rectangle r = Rc(*n);
                const float px = 15 * s;
                std::string cur;
                float y = r.y;
                size_t pos = 0;
                while (pos < body.size()) {
                    size_t sp2 = body.find(' ', pos);
                    const std::string word = body.substr(pos, sp2 == std::string::npos ? std::string::npos : sp2 - pos + 1);
                    pos = sp2 == std::string::npos ? body.size() : sp2 + 1;
                    if (!cur.empty() && TextW(gothic_, cur + word, px) > r.width) { Text(gothic_, cur, r.x, y, px, ink); y += px + 2 * s; cur.clear(); }
                    cur += word;
                }
                if (!cur.empty()) Text(gothic_, cur, r.x, y, px, ink);
            }
        }
        if (!img.empty()) if (const gd::GuiNode* n = Nd("picToolTip")) Blit(Texture(img), Rc(*n));
    }
}

}  // namespace f3
