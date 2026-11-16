-- Object script template (docs/object-scripts.md).
--
-- Copy this file to scripts/objects/<name>.lua (or press "Create from template" in the editor, tab "Object scripts"), then bind it to a map
-- object with a record in data/objects/*.json:  { "id": "my_terminal", "map": "<map>", "object": "<instance name>", "script": "objects/<name>.lua" }
--
-- Every hook is optional. `player` is a Critter, `obj` a MapObject handle. Return true from a hook to say "handled": the declarative steps
-- of the binding (on_use / on_look / skills in the JSON record) are then skipped. Keep top-level code to function definitions: the file
-- runs once when the server loads or reloads it (/reload scripts), in its own environment, so names do not clash with other scripts.
--
-- MapObject:  obj.id  obj.name (instance name)  obj.display_name  obj.map  obj.proto  obj.kind  obj.x  obj.y  obj.binding
--             obj.is_open  obj.is_locked  obj:open() :close() :toggle() :lock() :unlock()
--             obj:get_flag(name) / obj:set_flag(name, number)   -- persistent, per object (SQLite world state)
--             obj:used() / obj:set_used(bool)                   -- the one-shot mark
--             obj:start_dialog(player, "dialog_id")
-- Globals:    object_by_name(map, name)  object_by_id(id)  bound_object(binding_id)  start_dialog(player, dialog_id, obj)
--             skill_check(player, "Science", difficulty [, roll])  and the usual player:tell(text), player:give(proto), player:set_gvar(k, v), ...

-- Called when the player uses the object (cursor click). Return true when handled.
function on_use(player, obj)
    player:tell("You use the " .. obj.display_name .. ".")
    return true
end

-- Called when the player uses a skill on the object (skill is the English skill name, e.g. "Science").
-- function on_skill(player, obj, skill)
--     return false   -- false: the engine handles the skill as usual (lockpicking, ...)
-- end

-- Called when the player examines the object.
-- function on_look(player, obj)
--     player:tell("It hums quietly.")
--     return true
-- end

-- Return false to refuse the use before on_use / the steps run (tell the player why).
-- function can_use(player, obj)
--     return true
-- end

-- Called once after the object was bound (server start, /reload).
-- function on_loaded(obj)
-- end
