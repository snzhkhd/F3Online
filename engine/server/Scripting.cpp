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
        "hp", sol::property([](Critter& c) { return c.hp; }),
        "max_hp", sol::property([](Critter& c) { return c.maxHp; }),
        "level", sol::property([](Critter& c) { return c.level; }),
        "team", sol::property([](Critter& c) { return c.team; }, [](Critter& c, int t) { c.team = t; }),
        "dead", sol::property([](Critter& c) { return c.dead; }),
        "is_static", sol::property([](Critter& c) { return c.isStatic; }),
        "target", sol::property([](Critter& c) { return c.target; }),
        "script", sol::property([](Critter& c) { return c.script; }),
        "dialog", sol::property([](Critter& c) { return c.dialog; }),
        "proto", sol::property([&S](Critter& c) -> std::string {
            const Proto* p = c.protoPid >= 0 ? S.Data().GetProto((uint16_t)c.protoPid) : nullptr;
            return p ? p->key : std::string();
        }),
        // SPECIAL by index 1..7 (S P E C I A L) or name ("strength", "agility", ...)
        "special", [](Critter& c, sol::object k) -> int {
            static const char* names[7] = {"strength", "perception", "endurance", "charisma", "intelligence", "agility", "luck"};
            if (k.get_type() == sol::type::number) { int i = k.as<int>() - 1; return (i >= 0 && i < 7) ? c.special[i] : 0; }
            if (k.get_type() == sol::type::string) {
                std::string n = k.as<std::string>();
                for (int i = 0; i < 7; i++) if (n == names[i]) return c.special[i];
            }
            return 0;
        },
        "set_special", [](Critter& c, int i, int v) { if (i >= 1 && i <= 7) c.special[i - 1] = std::max(1, std::min(10, v)); },
        "inventory", [&S](Critter& c, sol::this_state ts) {
            sol::state_view sv(ts);
            sol::table t = sv.create_table();
            int n = 1;
            for (auto& it : c.inventory) {
                const Proto* p = S.Data().GetProto(it.pid);
                sol::table e = sv.create_table();
                e["id"] = it.id; e["pid"] = it.pid; e["count"] = it.count; e["equipped"] = it.equipped;
                e["name"] = p ? p->DisplayName() : std::string("?");
                e["proto"] = p ? p->key : std::string();
                t[n++] = e;
            }
            return t;
        },
        "give", [&S](Critter& c, const std::string& key, sol::optional<int> count) -> int {
            const Proto* p = S.Data().FindProto(key);
            if (!p) return 0;
            ItemId id = S.GiveItem(c, *p, count.value_or(1));
            if (c.IsPlayer()) { S.SendInventory(c); S.SendStats(c); }
            return (int)id;
        },
        "equip", [&S](Critter& c, int invId, sol::optional<bool> on) { return S.EquipItem(c, (ItemId)invId, on.value_or(true)); },
        "pickup", [&S](Critter& c, int itemId) { return S.PickUp(c, (ItemId)itemId); },
        "drop", [&S](Critter& c, int invId) { return S.DropItem(c, (ItemId)invId); },
        "use_object", [&S](Critter& c, int objId) { return S.UseObject(c, (ItemId)objId); },
        "attack", [&S](Critter& c, int targetId) { return S.StartAttack(c, (CritterId)targetId); },
        "damage", [&S](Critter& c, int amount, sol::optional<Critter*> by) { S.Damage(c, amount, by.value_or(nullptr)); },
        "heal", [&S](Critter& c, int amount) { S.Heal(c, amount); },
        "kill", [&S](Critter& c) { S.KillCritter(c, nullptr); },
        "enter_map", [&S](Critter& c, const std::string& map, sol::optional<int> entry) { return S.EnterMapByName(c, map, entry.value_or(0)); },
        "weight", [&S](Critter& c) { return S.Weight(c); },
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
        "critters", [](Map& m) { return sol::as_table(std::vector<Critter*>(m.critters)); },
        // map objects: ground items, doors, containers, usables -> { id, name, kind, x, y, proto, open }
        "objects", [&S](Map& m, sol::this_state ts) {
            sol::state_view sv(ts);
            sol::table t = sv.create_table();
            int n = 1;
            static const char* kinds[4] = {"item", "door", "container", "usable"};
            for (auto& [id, it] : m.items) {
                const Proto* p = S.Data().GetProto(it.pid);
                sol::table e = sv.create_table();
                e["id"] = it.id; e["name"] = it.name; e["kind"] = kinds[(int)it.kind]; e["x"] = it.pos.x; e["y"] = it.pos.y;
                e["proto"] = p ? p->key : std::string(); e["open"] = it.open; e["count"] = it.count;
                t[n++] = e;
            }
            return t;
        },
        "entry", [](Map& m, int id, sol::this_state ts) -> sol::object {
            const EntryPoint* ep = nullptr;
            for (auto& e : m.entries) if (e.id == id) ep = &e;
            if (!ep || ep->slots.empty()) return sol::make_object(ts, sol::nil);
            sol::state_view sv(ts);
            sol::table t = sv.create_table();
            t["x"] = ep->slots[0].pos.x; t["y"] = ep->slots[0].pos.y; t["slots"] = (int)ep->slots.size();
            return sol::make_object(sv, t);
        },
        "region_count", [](Map& m) { return (int)m.regions.size(); });

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
    // data-driven content (requires gamedata/server): prototype key like "Weapons44_Revolver.WEA"
    L["data_loaded"] = [&S]() { return S.Data().Loaded(); };
    L["map_count"] = [&W]() { return (int)W.Maps().size(); };
    L["map_names"] = [&W](sol::this_state ts) {
        sol::state_view sv(ts);
        sol::table t = sv.create_table();
        int i = 1;
        for (auto& m : W.Maps()) t[i++] = m->name;
        return t;
    };
    L["proto_info"] = [&S](const std::string& key, sol::this_state ts) -> sol::object {
        sol::state_view sv(ts);
        const Proto* p = S.Data().FindProto(key);
        if (!p) return sol::make_object(sv, sol::nil);
        sol::table t = sv.create_table();
        static const char* cls[8] = {"critter", "weapon", "armor", "ammo", "item", "door", "container", "usable"};
        t["pid"] = p->pid; t["key"] = p->key; t["class"] = cls[(int)p->cls]; t["name"] = p->DisplayName(); t["desc"] = p->desc;
        t["model"] = p->model; t["team"] = p->team; t["script"] = p->script; t["dialog"] = p->dialog;
        t["range"] = p->range; t["caliber"] = p->caliber; t["dmg_min"] = p->wDmgMin; t["dmg_max"] = p->wDmgMax;
        return sol::make_object(sv, t);
    };
    L["spawn_npc_proto"] = [&S](int map, const std::string& key, int x, int y) -> Critter* {
        const Proto* p = S.Data().FindProto(key);
        if (!p || p->cls != PClass::Critter) return nullptr;
        return S.SpawnCritterProto((MapId)map, *p, TilePos{(int16_t)x, (int16_t)y}, nullptr);
    };
    L["spawn_item_proto"] = [&S](int map, const std::string& key, int x, int y, sol::optional<int> count) -> int {
        const Proto* p = S.Data().FindProto(key);
        if (!p) return 0;
        return (int)S.SpawnObjectProto((MapId)map, *p, TilePos{(int16_t)x, (int16_t)y}, (uint16_t)count.value_or(1), nullptr);
    };
    L["get_global"] = [&S](const std::string& k) { return S.GetGlobal(k); };
    L["set_global"] = [&S](const std::string& k, double v) { S.SetGlobal(k, v); };
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

void Scripting::OnNpcTick(Critter& npc, uint64_t tick) {
    if (!L_) return;
    if (npc.npcType.empty()) {  // data-spawned critters pick their behaviour lazily
        sol::object ret;
        npc.npcType = "idle";
        if (lua::CallGlobal(*L_, "pick_npc_type", &ret, &npc) && ret.get_type() == sol::type::string) npc.npcType = ret.as<std::string>();
    }
    CallNpc(npc.npcType, "on_tick", &npc, tick);
}

void Scripting::OnNpcHeard(Critter& npc, Critter& speaker, const std::string& text) {
    CallNpc(npc.npcType, "on_heard", &npc, &speaker, text);
}

}  // namespace f3
