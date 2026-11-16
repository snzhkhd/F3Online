-- Lua ports of the Vault/Junktown tutorial object scripts (original AMX: extracted/Scripts/*.scrb, listings via tools/amxdis).
-- The AMX files are templates (damage/sounds/AI state) plus a few custom lines; only the custom, gameplay-relevant parts are ported.
-- The engine has no "use skill" UI yet, so UseSkill(identifier) entry points are mapped onto the plain Use action.
-- Port status table: docs/amx-scripts.md.

local M = {}

local function task_done(c, task, entry)      -- CloseTask(task) + AddEntry(task, entry)
    c:set_gvar("Task" .. task, 2)   -- journal state is per player (the world state below stays shared)
    c:tell_key("msg.task.completed", {text = stf_for(c, task)})
    if entry then c:set_gvar("TaskEntry" .. task, entry); c:tell_key("msg.task.journal", {text = stf_for(c, entry)}) end
end

local handlers = {}

-- 00_TutorialGeneratorRoomDoor01.scrb: OnCreate LockDoor(Myself); UseSkill(3 = lockpick) -> SetGlobal("00_Lock_Status", 1)
-- (OnUpdate then unlocks the door). The engine now owns the lock (Doors.cpp): the door starts locked from GDOR, Security picks it
-- (or the key opens it), and M.on_unlocked below sets the global like the script's UseSkill did. Locked message: stf 1299.

-- 00_TutorialGenerator01.scrb: DoSkillCheck(101 = repair) -> SetGlobal("00_Generator_Status", 1); " Repairing " + smoking/active VEG.
-- The vault computer (00_TutorialComputer01) then sets 00_Life_Support_Status; the tutorial task 3231 "Fix the Generator" is closed
-- (entry 3237).
handlers["00_TutorialGenerator01"] = function(c, id)
    if get_global("00_Generator_Status") ~= 0 then c:tell_key("lua.generator_running"); return end
    set_global("00_Generator_Status", 1)
    set_global("00_Life_Support_Status", 1)
    c:tell_key("lua.repairing")
    task_done(c, 3231, 3237)
end

-- 00_TutorialComputer01.scrb: receives the generator message, sets 00_Life_Support_Status, enables map effect "Life_SupportEffects".
handlers["00_TutorialComputer01"] = function(c, id)
    c:tell_key(get_global("00_Life_Support_Status") ~= 0 and "lua.life_support_on" or "lua.life_support_off")
end

-- 00_Tutorial_Vault_Door_Console.scrb (Junktown): opens the vault entrance doors "Outer_EntranceDS_Doors_000".
handlers["00_Tutorial_Vault_Door_Console"] = function(c, id)
    local m = get_map(c.map_id)
    for _, o in ipairs(m:objects()) do
        if o.kind == "door" and o.name:find("Outer_EntranceDS_Doors", 1, true) then
            set_door_locked(o.id, false); set_door_open(o.id, true)
        end
    end
    set_global("00_Vault_Door_Seen", 1)
end

-- called by main.lua door_use when somebody picked / key-unlocked an object: the picked object's original script decides what that means
function M.on_unlocked(c, obj_id)
    for _, o in ipairs(get_map(c.map_id):objects()) do
        if o.id == obj_id and (o.script or ""):gsub("%.amx$", "") == "00_TutorialGeneratorRoomDoor01" then
            if get_global("00_Lock_Status") == 0 then set_global("00_Lock_Status", 1) end
        end
    end
end

function M.on_use(c, obj_id, proto_key, script)
    local name = (script or ""):gsub("%.amx$", "")
    local h = handlers[name]
    if h then h(c, obj_id) end
end

return M
