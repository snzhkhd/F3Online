-- F3Online default game logic. Loaded by f3_server at startup and on `reload`.
-- API reference: engine/server/Scripting.h and docs/architecture.md.

log("main.lua: loading")

load_script("npc/guard.lua")
load_script("npc/wanderer.lua")

-- Called once, first successful load: build the world.
function on_init()
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
    c:tell("Commands: /help /who /pos /steps /reload-info")
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
commands["reload-info"] = function(c)
    c:tell("Admins can type 'reload' in the server console to hot-reload scripts.")
end

-- Return false to swallow the message, a string to replace it, nothing to pass it through.
function on_chat(c, text)
    local cmd = text:match("^/(%S+)")
    if cmd then
        local f = commands[cmd:lower()]
        if f then f(c) else c:tell("Unknown command: /" .. cmd) end
        return false
    end
end

function on_tick(t)
    if t % (TICK_RATE * 60) == 0 then
        log("heartbeat: " .. #players() .. " player(s) online")
    end
end
