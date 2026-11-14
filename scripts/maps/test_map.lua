-- Sample map definition. '#' tiles block movement. Returned table is passed to create_map().
local W, H = 48, 32
local rows = {}
for y = 0, H - 1 do
    local r = {}
    for x = 0, W - 1 do
        local c = "."
        if x == 0 or y == 0 or x == W - 1 or y == H - 1 then c = "#" end
        -- a walled compound with a door on the south side
        if x >= 18 and x <= 34 and y >= 6 and y <= 18 then
            if x == 18 or x == 34 or y == 6 or y == 18 then c = "#" end
            if y == 18 and x >= 25 and x <= 27 then c = "." end -- door
        end
        r[#r + 1] = c
    end
    rows[#rows + 1] = table.concat(r)
end
-- some crates / rubble outside
for _, p in ipairs({ {6, 6}, {7, 6}, {7, 7}, {12, 24}, {13, 24}, {40, 10}, {40, 11}, {41, 11}, {38, 26} }) do
    local row = rows[p[2] + 1]
    rows[p[2] + 1] = row:sub(1, p[1]) .. "#" .. row:sub(p[1] + 2)
end

return {
    name = "test_map",
    width = W,
    height = H,
    rows = rows,
    spawn = { x = 24, y = 22 },
}
