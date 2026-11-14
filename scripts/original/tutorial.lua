-- Lua ports of the Vault/Junktown tutorial object scripts (original AMX: extracted/Scripts/*.scrb, listings via tools/amxdis).
-- The AMX files are templates (damage/sounds/AI state) plus a few custom lines; only the custom, gameplay-relevant parts are ported.
-- The engine has no "use skill" UI yet, so UseSkill(identifier) entry points are mapped onto the plain Use action.
-- Port status table: docs/amx-scripts.md.

local M = {}

local function task_done(c, task, entry)      -- CloseTask(task) + AddEntry(task, entry)
    set_global("Task" .. task, 2)
    c:tell("Task completed: " .. stf(task))
    if entry then set_global("TaskEntry" .. task, entry); c:tell("Journal: " .. stf(entry)) end
end

local handlers = {}

-- 00_TutorialGeneratorRoomDoor01.scrb: OnCreate LockDoor(Myself); UseSkill(3 = lockpick) -> SetGlobal("00_Lock_Status", 1)
-- (OnUpdate then unlocks the door). Locked message: "GeneratorRoomDoor01:This Door is Locked".
handlers["00_TutorialGeneratorRoomDoor01"] = function(c, id)
    if get_global("00_Lock_Status") == 0 then
        set_global("00_Lock_Status", 1)
        set_door_locked(id, false)
        c:tell("You pick the lock.")
    end
end

-- 00_TutorialGenerator01.scrb: DoSkillCheck(101 = repair) -> SetGlobal("00_Generator_Status", 1); " Repairing " + smoking/active VEG.
-- The vault computer (00_TutorialComputer01) then sets 00_Life_Support_Status; the tutorial task 3231 "Fix the Generator" is closed
-- (entry 3237).
handlers["00_TutorialGenerator01"] = function(c, id)
    if get_global("00_Generator_Status") ~= 0 then c:tell("The generator is running."); return end
    set_global("00_Generator_Status", 1)
    set_global("00_Life_Support_Status", 1)
    c:tell(" Repairing ")
    task_done(c, 3231, 3237)
end

-- 00_TutorialComputer01.scrb: receives the generator message, sets 00_Life_Support_Status, enables map effect "Life_SupportEffects".
handlers["00_TutorialComputer01"] = function(c, id)
    c:tell(get_global("00_Life_Support_Status") ~= 0 and "Life support: online." or "Life support: offline (low power).")
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

function M.on_use(c, obj_id, proto_key, script)
    local name = (script or ""):gsub("%.amx$", "")
    local h = handlers[name]
    if h then h(c, obj_id) end
end

return M
