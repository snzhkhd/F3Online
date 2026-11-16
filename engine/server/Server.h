#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include "Account.h"
#include "AmxHost.h"
#include "Content.h"
#include "DataStore.h"
#include "GameConfig.h"
#include "World.h"
#include "common/Localization.h"
#include "common/buffer.h"
#include "common/protocol.h"
#include "common/protocol_game.h"

struct _ENetHost;

namespace f3 {

// Seed of the server-side gameplay RNGs (combat, doors/locks). 0 = pick a random one on first use; SetServerSeed(0) keeps it random.
void SetServerSeed(uint32_t seed);
uint32_t ServerSeed();

class Scripting;

class Server {
public:
    struct Config {
        uint32_t seed = 0;              // --seed N: seed of the gameplay RNGs (0 = random, the chosen seed is logged) so a failing run can be replayed
        uint16_t port = kDefaultPort;
        int tickRate = kDefaultTickRate;
        int moveTicks = 4;  // ticks per tile step (20Hz/4 = 5 tiles/s)
        int maxClients = 64;
        std::filesystem::path scriptsDir = "scripts";
        std::filesystem::path dataDir;  // gamedata/server (tools/dataimport output); empty = auto-detect, missing = test map only
        bool devKit = false;            // --dev-kit: new players get a few test items (gun, ammo, armor, revolver); default: nothing
        bool noData = false;            // --no-data: never load gamedata even if present
        int maxRunSeconds = 0;  // 0 = run until stopped; used by smoke tests
        std::filesystem::path savesDir;  // holds f3.db (accounts, characters, mail, world state, events); default: <repo>/saves, gitignored
        bool allowGuest = false;        // --allow-guest (implied by --dev-kit): legacy name-only login, nothing is saved
        int saveIntervalSec = 60;       // periodic save of all logged in players (0 = only on logout / transfer / shutdown)
        int pbkdf2Iterations = 10000;
        int maxLoginFails = 5;          // failed logins per account / ip within 5 minutes before a 30 s lockout
        std::string bindHost;   // empty = all interfaces; "127.0.0.1" = loopback only (no Windows Firewall prompt)
        std::filesystem::path contentDir;   // data/ (designer JSON, docs/data-format.md); empty = auto-detect, none found = built-in defaults
        bool lenient = false;               // --lenient: data validation errors are warnings (the bad records are dropped)
        bool devCommands = false;           // --dev-commands (implied by --dev-kit): /reload and friends from player chat
        std::string lang = "en";            // --lang: language of system messages for players that did not announce one (and of the console)
        std::filesystem::path localeDir;    // data/locale; empty = <content dir>/locale, else the nearest data/locale
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
    static constexpr int kViewCells = 48;  // default visibility radius in cells (24 world units, Perception 5); see ViewCells()
    // Visibility radius of a critter in cells: GameConfig ViewBase + ViewPerPerception * Perception (clamped 8..200).
    int ViewCells(const Critter& c) const { return std::clamp(rt_.ViewBase + rt_.ViewPerPerception * c.special[1], 8, 200); }
    static bool InRange(TilePos a, TilePos b, int r) { const int dx = a.x - b.x, dy = a.y - b.y; return dx * dx + dy * dy <= r * r; }

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
    bool EquipItem(Critter& c, ItemId invId, bool on, uint8_t slot = 0);
    bool UseObject(Critter& c, ItemId objId);
    Item* FindObject(ItemId id, Map** mapOut = nullptr);
    // ---- doors and containers (Doors.cpp): states Open / Closed / Locked + LockFlag bits, key, lock DC; see docs/research/doors.md
    enum class OperateResult : uint8_t { Opened, OpenedWithKey, Closed, AlreadyOpen, AlreadyClosed, Locked, NoOpen, Jammed, Blocked, Vetoed, NotOperable };
    static const char* OperateResultName(OperateResult r);
    // want: 1 open, 0 close, -1 toggle. Checks flags, lock and key (the actor's inventory), runs the OnOpen / OnClose script veto.
    OperateResult Operate(Critter* actor, Map& m, Item& o, int want);
    void ReportOperate(Critter* actor, const Item& o, OperateResult r);   // the player-visible message of an Operate result
    bool HasKey(const Critter& c, const Item& o, std::string* keyName = nullptr) const;
    bool CanOperate(const Critter& c, const Item& o) const;               // a closed door the critter may open right now
    bool DoorPassable(const Critter& c, Map& m, int cellIndex);           // pathing: closed operable door cell
    void OpenDoorAt(Critter& c, Map& m, int x, int y, bool report);
    void OpenDoorsAhead(Critter& c, Map& m);                              // doors on the next cells of the critter's path start opening
    int LockChance(const Critter& c, const Item& o, int* dc = nullptr);   // Security skill vs lock DC, percent
    bool Lockpick(Critter& c, Item& o);                                   // Security skill on a locked door / container (true = handled)
    void SetDoorLocked(Item& door, bool locked, const char* reason = "script");   // script API (UnlockDoor / LockDoor), doors and containers
    void SetLockFlags(Item& o, uint8_t flags, const char* reason = "script");
    void SetLockDC(Item& o, int dc);
    void SetLockKey(Item& o, const std::string& keyId);
    int ObjDist(const Item& o, TilePos from) const;                       // distance to the object or its doorway cells
    void LoadWorldState();                                                // table world_objects: door / container state per map
    void SaveWorldState(bool force = false);
    bool TakeFrom(Critter& c, ItemId containerId, ItemId entry, bool corpse = false);
    bool PutInto(Critter& c, ItemId containerId, ItemId entry, bool corpse);   // move one of c's items into a container / corpse
    bool OpenLoot(Critter& c, ItemId id, bool corpse);                          // send the contents (S2C_Container)
    void SendContainer(Critter& c, ItemId id, bool corpse);
    bool StartAttack(Critter& c, CritterId target);
    bool InAttackRange(const Critter& c, const Critter& target);   // weapon range + line of sight (AMX A_JustAttack)
    void Damage(Critter& target, int amount, Critter* attacker);
    void Heal(Critter& c, int amount);
    void KillCritter(Critter& c, Critter* killer);
    bool EnterMap(Critter& c, Map& dst, int entryId);        // map transition / login placement
    bool PickEntryPos(Map& dst, int entryId, TilePos& out, Dir& dir);
    bool EnterMapByName(Critter& c, const std::string& name, int entryId);
    void SendStats(Critter& c);
    void SendInventory(Critter& c);
    // ---- character system (Character.cpp)
    void RecalcDerived(Critter& c);   // maxHp from SPECIAL / level
    int SkillRate(const Critter& c) const;
    int SkillBase(const Critter& c, int skill) const;
    int SkillValue(const Critter& c, int skill) const;
    int XpForLevel(int level) const;
    int KillXp(const Critter& victim) const;
    void AddXp(Critter& c, int amount);
    void SendCharSheet(Critter& c);
    bool CreateCharacter(Critter& c, const gnet::CharCreate& m);
    bool SpendSkillPoints(Critter& c, int skill, int points);
    bool UseSkill(Critter& c, int skill, int kind, uint32_t target);
    // ---- Pip-Boy: quests, friends, mail (Pip.cpp). Quests and friends live in the character (tables quests / friends of the store), mail per account (table mail).
    bool QuestAdd(Critter& c, const std::string& id, const std::string& title, const std::string& text);   // false: id already in the log
    bool QuestSet(Critter& c, const std::string& id, uint8_t state);   // 1 completed, 2 failed, 0 active again; false: unknown quest
    int QuestState(const Critter& c, const std::string& id) const;     // -1 unknown, else 0 / 1 / 2
    bool FriendAdd(Critter& c, const std::string& name);
    bool FriendDel(Critter& c, const std::string& name);
    bool MailSend(Critter& c, const std::string& to, const std::string& text);
    bool MailRead(Critter& c, uint32_t id);
    bool MailDelete(Critter& c, uint32_t id);
    void SendPip(Critter& c);          // full snapshot (S2C_Pip)
    void PipOnLogin(Critter& c);       // snapshot, unread-mail notice, tells friends
    void PipOnLogout(Critter& c);      // updates the friends' lists
    bool PipChat(Critter& c, const std::string& text);   // /friend, /friends, /mail, /inbox, /quests chat commands; true = handled
    void HandlePip(Critter& c, const gnet::PipReq& m);
    // ---- data-driven content (GameContent.cpp): traits, perks, skills, quests and item / npc overrides from data/*.json
    const ContentDb& Content() const { return content_; }
    bool LoadContent();                                   // startup: strict (fails on errors unless --lenient); applies items / npcs to the prototypes
    std::vector<std::string> ReloadContent();             // /reload: re-reads data/, applies on success, returns the report lines
    std::string PerkName(const Critter& c, const PerkDef& p) const;     // localized (see LOr): perk.<id>.name / .desc, trait.<id>.name / .desc, quest.<id>.title / .text
    std::string PerkDesc(const Critter& c, const PerkDef& p) const;
    std::string TraitName(const Critter& c, const TraitDef& t) const;
    std::string TraitDesc(const Critter& c, const TraitDef& t) const;
    std::string QuestTitle(const Critter& c, const Critter::QuestEntry& q) const;
    std::string QuestText(const Critter& c, const Critter::QuestEntry& q) const;
    int TraitMaxEff() const;                              // traits allowed at creation: data/traits max_picks, else config.lua TraitMax
    int Mod(const Critter& c, int stat) const;            // sum of the stat modifiers of the critter's traits and perks (StatId / kStatSkillBase + skill)
    int PerkRank(const Critter& c, const std::string& id) const;
    bool PerkAvailable(const Critter& c, const PerkDef& p, std::string* why = nullptr) const;   // requirements, rank cap (not the perk points)
    bool TakePerk(Critter& c, const std::string& id);     // spends a perk point
    bool QuestStart(Critter& c, const std::string& id);   // data/quests record -> Pip-Boy task
    void SendContentDefs(Critter& c);
    bool LangChat(Critter& c, const std::string& text);      // /lang [code]; true = handled
    bool ContentChat(Critter& c, const std::string& text);   // /perks /perk /traits /reload; true = handled
    void SpawnDataNpcs();                                    // data/npcs "spawns"
    std::string ReloadAll(const std::string& what);          // "data" | "scripts" | "all" -> report text (console and /reload)
    // ---- accounts + persistence (Persist.cpp)
    bool OpenStore();                 // <saves>/f3.db: open + migrate the schema + import old JSON saves; false = fatal (clear message logged)
    Store& GetStore() { return store_; }
    // audit log (table events): kind login / logout / register / kick / level_up / kill / mail_sent / character_created ...; never throws
    void Audit(const std::string& kind, const std::string& actor, const nlohmann::json& data = nlohmann::json::object());
    bool SavePlayer(Critter& c);
    void SaveAllPlayers();           // one transaction for all players
    nlohmann::json CharacterToJson(const Critter& c) const;
    void ApplyCharacter(Critter& c, const nlohmann::json& j);
    // quest / script globals: per player overlay (Critter::gvars) over the shared table (originals from globals.ini)
    double GetGlobalFor(const Critter* pc, const std::string& k) const;
    void SetGlobalFor(Critter* pc, const std::string& k, double v);
    int Weight(const Critter& c) const;
    int ProtoWeight(const Proto& p) const;   // unit weight (config class table / per-prototype override)
    int CarryLimit(const Critter& c) const;  // CarryBase + CarryPerStrength * ST
    std::string Examine(Critter& viewer, ItemId id, bool isCritter);
    Map* FindMap(const std::string& name) { return world_.FindMapByName(name); }
    double GetGlobal(const std::string& k) const { auto it = globals_.find(k); return it == globals_.end() ? 0.0 : it->second; }
    void SetGlobal(const std::string& k, double v) { globals_[k] = v; }
    void RemoveCritter(CritterId id);
    bool MoveCritterTo(Critter& c, TilePos target, int run = -1);  // cell target (centre); pathfinds + smooths; run: -1 default rule, 0 walk, 1 run
    bool StartMove(Critter& c, float tx, float ty, bool run, bool exact);  // float target in cells; exact = stop at that point, else at the cell centre
    void StopMove(Critter& c, uint8_t reason = 1);   // sends CritterStop to viewers
    void StopCritter(Critter& c) { StopMove(c, 1); }
    bool DefaultRun(const Critter& c) const;

