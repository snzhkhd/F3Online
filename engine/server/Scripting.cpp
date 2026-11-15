#include "Scripting.h"

#include <algorithm>
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
        "is_moving", [](Critter& c) { return c.IsMoving(); },
        // continuous position in cell units (cell centre = x + 0.5)
        "fx", sol::property([](Critter& c) { return c.fx; }),
        "fy", sol::property([](Critter& c) { return c.fy; }),
        // ---- real-time timing (FOnline style): Wait/breaktime, AP (1/100 AP), battle flag
        "wait", [&S](Critter& c, int ms) { S.Wait(c, ms); },
        "is_free", [&S](Critter& c) { return S.IsFree(c) && !c.dead; },
        "is_busy", [&S](Critter& c) { return !S.IsFree(c); },
        "wait_left", [&S](Critter& c) { uint64_t n = S.NowMs(); return c.waitEndMs > n ? (int)(c.waitEndMs - n) : 0; },
        "ap", [&S](Critter& c) { S.SyncAp(c); return c.ap; },
        "max_ap", [&S](Critter& c) { return S.MaxAp(c); },
        "set_ap", [&S](Critter& c, int v) { S.SyncAp(c); c.ap = std::clamp(v, 0, S.MaxAp(c)); S.SendWaitState(c); },
        "use_ap", [&S](Critter& c, int cost) { bool ok = S.SpendAp(c, cost); if (ok) S.SendWaitState(c); return ok; },
        "in_battle", [&S](Critter& c) { return S.NowMs() < c.battleEndMs; },
        "set_battle", [&S](Critter& c) { S.SetBattle(c); },
        "set_battle_ms", [&S](Critter& c, int ms) { S.SetBattle(c, true, ms); },
        "think_due", [&S](Critter& c, int ms) {  // true at most once per `ms` (staggered by id): npc think interval, config.CritterIdleTick
            uint64_t n = S.NowMs();
            if (n < c.nextThinkMs) return false;
            c.nextThinkMs = n + (uint64_t)std::max(1, ms) + (c.id * 37) % 50;
            return true;
        },
        "hp", sol::property([](Critter& c) { return c.hp; }),
        "max_hp", sol::property([](Critter& c) { return c.maxHp; }),
        "level", sol::property([](Critter& c) { return c.level; }),
        "xp", sol::property([](Critter& c) { return c.xp; }),
        "skill_points", sol::property([](Critter& c) { return c.skillPoints; }),
        "perk_points", sol::property([](Critter& c) { return c.perkPoints; }),
        "created", sol::property([](Critter& c) { return c.created; }),
        "give_xp", [&S](Critter& c, int n) { S.AddXp(c, n); },
        "skill", [&S](Critter& c, int i) { return S.SkillValue(c, i - 1); },   // 1-based like special(): 1 Firearms .. 14 Traps

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
        "move_to", [&S](Critter& c, int x, int y, sol::optional<bool> run) {
            return S.MoveCritterTo(c, TilePos{(int16_t)x, (int16_t)y}, run ? (*run ? 1 : 0) : -1);
        },
        "stop", [&S](Critter& c) { S.StopCritter(c); },
        "teleport", [&S](Critter& c, int map, int x, int y) { return S.Teleport(c, (MapId)map, TilePos{(int16_t)x, (int16_t)y}); },
        "say", [&S](Critter& c, const std::string& t) { S.Say(c, t); },
        "tell", [&S](Critter& c, const std::string& t) { S.Tell(c, t); },
        "distance_to", [](Critter& c, Critter& o) { return c.mapId == o.mapId ? TileDist(c.pos, o.pos) : 9999; },
        "get_var", [](Critter& c, const std::string& k) { auto it = c.vars.find(k); return it == c.vars.end() ? 0.0 : it->second; },
        "set_var", [](Critter& c, const std::string& k, double v) { c.vars[k] = v; },
        "get_gvar", [&S](Critter& c, const std::string& k) { return S.GetGlobalFor(&c, k); },
        "set_gvar", [&S](Critter& c, const std::string& k, double v) { S.SetGlobalFor(&c, k, v); });

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
                e["proto"] = p ? p->key : std::string(); e["open"] = it.open; e["count"] = it.count; e["script"] = it.script; e["locked"] = it.locked;
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
    L["now_ms"] = [&S]() { return (double)S.NowMs(); };  // server clock in ms (the clock of Wait/WaitState)

    L["set_force_miss"] = [&S](bool on) { S.Rt().ForceMiss = on; };   // dev/test only
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
    L["dev_kit"] = [&S]() { return S.GetConfig().devKit; };
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
    // dev / test only (main.lua gates the /amxnative command on --dev-kit): run natives by name, see AmxHost::NativeTest
    L["amx_native"] = [&S](int owner, const std::string& program) -> std::string { return S.Amx() ? S.Amx()->NativeTest(static_cast<CritterId>(owner), program) : std::string("amx not available"); };
    L["amx_attacks"] = [&S](bool on) { if (S.Amx()) S.Amx()->SetAttacksEnabled(on); };   // dev / test only
    L["stf"] = [&S](int id) { return S.Data().String(id); };   // English.stf text by id
    L["set_door_locked"] = [&S](int objId, bool locked) { Item* d = S.FindObject((ItemId)objId); if (!d || d->kind != ObjKind::Door) return false; S.SetDoorLocked(*d, locked); return true; };
    L["set_door_open"] = [&S](int objId, bool open) {
        Map* m = nullptr; Item* d = S.FindObject((ItemId)objId, &m);
        if (!d || !m || d->kind != ObjKind::Door) return false;
        S.SetDoor(*m, *d, open); return true;
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
    std::filesystem::path cfgPath = dir_ / "config.lua";
    if (std::filesystem::exists(cfgPath) && !lua::RunFile(*L, cfgPath)) return nullptr;
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
    ApplyConfig(*L_);
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
    ApplyConfig(*L_);
    log::Info("scripting: reloaded (generation %d)", generation_);
    lua::CallGlobal(*L_, "on_reload", nullptr);
    return true;
}

