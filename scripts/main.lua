-- F3Online default game logic. Loaded by f3_server at startup and on `reload`.
-- API reference: engine/server/Scripting.h and docs/architecture.md.

log("main.lua: loading")

load_script("npc/guard.lua")
load_script("npc/wanderer.lua")
load_script("npc/hostile.lua")
local original_tutorial = load_script("original/tutorial.lua")

-- map object used (door, container, usable); script = original AMX script name of the object
function on_use(c, obj_id, proto_key, script)
    if original_tutorial then original_tutorial.on_use(c, obj_id, proto_key, script) end
end

-- Behaviour of critters spawned from the original map data (called once per critter, first tick).
function pick_npc_type(npc)
    if npc.is_static then return "idle" end
    if npc.team == 2 then return "hostile" end
    return "idle"
end

-- Called once, first successful load: build the world.
function on_init()
    if map_count() > 0 then
        log("world initialised from game data: " .. map_count() .. " maps")
        return
    end
    -- no gamedata/server: fall back to the built-in test map
    local def = load_script("maps/test_map.lua")
    local map = create_map(def)

    spawn_npc(map.id, "Vault Guard", 26, 14, "guard", 0x4070FFFF)
    spawn_npc(map.id, "Radroach", 10, 8, "wanderer", 0xA06030FF)
    spawn_npc(map.id, "Dogmeat", 30, 22, "wanderer", 0xD0B060FF)

    spawn_item(map.id, 1, "Stimpak", 22, 12)
    spawn_item(map.id, 2, "10mm Pistol", 28, 16)
    spawn_item(map.id, 3, "Nuka-Cola", 8, 20)
    log("world initialised: map '" .. map.name .. "' id " .. map.id)
end

-- Called after every successful hot reload (state is rebuilt, world is kept).
function on_reload()
    broadcast("[server] scripts reloaded")
end

function on_login(c)
    -- A new character has NOTHING equipped and an empty inventory (it looks like the base body). The test kit below is
    -- only given when the server runs with --dev-kit.
    if data_loaded() and dev_kit() and c:get_var("kit_given") == 0 and #c:inventory() == 0 then
        c:set_var("kit_given", 1)
        -- dev starter kit (real prototypes from the original data)
        local gun = c:give("Weapons9mm_Autoloader.WEA")
        c:give("Ammo9mm.AMO", 30)
        local arm = c:give("ArmorLeather_Outfit.ARM")
        c:give("Weapons44_Revolver.WEA")
        if gun > 0 then c:equip(gun, true) end
        if arm > 0 then c:equip(arm, true) end
    end
    c:tell("Welcome to F3Online, " .. c.name .. "! Click to move, Enter to chat. Try /help")
    broadcast(c.name .. " entered the wasteland")
    return true
end

-- Skill use hook (C2S_UseSkill: the client picks a skill, then clicks a target; the server has already checked that the target
-- exists on this map and is within SkillUseRange tiles, and checked / spent the AP): use_skill(cr, skill_name, value, kind, target_id)
-- kind 0 = yourself, 1 = another critter, 2 = map object (door / container / item / usable). Return true when the skill did something.
function use_skill(c, skill, value, kind, target)
    local t = (kind == 1 and target ~= 0) and get_critter(target) or c
    local obj = nil
    if kind == 2 then
        for _, o in ipairs(get_map(c.map_id):objects()) do
            if o.id == target then obj = o; break end
        end
        if not obj then c:tell("That object is not here."); return true end
    end
    local who = (t == c) and "yourself" or t.name
    if skill == "Medic" then
        if obj then c:tell("First aid does nothing for " .. obj.name .. "."); return true end
        if t.dead then c:tell("It is too late for first aid."); return true end
        if t.hp >= t.max_hp then c:tell((t == c and "You are" or t.name .. " is") .. " not hurt."); return true end
        if t ~= c and c:distance_to(t) > 4 then c:tell("You are too far away."); return true end
        local roll = math.random(1, 100)
        if roll > math.max(value, 15) then c:tell("Your first aid attempt fails."); return true end
        local heal = math.random(1, 4) + math.floor(value / 10)
        t:heal(heal)
        c:tell(string.format("You treat %s for %d hit points.", who, heal))
        c:give_xp(5)
        return true
    elseif skill == "Security" and obj and obj.kind == "door" then
        if not obj.locked then c:tell("The " .. obj.name .. " is not locked."); return true end
        if math.random(1, 100) > math.max(value, 10) then c:tell("You fail to pick the lock."); return true end
        set_door_locked(obj.id, false)
        c:tell("You pick the lock of the " .. obj.name .. ".")
        c:give_xp(10)
        return true
    elseif skill == "Science" and kind == 1 and t ~= c then
        c:tell(string.format("%s: %d / %d hit points%s.", t.name, t.hp, t.max_hp, t.dead and " (dead)" or ""))
        return true
    elseif skill == "Outdoorsman" or skill == "Science" or skill == "Mechanics" or skill == "Security" or skill == "Traps" then
        c:tell(string.format("%s (%d%%) on %s: nothing here that needs it yet.", skill, value, obj and ("the " .. obj.name) or who))
        return true
    end
    return false
