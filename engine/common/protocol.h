#pragma once
// Wire protocol shared by server and clients. Every packet is: u8 message id, payload.
// All messages travel reliable on channel 0 in the MVP.
#include <cstdint>
#include <string>
#include <vector>
#include "buffer.h"
#include "types.h"

namespace f3 {

inline constexpr uint32_t kProtocolVersion = 1;
inline constexpr uint16_t kDefaultPort = 4040;
inline constexpr int kDefaultTickRate = 20;
inline constexpr int kViewRadius = 24;  // tiles; per-map visibility range
inline constexpr size_t kMaxNameLen = 16;
inline constexpr size_t kMaxChatLen = 200;
inline constexpr size_t kMaxPacketSize = 64 * 1024;

enum class Msg : uint8_t {
    // client -> server
    C2S_Login = 1,
    C2S_MoveTo = 2,
    C2S_Chat = 3,
    C2S_Ping = 4,
    // server -> client
    S2C_LoginOk = 100,
    S2C_LoginFail = 101,
    S2C_MapInfo = 102,
    S2C_CritterAdd = 103,
    S2C_CritterRemove = 104,
    S2C_CritterMove = 105,
    S2C_Chat = 106,
    S2C_ItemAdd = 107,
    S2C_ItemRemove = 108,
    S2C_Pong = 109,
};

enum class ChatChannel : uint8_t { Say = 0, System = 1 };

namespace net {

struct LoginReq {
    static constexpr Msg kId = Msg::C2S_Login;
    uint32_t version = kProtocolVersion;
    std::string name;
    void Write(ByteBuffer& b) const { b.WriteU32(version); b.WriteString(name); }
    bool Read(ByteBuffer& b) { version = b.ReadU32(); name = b.ReadString(64); return b.Ok(); }
};

struct MoveToReq {
    static constexpr Msg kId = Msg::C2S_MoveTo;
    int16_t x = 0, y = 0;
    void Write(ByteBuffer& b) const { b.WriteI16(x); b.WriteI16(y); }
    bool Read(ByteBuffer& b) { x = b.ReadI16(); y = b.ReadI16(); return b.Ok(); }
};

struct ChatReq {
    static constexpr Msg kId = Msg::C2S_Chat;
    std::string text;
    void Write(ByteBuffer& b) const { b.WriteString(text); }
    bool Read(ByteBuffer& b) { text = b.ReadString(1024); return b.Ok(); }
};

struct Ping {
    static constexpr Msg kId = Msg::C2S_Ping;
    uint32_t token = 0;
    void Write(ByteBuffer& b) const { b.WriteU32(token); }
    bool Read(ByteBuffer& b) { token = b.ReadU32(); return b.Ok(); }
};

struct Pong {
    static constexpr Msg kId = Msg::S2C_Pong;
    uint32_t token = 0;
    void Write(ByteBuffer& b) const { b.WriteU32(token); }
    bool Read(ByteBuffer& b) { token = b.ReadU32(); return b.Ok(); }
};

struct LoginOk {
    static constexpr Msg kId = Msg::S2C_LoginOk;
    CritterId critterId = 0;
    uint16_t tickRate = kDefaultTickRate;
    uint16_t moveTicks = 4;  // ticks per tile step; client uses it for interpolation
    void Write(ByteBuffer& b) const { b.WriteU32(critterId); b.WriteU16(tickRate); b.WriteU16(moveTicks); }
    bool Read(ByteBuffer& b) { critterId = b.ReadU32(); tickRate = b.ReadU16(); moveTicks = b.ReadU16(); return b.Ok(); }
};

struct LoginFail {
    static constexpr Msg kId = Msg::S2C_LoginFail;
    std::string reason;
    void Write(ByteBuffer& b) const { b.WriteString(reason); }
    bool Read(ByteBuffer& b) { reason = b.ReadString(256); return b.Ok(); }
};

struct MapInfo {
    static constexpr Msg kId = Msg::S2C_MapInfo;
    MapId mapId = 0;
    std::string name;
    uint16_t width = 0, height = 0;
    std::vector<uint8_t> blocked;  // width*height, 1 = blocked
    uint16_t viewCells = 0;        // optional trailing field: this player's visibility radius in cells (0 = unknown, the client draws no fog)
    void Write(ByteBuffer& b) const {
        b.WriteU16(mapId); b.WriteString(name); b.WriteU16(width); b.WriteU16(height);
        b.WriteBytes(blocked.data(), blocked.size());
        b.WriteU16(viewCells);
    }
    bool Read(ByteBuffer& b) {
        mapId = b.ReadU16(); name = b.ReadString(64); width = b.ReadU16(); height = b.ReadU16();
        size_t n = static_cast<size_t>(width) * height;
        if (!b.Ok() || n > (1u << 20) || n > b.Remaining()) return false;
        blocked.resize(n);
        if (!b.ReadBytes(blocked.data(), n) || !b.Ok()) return false;
        viewCells = b.Remaining() >= 2 ? b.ReadU16() : 0;
        return b.Ok();
    }
};

struct CritterAdd {
    static constexpr Msg kId = Msg::S2C_CritterAdd;
    CritterId id = 0;
    std::string name;
    CritterKind kind = CritterKind::Player;
    int16_t x = 0, y = 0;
    Dir dir = Dir::S;
    uint32_t color = 0xFFFFFFFF;  // 0xRRGGBBAA, placeholder until real models exist
    void Write(ByteBuffer& b) const {
        b.WriteU32(id); b.WriteString(name); b.WriteU8(static_cast<uint8_t>(kind));
        b.WriteI16(x); b.WriteI16(y); b.WriteU8(static_cast<uint8_t>(dir)); b.WriteU32(color);
    }
    bool Read(ByteBuffer& b) {
        id = b.ReadU32(); name = b.ReadString(64); kind = static_cast<CritterKind>(b.ReadU8());
        x = b.ReadI16(); y = b.ReadI16(); dir = static_cast<Dir>(b.ReadU8() & 7); color = b.ReadU32();
        return b.Ok();
    }
};

struct CritterRemove {
    static constexpr Msg kId = Msg::S2C_CritterRemove;
    CritterId id = 0;
    void Write(ByteBuffer& b) const { b.WriteU32(id); }
    bool Read(ByteBuffer& b) { id = b.ReadU32(); return b.Ok(); }
};

struct CritterMove {
    static constexpr Msg kId = Msg::S2C_CritterMove;
    CritterId id = 0;
    int16_t x = 0, y = 0;
    Dir dir = Dir::S;
    uint8_t snap = 0;  // 1 = teleport / resync, do not interpolate
    void Write(ByteBuffer& b) const { b.WriteU32(id); b.WriteI16(x); b.WriteI16(y); b.WriteU8(static_cast<uint8_t>(dir)); b.WriteU8(snap); }
    bool Read(ByteBuffer& b) { id = b.ReadU32(); x = b.ReadI16(); y = b.ReadI16(); dir = static_cast<Dir>(b.ReadU8() & 7); snap = b.ReadU8(); return b.Ok(); }
};

struct ChatMsg {
    static constexpr Msg kId = Msg::S2C_Chat;
    CritterId from = 0;  // 0 = system
    std::string name;
    std::string text;
    ChatChannel channel = ChatChannel::Say;
    void Write(ByteBuffer& b) const { b.WriteU32(from); b.WriteString(name); b.WriteString(text); b.WriteU8(static_cast<uint8_t>(channel)); }
    bool Read(ByteBuffer& b) { from = b.ReadU32(); name = b.ReadString(64); text = b.ReadString(1024); channel = static_cast<ChatChannel>(b.ReadU8()); return b.Ok(); }
};

struct ItemAdd {
    static constexpr Msg kId = Msg::S2C_ItemAdd;
    ItemId id = 0;
    uint16_t pid = 0;  // prototype id
    std::string name;
    int16_t x = 0, y = 0;
    void Write(ByteBuffer& b) const { b.WriteU32(id); b.WriteU16(pid); b.WriteString(name); b.WriteI16(x); b.WriteI16(y); }
    bool Read(ByteBuffer& b) { id = b.ReadU32(); pid = b.ReadU16(); name = b.ReadString(64); x = b.ReadI16(); y = b.ReadI16(); return b.Ok(); }
};

struct ItemRemove {
    static constexpr Msg kId = Msg::S2C_ItemRemove;
    ItemId id = 0;
    void Write(ByteBuffer& b) const { b.WriteU32(id); }
    bool Read(ByteBuffer& b) { id = b.ReadU32(); return b.Ok(); }
};

template <class T>
ByteBuffer Pack(const T& m) {
    ByteBuffer b;
    b.WriteU8(static_cast<uint8_t>(T::kId));
    m.Write(b);
    return b;
}

}  // namespace net
}  // namespace f3