    // ---- real-time timing (Timing.cpp): FOnline-style breaktime, AP and battle flag
    GameConfig& Rt() { return rt_; }
    const GameConfig& Rt() const { return rt_; }
    uint64_t NowMs() const;                          // server clock, ms (never 0)
    static uint32_t Ms32(uint64_t ms) { return static_cast<uint32_t>(ms); }
    int MaxAp(const Critter& c) const;               // 1/100 AP
    void SyncAp(Critter& c);                         // regenerates AP up to now
    bool IsFree(const Critter& c) const { return NowMs() >= c.waitEndMs; }
    void Wait(Critter& c, int ms, bool notify = true);   // cr.Wait(ms): busy until now + ms
    void OnAttacked(Critter& target, Critter& attacker);  // every attack attempt, hit or miss
    void SetBattle(Critter& c, bool notify = true, int ms = -1);  // battle flag for TimeoutBattle ms
    bool SpendAp(Critter& c, int cost);              // spends if available
    gnet::ActionResult CheckAction(Critter& c, uint8_t action, int apCost, bool report = true);  // free? dead? enough AP? (no side effects)
    void CommitAction(Critter& c, uint8_t action, int apCost, int waitMs);                       // spends AP, sets wait, notifies
    void SendWaitState(Critter& c, uint8_t action = 0, gnet::ActionResult r = gnet::ActionResult::None);
    bool Teleport(Critter& c, MapId map, TilePos pos);
    void Say(Critter& speaker, const std::string& text);  // local chat, heard in view radius
    void Tell(Critter& player, const std::string& text);  // system message to one player
    void Broadcast(const std::string& text);              // system message to all players
    // ---- localization (Localization.cpp of common, data/locale/<lang>/*.json; docs/localization.md). Messages are localized on the server with the
    // language each player announced at login (Critter::lang); fallback chain: that language -> en -> the key itself.
    const loc::Locale& Loc() const { return loc_; }
    std::string LangOf(const Critter& c) const { return c.IsPlayer() && !c.lang.empty() ? c.lang : cfg_.lang; }
    std::string L(const Critter& c, const std::string& key, loc::Args args = {}) const { return loc_.Format(LangOf(c), key, args); }
    std::string LOr(const Critter& c, const std::string& key, const std::string& fallback) const { return loc_.TrOr(LangOf(c), key, fallback); }
    std::string Stf(const Critter& c, int id) const;     // English.stf text, or its "stf.<id>" override of the player's language
    std::string SkillName(const Critter& c, int skill) const;   // "skill.<id>.name" (the id is the English name)
    void TellK(Critter& player, const std::string& key, loc::Args args = {}) { Tell(player, L(player, key, args)); }
    void BroadcastK(const std::string& key, loc::Args args = {});   // every player in his own language
    bool LoadLocale();                                    // startup: data/locale; a missing folder only warns (keys are shown instead)
    void SendBuf(Critter& to, const ByteBuffer& b) { Send(to, b); }  // raw message to one player (AMX dialogue engine)
    AmxHost* Amx() { return amx_.get(); }
    void SetDoor(Map& m, Item& door, bool open);
    void TalkTo(Critter& pc, CritterId npc);   // dialogue: walk next to the NPC, then run its .scra conversation
    void RunTalk(Critter& pc);                 // per-tick continuation of TalkTo

private:
    void HandleEvents(int timeoutMs);
    void HandlePacket(struct _ENetPeer* peer, const uint8_t* data, size_t len);
    void OnLogin(struct _ENetPeer* peer, const net::LoginReq& req);
    void OnDisconnect(struct _ENetPeer* peer);
    void OnAuth(struct _ENetPeer* peer, const gnet::Auth& a);
    // creates + places the player critter; saved = character json of an account (nullptr = guest / fresh)
    bool StartSession(struct _ENetPeer* peer, const std::string& name, const nlohmann::json* saved, const std::string& account, const std::string& lang);
    std::string AnnouncedLang(const std::string& lang) const;   // client's language code, normalized; empty -> server default
    // Places a player on its map (saved position or the start entry), sends MapInfo / Stats / CharSheet / Inventory, runs on_login, saves.
    // Called at the end of StartSession for created characters and from CreateCharacter for a session that was in the "creating" state.
    void EnterWorld(Critter& c, const nlohmann::json* saved);
    bool IsCreating(const Critter& c) const { return c.IsPlayer() && !c.created && c.mapId == 0; }   // authenticated, not in the world yet
    std::unordered_map<CritterId, nlohmann::json> creatingSaved_;   // legacy save json of a session waiting in character creation (position to restore afterwards)
    void OnMoveReq(Critter& c, const net::MoveToReq& req);
    void OnMoveReqF(Critter& c, const gnet::MoveToF& req);
    void DoMoveCell(Critter& c, TilePos target);
    void DoMoveF(Critter& c, float x, float y, bool run);
    void RunQueuedMove(Critter& c);
    void OnChatReq(Critter& c, const std::string& text);

