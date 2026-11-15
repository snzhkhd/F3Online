// Real-time action timing, FOnline style: every action sets a breaktime ("Wait") during which the critter's further
// actions are rejected; AP regenerate linearly over ApRegeneration ms; combat sets a battle flag for TimeoutBattle ms.
// All numbers come from scripts/config.lua (GameConfig). Server-authoritative: the client only mirrors WaitState.
#include <algorithm>
#include "Server.h"
#include "common/log.h"

namespace f3 {

uint64_t Server::NowMs() const {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now() - start_).count()) + 1000;
}

int Server::MaxAp(const Critter& c) const {
    int div = std::max(1, rt_.ApAgilityDiv);
    return std::clamp((rt_.ApBase + c.special[5] / div) * 100, 100, 25500);
}

void Server::SyncAp(Critter& c) {
    uint64_t now = NowMs();
    int maxAp = MaxAp(c);
    if (c.ap < 0 || c.ap > maxAp) { c.ap = maxAp; c.apRegenMs = now; return; }
    if (c.ap == maxAp) { c.apRegenMs = now; return; }
    if (rt_.ApRegeneration <= 0) { c.ap = maxAp; c.apRegenMs = now; return; }
    uint64_t delta = now > c.apRegenMs ? now - c.apRegenMs : 0;
    uint64_t gain = static_cast<uint64_t>(maxAp) * delta / static_cast<uint64_t>(rt_.ApRegeneration);
    if (gain == 0) return;  // keep the timestamp so fractions accumulate
    if (c.ap + static_cast<int64_t>(gain) >= maxAp) { c.ap = maxAp; c.apRegenMs = now; return; }
    c.ap += static_cast<int>(gain);
    c.apRegenMs += gain * static_cast<uint64_t>(rt_.ApRegeneration) / static_cast<uint64_t>(maxAp);
}

void Server::Wait(Critter& c, int ms, bool notify) {
    c.waitEndMs = NowMs() + static_cast<uint64_t>(std::max(0, ms));
    if (notify && c.IsPlayer()) SendWaitState(c);
}

void Server::SetBattle(Critter& c, bool notify, int ms) {
    c.battleEndMs = NowMs() + static_cast<uint64_t>(std::max(0, ms >= 0 ? ms : rt_.TimeoutBattle));
    if (notify && c.IsPlayer()) SendWaitState(c);
}

bool Server::SpendAp(Critter& c, int cost) {
    SyncAp(c);
    if (cost <= 0) return true;
    if (c.ap < cost) return false;
    c.ap -= cost;
    return true;
}

void Server::SendWaitState(Critter& c, uint8_t action, gnet::ActionResult r) {
    if (!c.peer || !(c.caps & gnet::kCapContinuousMove)) return;
    SyncAp(c);
    uint64_t now = NowMs();
    gnet::WaitState w;
    w.nowMs = Ms32(now);
    w.waitEndMs = Ms32(std::max(now, c.waitEndMs));
    w.ap = static_cast<uint16_t>(std::clamp(c.ap, 0, 65535));
    w.maxAp = static_cast<uint16_t>(MaxAp(c));
    w.apRegenMs = static_cast<uint32_t>(std::max(0, rt_.ApRegeneration));
    w.battleEndMs = Ms32(std::max(now, c.battleEndMs));
    w.action = action;
    w.result = r;
    Send(c, gnet::PackG(w));
}

gnet::ActionResult Server::CheckAction(Critter& c, uint8_t action, int apCost, bool report) {
    SyncAp(c);
    gnet::ActionResult r = gnet::ActionResult::Ok;
    if (c.dead) r = gnet::ActionResult::Dead;
    else if (NowMs() < c.waitEndMs) r = gnet::ActionResult::Busy;
    else if (c.ap < apCost) r = gnet::ActionResult::NoAp;
    if (r != gnet::ActionResult::Ok && report && c.IsPlayer()) SendWaitState(c, action, r);
    return r;
}

void Server::CommitAction(Critter& c, uint8_t action, int apCost, int waitMs) {
    SyncAp(c);
    if (apCost > 0) c.ap = std::max(0, c.ap - apCost);
    c.waitEndMs = NowMs() + static_cast<uint64_t>(std::max(0, waitMs));
    if (c.IsPlayer()) SendWaitState(c, action, gnet::ActionResult::Ok);
}

int Server::AttackApCost(int weaponCost) const {
    return std::clamp(std::max(rt_.RtApCostAttackMin, weaponCost * rt_.RtApCostAttackPerCost), 0, 25500);
}

// AP cost (1/100 AP) of the non-attack actions, by request id (GMsg).
int Server::ActionCost(uint8_t action) const {
    switch (static_cast<GMsg>(action)) {
        case GMsg::C2S_PickUp: return rt_.RtApCostPickItem;
        case GMsg::C2S_Drop: return rt_.RtApCostDropItem;
        case GMsg::C2S_Equip: return rt_.RtApCostMoveItemInventory;
        case GMsg::C2S_Use: return rt_.RtApCostUseItem;
        case GMsg::C2S_Take: return rt_.RtApCostMoveItemContainer;
        case GMsg::C2S_Put: return rt_.RtApCostMoveItemContainer;
        case GMsg::C2S_Loot: return rt_.RtApCostUseItem;
        default: return 0;
    }
}

}  // namespace f3
