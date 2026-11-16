#pragma once
// Lua (sol2) game-logic host. The server calls hooks; scripts call the API bound in Scripting.cpp.
//
// Hooks (all optional globals in scripts/main.lua or files it loads):
//   on_init()                    once, first successful load (create maps, spawn npcs)
//   on_reload()                  after each successful hot reload
//   on_login(critter)            player entered the world
//   on_logout(critter)
//   on_critter_move(critter, from_x, from_y)   after every validated step (players and npcs)
//   on_chat(critter, text)       return false to swallow, a string to replace the text
//   on_tick(tick)                every server tick
// Gameplay hooks (all optional): on_death(c, killer), on_damage(c, attacker, amount), on_pickup(c, obj_id, proto_key),
//   on_use(c, obj_id, proto_key, script), on_region_enter(c, script), on_region_leave(c, script)
// Door / container hooks (Doors.cpp): door_use(c_or_nil, obj_id, result, used_key) after every operate / lockpick attempt (result: opened,
//   unlocked_key, closed, locked, no_open, jammed, blocked, vetoed, picked, pick_failed, pick_impossible, pick_no_kit, pick_not_needed);
//   lock_changed(obj_id, locked, flags, reason) whenever the lock state changes (reason: script, key, pick);
//   lockpick_chance(c, obj_id, chance) -> number overrides the percent chance of a Security pick.
// Object script hooks (data/objects "script": scripts/objects/<name>.lua, each file runs in its own environment, see ObjectScripts.cpp):
//   on_use(player, obj), on_skill(player, obj, skill_name), on_look(player, obj), can_use(player, obj) -> false refuses, on_loaded(obj)
//   a true result from on_use / on_skill / on_look means "handled": the declarative steps of the binding are skipped.
// pick_npc_type(npc) -> string chooses npc_types[...] for data-spawned critters (once, on their first tick).
// NPC behaviours live in the global table npc_types[name] = { on_tick(npc, tick), on_heard(npc, speaker, text) }.
//
// Critter/Map objects handed to Lua are raw handles: use them inside the hook that
// received them, keep ids (not objects) across ticks and re-fetch with get_critter(id).
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <vector>
#include <optional>
#include <string>
#include <sol/sol.hpp>
#include "World.h"
#include "common/lua_bind.h"

namespace f3 {

class Server;

// Handle of a map object given to Lua scripts (properties and methods are bound in Scripting.cpp). Looked up by id on every call, so it
// stays safe after the object is gone (calls then do nothing).
struct LuaObj {
    Server* s = nullptr;
    ItemId id = 0;
};

class Scripting {
public:
    Scripting(Server& server, std::filesystem::path scriptsDir);
    ~Scripting();

    bool Load();    // first load: runs main.lua then on_init
    bool Reload();  // builds a fresh state, swaps only on success, then on_reload

    bool OnLogin(Critter& c);
    void OnLogout(Critter& c);
    void OnCritterMove(Critter& c, TilePos from);
    bool OnChat(Critter& c, std::string& text);  // false = swallow
    void OnTick(uint64_t tick);
    void OnNpcTick(Critter& npc, uint64_t tick);
    void OnNpcHeard(Critter& npc, Critter& speaker, const std::string& text);

    // Generic hook call (on_death, on_damage, on_pickup, on_use, on_region_enter/leave, ...). Missing hooks are ignored.
    template <class... A> void Call(const char* fn, A&&... args) {
        if (L_) lua::CallGlobal(*L_, fn, nullptr, std::forward<A>(args)...);
    }

    // Hook that answers "handled?" (returns true when the Lua function exists, ran without error and returned true).
    template <class... A> bool CallHandled(const char* fn, A&&... args) {
        if (!L_) return false;
        sol::object r;
        if (!lua::CallGlobal(*L_, fn, &r, std::forward<A>(args)...)) return false;
        return r.is<bool>() && r.as<bool>();
    }

    // Hook that answers a number (std::nullopt when missing, failed or not a number).
    template <class... A> std::optional<double> CallNumber(const char* fn, A&&... args) {
        if (!L_) return std::nullopt;
        sol::object r;
        if (!lua::CallGlobal(*L_, fn, &r, std::forward<A>(args)...)) return std::nullopt;
        if (r.is<double>()) return r.as<double>();
        return std::nullopt;
    }

    const std::filesystem::path& Dir() const { return dir_; }

    // ---- object scripts (scripts/objects/*.lua)
    struct ScriptCheck {
        bool exists = false, ok = false;       // ok: the file parses (and runs) in a sandbox
        std::string error, warning;            // error: syntax; warning: top-level code that failed in the sandbox (API functions are missing there)
        std::vector<std::string> hooks;        // on_use, on_skill, on_look, can_use, on_loaded that the file defines
    };
    static ScriptCheck CheckScript(const std::filesystem::path& file);   // own sandbox state: safe from any thread
    // (re)loads the given files (relative to the scripts dir) into one environment each; returns one message per failure
    std::vector<std::string> LoadObjectScripts(const std::vector<std::string>& rels);
    bool EnsureObjectScript(const std::string& rel, std::string* err = nullptr);   // loads one file when it is not loaded yet (objects spawned after startup); a failure is remembered until the next LoadObjectScripts
    bool HasObjectHook(const std::string& rel, const char* fn) const {
        auto it = objEnvs_.find(rel);
        if (it == objEnvs_.end()) return false;
        sol::object f = it->second[fn];
        return f.get_type() == sol::type::function;
    }
    // Calls fn of the object script `rel`. False when missing or failed; *ret receives the first result.
    template <class... A> bool CallObjectHook(const std::string& rel, const char* fn, sol::object* ret, A&&... args) {
        auto it = objEnvs_.find(rel);
        if (it == objEnvs_.end() || !L_) return false;
        sol::object f = it->second[fn];
        if (f.get_type() != sol::type::function) return false;
        sol::protected_function pf = f.as<sol::protected_function>();
        sol::protected_function_result r = pf(std::forward<A>(args)...);
        if (!r.valid()) { sol::error e = r; log::Error("lua: %s %s: %s", rel.c_str(), fn, e.what()); return false; }
        if (ret) *ret = (r.return_count() > 0) ? r.get<sol::object>() : sol::make_object(sol::state_view(*L_), sol::nil);
        return true;
    }
    template <class... A> bool CallObjectHandled(const std::string& rel, const char* fn, A&&... args) {
        sol::object r;
        if (!CallObjectHook(rel, fn, &r, std::forward<A>(args)...)) return false;
        return r.is<bool>() && r.as<bool>();
    }

    // Real-time tunables: scripts/config.lua (global table `config`) -> GameConfig. Missing keys keep their defaults.
    bool LoadConfig();   // (re)reads config.lua into the live state and the server GameConfig
    void PollConfig();   // hot reload: reloads when config.lua changed on disk (cheap mtime check)

private:
    std::unique_ptr<sol::state> BuildState();
    void BindApi(sol::state& L);
    template <class... A> void CallNpc(const std::string& type, const char* fn, A&&... args);

    Server& server_;
    std::filesystem::path dir_;
    std::unique_ptr<sol::state> L_;
    std::set<std::string> objBad_;                      // object scripts that failed to load (not retried on every use)
    std::map<std::string, sol::environment> objEnvs_;   // declared after L_: destroyed (and cleared on reload) before the state
    int generation_ = 0;
    std::filesystem::file_time_type cfgTime_{};
    void ApplyConfig(sol::state_view L);
};

}  // namespace f3
