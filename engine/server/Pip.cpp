// Pip-Boy back end: quest log, friends list, mail. Quests and friends are part of the character (Persist.cpp; tables quests / friends), mail is kept
// per account in the store (table mail) so it reaches offline players. The client gets the complete snapshot (S2C_Pip) after every change.
#include <algorithm>
#include <ctime>
#include <fstream>
#include <sstream>
#include "Scripting.h"
#include "Server.h"
#include "common/log.h"
#include "common/utf8.h"

namespace f3 {

using json = nlohmann::json;

namespace {

std::string Lower(const std::string& s) { return utf8::Lower(s); }

// printable single-line UTF-8 text, trimmed, at most maxLen code points
std::string Clean(const std::string& in, size_t maxLen) { return utf8::SanitizeText(in, maxLen); }

constexpr size_t kMaxQuests = 100;

}  // namespace

// ------------------------------------------------------------------ helpers

static bool IsOnline(const Critter* p) { return p && p->IsPlayer() && p->peer && p->mapId != 0; }

static bool HasFriend(const Critter& c, const std::string& name) {
    const std::string k = Lower(name);
    for (const std::string& f : c.friends) if (Lower(f) == k) return true;
    return false;
}

// ------------------------------------------------------------------ quests

bool Server::QuestAdd(Critter& c, const std::string& idIn, const std::string& titleIn, const std::string& textIn) {
    if (!c.IsPlayer()) return false;
    const std::string id = Clean(idIn, 48), title = Clean(titleIn, 80), text = Clean(textIn, 600);
    if (id.empty() || c.quests.size() >= kMaxQuests) return false;
    for (const Critter::QuestEntry& q : c.quests) if (q.id == id) return false;
    Critter::QuestEntry q;
    q.id = id; q.title = title.empty() ? id : title; q.text = text; q.state = 0; q.updated = static_cast<int64_t>(std::time(nullptr));
    c.quests.push_back(std::move(q));
    TellK(c, "msg.pip.new_task", {{"title", QuestTitle(c, c.quests.back())}});
    SendPip(c);
    return true;
}

bool Server::QuestSet(Critter& c, const std::string& id, uint8_t state) {
    if (!c.IsPlayer() || state > 2) return false;
    for (Critter::QuestEntry& q : c.quests) {
        if (q.id != id) continue;
        if (q.state == state) return true;
        q.state = state;
        q.updated = static_cast<int64_t>(std::time(nullptr));
        TellK(c, state == 1 ? "msg.pip.task_done" : state == 2 ? "msg.pip.task_failed" : "msg.pip.task_reopened", {{"title", QuestTitle(c, q)}});
        const std::string qid = q.id;
        SendPip(c);
        // data/quests rewards (xp, items), once per character
        if (state == 1)
            if (const QuestDef* d = content_.FindQuest(qid))
                if (!c.vars.count("quest_reward." + qid)) {
                    c.vars["quest_reward." + qid] = 1;
                    for (const auto& [item, n] : d->items)
                        if (const Proto* ip = data_.FindProto(item)) GiveItem(c, *ip, n);
                    if (!d->items.empty()) SendInventory(c);
                    if (d->xp > 0) AddXp(c, d->xp);
                }
        return true;
    }
    return false;
}

int Server::QuestState(const Critter& c, const std::string& id) const {
    for (const Critter::QuestEntry& q : c.quests) if (q.id == id) return q.state;
    return -1;
}

// ------------------------------------------------------------------ friends

bool Server::FriendAdd(Critter& c, const std::string& nameIn) {
    if (!c.IsPlayer()) return false;
    std::string name = Clean(nameIn, 16);
    if (!AccountStore::ValidName(name)) { TellK(c, "msg.friend.invalid"); return false; }
    if (Lower(name) == Lower(c.name)) { TellK(c, "msg.friend.self"); return false; }
    if (HasFriend(c, name)) { TellK(c, "msg.friend.already", {{"name", name}}); return false; }
    if (c.friends.size() >= kMaxFriends) { TellK(c, "msg.friend.full"); return false; }
    Critter* online = world_.FindPlayerByName(name);
    if (!accounts_.Exists(name) && !IsOnline(online)) { TellK(c, "msg.friend.no_player"); return false; }
    if (online) name = online->name;   // canonical spelling
    c.friends.push_back(name);
    TellK(c, "msg.friend.added", {{"name", name}});
    SendPip(c);
    return true;
}

bool Server::FriendDel(Critter& c, const std::string& nameIn) {
    if (!c.IsPlayer()) return false;
    const std::string k = Lower(Clean(nameIn, 16));
    auto it = std::find_if(c.friends.begin(), c.friends.end(), [&](const std::string& f) { return Lower(f) == k; });
    if (it == c.friends.end()) { TellK(c, "msg.friend.not_in_list"); return false; }
    const std::string n = *it;
    c.friends.erase(it);
    TellK(c, "msg.friend.removed", {{"name", n}});
    SendPip(c);
    return true;
}

// ------------------------------------------------------------------ mail

std::vector<Server::MailMsg> Server::Mailbox(const std::string& key) {
    std::vector<MailMsg> box;
    for (const MailRow& r : store_.MailList(key)) {
        MailMsg m;
        m.id = r.id;
        m.from = Clean(r.from, 16);
        m.text = Clean(r.text, kMaxMailText);
        m.time = r.time;
        m.read = r.read;
        box.push_back(std::move(m));
    }
    return box;
}

bool Server::MailSend(Critter& c, const std::string& toIn, const std::string& textIn) {
    if (!c.IsPlayer()) return false;
    if (c.account.empty()) { TellK(c, "msg.mail.guest_send"); return false; }
    const std::string to = Clean(toIn, 16), text = Clean(textIn, kMaxMailText);
    if (!AccountStore::ValidName(to) || !accounts_.Exists(to)) { TellK(c, "msg.mail.no_player"); return false; }
    if (text.empty()) { TellK(c, "msg.mail.empty"); return false; }
    const std::string key = AccountStore::Key(to);
    if (static_cast<size_t>(store_.MailCount(key)) >= kMaxMailbox) { TellK(c, "msg.mail.box_full", {{"name", to}}); return false; }
    const uint32_t id = store_.MailAdd(key, c.name, text, static_cast<int64_t>(std::time(nullptr)));
    if (!id) { TellK(c, "msg.mail.save_failed"); return false; }
    Audit("mail_sent", c.name, {{"to", to}, {"mail_id", id}, {"length", text.size()}});
    TellK(c, "msg.mail.sent", {{"name", to}});
    for (auto& [id2, p] : world_.Critters())
        if (IsOnline(p.get()) && !p->account.empty() && AccountStore::Key(p->account) == key) {
            TellK(*p, "msg.pip.new_mail", {{"name", c.name}});
            SendPip(*p);
        }
    return true;
}

bool Server::MailRead(Critter& c, uint32_t id) {
    if (c.account.empty()) return false;
    const int r = store_.MailMarkRead(AccountStore::Key(c.account), id);
    if (r < 0) return false;
    if (r > 0) SendPip(c);
    return true;
}

bool Server::MailDelete(Critter& c, uint32_t id) {
    if (c.account.empty()) return false;
    if (!store_.MailDelete(AccountStore::Key(c.account), id)) return false;
    SendPip(c);
    return true;
}

// ------------------------------------------------------------------ snapshot + session events

void Server::SendPip(Critter& c) {
    if (!c.peer || !c.IsPlayer()) return;
    gnet::PipData d;
    for (const Critter::QuestEntry& q : c.quests) d.quests.push_back({q.id, QuestTitle(c, q), QuestText(c, q), q.state});
    for (const std::string& f : c.friends) d.friends.push_back({f, static_cast<uint8_t>(IsOnline(world_.FindPlayerByName(f)) ? 1 : 0)});
    if (!c.account.empty())
        for (const MailMsg& m : Mailbox(AccountStore::Key(c.account)))
            d.mail.push_back({m.id, m.from, m.text, static_cast<uint32_t>(std::max<int64_t>(0, m.time)), static_cast<uint8_t>(m.read ? 1 : 0)});
    Send(c, gnet::PackG(d));
}

void Server::PipOnLogin(Critter& c) {
    SendPip(c);
    if (!c.account.empty()) {
        const int unread = store_.MailUnread(AccountStore::Key(c.account));
        if (unread) TellK(c, "msg.pip.unread", {{"n", unread}});
    }
    for (auto& [id, p] : world_.Critters())
        if (p.get() != &c && IsOnline(p.get()) && HasFriend(*p, c.name)) {
            TellK(*p, "msg.friend.online", {{"name", c.name}});
            SendPip(*p);
        }
}

void Server::PipOnLogout(Critter& c) {
    for (auto& [id, p] : world_.Critters())
        if (p.get() != &c && IsOnline(p.get()) && HasFriend(*p, c.name)) {
            TellK(*p, "msg.friend.offline", {{"name", c.name}});
            SendPip(*p);
        }
}

void Server::HandlePip(Critter& c, const gnet::PipReq& m) {
    using gnet::PipOp;
    switch (static_cast<PipOp>(m.op)) {
        case PipOp::Request: SendPip(c); break;
        case PipOp::FriendAdd: FriendAdd(c, m.a); break;
        case PipOp::FriendDel: FriendDel(c, m.a); break;
        case PipOp::MailSend: MailSend(c, m.a, m.b); break;
        case PipOp::MailRead: MailRead(c, m.num); break;
        case PipOp::MailDelete: MailDelete(c, m.num); break;
        default: break;
    }
}

// ------------------------------------------------------------------ chat commands

bool Server::PipChat(Critter& c, const std::string& text) {
    if (text.size() < 2 || text[0] != '/') return false;
    std::istringstream is(text.substr(1));
    std::string cmd;
    is >> cmd;
    cmd = Lower(cmd);
    auto rest = [&]() { std::string r; std::getline(is, r); return Clean(r, kMaxMailText); };
    if (cmd == "friend") {
        std::string sub, name;
        is >> sub >> name;
        sub = Lower(sub);
        if (sub == "add") FriendAdd(c, name);
        else if (sub == "del" || sub == "remove") FriendDel(c, name);
        else TellK(c, "msg.friend.usage");
        return true;
    }
    if (cmd == "friends") {
        if (c.friends.empty()) TellK(c, "msg.friend.list_empty");
        for (const std::string& f : c.friends) TellK(c, IsOnline(world_.FindPlayerByName(f)) ? "msg.friend.list_online" : "msg.friend.list_offline", {{"name", f}});
        return true;
    }
    if (cmd == "mail") {
        std::string to;
        is >> to;
        MailSend(c, to, rest());
        return true;
    }
    if (cmd == "inbox") {
        if (c.account.empty()) { TellK(c, "msg.mail.guest_box"); return true; }
        const std::vector<MailMsg> box = Mailbox(AccountStore::Key(c.account));
        if (box.empty()) TellK(c, "msg.mail.inbox_empty");
        for (const MailMsg& m : box) TellK(c, m.read ? "msg.mail.line" : "msg.mail.line_new", {{"id", m.id}, {"from", m.from}, {"text", m.text}});
        return true;
    }
    if (cmd == "maildel") {
        uint32_t id = 0;
        is >> id;
        if (!MailDelete(c, id)) TellK(c, "msg.mail.no_message");
        return true;
    }
    if (cmd == "quests") {
        if (c.quests.empty()) TellK(c, "msg.pip.no_tasks");
        for (const Critter::QuestEntry& q : c.quests) {
            const loc::Arg state("state", L(c, q.state == 0 ? "msg.task.state.active" : q.state == 1 ? "msg.task.state.done" : "msg.task.state.failed"));
            const std::string text = QuestText(c, q);
            if (text.empty()) TellK(c, "msg.task.line", {state, {"title", QuestTitle(c, q)}});
            else TellK(c, "msg.task.line_text", {state, {"title", QuestTitle(c, q)}, {"text", text}});
        }
        return true;
    }
    return false;
}

}  // namespace f3
