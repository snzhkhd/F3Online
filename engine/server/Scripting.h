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
//   on_use(c, obj_id, proto_key), on_region_enter(c, script), on_region_leave(c, script)
// pick_npc_type(npc) -> string chooses npc_types[...] for data-spawned critters (once, on their first tick).
// NPC behaviours live in the global table npc_types[name] = { on_tick(npc, tick), on_heard(npc, speaker, text) }.
//
// Critter/Map objects handed to Lua are raw handles: use them inside the hook that
// received them, keep ids (not objects) across ticks and re-fetch with get_critter(id).
#include <filesystem>
#include <memory>
#include <string>
#include <sol/sol.hpp>
#include "World.h"
#include "common/lua_bind.h"

namespace f3 {

class Server;

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

    const std::filesystem::path& Dir() const { return dir_; }

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
    int generation_ = 0;
    std::filesystem::file_time_type cfgTime_{};
    void ApplyConfig(sol::state_view L);
};

}  // namespace f3
