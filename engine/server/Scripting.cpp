#include "Scripting.h"

#include <vector>
#include "Server.h"
#include "common/log.h"
#include "common/lua_bind.h"

namespace f3 {

Scripting::Scripting(Server& server, std::filesystem::path scriptsDir)
    : server_(server), dir_(std::move(scriptsDir)) {}
Scripting::~Scripting() = default;

static bool SafeRelPath(const std::string& rel) {
    std::filesystem::path p(rel);
    if (p.is_absolute() || p.has_root_name()) return false;
    for (auto& part : p)
        if (part == "..") return false;
    return true;
}

void Scripting::BindApi(sol::state& L) {
    Server& S = server_;
    World& W = S.GetWorld();

    L.new_usertype<Critter>(
        "Critter", sol::no_constructor,
        "id", sol::property([](Critter& c) { return c.id; }),
        "name", sol::property([](Critter& c) { return c.name; }),
        "x", sol::property([](Critter& c) { return c.pos.x; }),
        "y", sol::property([](Critter& c) { return c.pos.y; }),
        "dir", sol::property([](Critter& c) { return static_cast<int>(c.dir); }),
        "map_id", sol::property([](Critter& c) { return c.mapId; }),
        "npc_type", sol::property([](Critter& c) { return c.npcType; }),
        "is_npc", [](Critter& c) { return c.IsNpc(); },
        "is_player", [](Critter& c) { return c.IsPlayer(); },
        "is_moving", [](Critter& c) { return !c.path.empty(); },
        "move_to", [&S](Critter& c, int x, int y) { return S.MoveCritterTo(c, TilePos{(int16_t)x, (int16_t)y}); },
        "stop", [&S](Critter& c) { S.StopCritter(c); },
        "teleport", [&S](Critter& c, int map, int x, int y) { return S.Teleport(c, (MapId)map, TilePos{(int16_t)x, (int16_t)y}); },
        "say", [&S](Critter& c, const std::string& t) { S.Say(c, t); },
        "tell", [&S](Critter& c, const std::string& t) { S.Tell(c, t); },
        "distance_to", [](Critter& c, Critter& o) { return c.mapId == o.mapId ? TileDist(c.pos, o.pos) : 9999; },
        "get_var", [](Critter& c, const std::string& k) { auto it = c.vars.find(k); return it == c.vars.end() ? 0.0 : it->second; },
        "set_var", [](Critter& c, const std::string& k, double v) { c.vars[k] = v; });

    L.new_usertype<Map>(
        "Map", sol::no_constructor,
        "id", sol::property([](Map& m) { return m.id; }),
        "name", sol::property([](Map& m) { return m.name; }),
        "width", sol::property([](Map& m) { return m.width; }),
        "height", sol::property([](Map& m) { return m.height; }),
        "spawn_x", sol::property([](Map& m) { return m.spawn.x; }),
        "spawn_y", sol::property([](Map& m) { return m.spawn.y; }),
        "is_blocked", [](Map& m, int x, int y) { return m.IsBlocked(x, y); },
        "is_free", [](Map& m, int x, int y) { return m.IsFree(TilePos{(int16_t)x, (int16_t)y}); },
        "critters", [](Map& m) { return sol::as_table(std::vector<Critter*>(m.critters)); });

    L["TICK_RATE"] = S.GetConfig().tickRate;
    L["npc_types"] = L.create_table();

    L["log"] = [](const std::string& s) { log::Info("[lua] %s", s.c_str()); };
    L["tick"] = [&S]() { return S.Tick(); };

    L["get_critter"] = [&W](int id) -> Critter* { return W.GetCritter((CritterId)id); };
    L["find_player"] = [&W](const std::string& n) -> Critter* { return W.FindPlayerByName(n); };
    L["get_map"] = [&W](int id) -> Map* { return W.GetMap((MapId)id); };
    L["get_map_by_name"] = [&W](const std::string& n) -> Map* { return W.FindMapByName(n); };
    L["players"] = [&W](sol::this_state ts) {
        sol::state_view sv(ts);
        sol::table t = sv.create_table();
        int i = 1;
        for (auto& [id, c] : W.Critters())
            if (c->IsPlayer() && c->mapId != 0) t[i++] = c.get();
        return t;
    };

    // create_map{ name=, width=, height=, rows={ "####", "#..#" }, spawn={x=,y=} }  ('#' blocks)
    // Idempotent by name, so hot reload and re-running on_init never duplicate maps.
    L["create_map"] = [&S, &W](sol::table def) -> Map* {
        std::string name = def.get_or<std::string>("name", "map");
        if (Map* ex = W.FindMapByName(name)) return ex;
        int w = def.get_or("width", 32), h = def.get_or("height", 32);
        if (w < 1 || h < 1 || w > 1024 || h > 1024) throw std::runtime_error("create_map: bad size");
        std::vector<uint8_t> blocked(static_cast<size_t>(w) * h, 0);
        if (sol::optional<sol::table> rows = def["rows"]) {
            for (int y = 0; y < h; ++y) {
                sol::optional<std::string> row = (*rows)[y + 1];
                if (!row) continue;
                for (int x = 0; x < w && x < (int)row->size(); ++x)
                    if ((*row)[x] == '#') blocked[static_cast<size_t>(y) * w + x] = 1;
            }
        }
        TilePos spawn{(int16_t)(w / 2), (int16_t)(h / 2)};
        if (sol::optional<sol::table> sp = def["spawn"]) {
            spawn.x = (int16_t)sp->get_or("x", (int)spawn.x);
            spawn.y = (int16_t)sp->get_or("y", (int)spawn.y);
        }
        return S.CreateMap(name, w, h, std::move(blocked), spawn);
    };

    L["spawn_npc"] = [&S](int map, const std::string& name, int x, int y, const std::string& type,
                          sol::optional<int64_t> color) -> Critter* {
        return S.SpawnNpc((MapId)map, name, TilePos{(int16_t)x, (int16_t)y}, type,
                          (uint32_t)color.value_or(0xC0C0C0FF));
    };
    L["spawn_item"] = [&S](int map, int pid, const std::string& name, int x, int y) {
        return S.SpawnItem((MapId)map, (uint16_t)pid, name, TilePos{(int16_t)x, (int16_t)y});
    };
    L["remove_critter"] = [&S](int id) { S.RemoveCritter((CritterId)id); };
    L["say"] = [&S](Critter& c, const std::string& t) { S.Say(c, t); };
    L["tell"] = [&S](Critter& c, const std::string& t) { S.Tell(c, t); };
    L["broadcast"] = [&S](const std::string& t) { S.Broadcast(t); };

    // Sandboxed replacement for dofile: paths are relative to the scripts directory.
    L["load_script"] = [this](sol::this_state ts, const std::string& rel) -> sol::object {
        if (!SafeRelPath(rel)) throw std::runtime_error("load_script: path not allowed: " + rel);
        sol::state_view sv(ts);
        sol::object result = sol::make_object(sv, sol::nil);
        if (!lua::RunFile(sv, dir_ / rel, &result)) throw std::runtime_error("load_script failed: " + rel);
        return result;
    };
}

std::unique_ptr<sol::state> Scripting::BuildState() {
    auto L = lua::NewSandboxState();
    BindApi(*L);
    std::filesystem::path main = dir_ / "main.lua";
    if (!std::filesystem::exists(main)) {
        log::Error("scripting: %s not found", main.string().c_str());
        return nullptr;
    }
    if (!lua::RunFile(*L, main)) return nullptr;
    return L;
}

bool Scripting::Load() {
    L_ = BuildState();
    if (!L_) return false;
    ++generation_;
    log::Info("scripting: loaded (generation %d)", generation_);
    lua::CallGlobal(*L_, "on_init", nullptr);
    return true;
}

bool Scripting::Reload() {
    auto fresh = BuildState();
    if (!fresh) {
        log::Error("scripting: reload failed, keeping previous scripts");
        return false;
    }
    L_ = std::move(fresh);
    ++generation_;
    log::Info("scripting: reloaded (generation %d)", generation_);
    lua::CallGlobal(*L_, "on_reload", nullptr);
    return true;
}

bool Scripting::OnLogin(Critter& c) {
    if (!L_) return true;
    sol::object ret;
    if (lua::CallGlobal(*L_, "on_login", &ret, &c) && ret.is<bool>() && !ret.as<bool>()) return false;
    return true;
}

void Scripting::OnLogout(Critter& c) { if (L_) lua::CallGlobal(*L_, "on_logout", nullptr, &c); }

void Scripting::OnCritterMove(Critter& c, TilePos from) {
    if (L_) lua::CallGlobal(*L_, "on_critter_move", nullptr, &c, (int)from.x, (int)from.y);
}

bool Scripting::OnChat(Critter& c, std::string& text) {
    if (!L_) return true;
    sol::object ret;
    if (!lua::CallGlobal(*L_, "on_chat", &ret, &c, text)) return true;
    if (ret.is<bool>() && !ret.as<bool>()) return false;
    if (ret.get_type() == sol::type::string) text = ret.as<std::string>();
    return true;
}

void Scripting::OnTick(uint64_t tick) { if (L_) lua::CallGlobal(*L_, "on_tick", nullptr, tick); }

template <class... A>
void Scripting::CallNpc(const std::string& type, const char* fn, A&&... args) {
    if (!L_ || type.empty()) return;
    sol::object types = (*L_)["npc_types"];
    if (types.get_type() != sol::type::table) return;
    sol::object t = types.as<sol::table>()[type];
    if (t.get_type() != sol::type::table) return;
    sol::object f = t.as<sol::table>()[fn];
    if (f.get_type() != sol::type::function) return;
    sol::protected_function pf = f.as<sol::protected_function>();
    sol::protected_function_result r = pf(std::forward<A>(args)...);
    if (!r.valid()) {
        sol::error e = r;
        log::Error("lua: npc_types.%s.%s: %s", type.c_str(), fn, e.what());
    }
}

void Scripting::OnNpcTick(Critter& npc, uint64_t tick) { CallNpc(npc.npcType, "on_tick", &npc, tick); }

void Scripting::OnNpcHeard(Critter& npc, Critter& speaker, const std::string& text) {
    CallNpc(npc.npcType, "on_heard", &npc, &speaker, text);
}

}  // namespace f3
