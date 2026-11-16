#pragma once
// Runs the original AMX scripts attached to map entities (EEOV "script" field, e.g. "CaptainArmstrong.amx").
// PROTOTYPE: OnCreate once, OnUpdate once per second, OnDeath. Natives are bound by name; about 35 of the 115
// original natives are implemented (entity state, globals, random, time, float math, team stubs); the rest return 0
// and are logged once. Lua remains the primary scripting layer; see docs/server-data.md.
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include "Amx.h"
#include "World.h"

namespace f3 {

class Server;

class AmxHost {
public:
    AmxHost(Server& server, std::filesystem::path scriptsDir);

    bool Available() const { return available_; }
    void AttachCritter(const Critter& c);
    void AttachObject(const Item& it);
    void Detach(uint64_t key);
    void DetachCritter(CritterId id) { Detach(id); }
    void Tick(uint64_t tick, const std::unordered_set<MapId>& activeMaps);
    void OnDeath(const Critter& c);
    void OnAttacked(Critter& victim, Critter& attacker);   // runs the script's OnAttacked(attacker, attacker) (barks etc.)
    // Dev/test switch (Lua amx_attacks, chat /amxattacks on a --dev-kit server): while off, A_Attack / A_JustAttack actions end at once without attacking, so
    // regression tests are not disturbed by the scripted NPCs of the start map (the tutorial insurgents hunt players from the first second).
    void SetAttacksEnabled(bool on) { attacksEnabled_ = on; }
    bool IsIdle(CritterId id) const;                        // action queue empty
    // Runs a public (OnOpen / OnClose) of a map object script, if it has one, with the actor handle. Returns false only when the script ran
    // and returned 0 (the original vetoes the open / close then, GameDoor::Operate); no script / error = true.
    bool CallObject(ItemId id, const char* pub, CritterId actor = 0);
    size_t Count() const { return instances_.size(); }
    // dev/test helper (Lua amx_native, chat /amxnative on a --dev-kit server): call natives by name from a tiny expression language,
    // see AmxSpatial.cpp (NativeTest). Returns a one-line result.
    std::string NativeTest(CritterId owner, const std::string& program);
    // console helper: load `script` (name without extension), call public `pub`, with native tracing on
    std::string RunTest(const std::string& script, const std::string& pub);

    // ---- dialogue engine: runs the original .scra conversation graph (see docs/amx-scripts.md, "Dialogue engine")
    bool StartDialog(Critter& pc, Critter& npc);   // false if the NPC has no (loadable) dialogue
    void DialogChoose(Critter& pc, int index);     // index into the replies of the last node sent
    void DialogClose(Critter& pc, bool notify = true);
    bool InDialog(CritterId id) const;             // id is the player or the NPC of a running conversation
    // Test/debug access to the current node of a player's conversation.
    struct DlgView { bool open = false; int textId = 0; std::string text, node; std::vector<std::string> replies; };
    DlgView DialogView(CritterId pc) const;

    static constexpr uint64_t kObjectBit = 1ull << 32;  // entity key = critter id, or object id | kObjectBit
    static AmxCell Handle(uint64_t key) { return static_cast<AmxCell>((key & kObjectBit) ? ((key & 0xFFFFFFFFu) | 0x40000000u) : key); }

private:
    struct Entry {
        std::unique_ptr<AmxInstance> inst;
        MapId map = 0;
        bool created = false;
        bool failedLogged = false;
        std::string name;
    };
    std::shared_ptr<AmxScript> GetScript(const std::string& name);
    std::unique_ptr<AmxInstance> NewInstance(const std::string& scriptName, uint64_t key);
    void BindNatives();
    void BindActionNatives();   // AmxActions.cpp
    void BindFloatNatives();    // AmxSpatial.cpp
    void BindSpatialNatives();  // AmxSpatial.cpp: geometry, LOS, FindObjects, waypoints, FOW, VEG, items, death records
    Critter* CritterOf(AmxCell handle);
    int RandomRange(int lo, int hi);   // inclusive

