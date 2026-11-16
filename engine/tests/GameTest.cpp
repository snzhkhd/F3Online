// f3_gametest: end-to-end test of the data-driven server (run it against `f3_server --data gamedata/server`).
//   f3_gametest [--port N] [--expect-map NAME]
//   f3_gametest --locale        localization: per-player language of server messages, UTF-8 names / chat (server --data --dev-kit)
//   f3_gametest --content DIR   data layer: client sync, perk flow, /reload (DIR = content folder of the server, edited by the test)
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
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <map>
#include <string>
#include <thread>
#include <vector>
#include "common/protocol.h"
#include "common/protocol_game.h"

using namespace f3;
using Clock = std::chrono::steady_clock;

static int g_failed = 0;
static std::function<void()> g_failHook;   // prints the state of the bots (position, hp, who is around, recent combat / chat): set once the bots exist
#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            std::printf("FAIL: " __VA_ARGS__);            \
            std::printf("  [%s:%d]\n", __FILE__, __LINE__); \
            ++g_failed;                                   \
            if (g_failHook) g_failHook();                 \
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
    std::string failReason;           // text of the last LoginFail
    int authCode = -1, authNew = 0;   // account block
    gnet::CharSheet sheet;
    int sheetCount = 0;
    gnet::ContentDefs content;   // designer data sync (traits, perks, item overrides); items accumulate over the chunks of one sync
    int contentCount = 0;        // S2C_ContentDefs messages received
    int contentSyncs = 0;        // complete resyncs (first = 1)
    gnet::PipData pip;   // latest Pip-Boy snapshot (quests, friends, mail)
    int pipCount = 0;
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
    void Login(const std::string& n, const std::string& lang = std::string()) {
        name = n;
        net::LoginReq r;
        r.name = n;
        r.lang = lang;   // language of the server messages ("" = server default)
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
            case (uint8_t)Msg::S2C_LoginFail: { net::LoginFail m; m.Read(b); failed = true; failReason = m.reason; std::printf("login failed: %s\n", m.reason.c_str()); break; }
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
            case (uint8_t)GMsg::S2C_CellsSet: {   // door opened / closed: the bot keeps its grid in sync like the client does
                gnet::CellsSet m;
                if (m.Read(b))
                    for (auto& c : m.cells)
                        if (c.x >= 0 && c.y >= 0 && c.x < map.width && c.y < map.height) map.blocked[static_cast<size_t>(c.y) * map.width + c.x] = c.blocked;
                break;
            }
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
            case (uint8_t)GMsgPip::S2C_Pip: { if (pip.Read(b)) ++pipCount; break; }
            case (uint8_t)GMsgContent::S2C_ContentDefs: {
                gnet::ContentDefs m;
                if (!m.Read(b)) break;
                ++contentCount;
                if (m.first) { content = m; ++contentSyncs; content.items = m.items; }
                else content.items.insert(content.items.end(), m.items.begin(), m.items.end());
                break;
            }
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
static void PipOp(Bot& b, gnet::PipOp op, const std::string& a = std::string(), const std::string& t = std::string(), uint32_t num = 0) {
    gnet::PipReq r; r.op = static_cast<uint8_t>(op); r.a = a; r.b = t; r.num = num;
    b.Send(gnet::PackG(r));
}
static const gnet::PipData::Quest* FindQuest(const Bot& b, const char* id) { for (auto& q : b.pip.quests) if (q.id == id) return &q; return nullptr; }
static const gnet::PipData::Friend* FindFriend(const Bot& b, const char* n) { for (auto& f : b.pip.friends) if (f.name == n) return &f; return nullptr; }

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
        CHECK(a.contentCount > 0 && a.content.traits.size() == 16 && a.content.traits[3].name == "Fast Shot", "content sync (trait list) arrives while the session is in character creation");
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
            {   // /reload is a dev command: refused on a server without --dev-kit / --dev-commands; /perks stays available
                a.chat.clear();
                net::ChatReq rl; rl.text = "/reload";
                a.Send(net::Pack(rl));
                auto hasChat = [&](const char* n) { for (auto& t : a.chat) if (t.find(n) != std::string::npos) return true; return false; };
                CHECK(WaitFor({&a}, 3, [&] { return hasChat("Dev commands are disabled"); }), "/reload is refused without dev commands");
                a.chat.clear();
                rl.text = "/perks";
                a.Send(net::Pack(rl));
                CHECK(WaitFor({&a}, 3, [&] { return hasChat("Perks:"); }), "/perks works for everyone");
            }
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
                // nearby NPCs may speak (their scripts' floating speech reaches the chat), so only skill related replies count
                CHECK(!said(a, "passive") && !said(a, "cannot use") && !said(a, "Invalid") && !said(a, "not hurt") && !said(a, "too far"),
                      "UseSkill with an invalid skill index is ignored");
                a.chat.clear();
                us.skill = 0; us.kind = 0;                                           // Firearms is a passive skill: refused
                a.Send(gnet::PackG(us));
                CHECK(WaitFor({&a}, 3, [&] { return said(a, "passive"); }), "UseSkill with a passive skill (Firearms) is refused");
            }
        }
        {   // Pip-Boy: quests (demo quests of main.lua), friends, mail
            // every request costs one point of the per-client budget (10, +1 per 5 ticks = 4/s, silently dropped when empty): pause after a burst
            auto pace = [&](Bot& b) { WaitFor({&b}, 1.5, [] { return false; }); };
            auto said2 = [&](Bot& b, const char* needle) { for (auto& t : b.chat) if (t.find(needle) != std::string::npos) return true; return false; };
            PipOp(a, gnet::PipOp::Request);
            CHECK(WaitFor({&a}, 3, [&] { return FindQuest(a, "first_steps") && FindQuest(a, "survivor"); }) && FindQuest(a, "first_steps")->state == 0, "Pip-Boy: the demo quests are in the log (active)");
            PipOp(a, gnet::PipOp::FriendAdd, "ObsBot");
            CHECK(WaitFor({&a, &obs}, 3, [&] { return FindFriend(a, "ObsBot") != nullptr; }) && FindFriend(a, "ObsBot")->online == 1, "Pip-Boy: friend added, shown online");
            a.chat.clear();
            PipOp(a, gnet::PipOp::FriendAdd, "NoSuchPlayerX");
            PipOp(a, gnet::PipOp::FriendAdd, "acctbot");
            PipOp(a, gnet::PipOp::FriendAdd, "obsbot");
            CHECK(WaitFor({&a}, 3, [&] { return said2(a, "no such player") && said2(a, "yourself") && said2(a, "already"); }) && a.pip.friends.size() == 1, "Pip-Boy: unknown, own and duplicate friends refused");
            PipOp(a, gnet::PipOp::MailSend, "ObsBot", "hello from the pip-boy");
            CHECK(WaitFor({&a, &obs}, 3, [&] { return !obs.pip.mail.empty(); }) && obs.pip.mail[0].from == kName && obs.pip.mail[0].text == "hello from the pip-boy" && obs.pip.mail[0].read == 0,
                  "Pip-Boy: mail delivered to an online account (unread)");
            CHECK(said2(obs, "new mail"), "Pip-Boy: the recipient is told about the new mail");
            PipOp(obs, gnet::PipOp::MailRead, "", "", obs.pip.mail.empty() ? 0 : obs.pip.mail[0].id);
            CHECK(WaitFor({&a, &obs}, 3, [&] { return !obs.pip.mail.empty() && obs.pip.mail[0].read == 1; }), "Pip-Boy: reading marks the mail read");
            pace(a);
            a.chat.clear();
            PipOp(a, gnet::PipOp::MailSend, "NoSuchPlayerX", "x");
            PipOp(a, gnet::PipOp::MailSend, "ObsBot", "   ");
            CHECK(WaitFor({&a}, 3, [&] { return said2(a, "no such player") && said2(a, "empty"); }), "Pip-Boy: mail to an unknown player / an empty mail refused");
            net::ChatReq mc; mc.text = "/mail ObsBot second one via chat";   // chat command form
            a.Send(net::Pack(mc));
            CHECK(WaitFor({&a, &obs}, 3, [&] { return obs.pip.mail.size() == 2; }) && obs.pip.mail[1].text == "second one via chat", "Pip-Boy: /mail chat command");
            PipOp(obs, gnet::PipOp::MailDelete, "", "", obs.pip.mail[0].id);
            CHECK(WaitFor({&obs}, 3, [&] { return obs.pip.mail.size() == 1; }), "Pip-Boy: mail deleted");
            pace(a);   // budget refill, see above
            PipOp(a, gnet::PipOp::MailSend, kName, "persist me across the restart");   // to itself: checked after the server restart
            CHECK(WaitFor({&a}, 3, [&] { return !a.pip.mail.empty(); }), "Pip-Boy: mail to oneself arrives");
            size_t pc = a.pipCount;
            enet_peer_disconnect(obs.peer, 0);
            CHECK(WaitFor({&a, &obs}, 4, [&] { return a.pipCount > pc && FindFriend(a, "ObsBot") && FindFriend(a, "ObsBot")->online == 0; }), "Pip-Boy: a friend going offline updates the list");
        }
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
        CHECK(WaitFor({&a}, 3, [&] { return FindQuest(a, "first_steps") && FindQuest(a, "first_steps")->state == 1; }), "Pip-Boy: the pickup completed the quest 'First steps' (on_pickup hook, c:quest_done)");
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
    {   // Pip-Boy data restored: quests and friends from the character, mail from the account's mailbox file
        CHECK(WaitFor({&a}, 3, [&] { return a.pipCount > 0; }) && FindQuest(a, "first_steps") && FindQuest(a, "first_steps")->state == 1 && FindQuest(a, "survivor") && FindQuest(a, "survivor")->state == 0,
              "Pip-Boy: quest log restored (First steps done, Survivor active)");
        CHECK(FindFriend(a, "ObsBot") != nullptr && a.pip.friends.size() == 1, "Pip-Boy: friends list restored");
        bool mailOk = false;
        for (auto& m : a.pip.mail) if (m.text == "persist me across the restart" && m.from == kName && m.read == 0) mailOk = true;
        CHECK(mailOk, "Pip-Boy: mail survived the server restart (unread)");
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
// Localization suite: f3_gametest --locale against `f3_server --data ... --dev-kit` (no accounts). Players announce their language at login; the
// server localizes its messages per player (key + args, fallback lang -> en -> key); UTF-8 names / chat are validated in code points.
static bool HasNonAscii(const std::string& t) { for (unsigned char c : t) if (c >= 0x80) return true; return false; }
static bool AnyChat(const Bot& b, size_t from, const std::string& needle) {
    for (size_t i = from; i < b.chat.size(); ++i) if (b.chat[i].find(needle) != std::string::npos) return true;
    return false;
}
static size_t Codepoints(const std::string& t) { size_t n = 0; for (unsigned char c : t) if ((c & 0xC0) != 0x80) ++n; return n; }
static int RunLocaleTests(uint16_t port) {
    Bot r, e, d;
    if (!r.Connect(port) || !e.Connect(port) || !d.Connect(port)) { std::printf("FAIL: cannot connect to 127.0.0.1:%u\n", port); return 1; }
    const std::string ruName = u8"Сталкер";   // Stalker, in Cyrillic
    r.Login(ruName, "ru");
    e.Login("LocaleBotE", "en");
    d.Login("LocaleBotD");   // an old client that does not announce a language: the server default (en)
    CHECK(WaitFor({&r, &e, &d}, 8, [&] { return r.loggedIn && e.loggedIn && d.loggedIn && !r.chat.empty() && !e.chat.empty() && !d.chat.empty(); }), "three players logged in (ru, en, no language)");
    if (!r.loggedIn || !e.loggedIn || !d.loggedIn) return 1;
    auto say = [&](Bot& b, const std::string& t) { net::ChatReq c; c.text = t; b.Send(net::Pack(c)); };
    WaitFor({&r, &e, &d}, 0.5, [] { return false; });
    // the welcome line comes from a Lua script by key: Russian for the ru player, English for the others
    {
        bool ruWelcome = false, enWelcome = false, dWelcome = false, ruHasEnglish = false;
        for (auto& t : r.chat) { if (t.find("F3Online") != std::string::npos && HasNonAscii(t)) ruWelcome = true; if (t.find("Welcome to F3Online") != std::string::npos) ruHasEnglish = true; }
        for (auto& t : e.chat) if (t.find("Welcome to F3Online") != std::string::npos) enWelcome = true;
        for (auto& t : d.chat) if (t.find("Welcome to F3Online") != std::string::npos) dWelcome = true;
        CHECK(ruWelcome && !ruHasEnglish, "ru player gets the welcome message in Russian");
        CHECK(enWelcome, "en player gets the welcome message in English");
        CHECK(dWelcome, "player without a language gets the server default (English)");
    }
    // a server command message: same key, two languages
    {
        const size_t rn = r.chat.size(), en = e.chat.size();
        say(r, "/traits"); say(e, "/traits");
        CHECK(WaitFor({&r, &e}, 3, [&] { return r.chat.size() > rn && e.chat.size() > en; }), "/traits answered for both");
        std::string rt, et;
        for (size_t i = rn; i < r.chat.size(); ++i) rt += r.chat[i] + "\n";
        for (size_t i = en; i < e.chat.size(); ++i) et += e.chat[i] + "\n";
        CHECK(HasNonAscii(rt) && rt.find("Traits") == std::string::npos, "ru /traits is Russian: %s", rt.substr(0, 60).c_str());
        CHECK(et.find("Traits") != std::string::npos && !HasNonAscii(et), "en /traits is English: %s", et.substr(0, 60).c_str());
    }
    // designer data names follow the player language (content sync): trait.<id>.name keys
    if (!r.content.traits.empty() && !e.content.traits.empty()) {
        const gnet::ContentDefs::Trait& tr = r.content.traits[0];
        const gnet::ContentDefs::Trait* te = nullptr;
        for (auto& t : e.content.traits) if (t.id == tr.id) te = &t;
        CHECK(te && HasNonAscii(tr.name) && !HasNonAscii(te->name), "trait '%s' is named per language (ru '%s', en '%s')", tr.id.c_str(), tr.name.c_str(), te ? te->name.c_str() : "?");
    } else {
        std::printf("note: no trait definitions received, per-language trait names not checked\n");
    }
    // runtime switch: /lang
    {
        size_t rn = r.chat.size();
        say(r, "/lang");
        CHECK(WaitFor({&r}, 3, [&] { return r.chat.size() > rn; }) && AnyChat(r, rn, "ru"), "/lang shows the current language");
        rn = r.chat.size();
        say(r, "/lang en");
        CHECK(WaitFor({&r}, 3, [&] { return r.chat.size() > rn; }) && AnyChat(r, rn, "Language set to en"), "/lang en switches the ru player to English");
        rn = r.chat.size();
        say(r, "/lang xx");
        CHECK(WaitFor({&r}, 3, [&] { return r.chat.size() > rn; }) && AnyChat(r, rn, "Unknown language"), "unknown language is refused (in the current language)");
        rn = r.chat.size();
        say(r, "/lang ru");
        CHECK(WaitFor({&r}, 3, [&] { return r.chat.size() > rn; }) && HasNonAscii(r.chat.back()), "/lang ru switches back");
    }
    // UTF-8 chat: Cyrillic text arrives intact, control characters are dropped, the limit counts code points
    {
        const std::string text = u8"Привет, мир!";
        const size_t en = e.chat.size();
        say(r, text);
        CHECK(WaitFor({&e}, 3, [&] { return e.chat.size() > en; }) && e.chat.back() == text && e.chatFrom.back() == ruName, "Cyrillic chat line arrives intact from a Cyrillic name");
        const std::string test = u8"Тест";
        const size_t en2 = e.chat.size();
        say(r, test + "\x01\x02 ok");
        CHECK(WaitFor({&e}, 3, [&] { return e.chat.size() > en2; }) && e.chat.back() == test + " ok", "control characters are stripped from chat: '%s'", e.chat.back().c_str());
        std::string lng;
        for (int i = 0; i < 300; ++i) lng += u8"ж";
        const size_t en3 = e.chat.size();
        say(r, lng);
        const bool got = WaitFor({&e}, 3, [&] { return e.chat.size() > en3; });
        const size_t cps = got ? Codepoints(e.chat.back()) : 0;
        CHECK(got && cps == 200, "chat limit is counted in code points (%zu, expected 200)", cps);
    }
    // names: validated in code points, letters / digits / _ only, no control characters, no broken UTF-8
    {
        auto tryName = [&](const std::string& n, const std::string& lang) {
            Bot x;
            if (!x.Connect(port)) return std::string("noconnect");
            x.Login(n, lang);
            WaitFor({&x}, 3, [&] { return x.loggedIn || x.failed; });
            return x.loggedIn ? std::string("ok") : x.failed ? x.failReason : std::string("timeout");
        };
        std::string sixteen;
        for (int i = 0; i < 16; ++i) sixteen += u8"б";
        CHECK(tryName(sixteen, "ru") == "ok", "16 Cyrillic letters (32 bytes) are a valid name");
        const std::string longFail = tryName(sixteen + u8"б", "ru");
        CHECK(longFail != "ok" && HasNonAscii(longFail), "17 code points refused, reason in Russian: %s", longFail.c_str());
        const std::string badEn = tryName("Bad\x01Name", "en");
        CHECK(badEn != "ok" && !HasNonAscii(badEn) && badEn.find("invalid") != std::string::npos, "control character in a name refused, reason in English: %s", badEn.c_str());
        CHECK(tryName("bad\xD0", "en") != "ok", "truncated UTF-8 name refused");
        CHECK(tryName("a b", "en") != "ok", "space in a name refused");
    }
    std::printf(g_failed ? "LOCALE GAMETEST FAILED\n" : "LOCALE GAMETEST PASSED\n");
    return g_failed ? 1 : 0;
}

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
        int bestD = 1 << 20;   // Armstrong's display name is "Armstrong" but other NPCs share the spot: take the NPC closest to his post
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


// ---------------------------------------------------------------------------------------------------------------------
// Door / container suite: f3_gametest --doors 1|2 against `f3_server --data ... --dev-kit` (saves dir shared by both phases, server RESTARTED
// in between). Phase 1: states closed / open / locked, pathing through closed doors, locked doors block pathing, flags (no-open, no-pick,
// jammed), key in the inventory, Security lockpick success / failure, containers (loot only while open), then /saveworld.
// Phase 2: the saved state is back after the restart.
// ---------------------------------------------------------------------------------------------------------------------
static int RunDoorTests(uint16_t port, int phase, const std::string& stateFile) {
    int contentsLeft = -1;
    Bot a;
    if (!a.Connect(port)) { std::printf("FAIL: cannot connect\n"); return 1; }
    a.Login("DoorBot");
    CHECK(WaitFor({&a}, 8, [&] { return a.loggedIn && a.havePos && a.haveStats; }), "logged in");
    if (!a.loggedIn) return 1;
    auto say = [&](const std::string& t) { net::ChatReq c; c.text = t; a.Send(net::Pack(c)); };
    auto idle = [&](double sec) { WaitFor({&a}, sec, [] { return false; }); };
    auto said = [&](size_t from, const char* needle) { for (size_t i = from; i < a.chat.size(); ++i) if (a.chat[i].find(needle) != std::string::npos) return true; return false; };
    auto objAt = [&](int x, int y) -> ItemId {
        for (auto& [id, it] : a.items)
            if (it.x == x && it.y == y && a.objInfo.count(id) && a.objInfo[id].kind != gnet::ObjectKind::Ground) return id;
        return 0;
    };
    auto objWait = [&](int x, int y) -> ItemId { WaitFor({&a}, 4, [&] { return objAt(x, y) != 0; }); return objAt(x, y); };
    auto tp = [&](int x, int y) {
        say("/tp Tutorial_Vault " + std::to_string(x) + " " + std::to_string(y));
        WaitFor({&a}, 6, [&] { return a.pos == TilePos{(int16_t)x, (int16_t)y} && a.map.name == "00_04_Tutorial_Vault"; });
        idle(0.8);
    };
    auto tpNear = [&](int x, int y) {   // the first walkable cell around (x, y) that /tp accepts
        for (int r = 0; r <= 6; ++r)
            for (int dy = -r; dy <= r; ++dy)
                for (int dx = -r; dx <= r; ++dx) {
                    if (std::max(std::abs(dx), std::abs(dy)) != r || !Walkable(a, x + dx, y + dy)) continue;
                    say("/tp Tutorial_Vault " + std::to_string(x + dx) + " " + std::to_string(y + dy));
                    if (WaitFor({&a}, 1.5, [&] { return a.pos == TilePos{(int16_t)(x + dx), (int16_t)(y + dy)}; })) { idle(0.8); return; }
                }
    };
    auto use = [&](ItemId id) { gnet::Use u; u.id = id; a.Send(gnet::PackG(u)); };
    say("/amxattacks off");
    idle(0.3);
    const int doorAx = 152, doorAy = 219;    // Door01 between the corridors west (143,220) and east (162,220)
    const int doorBx = 111, doorBy = 200;    // second Door01, used for the persistence check
    tp(143, 220);
    ItemId A = objAt(doorAx, doorAy);
    CHECK(A != 0 && a.objInfo[A].kind == gnet::ObjectKind::Door, "Vault Door01 at %d,%d is in view (id %u)", doorAx, doorAy, A);
    if (!A) return 1;
    auto st = [&](ItemId id) -> gnet::ObjectInfo { return a.objInfo.count(id) ? a.objInfo[id] : gnet::ObjectInfo{}; };
    auto is = [&](ItemId id, int open, int locked, int flags) { gnet::ObjectInfo o = st(id); return o.open == open && o.locked == locked && o.flags == flags; };

    if (phase == 1) {
        CHECK(is(A, 0, 0, 0), "state Closed: open 0, locked 0, flags 0");
        CHECK(!Walkable(a, doorAx, doorAy), "a closed door blocks its doorway cell %d,%d in the map grid", doorAx, doorAy);

        // ---- the player does NOT open doors by walking into them (original GameActionMoveTo::OnMoved queues the operate action only for critters
        // outside the party): the path leads through the closed unlocked door, the player stops in front of it and uses it (PlayerAutoOpenDoors = 0)
        net::MoveToReq mv; mv.x = 162; mv.y = 220;
        a.Send(net::Pack(mv));
        WaitFor({&a}, 12, [&] { return a.pos.x >= 148; });
        idle(2.5);
        CHECK(a.pos.x >= 148 && a.pos.x < doorAx && st(A).open == 0, "the player stops in front of a closed unlocked door and does not open it by walking (bot at %d,%d, door open %d)", a.pos.x, a.pos.y, st(A).open);
        use(A);
        CHECK(WaitFor({&a}, 8, [&] { return st(A).open == 1; }), "Use opens the closed door");
        CHECK(Walkable(a, doorAx, doorAy), "the open door released its doorway cell (CellsSet)");
        a.Send(net::Pack(mv));
        CHECK(WaitFor({&a}, 12, [&] { return a.pos == TilePos{162, 220}; }), "the player walks through the opened door (bot at %d,%d)", a.pos.x, a.pos.y);
        idle(0.8);
        use(A);   // the bot stands east of it: Use closes it again
        CHECK(WaitFor({&a}, 8, [&] { return st(A).open == 0; }), "Use closes an open door");
        CHECK(!Walkable(a, doorAx, doorAy), "the closed door blocks again");

        // ---- NPCs open doors on their path (original: GameActionMoveTo inserts the operate action for non-party critters), native move_to and
        // AMX A_MoveToPosition alike; a locked door blocks them unless they carry the key
        {
            size_t n1 = a.chat.size();
            say("/npcspawn 145 220");
            CHECK(WaitFor({&a}, 4, [&] { return said(n1, "npc "); }), "test NPC spawned west of the door");
            unsigned npc = 0;
            for (size_t i = n1; i < a.chat.size(); ++i) if (a.chat[i].rfind("npc ", 0) == 0) npc = static_cast<unsigned>(std::atoi(a.chat[i].c_str() + 4));
            CHECK(npc != 0, "NPC id %u", npc);
            if (npc) {
                auto npcX = [&]() { return a.critters.count(npc) ? (int)a.critters[npc].x : -1; };
                auto npcGo = [&](int x, int y) { say("/npcgo " + std::to_string(npc) + " " + std::to_string(x) + " " + std::to_string(y)); };
                CHECK(WaitFor({&a}, 4, [&] { return npcX() >= 0; }), "the bot sees the NPC (at %d)", npcX());
                // a) AI / native movement through the closed unlocked door
                CHECK(st(A).open == 0 && !Walkable(a, doorAx, doorAy), "door closed before the NPC walks");
                npcGo(160, 220);
                CHECK(WaitFor({&a}, 14, [&] { return npcX() >= 158; }), "an NPC walks through a closed unlocked door (native move_to; NPC at %d)", npcX());
                CHECK(st(A).open == 1, "the NPC opened the door on its way (state Open)");
                idle(0.8);
                // b) locked, no key: the NPC cannot go back
                use(A);
                CHECK(WaitFor({&a}, 10, [&] { return st(A).open == 0; }), "door closed again behind the NPC");
                say("/lock " + std::to_string(A) + " locked=1 key=ItemsKeystest");
                CHECK(WaitFor({&a}, 4, [&] { return is(A, 0, 1, 0); }), "door locked (key ItemsKeystest)");
                idle(0.8);
                npcGo(145, 220);
                idle(5.0);
                CHECK(npcX() > doorAx && st(A).open == 0 && st(A).locked == 1, "a locked door blocks the NPC (NPC at %d, door closed and locked)", npcX());
                // c) with the key in its inventory the NPC unlocks and opens it
                say("/npckey " + std::to_string(npc) + " ItemsKeystest.ITM");
                idle(1.0);
                npcGo(145, 220);
                CHECK(WaitFor({&a}, 14, [&] { return npcX() > 0 && npcX() <= 148; }), "an NPC carrying the key walks through the locked door (NPC at %d)", npcX());
                CHECK(st(A).open == 1 && st(A).locked == 0, "the key unlocked and opened it (open %u, locked %u)", st(A).open, st(A).locked);
                idle(0.8);
                // d) AMX script movement (A_MoveToPosition queues the same StartMove) through a closed unlocked door
                use(A);
                CHECK(WaitFor({&a}, 10, [&] { return st(A).open == 0; }), "door closed again");
                say("/lock " + std::to_string(A) + " locked=0 key=");
                CHECK(WaitFor({&a}, 4, [&] { return is(A, 0, 0, 0); }), "door unlocked");
                idle(0.8);
                say("/amxnative owner=" + std::to_string(npc) + "; A_MoveToPosition " + std::to_string(npc) + " p" + std::to_string(a.myId) + " 0 0");
                CHECK(WaitFor({&a}, 16, [&] { return npcX() > doorAx + 2; }), "an AMX-driven NPC (A_MoveToPosition) walks through a closed unlocked door (NPC at %d)", npcX());
                CHECK(st(A).open == 1, "the AMX NPC opened the door");
                idle(0.8);
                // e) the NPC holds ItemsKeystest, the door now wants another key: closed to it
                use(A);
                WaitFor({&a}, 10, [&] { return st(A).open == 0; });
                say("/lock " + std::to_string(A) + " locked=1 key=NoSuchKey");
                CHECK(WaitFor({&a}, 4, [&] { return is(A, 0, 1, 0); }), "door locked with a key the NPC does not have");
                idle(0.8);
                npcGo(145, 220);
                idle(5.0);
                CHECK(npcX() > doorAx && st(A).open == 0, "a door locked with another key blocks the NPC (NPC at %d)", npcX());
                say("/lock " + std::to_string(A) + " locked=0 key=");
                CHECK(WaitFor({&a}, 4, [&] { return is(A, 0, 0, 0); }), "door unlocked again for the player tests");
                idle(0.8);
            }
        }

        // ---- locked door: Use refuses, pathing is blocked
        say("/lock " + std::to_string(A) + " locked=1");
        CHECK(WaitFor({&a}, 4, [&] { return is(A, 0, 1, 0); }), "state Locked announced (ObjectInfo locked 1)");
        idle(0.8);
        size_t n0 = a.chat.size();
        use(A);
        CHECK(WaitFor({&a}, 8, [&] { return said(n0, "locked"); }) && st(A).open == 0, "Use on a locked door says it is locked and keeps it closed");
        mv.x = 143; mv.y = 220;
        a.Send(net::Pack(mv));
        idle(4.0);
        CHECK(a.pos.x > 150 && st(A).open == 0, "a locked door blocks pathing: no way to the west side (bot at %d,%d, door still closed)", a.pos.x, a.pos.y);

        // ---- flags
        say("/lock " + std::to_string(A) + " locked=0 flags=1");
        CHECK(WaitFor({&a}, 4, [&] { return is(A, 0, 0, 1); }), "flag no-open announced");
        idle(0.8);
        n0 = a.chat.size();
        use(A);
        CHECK(WaitFor({&a}, 8, [&] { return said(n0, "cannot be opened"); }) && st(A).open == 0, "a no-open door refuses with 'cannot be opened'");
        say("/lock " + std::to_string(A) + " flags=4");
        CHECK(WaitFor({&a}, 4, [&] { return is(A, 0, 0, 4); }), "flag jammed announced");
        idle(0.8);
        n0 = a.chat.size();
        use(A);
        CHECK(WaitFor({&a}, 8, [&] { return said(n0, "jammed"); }) && st(A).open == 0, "a jammed door refuses with 'jammed'");

        // ---- keys: the key in the inventory unlocks and opens
        say("/lock " + std::to_string(A) + " locked=1 flags=0 key=ItemsKeystest");
        CHECK(WaitFor({&a}, 4, [&] { return is(A, 0, 1, 0); }), "door locked with key 'ItemsKeystest'");
        idle(0.8);
        n0 = a.chat.size();
        use(A);
        CHECK(WaitFor({&a}, 8, [&] { return said(n0, "locked"); }) && st(A).open == 0, "without the key the door stays locked");
        say("/give ItemsKeystest.ITM");
        CHECK(WaitFor({&a}, 4, [&] { return said(n0, "given"); }), "key given");
        idle(0.8);
        n0 = a.chat.size();
        use(A);
        CHECK(WaitFor({&a}, 8, [&] { return st(A).open == 1; }) && st(A).locked == 0 && said(n0, "opened the door with the"), "the key unlocks and opens the door (message from the original string 1358)");
        idle(0.8);
        use(A);   // close again
        WaitFor({&a}, 8, [&] { return st(A).open == 0; });

        // ---- lockpicking (Security skill through UseSkill)
        say("/lock " + std::to_string(A) + " locked=1 flags=0 key=");
        say("/lockroll 0");
        CHECK(WaitFor({&a}, 4, [&] { return is(A, 0, 1, 0); }), "door locked for the lockpick tests");
        idle(1.0);
        auto skill = [&](ItemId id) { gnet::UseSkill us; us.skill = 10; us.kind = 2; us.target = id; a.Send(gnet::PackG(us)); };   // 10 = Security
        n0 = a.chat.size();
        skill(A);
        CHECK(WaitFor({&a}, 6, [&] { return said(n0, "fail to pick"); }) && st(A).locked == 1, "Security: a failed pick leaves the door locked");
        idle(1.2);
        say("/lockroll 1");
        idle(1.0);
        say("/lock " + std::to_string(A) + " flags=2");
        WaitFor({&a}, 4, [&] { return is(A, 0, 1, 2); });
        idle(1.0);
        n0 = a.chat.size();
        skill(A);
        CHECK(WaitFor({&a}, 6, [&] { return said(n0, "cannot be picked"); }) && st(A).locked == 1, "Security: a no-pick lock cannot be picked even on a sure roll");
        say("/lock " + std::to_string(A) + " flags=0");
        WaitFor({&a}, 4, [&] { return is(A, 0, 1, 0); });
        idle(1.2);
        n0 = a.chat.size();
        skill(A);
        CHECK(WaitFor({&a}, 6, [&] { return st(A).locked == 0; }) && said(n0, "unlock"), "Security: a successful pick unlocks the door (ObjectInfo locked 0)");
        idle(1.2);
        n0 = a.chat.size();
        skill(A);
        CHECK(WaitFor({&a}, 6, [&] { return said(n0, "not locked"); }), "Security on an unlocked door: 'not locked'");
        say("/lockroll off");

        // ---- containers: loot only while open, lock, pick
        tpNear(182, 250);
        ItemId C = objWait(182, 246);
        CHECK(C != 0 && a.objInfo[C].kind == gnet::ObjectKind::Container, "vault storage shelf (container) in view (id %u)", C);
        if (C) {
            CHECK(is(C, 0, 0, 0), "container starts Closed");
            size_t nc = a.cons.size();
            gnet::Take tk; tk.container = C; tk.entry = 0;
            a.Send(gnet::PackG(tk));
            idle(1.5);
            CHECK(a.cons.size() == nc, "a closed container cannot be looted (Take refused)");
            say("/lock " + std::to_string(C) + " locked=1");
            WaitFor({&a}, 4, [&] { return is(C, 0, 1, 0); });
            idle(0.8);
            n0 = a.chat.size();
            use(C);
            CHECK(WaitFor({&a}, 8, [&] { return said(n0, "locked"); }) && a.cons.size() == nc && st(C).open == 0, "Use on a locked container: told it is locked, no window");
            say("/lockroll 1");
            idle(1.0);
            skill(C);
            CHECK(WaitFor({&a}, 6, [&] { return st(C).locked == 0; }), "Security picks the container lock");
            say("/lockroll off");
            idle(1.0);
            use(C);
            CHECK(WaitFor({&a}, 8, [&] { return st(C).open == 1 && a.cons.size() > nc; }) && a.cons.back().owner == C, "Use opens the unlocked container (state Open) and lists its contents");
            if (a.cons.size() > nc && !a.cons.back().items.empty()) {
                const size_t had = a.cons.back().items.size();
                idle(0.8);
                size_t nc2 = a.cons.size();
                gnet::Take t1; t1.container = C; t1.entry = a.cons.back().items[0].id;
                a.Send(gnet::PackG(t1));
                CHECK(WaitFor({&a}, 8, [&] { return a.cons.size() > nc2; }) && a.cons.back().items.size() == had - 1, "an open container can be looted (%zu -> %zu entries)", had, a.cons.back().items.size());
                contentsLeft = static_cast<int>(a.cons.back().items.size());
            }
            gnet::CloseObj co; co.id = C;
            a.Send(gnet::PackG(co));
            CHECK(WaitFor({&a}, 6, [&] { return st(C).open == 0; }), "closing the loot window closes the container (C2S_CloseObj)");
        }

        // ---- persistence: door B (set open) and door A's lock settings
        tp(104, 200);
        ItemId B = objWait(doorBx, doorBy);
        CHECK(B != 0, "second door at %d,%d in view (id %u)", doorBx, doorBy, B);
        if (B) {
            say("/lock " + std::to_string(B) + " open=1 dc=33 key=VaultKey");
            CHECK(WaitFor({&a}, 4, [&] { return st(B).open == 1; }), "door B set open");
        }
        say("/lock " + std::to_string(A) + " open=0 locked=1 flags=2 dc=21");
        idle(1.0);
        say("/saveworld");
        CHECK(WaitFor({&a}, 4, [&] { return said(0, "world state saved"); }), "world state saved");
        if (FILE* f = std::fopen(stateFile.c_str(), "w")) { std::fprintf(f, "%d\n", contentsLeft); std::fclose(f); }
    } else {
        // after the server restart: door A was left locked + no-pick, door B open, the container closed (one item lighter)
        CHECK(is(A, 0, 1, 2), "persisted: door A is still locked with the no-pick flag (open %u, locked %u, flags %u)", st(A).open, st(A).locked, st(A).flags);
        CHECK(!Walkable(a, doorAx, doorAy), "persisted: door A still blocks its doorway");
        tp(104, 200);
        ItemId B = objWait(doorBx, doorBy);
        CHECK(B != 0 && st(B).open == 1 && st(B).locked == 0, "persisted: door B is still open");
        CHECK(Walkable(a, doorBx, doorBy), "persisted: the open door B releases its doorway cell");
        tpNear(182, 250);
        ItemId C = objWait(182, 246);
        if (!C) for (auto& [id, it] : a.items) std::printf("note: item %u '%s' at %d,%d objInfo %d (bot at %d,%d)\n", id, it.name.c_str(), it.x, it.y, (int)a.objInfo.count(id), a.pos.x, a.pos.y);
        CHECK(C != 0 && is(C, 0, 0, 0), "persisted: the container is closed again (open %u, locked %u, flags %u)", st(C).open, st(C).locked, st(C).flags);
        if (C) {
            idle(0.8);
            size_t nc = a.cons.size();
            use(C);
            CHECK(WaitFor({&a}, 8, [&] { return a.cons.size() > nc; }), "the container opens again after the restart");
            int expect = -2;
            if (FILE* f = std::fopen(stateFile.c_str(), "r")) { if (std::fscanf(f, "%d", &expect) != 1) expect = -2; std::fclose(f); }
            const int left = a.cons.empty() ? -1 : static_cast<int>(a.cons.back().items.size());
            CHECK(expect >= 0 && left == expect, "persisted: the looted container keeps its remaining entries (%d, expected %d)", left, expect);
        }
    }
    for (auto& t : a.chat) std::printf("chat: %s\n", t.c_str());
    std::printf(g_failed ? "DOORTEST %d FAILED (%d)\n" : "DOORTEST %d PASSED\n", phase, g_failed);
    enet_deinitialize();
    return g_failed ? 1 : 0;
}


// Data layer end-to-end: client sync, perk flow, stat modifiers, /reload (applied, diff report, failure keeps the old data).
// `contentDir` is the content folder of the server under test (a throw-away copy of data/): the test edits it and sends /reload.
static int RunContentTests(uint16_t port, const std::string& contentDir) {
    namespace fs = std::filesystem;
    const fs::path dir = contentDir;
    auto readAll = [](const fs::path& p) { std::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); };
    auto writeAll = [](const fs::path& p, const std::string& s) { fs::create_directories(p.parent_path()); std::ofstream f(p, std::ios::binary); f << s; };
    auto said = [](Bot& b, const char* needle) { for (auto& t : b.chat) if (t.find(needle) != std::string::npos) return true; return false; };
    auto say = [](Bot& b, const char* text) { net::ChatReq r; r.text = text; b.Send(net::Pack(r)); };

    Bot a;
    if (!a.Connect(port)) { std::printf("FAIL: cannot connect\n"); return 1; }
    a.Login("DataBot");
    CHECK(WaitFor({&a}, 8, [&] { return a.loggedIn && a.mapInfoCount > 0 && a.haveStats && a.contentCount > 0 && a.sheetCount > 0; }), "login with content sync");
    if (!a.loggedIn) return 1;
    CHECK(a.content.traits.size() == 16 && a.content.traitMax == 2 && a.content.schema == 1, "ContentDefs: %zu traits, traitMax %u, schema %u (16 / 2 / 1)", a.content.traits.size(), a.content.traitMax, a.content.schema);
    {
        bool names = a.content.traits.size() == 16;
        for (auto& t : a.content.traits) if (t.index >= 16 || t.name.empty() || t.desc.empty()) names = false;
        CHECK(names && a.content.traits[0].name == "Bruiser" && a.content.traits[15].name == "Small Frame", "trait slots carry names and descriptions (0 = %s, 15 = %s)", a.content.traits.empty() ? "-" : a.content.traits[0].name.c_str(), a.content.traits.empty() ? "-" : a.content.traits[15].name.c_str());
        bool tough = false;
        for (auto& p : a.content.perks) if (p.id == "toughness" && p.reqLevel == 3 && !p.name.empty()) tough = true;
        CHECK(tough && a.content.perks.size() >= 9, "ContentDefs carries the perks (%zu)", a.content.perks.size());
    }
    CHECK(a.sheet.created && a.sheet.perkPoints == 0 && a.sheet.traitMax == 2, "CharSheet: created, no perk points, traitMax %u", a.sheet.traitMax);
    const int hp0 = a.stats.maxHp;

    // ---- chat commands and the perk flow
    a.chat.clear();
    say(a, "/perks");
    CHECK(WaitFor({&a}, 3, [&] { return said(a, "toughness") && said(a, "Perks:"); }), "/perks lists the perks from data/perks");
    a.chat.clear();
    say(a, "/traits");
    CHECK(WaitFor({&a}, 3, [&] { return said(a, "Bruiser") && said(a, "Small Frame"); }), "/traits lists the traits");
    a.chat.clear();
    say(a, "/perk take toughness");
    CHECK(WaitFor({&a}, 3, [&] { return said(a, "no perk points"); }), "taking a perk without perk points is refused");
    say(a, "/givexp 3000");
    CHECK(WaitFor({&a}, 4, [&] { return a.sheet.level == 3 && a.sheet.perkPoints == 1; }), "level 3 grants a perk point (level %d, points %u)", a.sheet.level, a.sheet.perkPoints);
    WaitFor({&a}, 0.5, [] { return false; });
    const int hp3 = a.stats.maxHp;   // level 3 hit points
    a.chat.clear();
    say(a, "/perk take no_such_perk");
    CHECK(WaitFor({&a}, 3, [&] { return said(a, "unknown perk"); }) && a.sheet.perkPoints == 1, "unknown perk refused, point kept");
    a.chat.clear();
    say(a, "/perk take thick_skin");
    CHECK(WaitFor({&a}, 3, [&] { return said(a, "requires"); }) && a.sheet.perkPoints == 1, "a perk whose requirements are not met is refused (thick_skin needs level 6 and Toughness)");
    a.chat.clear();
    say(a, "/perk take toughness");
    CHECK(WaitFor({&a}, 4, [&] { return said(a, "Perk gained: Toughness") && a.sheet.perkPoints == 0 && a.stats.maxHp == hp3 + 10; }), "Toughness taken: perk point spent, maxHp %d -> %d (+10 from the data effect)", hp3, a.stats.maxHp);
    CHECK(a.stats.hp > 0 && a.stats.hp <= a.stats.maxHp, "hp follows");
    a.chat.clear();
    say(a, "/perk take toughness");
    CHECK(WaitFor({&a}, 3, [&] { return said(a, "no perk points") || said(a, "maximum rank"); }), "no second rank of a single-rank perk");
    (void)hp0;

    // ---- /reload: edit a perk, add item + npc overrides
    const fs::path perksFile = dir / "perks" / "perks.json";
    const std::string perksOrig = readAll(perksFile);
    if (perksOrig.empty()) { std::printf("FAIL: cannot read %s\n", perksFile.string().c_str()); return 1; }
    std::string perksNew = perksOrig;
    {
        const std::string d0 = "You can take a beating: +10 maximum hit points.";
        size_t at = perksNew.find(d0);
        if (at != std::string::npos) perksNew.replace(at, d0.size(), "Reloaded description.");
        size_t m = perksNew.find("\"max_hp\"");
        size_t ad = m == std::string::npos ? m : perksNew.find("\"add\": 10", m);
        if (ad != std::string::npos) perksNew.replace(ad, 9, "\"add\": 25");
    }
    CHECK(perksNew != perksOrig, "test edit of perks.json prepared");
    writeAll(perksFile, perksNew);
    writeAll(dir / "items" / "zz_test.json", R"({"schema":1,"kind":"items","records":[
        {"id":"Weapons9mm_Autoloader.WEA","name":"Reloaded Autoloader","desc":"Edited by the test.","weight":2,"weapon":{"range":22,"dmg_min":9,"dmg_max":19}},
        {"id":"F3_TestPistol.WEA","base":"Weapons9mm_Autoloader.WEA","name":"Test Pistol","desc":"A new item."}]})");
    writeAll(dir / "npcs" / "zz_test.json", R"({"schema":1,"kind":"npcs","records":[{"id":"Critters00_TMBobby.CRT","name":"Bobby Reloaded","xp":777}]})");
    const int syncs0 = a.contentSyncs;
    const uint32_t rev0 = a.content.revision;
    a.chat.clear();
    say(a, "/reload data");
    CHECK(WaitFor({&a}, 6, [&] { return said(a, "data reloaded") && a.contentSyncs > syncs0; }), "/reload data: applied and the client is re-synced");
    CHECK(said(a, "perks: +0 -0 ~1") && said(a, "toughness") && said(a, "items: +2") && said(a, "npcs: +1"), "the report lists what changed (perk changed, items added, npc added)");
    WaitFor({&a}, 0.6, [] { return false; });
    CHECK(a.content.revision > rev0, "revision increased (%u -> %u)", rev0, a.content.revision);
    {
        bool desc = false;
        for (auto& p : a.content.perks) if (p.id == "toughness" && p.desc == "Reloaded description.") desc = true;
        CHECK(desc, "the client got the new perk description");
        const gnet::ContentDefs::Item *ov = nullptr, *nw = nullptr;
        for (auto& it : a.content.items) { if (it.name == "Reloaded Autoloader") ov = &it; if (it.key == "F3_TestPistol.WEA") nw = &it; }
        CHECK(ov && ov->basePid == 0xFFFF && ov->desc == "Edited by the test." && ov->range == 22 && ov->dmgMax == 19.0f, "item override synced (range %u, dmg %.0f)", ov ? ov->range : 0, ov ? ov->dmgMax : 0.f);
        CHECK(nw && ov && nw->basePid == ov->pid && nw->pid > ov->pid && nw->name == "Test Pistol", "new item synced: pid %u cloned from base pid %u", nw ? nw->pid : 0, nw ? nw->basePid : 0);
    }
    CHECK(a.stats.maxHp == hp3 + 25, "existing character picks up the changed effect: maxHp %d (expect %d)", a.stats.maxHp, hp3 + 25);
    {   // existing instances keep working: the inventory of the dev kit shows the overridden name, the session is alive
        int sc = a.invUpdates;
        a.Send(gnet::PackG(gnet::ReqState{}));
        CHECK(WaitFor({&a}, 3, [&] { return a.invUpdates > sc; }) && !a.failed, "session and inventory keep working after the reload");
        bool renamed = false;
        for (auto& e : a.inv) if (e.name == "Reloaded Autoloader") renamed = true;
        CHECK(renamed, "the existing 9mm autoloader now carries the reloaded name");
    }

    // ---- a broken edit: the reload fails as a whole, the previous data stays active
    const int count1 = a.contentCount;
    writeAll(dir / "items" / "zz_test.json", R"({"schema":1,"kind":"items","records":[{"id":"Nope_Not_There.WEA","name":"x"}]})");
    a.chat.clear();
    say(a, "/reload data");
    CHECK(WaitFor({&a}, 5, [&] { return said(a, "data reload FAILED") && said(a, "zz_test.json"); }), "a broken data file fails the reload, naming the file");
    WaitFor({&a}, 0.8, [] { return false; });
    CHECK(a.contentCount == count1, "no resync after a failed reload");
    a.chat.clear();
    say(a, "/perks");
    CHECK(WaitFor({&a}, 3, [&] { return said(a, "Reloaded description."); }), "the previous (good) data is still active");

    // ---- restore the folder, reload again
    writeAll(perksFile, perksOrig);
    fs::remove(dir / "items" / "zz_test.json");
    fs::remove(dir / "npcs" / "zz_test.json");
    a.chat.clear();
    say(a, "/reload");
    CHECK(WaitFor({&a}, 6, [&] { return said(a, "data reloaded") && said(a, "scripts reloaded"); }), "/reload (all): scripts and data");
    WaitFor({&a}, 0.8, [] { return false; });
    CHECK(said(a, "items: +0 -2") && said(a, "npcs: +0 -1") && a.stats.maxHp == hp3 + 10, "restored files: records removed again, maxHp %d back to %d", a.stats.maxHp, hp3 + 10);
    {
        bool gone = true;
        for (auto& it : a.content.items) if (it.name == "Reloaded Autoloader" || it.key == "F3_TestPistol.WEA") gone = false;
        CHECK(gone, "client item list is clean again");
    }
    a.chat.clear();
    say(a, "/reload bogus");
    CHECK(WaitFor({&a}, 3, [&] { return said(a, "usage: reload"); }), "/reload with an unknown argument prints the usage");
    std::printf(g_failed ? "CONTENT TESTS FAILED\n" : "CONTENT TESTS PASSED\n");
    return g_failed ? 1 : 0;
}

