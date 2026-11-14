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
    size_t Count() const { return instances_.size(); }
    // console helper: load `script` (name without extension), call public `pub`, with native tracing on
    std::string RunTest(const std::string& script, const std::string& pub);

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

    Server& server_;
    std::filesystem::path dir_;
    bool available_ = false;
    std::unordered_map<std::string, AmxNative> natives_;
    std::unordered_map<std::string, std::shared_ptr<AmxScript>> scripts_;
    std::unordered_map<uint64_t, Entry> instances_;
};

}  // namespace f3
