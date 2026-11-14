-- Hostile NPCs (team 2 in the original data): attack the nearest living player/team-1 critter in sight range.
npc_types.hostile = {
    on_tick = function(npc, t)
        if npc.dead or npc.target ~= 0 then return end
        if t % 10 ~= npc.id % 10 then return end
        local map = get_map(npc.map_id)
        if not map then return end
        local best, bestd = nil, 28
        for _, c in ipairs(map:critters()) do
            if not c.dead and c.team == 1 then
                local d = npc:distance_to(c)
                if d < bestd then best, bestd = c, d end
            end
        end
        if best then npc:attack(best.id) end
    end,
}

-- Does nothing (props, bystanders). Data-spawned critters use this unless pick_npc_type says otherwise.
npc_types.idle = {}