end

function on_logout(c)
    broadcast(c.name .. " left the wasteland")
end

function on_critter_move(c, from_x, from_y)
    if c:is_player() then
        c:set_var("steps", c:get_var("steps") + 1)
    end
end

local commands = {}
function commands.help(c)
    c:tell("Commands: /help /who /pos /steps /ap /stats /inv /maps /goto <map> [entry] /heal /reload-info")
end
function commands.who(c)
    local names = {}
    for _, p in ipairs(players()) do names[#names + 1] = p.name end
    c:tell("Online (" .. #names .. "): " .. table.concat(names, ", "))
end
function commands.pos(c)
    c:tell(string.format("You are at %d,%d on map %d", c.x, c.y, c.map_id))
end
function commands.steps(c)
    c:tell("Steps taken: " .. math.floor(c:get_var("steps")))
end
function commands.stats(c)
    c:tell(string.format("HP %d/%d  S%d P%d E%d C%d I%d A%d L%d  (carry %d)", c.hp, c.max_hp,
        c:special(1), c:special(2), c:special(3), c:special(4), c:special(5), c:special(6), c:special(7), c:weight()))
end
function commands.inv(c)
    local parts = {}
    for _, it in ipairs(c:inventory()) do
        parts[#parts + 1] = it.name .. (it.count > 1 and (" x" .. it.count) or "") .. (it.equipped and " [E]" or "")
    end
    c:tell("Inventory: " .. (#parts > 0 and table.concat(parts, ", ") or "(empty)"))
end
function commands.maps(c)
    c:tell(table.concat(map_names(), ", "))
end
-- dev: /forcemiss [on|off]: every attack misses (tests NPC aggro on a miss)
function commands.forcemiss(c, arg) set_force_miss(arg ~= "off"); c:tell("force miss " .. ((arg ~= "off") and "on" or "off")) end
-- dev helper: /tp x y  (cell coordinates on the current map)
function commands.tp(c, arg)
    local x, y = (arg or ""):match("^(%d+)%s+(%d+)")
    if not x or not c:teleport(c.map_id, tonumber(x), tonumber(y)) then c:tell("usage: /tp x y (walkable cell)") end
end
-- dev: /glob <name> prints a global; /usescript <original script name> runs the Lua port of that object script's use handler
function commands.glob(c, arg) c:tell("global " .. tostring(arg) .. " = " .. tostring(get_global(arg or ""))) end
function commands.usescript(c, arg) original_tutorial.on_use(c, 0, "", arg) end
-- dev (--dev-kit only): /amxnative <program> runs original AMX natives by name for tests, e.g. /amxnative Distance 12 13 (see AmxHost::NativeTest)
-- dev (--dev-kit only): /amxattacks on|off switches the attacks queued by A_Attack / A_JustAttack (tests turn the scripted NPC aggression off)
function commands.amxattacks(c, arg)
    if not dev_kit() then c:tell("amxattacks needs a --dev-kit server"); return end
    amx_attacks((arg or "") ~= "off")
    c:tell("amx attacks " .. ((arg or "") ~= "off" and "on" or "off"))
end
function commands.amxnative(c, arg)
    if not dev_kit() then c:tell("amxnative needs a --dev-kit server"); return end
    c:tell("amx: " .. amx_native(c.id, arg or ""))
end

-- dev: /near <name or dialogue part>: teleport next to the first critter of this map whose name/dialogue contains the text (e.g. /near Armstrong)
function commands.near(c, arg)
    local m = get_map(c.map_id)
    local want = (arg or ''):lower()
    if not m or want == '' then c:tell('usage: /near <name part>'); return end
    for _, o in ipairs(m:critters()) do
        if o.id ~= c.id and (o.name .. ' ' .. o.dialog):lower():find(want, 1, true) then
            for dx = -2, 2 do for dy = -2, 2 do
                if m:is_free(o.x + dx, o.y + dy) and c:teleport(c.map_id, o.x + dx, o.y + dy) then return end
            end end
        end
    end
    local names = {}
    for _, o in ipairs(m:critters()) do names[#names + 1] = o.name end
    c:tell('nothing named like that on this map (' .. #names .. ' critters: ' .. table.concat(names, ', ', 1, math.min(#names, 6)) .. ')')
end

-- dev (--dev-kit): /killnear [name part]  kills the nearest living NPC without a dialogue (and within 20 cells) whose name contains the text (corpse / loot tests)
function commands.killnear(c, arg)
    if not dev_kit() then c:tell("killnear needs a --dev-kit server"); return end
    local m = get_map(c.map_id)
    local want = (arg or ''):lower()
    local best, bd = nil, 1e9
    if m then
        for _, o in ipairs(m:critters()) do
            if o:is_npc() and o.dialog == '' and not o.dead and o.name:lower():find(want, 1, true) then
                local d = math.max(math.abs(o.x - c.x), math.abs(o.y - c.y))
                if d < bd and d <= 30 then best, bd = o, d end
            end
        end
    end
    if best then best:kill() else c:tell('nothing alive named like that') end
end

function commands.heal(c) c:heal(c.max_hp) end
-- dev helpers for the character system: /givexp n, /sheet
function commands.givexp(c, arg) c:give_xp(math.floor(tonumber(arg or "") or 100)) end
function commands.sheet(c)
    local names = {"Firearms", "Melee", "Unarmed", "Barter", "Persuasion", "Deception", "Mechanics", "Medic", "Outdoorsman", "Science", "Security", "Sneak", "Steal", "Traps"}
    local t = {}
    for i, n in ipairs(names) do t[#t + 1] = n .. " " .. c:skill(i) end
    c:tell(string.format("Level %d  XP %d  skill points %d  perk points %d", c.level, c.xp, c.skill_points, c.perk_points))
    c:tell(table.concat(t, ", "))
end
-- dev helper: /gvar name [value]  reads / sets a per player (persisted) quest / script global
function commands.gvar(c, arg)
    local k, v = (arg or ""):match("^(%S+)%s*(%S*)$")
    if not k then c:tell("usage: /gvar name [value]"); return end
    if v ~= "" and tonumber(v) then c:set_gvar(k, tonumber(v)) end
    c:tell(k .. " = " .. tostring(c:get_gvar(k)))
end
-- dev helper: /battle [ms]  sets the battle flag for ms (default TimeoutBattle); walking must still work while it is set
function commands.battle(c, arg)
    local ms = tonumber(arg or "")
    if ms then c:set_battle_ms(math.floor(ms)) else c:set_battle() end
end
-- real-time state: AP (1/100), wait (breaktime) left in ms, battle flag, continuous position in cells
function commands.ap(c)
    c:tell(string.format("AP %.2f/%.2f  wait %d ms  battle %s  pos %.2f,%.2f  (Breaktime %d, ApRegeneration %d)", c:ap() / 100, c:max_ap() / 100,
        c:wait_left(), tostring(c:in_battle()), c.fx, c.fy, config.Breaktime, config.ApRegeneration))
end
-- dev helper: /spawn <proto key>  puts a real item (e.g. Weapons44_Revolver.WEA) two cells next to you
function commands.spawn(c, arg)
    if not arg or not data_loaded() then c:tell("usage: /spawn Weapons44_Revolver.WEA"); return end
    if spawn_item_proto(c.map_id, arg, c.x + 2, c.y, 1) == 0 then c:tell("unknown prototype: " .. arg) end
end
commands["reload-info"] = function(c)
    c:tell("Admins can type 'reload' in the server console to hot-reload scripts.")
end

-- Return false to swallow the message, a string to replace it, nothing to pass it through.
function on_chat(c, text)
    local goto_map, goto_entry = text:match("^/goto%s+(%S+)%s*(%d*)")
    if goto_map then
        if not c:enter_map(goto_map, tonumber(goto_entry) or 0) then c:tell("No such map: " .. goto_map) end
        return false
    end
    local cmd = text:match("^/(%S+)")
    if cmd then
        local f = commands[cmd:lower()]
        if f then f(c, text:match("^/%S+%s+(.+)$")) else c:tell("Unknown command: /" .. cmd) end
        return false
    end
end

function on_tick(t)
    if t % (TICK_RATE * 60) == 0 then
        log("heartbeat: " .. #players() .. " player(s) online")
    end
end
