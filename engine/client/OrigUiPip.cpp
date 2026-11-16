// Pip-Boy window (key P) drawn over the original art: f3_pipboy.tga (referenced by extracted\Interface\PipBoy.gui, 1024x609 layout units) with
// the two green screens of the layout (list pane (37,312)-(577,593), detail pane (37,27)-(577,286)) and the four plates on the right used as
// tabs. The demo has no Status / Data / Automaps screens, so the plates are re-purposed: Alarm = friends, Tasks = quests, Maps = (empty),
// Downloads = mail. Data comes from the server snapshot (ClientCore::pip()); every change is a UiCommand::PipOp. See docs/pipboy.md.
#include "OrigUi.h"
#include <algorithm>
#include "TextUtil.h"
#include "common/Localization.h"
#include "common/utf8.h"
#include <cmath>
#include <ctime>

namespace f3 {
namespace {

const Color kGreen{110, 235, 100, 255};
const Color kGreenDim{60, 140, 56, 255};
const Color kGreenBright{190, 255, 170, 255};
const Color kRed{235, 110, 90, 255};
const char* const kTabNames[4] = {"pip.tab0", "pip.tab1", "pip.tab2", "pip.tab3"};   // locale keys
// the four plates of f3_pipboy.tga (layout units of the 1024x609 sheet)
const Rectangle kTabRect[4] = {{746, 461, 142, 31}, {746, 495, 142, 31}, {746, 529, 142, 31}, {746, 565, 142, 31}};
const Rectangle kListPane{37, 312, 540, 281};
const Rectangle kDetailPane{37, 27, 540, 259};

std::string TimeText(uint32_t t) {
    if (!t) return std::string();
    const std::time_t tt = static_cast<std::time_t>(t);
    char b[48] = "";
    if (const std::tm* tm = std::localtime(&tt)) std::strftime(b, sizeof b, "%Y-%m-%d %H:%M", tm);
    return b;
}

}  // namespace

std::vector<std::string> OrigUi::WrapText(const Font& f, const std::string& text, float size, float maxW) const {
    return txt::Wrap(text, maxW, [&](const std::string& s) { return TextW(f, s, size); });
}

bool OrigUi::HasPip() const { return ok_ && havePip_; }

void OrigUi::DrawPipBoy(const UiView& ui, std::vector<Rectangle>& rects) {
    if (!HasPip() || !haveFonts_ || !ui.state || !ui.state->pipboy || !ui.core || !ui.commands) return;
    const Texture2D* bg = Texture("f3_pipboy.tga");
    if (!bg) return;
    const gnet::PipData& pip = ui.core->pip();
    const float sw = static_cast<float>(GetScreenWidth()), sh = static_cast<float>(GetScreenHeight());
    const float k = std::min(sw / 1024.0f, sh / 768.0f);
    const Rectangle win{std::floor(sw / 2 - 512 * k), std::floor(sh / 2 - 304.5f * k), 1024 * k, 609 * k};
    rects.push_back(win);
    Blit(bg, win);
    auto L = [&](Rectangle r) { return Rectangle{win.x + r.x * k, win.y + r.y * k, r.width * k, r.height * k}; };
    const Vector2 mp = GetMousePosition();
    const bool click = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    const float fs = 15 * k, lh = fs + 5 * k;

    // ---- tabs (the plates on the right)
    int unread = 0;
    for (const gnet::PipData::Mail& m : pip.mail) if (!m.read) ++unread;
    for (int i = 0; i < 4; ++i) {
        const Rectangle r = L(kTabRect[i]);
        const bool hot = CheckCollisionPointRec(mp, r), cur = ui.state->pipTab == i;
        if (cur) DrawRectangleRec(r, Color{30, 70, 26, 110});
        else if (hot) DrawRectangleRec(r, Color{255, 255, 255, 30});
        DrawRectangleLinesEx(r, cur ? 2.0f : 1.0f, cur ? kGreen : Color{20, 28, 16, 200});
        std::string label = loc::T(kTabNames[i]);
        if (i == 3 && unread) label += " (" + std::to_string(unread) + ")";
        DrawNumberText("pipTab" + std::to_string(i), fallouty_, r.x, r.y + (r.height - 15 * k) / 2 - 1, 15 * k, cur || hot ? kGreenBright : kGreen, 1, r.width, label);   // the unread count rolls
        if (hot && click && ui.state->pipTab != i) { ui.state->pipTab = i; ui.state->pipSel.clear(); pipScroll_ = 0; }
    }

    // ---- list entries of the current tab
    struct Row { std::string key, text; Color col = kGreen; };
    std::vector<Row> rows;
    if (ui.state->pipTab == 0) {
        for (const gnet::PipData::Friend& f : pip.friends) rows.push_back({f.name, f.name + "   " + loc::T(f.online ? "pip.online_tag" : "pip.offline_tag"), f.online ? kGreenBright : kGreenDim});
    } else if (ui.state->pipTab == 1) {
        for (const gnet::PipData::Quest& q : pip.quests) {
            const std::string st = q.state == 0 ? std::string() : "   " + loc::T(q.state == 1 ? "pip.q.done" : "pip.q.failed");
            rows.push_back({q.id, q.title + st, q.state == 0 ? kGreenBright : q.state == 1 ? kGreenDim : kRed});
        }
    } else if (ui.state->pipTab == 3) {
        for (auto it = pip.mail.rbegin(); it != pip.mail.rend(); ++it) {
            std::string t = it->text;
            if (utf8::Count(t) > 44) t = utf8::Truncate(t, 41) + "...";
            rows.push_back({std::to_string(it->id), std::string(it->read ? "  " : "* ") + it->from + ": " + t, it->read ? kGreenDim : kGreenBright});
        }
    }
    const Rectangle lp = L(kListPane), dp = L(kDetailPane);
    const float pad = 14 * k;
    const float rowH = 24 * k;
    const int visible = std::max(1, static_cast<int>((lp.height - 2 * pad) / rowH));
    const int overflow = std::max(0, static_cast<int>(rows.size()) - visible);
    if (CheckCollisionPointRec(mp, lp) && overflow > 0) pipScroll_ -= static_cast<int>(std::lround(GetMouseWheelMove()));
    pipScroll_ = std::clamp(pipScroll_, 0, overflow);
    BeginScissorMode(static_cast<int>(lp.x), static_cast<int>(lp.y), static_cast<int>(lp.width), static_cast<int>(lp.height));
    if (rows.empty()) {
        const std::string e = loc::T(ui.state->pipTab == 0 ? "pip.empty.friends" : ui.state->pipTab == 1 ? "pip.empty.tasks" : ui.state->pipTab == 2 ? "pip.empty.maps" : "pip.empty.mail");
        Text(gothic_, e, lp.x + pad, lp.y + pad, fs, kGreenDim);
    }
    for (int i = 0; i < visible && pipScroll_ + i < static_cast<int>(rows.size()); ++i) {
        const Row& r = rows[pipScroll_ + i];
        const Rectangle rr{lp.x + pad * 0.5f, lp.y + pad + i * rowH, lp.width - pad, rowH};
        const bool hot = CheckCollisionPointRec(mp, rr) && CheckCollisionPointRec(mp, lp), sel = r.key == ui.state->pipSel;
        if (sel) DrawRectangleRec(rr, Color{40, 100, 36, 140});
        else if (hot) DrawRectangleRec(rr, Color{40, 100, 36, 70});
        Text(gothic_, r.text, rr.x + 6 * k, rr.y + 3 * k, fs, r.col);
        if (hot && click) {
            ui.state->pipSel = r.key;
            if (ui.state->pipTab == 3)   // opening a mail reads it
                for (const gnet::PipData::Mail& m : pip.mail)
                    if (std::to_string(m.id) == r.key && !m.read) ui.commands->push_back(UiCommand{UiCommand::PipOp, static_cast<uint32_t>(gnet::PipOp::MailRead), m.id, 0, std::string()});
        }
    }
    EndScissorMode();
    if (overflow > 0) {
        if (pipScroll_ > 0) Text(gothic_, "^", lp.x, lp.y + 2 * k, fs, kGreen, 2, lp.width - 6 * k);
        if (pipScroll_ < overflow) Text(gothic_, "v", lp.x, lp.y + lp.height - lh, fs, kGreen, 2, lp.width - 6 * k);
    }

    // ---- detail pane + its buttons
    struct Btn { std::string label; uint32_t op, num; std::string name; };
    std::vector<Btn> btns;
    std::string title, meta, body;
    if (ui.state->pipTab == 0) {
        title = loc::T("pip.title.friends");
        for (const gnet::PipData::Friend& f : pip.friends)
            if (f.name == ui.state->pipSel) {
                title = f.name; meta = loc::T(f.online ? "pip.status.online" : "pip.status.offline"); body = loc::T("pip.write_hint", {{"name", f.name}});
                btns.push_back({loc::T("pip.btn.remove"), static_cast<uint32_t>(gnet::PipOp::FriendDel), 0, f.name});
                btns.push_back({loc::T("pip.btn.write"), 0xFFFFFFFFu, 0, f.name});
            }
        if (ui.state->pipSel.empty()) body = loc::T("pip.select_friend");
    } else if (ui.state->pipTab == 1) {
        title = loc::T("pip.title.tasks");
        for (const gnet::PipData::Quest& q : pip.quests)
            if (q.id == ui.state->pipSel) { title = q.title; meta = loc::T(q.state == 0 ? "pip.q.active" : q.state == 1 ? "pip.q.completed" : "pip.q.failed_state"); body = q.text; }
        if (ui.state->pipSel.empty()) body = pip.quests.empty() ? std::string() : loc::T("pip.select_task");
    } else if (ui.state->pipTab == 2) {
        title = loc::T("pip.title.maps");
        body = loc::T("pip.no_automaps");
    } else {
        title = loc::T("pip.title.mail");
        for (const gnet::PipData::Mail& m : pip.mail)
            if (std::to_string(m.id) == ui.state->pipSel) {
                title = loc::T("pip.from", {{"name", m.from}}); meta = TimeText(m.time) + (m.read ? std::string() : "   " + loc::T("pip.new_tag")); body = m.text;
                if (!m.read) btns.push_back({loc::T("pip.btn.mark_read"), static_cast<uint32_t>(gnet::PipOp::MailRead), m.id, std::string()});
                btns.push_back({loc::T("pip.btn.delete"), static_cast<uint32_t>(gnet::PipOp::MailDelete), m.id, std::string()});
                btns.push_back({loc::T("pip.btn.reply"), 0xFFFFFFFFu, 0, m.from});
            }
        if (ui.state->pipSel.empty()) body = loc::T("pip.select_mail", {{"n", pip.mail.size()}, {"unread", unread}});
    }
    Text(fallouty_, title, dp.x + pad, dp.y + pad, 20 * k, kGreenBright);
    if (!meta.empty()) Text(gothic_, meta, dp.x + pad, dp.y + pad + 26 * k, fs, kGreenDim);
    const float bodyY = dp.y + pad + (meta.empty() ? 30 : 50) * k;
    const float btnH = 26 * k, btnY = dp.y + dp.height - pad - btnH;
    const std::vector<std::string> lines = WrapText(gothic_, body, fs, dp.width - 2 * pad);
    for (size_t i = 0; i < lines.size(); ++i) {
        const float y = bodyY + i * lh;
        if (y + lh > btnY) break;
        Text(gothic_, lines[i], dp.x + pad, y, fs, kGreen);
    }
    float bx = dp.x + pad;
    for (const Btn& b : btns) {
        const float bw = TextW(gothic_, b.label, fs) + 24 * k;
        const Rectangle r{bx, btnY, bw, btnH};
        const bool hot = CheckCollisionPointRec(mp, r);
        DrawRectangleRec(r, hot ? Color{40, 100, 36, 200} : Color{16, 40, 14, 220});
        DrawRectangleLinesEx(r, 1, kGreen);
        Text(gothic_, b.label, r.x, r.y + (r.height - fs) / 2 - 1, fs, hot ? kGreenBright : kGreen, 1, r.width);
        if (hot && click) {
            ui.commands->push_back(UiCommand{UiCommand::PipOp, b.op, b.num, 0, b.name});   // op 0xFFFFFFFF = Reply / Write mail (main.cpp opens the chat line with "/mail <name> ")
            if (b.op == static_cast<uint32_t>(gnet::PipOp::MailDelete) || b.op == static_cast<uint32_t>(gnet::PipOp::FriendDel)) ui.state->pipSel.clear();
        }
        bx += bw + 10 * k;
    }
    Text(gothic_, loc::T("pip.close"), dp.x, dp.y + dp.height - 24 * k, 12 * k, kGreenDim, 2, dp.width - 30 * k);
}

}  // namespace f3
