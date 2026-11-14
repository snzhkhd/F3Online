#pragma once
// Runs the original AMX scripts attached to map entities (EEOV "script" field, e.g. "CaptainArmstrong.amx").
// PROTOTYPE: OnCreate once, OnUpdate once per second, OnDeath. Natives are bound by name; about 35 of the 115
// original natives are implemented (entity state, globals, random, time, float math, team stubs); the rest return 0
// and are logged once. Lua remains the primary scripting layer; see docs/server-data.md.
#include <cstdint>
#include <filesystem>
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
    void CallObject(ItemId id, const char* pub);   // runs a public (OnOpen / OnClose) of a map object script, if it has one
    size_t Count() const { return instances_.size(); }
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
        std::string name;
    };
    std::shared_ptr<AmxScript> GetScript(const std::string& name);
    std::unique_ptr<AmxInstance> NewInstance(const std::string& scriptName, uint64_t key);
    void BindNatives();
    Critter* CritterOf(AmxCell handle);

    struct DlgReply { int flag = 0, textId = 0; std::string action, logic; };
    struct Dlg {
        CritterId pc = 0, npc = 0;
        std::unique_ptr<AmxInstance> inst;
        std::string node, textFn;
        int textId = 0;
        std::vector<DlgReply> replies;
    };
    void Present(Critter& pc);   // builds the replies of the current node and sends it (or ends the conversation)
    std::string DlgText(int id) const;
    std::unordered_map<CritterId, Dlg> dialogs_;   // by player id
    Dlg* active_ = nullptr;                        // conversation the natives currently write to
    std::unordered_map<std::string, std::shared_ptr<AmxScript>> dlgScripts_;

    Server& server_;
    std::filesystem::path dir_;
    bool available_ = false;
    std::unordered_map<std::string, AmxNative> natives_;
    std::unordered_map<std::string, std::shared_ptr<AmxScript>> scripts_;
    std::unordered_map<uint64_t, Entry> instances_;
};

}  // namespace f3
