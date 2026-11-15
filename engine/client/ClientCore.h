#pragma once
// Rendering-independent client logic: connection, protocol handling, replicated world state.
// No raylib and no ENet headers leak out of this header.
//
// Movement model: every critter has a timed polyline ("track", tile units, tile centre = integer coordinates) that the
// renderer samples at the current time, so characters glide continuously instead of stepping from tile to tile.
//   * Path-based server (MovePath/CritterStop): the polyline comes straight from the server with exact timing.
//   * Legacy server (CritterMove per tile): consecutive moves are chained into one polyline, one step duration apart.
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "ProtoDb.h"
#include "common/protocol.h"
#include "common/protocol_game.h"

struct _ENetHost;
struct _ENetPeer;

namespace f3 {

struct TrackKey { float x = 0, y = 0; double t = 0; };

struct ClientCritter {
    CritterId id = 0;
    std::string name;
    CritterKind kind = CritterKind::Player;
    uint32_t color = 0xFFFFFFFF;
    Dir dir = Dir::S;   // facing when standing still
    TilePos pos;        // last authoritative tile
    int pid = -1;       // prototype index (protos.json) or -1
    bool dead = false;
    int16_t hp = -1, maxHp = -1;  // -1 unknown
    bool run = false;   // current path is a run
    // timed polyline; empty = stand at `pos`
    std::vector<TrackKey> track;
    float offX = 0, offY = 0; double offT = -10;  // visual correction after a path replace, decays over kBlendTime
    // combat presentation timestamps (client clock, -1 = never)
    double attackT = -1, hitT = -1, deathT = -1;
    CritterId attackTarget = 0;
};

struct CritterPose {
    float x = 0, y = 0;          // tile units
    bool moving = false;
    float speed = 0;             // tiles per second along the current segment
    float hx = 0, hy = 1;        // heading (unit, tile axes) while moving
};

struct ClientItem {
    ItemId id = 0;
    uint16_t pid = 0;
    std::string name;
    TilePos pos;
    gnet::ObjectKind kind = gnet::ObjectKind::Ground;
    bool open = false;
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

struct Floater {  // floating combat text
    CritterId target = 0;
    std::string text;
    int kind = 0;  // 0 damage, 1 miss, 2 critical, 3 kill
    double time = 0;
};

class ClientCore {
public:
    enum class State { Disconnected, Connecting, LoggingIn, InGame, Failed };
    static constexpr double kBlendTime = 0.2;

    ClientCore();
    ~ClientCore();

    // Starts connecting; call Poll() until state() leaves Connecting/LoggingIn.
    bool Connect(const std::string& host, uint16_t port, const std::string& name);
    void Disconnect();
    void Poll();  // pumps the network, never blocks

    // ---- requests -----------------------------------------------------------------------------------------
    void RequestMove(TilePos target);                       // legacy tile move
    // Precise destination in tile units (tile centre = integer). Uses MoveToF when the server speaks the timing protocol.
    void RequestMoveTo(float tx, float ty, bool run);
    void SendChat(const std::string& text);
    void SendPing();
    void PickUp(ItemId id);
    void Drop(uint32_t invId);
    void Equip(uint32_t invId, bool on, uint32_t slot = 0);
    void Use(uint32_t objectId);
    void Attack(CritterId id);   // 0 = stop
    void Take(uint32_t container, uint32_t entry);   // entry 0 = everything
    void Examine(uint32_t id, bool isCritter);
    // ---- dialogue (protocol_game.h dialogue block)
    struct DialogState { bool open = false; uint32_t npc = 0; std::string npcName, text; std::vector<std::string> replies; };
    void Talk(uint32_t critter);          // walk to the NPC and start its conversation
    void DialogChoose(int index);         // answer with reply `index` of the current node
    void DialogClose();                   // leave the conversation
    const DialogState& dialog() const { return dialog_; }
    void RequestState();
    void SetLegacyOnly(bool v) { legacyOnly_ = v; }   // do not announce the timing capability (tests the per-cell CritterMove path)

