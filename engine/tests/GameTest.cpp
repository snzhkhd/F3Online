// f3_gametest: end-to-end test of the data-driven server (run it against `f3_server --data gamedata/server`).
//   f3_gametest [--port N] [--expect-map NAME]
// Two bots log in on the real start map: verifies the map size/grid, stats and starter inventory, walking
// with real-grid pathfinding, pickup of a ground item, an attack between the bots (hit/damage events),
// and a map transition (walk into the Junktown -> Vault region). Exit code 0 = all checks passed.
#include <enet/enet.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>
#include "common/protocol.h"
#include "common/protocol_game.h"

using namespace f3;
using Clock = std::chrono::steady_clock;

static int g_failed = 0;
#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            std::printf("FAIL: " __VA_ARGS__);            \
            std::printf("  [%s:%d]\n", __FILE__, __LINE__); \
            ++g_failed;                                   \
        } else {                                          \
            std::printf("ok:   " __VA_ARGS__);            \
            std::printf("\n");                            \
        }                                                 \
    } while (0)

struct Bot {
    ENetHost* host = nullptr;
    ENetPeer* peer = nullptr;
    std::string name;
    bool loggedIn = false, failed = false;
    CritterId myId = 0;
    net::MapInfo map;
    int mapInfoCount = 0;
    TilePos pos;
    bool havePos = false;
    gnet::Stats stats;
    bool haveStats = false;
    std::vector<gnet::InvEntry> inv;
    int invUpdates = 0;
    std::map<CritterId, net::CritterAdd> critters;
    std::map<CritterId, gnet::CritterState> states;
    std::map<ItemId, net::ItemAdd> items;
    std::map<ItemId, gnet::ObjectInfo> objInfo;
    std::vector<gnet::CombatEvent> combat;
    std::vector<std::string> chat;

    bool Connect(uint16_t port) {
        host = enet_host_create(nullptr, 1, 2, 0, 0);
        if (!host) return false;
        ENetAddress a{};
        enet_address_set_host(&a, "127.0.0.1");
        a.port = port;
        peer = enet_host_connect(host, &a, 2, 0);
        ENetEvent ev;
        if (!peer || enet_host_service(host, &ev, 3000) <= 0 || ev.type != ENET_EVENT_TYPE_CONNECT) return false;
        return true;
    }
    void Send(const ByteBuffer& b) {
        ENetPacket* p = enet_packet_create(b.Data(), b.Size(), ENET_PACKET_FLAG_RELIABLE);
        enet_peer_send(peer, 0, p);
        enet_host_flush(host);
    }
    void Login(const std::string& n) {
        name = n;
        net::LoginReq r;
        r.name = n;
        Send(net::Pack(r));
    }
    void Handle(const uint8_t* d, size_t len) {
        ByteBuffer b(d, len);
        uint8_t id = b.ReadU8();
        switch (id) {
            case (uint8_t)Msg::S2C_LoginOk: { net::LoginOk m; m.Read(b); myId = m.critterId; loggedIn = true; break; }
            case (uint8_t)Msg::S2C_LoginFail: { net::LoginFail m; m.Read(b); failed = true; std::printf("login failed: %s\n", m.reason.c_str()); break; }
            case (uint8_t)Msg::S2C_MapInfo: { map = net::MapInfo{}; map.Read(b); ++mapInfoCount; critters.clear(); items.clear(); objInfo.clear(); break; }
            case (uint8_t)Msg::S2C_CritterAdd: {
                net::CritterAdd m; m.Read(b);
                critters[m.id] = m;
                if (m.id == myId) { pos = {m.x, m.y}; havePos = true; }
                break;
            }
            case (uint8_t)Msg::S2C_CritterRemove: { net::CritterRemove m; m.Read(b); critters.erase(m.id); break; }
            case (uint8_t)Msg::S2C_CritterMove: {
                net::CritterMove m; m.Read(b);
                if (critters.count(m.id)) { critters[m.id].x = m.x; critters[m.id].y = m.y; }
                if (m.id == myId) { pos = {m.x, m.y}; havePos = true; }
                break;
            }
            case (uint8_t)Msg::S2C_ItemAdd: { net::ItemAdd m; m.Read(b); items[m.id] = m; break; }
            case (uint8_t)Msg::S2C_ItemRemove: { net::ItemRemove m; m.Read(b); items.erase(m.id); break; }
            case (uint8_t)Msg::S2C_Chat: { net::ChatMsg m; m.Read(b); chat.push_back(m.text); break; }
            case (uint8_t)GMsg::S2C_Stats: { stats.Read(b); haveStats = true; break; }
            case (uint8_t)GMsg::S2C_Inventory: { gnet::Inventory m; if (m.Read(b)) { inv = m.items; ++invUpdates; } break; }
            case (uint8_t)GMsg::S2C_ObjectInfo: { gnet::ObjectInfo m; m.Read(b); objInfo[m.id] = m; break; }
            case (uint8_t)GMsg::S2C_CritterState: { gnet::CritterState m; m.Read(b); states[m.id] = m; break; }
            case (uint8_t)GMsg::S2C_CombatEvent: { gnet::CombatEvent m; m.Read(b); combat.push_back(m); break; }
            default: break;
        }
    }
    void Pump(int ms) {
        ENetEvent ev;
        int r = enet_host_service(host, &ev, ms);
        while (r > 0) {
            if (ev.type == ENET_EVENT_TYPE_RECEIVE) { Handle(ev.packet->data, ev.packet->dataLength); enet_packet_destroy(ev.packet); }
            else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) { failed = true; }
            r = enet_host_service(host, &ev, 0);
        }
    }
};

