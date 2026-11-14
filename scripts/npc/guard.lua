-- Stationary guard: greets players who talk nearby.
npc_types.guard = {
    on_heard = function(npc, speaker, text)
        if text:lower():find("hello") or text:lower():find("hi") then
            npc:say("Halt, " .. speaker.name .. ". Keep your weapon holstered in the Vault.")
        end
    end,
}
