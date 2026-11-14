// f3_testclient: headless bot for smoke tests. Exits 0 when every check passed.
//   f3_testclient [--host H] [--port N] [--name NAME] [--timeout S]
// Checks: connect+login, map received, self visible, an NPC visible, move round trip
// (server-validated path ends on the requested tile), blocked-tile request rejected,
// chat echo, scripted /pos command reply, ping.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include "ClientCore.h"
#include "common/log.h"

using namespace f3;

namespace {
int g_failed = 0;
void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    std::fflush(stdout);
    if (!ok) ++g_failed;
}
}  // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1", name = "Bot";
    uint16_t port = kDefaultPort;
    double timeout = 10.0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--host") host = next();
        else if (a == "--port") port = static_cast<uint16_t>(std::atoi(next()));
        else if (a == "--name") name = next();
        else if (a == "--timeout") timeout = std::atof(next());
    }

    ClientCore c;
    auto waitFor = [&](auto pred, double seconds) {
        double end = c.Now() + seconds;
        while (c.Now() < end) {
            c.Poll();
            if (pred()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return pred();
    };

    if (!c.Connect(host, port, name)) { Check(false, "connect call"); return 1; }
    bool in = waitFor([&] { return c.state() == ClientCore::State::InGame || c.state() == ClientCore::State::Failed; }, timeout);
    Check(in && c.state() == ClientCore::State::InGame, "login accepted");
    if (c.state() != ClientCore::State::InGame) { std::printf("reason: %s\n", c.failReason().c_str()); return 1; }

    Check(waitFor([&] { return c.hasMap() && c.self() != nullptr; }, timeout), "map info + self critter received");
    if (!c.self()) return 1;
    std::printf("self #%u at %d,%d on '%s' %dx%d\n", c.selfId(), c.self()->pos.x, c.self()->pos.y,
                c.map().name.c_str(), c.map().width, c.map().height);

    bool sawNpc = waitFor([&] {
        for (auto& [id, cr] : c.critters()) if (cr.kind == CritterKind::Npc) return true;
        return false;
    }, 2.0);
    Check(sawNpc, "npc visible");
    Check(!c.items().empty() || waitFor([&] { return !c.items().empty(); }, 1.0), "item visible");

    // move round trip: pick a free tile 6 away
    const ClientMap& m = c.map();
    TilePos start = c.self()->pos, target = start;
    bool found = false;
    for (int r = 6; r >= 3 && !found; --r)
        for (int d = 0; d < 8 && !found; ++d) {
            int tx = start.x + kDirDx[d] * r, ty = start.y + kDirDy[d] * r;
            if (!m.IsBlocked(tx, ty)) { target = TilePos{(int16_t)tx, (int16_t)ty}; found = true; }
        }
    Check(found, "found a target tile");
    c.RequestMove(target);
    bool arrived = waitFor([&] { return c.self() && c.self()->pos == target; }, timeout);
    std::printf("moved %d,%d -> %d,%d (target %d,%d)\n", start.x, start.y, c.self()->pos.x, c.self()->pos.y, target.x, target.y);
    Check(arrived, "move round trip: server walked us to the target tile");

    // a blocked target must not move us (map corner 0,0 is a wall)
    TilePos before = c.self()->pos;
    c.RequestMove(TilePos{0, 0});
    waitFor([] { return false; }, 0.5);
    Check(c.self()->pos == before, "move to blocked tile rejected");

    // chat echo (server broadcasts to speaker too)
    uint64_t chatBefore = c.chatCounter();
    c.SendChat("hello from the bot");
    bool echoed = waitFor([&] {
        for (auto& l : c.chat()) if (l.name == name && l.text == "hello from the bot") return true;
        return false;
    }, 3.0);
    Check(echoed, "chat echo received");

    // scripted command (scripts/main.lua)
    c.SendChat("/pos");
    bool pos = waitFor([&] {
        for (auto& l : c.chat()) if (l.channel == ChatChannel::System && l.text.rfind("You are at", 0) == 0) return true;
        return false;
    }, 3.0);
    Check(pos, "lua /pos command answered");
    (void)chatBefore;

    c.SendPing();
    Check(waitFor([&] { return c.lastPongToken() != 0; }, 2.0), "ping/pong");

    c.Disconnect();
    std::printf("%s (%d failed)\n", g_failed ? "SMOKE FAILED" : "SMOKE OK", g_failed);
    return g_failed ? 1 : 0;
}
