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
    void Write(ByteBuffer& b) const { b.WriteU32(id); b.WriteU8(on); }
    bool Read(ByteBuffer& b) { id = b.ReadU32(); on = b.ReadU8(); return b.Ok(); }
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

struct InvEntry {
    uint32_t id = 0;
    uint16_t pid = 0, count = 1;
    uint8_t equipped = 0;
    std::string name;
    void Write(ByteBuffer& b) const { b.WriteU32(id); b.WriteU16(pid); b.WriteU16(count); b.WriteU8(equipped); b.WriteString(name); }
    bool Read(ByteBuffer& b) { id = b.ReadU32(); pid = b.ReadU16(); count = b.ReadU16(); equipped = b.ReadU8(); name = b.ReadString(64); return b.Ok(); }
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
    void Write(ByteBuffer& b) const { b.WriteU32(attacker); b.WriteU32(target); b.WriteU8(static_cast<uint8_t>(result)); b.WriteI16(damage); b.WriteI16(targetHp); }
    bool Read(ByteBuffer& b) { attacker = b.ReadU32(); target = b.ReadU32(); result = static_cast<HitResult>(b.ReadU8()); damage = b.ReadI16(); targetHp = b.ReadI16(); return b.Ok(); }
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
