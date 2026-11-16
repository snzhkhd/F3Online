// Pip-Boy back end: quest log, friends list, mail. Quests and friends are part of the character (Persist.cpp), mail is kept per account in
// <saves>/mail/<account key>.json so it reaches offline players. The client gets the complete snapshot (S2C_Pip) after every change.
#include <algorithm>
#include <ctime>
#include <fstream>
#include <sstream>
#include "Scripting.h"
#include "Server.h"
#include "common/log.h"

namespace f3 {

using json = nlohmann::json;

namespace {

std::string Lower(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

// printable single-line text, trimmed, at most maxLen bytes
std::string Clean(const std::string& in, size_t maxLen) {
    std::string out;
    for (unsigned char ch : in) {
        if (ch < 32 || ch == 127) continue;
        out.push_back(static_cast<char>(ch));
    }
    const size_t b = out.find_first_not_of(' ');
    if (b == std::string::npos) return std::string();
    out = out.substr(b, out.find_last_not_of(' ') - b + 1);
    if (out.size() > maxLen) out.resize(maxLen);
    return out;
}

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
    Tell(c, "Pip-Boy: new task - " + c.quests.back().title);
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
        Tell(c, std::string("Pip-Boy: task ") + (state == 1 ? "completed" : state == 2 ? "failed" : "reopened") + " - " + q.title);
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
    if (!AccountStore::ValidName(name)) { Tell(c, "Friends: invalid name."); return false; }
    if (Lower(name) == Lower(c.name)) { Tell(c, "Friends: you cannot add yourself."); return false; }
    if (HasFriend(c, name)) { Tell(c, "Friends: " + name + " is already in your list."); return false; }
    if (c.friends.size() >= kMaxFriends) { Tell(c, "Friends: the list is full."); return false; }
    Critter* online = world_.FindPlayerByName(name);
    if (!accounts_.Exists(name) && !IsOnline(online)) { Tell(c, "Friends: no such player."); return false; }
    if (online) name = online->name;   // canonical spelling
    c.friends.push_back(name);
    Tell(c, "Friends: " + name + " added.");
    SendPip(c);
    return true;
}

bool Server::FriendDel(Critter& c, const std::string& nameIn) {
    if (!c.IsPlayer()) return false;
    const std::string k = Lower(Clean(nameIn, 16));
    auto it = std::find_if(c.friends.begin(), c.friends.end(), [&](const std::string& f) { return Lower(f) == k; });
    if (it == c.friends.end()) { Tell(c, "Friends: not in your list."); return false; }
    const std::string n = *it;
    c.friends.erase(it);
    Tell(c, "Friends: " + n + " removed.");
    SendPip(c);
    return true;
}

// ------------------------------------------------------------------ mail

std::vector<Server::MailMsg>& Server::Mailbox(const std::string& key) {
    auto it = mail_.find(key);
    if (it != mail_.end()) return it->second;
    std::vector<MailMsg>& box = mail_[key];
    std::ifstream f(cfg_.savesDir / "mail" / (key + ".json"), std::ios::binary);
    if (!f) return box;
    try {
        const json j = json::parse(f);
        if (j.is_array())
            for (const json& e : j) {
                MailMsg m;
                m.id = e.value("id", 0u);
                m.from = Clean(e.value("from", std::string()), 16);
                m.text = Clean(e.value("text", std::string()), kMaxMailText);
                m.time = e.value("time", static_cast<int64_t>(0));
                m.read = e.value("read", false);
                if (m.id) box.push_back(std::move(m));
            }
    } catch (const std::exception& e) {
        log::Warn("mailbox '%s' unreadable: %s", key.c_str(), e.what());
    }
    return box;
}

bool Server::SaveMailbox(const std::string& key) {
    json j = json::array();
    for (const MailMsg& m : Mailbox(key)) j.push_back({{"id", m.id}, {"from", m.from}, {"text", m.text}, {"time", m.time}, {"read", m.read}});
    std::error_code ec;
    std::filesystem::create_directories(cfg_.savesDir / "mail", ec);
    const bool ok = WriteFileAtomic(cfg_.savesDir / "mail" / (key + ".json"), j.dump());
    if (!ok) log::Error("mailbox '%s' save FAILED", key.c_str());
    return ok;
}

bool Server::MailSend(Critter& c, const std::string& toIn, const std::string& textIn) {
    if (!c.IsPlayer()) return false;
    if (c.account.empty()) { Tell(c, "Mail: guests cannot send mail."); return false; }
    const std::string to = Clean(toIn, 16), text = Clean(textIn, kMaxMailText);
    if (!AccountStore::ValidName(to) || !accounts_.Exists(to)) { Tell(c, "Mail: no such player."); return false; }
    if (text.empty()) { Tell(c, "Mail: the message is empty."); return false; }
    const std::string key = AccountStore::Key(to);
    std::vector<MailMsg>& box = Mailbox(key);
    if (box.size() >= kMaxMailbox) { Tell(c, "Mail: " + to + "'s mailbox is full."); return false; }
    MailMsg m;
    for (const MailMsg& o : box) m.id = std::max(m.id, o.id);
    m.id += 1;
    m.from = c.name; m.text = text; m.time = static_cast<int64_t>(std::time(nullptr));
    box.push_back(std::move(m));
    SaveMailbox(key);
    Tell(c, "Mail sent to " + to + ".");
    for (auto& [id, p] : world_.Critters())
        if (IsOnline(p.get()) && !p->account.empty() && AccountStore::Key(p->account) == key) {
            Tell(*p, "Pip-Boy: new mail from " + c.name + ".");
            SendPip(*p);
        }
    return true;
}

bool Server::MailRead(Critter& c, uint32_t id) {
    if (c.account.empty()) return false;
    const std::string key = AccountStore::Key(c.account);
    for (MailMsg& m : Mailbox(key))
        if (m.id == id) {
            if (!m.read) { m.read = true; SaveMailbox(key); SendPip(c); }
            return true;
        }
    return false;
}

bool Server::MailDelete(Critter& c, uint32_t id) {
    if (c.account.empty()) return false;
    const std::string key = AccountStore::Key(c.account);
    std::vector<MailMsg>& box = Mailbox(key);
    auto it = std::find_if(box.begin(), box.end(), [&](const MailMsg& m) { return m.id == id; });
    if (it == box.end()) return false;
    box.erase(it);
    SaveMailbox(key);
    SendPip(c);
    return true;
}

// ------------------------------------------------------------------ snapshot + session events

void Server::SendPip(Critter& c) {
    if (!c.peer || !c.IsPlayer()) return;
    gnet::PipData d;
    for (const Critter::QuestEntry& q : c.quests) d.quests.push_back({q.id, q.title, q.text, q.state});
    for (const std::string& f : c.friends) d.friends.push_back({f, static_cast<uint8_t>(IsOnline(world_.FindPlayerByName(f)) ? 1 : 0)});
    if (!c.account.empty())
        for (const MailMsg& m : Mailbox(AccountStore::Key(c.account)))
            d.mail.push_back({m.id, m.from, m.text, static_cast<uint32_t>(std::max<int64_t>(0, m.time)), static_cast<uint8_t>(m.read ? 1 : 0)});
    Send(c, gnet::PackG(d));
}

void Server::PipOnLogin(Critter& c) {
    SendPip(c);
    if (!c.account.empty()) {
        int unread = 0;
        for (const MailMsg& m : Mailbox(AccountStore::Key(c.account))) if (!m.read) ++unread;
        if (unread) Tell(c, "Pip-Boy: you have " + std::to_string(unread) + " unread message" + (unread == 1 ? "" : "s") + " (press P).");
    }
    for (auto& [id, p] : world_.Critters())
        if (p.get() != &c && IsOnline(p.get()) && HasFriend(*p, c.name)) {
            Tell(*p, "Friends: " + c.name + " is online.");
            SendPip(*p);
        }
}

void Server::PipOnLogout(Critter& c) {
    for (auto& [id, p] : world_.Critters())
        if (p.get() != &c && IsOnline(p.get()) && HasFriend(*p, c.name)) {
            Tell(*p, "Friends: " + c.name + " went offline.");
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
        else Tell(c, "Usage: /friend add <name>, /friend del <name>, /friends");
        return true;
    }
    if (cmd == "friends") {
        if (c.friends.empty()) Tell(c, "Friends: the list is empty. /friend add <name>");
        for (const std::string& f : c.friends) Tell(c, "Friends: " + f + (IsOnline(world_.FindPlayerByName(f)) ? " (online)" : " (offline)"));
        return true;
    }
    if (cmd == "mail") {
        std::string to;
        is >> to;
        MailSend(c, to, rest());
        return true;
    }
    if (cmd == "inbox") {
        if (c.account.empty()) { Tell(c, "Mail: guests have no mailbox."); return true; }
        const std::vector<MailMsg>& box = Mailbox(AccountStore::Key(c.account));
        if (box.empty()) Tell(c, "Mail: the inbox is empty.");
        for (const MailMsg& m : box) Tell(c, "Mail #" + std::to_string(m.id) + (m.read ? " " : " [new] ") + m.from + ": " + m.text);
        return true;
    }
    if (cmd == "maildel") {
        uint32_t id = 0;
        is >> id;
        if (!MailDelete(c, id)) Tell(c, "Mail: no such message. Usage: /maildel <id>");
        return true;
    }
    if (cmd == "quests") {
        if (c.quests.empty()) Tell(c, "Pip-Boy: no tasks.");
        for (const Critter::QuestEntry& q : c.quests) Tell(c, std::string("Task [") + (q.state == 0 ? "active" : q.state == 1 ? "done" : "failed") + "] " + q.title + (q.text.empty() ? "" : ": " + q.text));
        return true;
    }
    return false;
}

}  // namespace f3
