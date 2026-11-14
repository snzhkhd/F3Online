-- F3Online default game logic. Loaded by f3_server at startup and on `reload`.
-- API reference: engine/server/Scripting.h and docs/architecture.md.

log("main.lua: loading")

load_script("npc/guard.lua")
load_script("npc/wanderer.lua")
load_script("npc/hostile.lua")

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
    if data_loaded() and #c:inventory() == 0 then
        -- starter kit (real prototypes from the original data)
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
-- dev helper: /tp x y  (cell coordinates on the current map)
function commands.tp(c, arg)
    local x, y = (arg or ""):match("^(%d+)%s+(%d+)")
    if not x or not c:teleport(c.map_id, tonumber(x), tonumber(y)) then c:tell("usage: /tp x y (walkable cell)") end
end
function commands.heal(c) c:heal(c.max_hp) end
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
