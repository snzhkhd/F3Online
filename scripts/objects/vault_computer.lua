-- Sample: the Vault 13 maintenance terminal (data/objects/_samples.json, binding "vault_computer").
-- Using it opens a data dialog; using the Science skill on it tries to bypass the security and, on success, releases the overseer door.
local DOOR = "Overseer_Door01VaultOverseer_000"

function on_use(player, obj)
    return start_dialog(player, "vault_terminal", obj)
end

function on_skill(player, obj, skill)
    if skill ~= "Science" then return false end
    if obj:get_flag("hacked") == 1 then
        player:tell("The terminal is already unlocked.")
        return true
    end
    if skill_check(player, "Science", 50, true) then   -- a percent roll: chance = Science - 50 (clamped)
        obj:set_flag("hacked", 1)                     -- persists across restarts
        player:set_gvar("vault_terminal_hacked", 1)   -- read by the dialog's start route
        local door = object_by_name(obj.map, DOOR)
        if door then door:unlock(); door:open() end
        player:tell("You bypass the security. The overseer door unlocks.")
    else
        player:tell("The terminal rejects your attempt.")
    end
    return true
end
