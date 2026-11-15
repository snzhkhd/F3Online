#pragma once
// Gameplay protocol extension (inventory, stats, combat, doors/containers). Kept separate from
// protocol.h so the base protocol stays untouched; ids live in unused ranges (client->server
// 20..39, server->client 120..139). Old clients ignore unknown server messages.
//
// Entities are addressed by the ids already used in protocol.h: critter ids (CritterAdd) and map
// object ids (ItemAdd.id, which now also covers doors, containers and usables; see ObjectInfo).
// ItemAdd.pid / InvEntry.pid index the server's prototype list, which is the key-sorted order of
// gamedata/server/protos.json (clients that read the original .een files can reproduce it).
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "protocol.h"

namespace f3 {

enum class GMsg : uint8_t {
    // client -> server
    C2S_PickUp = 20,   // ground item id; the server walks the player adjacent first
    C2S_Drop = 21,     // inventory item id
    C2S_Equip = 22,    // inventory item id + on/off
    C2S_Use = 23,      // map object id: toggles a door, opens a container, triggers a usable
    C2S_Attack = 24,   // critter id (0 = stop attacking)
    C2S_Take = 25,     // container id + inventory entry id (0 = take everything)
    C2S_Examine = 26,  // id + isCritter, answered with a system chat line
    C2S_ReqState = 27, // resend Stats + Inventory
    // server -> client
    S2C_Stats = 120,
    S2C_Inventory = 121,
    S2C_ObjectInfo = 122,
    S2C_CritterState = 123,
    S2C_CombatEvent = 124,
    S2C_Container = 125,
    S2C_CellsSet = 126,
    S2C_Appearance = 139,  // appearance block: what a critter wears / wields (armor, helmet, hand-1 weapon)
};

namespace gnet {

template <class T>
inline ByteBuffer PackG(const T& m) {
    ByteBuffer b;
    b.WriteU8(static_cast<uint8_t>(T::kId));
    m.Write(b);
    return b;
}

struct PickUp {
    static constexpr GMsg kId = GMsg::C2S_PickUp;
    uint32_t id = 0;
    void Write(ByteBuffer& b) const { b.WriteU32(id); }
    bool Read(ByteBuffer& b) { id = b.ReadU32(); return b.Ok(); }
};
struct Drop {
    static constexpr GMsg kId = GMsg::C2S_Drop;
    uint32_t id = 0;
    void Write(ByteBuffer& b) const { b.WriteU32(id); }
    bool Read(ByteBuffer& b) { id = b.ReadU32(); return b.Ok(); }
};
struct Equip {
    static constexpr GMsg kId = GMsg::C2S_Equip;
    uint32_t id = 0;
    uint8_t on = 1;
    // --- inventory UI block: optional target slot (EquipSlot, 0 = server picks); older clients omit the byte ---
    uint8_t slot = 0;
    void Write(ByteBuffer& b) const { b.WriteU32(id); b.WriteU8(on); b.WriteU8(slot); }
    bool Read(ByteBuffer& b) { id = b.ReadU32(); on = b.ReadU8(); slot = b.Remaining() ? b.ReadU8() : 0; return b.Ok(); }
    // --- end inventory UI block ---
};
struct Use {
    static constexpr GMsg kId = GMsg::C2S_Use;
    uint32_t id = 0;
    void Write(ByteBuffer& b) const { b.WriteU32(id); }
    bool Read(ByteBuffer& b) { id = b.ReadU32(); return b.Ok(); }
};
struct Attack {
    static constexpr GMsg kId = GMsg::C2S_Attack;
    uint32_t id = 0;
    void Write(ByteBuffer& b) const { b.WriteU32(id); }
    bool Read(ByteBuffer& b) { id = b.ReadU32(); return b.Ok(); }
};
struct Take {
    static constexpr GMsg kId = GMsg::C2S_Take;
    uint32_t container = 0, entry = 0;
    void Write(ByteBuffer& b) const { b.WriteU32(container); b.WriteU32(entry); }
    bool Read(ByteBuffer& b) { container = b.ReadU32(); entry = b.ReadU32(); return b.Ok(); }
};
struct Examine {
    static constexpr GMsg kId = GMsg::C2S_Examine;
    uint32_t id = 0;
    uint8_t isCritter = 0;
    void Write(ByteBuffer& b) const { b.WriteU32(id); b.WriteU8(isCritter); }
    bool Read(ByteBuffer& b) { id = b.ReadU32(); isCritter = b.ReadU8(); return b.Ok(); }
};
struct ReqState {
    static constexpr GMsg kId = GMsg::C2S_ReqState;
    void Write(ByteBuffer&) const {}
    bool Read(ByteBuffer& b) { return b.Ok(); }
};

struct Stats {
    static constexpr GMsg kId = GMsg::S2C_Stats;
    uint32_t critterId = 0;
    uint8_t special[7] = {5, 5, 5, 5, 5, 5, 5};  // S P E C I A L
    uint8_t level = 1;
    int16_t hp = 0, maxHp = 0;
    uint8_t armorClass = 0, sequence = 0, meleeBonus = 0;
    uint16_t carryWeight = 0, carried = 0;
    void Write(ByteBuffer& b) const {
        b.WriteU32(critterId);
        for (int i = 0; i < 7; i++) b.WriteU8(special[i]);
        b.WriteU8(level); b.WriteI16(hp); b.WriteI16(maxHp);
        b.WriteU8(armorClass); b.WriteU8(sequence); b.WriteU8(meleeBonus);
        b.WriteU16(carryWeight); b.WriteU16(carried);
    }
    bool Read(ByteBuffer& b) {
        critterId = b.ReadU32();
        for (int i = 0; i < 7; i++) special[i] = b.ReadU8();
        level = b.ReadU8(); hp = b.ReadI16(); maxHp = b.ReadI16();
        armorClass = b.ReadU8(); sequence = b.ReadU8(); meleeBonus = b.ReadU8();
        carryWeight = b.ReadU16(); carried = b.ReadU16();
        return b.Ok();
    }
};

// --- inventory UI block ---------------------------------------------------------------------------------------
// Equipment slots. InvEntry.equipped is the slot the item occupies (0 = carried only), so "equipped != 0" still
// means "worn/wielded" for older code. Hand1 is the active weapon (used for attacks), Hand2 the spare weapon.
enum EquipSlot : uint8_t { SlotNone = 0, SlotBody = 1, SlotHead = 2, SlotHand1 = 3, SlotHand2 = 4, SlotCount = 5 };
// -------------------------------------------------------------------------------------------------------------

struct InvEntry {
    uint32_t id = 0;
    uint16_t pid = 0, count = 1;
    uint8_t equipped = 0;   // EquipSlot
    std::string name;
    uint16_t weight = 0;    // inventory UI block: weight of ONE unit (server table / config), carried weight = weight * count
    void Write(ByteBuffer& b) const { b.WriteU32(id); b.WriteU16(pid); b.WriteU16(count); b.WriteU8(equipped); b.WriteString(name); b.WriteU16(weight); }
    bool Read(ByteBuffer& b) { id = b.ReadU32(); pid = b.ReadU16(); count = b.ReadU16(); equipped = b.ReadU8(); name = b.ReadString(64); weight = b.ReadU16(); return b.Ok(); }
};

// Owner's inventory (S2C_Inventory) or a container's contents (S2C_Container, owner = container id).
struct ItemList {
    uint32_t owner = 0;
    std::vector<InvEntry> items;
    void Write(ByteBuffer& b) const {
        b.WriteU32(owner); b.WriteU16(static_cast<uint16_t>(items.size()));
        for (auto& e : items) e.Write(b);
    }
    bool Read(ByteBuffer& b) {
        owner = b.ReadU32();
        uint16_t n = b.ReadU16();
        if (!b.Ok() || n > 1024) return false;
        items.resize(n);
        for (auto& e : items) if (!e.Read(b)) return false;
        return b.Ok();
    }
};
struct Inventory : ItemList { static constexpr GMsg kId = GMsg::S2C_Inventory; };
struct Container : ItemList { static constexpr GMsg kId = GMsg::S2C_Container; };

enum class ObjectKind : uint8_t { Ground = 0, Door = 1, Container = 2, Usable = 3 };

// Sent after ItemAdd for objects that are not plain ground items, and whenever the state changes.
struct ObjectInfo {
    static constexpr GMsg kId = GMsg::S2C_ObjectInfo;
    uint32_t id = 0;
    ObjectKind kind = ObjectKind::Ground;
    uint8_t open = 0;  // doors: 1 = open
    void Write(ByteBuffer& b) const { b.WriteU32(id); b.WriteU8(static_cast<uint8_t>(kind)); b.WriteU8(open); }
    bool Read(ByteBuffer& b) { id = b.ReadU32(); kind = static_cast<ObjectKind>(b.ReadU8()); open = b.ReadU8(); return b.Ok(); }
};

// --- appearance block -----------------------------------------------------------------------------------------
// Prototype indices (ItemAdd.pid space) of the body armor, the helmet and the active (hand 1) weapon of a critter;
// 0xFFFF = nothing. Sent to every client that sees the critter when it appears and whenever the equipment changes.
constexpr uint16_t kNoPid = 0xFFFF;
struct Appearance {
    static constexpr GMsg kId = GMsg::S2C_Appearance;
    uint32_t id = 0;
    uint16_t body = kNoPid, head = kNoPid, weapon = kNoPid;
    void Write(ByteBuffer& b) const { b.WriteU32(id); b.WriteU16(body); b.WriteU16(head); b.WriteU16(weapon); }
    bool Read(ByteBuffer& b) { id = b.ReadU32(); body = b.ReadU16(); head = b.ReadU16(); weapon = b.ReadU16(); return b.Ok(); }
};
// --- end appearance block -------------------------------------------------------------------------------------

struct CritterState {
    static constexpr GMsg kId = GMsg::S2C_CritterState;
    uint32_t id = 0;
    int16_t hp = 0, maxHp = 0;
    uint8_t dead = 0;
    void Write(ByteBuffer& b) const { b.WriteU32(id); b.WriteI16(hp); b.WriteI16(maxHp); b.WriteU8(dead); }
    bool Read(ByteBuffer& b) { id = b.ReadU32(); hp = b.ReadI16(); maxHp = b.ReadI16(); dead = b.ReadU8(); return b.Ok(); }
};

enum class HitResult : uint8_t { Miss = 0, Hit = 1, Critical = 2, Kill = 3 };

struct CombatEvent {
    static constexpr GMsg kId = GMsg::S2C_CombatEvent;
    uint32_t attacker = 0, target = 0;
    HitResult result = HitResult::Miss;
    int16_t damage = 0, targetHp = 0;
    uint16_t durMs = 0;   // appearance block: duration of the attack action (the animation is stretched to it); 0 = unknown
    void Write(ByteBuffer& b) const { b.WriteU32(attacker); b.WriteU32(target); b.WriteU8(static_cast<uint8_t>(result)); b.WriteI16(damage); b.WriteI16(targetHp); b.WriteU16(durMs); }
    bool Read(ByteBuffer& b) { attacker = b.ReadU32(); target = b.ReadU32(); result = static_cast<HitResult>(b.ReadU8()); damage = b.ReadI16(); targetHp = b.ReadI16(); durMs = b.Remaining() >= 2 ? b.ReadU16() : 0; return b.Ok(); }
};

// Cells whose blocked flag changed (door opened/closed).
struct CellsSet {
    static constexpr GMsg kId = GMsg::S2C_CellsSet;
    struct Cell { int16_t x = 0, y = 0; uint8_t blocked = 0; };
    std::vector<Cell> cells;
    void Write(ByteBuffer& b) const {
        b.WriteU16(static_cast<uint16_t>(cells.size()));
        for (auto& c : cells) { b.WriteI16(c.x); b.WriteI16(c.y); b.WriteU8(c.blocked); }
    }
    bool Read(ByteBuffer& b) {
        uint16_t n = b.ReadU16();
        if (!b.Ok() || n > 4096) return false;
        cells.resize(n);
        for (auto& c : cells) { c.x = b.ReadI16(); c.y = b.ReadI16(); c.blocked = b.ReadU8(); }
        return b.Ok();
    }
};

}  // namespace gnet
}  // namespace f3

