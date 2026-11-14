#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "AmxHost.h"
#include "DataStore.h"
#include "World.h"
#include "common/buffer.h"
#include "common/protocol.h"
#include "common/protocol_game.h"

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
        std::filesystem::path dataDir;  // gamedata/server (tools/dataimport output); empty = auto-detect, missing = test map only
        bool noData = false;            // --no-data: never load gamedata even if present
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
    const DataStore& Data() const { return data_; }
    static constexpr int kViewCells = 48;  // visibility radius in cells (24 world units)

    Map* CreateMap(const std::string& name, int w, int h, std::vector<uint8_t> blocked, TilePos spawn);
    Critter* SpawnNpc(MapId map, const std::string& name, TilePos pos, const std::string& type, uint32_t color);
    ItemId SpawnItem(MapId map, uint16_t pid, const std::string& name, TilePos pos);

    // ---- data-driven content (Game.cpp)
    bool LoadWorldData();                                   // maps + entity instances from DataStore
    Critter* SpawnCritterProto(MapId map, const Proto& p, TilePos pos, const EntityDef* inst);
    ItemId SpawnObjectProto(MapId map, const Proto& p, TilePos pos, uint16_t count, const EntityDef* inst);
    void InitCritterStats(Critter& c, const Proto& p);
    ItemId GiveItem(Critter& c, const Proto& p, int count);  // returns inventory entry id
    bool PickUp(Critter& c, ItemId groundId);
    bool DropItem(Critter& c, ItemId invId);
    bool EquipItem(Critter& c, ItemId invId, bool on);
    bool UseObject(Critter& c, ItemId objId);
    bool TakeFrom(Critter& c, ItemId containerId, ItemId entry);
    bool StartAttack(Critter& c, CritterId target);
    void Damage(Critter& target, int amount, Critter* attacker);
    void Heal(Critter& c, int amount);
    void KillCritter(Critter& c, Critter* killer);
    bool EnterMap(Critter& c, Map& dst, int entryId);        // map transition / login placement
    bool PickEntryPos(Map& dst, int entryId, TilePos& out, Dir& dir);
    bool EnterMapByName(Critter& c, const std::string& name, int entryId);
    void SendStats(Critter& c);
    void SendInventory(Critter& c);
    int Weight(const Critter& c) const;
    std::string Examine(Critter& viewer, ItemId id, bool isCritter);
    Map* FindMap(const std::string& name) { return world_.FindMapByName(name); }
    double GetGlobal(const std::string& k) const { auto it = globals_.find(k); return it == globals_.end() ? 0.0 : it->second; }
    void SetGlobal(const std::string& k, double v) { globals_[k] = v; }
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

    void HandleGamePacket(struct _ENetPeer* peer, Critter* c, GMsg id, ByteBuffer& b, bool& ok);
    void BeginInteract(Critter& c, PendingType type, ItemId id, ItemId sub);
    void RunPending(Critter& c);
    void TickCombat(Critter& c);
    void CheckRegions(Critter& c);
    void SendCritterState(const Critter& c);
    void SendObjectInfo(Critter& to, const Item& it);
    void DropAllToGround(Critter& c);
    void Respawn(Critter& c);
    void SetDoor(Map& m, Item& door, bool open);
    void PlaceCritter(Critter& c, Map& m, TilePos pos);
    void UnplaceCritter(Critter& c);
    void SendMapInfo(Critter& player, const Map& m);

    void Send(Critter& to, const ByteBuffer& b);
    void SendRaw(struct _ENetPeer* peer, const ByteBuffer& b);
    template <class T> void SendMsg(Critter& to, const T& m) { Send(to, net::Pack(m)); }
    net::CritterAdd MakeAdd(const Critter& c) const;

    Config cfg_;
    World world_;
    DataStore data_;
    std::map<std::string, double> globals_;  // original Scripts/globals.ini variables (runtime values)
    std::vector<std::string> warnedMaps_;
    _ENetHost* host_ = nullptr;
    std::unique_ptr<Scripting> scripting_;
    std::unique_ptr<AmxHost> amx_;  // original AMX scripts (prototype), only with game data
    std::atomic<bool> running_{false};
    uint64_t tick_ = 0;

    std::mutex cmdMutex_;
    std::vector<std::string> commands_;
};

}  // namespace f3
