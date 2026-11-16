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
    C2S_CloseObj = 40, // map object id: the actor closes the container it was looking into (loot window closed)
    C2S_Loot = 38,     // dead critter id (corpse): walk next to it, then open its container window (S2C_Container)
    C2S_Put = 39,      // container / corpse id + isCritter + own inventory entry id: move an item into it
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
    uint8_t isCritter = 0;   // loot block: 1 = `container` is a corpse (critter id); older clients omit the byte
    void Write(ByteBuffer& b) const { b.WriteU32(container); b.WriteU32(entry); b.WriteU8(isCritter); }
    bool Read(ByteBuffer& b) { container = b.ReadU32(); entry = b.ReadU32(); isCritter = b.Remaining() ? b.ReadU8() : 0; return b.Ok(); }
};
// Loot block: open a corpse (C2S_Loot) / move one of the player's own items into a container or corpse (C2S_Put).
struct Loot {
    static constexpr GMsg kId = GMsg::C2S_Loot;
    uint32_t id = 0;
    void Write(ByteBuffer& b) const { b.WriteU32(id); }
    bool Read(ByteBuffer& b) { id = b.ReadU32(); return b.Ok(); }
};
struct Put {
    static constexpr GMsg kId = GMsg::C2S_Put;
    uint32_t container = 0, entry = 0;
    uint8_t isCritter = 0;
    void Write(ByteBuffer& b) const { b.WriteU32(container); b.WriteU32(entry); b.WriteU8(isCritter); }
    bool Read(ByteBuffer& b) { container = b.ReadU32(); entry = b.ReadU32(); isCritter = b.ReadU8(); return b.Ok(); }
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
// Contents of a container object or a corpse. isCritter = 1: `owner` is a critter id (corpse); name = window title.
struct Container : ItemList {
    static constexpr GMsg kId = GMsg::S2C_Container;
    uint8_t isCritter = 0;
    std::string name;
    void Write(ByteBuffer& b) const { ItemList::Write(b); b.WriteU8(isCritter); b.WriteString(name); }
    bool Read(ByteBuffer& b) {
        if (!ItemList::Read(b)) return false;
        isCritter = b.Remaining() ? b.ReadU8() : 0;
        name = b.Remaining() ? b.ReadString(64) : std::string();
        return b.Ok();
    }
};

enum class ObjectKind : uint8_t { Ground = 0, Door = 1, Container = 2, Usable = 3 };

// Sent after ItemAdd for objects that are not plain ground items, and whenever the state changes.
// Lock block (backwards compatible: older clients stop after `open`, older servers do not send the tail):
// locked = 1 while the door / container is locked, flags = LockFlag bits (types.h). State names: Open (open), Closed (!open, !locked), Locked.
struct ObjectInfo {
    static constexpr GMsg kId = GMsg::S2C_ObjectInfo;
    uint32_t id = 0;
    ObjectKind kind = ObjectKind::Ground;
    uint8_t open = 0;  // doors / containers: 1 = open
    uint8_t locked = 0;
    uint8_t flags = 0;
    void Write(ByteBuffer& b) const { b.WriteU32(id); b.WriteU8(static_cast<uint8_t>(kind)); b.WriteU8(open); b.WriteU8(locked); b.WriteU8(flags); }
    bool Read(ByteBuffer& b) {
        id = b.ReadU32(); kind = static_cast<ObjectKind>(b.ReadU8()); open = b.ReadU8();
        locked = b.Remaining() >= 2 ? b.ReadU8() : 0;
        flags = b.Remaining() >= 1 ? b.ReadU8() : 0;
        return b.Ok();
    }
};
struct CloseObj {
    static constexpr GMsg kId = GMsg::C2S_CloseObj;
    uint32_t id = 0;
    void Write(ByteBuffer& b) const { b.WriteU32(id); }
    bool Read(ByteBuffer& b) { id = b.ReadU32(); return b.Ok(); }
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
    uint8_t appear = 0;   // 1 = sent while the critter enters the view: a dead critter is shown in its final pose (no death animation)
    void Write(ByteBuffer& b) const { b.WriteU32(id); b.WriteI16(hp); b.WriteI16(maxHp); b.WriteU8(dead); b.WriteU8(appear); }
    bool Read(ByteBuffer& b) { id = b.ReadU32(); hp = b.ReadI16(); maxHp = b.ReadI16(); dead = b.ReadU8(); appear = b.Remaining() ? b.ReadU8() : 0; return b.Ok(); }
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

// ===================================================================================================
// ACCOUNT BLOCK (registration / password login). Ids: client->server 33, server->client 140.
// Auth is the first message of a session: mode 0 = login, 1 = register (creates the account and its first character, then
// logs in). Passwords travel in clear text inside the (unencrypted) ENet packet: run behind a VPN/TLS tunnel for real
// deployments; the server never stores them (salted PBKDF2-HMAC-SHA256). The result is always an AuthResult; on code 0
// LoginOk + MapInfo + ... follow exactly like the legacy name-only Login, on failure the server closes the connection.
// The legacy C2S_Login (name only, no persistence) is accepted only when the server runs with --allow-guest (or --dev-kit).
// ===================================================================================================
namespace f3 {
enum class GMsgAuth : uint8_t { C2S_Auth = 33, S2C_AuthResult = 140 };
namespace gnet {
enum class AuthMode : uint8_t { Login = 0, Register = 1 };
enum class AuthCode : uint8_t {
    Ok = 0, BadCredentials = 1, NameTaken = 2, InvalidName = 3, WeakPassword = 4, RateLimited = 5,
    GuestDisabled = 6, VersionMismatch = 7, ServerError = 8, Full = 9
};
inline const char* AuthCodeText(AuthCode c) {
    switch (c) {
        case AuthCode::Ok: return "ok";
        case AuthCode::BadCredentials: return "wrong name or password";
        case AuthCode::NameTaken: return "that name is already registered";
        case AuthCode::InvalidName: return "invalid name (2-16 letters, digits or _)";
        case AuthCode::WeakPassword: return "password must be 4-64 characters";
        case AuthCode::RateLimited: return "too many failed attempts, try again later";
        case AuthCode::GuestDisabled: return "this server requires an account (name + password)";
        case AuthCode::VersionMismatch: return "protocol version mismatch";
        case AuthCode::Full: return "server is full";
        default: return "server error";
    }
}
// Locale key of an AuthCode text (data/locale/<lang>/messages.json); AuthCodeText above is the English fallback.
inline const char* AuthCodeKey(AuthCode c) {
    switch (c) {
        case AuthCode::Ok: return "auth.ok";
        case AuthCode::BadCredentials: return "auth.bad_credentials";
        case AuthCode::NameTaken: return "auth.name_taken";
        case AuthCode::InvalidName: return "auth.invalid_name";
        case AuthCode::WeakPassword: return "auth.weak_password";
        case AuthCode::RateLimited: return "auth.rate_limited";
        case AuthCode::GuestDisabled: return "auth.guest_disabled";
        case AuthCode::VersionMismatch: return "auth.version_mismatch";
        case AuthCode::Full: return "auth.full";
        default: return "auth.server_error";
    }
}
struct Auth {
    static constexpr GMsg kId = static_cast<GMsg>(GMsgAuth::C2S_Auth);
    uint32_t version = kProtocolVersion;
    uint8_t mode = 0;
    std::string name, password;
    std::string lang;   // optional trailing field: the client's UI language code ("en", "ru"); the server localizes its messages with it
    void Write(ByteBuffer& b) const { b.WriteU32(version); b.WriteU8(mode); b.WriteString(name); b.WriteString(password); if (!lang.empty()) b.WriteString(lang); }
    bool Read(ByteBuffer& b) { version = b.ReadU32(); mode = b.ReadU8(); name = b.ReadString(64); password = b.ReadString(128); if (b.Ok() && b.Remaining() >= 2) lang = b.ReadString(8); return b.Ok(); }
};
struct AuthResult {
    static constexpr GMsg kId = static_cast<GMsg>(GMsgAuth::S2C_AuthResult);
    uint8_t code = 0;
    uint8_t isNew = 0;   // 1 = the character was just created (register)
    std::string text;
    void Write(ByteBuffer& b) const { b.WriteU8(code); b.WriteU8(isNew); b.WriteString(text); }
    bool Read(ByteBuffer& b) { code = b.ReadU8(); isNew = b.ReadU8(); text = b.ReadString(256); return b.Ok(); }
};
}  // namespace gnet
}  // namespace f3

// ===================================================================================================
// CHARACTER BLOCK (SPECIAL allocation, skills, XP / levels, skill use). Ids: client->server 34, 35, 37; server->client 141.
// A new account character starts with SPECIAL 5 in everything, `unspentSpecial` free points and created = 0 and is NOT in the world:
// after the login the server sends only LoginOk, Stats and CharSheet (no MapInfo, no critters) and keeps the session in a "creating"
// state (no map, invisible, no ticks) until the client answers with a valid CharCreate{special, tagMask, traitMask}; only then the
// character is placed on its map (MapInfo, critters, inventory follow). Until then the server ignores everything except
// ping / ReqState / ClientCaps / CharCreate.
// CharSheet carries everything the character window shows and is sent after login, ReqState and every change (level up,
// skill spend, creation). Skill ids are the order of kSkillNames (the Van Buren skill list of English.stf, ids 83..96).
// ===================================================================================================
namespace f3 {
enum class GMsgChar : uint8_t { C2S_CharCreate = 34, C2S_SpendSkill = 35, C2S_UseSkill = 37, S2C_CharSheet = 141 };
inline constexpr int kSkillCount = 14;
inline const char* const kSkillNames[kSkillCount] = {"Firearms", "Melee", "Unarmed", "Barter", "Persuasion", "Deception", "Mechanics",
                                                     "Medic", "Outdoorsman", "Science", "Security", "Sneak", "Steal", "Traps"};
// Skills that can be used actively on a target (the Fallout 1/2 skilldex set): Repair (Mechanics), Doctor/First Aid (Medic), Science,
// Lockpick (Security), Sneak, Steal, Traps. The others (weapon skills, Barter, Persuasion, Deception, Outdoorsman) are passive.
inline constexpr int kUsableSkills[] = {7 /*Medic*/, 9 /*Science*/, 6 /*Mechanics*/, 10 /*Security*/, 11 /*Sneak*/, 12 /*Steal*/, 13 /*Traps*/};
inline constexpr int kUsableSkillCount = static_cast<int>(sizeof(kUsableSkills) / sizeof(kUsableSkills[0]));
inline bool IsUsableSkill(int s) { for (int u : kUsableSkills) if (u == s) return true; return false; }
namespace gnet {
struct CharCreate {
    static constexpr GMsg kId = static_cast<GMsg>(GMsgChar::C2S_CharCreate);
    uint8_t special[7] = {5, 5, 5, 5, 5, 5, 5};
    uint16_t tagMask = 0;   // bit i = kSkillNames[i] is a tag skill (exactly 3)
    uint16_t traitMask = 0; // bit i = optional trait i (original trait list, English.stf 159..174), at most `traitMax` (2) bits
    void Write(ByteBuffer& b) const { for (int i = 0; i < 7; i++) b.WriteU8(special[i]); b.WriteU16(tagMask); b.WriteU16(traitMask); }
    bool Read(ByteBuffer& b) { for (int i = 0; i < 7; i++) special[i] = b.ReadU8(); tagMask = b.ReadU16(); traitMask = b.ReadU16(); return b.Ok(); }
};
struct SpendSkill {
    static constexpr GMsg kId = static_cast<GMsg>(GMsgChar::C2S_SpendSkill);
    uint8_t skill = 0, points = 1;
    void Write(ByteBuffer& b) const { b.WriteU8(skill); b.WriteU8(points); }
    bool Read(ByteBuffer& b) { skill = b.ReadU8(); points = b.ReadU8(); return b.Ok(); }
};
struct UseSkill {
    static constexpr GMsg kId = static_cast<GMsg>(GMsgChar::C2S_UseSkill);
    uint8_t skill = 0;
    uint8_t kind = 0;    // 0 = no target / self, 1 = critter, 2 = map object
    uint32_t target = 0;
    void Write(ByteBuffer& b) const { b.WriteU8(skill); b.WriteU8(kind); b.WriteU32(target); }
    bool Read(ByteBuffer& b) { skill = b.ReadU8(); kind = b.ReadU8(); target = b.ReadU32(); return b.Ok(); }
};
struct CharSheet {
    static constexpr GMsg kId = static_cast<GMsg>(GMsgChar::S2C_CharSheet);
    uint8_t created = 1, level = 1, perkPoints = 0, unspentSpecial = 0, creationTotal = 40;   // creationTotal = SPECIAL sum to reach
    uint32_t xp = 0, xpNext = 0;   // xpNext = total xp for the next level (0 at the level cap)
    uint16_t skillPoints = 0, tagMask = 0;
    uint8_t special[7] = {5, 5, 5, 5, 5, 5, 5};
    uint16_t skill[kSkillCount] = {};   // total value (base from SPECIAL + tag bonus + spent ranks)
    uint8_t rank[kSkillCount] = {};     // points spent
    uint8_t maxAp = 0, healRate = 0, critChance = 0, skillRate = 0, tagCount = 3;
    uint16_t traitMask = 0;
    uint8_t traitMax = 2;
    void Write(ByteBuffer& b) const {
        b.WriteU8(created); b.WriteU8(level); b.WriteU8(perkPoints); b.WriteU8(unspentSpecial); b.WriteU8(creationTotal);
        b.WriteU32(xp); b.WriteU32(xpNext); b.WriteU16(skillPoints); b.WriteU16(tagMask);
        for (int i = 0; i < 7; i++) b.WriteU8(special[i]);
        for (int i = 0; i < kSkillCount; i++) { b.WriteU16(skill[i]); b.WriteU8(rank[i]); }
        b.WriteU8(maxAp); b.WriteU8(healRate); b.WriteU8(critChance); b.WriteU8(skillRate); b.WriteU8(tagCount);
        b.WriteU16(traitMask); b.WriteU8(traitMax);
    }
    bool Read(ByteBuffer& b) {
        created = b.ReadU8(); level = b.ReadU8(); perkPoints = b.ReadU8(); unspentSpecial = b.ReadU8(); creationTotal = b.ReadU8();
        xp = b.ReadU32(); xpNext = b.ReadU32(); skillPoints = b.ReadU16(); tagMask = b.ReadU16();
        for (int i = 0; i < 7; i++) special[i] = b.ReadU8();
        for (int i = 0; i < kSkillCount; i++) { skill[i] = b.ReadU16(); rank[i] = b.ReadU8(); }
        maxAp = b.ReadU8(); healRate = b.ReadU8(); critChance = b.ReadU8(); skillRate = b.ReadU8(); tagCount = b.ReadU8();
        traitMask = b.ReadU16(); traitMax = b.ReadU8();
        return b.Ok();
    }
};
}  // namespace gnet
// ---- Pip-Boy block: quest log, friends list and mail. One request message (C2S_Pip = 36, the last free id of the 20..39 client range)
// carries an operation; the answer to every operation (and the unsolicited update after a quest / friend / mail change or a friend
// logging in or out) is the complete S2C_Pip snapshot, which is small (<= 50 mails of <= 400 chars). Older clients never send or
// receive these ids.
enum class GMsgPip : uint8_t { C2S_Pip = 36, S2C_Pip = 142 };
inline constexpr size_t kMaxMailText = 400;
inline constexpr size_t kMaxMailbox = 50;
inline constexpr size_t kMaxFriends = 50;
namespace gnet {
enum class PipOp : uint8_t { Request = 0, FriendAdd = 1, FriendDel = 2, MailSend = 3, MailRead = 4, MailDelete = 5 };
struct PipReq {
    static constexpr GMsg kId = static_cast<GMsg>(GMsgPip::C2S_Pip);
    uint8_t op = 0;
    uint32_t num = 0;     // MailRead / MailDelete: mail id
    std::string a, b;     // FriendAdd / FriendDel: a = name; MailSend: a = recipient, b = text
    void Write(ByteBuffer& w) const { w.WriteU8(op); w.WriteU32(num); w.WriteString(a); w.WriteString(b); }
    bool Read(ByteBuffer& r) { op = r.ReadU8(); num = r.ReadU32(); a = r.ReadString(64); b = r.ReadString(kMaxMailText * 4); return r.Ok(); }
};
struct PipData {
    static constexpr GMsg kId = static_cast<GMsg>(GMsgPip::S2C_Pip);
    struct Quest { std::string id, title, text; uint8_t state = 0; };   // state 0 active, 1 completed, 2 failed
    struct Friend { std::string name; uint8_t online = 0; };
    struct Mail { uint32_t id = 0; std::string from, text; uint32_t time = 0; uint8_t read = 0; };
    std::vector<Quest> quests;
    std::vector<Friend> friends;
    std::vector<Mail> mail;
    void Write(ByteBuffer& w) const {
        w.WriteU16(static_cast<uint16_t>(quests.size()));
        for (const Quest& q : quests) { w.WriteString(q.id); w.WriteString(q.title); w.WriteString(q.text); w.WriteU8(q.state); }
        w.WriteU16(static_cast<uint16_t>(friends.size()));
        for (const Friend& f : friends) { w.WriteString(f.name); w.WriteU8(f.online); }
        w.WriteU16(static_cast<uint16_t>(mail.size()));
        for (const Mail& m : mail) { w.WriteU32(m.id); w.WriteString(m.from); w.WriteString(m.text); w.WriteU32(m.time); w.WriteU8(m.read); }
    }
    bool Read(ByteBuffer& r) {
        quests.clear(); friends.clear(); mail.clear();
        uint16_t n = r.ReadU16();
        for (uint16_t i = 0; i < n && r.Ok(); i++) { Quest q; q.id = r.ReadString(64); q.title = r.ReadString(200); q.text = r.ReadString(2000); q.state = r.ReadU8(); quests.push_back(std::move(q)); }
        n = r.ReadU16();
        for (uint16_t i = 0; i < n && r.Ok(); i++) { Friend f; f.name = r.ReadString(64); f.online = r.ReadU8(); friends.push_back(std::move(f)); }
        n = r.ReadU16();
        for (uint16_t i = 0; i < n && r.Ok(); i++) { Mail m; m.id = r.ReadU32(); m.from = r.ReadString(64); m.text = r.ReadString(kMaxMailText * 4); m.time = r.ReadU32(); m.read = r.ReadU8(); mail.push_back(std::move(m)); }
        return r.Ok();
    }
};
}  // namespace gnet

}  // namespace f3

// ---- Content block: designer data (data/*.json, docs/data-format.md) the client needs for display. S2C only (id 130, free in the
// 120..139 range); old clients ignore it, old servers never send it (a new client then keeps its built-in fallbacks: the original
// English.stf trait names, the prototype table of gamedata/server). Sent after login (also while the session waits in character
// creation) and again to every player after a data reload. A message with first = 1 replaces what the client knew (traits, perks);
// the item list is split over several messages (first = 0 for the following ones).
namespace f3 {
enum class GMsgContent : uint8_t { S2C_ContentDefs = 130 };
namespace gnet {
struct ContentDefs {
    static constexpr GMsg kId = static_cast<GMsg>(GMsgContent::S2C_ContentDefs);
    struct Trait { uint8_t index = 0; std::string id, name, desc; };               // index = bit of the trait mask / creation screen slot
    struct Perk { std::string id, name, desc; uint8_t ranks = 1, reqLevel = 0; };
    // A prototype overridden or added by the data layer. basePid = 0xFFFF: an override of the prototype `pid`; else a NEW prototype
    // (pid = index after the original ones) that the client clones from basePid (model, icon, ...) and then applies name / desc / numbers to.
    struct Item { uint16_t pid = 0, basePid = 0xFFFF; std::string key, name, desc; uint16_t range = 0; float dmgMin = 0, dmgMax = 0; uint16_t armor[7] = {}; };
    uint8_t first = 1;
    uint16_t schema = 1;
    uint32_t revision = 0;
    uint8_t traitMax = 2;
    std::vector<Trait> traits;
    std::vector<Perk> perks;
    std::vector<Item> items;
    void Write(ByteBuffer& b) const {
        b.WriteU8(first); b.WriteU16(schema); b.WriteU32(revision); b.WriteU8(traitMax);
        b.WriteU16(static_cast<uint16_t>(traits.size()));
        for (const Trait& t : traits) { b.WriteU8(t.index); b.WriteString(t.id); b.WriteString(t.name); b.WriteString(t.desc); }
        b.WriteU16(static_cast<uint16_t>(perks.size()));
        for (const Perk& p : perks) { b.WriteString(p.id); b.WriteString(p.name); b.WriteString(p.desc); b.WriteU8(p.ranks); b.WriteU8(p.reqLevel); }
        b.WriteU16(static_cast<uint16_t>(items.size()));
        for (const Item& i : items) {
            b.WriteU16(i.pid); b.WriteU16(i.basePid); b.WriteString(i.key); b.WriteString(i.name); b.WriteString(i.desc);
            b.WriteU16(i.range); WriteF32(b, i.dmgMin); WriteF32(b, i.dmgMax);
            for (int k = 0; k < 7; k++) b.WriteU16(i.armor[k]);
        }
    }
    bool Read(ByteBuffer& b) {
        traits.clear(); perks.clear(); items.clear();
        first = b.ReadU8(); schema = b.ReadU16(); revision = b.ReadU32(); traitMax = b.ReadU8();
        uint16_t n = b.ReadU16();
        for (uint16_t k = 0; k < n && b.Ok(); k++) { Trait t; t.index = b.ReadU8(); t.id = b.ReadString(64); t.name = b.ReadString(200); t.desc = b.ReadString(2000); traits.push_back(std::move(t)); }
        n = b.ReadU16();
        for (uint16_t k = 0; k < n && b.Ok(); k++) { Perk p; p.id = b.ReadString(64); p.name = b.ReadString(200); p.desc = b.ReadString(2000); p.ranks = b.ReadU8(); p.reqLevel = b.ReadU8(); perks.push_back(std::move(p)); }
        n = b.ReadU16();
        for (uint16_t k = 0; k < n && b.Ok(); k++) {
            Item i;
            i.pid = b.ReadU16(); i.basePid = b.ReadU16(); i.key = b.ReadString(200); i.name = b.ReadString(200); i.desc = b.ReadString(4000);
            i.range = b.ReadU16(); i.dmgMin = ReadF32(b); i.dmgMax = ReadF32(b);
            for (int q = 0; q < 7; q++) i.armor[q] = b.ReadU16();
            items.push_back(std::move(i));
        }
        return b.Ok();
    }
};
}  // namespace gnet
}  // namespace f3