    // An entity handle resolved to a critter or a map object, with its world position (original world units; heights are not modelled: y = 0).
    struct EntRef {
        Critter* c = nullptr;
        Item* it = nullptr;
        MapId map = 0;
        float x = 0, z = 0;
        explicit operator bool() const { return c || it; }
    };
    EntRef Resolve(AmxCell handle);
    MapId OwnerMap(AmxInstance& m);                        // map of the script owner (critter or object), 0 if unknown
    bool Argc(const char* native, int got, int want);      // original VerifyScriptParameters: exact count, mismatch logs (once per native) and the native returns 0
    static bool ReadVec(const AmxInstance& m, AmxCell addr, float out[3]);
    static void WriteVec(AmxInstance& m, AmxCell addr, float x, float y, float z);
    bool LineClear(const Map& map, float ax, float az, float bx, float bz) const;   // grid line of sight between two world points
    bool Visible(const Map& map, float x, float z) const;  // some living player of the map sees the point (IsUnderFOW model)
    std::unordered_map<AmxCell, std::vector<AmxCell>> objectLists_;   // FindObjects result lists by handle; they live one tick like the original frame lists
    AmxCell nextObjectList_ = 100;
    std::unordered_map<uint64_t, std::vector<std::pair<std::string, int>>> vegs_;   // attached visual effects per entity key (server only allocates ids)
    int nextVeg_ = 1;
    std::unordered_set<std::string> argcWarned_;

    struct DlgReply { int flag = 0, textId = 0; std::string action, logic; };
    struct Dlg {
        CritterId pc = 0, npc = 0;
        std::unique_ptr<AmxInstance> inst;
        std::string node, textFn;
        int textId = 0;
        std::vector<DlgReply> replies;
    };
    void Present(Critter& pc);   // builds the replies of the current node and sends it (or ends the conversation)
    std::string DlgText(int id, const Critter* viewer = nullptr) const;   // stf text (colour tags stripped) in the viewer's language
    // NPC-initiated conversations (StartDialogue native): range, line of sight, combat, cooldown; see RequestNpcDialog
    static constexpr int kNpcDialogStartCells = 8, kNpcDialogCooldownSec = 30, kNpcDialogWaitSec = 60;   // the conversation ends at 12 cells (Tick)
    struct NpcDlgRequest { CritterId npc = 0, pc = 0; uint64_t expire = 0; };
    std::vector<NpcDlgRequest> npcDialogWait_;
    std::unordered_map<CritterId, uint64_t> npcDialogCooldown_;   // NPC -> tick before which it starts no new conversation
    bool NpcDialogReady(const Critter& pc, const Critter& npc) const;
    bool RequestNpcDialog(Critter& npc, Critter& pc);
    void TickNpcDialogRequests(uint64_t tick);
    std::unordered_map<CritterId, Dlg> dialogs_;   // by player id
    Dlg* active_ = nullptr;                        // conversation the natives currently write to
    std::unordered_map<std::string, std::shared_ptr<AmxScript>> dlgScripts_;

    // ---- action queue (AmxActions.cpp): A_Wait / A_FloatingSpeechText / A_MoveTo* / A_Attack run one after another per critter
    enum class ActType { Wait, Speech, Move, Attack, Wander };
    struct Act {
        ActType type = ActType::Wait;
        int ms = 0;
        float x = 0, y = 0;          // Move: destination in cells
        bool run = false, started = false;
        CritterId target = 0;        // Move (follow) / Attack
        uint64_t endMs = 0;
        std::string text;
        // Move to an entity (A_MoveToEntity): follows the target, arrives within stopDist world units (and, if reach >= 0, only with GetLOS == reach)
        bool follow = false;
        float stopDist = 5.f;
        int reach = -1;
        uint64_t lastRepath = 0;
        // Attack
        bool noMove = false;         // A_JustAttack: only attacks when already in range
        uint32_t startAttacks = 0;
        // Wander: rectangle in world units (upperLeft / lowerRight like the original), repetitions 0 = forever
        float ulx = 0, ulz = 0, lrx = 0, lrz = 0;
        int reps = 0;
    };
    void QueueAction(CritterId id, Act a);
    void TickActions(const std::unordered_set<MapId>& active);
    Critter* Protagonist(AmxInstance& m);   // conversation player, else nearest player to the script owner
    std::unordered_map<CritterId, std::deque<Act>> actions_;
    std::unordered_map<CritterId, int> squadOf_, squadRank_, teamRank_;
    int nextSquad_ = 1;
    bool attacksEnabled_ = true;
    std::map<std::pair<int, int>, int> teamAttitude_, squadAttitude_;   // (source, target) -> attitude

    Server& server_;
    std::filesystem::path dir_;
    bool available_ = false;
    std::unordered_map<std::string, AmxNative> natives_;
    std::unordered_map<std::string, std::shared_ptr<AmxScript>> scripts_;
    std::unordered_map<uint64_t, Entry> instances_;
    std::vector<Entry> graveyard_;   // detached instances stay alive until the end of the tick (a script may destroy its own entity mid-call)
};

}  // namespace f3