// ===================================================================================================
// REAL-TIME TIMING BLOCK (server agent). Ids: client->server 28..29, server->client 127..129.
// Time base: the server clock in milliseconds (u32, wraps; compare with unsigned subtraction). Clients never need
// to sync clocks: every message that carries a server time also carries `nowMs` (server time at send), so a client
// computes age = nowMs - startMs and interpolates from its own receive time.
// Positions are float CELL units (1 cell = 0.5 world unit); cell (cx,cy) has its centre at (cx+0.5, cy+0.5); the
// legacy int16 positions are floor() of these. A client that sends ClientCaps(bit0) gets MovePath/CritterStop/
// WaitState; clients that do not keep receiving the legacy CritterMove (one per cell crossed). Floats are sent as
// their IEEE-754 bit pattern in a u32 (little endian). Docs: docs/server-data.md, section "Real-time timing".
// ===================================================================================================
namespace f3 {

enum class GMsgRt : uint8_t {
    C2S_ClientCaps = 28,  // u32 caps; bit0 = understands MovePath/CritterStop/WaitState (send once after LoginOk)
    C2S_MoveToF = 29,     // f32 x, f32 y (cells), u8 run (0 walk, 1 run): a precise destination point
    S2C_MovePath = 127,
    S2C_CritterStop = 128,
    S2C_WaitState = 129,
};

namespace gnet {

inline constexpr uint32_t kCapContinuousMove = 1;

inline void WriteF32(ByteBuffer& b, float v) { uint32_t u; std::memcpy(&u, &v, 4); b.WriteU32(u); }
inline float ReadF32(ByteBuffer& b) { uint32_t u = b.ReadU32(); float v; std::memcpy(&v, &u, 4); return v; }

struct ClientCaps {
    static constexpr GMsg kId = static_cast<GMsg>(GMsgRt::C2S_ClientCaps);
    uint32_t caps = 0;
    void Write(ByteBuffer& b) const { b.WriteU32(caps); }
    bool Read(ByteBuffer& b) { caps = b.ReadU32(); return b.Ok(); }
};

struct MoveToF {
    static constexpr GMsg kId = static_cast<GMsg>(GMsgRt::C2S_MoveToF);
    float x = 0, y = 0;
    uint8_t run = 1;
    void Write(ByteBuffer& b) const { WriteF32(b, x); WriteF32(b, y); b.WriteU8(run); }
    bool Read(ByteBuffer& b) { x = ReadF32(b); y = ReadF32(b); run = b.ReadU8(); return b.Ok(); }
};

// A critter starts walking along a polyline. pts[0] is the position at startMs (the critter's exact position when
// the move began); it moves along the straight segments at constant `speed` (cells/s) and stops at the last point.
// Replaces any previous path of that critter. The server validated every segment against the walk grid.
struct MovePath {
    static constexpr GMsg kId = static_cast<GMsg>(GMsgRt::S2C_MovePath);
    struct Pt { float x = 0, y = 0; };
    uint32_t id = 0;
    uint32_t nowMs = 0;    // server time when sent
    uint32_t startMs = 0;  // server time at which the critter was at pts[0] (<= nowMs)
    float speed = 0;       // cells per second
    uint8_t run = 0;
    std::vector<Pt> pts;   // 2..256 points
    void Write(ByteBuffer& b) const {
        b.WriteU32(id); b.WriteU32(nowMs); b.WriteU32(startMs); WriteF32(b, speed); b.WriteU8(run);
        b.WriteU16(static_cast<uint16_t>(pts.size()));
        for (auto& p : pts) { WriteF32(b, p.x); WriteF32(b, p.y); }
    }
    bool Read(ByteBuffer& b) {
        id = b.ReadU32(); nowMs = b.ReadU32(); startMs = b.ReadU32(); speed = ReadF32(b); run = b.ReadU8();
        uint16_t n = b.ReadU16();
        if (!b.Ok() || n > 256 || static_cast<size_t>(n) * 8 > b.Remaining()) return false;
        pts.resize(n);
        for (auto& p : pts) { p.x = ReadF32(b); p.y = ReadF32(b); }
        return b.Ok();
    }
};

// The critter stands at (x,y) now: end of a path (reason 0), interrupted/blocked/out of AP (1), or a resync of a freshly
// added critter (3). Teleports and map changes keep using the legacy CritterMove with snap=1.
struct CritterStop {
    static constexpr GMsg kId = static_cast<GMsg>(GMsgRt::S2C_CritterStop);
    uint32_t id = 0;
    float x = 0, y = 0;
    uint8_t dir = 0;  // Dir
    uint8_t reason = 0;
    void Write(ByteBuffer& b) const { b.WriteU32(id); WriteF32(b, x); WriteF32(b, y); b.WriteU8(dir); b.WriteU8(reason); }
    bool Read(ByteBuffer& b) { id = b.ReadU32(); x = ReadF32(b); y = ReadF32(b); dir = b.ReadU8(); reason = b.ReadU8(); return b.Ok(); }
};

enum class ActionResult : uint8_t { Ok = 0, Busy = 1, NoAp = 2, Invalid = 3, Dead = 4, None = 255 /* unsolicited update */ };

// Sent to the owning player after every action (accepted or rejected), on login, on ReqState and when the battle flag
// changes. Client model (same as FOnline): the critter is free when server time >= waitEndMs; AP regenerate linearly:
//   ap(t) = min(maxAp, ap + maxAp * (t - nowMs) / apRegenMs)        (ap in 1/100 AP, FOnline AP_DIVIDER = 100)
struct WaitState {
    static constexpr GMsg kId = static_cast<GMsg>(GMsgRt::S2C_WaitState);
    uint32_t nowMs = 0;        // server time when sent
    uint32_t waitEndMs = 0;    // actions are rejected until server time reaches this (<= nowMs: free)
    uint16_t ap = 0;           // current AP, 1/100 AP, valid at nowMs
    uint16_t maxAp = 0;
    uint32_t apRegenMs = 0;    // milliseconds for a full regeneration (config ApRegeneration)
    uint32_t battleEndMs = 0;  // in battle while server time < battleEndMs
    uint8_t action = 0;        // GMsg id of the request this answers (0 = none)
    ActionResult result = ActionResult::None;
    void Write(ByteBuffer& b) const {
        b.WriteU32(nowMs); b.WriteU32(waitEndMs); b.WriteU16(ap); b.WriteU16(maxAp); b.WriteU32(apRegenMs); b.WriteU32(battleEndMs);
        b.WriteU8(action); b.WriteU8(static_cast<uint8_t>(result));
    }
    bool Read(ByteBuffer& b) {
        nowMs = b.ReadU32(); waitEndMs = b.ReadU32(); ap = b.ReadU16(); maxAp = b.ReadU16(); apRegenMs = b.ReadU32(); battleEndMs = b.ReadU32();
        action = b.ReadU8(); result = static_cast<ActionResult>(b.ReadU8());
        return b.Ok();
    }
};

}  // namespace gnet
}  // namespace f3