// Reads the global table `config` (FOnline names, see scripts/config.lua) into the server GameConfig.
void Scripting::ApplyConfig(sol::state_view L) {
    GameConfig& g = server_.Rt();
    g = GameConfig{};
    std::error_code ec;
    cfgTime_ = std::filesystem::last_write_time(dir_ / "config.lua", ec);
    sol::object o = L["config"];
    if (o.get_type() != sol::type::table) { log::Warn("scripting: no config table (scripts/config.lua), using defaults"); return; }
    sol::table t = o.as<sol::table>();
#define F3_CFG_INT(name) g.name = t.get_or("" #name, g.name)
#define F3_CFG_NUM(name) g.name = t.get_or("" #name, g.name)
    F3_CFG_INT(CritterIdleTick); F3_CFG_INT(Breaktime); F3_CFG_INT(TimeoutTransfer); F3_CFG_INT(TimeoutBattle);
    F3_CFG_INT(ApRegeneration); F3_CFG_INT(RtApCostCritterWalk); F3_CFG_INT(RtApCostCritterRun);
    F3_CFG_INT(RtApCostMoveItemContainer); F3_CFG_INT(RtApCostMoveItemInventory); F3_CFG_INT(RtApCostPickItem);
    F3_CFG_INT(RtApCostDropItem); F3_CFG_INT(RtApCostReloadWeapon); F3_CFG_INT(RtApCostPickCritter);
    F3_CFG_INT(RtApCostUseItem); F3_CFG_INT(RtApCostUseSkill);
    F3_CFG_INT(RtAlwaysRun); F3_CFG_INT(RunOnCombat);
    F3_CFG_INT(ApBase); F3_CFG_INT(ApAgilityDiv); F3_CFG_NUM(WalkSpeed); F3_CFG_NUM(RunSpeed); F3_CFG_NUM(MoveClearance);
    F3_CFG_INT(AttackMinMs); F3_CFG_INT(AttackMaxMs); F3_CFG_INT(AttackMsPerCost);
    F3_CFG_INT(RtApCostAttackPerCost); F3_CFG_INT(RtApCostAttackMin); F3_CFG_INT(TransferLock); F3_CFG_INT(AggroAlertRadius); F3_CFG_INT(ForceMiss);
    F3_CFG_INT(CarryBase); F3_CFG_INT(CarryPerStrength);
    F3_CFG_INT(WeightWeapon); F3_CFG_INT(WeightArmor); F3_CFG_INT(WeightAmmo); F3_CFG_INT(WeightItem);
    F3_CFG_INT(RequireCharacterCreation); F3_CFG_INT(SpecialBase); F3_CFG_INT(CharPoints); F3_CFG_INT(SpecialMin); F3_CFG_INT(SpecialMax);
    F3_CFG_INT(TagCount); F3_CFG_INT(TraitMax); F3_CFG_INT(SkillUseRange); F3_CFG_INT(TagBonus); F3_CFG_INT(SkillMax); F3_CFG_INT(StartSkillPoints);
    F3_CFG_INT(ViewBase); F3_CFG_INT(ViewPerPerception); F3_CFG_INT(SkillRateBase); F3_CFG_INT(SkillRatePerInt); F3_CFG_INT(XpBase); F3_CFG_INT(MaxLevel); F3_CFG_INT(PerkRate);
    F3_CFG_INT(HpBase); F3_CFG_INT(HpPerLevelBase); F3_CFG_INT(KillXpBase); F3_CFG_INT(KillXpPerHp);
#undef F3_CFG_INT
#undef F3_CFG_NUM
    {
        sol::object w = t["ItemWeights"];
        if (w.get_type() == sol::type::table)
            for (auto& kv : w.as<sol::table>())
                if (kv.first.is<std::string>() && kv.second.is<int>()) g.ItemWeights[kv.first.as<std::string>()] = kv.second.as<int>();
    }
    {
        sol::object w = t["XpByProto"];
        if (w.get_type() == sol::type::table)
            for (auto& kv : w.as<sol::table>())
                if (kv.first.is<std::string>() && kv.second.is<int>()) g.XpByProto[kv.first.as<std::string>()] = kv.second.as<int>();
    }
    g.SpecialMin = std::clamp(g.SpecialMin, 1, 10);
    g.SpecialMax = std::clamp(g.SpecialMax, g.SpecialMin, 10);
    g.TagCount = std::clamp(g.TagCount, 0, kSkillCount);
    g.TraitMax = std::clamp(g.TraitMax, 0, 16);
    g.SkillUseRange = std::clamp(g.SkillUseRange, 1, 40);
    g.MaxLevel = std::clamp(g.MaxLevel, 1, 99);
    g.SkillMax = std::clamp(g.SkillMax, 1, 255);
    g.Breaktime = std::max(0, g.Breaktime);
    g.WalkSpeed = std::clamp(g.WalkSpeed, 0.1, 100.0);
    g.RunSpeed = std::clamp(g.RunSpeed, 0.1, 100.0);
    log::Info("config: Breaktime=%d TimeoutBattle=%d ApRegeneration=%d RtAlwaysRun=%d walk=%.1f run=%.1f cells/s", g.Breaktime,
              g.TimeoutBattle, g.ApRegeneration, g.RtAlwaysRun ? 1 : 0, g.WalkSpeed, g.RunSpeed);
}

bool Scripting::LoadConfig() {
    if (!L_) return false;
    std::filesystem::path p = dir_ / "config.lua";
    if (!std::filesystem::exists(p)) return false;
    // run on a scratch copy first so a broken edit keeps the previous values
    auto probe = lua::NewSandboxState();
    if (!lua::RunFile(*probe, p)) { log::Error("config: reload failed, keeping previous values"); return false; }
    if (!lua::RunFile(*L_, p)) return false;
    ApplyConfig(*L_);
    return true;
}

void Scripting::PollConfig() {
    std::error_code ec;
    auto t = std::filesystem::last_write_time(dir_ / "config.lua", ec);
    if (ec || t == cfgTime_) return;
    cfgTime_ = t;
    log::Info("config.lua changed, reloading");
    LoadConfig();
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
