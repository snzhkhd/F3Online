-- Wanders to random nearby tiles every few seconds.
npc_types.wanderer = {
    on_tick = function(npc, t)
        if npc:is_moving() then return end
        if t % (TICK_RATE * 3) ~= (npc.id % (TICK_RATE * 3)) then return end
        local tx = npc.x + math.random(-6, 6)
        local ty = npc.y + math.random(-6, 6)
        npc:move_to(tx, ty)
    end,
}
