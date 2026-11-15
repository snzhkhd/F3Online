// f3_gametest: end-to-end test of the data-driven server (run it against `f3_server --data gamedata/server`).
//   f3_gametest [--port N] [--expect-map NAME]
// Two bots log in on the real start map: verifies the map size/grid, stats and starter inventory, walking
// with real-grid pathfinding, pickup of a ground item, an attack between the bots (hit/damage events),
// and a map transition (walk into the Junktown -> Vault region). Real-time timing checks (bot A announces the
// continuous-move capability, bot B is a legacy client): MovePath validated against the grid, MoveToF, legacy per-cell
// CritterMove for B, breaktime rejection (Equip twice, move while busy), attack action duration, AP spend + regeneration.
// Exit code 0 = all checks passed.
#include <enet/enet.h>

#include <algorithm>
#include <chrono>
#include <cmath>
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
    int authCode = -1, authNew = 0;   // account block
    gnet::CharSheet sheet;
    int sheetCount = 0;
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
    std::vector<gnet::Container> cons;   // S2C_Container messages (container objects and corpses)
    std::vector<std::string> chat, chatFrom;
    // dialogue
    std::vector<gnet::DialogOpen> dlgOpens;
    std::vector<gnet::DialogNode> dlgNodes;
    int dlgCloses = 0;
    // real-time state (continuous-move capable clients only)
    struct Timed { gnet::WaitState w; double localMs; };
    std::vector<Timed> waits;
    std::vector<gnet::MovePath> paths;
    std::vector<gnet::CritterStop> stops;
    float fx = 0, fy = 0;
    std::vector<double> combatMs;  // local arrival time of each CombatEvent
    static double LocalMs() { return std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count(); }

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
    void AuthMsg(gnet::AuthMode mode, const std::string& n, const std::string& pw) {
        name = n;
        gnet::Auth a; a.mode = (uint8_t)mode; a.name = n; a.password = pw;
        Send(gnet::PackG(a));
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
            case (uint8_t)Msg::S2C_Chat: { net::ChatMsg m; m.Read(b); chat.push_back(m.text); chatFrom.push_back(m.name); break; }
            case (uint8_t)GMsg::S2C_Stats: { stats.Read(b); haveStats = true; break; }
            case (uint8_t)GMsg::S2C_Inventory: { gnet::Inventory m; if (m.Read(b)) { inv = m.items; ++invUpdates; } break; }
            case (uint8_t)GMsg::S2C_Container: { gnet::Container m; if (m.Read(b)) cons.push_back(m); break; }
            case (uint8_t)GMsg::S2C_ObjectInfo: { gnet::ObjectInfo m; m.Read(b); objInfo[m.id] = m; break; }
            case (uint8_t)GMsg::S2C_CritterState: { gnet::CritterState m; m.Read(b); states[m.id] = m; break; }
            case (uint8_t)GMsg::S2C_CombatEvent: { gnet::CombatEvent m; m.Read(b); combat.push_back(m); combatMs.push_back(LocalMs()); break; }
            case (uint8_t)GMsgRt::S2C_MovePath: { gnet::MovePath m; if (m.Read(b)) paths.push_back(m); break; }
            case (uint8_t)GMsgRt::S2C_CritterStop: {
                gnet::CritterStop m;
                if (!m.Read(b)) break;
                stops.push_back(m);
                TilePos cell{static_cast<int16_t>(std::floor(m.x)), static_cast<int16_t>(std::floor(m.y))};
                if (critters.count(m.id)) { critters[m.id].x = cell.x; critters[m.id].y = cell.y; }
                if (m.id == myId) { pos = cell; fx = m.x; fy = m.y; havePos = true; }
                break;
            }
            case (uint8_t)GMsgRt::S2C_WaitState: { gnet::WaitState m; if (m.Read(b)) waits.push_back({m, LocalMs()}); break; }
            case (uint8_t)GMsgAuth::S2C_AuthResult: { gnet::AuthResult m; if (m.Read(b)) { authCode = m.code; authNew = m.isNew; } break; }
            case (uint8_t)GMsgChar::S2C_CharSheet: { if (sheet.Read(b)) ++sheetCount; break; }
            case (uint8_t)GMsgDlg::S2C_DialogOpen: { gnet::DialogOpen m; if (m.Read(b)) dlgOpens.push_back(m); break; }
            case (uint8_t)GMsgDlg::S2C_DialogNode: { gnet::DialogNode m; if (m.Read(b)) dlgNodes.push_back(m); break; }
            case (uint8_t)GMsgDlg::S2C_DialogClose: { ++dlgCloses; break; }
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

// every sample of the segment must lie on a walkable cell (authoritative path check from the client side)
static bool SegmentWalkable(const Bot& b, float ax, float ay, float bx, float by) {
    float dx = bx - ax, dy = by - ay, len = std::sqrt(dx * dx + dy * dy);
    int n = static_cast<int>(len / 0.1f) + 1;
    for (int i = 0; i <= n; ++i) {
        float t = static_cast<float>(i) / n;
        int cx = static_cast<int>(std::floor(ax + dx * t)), cy = static_cast<int>(std::floor(ay + dy * t));
        if (cx < 0 || cy < 0 || cx >= b.map.width || cy >= b.map.height || b.map.blocked[static_cast<size_t>(cy) * b.map.width + cx]) return false;
    }
    return true;
}

static const gnet::WaitState* LastWait(const Bot& b, uint8_t action) {
    for (size_t i = b.waits.size(); i-- > 0;)
        if (b.waits[i].w.action == action) return &b.waits[i].w;
    return nullptr;
}

static bool Walkable(const Bot& b, int x, int y) {
    return x >= 0 && y >= 0 && x < b.map.width && y < b.map.height && b.map.blocked[static_cast<size_t>(y) * b.map.width + x] == 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// Account / persistence suite: f3_gametest --accounts 1|2 [--state FILE]. Server: --data, NO --dev-kit / --allow-guest.
// Phase 1: guest refused, register (+ invalid / weak / taken), wrong password, walk, spawn + pick up + equip a revolver,
//          set a per-player global, logout. Phase 2 (run against a RESTARTED server with the same --saves): login, assert the
//          position, inventory with the equipped slot, vars and globals were restored; wrong password; duplicate login kicks the first session.
// ---------------------------------------------------------------------------------------------------------------------
static int RunAccountTests(uint16_t port, int phase, const std::string& stateFile) {
    const char* kName = "AcctBot";
    const char* kPass = "secret1";
    auto auth = [&](Bot& b, gnet::AuthMode mode, const std::string& n, const std::string& pw) {
        b.name = n;
        gnet::Auth a; a.mode = (uint8_t)mode; a.name = n; a.password = pw;
        b.Send(gnet::PackG(a));
    };
    auto code = [&](Bot& b, int c) { return WaitFor({&b}, 6, [&] { return b.authCode >= 0; }) && b.authCode == c; };
    if (phase == 1) {
        {   // guest login is off by default
            Bot g;
            CHECK(g.Connect(port), "guest bot connected");
            g.Login("GuestBot");
            CHECK(WaitFor({&g}, 4, [&] { return g.failed; }) && !g.loggedIn, "legacy name-only login refused without --allow-guest");
        }
        {
            Bot x;
            x.Connect(port);
            auth(x, gnet::AuthMode::Register, "bad name!", kPass);
            CHECK(code(x, (int)gnet::AuthCode::InvalidName), "register with an invalid name rejected");
        }
        {
            Bot x;
            x.Connect(port);
            auth(x, gnet::AuthMode::Register, "WeakBot", "abc");
            CHECK(code(x, (int)gnet::AuthCode::WeakPassword), "register with a short password rejected");
        }
        {
            Bot x;
            x.Connect(port);
            auth(x, gnet::AuthMode::Login, "NoSuchBot", kPass);
            CHECK(code(x, (int)gnet::AuthCode::BadCredentials), "login of an unknown account rejected");
        }
    }
    Bot a;
    if (!a.Connect(port)) { std::printf("FAIL: cannot connect\n"); return 1; }
    if (phase == 1) {
        // an observer that is already in the world (created through the normal flow) proves that a session in character creation is not placed
        Bot obs;
        if (!obs.Connect(port)) { std::printf("FAIL: cannot connect\n"); return 1; }
        auth(obs, gnet::AuthMode::Register, "ObsBot", kPass);
        CHECK(WaitFor({&obs}, 10, [&] { return obs.loggedIn && obs.sheetCount > 0; }) && !obs.sheet.created && obs.mapInfoCount == 0 && !obs.havePos && obs.invUpdates == 0 && obs.critters.empty(),
              "new account: LoginOk + CharSheet only, no MapInfo / critters / inventory before character creation");
        { gnet::CharCreate oc; oc.special[0] = 8; oc.special[4] = 8; oc.special[6] = 4; oc.tagMask = 7; obs.Send(gnet::PackG(oc)); }
        CHECK(WaitFor({&obs}, 6, [&] { return obs.sheet.created && obs.mapInfoCount > 0 && obs.havePos && obs.invUpdates > 0; }), "observer created its character and entered the world");

        auth(a, gnet::AuthMode::Register, kName, kPass);
        CHECK(WaitFor({&a}, 10, [&] { return a.loggedIn && a.sheetCount > 0 && a.haveStats; }) && a.authCode == 0 && a.authNew == 1,
              "register + login (code %d, new %d)", a.authCode, a.authNew);
        if (!a.loggedIn) return 1;
        CHECK(!a.sheet.created && a.sheet.unspentSpecial == 5 && a.sheet.creationTotal == 40 && a.stats.special[0] == 5 && a.sheet.traitMax == 2,
              "new character: creation pending, SPECIAL 5 each, 5 points to allocate, up to %d traits", a.sheet.traitMax);
        CHECK(a.mapInfoCount == 0 && !a.havePos && a.invUpdates == 0 && a.critters.empty(), "session in character creation has no map, no position, no critters, no inventory");
        {   // not in the world: gated requests are ignored, chat is not delivered, nobody sees the session
            net::MoveToReq mv0; mv0.x = 10; mv0.y = 10;
            a.Send(net::Pack(mv0));
            net::ChatReq say; say.text = "hello from creation";
            a.Send(net::Pack(say));
            net::ChatReq heal; heal.text = "/heal";
            a.Send(net::Pack(heal));
            WaitFor({&a, &obs}, 1.2, [] { return false; });
            CHECK(!a.havePos && a.mapInfoCount == 0 && a.chat.empty(), "movement / chat / commands are ignored while in character creation");
            bool seen = obs.critters.count(a.myId) != 0;
            for (auto& s2 : obs.chat) if (s2.find("hello from creation") != std::string::npos) seen = true;
            CHECK(!seen, "the observer in the world neither sees the creating session nor hears it");
            int sc = a.sheetCount;   // ReqState works (client refresh) and only resends stats + sheet
            a.Send(gnet::PackG(gnet::ReqState{}));
            CHECK(WaitFor({&a}, 3, [&] { return a.sheetCount > sc; }) && a.mapInfoCount == 0, "ReqState during creation answers with the sheet, still no map");
        }
        {   // disconnecting during creation leaves a new account: logging in again shows the creation screen again
            Bot q;
            q.Connect(port);
            auth(q, gnet::AuthMode::Register, "QuitBot", kPass);
            CHECK(WaitFor({&q}, 8, [&] { return q.loggedIn && q.sheetCount > 0; }) && !q.sheet.created, "second new account waits in creation");
            enet_peer_disconnect(q.peer, 0);
            WaitFor({&q}, 1, [] { return false; });
            Bot q2;
            q2.Connect(port);
            auth(q2, gnet::AuthMode::Login, "QuitBot", kPass);
            CHECK(WaitFor({&q2}, 8, [&] { return q2.loggedIn && q2.sheetCount > 0; }) && !q2.sheet.created && q2.mapInfoCount == 0, "after quitting during creation the account is still new (creation again, no map)");
            enet_peer_disconnect(q2.peer, 0);
            WaitFor({&q2}, 1, [] { return false; });
        }
        {   // invalid allocations: sum 42, Luck 0, only two tags, three traits
            gnet::CharCreate bad; for (int i = 0; i < 7; i++) bad.special[i] = 6; bad.tagMask = 7;
            a.Send(gnet::PackG(bad));
            gnet::CharCreate bad2; bad2.special[0] = 10; bad2.special[1] = 10; bad2.special[2] = 10; bad2.special[3] = 5; bad2.special[4] = 5; bad2.special[5] = 5; bad2.special[6] = 0; bad2.tagMask = 7;
            a.Send(gnet::PackG(bad2));
            gnet::CharCreate bad3; bad3.special[0] = 8; bad3.special[4] = 8; bad3.special[6] = 4; bad3.tagMask = 3;
            a.Send(gnet::PackG(bad3));
            gnet::CharCreate bad4; bad4.special[0] = 8; bad4.special[4] = 8; bad4.special[6] = 4; bad4.tagMask = 7; bad4.traitMask = 0x0B;
            a.Send(gnet::PackG(bad4));
            WaitFor({&a}, 1, [] { return false; });
            CHECK(!a.sheet.created && a.mapInfoCount == 0, "invalid creation requests (sum, range, tag count, three traits) rejected, still not in the world");
            gnet::CharCreate ok; ok.special[0] = 8; ok.special[4] = 8; ok.special[6] = 4; ok.tagMask = (1u << 0) | (1u << 7) | (1u << 9); ok.traitMask = (1u << 0) | (1u << 3);
            a.Send(gnet::PackG(ok));
            CHECK(WaitFor({&a}, 6, [&] { return a.sheet.created && a.mapInfoCount > 0 && a.havePos && a.invUpdates > 0; }), "valid creation accepted (S8 P5 E5 C5 I8 A5 L4, tags Firearms/Medic/Science, traits 0 + 3): the character enters the world only now");
            CHECK(a.inv.empty() && a.sheet.traitMask == 9, "new account character owns nothing, traits stored (mask %u)", a.sheet.traitMask);
            CHECK(WaitFor({&obs}, 4, [&] { return obs.critters.count(a.myId) != 0; }), "the observer now sees the new character");
            WaitFor({&a}, 1, [] { return false; });
            CHECK(a.stats.special[0] == 8 && a.stats.maxHp == 33 && a.sheet.skill[0] == 45 && a.sheet.skill[7] == 46 && a.sheet.skillPoints == 10 && a.sheet.maxAp == 7,
                  "derived values: maxHp %d (33), Firearms %u (45), Medic %u (46), skill points %u (10), AP %u (7)", a.stats.maxHp, a.sheet.skill[0], a.sheet.skill[7], a.sheet.skillPoints, a.sheet.maxAp);
            gnet::SpendSkill sp; sp.skill = 0; sp.points = 4;
            a.Send(gnet::PackG(sp));
            CHECK(WaitFor({&a}, 3, [&] { return a.sheet.skill[0] == 49 && a.sheet.skillPoints == 6; }), "spent 4 skill points on Firearms (49, 6 left)");
            gnet::SpendSkill over; over.skill = 3; over.points = 200;
            a.Send(gnet::PackG(over));
            WaitFor({&a}, 1, [] { return false; });
            CHECK(a.sheet.skillPoints == 0 && a.sheet.skill[3] == 26, "spending more than owned is capped (Barter %u, points %u)", a.sheet.skill[3], a.sheet.skillPoints);
            net::ChatReq xp; xp.text = "/givexp 1000";
            a.Send(net::Pack(xp));
            CHECK(WaitFor({&a}, 3, [&] { return a.sheet.level == 2; }), "1000 xp -> level 2 (level %d, xp %u)", a.sheet.level, a.sheet.xp);
            WaitFor({&a}, 1, [] { return false; });
            CHECK(a.sheet.skillPoints == 21 && a.sheet.xpNext == 3000 && a.stats.maxHp == 37, "level up: +21 skill points (%u), next level at %u xp (3000), maxHp %d (37)", a.sheet.skillPoints, a.sheet.xpNext, a.stats.maxHp);
            {   // skill cursor: UseSkill target validation (before AP is spent) and self / critter targets
                auto said = [&](Bot& b, const char* needle) { for (auto& t : b.chat) if (t.find(needle) != std::string::npos) return true; return false; };
                a.chat.clear();
                gnet::UseSkill us; us.skill = 7; us.kind = 1; us.target = 999999;   // Medic on a critter that does not exist
                a.Send(gnet::PackG(us));
                CHECK(WaitFor({&a}, 3, [&] { return said(a, "not here"); }), "UseSkill on a missing critter is refused");
                a.chat.clear();
                us.kind = 2;                                                         // ... and on a missing object
                a.Send(gnet::PackG(us));
                CHECK(WaitFor({&a}, 3, [&] { return said(a, "not here"); }), "UseSkill on a missing object is refused");
                a.chat.clear();
                us.kind = 0; us.target = 0;                                          // Medic on yourself: full health
                a.Send(gnet::PackG(us));
                CHECK(WaitFor({&a}, 3, [&] { return said(a, "not hurt"); }), "UseSkill on yourself reaches the script (Medic: not hurt)");
                a.chat.clear();
                WaitFor({&a}, 1.5, [] { return false; });   // break time of the previous use
                us.kind = 1; us.target = obs.myId;                                   // Medic on another critter: in range -> script answer, else refused
                a.Send(gnet::PackG(us));
                CHECK(WaitFor({&a}, 3, [&] { return said(a, "not hurt") || said(a, "too far"); }), "UseSkill on another critter is range-checked and reaches the script");
                a.chat.clear();
                us.skill = 14; us.kind = 0;                                          // skill index out of range: ignored
                a.Send(gnet::PackG(us));
                WaitFor({&a}, 1, [] { return false; });
                CHECK(a.chat.empty(), "UseSkill with an invalid skill index is ignored");
            }
        }
        enet_peer_disconnect(obs.peer, 0);
        {   // name taken (case insensitive)
            Bot x;
            x.Connect(port);
            auth(x, gnet::AuthMode::Register, "acctbot", "other1234");
            CHECK(code(x, (int)gnet::AuthCode::NameTaken), "registering the same name again rejected");
        }
        {   // wrong password
            Bot x;
            x.Connect(port);
            auth(x, gnet::AuthMode::Login, kName, "wrongpass");
            CHECK(code(x, (int)gnet::AuthCode::BadCredentials), "login with a wrong password rejected");
        }
        // walk somewhere
        TilePos target = a.pos;
        bool found = false;
        for (int r = 8; r >= 4 && !found; --r)
            for (int dy = -r; dy <= r && !found; ++dy)
                for (int dx = -r; dx <= r && !found; ++dx)
                    if (std::max(std::abs(dx), std::abs(dy)) == r && Walkable(a, a.pos.x + dx, a.pos.y + dy)) { target = {(int16_t)(a.pos.x + dx), (int16_t)(a.pos.y + dy)}; found = true; }
        net::MoveToReq mv; mv.x = target.x; mv.y = target.y;
        a.Send(net::Pack(mv));
        CHECK(WaitFor({&a}, 12, [&] { return a.pos == target; }), "walked to %d,%d", target.x, target.y);
        // spawn + pick up + equip a revolver
        std::vector<ItemId> before;
        for (auto& [id, it] : a.items) before.push_back(id);
        net::ChatReq c; c.text = "/spawn Weapons44_Revolver.WEA";
        a.Send(net::Pack(c));
        ItemId ground = 0;
        WaitFor({&a}, 4, [&] { for (auto& [id, it] : a.items) if (std::find(before.begin(), before.end(), id) == before.end()) ground = id; return ground != 0; });
        CHECK(ground != 0, "dev spawn produced a ground item");
        int upd = a.invUpdates;
        gnet::PickUp pu; pu.id = ground;
        a.Send(gnet::PackG(pu));
        CHECK(WaitFor({&a}, 15, [&] { return a.invUpdates > upd && !a.inv.empty(); }), "picked up (inventory %zu)", a.inv.size());
        if (!a.inv.empty()) {
            WaitFor({&a}, 1, [] { return false; });   // breaktime
            gnet::Equip e; e.id = a.inv[0].id; e.on = 1;
            a.Send(gnet::PackG(e));
            CHECK(WaitFor({&a}, 6, [&] { return !a.inv.empty() && a.inv[0].equipped == gnet::SlotHand1; }), "revolver equipped in hand 1");
        }
        net::ChatReq g; g.text = "/gvar Task7 2";
        a.Send(net::Pack(g));
        WaitFor({&a}, 1, [] { return false; });
        // remember the expected state, then log out (save on logout)
        if (!a.havePos) return 1;
        FILE* f = std::fopen(stateFile.c_str(), "w");
        if (f) { std::fprintf(f, "%d %d %zu %d %d %d %u %d %s\n", a.pos.x, a.pos.y, a.inv.size(), a.inv.empty() ? 0 : a.inv[0].equipped, a.sheet.level, a.sheet.skillPoints, a.sheet.skill[0], a.stats.special[0], a.map.name.c_str()); std::fclose(f); }
        enet_peer_disconnect(a.peer, 0);
        WaitFor({&a}, 1, [] { return false; });
        std::printf(g_failed ? "ACCOUNTTEST 1 FAILED\n" : "ACCOUNTTEST 1 PASSED\n");
        return g_failed ? 1 : 0;
    }
    // ---- phase 2: restarted server
    int ex = 0, ey = 0, eInv = 0, eEq = 0, eLevel = 0, eSp = 0, eFire = 0, eS = 0;
    char emap[128] = {0};
    if (FILE* f = std::fopen(stateFile.c_str(), "r")) { std::fscanf(f, "%d %d %d %d %d %d %d %d %127s", &ex, &ey, &eInv, &eEq, &eLevel, &eSp, &eFire, &eS, emap); std::fclose(f); }
    else { std::printf("FAIL: no state file\n"); return 1; }
    {
        Bot x;
        x.Connect(port);
        auth(x, gnet::AuthMode::Login, kName, "wrongpass");
        CHECK(code(x, (int)gnet::AuthCode::BadCredentials), "wrong password rejected after restart");
    }
    auth(a, gnet::AuthMode::Login, "ACCTBOT", kPass);   // case-insensitive account name
    CHECK(WaitFor({&a}, 10, [&] { return a.loggedIn && a.mapInfoCount > 0 && a.havePos && a.haveStats && a.invUpdates > 0; }) && a.authCode == 0 && a.authNew == 0, "login after server restart");
    if (!a.loggedIn) return 1;
    WaitFor({&a}, 1, [] { return false; });
    CHECK(a.map.name == emap, "map restored: %s", a.map.name.c_str());
    CHECK(a.pos.x == ex && a.pos.y == ey, "position restored %d,%d (expected %d,%d)", a.pos.x, a.pos.y, ex, ey);
    CHECK((int)a.inv.size() == eInv && !a.inv.empty() && a.inv[0].equipped == eEq && a.inv[0].name.find("44") != std::string::npos,
          "inventory restored: %zu entries, equipped slot %d (expected %d)", a.inv.size(), a.inv.empty() ? -1 : a.inv[0].equipped, eEq);
    CHECK(WaitFor({&a}, 3, [&] { return a.sheetCount > 0; }) && a.sheet.created && a.sheet.level == eLevel && (int)a.sheet.skillPoints == eSp && (int)a.sheet.skill[0] == eFire && a.stats.special[0] == eS && a.sheet.tagMask == ((1u << 0) | (1u << 7) | (1u << 9)) && a.sheet.traitMask == 9,
          "character sheet restored: level %d, skill points %d, Firearms %d, S %d, tags %u, traits %u", a.sheet.level, a.sheet.skillPoints, a.sheet.skill[0], a.stats.special[0], a.sheet.tagMask, a.sheet.traitMask);
    {   // skill use hook: Medic on yourself at full health
        size_t n = a.chat.size();
        net::ChatReq hl; hl.text = "/heal";
        a.Send(net::Pack(hl));
        WaitFor({&a}, 1, [] { return false; });
        gnet::UseSkill us; us.skill = 7; us.kind = 0;
        a.Send(gnet::PackG(us));
        CHECK(WaitFor({&a}, 4, [&] { for (size_t i = n; i < a.chat.size(); ++i) if (a.chat[i].find("not hurt") != std::string::npos) return true; return false; }), "use_skill hook answered for Medic");
    }
    {
        size_t n = a.chat.size();
        net::ChatReq g; g.text = "/gvar Task7";
        a.Send(net::Pack(g));
        bool ok = WaitFor({&a}, 4, [&] { for (size_t i = n; i < a.chat.size(); ++i) if (a.chat[i].find("Task7 = 2") != std::string::npos) return true; return false; });
        CHECK(ok, "per-player global Task7 restored (= 2)");
    }
    {   // duplicate login: a second session of the same account takes over, the first is kicked
        Bot d;
        d.Connect(port);
        auth(d, gnet::AuthMode::Login, kName, kPass);
        CHECK(WaitFor({&a, &d}, 10, [&] { return d.loggedIn && d.havePos; }), "second session logged in");
        CHECK(WaitFor({&a, &d}, 6, [&] { return a.failed; }), "first session was kicked");
        CHECK(d.pos.x == ex && d.pos.y == ey, "second session starts where the first one stood");
        enet_peer_disconnect(d.peer, 0);
        WaitFor({&d}, 1, [] { return false; });
    }
    {   // rate limit: repeated wrong passwords lock the account out, even for the right password
        bool limited = false;
        for (int i = 0; i < 8 && !limited; ++i) {
            Bot x;
            x.Connect(port);
            auth(x, gnet::AuthMode::Login, kName, "wrong" + std::to_string(i));
            WaitFor({&x}, 4, [&] { return x.authCode >= 0; });
            limited = x.authCode == (int)gnet::AuthCode::RateLimited;
        }
        CHECK(limited, "failed logins are rate limited");
        Bot y;
        y.Connect(port);
        auth(y, gnet::AuthMode::Login, kName, kPass);
        CHECK(code(y, (int)gnet::AuthCode::RateLimited), "correct password is refused during the lockout");
    }
    std::printf(g_failed ? "ACCOUNTTEST 2 FAILED\n" : "ACCOUNTTEST 2 PASSED\n");
    return g_failed ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// Tutorial scenario: f3_gametest --tutorial (server with --dev-kit). Vault: Frank (AMX behaviour 00_TutorialFrank) barks while
// the player has not talked to him, the AMX dialogue (00 Frank.scra) runs, the per player globals / journal tasks it sets are
// stored on the player (not shared), and Frank stops barking afterwards. Junktown: Armstrong's dialogue opens the tutorial tasks.
// ---------------------------------------------------------------------------------------------------------------------
static int RunTutorialTests(uint16_t port) {
    Bot a;
    if (!a.Connect(port)) { std::printf("FAIL: cannot connect\n"); return 1; }
    a.Login("TutBot");
    CHECK(WaitFor({&a}, 8, [&] { return a.loggedIn && a.havePos && a.haveStats; }), "logged in");
    if (!a.loggedIn) return 1;
    auto say = [&](const std::string& t) { net::ChatReq c; c.text = t; a.Send(net::Pack(c)); };
    if (!std::getenv("F3_AMX_ATTACKS")) { say("/amxattacks off"); WaitFor({&a}, 0.3, [] { return false; }); }   // scripted NPCs would otherwise hunt the bot (A_Attack is real)
    auto countFrom = [&](const char* who) { int n = 0; for (auto& f : a.chatFrom) if (f == who) ++n; return n; };
    auto gvar = [&](const char* name) -> double {
        size_t n = a.chat.size();
        say(std::string("/gvar ") + name);
        double v = -1;
        WaitFor({&a}, 3, [&] {
            for (size_t i = n; i < a.chat.size(); ++i) {
                size_t p = a.chat[i].find(std::string(name) + " = ");
                if (p != std::string::npos) { v = std::atof(a.chat[i].c_str() + p + std::strlen(name) + 3); return true; }
            }
            return false;
        });
        return v;
    };
    // ---- Junktown: Armstrong
    {
        say("/tp 170 572");
        WaitFor({&a}, 2, [] { return false; });
        CritterId arm = 0;
        int bestD = 1 << 20;   // Armstrong's display name is the type string ("GMC Soldier"): take the NPC closest to his post
        for (auto& [id, c] : a.critters) if (c.kind == CritterKind::Npc) { int d = std::abs(c.x - 170) + std::abs(c.y - 578); if (d < bestD) { bestD = d; arm = id; } }
        CHECK(arm != 0 && bestD < 12, "Corporal Armstrong (nearest NPC to his post, distance %d) is in view in Junktown", bestD);
        if (arm) {
            gnet::Talk t; t.critter = arm;
            a.Send(gnet::PackG(t));
            CHECK(WaitFor({&a}, 10, [&] { return !a.dlgNodes.empty(); }), "Armstrong dialogue opened (%zu nodes)", a.dlgNodes.size());
            // walk through the conversation: always the first reply until it ends
            for (int i = 0; i < 12 && a.dlgCloses == 0; ++i) {
                size_t n = a.dlgNodes.size();
                if (a.dlgNodes.back().replies.empty()) break;
                gnet::DialogChoose c; c.index = 0;
                a.Send(gnet::PackG(c));
                WaitFor({&a}, 2, [&] { return a.dlgNodes.size() > n || a.dlgCloses > 0; });
            }
            say("/gvar Task3224");
            WaitFor({&a}, 1, [] { return false; });
            std::printf("note: Task3224 (vault entrance) = %.0f\n", gvar("Task3224"));
            gnet::DialogCloseReq cl;
            a.Send(gnet::PackG(cl));
            WaitFor({&a}, 1, [] { return false; });
        }
    }
    // ---- Vault: Frank
    const size_t opensBefore = a.dlgOpens.size();
    say("/goto 00_04_Tutorial_Vault");
    CHECK(WaitFor({&a}, 8, [&] { return a.map.name == "00_04_Tutorial_Vault" && a.mapInfoCount >= 2; }), "entered the Vault");
    say("/tp 141 203");
    WaitFor({&a}, 2, [] { return false; });
    CritterId frank = 0;
    for (auto& [id, c] : a.critters) if (c.name == "Frank") frank = id;
    CHECK(frank != 0, "Frank is in view in the Vault"); if (!frank) for (auto& [id, c] : a.critters) std::printf("note: visible %u '%s' at %d,%d\n", (unsigned)id, c.name.c_str(), (int)c.x, (int)c.y);
    // Frank (OnUpdate) either starts the conversation himself (StartDialogue) or barks while the player has not talked to him
    CHECK(WaitFor({&a}, 30, [&] { return countFrom("Frank") > 0 || a.dlgOpens.size() > opensBefore; }), "Frank reacts to the newcomer (barks %d, conversations he started %zu)", countFrom("Frank"), a.dlgOpens.size() - opensBefore);
    if (frank) {
        size_t nOpens = a.dlgOpens.size();
        if (nOpens == opensBefore) {
            gnet::Talk t; t.critter = frank;
            a.Send(gnet::PackG(t));
        }
        CHECK(WaitFor({&a}, 12, [&] { return a.dlgOpens.size() > nOpens || nOpens > opensBefore; }), "Frank dialogue is open");
        for (int i = 0; i < 14 && a.dlgCloses == 0 && !a.dlgNodes.empty(); ++i) {
            size_t n = a.dlgNodes.size();
            if (a.dlgNodes.back().replies.empty()) break;
            gnet::DialogChoose c; c.index = 0;
            a.Send(gnet::PackG(c));
            WaitFor({&a}, 2, [&] { return a.dlgNodes.size() > n || a.dlgCloses > 0; });
        }
        if (!a.dlgNodes.empty()) std::printf("note: last Frank node: %.70s\n", a.dlgNodes.back().text.c_str());
        gnet::DialogCloseReq cl;
        a.Send(gnet::PackG(cl));
        WaitFor({&a}, 1, [] { return false; });
        std::printf("note: after the dialogue 00_Talked_Frank = %.0f\n", gvar("00_Talked_Frank"));
    }
    for (size_t i = 0; i < a.chat.size(); ++i) if (!a.chatFrom[i].empty()) std::printf("chat: <%s> %.80s\n", a.chatFrom[i].c_str(), a.chat[i].c_str());
    std::printf(g_failed ? "TUTORIALTEST FAILED\n" : "TUTORIALTEST PASSED\n");
    return g_failed ? 1 : 0;
}

int main(int argc, char** argv) {
    uint16_t port = 4048;
    std::string expectMap = "00_03_Tutorial_Junktown";
    for (int i = 1; i + 1 < argc; i++) {
        if (!std::strcmp(argv[i], "--port")) port = static_cast<uint16_t>(std::atoi(argv[i + 1]));
        if (!std::strcmp(argv[i], "--expect-map")) expectMap = argv[i + 1];
    }
    bool expectEmpty = false;   // server without --dev-kit: a new character owns and wears nothing
    for (int i = 1; i < argc; i++) if (!std::strcmp(argv[i], "--expect-empty")) expectEmpty = true;
    int accountPhase = 0;
    std::string stateFile = "f3_acct_state.txt";
    for (int i = 1; i + 1 < argc; i++) {
        if (!std::strcmp(argv[i], "--accounts")) accountPhase = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--state")) stateFile = argv[i + 1];
    }
    if (enet_initialize() != 0) return 2;
    if (accountPhase) return RunAccountTests(port, accountPhase, stateFile);
    for (int i = 1; i < argc; i++) if (!std::strcmp(argv[i], "--tutorial")) return RunTutorialTests(port);

    Bot a, b;
    if (!a.Connect(port) || !b.Connect(port)) { std::printf("FAIL: cannot connect to 127.0.0.1:%u\n", port); return 1; }
    a.Login("GameBotA");
    CHECK(WaitFor({&a}, 8, [&] { return a.loggedIn && a.mapInfoCount > 0 && a.havePos && a.haveStats && a.invUpdates > 0; }), "bot A logged in with map, stats, inventory");
    if (expectEmpty) {
        CHECK(a.inv.empty(), "new character has an empty inventory (%zu entries) and nothing equipped", a.inv.size());
        std::printf(g_failed ? "GAMETEST FAILED\n" : "GAMETEST PASSED (empty character)\n");
        return g_failed ? 1 : 0;
    }
    {
        gnet::ClientCaps cp; cp.caps = gnet::kCapContinuousMove;
        a.Send(gnet::PackG(cp));  // bot A is a continuous-movement client, bot B stays legacy
        CHECK(WaitFor({&a}, 3, [&] { return !a.waits.empty(); }), "WaitState received after ClientCaps");
        if (!a.waits.empty()) {
            const gnet::WaitState& w = a.waits.back().w;
            CHECK(w.maxAp == 800 && w.ap == w.maxAp, "initial AP is full: %u/%u (5 + Agility 6 / 2 = 8 AP)", w.ap, w.maxAp);
            CHECK(w.apRegenMs == 7000 && w.waitEndMs - w.nowMs < 100000 && w.waitEndMs <= w.nowMs + 1, "free at login, ApRegeneration %u ms", w.apRegenMs);
        }
    }
    b.Login("GameBotB");
    CHECK(WaitFor({&a, &b}, 8, [&] { return b.loggedIn && b.havePos && b.haveStats; }), "bot B logged in");
    if (a.failed || b.failed || !a.loggedIn || !b.loggedIn) return 1;
    { net::ChatReq aa; aa.text = "/amxattacks off"; a.Send(net::Pack(aa)); WaitFor({&a, &b}, 0.3, [] { return false; }); }   // scripted NPC aggression (A_Attack is real) must not disturb the bots

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
    {   // slots + weight (inventory UI block)
        int gunSlot = -1, armSlot = -1, wsum = 0;
        for (auto& e : a.inv) {
            if (e.name.find("9mm Auto") != std::string::npos) gunSlot = e.equipped;
            if (e.name.find("Leather") != std::string::npos) armSlot = e.equipped;
            wsum += e.weight * e.count;
        }
        CHECK(gunSlot == gnet::SlotHand1 && armSlot == gnet::SlotBody, "equip slots: gun in hand 1 (%d), armor on body (%d)", gunSlot, armSlot);
        CHECK(wsum == a.stats.carried && a.stats.carried <= a.stats.carryWeight, "weight: carried %u == sum of entries %d, limit %u", a.stats.carried, wsum, a.stats.carryWeight);
    }
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
    CHECK(WaitFor({&a, &b}, 10, [&] { return a.pos == target && !a.stops.empty() && a.stops.back().reason == 0; }), "bot A walked to %d,%d (at %d,%d)", target.x, target.y, a.pos.x, a.pos.y);
    {
        const gnet::MovePath* mp = nullptr;
        for (auto& p : a.paths) if (p.id == a.myId) mp = &p;
        CHECK(mp && mp->pts.size() >= 2 && mp->speed > 0, "MovePath received for bot A (%zu points, %.1f cells/s)", mp ? mp->pts.size() : 0, mp ? mp->speed : 0.f);
        bool allOk = mp != nullptr;
        float len = 0;
        if (mp)
            for (size_t i = 0; i + 1 < mp->pts.size(); ++i) {
                allOk = allOk && SegmentWalkable(a, mp->pts[i].x, mp->pts[i].y, mp->pts[i + 1].x, mp->pts[i + 1].y);
                len += std::hypot(mp->pts[i + 1].x - mp->pts[i].x, mp->pts[i + 1].y - mp->pts[i].y);
            }
        CHECK(allOk, "every MovePath segment lies on walkable cells (length %.1f cells)", len);
        CHECK(mp && mp->pts.size() <= 8, "path is smoothed (%zu points for a %d-cell hop)", mp ? mp->pts.size() : 0, mp ? TileDist(target, TilePos{(int16_t)mp->pts[0].x, (int16_t)mp->pts[0].y}) : 0);
        CHECK(std::fabs(a.fx - (target.x + 0.5f)) < 0.02f && std::fabs(a.fy - (target.y + 0.5f)) < 0.02f, "stopped at the cell centre (%.2f,%.2f)", a.fx, a.fy);
        WaitFor({&a, &b}, 0.7, [] { return false; });  // let the pickup/walk breaktime pass
    }
    // precise (sub-cell) destination
    {
        TilePos t2 = a.pos;
        bool f2 = false;
        for (int dx = 5; dx >= 2 && !f2; --dx)
            if (SegmentWalkable(a, a.pos.x + 0.5f, a.pos.y + 0.5f, a.pos.x + dx + 0.75f, a.pos.y + 0.25f)) { t2 = {static_cast<int16_t>(a.pos.x + dx), a.pos.y}; f2 = true; }
        if (f2) {
            size_t sc = a.stops.size();
            gnet::MoveToF mv; mv.x = t2.x + 0.75f; mv.y = t2.y + 0.25f; mv.run = 0;
            a.Send(gnet::PackG(mv));
            CHECK(WaitFor({&a, &b}, 8, [&] { return a.stops.size() > sc && a.stops.back().reason == 0; }) && std::fabs(a.fx - (t2.x + 0.75f)) < 0.02f && std::fabs(a.fy - (t2.y + 0.25f)) < 0.02f,
                  "MoveToF stops at the exact point (%.2f,%.2f), not snapped to a cell", a.fx, a.fy);
            CHECK(!a.paths.empty() && a.paths.back().run == 0 && a.paths.back().speed < 12.0f, "walk flag respected (speed %.1f)", a.paths.back().speed);
        } else {
            CHECK(false, "no sub-cell target found next to bot A");
        }
    }
    // legacy client B: per-cell CritterMove, A sees B's MovePath at run speed (RtAlwaysRun)
    {
        TilePos tb = b.pos;
        bool fb = false;
        for (int dx = 4; dx >= 2 && !fb; --dx)
            if (Walkable(b, b.pos.x + dx, b.pos.y) && SegmentWalkable(b, b.pos.x + 0.5f, b.pos.y + 0.5f, b.pos.x + dx + 0.5f, b.pos.y + 0.5f)) { tb = {static_cast<int16_t>(b.pos.x + dx), b.pos.y}; fb = true; }
        if (fb) {
            net::MoveToReq mv; mv.x = tb.x; mv.y = tb.y;
            size_t pc = a.paths.size();
            b.Send(net::Pack(mv));
            CHECK(WaitFor({&a, &b}, 8, [&] { return b.pos == tb; }), "legacy bot B reached %d,%d via per-cell CritterMove (at %d,%d)", tb.x, tb.y, b.pos.x, b.pos.y);
            bool seen = false;
            for (size_t i = pc; i < a.paths.size(); ++i) if (a.paths[i].id == b.myId && a.paths[i].speed >= 14.0f && a.paths[i].run) seen = true;
            CHECK(seen, "bot A received B's MovePath (run speed from config RunSpeed)");
        } else {
            CHECK(false, "no walk target for B");
        }
    }

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

    // --- pickup after the item left the view and came back (walk spot used to be 3 cells away, PickUp needs 2: "invalid")
    {
        auto say = [&](const std::string& t) { net::ChatReq c; c.text = t; a.Send(net::Pack(c)); };
        auto idle = [&](double sec) { WaitFor({&a, &b}, sec, [] { return false; }); };
        // a walkable cell farc (> minD) from p
        auto farCell = [&](TilePos p, int minD) {
            for (int y = 0; y < a.map.height; y += 5)
                for (int x = 0; x < a.map.width; x += 5)
                    if (Walkable(a, x, y) && TileDist(TilePos{(int16_t)x, (int16_t)y}, p) > minD) return TilePos{(int16_t)x, (int16_t)y};
            return TilePos{-1, -1};
        };
        // a walkable cell with a straight walkable line to p, between d0 and d1 cells away
        auto nearCell = [&](TilePos p, int d0, int d1) {
            for (int d = d0; d <= d1; ++d)
                for (int dy = -d; dy <= d; ++dy)
                    for (int dx = -d; dx <= d; ++dx) {
                        if (std::max(std::abs(dx), std::abs(dy)) != d) continue;
                        int x = p.x + dx, y = p.y + dy;
                        if (Walkable(a, x, y) && SegmentWalkable(a, x + 0.5f, y + 0.5f, p.x + 0.5f, p.y + 0.5f)) return TilePos{(int16_t)x, (int16_t)y};
                    }
            return TilePos{-1, -1};
        };
        auto tp = [&](TilePos t) { say("/tp " + std::to_string(t.x) + " " + std::to_string(t.y)); };
        idle(1.0);
        // ---- bug 2: pick up right after coming back into view
        std::map<ItemId, bool> known;
        for (auto& [id, it] : a.items) known[id] = true;
        say("/spawn Weapons44_Revolver.WEA");
        ItemId drop = 0;
        WaitFor({&a, &b}, 3, [&] { for (auto& [id, it] : a.items) if (!known.count(id)) { drop = id; return true; } return false; });
        CHECK(drop != 0, "reload test: a revolver was spawned next to bot A");
        if (drop) {
            TilePos ip{a.items[drop].x, a.items[drop].y};
            TilePos farc = farCell(ip, 60), backc = nearCell(ip, 8, 12);
            CHECK(farc.x >= 0 && backc.x >= 0, "reload test: found a far cell (%d,%d) and a return cell (%d,%d)", farc.x, farc.y, backc.x, backc.y);
            if (farc.x >= 0 && backc.x >= 0) {
                tp(farc);
                CHECK(WaitFor({&a, &b}, 5, [&] { return !a.items.count(drop); }), "the item left bot A's view range (ItemRemove)");
                tp(backc);
                CHECK(WaitFor({&a, &b}, 5, [&] { return a.items.count(drop) != 0; }), "the item re-entered the view (same id %u)", drop);
                idle(1.0);
                size_t wc = a.waits.size();
                int upd = a.invUpdates;
                gnet::PickUp pu; pu.id = drop;
                a.Send(gnet::PackG(pu));   // immediately, from ~8 cells away: the server walks the player and picks up on arrival
                bool done = WaitFor({&a, &b}, 15, [&] { return !a.items.count(drop) && a.invUpdates > upd; });
                bool invalid = false;
                for (size_t i = wc; i < a.waits.size(); ++i)
                    if (a.waits[i].w.action == (uint8_t)GMsg::C2S_PickUp && a.waits[i].w.result == gnet::ActionResult::Invalid) invalid = true;
                CHECK(done && !invalid, "picked up the returned item at the first attempt (done %d, invalid answer %d, bot at %d,%d, item at %d,%d)", (int)done, (int)invalid, a.pos.x, a.pos.y, ip.x, ip.y);
                if (!done || invalid) for (size_t i = a.chat.size() > 3 ? a.chat.size() - 3 : 0; i < a.chat.size(); ++i) std::printf("note: chat: %s\n", a.chat[i].c_str());
            }
        }
        idle(1.0);
    }

    // --- NPC AI: the Junktown insurgent (team 2, cell ~183,226) must attack a nearby player (hostile.lua + server combat)
    {
        net::ChatReq c; c.text = "/tp 183 232";
        b.Send(net::Pack(c));
        CritterId bid = b.myId;
        bool ok = WaitFor({&a, &b}, 25, [&] {
            for (auto& e : b.combat) if (e.target == bid && e.attacker != bid) return true;
            return false;
        });
        CHECK(ok, "hostile NPC attacked bot B near the insurgent (%zu combat events)", b.combat.size());
        net::ChatReq h; h.text = "/heal";
        b.Send(net::Pack(h));
        net::ChatReq back; back.text = "/tp " + std::to_string(a.pos.x + 3) + " " + std::to_string(a.pos.y);
        b.Send(net::Pack(back));
        WaitFor({&a, &b}, 3, [&] { return false; });
        b.combat.clear();
    }

    // --- NPC aggro on a MISS: a third bot C (disconnects afterwards so the NPC does not keep hunting) attacks a neutral
    // (idle) NPC with forced misses; the NPC must still turn on C.
    {
        Bot c;
        if (!c.Connect(port)) { CHECK(false, "bot C connects"); } else {
        c.Login("GameBotC");
        CHECK(WaitFor({&a, &b, &c}, 8, [&] { return c.loggedIn && c.havePos && c.haveStats; }), "bot C logged in");
        net::ChatReq fm; fm.text = "/forcemiss on";
        c.Send(net::Pack(fm));
        net::ChatReq nr; nr.text = "/near GMC Soldier";
        c.Send(net::Pack(nr));
        WaitFor({&a, &b, &c}, 3, [&] { return false; });
        CritterId npc = 0;
        int bd = 1 << 30;
        for (auto& [id, cr] : c.critters) {
            if (id == a.myId || id == b.myId || id == c.myId || cr.name.find("GMC Soldier") == std::string::npos) continue;
            int d = TileDist(TilePos{cr.x, cr.y}, c.pos);
            if (d < bd) { bd = d; npc = id; }
        }
        CHECK(npc != 0, "found neutral NPC 'GMC Soldier' near bot C");
        if (npc) {
            c.combat.clear();
            gnet::Attack at; at.id = npc;
            c.Send(gnet::PackG(at));
            bool shot = WaitFor({&a, &b, &c}, 10, [&] { for (auto& e : c.combat) if (e.attacker == c.myId) return true; return false; });
            bool allMiss = shot;
            for (auto& e : c.combat) if (e.attacker == c.myId && e.result != gnet::HitResult::Miss) allMiss = false;
            CHECK(allMiss, "forced miss: C's shots at the NPC all missed (%zu events)", c.combat.size());
            bool reacted = WaitFor({&a, &b, &c}, 15, [&] { for (auto& e : c.combat) if (e.attacker == npc && e.target == c.myId) return true; return false; });
            CHECK(reacted, "NPC turned on C after a MISS (retaliating combat event)");
        }
        fm.text = "/forcemiss off";
        c.Send(net::Pack(fm));
        WaitFor({&a, &b, &c}, 0.5, [] { return false; });
        enet_peer_disconnect_now(c.peer, 0);
        enet_host_flush(c.host);
        WaitFor({&a, &b}, 1, [] { return false; });
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
        WaitFor({&a, &b}, 4, [&] { return a.combat.size() >= 3; });  // a few shots for the interval check
        WaitFor({&a, &b}, 1, [] { return false; });
        auto st = b.states.find(b.myId);
        CHECK(st != b.states.end() && st->second.hp < st->second.maxHp, "bot B received HP update (hp %d)", st != b.states.end() ? st->second.hp : -1);
        gnet::Attack stop; stop.id = 0;
        a.Send(gnet::PackG(stop));
        // real-time attack timing: shots are at least one action duration apart; each shot spends AP and sets a wait
        double minGap = 1e9;
        std::vector<double> mine;
        for (size_t i = 0; i < a.combat.size(); ++i) if (a.combat[i].attacker == a.myId) mine.push_back(a.combatMs[i]);
        for (size_t i = 1; i < mine.size(); ++i) minGap = std::min(minGap, mine[i] - mine[i - 1]);
        CHECK(mine.size() < 2 || minGap >= 400, "attack breaktime: shots at least %.0f ms apart (%zu shots, config AttackMinMs 500)", minGap, mine.size());
        const gnet::WaitState* aw = LastWait(a, (uint8_t)GMsg::C2S_Attack);
        CHECK(aw && aw->result == gnet::ActionResult::Ok && aw->waitEndMs - aw->nowMs >= 450 && aw->ap < aw->maxAp,
              "attack WaitState: wait %u ms, ap %u/%u (action cost spent)", aw ? aw->waitEndMs - aw->nowMs : 0, aw ? aw->ap : 0, aw ? aw->maxAp : 0);
        if (aw) {
            uint16_t apAfter = aw->ap;
            double t0 = Bot::LocalMs();
            WaitFor({&a, &b}, 2.0, [] { return false; });
            size_t wc = a.waits.size();
            gnet::ReqState rq;
            a.Send(gnet::PackG(rq));
            WaitFor({&a, &b}, 2, [&] { return a.waits.size() > wc; });
            const gnet::WaitState& nw = a.waits.back().w;
            double dt = Bot::LocalMs() - t0;
            // 800 AP-units over 7000 ms = 0.114 per ms; after ~2 s expect about +230 (or full)
            CHECK(a.waits.size() > wc && (nw.ap > apAfter || nw.ap == nw.maxAp) && (nw.ap >= nw.maxAp || nw.ap - apAfter < 0.114 * (dt + 400)),
                  "AP regenerated server-side: %u -> %u of %u after %.0f ms (ApRegeneration 7000)", apAfter, nw.ap, nw.maxAp, dt);
        }
    }
    // --- walking away from a fight: an attack's wait is short (cap), a walk request during it is answered Busy but is
    // queued (the auto attack stops at once) and starts when the wait ends; the battle flag never blocks walking.
    {
        WaitFor({&a, &b}, 2.5, [] { return false; });   // previous action finished, AP regenerated
        CritterId bid = b.myId;
        // a walkable target away from B (B stands a few cells to the right of A)
        TilePos away = a.pos;
        bool fa = false;
        for (int r = 6; r >= 3 && !fa; --r)
            for (int dy = -r; dy <= r && !fa; ++dy)
                for (int dx = -r; dx <= 0 && !fa; ++dx)
                    if (std::max(std::abs(dx), std::abs(dy)) == r && Walkable(a, a.pos.x + dx, a.pos.y + dy) &&
                        SegmentWalkable(a, a.fx > 0 ? a.fx : a.pos.x + 0.5f, a.fy > 0 ? a.fy : a.pos.y + 0.5f, a.pos.x + dx + 0.5f, a.pos.y + dy + 0.5f)) {
                        away = {static_cast<int16_t>(a.pos.x + dx), static_cast<int16_t>(a.pos.y + dy)};
                        fa = true;
                    }
        CHECK(fa, "found a walkable cell away from the fight (%d,%d)", away.x, away.y);
        size_t combat0 = a.combat.size();
        gnet::Attack at; at.id = bid;
        a.Send(gnet::PackG(at));
        bool shot = WaitFor({&a, &b}, 8, [&] { for (size_t i = combat0; i < a.combat.size(); ++i) if (a.combat[i].attacker == a.myId) return true; return false; });
        CHECK(shot, "attack started (first shot)");
        const double tShot = Bot::LocalMs();
        const gnet::WaitState* aw = LastWait(a, (uint8_t)GMsg::C2S_Attack);
        CHECK(aw && aw->waitEndMs - aw->nowMs <= 1000, "attack wait is capped: %u ms (config AttackMaxMs 1000)", aw ? aw->waitEndMs - aw->nowMs : 0);
        const float startX = a.fx, startY = a.fy;
        size_t paths0 = a.paths.size(), waits0 = a.waits.size();
        gnet::MoveToF mv; mv.x = away.x + 0.5f; mv.y = away.y + 0.5f; mv.run = 0;
        a.Send(gnet::PackG(mv));    // walk away right in the middle of the attack's wait window
        bool busyAnswer = WaitFor({&a, &b}, 1.0, [&] {
            for (size_t i = waits0; i < a.waits.size(); ++i)
                if (a.waits[i].w.action == (uint8_t)GMsgRt::C2S_MoveToF && a.waits[i].w.result == gnet::ActionResult::Busy) return true;
            return false;
        });
        CHECK(busyAnswer, "walk request inside the attack wait is answered Busy (feedback)");
        bool started = WaitFor({&a, &b}, 3, [&] { return a.paths.size() > paths0 && a.paths.back().id == a.myId; });
        const double tStart = Bot::LocalMs();
        CHECK(started && tStart - tShot < 1500, "queued walk starts when the wait ends (%.0f ms after the shot)", tStart - tShot);
        size_t shotsAfter = 0;
        const size_t combat1 = a.combat.size();
        // (within one cell: AMX-driven NPCs wander now (A_Wander) and may stand on the goal cell)
        CHECK(WaitFor({&a, &b}, 10, [&] { return TileDist(a.pos, away) <= 1 && !a.stops.empty() && a.stops.back().reason == 0; }), "bot A walked away to %d,%d (at %d,%d)", away.x, away.y, a.pos.x, a.pos.y);
        for (size_t i = combat1; i < a.combat.size(); ++i) if (a.combat[i].attacker == a.myId) ++shotsAfter;
        CHECK(shotsAfter == 0, "the auto attack stopped when walking away (%zu shots after the walk started)", shotsAfter);
        CHECK(std::hypot(a.fx - startX, a.fy - startY) >= 2.0f, "bot A really moved (%.1f cells)", std::hypot(a.fx - startX, a.fy - startY));

        // battle flag set for 5 s: walking is not blocked by it (RunOnCombat only decides run vs walk)
        WaitFor({&a, &b}, 1.2, [] { return false; });
        net::ChatReq bc; bc.text = "/battle 5000";
        size_t wb = a.waits.size();
        a.Send(net::Pack(bc));
        CHECK(WaitFor({&a, &b}, 3, [&] { return a.waits.size() > wb && a.waits.back().w.battleEndMs - a.waits.back().w.nowMs > 4000; }), "battle flag active for ~5 s (/battle)");
        TilePos back = a.pos;
        bool fb = false;
        for (int r = 6; r >= 3 && !fb; --r)
            for (int dy = -r; dy <= r && !fb; ++dy)
                for (int dx = -r; dx <= r && !fb; ++dx)
                    if (std::max(std::abs(dx), std::abs(dy)) == r && Walkable(a, a.pos.x + dx, a.pos.y + dy) &&
                        SegmentWalkable(a, a.fx, a.fy, a.pos.x + dx + 0.5f, a.pos.y + dy + 0.5f)) { back = {static_cast<int16_t>(a.pos.x + dx), static_cast<int16_t>(a.pos.y + dy)}; fb = true; }
        size_t p1 = a.paths.size();
        gnet::MoveToF m2; m2.x = back.x + 0.5f; m2.y = back.y + 0.5f; m2.run = 1;
        a.Send(gnet::PackG(m2));
        CHECK(WaitFor({&a, &b}, 1.0, [&] { return a.paths.size() > p1; }), "move accepted immediately while the battle flag is set (MovePath)");
        CHECK(!a.waits.empty() && a.waits.back().w.battleEndMs > a.waits.back().w.nowMs, "battle flag still set during the walk");
        CHECK(WaitFor({&a, &b}, 10, [&] { return a.pos == back && !a.stops.empty() && a.stops.back().reason == 0; }), "bot A arrived at %d,%d during battle (at %d,%d)", back.x, back.y, a.pos.x, a.pos.y);
        WaitFor({&a, &b}, 0.6, [] { return false; });
    }
    // --- breaktime: Equip twice in a row, the second one is rejected as Busy; moving while busy is rejected too
    {
        ItemId gun = 0;
        for (auto& e : a.inv) if (e.equipped && e.name.find("9mm") != std::string::npos) gun = e.id;
        if (!gun) for (auto& e : a.inv) if (e.name.find("9mm") != std::string::npos) gun = e.id;
        CHECK(gun != 0, "found the 9mm autoloader in the inventory");
        WaitFor({&a, &b}, 4.5, [&] { return false; });  // let the last attack's wait end
        size_t w0 = a.waits.size();
        gnet::Equip e1; e1.id = gun; e1.on = 0;
        gnet::Equip e2; e2.id = gun; e2.on = 1;
        a.Send(gnet::PackG(e1));
        a.Send(gnet::PackG(e2));
        CHECK(WaitFor({&a, &b}, 3, [&] { return a.waits.size() >= w0 + 2; }), "two WaitState answers for two quick Equip requests");
        if (a.waits.size() >= w0 + 2) {
            const gnet::WaitState& r1 = a.waits[w0].w;
            const gnet::WaitState& r2 = a.waits[w0 + 1].w;
            CHECK(r1.action == (uint8_t)GMsg::C2S_Equip && r1.result == gnet::ActionResult::Ok && r1.waitEndMs - r1.nowMs >= 450 && r1.waitEndMs - r1.nowMs <= 520,
                  "first Equip accepted, breaktime %u ms (config Breaktime 500)", r1.waitEndMs - r1.nowMs);
            CHECK(r2.action == (uint8_t)GMsg::C2S_Equip && r2.result == gnet::ActionResult::Busy && r2.waitEndMs > r2.nowMs,
                  "second Equip rejected as Busy (%u ms left)", r2.waitEndMs - r2.nowMs);
        }
        size_t pc = a.paths.size(), w1 = a.waits.size();
        gnet::MoveToF mv; mv.x = a.fx + 2; mv.y = a.fy; mv.run = 1;
        a.Send(gnet::PackG(mv));
        CHECK(WaitFor({&a, &b}, 2, [&] { return a.waits.size() > w1; }) && a.waits.back().w.result == gnet::ActionResult::Busy && a.paths.size() == pc,
              "move request while busy is answered Busy (no MovePath yet, the walk is queued)");
        WaitFor({&a, &b}, 0.7, [] { return false; });
        w1 = a.waits.size();
        a.Send(gnet::PackG(e2));
        CHECK(WaitFor({&a, &b}, 2, [&] { return a.waits.size() > w1; }) && a.waits.back().w.result == gnet::ActionResult::Ok, "Equip accepted again after the breaktime (%zu entries)", a.inv.size());
        WaitFor({&a, &b}, 0.7, [] { return false; });
    }
    // --- hand switching with an empty hand: hand 1 -> hand 2 leaves the active hand empty (holstered), and back
    {
        ItemId gun = 0;
        for (auto& e : a.inv) if (e.equipped == gnet::SlotHand1 && e.name.find("9mm") != std::string::npos) gun = e.id;
        auto slotOf = [&](ItemId id) { for (auto& e : a.inv) if (e.id == id) return (int)e.equipped; return -1; };
        if (gun) {
            WaitFor({&a, &b}, 1.0, [] { return false; });
            gnet::Equip toH2; toH2.id = gun; toH2.on = 1; toH2.slot = gnet::SlotHand2;
            a.Send(gnet::PackG(toH2));
            CHECK(WaitFor({&a, &b}, 3, [&] { return slotOf(gun) == gnet::SlotHand2; }), "weapon moved to hand 2, the active hand 1 is now empty (slot %d)", slotOf(gun));
            WaitFor({&a, &b}, 0.7, [] { return false; });
            gnet::Equip toH1; toH1.id = gun; toH1.on = 1; toH1.slot = gnet::SlotHand1;
            a.Send(gnet::PackG(toH1));
            CHECK(WaitFor({&a, &b}, 3, [&] { return slotOf(gun) == gnet::SlotHand1; }), "weapon back in the active hand (slot %d)", slotOf(gun));
            WaitFor({&a, &b}, 0.7, [] { return false; });
        }
    }

    // --- dialogue: Corporal Armstrong (00 Corporal Armstrong.scra) runs through the original conversation graph
    {
        net::ChatReq c; c.text = "/near Armstrong";
        a.Send(net::Pack(c));
        CritterId arm = 0;
        CHECK(WaitFor({&a, &b}, 5, [&] {
                  int best = 99999;   // Armstrong's prototype is displayed as "GMC Soldier": take the nearest one (we stand next to him)
                  for (auto& [id, cr] : a.critters)
                      if (cr.name == "GMC Soldier" && TileDist(TilePos{cr.x, cr.y}, a.pos) < best) { best = TileDist(TilePos{cr.x, cr.y}, a.pos); arm = id; }
                  return arm != 0 && best <= 4;
              }), "Armstrong is in view after /near");
        if (arm) {
            gnet::Talk t; t.critter = arm;
            a.Send(gnet::PackG(t));
            bool got = WaitFor({&a, &b}, 8, [&] { return !a.dlgOpens.empty() && !a.dlgNodes.empty(); });
            CHECK(got, "Talk -> DialogOpen + DialogNode received");
            if (got) {
                CHECK(a.dlgOpens[0].npc == arm, "DialogOpen names the NPC (%s)", a.dlgOpens[0].name.c_str());
                const gnet::DialogNode& n0 = a.dlgNodes[0];
                CHECK(!n0.text.empty() && !n0.replies.empty(), "first node has text and %zu replies: \"%.60s\"", n0.replies.size(), n0.text.c_str());
                // walk the conversation: always choose the first reply; every answer must be a new node or a close
                int steps = 0; size_t seen = a.dlgNodes.size(); bool ended = false;
                std::string last = n0.text;
                for (; steps < 20 && !ended; ++steps) {
                    gnet::DialogChoose ch; ch.index = 0;
                    int closes = a.dlgCloses;
                    a.Send(gnet::PackG(ch));
                    WaitFor({&a, &b}, 3, [&] { return a.dlgNodes.size() > seen || a.dlgCloses > closes; });
                    if (a.dlgCloses > closes) ended = true;
                    else if (a.dlgNodes.size() > seen) {
                        seen = a.dlgNodes.size(); last = a.dlgNodes.back().text;
                        std::printf("      dlg step %d: \"%.70s\" (%zu replies; first \"%.40s\")\n", steps, last.c_str(), a.dlgNodes.back().replies.size(), a.dlgNodes.back().replies.empty() ? "" : a.dlgNodes.back().replies[0].text.c_str());
                    }
                    else break;
                }
                CHECK(ended || steps >= 3, "conversation advanced %d choices (ended=%d), last text \"%.50s\"", steps, ended ? 1 : 0, last.c_str());
            }
            if (got && !a.dlgNodes.empty() && a.dlgCloses == 0) {   // the last reply of every node is the goodbye
                gnet::DialogChoose ch; ch.index = static_cast<uint8_t>(a.dlgNodes.back().replies.size() - 1);
                std::printf("      leaving with \"%.40s\"\n", a.dlgNodes.back().replies.back().text.c_str());
                a.Send(gnet::PackG(ch));
                CHECK(WaitFor({&a, &b}, 3, [&] { return a.dlgCloses > 0; }), "the last reply ends the conversation (DialogClose)");
            }
            gnet::DialogCloseReq cl;
            a.Send(gnet::PackG(cl));
            WaitFor({&a, &b}, 0.3, [] { return false; });
        }
    }

    // --- AMX natives vs. the original (docs/research/vbdecomp-script-natives.md). `/amxnative <program>` runs natives by name on a --dev-kit server
    // (AmxHost::NativeTest): int 12 | f1.5 float | v1,2,3 vector | p<critter> entity position | s<text> string | n<critter> instance name |
    // &buf output buffer | @buf its first cell | `name=Native ...` stores the result in buf `name`; calls are separated by ';'.
    {
        auto parts = [](const std::string& r) {
            std::vector<std::string> v;
            std::string s = r.rfind("amx: ", 0) == 0 ? r.substr(5) : r;
            size_t p = 0;
            while (true) {
                size_t q = s.find(" | ", p);
                v.push_back(s.substr(p, q == std::string::npos ? std::string::npos : q - p));
                if (q == std::string::npos) break;
                p = q + 3;
            }
            return v;
        };
        auto amx = [&](const std::string& prog) {
            WaitFor({&a, &b}, 0.28, [] { return false; });   // chat rate limit: 10 requests, +1 per 250 ms
            size_t n = a.chat.size();
            net::ChatReq c; c.text = "/amxnative " + prog;
            a.Send(net::Pack(c));
            std::string res;
            WaitFor({&a, &b}, 4, [&] { for (size_t i = n; i < a.chat.size(); ++i) if (a.chat[i].rfind("amx: ", 0) == 0) { res = a.chat[i]; return true; } return false; });
            return parts(res);
        };
        auto ret = [](const std::vector<std::string>& v, size_t i) -> long long {   // "Native=<int>[<float>]"
            if (i >= v.size()) return -999999;
            size_t e = v[i].find('=');
            return e == std::string::npos ? -999999 : std::atoll(v[i].c_str() + e + 1);
        };
        auto buf = [](const std::vector<std::string>& v, size_t i, float f[3]) -> bool {   // " name:i=.. f=(x,y,z) s='..'"
            if (i >= v.size()) return false;
            size_t p = v[i].find("f=(");
            return p != std::string::npos && std::sscanf(v[i].c_str() + p, "f=(%f,%f,%f)", &f[0], &f[1], &f[2]) == 3;
        };
        auto bufStr = [](const std::vector<std::string>& v, size_t i) -> std::string {
            if (i >= v.size()) return "";
            size_t p = v[i].find("s='");
            return p == std::string::npos ? "" : v[i].substr(p + 3, v[i].size() - p - 4);
        };
        auto say = [&](const std::string& t) { net::ChatReq c; c.text = t; a.Send(net::Pack(c)); WaitFor({&a, &b}, 0.28, [] { return false; }); };
        const std::string A = std::to_string(a.myId), B = std::to_string(b.myId);
        WaitFor({&a, &b}, 1.0, [] { return false; });
        auto stand = [&](const Bot& x, float& fx, float& fy) { fx = (&x == &a) ? a.fx : x.pos.x + 0.5f; fy = (&x == &a) ? a.fy : x.pos.y + 0.5f; };

        // integer returns: Distance / PointDistance / GetDirectionToEntity are truncated ints in world units (2 cells), not float bits
        {
            float ax, ay, bx, by;
            stand(a, ax, ay); stand(b, bx, by);
            const double dist = std::hypot(ax - bx, ay - by) / 2.0;
            auto r = amx("Distance " + A + " " + B + ";Distance " + A + ";Distance " + A + " " + B + " " + A + ";PointDistance v0,0,0 v3,0,4;PointDistance v0,0,0 v1,0,1;PointDistance v1,5,1 v1,5,1");
            CHECK(r.size() == 6 && std::fabs(static_cast<double>(ret(r, 0)) - dist) <= 1.0 && ret(r, 0) <= dist + 0.001 && ret(r, 0) >= 0,
                  "Distance(A,B) is the truncated world-unit distance: %lld (exact %.2f)", ret(r, 0), dist);
            CHECK(r.size() == 6 && ret(r, 1) == 0 && ret(r, 2) == 0, "Distance with 1 or 3 arguments returns 0 (exact argument count like VerifyScriptParameters)");
            CHECK(r.size() == 6 && ret(r, 3) == 5 && ret(r, 4) == 1 && ret(r, 5) == 0, "PointDistance (float[3] vectors) is a truncated int: 5, 1 (1.41), 0");
            // direction A -> B: acos(z) in degrees, negative for x < 0, truncated (0 = +Z = cell +y)
            const double ex = (bx - ax), ez = (by - ay), len = std::hypot(ex, ez);
            double yaw = len > 0 ? std::acos(ez / len) * 57.295776 : 0;
            if (ex < 0) yaw = -yaw;
            auto d = amx("GetDirectionToEntity " + A + " " + B);
            CHECK(d.size() == 1 && std::fabs(static_cast<double>(ret(d, 0)) - yaw) <= 1.0 && (yaw >= 0) == (ret(d, 0) >= 0 || std::fabs(yaw) < 1.0),
                  "GetDirectionToEntity(A,B) = %lld degrees (exact %.2f)", ret(d, 0), yaw);
        }
        // facing: integer degrees, 0 = +Z, 90 = +X, +-180 = -Z; 8 directions stored, so 37 reads back 45
        {
            auto r = amx("SetFacing " + A + " 90;GetFacing " + A + ";SetFacing " + A + " 180;GetFacing " + A + ";SetFacing " + A + " -90;GetFacing " + A + ";SetFacing " + A + " 0;GetFacing " + A + ";SetFacing " + A + " 37;GetFacing " + A);
            CHECK(r.size() == 10 && ret(r, 1) == 90 && ret(r, 3) == 180 && ret(r, 5) == -90 && ret(r, 7) == 0 && ret(r, 9) == 45,
                  "SetFacing/GetFacing round trip in integer degrees: %lld %lld %lld %lld %lld", ret(r, 1), ret(r, 3), ret(r, 5), ret(r, 7), ret(r, 9));
        }
        // Pawn float module
        {
            auto r = amx("floatdiv f1 f0;floatdiv f-1 f0;floatdiv f1 f2;x=floatdiv f0 f0;floatcmp @x f1;floatfract f-1.25");
            CHECK(r.size() == 6 && ret(r, 0) == 0x7F800000 && ret(r, 1) == static_cast<int>(0xFF800000u) && ret(r, 2) == 0x3F000000, "floatdiv by zero is +-infinity (IEEE), 1/2 = 0.5");
            CHECK(r.size() == 6 && ret(r, 4) == -1, "floatcmp with NaN is -1 (not equal, not greater)");
            CHECK(r.size() == 6 && ret(r, 5) == 0x3F400000, "floatfract(-1.25) = 0.75 (x - floor(x)): bits %lld", ret(r, 5));
            auto q = amx("floatround f2.5 0;floatround f-2.5 0;floatround f-1.5 1;floatround f1.2 2;floatround f-1.7 3;floatround f1.7 3;floatround f2.5");
            CHECK(q.size() == 7 && ret(q, 0) == 3 && ret(q, 1) == -2 && ret(q, 2) == -2 && ret(q, 3) == 2 && ret(q, 4) == -1 && ret(q, 5) == 1 && ret(q, 6) == 3,
                  "floatround modes: round(2.5)=3, round(-2.5)=-2 (floor(x+.5)), floor(-1.5)=-2, ceil(1.2)=2, trunc(-1.7)=-1, trunc(1.7)=1, no mode = round: %lld %lld %lld %lld %lld %lld %lld",
                  ret(q, 0), ret(q, 1), ret(q, 2), ret(q, 3), ret(q, 4), ret(q, 5), ret(q, 6));
        }
        // AddToSquad(entity, squad): entity first
        {
            auto r = amx("sq=CreateSquad;AddToSquad " + A + " @sq;GetSquadID " + A + ";AddToSquad " + B + " @sq;GetSquadID " + B);
            CHECK(r.size() == 5 && ret(r, 0) >= 1 && ret(r, 1) == 1 && ret(r, 2) == ret(r, 0) && ret(r, 4) == ret(r, 0),
                  "AddToSquad(entity, squad) assigns the squad (squad %lld, A in %lld, B in %lld)", ret(r, 0), ret(r, 2), ret(r, 4));
        }
        // HasItem is a COUNT (resource name, case-insensitive)
        {
            auto r = amx("HasItem " + A + " sArmorLeather_Outfit.ARM;HasItem " + A + " sarmorleather_outfit.arm;HasItem " + A + " sKitsTool_Kit.ITM;HasItem " + A + " sAmmo9mm.AMO;HasItem " + A);
            int ammo = 0, ammoEntries = 0;
            for (auto& e : a.inv) if (e.name.find("Ammo") != std::string::npos || e.name.find("ammo") != std::string::npos) { ammo += e.count; ++ammoEntries; }
            CHECK(r.size() == 5 && ret(r, 0) == 1 && ret(r, 1) == 1 && ret(r, 2) == 0 && ret(r, 4) == 0, "HasItem returns the count: leather outfit %lld, any case %lld, absent %lld, wrong argument count %lld", ret(r, 0), ret(r, 1), ret(r, 2), ret(r, 4));
            CHECK(r.size() == 5 && ret(r, 3) >= 1 && (ammoEntries == 0 || ret(r, 3) <= ammo), "HasItem(Ammo9mm.AMO) is a count (%lld of %d ammo in the inventory)", ret(r, 3), ammo);
        }
        // line of sight on the walkability grid (approximation, see docs/amx-scripts.md): compare with a client side Bresenham
        {
            auto los = [&](TilePos p, TilePos q) {   // endpoints excluded, same walk as AmxHost::LineClear
                int x0 = p.x, y0 = p.y, x1 = q.x, y1 = q.y;
                const int sx0 = x0, sy0 = y0;
                int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
                for (int g = 0; g < 4096; ++g) {
                    if (!(x0 == sx0 && y0 == sy0) && !(x0 == x1 && y0 == y1) && !Walkable(a, x0, y0)) return false;
                    if (x0 == x1 && y0 == y1) break;
                    int e2 = 2 * err;
                    if (e2 >= dy) { err += dy; x0 += sx; }
                    if (e2 <= dx) { err += dx; y0 += sy; }
                }
                return true;
            };
            auto pr = amx("GetPosition " + A + " &p;GetLOS " + A + " " + B + ";GetLOS " + A + " 0;GetLOS " + A);
            float p[3] = {0, 0, 0};
            CHECK(pr.size() == 4 && buf(pr, 0, p), "GetPosition writes a float[3] in world units (%.2f, %.2f, %.2f)", p[0], p[1], p[2]);
            CHECK(pr.size() == 4 && ret(pr, 1) == (los(a.pos, b.pos) ? 2 : 0), "GetLOS(A,B) = %lld matches the grid line (%d)", ret(pr, 1), los(a.pos, b.pos) ? 2 : 0);
            CHECK(pr.size() == 4 && ret(pr, 2) == 0 && ret(pr, 3) == 0, "GetLOS with an unknown entity or a wrong argument count returns 0");
            const float ox = p[0] - a.fx * 0.5f, oz = p[2] - a.fy * 0.5f;   // world = cell / 2 + map origin
            int clear = 0, blocked = 0, bad = 0;
            std::string prog;
            std::vector<int> expect;
            for (int k = 0; k < 6; ++k) {
                int ang = k * 60;
                for (int r = 8; r <= 40; r += 8) {
                    TilePos t{static_cast<int16_t>(a.pos.x + std::lround(std::cos(ang * 3.14159 / 180) * r)), static_cast<int16_t>(a.pos.y + std::lround(std::sin(ang * 3.14159 / 180) * r))};
                    if (!Walkable(a, t.x, t.y)) continue;
                    char tmp[96];
                    std::snprintf(tmp, sizeof tmp, "%sGetLOSPoint %s v%.4f,0,%.4f", prog.empty() ? "" : ";", A.c_str(), ox + (t.x + 0.5f) * 0.5f, oz + (t.y + 0.5f) * 0.5f);
                    if (prog.size() + std::strlen(tmp) > 150) break;
                    prog += tmp;
                    expect.push_back(los(a.pos, t) ? 2 : 0);
                }
            }
            auto lr = amx(prog);
            for (size_t i = 0; i < expect.size() && i < lr.size(); ++i) { if (ret(lr, i) == expect[i]) (expect[i] ? ++clear : ++blocked); else ++bad; }
            CHECK(lr.size() == expect.size() && bad == 0 && clear + blocked > 3, "GetLOSPoint agrees with the grid line for %zu points (%d clear, %d blocked, %d mismatches)", expect.size(), clear, blocked, bad);
        }
        // IsUnderFOW: players see themselves; a critter far from every player is under fog
        {
            auto r = amx("IsUnderFOW " + A + ";IsUnderFOW " + B + ";IsUnderFOW 0;IsUnderFOW 2;Distance " + A + " 2");
            CHECK(r.size() == 5 && ret(r, 0) == 0 && ret(r, 1) == 0 && ret(r, 2) == 0, "IsUnderFOW is 0 for players (and for an unknown entity)");
            if (r.size() == 5 && ret(r, 4) * 2 > 60) CHECK(ret(r, 3) == 1, "IsUnderFOW is 1 for the far insurgent (%lld units from every player)", ret(r, 4));
            else std::printf("note: critter 2 is %lld units away, skipped the under-fog check\n", r.size() == 5 ? ret(r, 4) : -1);
        }
        // FindObjects (farthest first, owner excluded, output cell untouched when nothing is found) + GetObject + GetType
        {
            auto r = amx("n=FindObjects &h p" + A + " f30;x0=GetObject @h 0;x1=GetObject @h 1;d0=Distance " + A + " @x0;d1=Distance " + A + " @x1;GetType @x0;n2=FindObjects &g v-500,0,-500 f1");
            const long long n = ret(r, 0), x0 = ret(r, 1), x1 = ret(r, 2), d0 = ret(r, 3), d1 = ret(r, 4);
            CHECK(r.size() == 7 && n >= 2 && x0 > 0 && x1 > 0 && x0 != a.myId && x1 != a.myId, "FindObjects finds %lld entities within 30 units, the owner is not among them (%lld, %lld)", n, x0, x1);
            CHECK(r.size() == 7 && d0 >= d1 && d0 <= 31, "FindObjects sorts farthest first (distances %lld >= %lld)", d0, d1);
            CHECK(r.size() == 7 && ret(r, 5) >= 1 && ret(r, 5) <= 9, "GetType of a found entity is an entity class (%lld)", ret(r, 5));
            CHECK(r.size() == 7 && ret(r, 6) == 0 && r.size() > 6 && r[6].find("g:i=0") != std::string::npos, "FindObjects with nothing in range returns 0 and leaves the output cell unchanged (%s)", r.size() > 6 ? r[6].c_str() : "?");
        }
        // waypoints (EPTH) and nav points (EMNP): map data of the owner's map
        {
            auto r = amx("GetWayPointCount sWaypoint01;GetWayPointCount sWaypoint04;GetWayPointCount sNoSuchPath;GetWayPointPosition sWaypoint01 1 &w");
            auto r2 = amx("GetWayPointPosition sWaypoint01 4 &w2;GetWayPointPosition sNoSuchPath 0 &w3;GetNearestPath " + A + " &nm;GetNavPointCount;GetNavPointPosition 0 &np");
            float w[3] = {0, 0, 0};
            CHECK(r.size() == 4 && ret(r, 0) == 4 && ret(r, 1) == 5 && ret(r, 2) == 0, "GetWayPointCount: Waypoint01 has %lld, Waypoint04 %lld, unknown 0 (%lld)", ret(r, 0), ret(r, 1), ret(r, 2));
            CHECK(r.size() == 4 && ret(r, 3) == 1 && buf(r, 3, w) && std::fabs(w[0] - 91.9215f) < 0.01f && std::fabs(w[2] - 182.1852f) < 0.01f && w[1] == 0.f,
                  "GetWayPointPosition(Waypoint01, 1) = (%.3f, %.3f, %.3f) world units", w[0], w[1], w[2]);
            CHECK(r2.size() == 5 && ret(r2, 0) == 0 && ret(r2, 1) == 0, "GetWayPointPosition: index past the end and unknown path return 0");
            CHECK(r2.size() == 5 && ret(r2, 2) == 1 && bufStr(r2, 2).rfind("Waypoint0", 0) == 0, "GetNearestPath writes the name of the nearest path ('%s')", bufStr(r2, 2).c_str());
            CHECK(r2.size() == 5 && ret(r2, 3) == 0 && ret(r2, 4) == 0, "no nav points on the demo maps (count %lld)", ret(r2, 3));
        }
        // visual effects: the server only allocates ids
        {
            auto r = amx("v=AttachVEG " + A + " sFX_Test.veg;GetVEGID " + A + " sFX_Test.veg;GetVEGID " + A + " sother.veg;RemoveVEG " + A + " @v;GetVEGID " + A + " sFX_Test.veg;AttachVEG 0 sFX_Test.veg");
            CHECK(r.size() == 6 && ret(r, 0) >= 1 && ret(r, 1) == ret(r, 0) && ret(r, 2) == 0 && ret(r, 3) == 1 && ret(r, 4) == 0 && ret(r, 5) == 0,
                  "AttachVEG/GetVEGID/RemoveVEG track an effect id (%lld), unknown entity returns 0", ret(r, 0));
        }
        // HasDied reads the _DEAD_<instance name> counter; a dying NPC bumps it; IsDead of an unknown entity is 1
        {
            CritterId victim = 0;
            int farD = 0;
            for (auto& [id, cr] : a.critters)   // the most distant NPC in view
                if (cr.kind == CritterKind::Npc && id != a.myId && id != b.myId && TileDist(TilePos{cr.x, cr.y}, a.pos) > farD) { farD = TileDist(TilePos{cr.x, cr.y}, a.pos); victim = id; }
            if (victim) {
                const std::string V = std::to_string(victim);
                auto r = amx("HasDied n" + V + ";IsDead " + V + ";IsDead 0;A_Kill " + V);
                CHECK(r.size() == 4 && ret(r, 0) == 0 && ret(r, 1) == 0 && ret(r, 2) == 1 && ret(r, 3) == 1, "before its death: HasDied 0, IsDead 0 (unknown entity: IsDead 1)");
                WaitFor({&a, &b}, 0.4, [] { return false; });
                auto r2 = amx("HasDied n" + V + ";IsDead " + V + ";HasDied sNoSuchName;HasDied");
                CHECK(r2.size() == 4 && ret(r2, 0) == 1 && ret(r2, 1) == 1 && ret(r2, 2) == 0 && ret(r2, 3) == 0, "after A_Kill: HasDied(name) = %lld (the _DEAD_<name> counter), IsDead 1", ret(r2, 0));
            } else CHECK(false, "found an NPC to kill for the HasDied check");
        }
        // A_MoveToPosition takes a float[3] (world units) and A_Wander two rectangle vectors: use bot B as the walker
        {
            TilePos goal = b.pos;
            bool found = false;
            for (int r = 6; r >= 3 && !found; --r)
                for (int dx = -r; dx <= r && !found; ++dx)
                    for (int dy = -r; dy <= r && !found; ++dy)
                        if (std::max(std::abs(dx), std::abs(dy)) == r && Walkable(b, b.pos.x + dx, b.pos.y + dy) &&
                            SegmentWalkable(b, b.pos.x + 0.5f, b.pos.y + 0.5f, b.pos.x + dx + 0.5f, b.pos.y + dy + 0.5f)) { goal = {static_cast<int16_t>(b.pos.x + dx), static_cast<int16_t>(b.pos.y + dy)}; found = true; }
            CHECK(found, "found a walk goal for A_MoveToPosition (%d,%d)", goal.x, goal.y);
            if (found) {
                char prog[200];
                std::snprintf(prog, sizeof prog, "A_MoveToPosition %s p%s,%.3f,%.3f 5 0;IsIdle %s;A_MoveToPosition %s p%s,1,1", B.c_str(), B.c_str(),
                              (goal.x - b.pos.x) * 0.5, (goal.y - b.pos.y) * 0.5, B.c_str(), B.c_str(), B.c_str());
                auto r = amx(prog);
                CHECK(r.size() == 3 && ret(r, 0) == 1 && ret(r, 1) == 0 && ret(r, 2) == 0, "A_MoveToPosition(entity, vector, stop, 0) queues a move (IsIdle 0 afterwards); with 3 arguments it returns 0");
                CHECK(WaitFor({&a, &b}, 10, [&] { return b.pos == goal; }), "bot B walked to the vector's cell %d,%d (at %d,%d)", goal.x, goal.y, b.pos.x, b.pos.y);
                auto r3 = amx("IsIdle " + B);
                WaitFor({&a, &b}, 1.0, [] { return false; });
                r3 = amx("IsIdle " + B);
                CHECK(r3.size() == 1 && ret(r3, 0) == 1, "the move action finished (IsIdle %lld)", ret(r3, 0));
                // A_Wander(entity, upperLeft, lowerRight, 1): rectangle around B (upperLeft.z >= lowerRight.z like the original), one repetition
                auto w = amx("A_Wander " + B + " p" + B + ",-3,3 p" + B + ",3,-3 1;IsIdle " + B);
                CHECK(w.size() == 2 && ret(w, 0) == 1 && ret(w, 1) == 0, "A_Wander queues wait + move (IsIdle 0)");
                CHECK(WaitFor({&a, &b}, 14, [&] { auto q = amx("IsIdle " + B); return q.size() == 1 && ret(q, 0) == 1; }), "the wander repetition finishes (wait 1..6 s, then one move)");
                CHECK(TileDist(b.pos, goal) <= 7, "bot B stayed inside the wander rectangle (at %d,%d, started %d,%d)", b.pos.x, b.pos.y, goal.x, goal.y);
            }
        }
        // A_Attack / A_JustAttack: a real attack through the server combat (weapon, AP, breaktime); one attack per action
        {
            say("/forcemiss on");
            WaitFor({&a, &b}, 2.0, [] { return false; });   // earlier actions' breaktime / AP
            size_t c0 = a.combat.size();
            auto ownShots = [&](size_t from) { size_t n = 0; for (size_t i = from; i < a.combat.size(); ++i) if (a.combat[i].attacker == a.myId && a.combat[i].target == b.myId) ++n; return n; };
            say("/amxattacks on");
            auto r = amx("A_Attack " + A + " " + B + ";IsIdle " + A + ";A_Attack " + A + ";A_Attack " + A + " 0");
            CHECK(r.size() == 4 && ret(r, 0) == 1 && ret(r, 1) == 0 && ret(r, 2) == 0 && ret(r, 3) == 0, "A_Attack(entity, target) queues an attack (IsIdle 0), wrong argument count / target 0 return 0");
            CHECK(WaitFor({&a, &b}, 10, [&] { return ownShots(c0) >= 1; }), "A_Attack makes bot A attack bot B through the server combat (%zu shot(s))", ownShots(c0));
            WaitFor({&a, &b}, 3.0, [] { return false; });
            CHECK(ownShots(c0) == 1, "the attack action ends after ONE attack (%zu shots)", ownShots(c0));
            auto idle = amx("IsIdle " + A);
            CHECK(idle.size() == 1 && ret(idle, 0) == 1, "bot A is idle again after the attack action");
            // JustAttack neither walks nor turns: bot A attacks only when B is already in range
            c0 = a.combat.size();
            auto j = amx("A_JustAttack " + A + " " + B);
            WaitFor({&a, &b}, 5.0, [&] { return ownShots(c0) >= 1; });
            std::printf("note: A_JustAttack on a target %d cells away fired %zu shot(s)\n", TileDist(a.pos, b.pos), ownShots(c0));
            CHECK(j.size() == 1 && ret(j, 0) == 1, "A_JustAttack(entity, target) is accepted");
            say("/amxattacks off");
            say("/forcemiss off");
            WaitFor({&a, &b}, 1.0, [] { return false; });
        }
    }

    // --- Lua port of the tutorial generator (scripts/original/tutorial.lua): use -> 00_Generator_Status = 1
    {
        auto say = [&](const char* t) { net::ChatReq c; c.text = t; a.Send(net::Pack(c)); };
        auto sawChat = [&](const char* needle) { for (auto& l : a.chat) if (l.find(needle) != std::string::npos) return true; return false; };
        say("/glob 00_Generator_Status");
        CHECK(WaitFor({&a, &b}, 3, [&] { return sawChat("00_Generator_Status = 0"); }), "generator status starts at 0");
        say("/usescript 00_TutorialGenerator01");
        CHECK(WaitFor({&a, &b}, 3, [&] { return sawChat("Repairing"); }), "using the generator repairs it (Lua port)");
        say("/glob 00_Generator_Status");
        CHECK(WaitFor({&a, &b}, 3, [&] { return sawChat("00_Generator_Status = 1"); }), "generator status is 1 after repair");
        CHECK(sawChat("Task completed"), "task 3231 completed");
    }

    // --- an already dead critter re-entering the view (CritterState.appear = no death animation again) and the corpse loot flow
    {
        auto say = [&](const std::string& t) { net::ChatReq c; c.text = t; a.Send(net::Pack(c)); };
        auto idle = [&](double sec) { WaitFor({&a, &b}, sec, [] { return false; }); };
        // a walkable cell farc (> minD) from p
        auto farCell = [&](TilePos p, int minD) {
            for (int y = 0; y < a.map.height; y += 5)
                for (int x = 0; x < a.map.width; x += 5)
                    if (Walkable(a, x, y) && TileDist(TilePos{(int16_t)x, (int16_t)y}, p) > minD) return TilePos{(int16_t)x, (int16_t)y};
            return TilePos{-1, -1};
        };
        // a walkable cell with a straight walkable line to p, between d0 and d1 cells away
        auto nearCell = [&](TilePos p, int d0, int d1) {
            for (int d = d0; d <= d1; ++d)
                for (int dy = -d; dy <= d; ++dy)
                    for (int dx = -d; dx <= d; ++dx) {
                        if (std::max(std::abs(dx), std::abs(dy)) != d) continue;
                        int x = p.x + dx, y = p.y + dy;
                        if (Walkable(a, x, y) && SegmentWalkable(a, x + 0.5f, y + 0.5f, p.x + 0.5f, p.y + 0.5f)) return TilePos{(int16_t)x, (int16_t)y};
                    }
            return TilePos{-1, -1};
        };
        auto tp = [&](TilePos t) { say("/tp " + std::to_string(t.x) + " " + std::to_string(t.y)); };
        idle(1.0);
        // ---- bug 3 + loot flow: kill an NPC, leave, return
        say("/near Insurgent");
        idle(1.5);
        std::map<CritterId, bool> deadBefore;
        for (auto& [id, st] : a.states) if (st.dead) deadBefore[id] = true;
        say("/killnear Insurgent");
        CritterId corpse = 0;
        WaitFor({&a, &b}, 5, [&] {
            for (auto& [id, st] : a.states)
                if (st.dead && !deadBefore.count(id) && id != a.myId && id != b.myId) { corpse = id; return true; }
            return false;
        });
        if (!corpse) { for (size_t i = a.chat.size() > 4 ? a.chat.size() - 4 : 0; i < a.chat.size(); ++i) std::printf("note: chat: %s\n", a.chat[i].c_str()); std::printf("note: a at %d,%d, %zu critters in view, %zu dead states\n", a.pos.x, a.pos.y, a.critters.size(), deadBefore.size()); }
        CHECK(corpse != 0, "an NPC was killed next to bot A (dev command)");
        if (corpse && a.critters.count(corpse)) {
            CHECK(a.states[corpse].appear == 0, "a live kill sends the dead state without 'appear' (the death animation plays)");
            TilePos cp{a.critters[corpse].x, a.critters[corpse].y};
            int groundBefore = (int)a.items.size();
            TilePos farc = farCell(cp, 60), backc = nearCell(cp, 5, 8);
            if (farc.x >= 0 && backc.x >= 0) {
                tp(farc);
                CHECK(WaitFor({&a, &b}, 5, [&] { return !a.critters.count(corpse); }), "the corpse left bot A's view");
                a.states.erase(corpse);
                tp(backc);
                CHECK(WaitFor({&a, &b}, 5, [&] { return a.critters.count(corpse) && a.states.count(corpse); }) && a.states[corpse].dead && a.states[corpse].appear == 1,
                      "the corpse re-enters the view already dead and marked 'appear' (no death animation again)");
            } else {
                CHECK(false, "no farc / return cell for the corpse test");
            }
            (void)groundBefore;
            // ---- loot window protocol
            idle(1.0);
            size_t nc = a.cons.size();
            gnet::Loot lo; lo.id = corpse;
            a.Send(gnet::PackG(lo));
            CHECK(WaitFor({&a, &b}, 15, [&] { return a.cons.size() > nc; }) && a.cons.back().owner == corpse && a.cons.back().isCritter == 1,
                  "Loot on the corpse opens its container (isCritter, name '%s')", a.cons.empty() ? "" : a.cons.back().name.c_str());
            CHECK(a.cons.size() > nc && a.cons.back().name.find(".CRT") == std::string::npos && a.cons.back().name.find("Critters0") == std::string::npos && !a.cons.back().name.empty(),
                  "the loot title is a readable name, not the internal file name ('%s')", a.cons.size() > nc ? a.cons.back().name.c_str() : "");
            if (a.cons.size() > nc) {
                size_t had = a.cons.back().items.size();
                CHECK(had > 0, "the corpse keeps its inventory (%zu entries) instead of dropping it on the ground", had);
                bool onGround = false;   // nothing was dropped at the corpse's feet
                for (auto& [id, it] : a.items) if (TileDist(TilePos{it.x, it.y}, cp) <= 1 && !a.objInfo.count(id)) onGround = true;
                CHECK(!onGround, "no loose items lie at the corpse");
                if (had > 0) {
                    idle(1.0);
                    gnet::InvEntry e = a.cons.back().items[0];
                    size_t invBefore = a.inv.size();
                    int upd = a.invUpdates;
                    size_t nc2 = a.cons.size();
                    gnet::Take tk; tk.container = corpse; tk.entry = e.id; tk.isCritter = 1;
                    a.Send(gnet::PackG(tk));
                    bool ok = WaitFor({&a, &b}, 8, [&] { return a.invUpdates > upd && a.cons.size() > nc2; });
                    CHECK(ok && a.cons.back().items.size() == had - 1, "Take moves one entry (%s) from the corpse (%zu -> %zu entries)", e.name.c_str(), had, a.cons.back().items.size());
                    CHECK(a.inv.size() >= invBefore, "the taken item is in bot A's inventory");
                    // Put onto the corpse is refused (the dead are not storage); the item stays with the player
                    idle(1.0);
                    uint32_t mine = 0;
                    for (auto& x : a.inv) if (x.pid == e.pid && !x.equipped) mine = x.id;
                    if (mine) {
                        size_t nc3 = a.cons.size();
                        size_t invN = a.inv.size();
                        gnet::Put pt; pt.container = corpse; pt.entry = mine; pt.isCritter = 1;
                        a.Send(gnet::PackG(pt));
                        idle(2.0);
                        bool told = false;
                        for (auto& t : a.chat) if (t.find("cannot put items into the dead") != std::string::npos) told = true;
                        CHECK(a.cons.size() == nc3 && a.inv.size() == invN && told, "Put onto a corpse is rejected server-side (container unchanged, told)");
                    } else {
                        CHECK(false, "the taken item (pid %u) was not found in the inventory", e.pid);
                    }
                    // Take All
                    idle(1.0);
                    size_t nc4 = a.cons.size();
                    gnet::Take all; all.container = corpse; all.entry = 0; all.isCritter = 1;
                    a.Send(gnet::PackG(all));
                    WaitFor({&a, &b}, 8, [&] { return a.cons.size() > nc4; });
                    bool heavy = false;
                    for (auto& t : a.chat) if (t.find("cannot carry") != std::string::npos) heavy = true;
                    CHECK(a.cons.size() > nc4 && (a.cons.back().items.empty() || heavy), "Take All empties the corpse (%zu left)", a.cons.back().items.size());
                }
            }
            // a live NPC cannot be looted (no container comes backc)
            idle(1.0);
            CritterId live = 0;
            for (auto& [id, cr] : a.critters)
                if (id != a.myId && id != b.myId && cr.kind == CritterKind::Npc && TileDist(TilePos{cr.x, cr.y}, a.pos) <= 3 && !(a.states.count(id) && a.states[id].dead)) { live = id; break; }
            if (live) {
                size_t nc5 = a.cons.size();
                gnet::Loot l2; l2.id = live;
                a.Send(gnet::PackG(l2));
                idle(2.0);
                CHECK(a.cons.size() == nc5, "Loot on a living NPC opens nothing");
            }
        }
    }

    // --- map transition: go to the Vault side of Junktown's transition region
    {
        WaitFor({&a, &b}, 0.2, [] { return false; });
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
        // grid alignment: the mapdata grid starts at the tre root bbox min (44, 4) in the Vault. Door01 at world (78.89, 39.97) stands in a
        // doorway (walkable, unlocked) between two wall cells; Door02 at (141, 45) is initially locked and blocks its doorway.
        CHECK(Walkable(a, 69, 71) && !Walkable(a, 60, 71) && !Walkable(a, 82, 71), "Vault grid origin: Door01 doorway (69,71) walkable between wall cells");
        {
            bool lockedBlocked = false;
            for (int dy = -2; dy <= 2; ++dy) for (int dx = -2; dx <= 2; ++dx) lockedBlocked = lockedBlocked || !Walkable(a, 194 + dx, 82 + dy);
            CHECK(lockedBlocked, "locked Door02 keeps its doorway blocked at cell 194,82");
        }
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
            // unlocked doors also open by themselves when the player comes close, so a Use may close an already open door: use again
            opened = WaitFor({&a, &b}, 12, [&] { return a.objInfo[door].open == 1 || a.chat.size() > chats; }) && a.objInfo[door].open == 1;
            if (!opened && a.chat.size() == chats) {
                a.Send(gnet::PackG(u));
                opened = WaitFor({&a, &b}, 8, [&] { return a.objInfo[door].open == 1 || a.chat.size() > chats; }) && a.objInfo[door].open == 1;
            }
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