// ---- data dialogs (data/dialogs): conditions, effects, quest/flag state, stf answers, Lua hook, attack/end
static int RunDialogTests(uint16_t port) {
    Bot a;
    if (!a.Connect(port)) { std::printf("FAIL: cannot connect\n"); return 1; }
    a.Login("DlgBot");
    CHECK(WaitFor({&a}, 8, [&] { return a.loggedIn && a.mapInfoCount > 0 && a.haveStats; }), "login");
    if (!a.loggedIn) return 1;
    auto say = [&](const std::string& t) { net::ChatReq c; c.text = t; a.Send(net::Pack(c)); };
    auto pause = [&](double s) { WaitFor({&a}, s, [] { return false; }); };
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
    auto findNpc = [&](const char* name) -> CritterId { for (auto& [id, c] : a.critters) if (c.kind == CritterKind::Npc && c.name == name) return id; return 0; };
    auto hasReply = [](const gnet::DialogNode& n, const char* part) { for (auto& r : n.replies) if (r.text.find(part) != std::string::npos) return true; return false; };
    auto textHas = [&](const char* part) { return !a.dlgNodes.empty() && a.dlgNodes.back().text.find(part) != std::string::npos; };
    auto talk = [&](CritterId npc) {
        size_t n = a.dlgNodes.size();
        gnet::Talk t; t.critter = npc;
        a.Send(gnet::PackG(t));
        return WaitFor({&a}, 5, [&] { return a.dlgNodes.size() > n; });
    };
    auto choose = [&](int idx) {
        size_t n = a.dlgNodes.size(); int c0 = a.dlgCloses;
        gnet::DialogChoose c; c.index = (uint8_t)idx;
        a.Send(gnet::PackG(c));
        return WaitFor({&a}, 5, [&] { return a.dlgNodes.size() > n || a.dlgCloses > c0; });
    };
    auto closeDlg = [&] { int c0 = a.dlgCloses; gnet::DialogCloseReq cl; a.Send(gnet::PackG(cl)); WaitFor({&a}, 3, [&] { return a.dlgCloses > c0; }); };
    auto questState = [&]() -> int { PipOp(a, gnet::PipOp::Request); pause(0.6); auto* q = FindQuest(a, "scrapper_toolkit"); return q ? (int)q->state : -1; };
    auto invCount = [&](const char* part) { int n = 0; for (auto& e : a.inv) if (e.name.find(part) != std::string::npos) n += e.count; return n; };

    say("/amxattacks off");
    say("/near Joe");
    pause(1.0);
    const CritterId joe = findNpc("Scrapper Joe");
    CHECK(joe != 0, "Scrapper Joe (data NPC, dialog junktown_scrapper) is in view");
    if (!joe) return 1;

    // greeting: the skill-gated answer is hidden at a low Barter, the others are shown
    CHECK(talk(joe) && !a.dlgOpens.empty() && a.dlgOpens.back().name == "Scrapper Joe", "Talk opens the data dialog with the speaker name");
    const gnet::DialogNode g1 = a.dlgNodes.back();
    CHECK(textHas("Scrap is the only currency"), "node text comes from the data record");
    CHECK(g1.replies.size() == 3 && !hasReply(g1, "[Barter 30]"), "skill condition hides the Barter answer (%zu replies)", g1.replies.size());
    CHECK(questState() == -1, "quest not started before the dialog");
    CHECK(choose(0) && textHas("Name is Joe"), "goto moves to the next node");
    closeDlg();
    CHECK(questState() == -1, "browsing does not start the quest");

    // level condition: the gated answer appears after the level-up
    say("/givexp 3000");
    pause(1.0);
    CHECK(talk(joe) && textHas("Scrap is the only currency"), "second Talk starts at the greeting again");
    CHECK(choose(1) && textHas("tool kit"), "answer 'looking for work' leads to the job node");
    CHECK(questState() == 0, "quest effect added scrapper_toolkit (active)");
    CHECK(gvar("scrapper_met") == 1, "flag effect set scrapper_met");
    CHECK(hasReply(a.dlgNodes.back(), "[Level 2]"), "stat:level condition passes at level >= 2");
    const int fak0 = invCount("First Aid");
    CHECK(choose(0) && textHas("first aid kit"), "advance answer leads to advance_given");
    pause(0.8);
    CHECK(invCount("First Aid") == fak0 + 1, "give effect added a First Aid Kit (%d -> %d)", fak0, invCount("First Aid"));
    CHECK(gvar("scrapper_advance") == 1, "flag effect set scrapper_advance");
    CHECK(a.dlgNodes.back().replies.size() == 1, "a node without answers offers one way out");
    {
        int c0 = a.dlgCloses;
        CHECK(choose(0) && a.dlgCloses > c0, "the leave reply ends the dialog");
    }

    // quest routing: an active quest starts at the reminder; the item condition hides the hand-in answer
    CHECK(talk(joe) && textHas("Do you have my tool kit"), "start route by quest state (active -> reminder)");
    CHECK(a.dlgNodes.back().replies.size() == 1 && !hasReply(a.dlgNodes.back(), "Here it is"), "item condition hides the hand-in answer without the kit");
    closeDlg();
    say("/give KitsTool_Kit.ITM");
    pause(1.0);
    CHECK(invCount("Tool Kit") >= 1, "dev kit gave a Tool Kit");
    CHECK(talk(joe) && hasReply(a.dlgNodes.back(), "Here it is"), "item condition shows the hand-in answer once the kit is carried");
    const int tk0 = invCount("Tool Kit");
    int ri = 0;
    for (size_t i = 0; i < a.dlgNodes.back().replies.size(); ++i) if (a.dlgNodes.back().replies[i].text.find("Here it is") != std::string::npos) ri = (int)i;
    CHECK(choose(ri) && textHas("That is my kit"), "hand-in answer leads to thanks");
    pause(0.8);
    CHECK(invCount("Tool Kit") == tk0 - 1, "take effect removed the kit (%d -> %d)", tk0, invCount("Tool Kit"));
    CHECK(questState() == 1, "quest effect completed the quest");
    CHECK(gvar("scrapper_done") == 1, "flag effect stored an integer value");
    closeDlg();
    CHECK(talk(joe) && textHas("Thanks again"), "start route by quest state (done -> after)");
    closeDlg();

    // second dialog: stf answers, 'any' condition, Lua hook + attack effect
    say("/near Drifter");
    pause(1.0);
    const CritterId drifter = findNpc("Drifter");
    CHECK(drifter != 0, "Drifter (dialog junktown_drifter) is in view");
    if (drifter) {
        CHECK(talk(drifter), "drifter dialog opens");
        const gnet::DialogNode d1 = a.dlgNodes.back();
        CHECK(d1.replies.size() == 3 && !d1.replies[0].text.empty() && !d1.replies[1].text.empty(), "stf answers resolve to original text ('%s' / '%s'), 'any' condition passes at level 3 (%zu replies)",
              d1.replies.size() > 0 ? d1.replies[0].text.c_str() : "", d1.replies.size() > 1 ? d1.replies[1].text.c_str() : "", d1.replies.size());
        if (d1.replies.size() == 3) {
            CHECK(choose(0) && textHas("Psst"), "trade stub keeps the dialog open (goto the same node)");
            a.chat.clear();
            int c0 = a.dlgCloses;
            choose(2);
            pause(1.0);
            CHECK(AnyChat(a, 0, "hand drops to his belt"), "lua effect ran dlg_demo_hook");
            CHECK(a.dlgCloses > c0, "the attack effect closed the dialog");
            CHECK(gvar("drifter_robbed") == 1, "flag effect ahead of the attack was applied");
        }
    }

    // third dialog: 'any' start route and a 'not' condition
    say("/near Mara");
    pause(1.0);
    const CritterId mara = findNpc("Old Mara");
    CHECK(mara != 0, "Old Mara (dialog junktown_guide) is in view");
    if (mara) {
        CHECK(talk(mara) && textHas("Seen you around"), "start route with an 'any' condition (level >= 3)");
        CHECK(hasReply(a.dlgNodes.back(), "Remind me"), "'not flag' condition shows the answer while the flag is unset");
        closeDlg();
    }
    std::printf(g_failed ? "DIALOG TESTS FAILED (%d)\n" : "dialog tests passed\n", g_failed);
    return g_failed ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// Object scripts suite (data/objects, docs/object-scripts.md): f3_gametest --objects 1|2 against `f3_server --data ... --content <copy of data/ with the
// samples renamed to samples.json and engine/tests/objdata/test_objects.json> --dev-kit` (saves dir shared by both phases, server RESTARTED in between).
// Phase 1: cursor label + name override reach the client, a bound computer opens a dialog (npc 0), the Science gate (roll forced with /lockroll)
// fails and succeeds, a declarative skill check with success / failure steps, hidden_until, the server-side range check, one-shot objects.
// Phase 2: after the restart the one-shot marks and the object flag of the computer are still there.
// ---------------------------------------------------------------------------------------------------------------------
static int RunObjectTests(uint16_t port, int phase) {
    Bot a;
    if (!a.Connect(port)) { std::printf("FAIL: cannot connect\n"); return 1; }
    a.Login("ObjBot");
    CHECK(WaitFor({&a}, 8, [&] { return a.loggedIn && a.havePos && a.haveStats; }), "logged in");
    if (!a.loggedIn) return 1;
    auto say = [&](const std::string& t) { net::ChatReq c; c.text = t; a.Send(net::Pack(c)); };
    auto idle = [&](double sec) { WaitFor({&a}, sec, [] { return false; }); };
    auto said = [&](size_t from, const char* needle) { for (size_t i = from; i < a.chat.size(); ++i) if (a.chat[i].find(needle) != std::string::npos) return true; return false; };
    auto objAt = [&](int x, int y) -> ItemId {
        for (auto& [id, it] : a.items)
            if (it.x == x && it.y == y && a.objInfo.count(id) && a.objInfo[id].kind != gnet::ObjectKind::Ground) return id;
        return 0;
    };
    auto tpNear = [&](int x, int y, int minR = 0) {   // the first walkable cell around (x, y) that /tp accepts
        for (int r = minR; r <= 12; ++r)
            for (int dy = -r; dy <= r; ++dy)
                for (int dx = -r; dx <= r; ++dx) {
                    if (std::max(std::abs(dx), std::abs(dy)) != r || !Walkable(a, x + dx, y + dy)) continue;
                    say("/tp Tutorial_Vault " + std::to_string(x + dx) + " " + std::to_string(y + dy));
                    if (WaitFor({&a}, 1.5, [&] { return a.pos == TilePos{(int16_t)(x + dx), (int16_t)(y + dy)}; })) { idle(1.0); return true; }
                }
        return false;
    };
    // the map grid is only known after a /tp into the vault: the first teleport targets a cell known to be walkable
    say("/tp Tutorial_Vault 143 220");
    WaitFor({&a}, 6, [&] { return a.map.name == "00_04_Tutorial_Vault" && a.pos == TilePos{143, 220}; });
    idle(1.0);
    say("/amxattacks off");
    auto use = [&](ItemId id) { gnet::Use u; u.id = id; a.Send(gnet::PackG(u)); };
    auto skill = [&](ItemId id, int sk) { gnet::UseSkill us; us.skill = (uint8_t)sk; us.kind = 2; us.target = id; a.Send(gnet::PackG(us)); };
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
    const int kScience = 9;
    const int compX = 98, compY = 165, leverX = 215, leverY = 56, yelX = 226, yelY = 198, blueX = 211, blueY = 243, shelfX = 182, shelfY = 246;

    // ---- the computer (sample binding vault_computer: Lua script, label "Use")
    CHECK(tpNear(compX, compY), "teleported next to the computer");
    ItemId comp = objAt(compX, compY);
    CHECK(comp != 0 && a.objInfo[comp].kind == gnet::ObjectKind::Usable, "the computer is in view (id %u)", comp);
    if (!comp) return 1;
    if (phase == 1) {
        CHECK(a.objInfo[comp].label == "Use", "the server sends the cursor label of the binding (\"%s\")", a.objInfo[comp].label.c_str());
        {
            size_t n = a.chat.size();
            say("/objs");
            CHECK(WaitFor({&a}, 3, [&] { return said(n, "vault_computer"); }), "/objs lists the bound computer");
        }
        size_t d0 = a.dlgNodes.size();
        use(comp);
        CHECK(WaitFor({&a}, 8, [&] { return a.dlgNodes.size() > d0; }), "using the bound computer opens a data dialog");
        CHECK(!a.dlgOpens.empty() && a.dlgOpens.back().npc == 0 && !a.dlgOpens.back().name.empty(), "an object speaker has npc id 0 and a name");
        CHECK(!a.dlgNodes.empty() && a.dlgNodes.back().text.find("MAINTENANCE TERMINAL") != std::string::npos, "the node text comes from data/dialogs");
        {
            int c0 = a.dlgCloses;
            gnet::DialogCloseReq cl; a.Send(gnet::PackG(cl));
            WaitFor({&a}, 3, [&] { return a.dlgCloses > c0; });
        }
        // Science gate: a forced failed roll, then a forced success
        say("/lockroll 0");
        idle(0.5);
        size_t n = a.chat.size();
        skill(comp, kScience);
        CHECK(WaitFor({&a}, 5, [&] { return said(n, "rejects your attempt"); }), "Science gate: a failed check is reported");
        CHECK(gvar("vault_terminal_hacked") <= 0, "a failed check changes nothing");
        say("/lockroll 1");
        idle(1.5);
        n = a.chat.size();
        skill(comp, kScience);
        CHECK(WaitFor({&a}, 5, [&] { return said(n, "overseer door unlocks"); }), "Science gate: a successful check opens the vault door");
        CHECK(gvar("vault_terminal_hacked") == 1, "the success set the player flag read by the dialog");
        idle(1.5);
        n = a.chat.size();
        skill(comp, kScience);
        CHECK(WaitFor({&a}, 5, [&] { return said(n, "already unlocked"); }), "the object flag \"hacked\" is remembered (second attempt)");
        { gnet::DialogCloseReq cl; a.Send(gnet::PackG(cl)); idle(0.5); }   // an ambient AMX dialogue (Frank) must not block the use
        d0 = a.dlgNodes.size();
        use(comp);
        CHECK(WaitFor({&a}, 8, [&] { return a.dlgNodes.size() > d0; }) && a.dlgNodes.back().text.find("ACCESS GRANTED") != std::string::npos, "the dialog now starts at the unlocked node (flag condition)");
        { gnet::DialogCloseReq cl; a.Send(gnet::PackG(cl)); idle(0.5); }
        // the door itself: open on the server
        CHECK(tpNear(301, 102, 2), "teleported to the overseer door");
        ItemId od = objAt(301, 102);
        CHECK(od != 0 && a.objInfo[od].open == 1 && a.objInfo[od].locked == 0, "the overseer door was unlocked and opened by the script (open %d locked %d)", od ? a.objInfo[od].open : -1, od ? a.objInfo[od].locked : -1);
    } else {
        say("/lockroll 0");
        idle(0.5);
        size_t n = a.chat.size();
        skill(comp, kScience);
        CHECK(WaitFor({&a}, 5, [&] { return said(n, "already unlocked"); }), "the object flag \"hacked\" survived the server restart");
    }

    // ---- the lever (sample binding vault_lever: one_shot, name override, label "Pull")
    CHECK(tpNear(leverX, leverY), "teleported next to the lever");
    ItemId lever = objAt(leverX, leverY);
    CHECK(lever != 0, "the lever object is in view (id %u)", lever);
    if (!lever) return 1;
    if (phase == 1) {
        CHECK(a.items[lever].name == "Power lever" && a.objInfo[lever].label == "Pull", "name override and label reach the client (\"%s\" / \"%s\")", a.items[lever].name.c_str(), a.objInfo[lever].label.c_str());
        size_t n = a.chat.size();
        use(lever);
        CHECK(WaitFor({&a}, 8, [&] { return said(n, "grinds open"); }), "using the lever runs its on_use");
        idle(1.5);
        n = a.chat.size();
        use(lever);
        CHECK(WaitFor({&a}, 8, [&] { return said(n, "nothing more to do"); }) && !said(n, "grinds open"), "a one-shot object refuses the second use");
    } else {
        size_t n = a.chat.size();
        use(lever);
        CHECK(WaitFor({&a}, 8, [&] { return said(n, "nothing more to do"); }) && !said(n, "grinds open"), "the one-shot mark survived the server restart");
    }

    // ---- declarative bindings (test_objects.json)
    CHECK(tpNear(yelX, yelY, 6), "teleported near the yellow box (outside its use range)");
    ItemId yel = objAt(yelX, yelY);
    CHECK(yel != 0, "the yellow box is in view (id %u)", yel);
    if (!yel) return 1;
    if (phase == 1) {
        CHECK(a.objInfo[yel].label == "Grab", "declarative binding: label \"Grab\" (got \"%s\")", a.objInfo[yel].label.c_str());
        size_t n = a.chat.size();
        say("/useobj " + std::to_string(yel));
        CHECK(WaitFor({&a}, 4, [&] { return said(n, "useobj false"); }), "range check: the server refuses a use from %d cells away", std::max(std::abs(a.pos.x - yelX), std::abs(a.pos.y - yelY)));
        CHECK(!said(n, "T-ONCE") && gvar("t_once") <= 0, "the refused use did nothing and did not consume the one-shot");
        CHECK(tpNear(yelX, yelY), "teleported next to the yellow box");
        n = a.chat.size();
        say("/useobj " + std::to_string(yel));
        CHECK(WaitFor({&a}, 4, [&] { return said(n, "T-ONCE"); }) && gvar("t_once") == 3, "declarative on_use: message and flag effect (t_once = 3)");
        n = a.chat.size();
        say("/useobj " + std::to_string(yel));
        CHECK(WaitFor({&a}, 4, [&] { return said(n, "nothing more to do"); }) && !said(n, "T-ONCE"), "declarative one_shot: the second use is refused");
    } else {
        CHECK(tpNear(yelX, yelY), "teleported next to the yellow box");
        size_t n = a.chat.size();
        say("/useobj " + std::to_string(yel));
        CHECK(WaitFor({&a}, 4, [&] { return said(n, "nothing more to do"); }) && !said(n, "T-ONCE"), "the declarative one-shot mark survived the restart");
    }
    if (phase == 1) {
        // hidden_until: the shelf is not sent while the flag is unset
        CHECK(tpNear(shelfX, shelfY, 1), "teleported next to the shelf");
        idle(0.5);
        CHECK(objAt(shelfX, shelfY) == 0, "hidden_until: the shelf is not sent to a player without the flag");
        // declarative skill check on the blue box
        CHECK(tpNear(blueX, blueY), "teleported next to the blue box");
        ItemId blue = objAt(blueX, blueY);
        CHECK(blue != 0, "the blue box is in view (id %u)", blue);
        if (!blue) return 1;
        say("/lockroll 0");
        idle(0.5);
        size_t n = a.chat.size();
        skill(blue, kScience);
        CHECK(WaitFor({&a}, 5, [&] { return said(n, "T-FAIL"); }) && !said(n, "T-OK"), "declarative skill check: the failure steps run");
        CHECK(gvar("t_box_flag") <= 0, "a failed check sets no flag");
        say("/lockroll 1");
        idle(1.5);
        n = a.chat.size();
        skill(blue, kScience);
        CHECK(WaitFor({&a}, 5, [&] { return said(n, "T-OK"); }) && !said(n, "T-FAIL"), "declarative skill check: the success steps run");
        CHECK(gvar("t_box_flag") == 1, "the success steps set the flag");
        idle(1.0);
        n = a.chat.size();
        say("/useobj " + std::to_string(blue));
        CHECK(WaitFor({&a}, 4, [&] { return said(n, "T-BOX-USED"); }), "declarative on_use takes over from the default container use");
        CHECK(tpNear(152, 219, 2), "teleported to the door of the success steps");
        ItemId d5 = objAt(152, 219);
        CHECK(d5 != 0 && a.objInfo[d5].open == 1, "the door step opened Door01Vault13_005 (open %d)", d5 ? a.objInfo[d5].open : -1);
        CHECK(tpNear(shelfX, shelfY, 1), "teleported next to the shelf again");
        CHECK(WaitFor({&a}, 3, [&] { return objAt(shelfX, shelfY) != 0; }), "hidden_until: the shelf appears once the flag is set");
        say("/saveworld");
        idle(1.0);
    }
    say("/lockroll off");
    idle(0.3);
    for (auto& t : a.chat) std::printf("chat: %s\n", t.c_str());
    std::printf(g_failed ? "OBJECTTEST %d FAILED (%d)\n" : "OBJECTTEST %d PASSED\n", phase, g_failed);
    enet_deinitialize();
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
    for (int i = 1; i + 1 < argc; i++) if (!std::strcmp(argv[i], "--doors")) return RunDoorTests(port, std::atoi(argv[i + 1]), stateFile);
    for (int i = 1; i + 1 < argc; i++) if (!std::strcmp(argv[i], "--objects")) return RunObjectTests(port, std::atoi(argv[i + 1]));
    for (int i = 1; i < argc; i++) if (!std::strcmp(argv[i], "--tutorial")) return RunTutorialTests(port);
    for (int i = 1; i < argc; i++) if (!std::strcmp(argv[i], "--locale")) return RunLocaleTests(port);
    for (int i = 1; i + 1 < argc; i++) if (!std::strcmp(argv[i], "--content")) return RunContentTests(port, argv[i + 1]);
    for (int i = 1; i < argc; i++) if (!std::strcmp(argv[i], "--dialogs")) return RunDialogTests(port);

    Bot a, b;
    if (!a.Connect(port) || !b.Connect(port)) { std::printf("FAIL: cannot connect to 127.0.0.1:%u\n", port); return 1; }
    g_failHook = [&] {   // diagnostics for a failed check: the wall clock matches the server log's timestamps
        std::time_t now = std::time(nullptr);
        char ts[16]; std::strftime(ts, sizeof ts, "%H:%M:%S", std::localtime(&now));
        std::printf("  diag %s:\n", ts);
        for (Bot* bp : {&a, &b}) {
            auto st = bp->states.find(bp->myId);
            std::printf("  diag %s id %u map %s at %d,%d hp %d/%d dead %d, %zu critters in view, %zu combat events\n", bp->name.c_str(), (unsigned)bp->myId, bp->map.name.c_str(), bp->pos.x, bp->pos.y,
                        st != bp->states.end() ? st->second.hp : -1, st != bp->states.end() ? st->second.maxHp : -1, st != bp->states.end() ? st->second.dead : -1, bp->critters.size(), bp->combat.size());
            for (size_t i = bp->combat.size() > 4 ? bp->combat.size() - 4 : 0; i < bp->combat.size(); ++i)
                std::printf("  diag   combat %u -> %u result %d dmg %d targetHp %d\n", (unsigned)bp->combat[i].attacker, (unsigned)bp->combat[i].target, (int)bp->combat[i].result, bp->combat[i].damage, bp->combat[i].targetHp);
            for (auto& [id, cr] : bp->critters) {
                if (id == bp->myId || TileDist(TilePos{cr.x, cr.y}, bp->pos) > 14) continue;
                auto cs = bp->states.find(id);
                std::printf("  diag   near %u '%s' at %d,%d dist %d hp %d dead %d\n", (unsigned)id, cr.name.c_str(), (int)cr.x, (int)cr.y, TileDist(TilePos{cr.x, cr.y}, bp->pos), cs != bp->states.end() ? cs->second.hp : -1, cs != bp->states.end() ? cs->second.dead : -1);
            }
            for (size_t i = bp->chat.size() > 3 ? bp->chat.size() - 3 : 0; i < bp->chat.size(); ++i) std::printf("  diag   chat: %.100s\n", bp->chat[i].c_str());
        }
        std::fflush(stdout);
    };
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
        net::ChatReq nr; nr.text = "/near Armstrong";
        c.Send(net::Pack(nr));
        WaitFor({&a, &b, &c}, 3, [&] { return false; });
        CritterId npc = 0;
        int bd = 1 << 30;
        for (auto& [id, cr] : c.critters) {
            if (id == a.myId || id == b.myId || id == c.myId || cr.name.find("Armstrong") == std::string::npos) continue;
            int d = TileDist(TilePos{cr.x, cr.y}, c.pos);
            if (d < bd) { bd = d; npc = id; }
        }
        CHECK(npc != 0, "found neutral NPC 'Armstrong' near bot C");
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
                  int best = 99999;   // Armstrong's prototype is displayed as "Armstrong" (npcs/original.json name): take the nearest one (we stand next to him)
                  for (auto& [id, cr] : a.critters)
                      if (cr.name == "Armstrong" && TileDist(TilePos{cr.x, cr.y}, a.pos) < best) { best = TileDist(TilePos{cr.x, cr.y}, a.pos); arm = id; }
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
                    const bool alreadyEmpty = !a.cons.empty() && a.cons.back().items.empty();   // a one-item corpse is empty after the single Take: no update follows
                    if (!alreadyEmpty) WaitFor({&a, &b}, 8, [&] { return a.cons.size() > nc4; });
                    bool heavy = false;
                    for (auto& t : a.chat) if (t.find("cannot carry") != std::string::npos) heavy = true;
                    CHECK((alreadyEmpty || a.cons.size() > nc4) && (a.cons.back().items.empty() || heavy), "Take All empties the corpse (%zu left)", a.cons.back().items.size());
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
        bool ok = false;
        for (int attempt = 0; attempt < 4 && !ok; ++attempt) {
            TilePos lastP = a.pos; double lastT = Bot::LocalMs();   // done when the map changed or the bot stood still for 3 s
            ok = WaitFor({&a, &b}, 30, [&] {
                if (a.mapInfoCount > maps) return true;
                if (a.pos != lastP) { lastP = a.pos; lastT = Bot::LocalMs(); }
                return Bot::LocalMs() - lastT > 3000;
            }) && a.mapInfoCount > maps;
            if (ok) break;
            // the player does not open doors by walking (original behaviour): when the walk stops at a closed door, use the nearest one and go on
            ItemId nearDoor = 0; int nd = 99;
            for (auto& [id, o] : a.objInfo)
                if (o.kind == gnet::ObjectKind::Door && o.open == 0 && a.items.count(id)) {
                    int d = TileDist({a.items[id].x, a.items[id].y}, a.pos);
                    if (d < nd) { nd = d; nearDoor = id; }
                }
            std::printf("note: transition walk idle at %d,%d; nearest closed door %u at %d cells\n", a.pos.x, a.pos.y, nearDoor, nd);
            if (!nearDoor || nd > 12) break;
            std::printf("note: walk to the transition stopped at closed door %u (%d cells); using it\n", nearDoor, nd);
            gnet::Use u; u.id = nearDoor; a.Send(gnet::PackG(u));
            WaitFor({&a, &b}, 3, [&] { return a.objInfo[nearDoor].open == 1; });
            a.Send(net::Pack(mv));
        }
        CHECK(ok && a.map.name == "00_04_Tutorial_Vault", "walking into the transition region loaded map '%s'", a.map.name.c_str());
        CHECK(a.map.width == 384 && a.map.height == 334, "vault grid is %dx%d (expect 384x334)", a.map.width, a.map.height);
        WaitFor({&a}, 1, [] { return false; });
        CHECK(Walkable(a, a.pos.x, a.pos.y), "arrival cell %d,%d is walkable", a.pos.x, a.pos.y);
        // grid alignment: the mapdata grid starts at the tre root bbox min (44, 4) in the Vault. Door01 at world (78.89, 39.97) stands in a
        // doorway (closed: blocks its cells until somebody opens it) between two wall cells; Door02 at (141, 45) is initially locked and blocks its doorway.
        // (a closed door blocks its doorway cells; the door suite, --doors, opens it and walks through)
        CHECK(!Walkable(a, 69, 71) && !Walkable(a, 60, 71) && !Walkable(a, 82, 71), "Vault grid origin: Door01 doorway (69,71) is blocked while the door is closed, wall cells beside it too");
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