// Pumps all bots until pred() or timeout.
static bool WaitFor(std::vector<Bot*> bots, double seconds, const std::function<bool()>& pred) {
    auto end = Clock::now() + std::chrono::milliseconds(static_cast<int>(seconds * 1000));
    while (Clock::now() < end) {
        for (Bot* b : bots) b->Pump(5);
        if (pred()) return true;
    }
    return pred();
}

static bool Walkable(const Bot& b, int x, int y) {
    return x >= 0 && y >= 0 && x < b.map.width && y < b.map.height && b.map.blocked[static_cast<size_t>(y) * b.map.width + x] == 0;
}

int main(int argc, char** argv) {
    uint16_t port = 4048;
    std::string expectMap = "00_03_Tutorial_Junktown";
    for (int i = 1; i + 1 < argc; i++) {
        if (!std::strcmp(argv[i], "--port")) port = static_cast<uint16_t>(std::atoi(argv[i + 1]));
        if (!std::strcmp(argv[i], "--expect-map")) expectMap = argv[i + 1];
    }
    if (enet_initialize() != 0) return 2;

    Bot a, b;
    if (!a.Connect(port) || !b.Connect(port)) { std::printf("FAIL: cannot connect to 127.0.0.1:%u\n", port); return 1; }
    a.Login("GameBotA");
    CHECK(WaitFor({&a}, 8, [&] { return a.loggedIn && a.mapInfoCount > 0 && a.havePos && a.haveStats && a.invUpdates > 0; }), "bot A logged in with map, stats, inventory");
    b.Login("GameBotB");
    CHECK(WaitFor({&a, &b}, 8, [&] { return b.loggedIn && b.havePos && b.haveStats; }), "bot B logged in");
    if (a.failed || b.failed || !a.loggedIn || !b.loggedIn) return 1;

    // --- real map
    CHECK(a.map.name == expectMap, "start map is '%s' (got '%s')", expectMap.c_str(), a.map.name.c_str());
    CHECK(a.map.width == 384 && a.map.height == 624, "map size is %dx%d cells (expect 384x624)", a.map.width, a.map.height);
    CHECK(Walkable(a, a.pos.x, a.pos.y), "spawn cell %d,%d is walkable on the real grid", a.pos.x, a.pos.y);
    size_t blockedCells = 0;
    for (uint8_t v : a.map.blocked) blockedCells += v;
    CHECK(blockedCells > a.map.blocked.size() / 10, "grid has real obstacles (%zu blocked cells)", blockedCells);
    CHECK(a.stats.hp > 0 && a.stats.hp == a.stats.maxHp && a.stats.special[0] == 6, "stats: hp %d/%d, S=%d (Default_PC)", a.stats.hp, a.stats.maxHp, a.stats.special[0]);
    bool hasGun = false, hasEquipped = false;
    for (auto& e : a.inv) { if (e.name.find("9mm") != std::string::npos) hasGun = true; if (e.equipped) hasEquipped = true; }
    CHECK(hasGun && hasEquipped, "starter inventory present (%zu entries, names from English.stf)", a.inv.size());
    CHECK(WaitFor({&a, &b}, 3, [&] { return a.critters.size() >= 2; }), "bot A sees bot B and NPCs (%zu critters)", a.critters.size());

    // --- walking with the real grid
    TilePos target = a.pos;
    bool found = false;
    for (int r = 8; r >= 4 && !found; --r)
        for (int dy = -r; dy <= r && !found; ++dy)
            for (int dx = -r; dx <= r && !found; ++dx)
                if (std::max(std::abs(dx), std::abs(dy)) == r && Walkable(a, a.pos.x + dx, a.pos.y + dy)) { target = {static_cast<int16_t>(a.pos.x + dx), static_cast<int16_t>(a.pos.y + dy)}; found = true; }
    {
        net::MoveToReq mv; mv.x = target.x; mv.y = target.y;
        a.Send(net::Pack(mv));
    }
    CHECK(WaitFor({&a, &b}, 10, [&] { return a.pos == target; }), "bot A walked to %d,%d (at %d,%d)", target.x, target.y, a.pos.x, a.pos.y);

    // --- pickup: nearest plain ground item (weapons/ammo lying in the town)
    {
        ItemId best = 0;
        int bestD = 1 << 30;
        for (auto& [id, it] : a.items) {
            if (a.objInfo.count(id) && a.objInfo[id].kind != gnet::ObjectKind::Ground) continue;
            int d = TileDist({it.x, it.y}, a.pos);
            if (d < bestD) { bestD = d; best = id; }
        }
        if (!best) {
            // nothing lying around in view: use the dev command to drop a real revolver next to the bot
            net::ChatReq c; c.text = "/spawn Weapons44_Revolver.WEA";
            a.Send(net::Pack(c));
            WaitFor({&a, &b}, 3, [&] { return !a.items.empty(); });
            for (auto& [id, it] : a.items) best = id;
        }
        if (!best) {
            CHECK(false, "no ground item available for the pickup check");
        } else {
            size_t before = a.inv.size();
            int upd = a.invUpdates;
            gnet::PickUp pu; pu.id = best;
            a.Send(gnet::PackG(pu));
            CHECK(WaitFor({&a, &b}, 15, [&] { return !a.items.count(best) && a.invUpdates > upd; }), "bot A picked up ground item %u (inventory %zu -> %zu)", best, before, a.inv.size());
        }
    }

    // --- combat: A attacks B (both players, team 1; the server does not forbid PvP)
    {
        CritterId bid = b.myId;
        gnet::Attack at; at.id = bid;
        a.Send(gnet::PackG(at));
        bool ok = WaitFor({&a, &b}, 25, [&] {
            for (auto& e : a.combat) if (e.target == bid && e.result != gnet::HitResult::Miss) return true;
            return false;
        });
        CHECK(ok, "combat: bot A hit bot B (%zu combat events)", a.combat.size());
        WaitFor({&a, &b}, 1, [] { return false; });
        auto st = b.states.find(b.myId);
        CHECK(st != b.states.end() && st->second.hp < st->second.maxHp, "bot B received HP update (hp %d)", st != b.states.end() ? st->second.hp : -1);
        gnet::Attack stop; stop.id = 0;
        a.Send(gnet::PackG(stop));
    }

    // --- map transition: go to the Vault side of Junktown's transition region
    {
        net::ChatReq c; c.text = "/goto 00_03_Tutorial_Junktown 2";
        a.Send(net::Pack(c));
        int maps = a.mapInfoCount;
        CHECK(WaitFor({&a, &b}, 5, [&] { return TileDist(a.pos, TilePos{164, 105}) <= 8; }), "chat /goto moved bot A to entry 2 at %d,%d", a.pos.x, a.pos.y);
        // transition region (cells): x 154..178, y 53..68
        net::MoveToReq mv; mv.x = 166; mv.y = 62;
        a.Send(net::Pack(mv));
        maps = a.mapInfoCount;
        bool ok = WaitFor({&a, &b}, 30, [&] { return a.mapInfoCount > maps; });
        CHECK(ok && a.map.name == "00_04_Tutorial_Vault", "walking into the transition region loaded map '%s'", a.map.name.c_str());
        CHECK(a.map.width == 384 && a.map.height == 334, "vault grid is %dx%d (expect 384x334)", a.map.width, a.map.height);
        WaitFor({&a}, 1, [] { return false; });
        CHECK(Walkable(a, a.pos.x, a.pos.y), "arrival cell %d,%d is walkable", a.pos.x, a.pos.y);
        // vault NPCs/containers/doors should be visible somewhere near the entry; just report
        int doors = 0, cons = 0;
        for (auto& [id, o] : a.objInfo) { if (o.kind == gnet::ObjectKind::Door) doors++; if (o.kind == gnet::ObjectKind::Container) cons++; }
        // door: open the nearest door in view and expect the open state + released cells
        std::vector<std::pair<int, ItemId>> doors2;
        for (auto& [id, o] : a.objInfo)
            if (o.kind == gnet::ObjectKind::Door && a.items.count(id)) doors2.push_back({TileDist({a.items[id].x, a.items[id].y}, a.pos), id});
        std::sort(doors2.begin(), doors2.end());
        bool opened = false;
        for (auto& [dist, door] : doors2) {  // doors behind other closed doors are legitimately unreachable: try the next one
            size_t chats = a.chat.size();
            gnet::Use u; u.id = door;
            a.Send(gnet::PackG(u));
            opened = WaitFor({&a, &b}, 12, [&] { return a.objInfo[door].open == 1 || a.chat.size() > chats; }) && a.objInfo[door].open == 1;
            if (opened) { std::printf("note: door %u opened (distance %d cells)\n", door, dist); break; }
        }
        CHECK(doors2.empty() || opened, "a reachable door can be opened (%zu doors in view)", doors2.size());
        std::printf("note: vault view: %zu critters, %zu objects (%d doors, %d containers)\n", a.critters.size(), a.items.size(), doors, cons);
    }

    for (auto& t : a.chat) std::printf("chat: %s\n", t.c_str());
    std::printf(g_failed ? "GAMETEST FAILED (%d)\n" : "GAMETEST PASSED\n", g_failed);
    enet_deinitialize();
    return g_failed ? 1 : 0;
}