    void DoTick();
    void StepCritter(Critter& c);
    void AdvanceMove(Critter& c, uint64_t now);
    void SendMovePath(Critter& c, Critter* onlyTo = nullptr);
    void SendCritterStop(const Critter& c, uint8_t reason, Critter* onlyTo = nullptr);
    void SendLegacyMove(const Critter& c, uint8_t snap);
    void SendCapsResync(Critter& player);
    int AttackApCost(int weaponCost) const;
    int ActionCost(uint8_t action) const;
    void UpdateVisibility(Map& m);
    void ProcessCommands();
    void ExecCommand(const std::string& cmd);

    void HandleGamePacket(struct _ENetPeer* peer, Critter* c, GMsg id, ByteBuffer& b, bool& ok);
    void BeginInteract(Critter& c, PendingType type, ItemId id, ItemId sub, bool critter = false);
    // The contents vector of a container object or a corpse (dead NPC) within reach of c (same map, range); nullptr otherwise.
    std::vector<InvItem>* LootTarget(Critter& c, ItemId id, bool corpse, std::string* name = nullptr);
    void RunPending(Critter& c);
    void TickCombat(Critter& c);
    void CheckRegions(Critter& c);
    void SendCritterState(const Critter& c);
    void SendAppearance(const Critter& c, Critter* onlyTo = nullptr);   // worn armor / helmet / hand-1 weapon (appearance block)
    void SendObjectInfo(Critter& to, const Item& it);
    void DropAllToGround(Critter& c);
    void Respawn(Critter& c);
    void TickObjects(Map& m);
    void ApplyDoorBlock(Map& m, Item& door);
    void NotifyObject(Map& m, const Item& o);   // ObjectInfo to every player that sees the object
    void LockChanged(Item& o, const char* reason);
    void SetOpen(Map& m, Item& o, bool open);
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
    GameConfig rt_;
    ContentDb content_;
    uint32_t contentRev_ = 0;
    Store store_;           // declared before accounts_ (which points at it); all access on the server thread (Db.h)
    AccountStore accounts_;
    uint64_t nextSaveTick_ = 0;
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();

    struct MailMsg { uint32_t id = 0; std::string from, text; int64_t time = 0; bool read = false; };
    std::vector<MailMsg> Mailbox(const std::string& accountKey);   // from the store, oldest first
    void DbCommand(const std::string& cmd, std::istringstream& args);   // console: dbinfo | events [n] [kind]
    loc::Locale loc_;
    std::mutex cmdMutex_;
    std::vector<std::string> commands_;
};

}  // namespace f3
