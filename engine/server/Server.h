#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "World.h"
#include "common/buffer.h"
#include "common/protocol.h"

struct _ENetHost;

namespace f3 {

class Scripting;

class Server {
public:
    struct Config {
        uint16_t port = kDefaultPort;
        int tickRate = kDefaultTickRate;
        int moveTicks = 4;  // ticks per tile step (20Hz/4 = 5 tiles/s)
        int maxClients = 64;
        std::filesystem::path scriptsDir = "scripts";
        int maxRunSeconds = 0;  // 0 = run until stopped; used by smoke tests
    };

    explicit Server(Config cfg);
    ~Server();

    bool Init();
    void Run();
    void RequestStop() { running_ = false; }
    void PostCommand(std::string cmd);  // thread-safe; processed on the main loop

    // ---- game operations (used by the network layer and by the Lua API) ----
    World& GetWorld() { return world_; }
    const Config& GetConfig() const { return cfg_; }
    uint64_t Tick() const { return tick_; }

    Map* CreateMap(const std::string& name, int w, int h, std::vector<uint8_t> blocked, TilePos spawn);
    Critter* SpawnNpc(MapId map, const std::string& name, TilePos pos, const std::string& type, uint32_t color);
    ItemId SpawnItem(MapId map, uint16_t pid, const std::string& name, TilePos pos);
    void RemoveCritter(CritterId id);
    bool MoveCritterTo(Critter& c, TilePos target);  // validated; pathfinds; false if no path
    void StopCritter(Critter& c) { c.path.clear(); }
    bool Teleport(Critter& c, MapId map, TilePos pos);
    void Say(Critter& speaker, const std::string& text);  // local chat, heard in view radius
    void Tell(Critter& player, const std::string& text);  // system message to one player
    void Broadcast(const std::string& text);              // system message to all players

private:
    void HandleEvents(int timeoutMs);
    void HandlePacket(struct _ENetPeer* peer, const uint8_t* data, size_t len);
    void OnLogin(struct _ENetPeer* peer, const net::LoginReq& req);
    void OnDisconnect(struct _ENetPeer* peer);
    void OnMoveReq(Critter& c, const net::MoveToReq& req);
    void OnChatReq(Critter& c, const std::string& text);

    void DoTick();
    void StepCritter(Critter& c);
    void UpdateVisibility(Map& m);
    void ProcessCommands();
    void ExecCommand(const std::string& cmd);

    void PlaceCritter(Critter& c, Map& m, TilePos pos);
    void UnplaceCritter(Critter& c);
    void SendMapInfo(Critter& player, const Map& m);

    void Send(Critter& to, const ByteBuffer& b);
    void SendRaw(struct _ENetPeer* peer, const ByteBuffer& b);
    template <class T> void SendMsg(Critter& to, const T& m) { Send(to, net::Pack(m)); }
    net::CritterAdd MakeAdd(const Critter& c) const;

    Config cfg_;
    World world_;
    _ENetHost* host_ = nullptr;
    std::unique_ptr<Scripting> scripting_;
    std::atomic<bool> running_{false};
    uint64_t tick_ = 0;

    std::mutex cmdMutex_;
    std::vector<std::string> commands_;
};

}  // namespace f3