// ===================================================================================================
// CLIENT-RENDERING BLOCK (client agent). Optional: critter model identification. Id: server->client 135.
// CritterAdd carries no model. A client that has gamedata/server/protos.json maps `pid` (index in the key-sorted protos
// list, same as ItemAdd.pid) to model, skin and the `static` flag. Server: send this right after CritterAdd for every
// critter that was created from a prototype (players: pid of Default_PC.CRT). Clients without it fall back to a
// name -> prototype lookup in protos.json, so ignoring this message is harmless but less accurate.
// ===================================================================================================
namespace f3 {
enum class GMsgView : uint8_t { S2C_CritterProto = 135 };
namespace gnet {
struct CritterProto {
    static constexpr GMsg kId = static_cast<GMsg>(GMsgView::S2C_CritterProto);
    uint32_t id = 0;   // critter id
    uint16_t pid = 0;  // prototype index (key-sorted protos.json)
    void Write(ByteBuffer& b) const { b.WriteU32(id); b.WriteU16(pid); }
    bool Read(ByteBuffer& b) { id = b.ReadU32(); pid = b.ReadU16(); return b.Ok(); }
};
}  // namespace gnet
}  // namespace f3

// ===================================================================================================
// DIALOGUE BLOCK. Ids: client->server 30..32, server->client 136..138.
// Talk: the player asks to talk to an NPC (critter with a dialogue script); the server walks the player next to the NPC,
// runs the original conversation graph (.scra) and answers with DialogOpen, then DialogNode for every node shown. The
// client answers with DialogChoose(index into the last DialogNode.replies); DialogClose ends the conversation from either
// side. `text` fields are final English strings (stf ids resolved, {cNNN} colour tags stripped). `textId`/`replyId` carry
// the original string ids for clients that want to localise. Old clients ignore the server messages.
// ===================================================================================================
namespace f3 {
enum class GMsgDlg : uint8_t {
    C2S_Talk = 30,
    C2S_DialogChoose = 31,
    C2S_DialogClose = 32,
    S2C_DialogOpen = 136,
    S2C_DialogNode = 137,
    S2C_DialogClose = 138,
};
namespace gnet {
struct Talk {
    static constexpr GMsg kId = static_cast<GMsg>(GMsgDlg::C2S_Talk);
    uint32_t critter = 0;
    void Write(ByteBuffer& b) const { b.WriteU32(critter); }
    bool Read(ByteBuffer& b) { critter = b.ReadU32(); return b.Ok(); }
};
struct DialogChoose {
    static constexpr GMsg kId = static_cast<GMsg>(GMsgDlg::C2S_DialogChoose);
    uint8_t index = 0;
    void Write(ByteBuffer& b) const { b.WriteU8(index); }
    bool Read(ByteBuffer& b) { index = b.ReadU8(); return b.Ok(); }
};
struct DialogCloseReq {
    static constexpr GMsg kId = static_cast<GMsg>(GMsgDlg::C2S_DialogClose);
    void Write(ByteBuffer&) const {}
    bool Read(ByteBuffer& b) { return b.Ok(); }
};
struct DialogOpen {
    static constexpr GMsg kId = static_cast<GMsg>(GMsgDlg::S2C_DialogOpen);
    uint32_t npc = 0;   // critter id
    std::string name;   // speaker name
    void Write(ByteBuffer& b) const { b.WriteU32(npc); b.WriteString(name); }
    bool Read(ByteBuffer& b) { npc = b.ReadU32(); name = b.ReadString(64); return b.Ok(); }
};
struct DialogNode {
    static constexpr GMsg kId = static_cast<GMsg>(GMsgDlg::S2C_DialogNode);
    struct Reply { uint32_t replyId = 0; std::string text; };
    uint32_t npc = 0;
    uint32_t textId = 0;
    std::string text;
    std::vector<Reply> replies;
    void Write(ByteBuffer& b) const {
        b.WriteU32(npc); b.WriteU32(textId); b.WriteString(text);
        b.WriteU8(static_cast<uint8_t>(replies.size()));
        for (auto& r : replies) { b.WriteU32(r.replyId); b.WriteString(r.text); }
    }
    bool Read(ByteBuffer& b) {
        npc = b.ReadU32(); textId = b.ReadU32(); text = b.ReadString(2048);
        uint8_t n = b.ReadU8();
        replies.clear();
        for (uint8_t i = 0; i < n && b.Ok(); ++i) { Reply r; r.replyId = b.ReadU32(); r.text = b.ReadString(1024); replies.push_back(r); }
        return b.Ok();
    }
};
struct DialogClose {
    static constexpr GMsg kId = static_cast<GMsg>(GMsgDlg::S2C_DialogClose);
    void Write(ByteBuffer&) const {}
    bool Read(ByteBuffer& b) { return b.Ok(); }
};
}  // namespace gnet
}  // namespace f3
