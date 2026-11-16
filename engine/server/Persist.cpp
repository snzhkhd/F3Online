// Account login, character save / load (accounts + persistence layer of the Server). See docs/server-data.md "Accounts and persistence".
#include <chrono>
#include <ctime>
#include <enet/enet.h>
#include "Scripting.h"
#include "Server.h"
#include "common/log.h"

namespace f3 {

using json = nlohmann::json;

// ------------------------------------------------------------------ character <-> json

json Server::CharacterToJson(const Critter& c) const {
    json j;
    j["version"] = 1;
    j["name"] = c.name;
    j["saved_at"] = static_cast<int64_t>(std::time(nullptr));
    if (const Map* m = const_cast<World&>(world_).GetMap(c.mapId)) j["map"] = m->name;
    j["x"] = c.pos.x;
    j["y"] = c.pos.y;
    j["dir"] = static_cast<int>(c.dir);
    j["hp"] = c.hp;
    j["max_hp"] = c.maxHp;
    j["dead"] = c.dead;
    j["level"] = c.level;
    j["xp"] = c.xp;
    j["special"] = json::array();
    for (int i = 0; i < 7; i++) j["special"].push_back(c.special[i]);
    j["skill_points"] = c.skillPoints;
    j["skill_ranks"] = c.skillRanks;
    j["unspent_special"] = c.unspentSpecial;
    j["tags"] = c.tagMask;
    j["traits"] = c.traitMask;
    j["perk_points"] = c.perkPoints;
    j["perks"] = json::object();
    for (auto& [id, rank] : c.perks) j["perks"][id] = rank;
    j["created"] = c.created;
    if (const Proto* p = c.protoPid >= 0 ? data_.GetProto(static_cast<uint16_t>(c.protoPid)) : nullptr) j["proto"] = p->key;
    j["inventory"] = json::array();
    for (const InvItem& it : c.inventory) {
        const Proto* p = data_.GetProto(it.pid);
        if (!p) continue;
        j["inventory"].push_back({{"proto", p->key}, {"count", it.count}, {"slot", it.slot}});
    }
    j["vars"] = json::object();
    for (auto& [k, v] : c.vars) j["vars"][k] = v;
    j["quests"] = json::array();
    for (const Critter::QuestEntry& q : c.quests) j["quests"].push_back({{"id", q.id}, {"title", q.title}, {"text", q.text}, {"state", q.state}, {"updated", q.updated}});
    j["friends"] = c.friends;
    j["gvars"] = json::object();   // per player quest / journal / script globals (original globals.ini names)
    for (auto& [k, v] : c.gvars) j["gvars"][k] = v;
    return j;
}

void Server::ApplyCharacter(Critter& c, const json& j) {
    c.created = j.value("created", true);
    if (j.contains("special") && j["special"].is_array() && j["special"].size() == 7)
        for (int i = 0; i < 7; i++) c.special[i] = std::clamp(j["special"][i].get<int>(), 1, 10);
    c.level = std::max(1, j.value("level", 1));
    c.xp = std::max(0, j.value("xp", 0));
    c.skillPoints = std::max(0, j.value("skill_points", 0));
    c.unspentSpecial = std::max(0, j.value("unspent_special", 0));
    c.tagMask = static_cast<uint16_t>(j.value("tags", 0));
    c.traitMask = static_cast<uint16_t>(j.value("traits", 0));
    c.perkPoints = std::max(0, j.value("perk_points", 0));
    if (j.contains("perks") && j["perks"].is_object())
        for (auto& [k, v] : j["perks"].items()) if (v.is_number_integer() && v.get<int>() > 0) c.perks[k] = std::min(v.get<int>(), 10);
    if (j.contains("skill_ranks") && j["skill_ranks"].is_object())
        for (auto& [k, v] : j["skill_ranks"].items()) if (v.is_number()) c.skillRanks[k] = v.get<int>();
    RecalcDerived(c);
    c.hp = std::clamp(j.value("hp", c.maxHp), 0, c.maxHp);
    if (j.value("dead", false)) { c.dead = true; c.hp = 0; c.deadSince = tick_; }
    if (j.contains("inventory") && j["inventory"].is_array())
        for (const json& e : j["inventory"]) {
            const Proto* p = data_.FindProto(e.value("proto", std::string()));
            if (!p) { log::Warn("load '%s': unknown item prototype '%s' dropped", c.name.c_str(), e.value("proto", std::string()).c_str()); continue; }
            ItemId id = GiveItem(c, *p, std::clamp(e.value("count", 1), 1, 65535));
            uint8_t slot = static_cast<uint8_t>(e.value("slot", 0));
            if (slot > 0 && slot < gnet::SlotCount)
                for (auto& it : c.inventory) if (it.id == id) { it.slot = slot; it.equipped = true; }
        }
    if (j.contains("vars") && j["vars"].is_object())
        for (auto& [k, v] : j["vars"].items()) if (v.is_number()) c.vars[k] = v.get<double>();
    if (j.contains("quests") && j["quests"].is_array())
        for (const json& e : j["quests"]) {
            if (!e.is_object() || c.quests.size() >= 100) continue;
            Critter::QuestEntry q;
            q.id = e.value("id", std::string());
            q.title = e.value("title", q.id);
            q.text = e.value("text", std::string());
            q.state = static_cast<uint8_t>(std::clamp(e.value("state", 0), 0, 2));
            q.updated = e.value("updated", static_cast<int64_t>(0));
            if (!q.id.empty()) c.quests.push_back(std::move(q));
        }
    if (j.contains("friends") && j["friends"].is_array())
        for (const json& e : j["friends"]) if (e.is_string() && c.friends.size() < 50) c.friends.push_back(e.get<std::string>());
    if (j.contains("gvars") && j["gvars"].is_object())
        for (auto& [k, v] : j["gvars"].items()) if (v.is_number()) c.gvars[k] = v.get<double>();
}

bool Server::SavePlayer(Critter& c) {
    if (c.account.empty() || !c.IsPlayer() || !accounts_.Ready()) return false;
    if (IsCreating(c)) return false;   // still in character creation: not in the world, nothing worth saving
    bool ok = accounts_.SaveCharacter(c.account, 0, CharacterToJson(c));
    if (!ok) log::Error("save of '%s' FAILED", c.account.c_str());
    return ok;
}

void Server::SaveAllPlayers() {
    int n = 0;
    for (auto& [id, c] : world_.Critters())
        if (c->IsPlayer() && c->mapId != 0 && SavePlayer(*c)) ++n;
    if (n) log::Info("saved %d player(s)", n);
}

// ------------------------------------------------------------------ login

static std::string PeerIp(ENetPeer* peer) {
    char buf[64] = "?";
    enet_address_get_host_ip(&peer->address, buf, sizeof buf);
    return buf;
}

void Server::OnAuth(ENetPeer* peer, const gnet::Auth& a) {
    using gnet::AuthCode;
    auto reply = [&](AuthCode code, bool isNew = false) {
        gnet::AuthResult r;
        r.code = static_cast<uint8_t>(code);
        r.isNew = isNew;
        r.text = gnet::AuthCodeText(code);
        SendRaw(peer, gnet::PackG(r));
        if (code != AuthCode::Ok) {
            log::Info("auth '%s' rejected: %s", a.name.c_str(), r.text.c_str());
            enet_peer_disconnect_later(peer, 0);
        }
    };
    if (a.version != kProtocolVersion) return reply(AuthCode::VersionMismatch);
    if (!accounts_.Ready()) return reply(AuthCode::ServerError);
    if (!AccountStore::ValidName(a.name)) return reply(AuthCode::InvalidName);
    const std::string ip = PeerIp(peer);
    json ch;
    bool isNew = false;
    AccountStore::Result r;
    if (a.mode == static_cast<uint8_t>(gnet::AuthMode::Register)) {
        r = accounts_.Register(a.name, a.password, ip, json{{"new", true}, {"name", a.name}});
        isNew = r == AccountStore::Result::Ok;
        ch = json{{"new", true}, {"account_name", a.name}};
    } else {
        r = accounts_.Login(a.name, a.password, ip, &ch);
    }
    switch (r) {
        case AccountStore::Result::Ok: break;
        case AccountStore::Result::BadCredentials: return reply(AuthCode::BadCredentials);
        case AccountStore::Result::NameTaken: return reply(AuthCode::NameTaken);
        case AccountStore::Result::InvalidName: return reply(AuthCode::InvalidName);
        case AccountStore::Result::WeakPassword: return reply(AuthCode::WeakPassword);
        case AccountStore::Result::RateLimited: return reply(AuthCode::RateLimited);
        default: return reply(AuthCode::ServerError);
    }
    const std::string name = ch.value("account_name", a.name);

    // duplicate login: the new session wins, the old one is saved and kicked
    if (Critter* old = world_.FindPlayerByName(name)) {
        log::Info("duplicate login of '%s': kicking the old session", name.c_str());
        if (old->peer) {
            ENetPeer* op = old->peer;
            SendRaw(op, net::Pack(net::LoginFail{"logged in from another location"}));
            OnDisconnect(op);   // saves, removes the critter
            enet_peer_disconnect_later(op, 0);
        }
    }
    reply(AuthCode::Ok, isNew);
    if (!StartSession(peer, name, &ch, name)) {
        enet_peer_disconnect_later(peer, 0);
    }
}

}  // namespace f3