    // ---- replicated state ---------------------------------------------------------------------------------
    State state() const { return state_; }
    const std::string& failReason() const { return failReason_; }
    CritterId selfId() const { return selfId_; }
    const ClientCritter* self() const { return Find(selfId_); }
    const ClientCritter* Find(CritterId id) const;
    const ClientMap& map() const { return map_; }
    bool hasMap() const { return map_.width > 0; }
    uint64_t mapSerial() const { return mapSerial_; }   // increments on every MapInfo
    const std::unordered_map<CritterId, ClientCritter>& critters() const { return critters_; }
    const std::unordered_map<ItemId, ClientItem>& items() const { return items_; }
    const std::deque<ChatLine>& chat() const { return chat_; }
    uint64_t chatCounter() const { return chatCounter_; }  // total lines ever received
    int lastPongToken() const { return static_cast<int>(lastPong_); }
    const std::vector<Floater>& floaters() const { return floaters_; }

    bool hasStats() const { return hasStats_; }
    const gnet::Stats& stats() const { return stats_; }
    const std::vector<gnet::InvEntry>& inventory() const { return inventory_; }
    bool hasContainer() const { return containerOwner_ != 0; }
    uint32_t containerOwner() const { return containerOwner_; }
    const std::vector<gnet::InvEntry>& container() const { return container_; }
    void CloseContainer() { containerOwner_ = 0; container_.clear(); }

    // ---- real-time timing (FOnline-like wait / AP) --------------------------------------------------------
    bool serverTiming() const { return serverTiming_; }       // the server sent MovePath/WaitState/CritterStop at least once
    bool waitKnown() const { return wait_.valid; }
    bool IsBreak() const;                  // actions locked now
    double BreakRemaining() const;         // seconds until the lock ends (0 if free)
    double BreakTotal() const { return wait_.total; }   // length of the lock that is running (for the bar)
    float CurrentAp() const;               // whole AP (fractional)
    float MaxAp() const { return wait_.maxAp / 100.0f; }
    bool InBattle() const;
    gnet::ActionResult lastResult() const { return wait_.result; }
    double lastResultTime() const { return wait_.recvLocal; }
    uint8_t lastAction() const { return wait_.action; }   // request id the last WaitState answered

    // ---- sampling for the renderer -----------------------------------------------------------------------
    CritterPose Pose(const ClientCritter& c, double t) const;
    void RenderPos(const ClientCritter& c, float& x, float& y) const;   // Pose at Now()
    double Now() const;

    // ---- protocol-independent model lookup ----------------------------------------------------------------
    ProtoDb& protos() { return protos_; }
    const ProtoDb& protos() const { return protos_; }

private:
    void Send(const ByteBuffer& b);
    template <class T> void SendG(const T& m) { if (state_ == State::InGame) Send(gnet::PackG(m)); }
    void Handle(const uint8_t* data, size_t len);
    void HandleGame(GMsg id, ByteBuffer& b);
    void AddChat(ChatLine l);
    void SetTrackSnap(ClientCritter& c, float x, float y);
    void ReplaceTrack(ClientCritter& c, std::vector<TrackKey> keys);
    void OnLoginOk();
    void ResolvePid(ClientCritter& c);

    _ENetHost* host_ = nullptr;
    _ENetPeer* peer_ = nullptr;
    State state_ = State::Disconnected;
    std::string failReason_;
    std::string name_;
    CritterId selfId_ = 0;
    double stepDur_ = 0.2;
    ClientMap map_;
    uint64_t mapSerial_ = 0;
    std::unordered_map<CritterId, ClientCritter> critters_;
    std::unordered_map<ItemId, ClientItem> items_;
    std::unordered_map<ItemId, gnet::ObjectInfo> pendingInfo_;
    std::deque<ChatLine> chat_;
    uint64_t chatCounter_ = 0;
    uint32_t lastPong_ = 0;
    std::vector<Floater> floaters_;

    bool hasStats_ = false;
    gnet::Stats stats_;
    std::vector<gnet::InvEntry> inventory_;
    uint32_t containerOwner_ = 0;
    DialogState dialog_;
    std::vector<gnet::InvEntry> container_;

    bool legacyOnly_ = false;
    bool serverTiming_ = false;
    struct Wait {
        bool valid = false;
        double recvLocal = 0, waitEnd = 0, battleEnd = 0, total = 0;
        float ap = 0;  // 1/100 AP at recvLocal
        uint16_t maxAp = 0;
        uint32_t apRegenMs = 20000;
        gnet::ActionResult result = gnet::ActionResult::None;
        uint8_t action = 0;
    } wait_;

    ProtoDb protos_;
};

}  // namespace f3
