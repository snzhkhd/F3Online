-- Sample: a one-shot lever (data/objects/_samples.json, binding "vault_lever": "one_shot": true, label "Pull").
-- The binding makes it one-shot: after the first successful use the server answers "There is nothing more to do here."
function on_use(player, obj)
    local door = object_by_name(obj.map, "Door02Vault13_000")
    if door then door:unlock(); door:open() end
    player:tell("You pull the lever. Somewhere a door grinds open.")
    return true
end
