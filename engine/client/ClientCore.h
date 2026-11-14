#pragma once
// Rendering-independent client logic: connection, protocol handling, replicated world state.
// No raylib and no ENet headers leak out of this header.
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>
#include "common/protocol.h"

struct _ENetHost;
struct _ENetPeer;

namespace f3 {

struct ClientCritter {
    CritterId id = 0;
    std::string name;
    CritterKind kind = CritterKind::Player;
    uint32_t color = 0xFFFFFFFF;
    Dir dir = Dir::S;
    TilePos pos;        // authoritative tile
    TilePos from;       // tile we are interpolating from
    double moveStart = 0;
    double moveDur = 0; // 0 = not interpolating
};

struct ClientItem {
    ItemId id = 0;
    uint16_t pid = 0;
    std::string name;
    TilePos pos;
};

struct ClientMap {
    MapId id = 0;
    std::string name;
    int width = 0, height = 0;
    std::vector<uint8_t> blocked;
    bool IsBlocked(int x, int y) const {
        return x < 0 || y < 0 || x >= width || y >= height || blocked[static_cast<size_t>(y) * width + x] != 0;
    }
};

struct ChatLine {
    std::string name;  // empty for system
    std::string text;
    ChatChannel channel = ChatChannel::Say;
    double time = 0;
};

class ClientCore {
public:
    enum class State { Disconnected, Connecting, LoggingIn, InGame, Failed };

    ClientCore();
    ~ClientCore();

    // Starts connecting; call Poll() until state() leaves Connecting/LoggingIn.
    bool Connect(const std::string& host, uint16_t port, const std::string& name);
    void Disconnect();
    void Poll();  // pumps the network, never blocks

    void RequestMove(TilePos target);
    void SendChat(const std::string& text);
    void SendPing();

    State state() const { return state_; }
    const std::string& failReason() const { return failReason_; }
    CritterId selfId() const { return selfId_; }
    const ClientCritter* self() const { return Find(selfId_); }
    const ClientCritter* Find(CritterId id) const;
    const ClientMap& map() const { return map_; }
    bool hasMap() const { return map_.width > 0; }
    const std::unordered_map<CritterId, ClientCritter>& critters() const { return critters_; }
    const std::unordered_map<ItemId, ClientItem>& items() const { return items_; }
    const std::deque<ChatLine>& chat() const { return chat_; }
    uint64_t chatCounter() const { return chatCounter_; }  // total lines ever received
    int lastPongToken() const { return static_cast<int>(lastPong_); }

    // Interpolated position in (fractional) tile units.
    void RenderPos(const ClientCritter& c, float& x, float& y) const;
    double Now() const;

private:
    void Send(const ByteBuffer& b);
    void Handle(const uint8_t* data, size_t len);
    void AddChat(ChatLine l);

    _ENetHost* host_ = nullptr;
    _ENetPeer* peer_ = nullptr;
    State state_ = State::Disconnected;
    std::string failReason_;
    std::string name_;
    CritterId selfId_ = 0;
    double moveDur_ = 0.2;
    ClientMap map_;
    std::unordered_map<CritterId, ClientCritter> critters_;
    std::unordered_map<ItemId, ClientItem> items_;
    std::deque<ChatLine> chat_;
    uint64_t chatCounter_ = 0;
    uint32_t lastPong_ = 0;
};

}  // namespace f3
