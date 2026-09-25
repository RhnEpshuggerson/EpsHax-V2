-- GrowPai Bridge Script v3 (main-thread safe — uses timer callback)
-- Dumps game state for EpsHax to read. Paste into GrowPai and execute.

local bridgePath = nil
local dumpCount = 0

if io == nil then
    log("[BRIDGE] io not available")
    return
end

local candidates = {
    "C:\\temp\\growpai_bridge.json",
    "C:/temp/growpai_bridge.json",
}
local tmp = os.getenv and os.getenv("TEMP")
if tmp then candidates[#candidates+1] = tmp .. "\\growpai_bridge.json" end

for _, p in ipairs(candidates) do
    local f = io.open(p, "w")
    if f then f:write("{}") f:close() bridgePath = p break end
end
if not bridgePath then
    log("[BRIDGE] no writable path")
    return
end
log("[BRIDGE] Writing to " .. bridgePath)

local function esc(s)
    if type(s) ~= "string" then return tostring(s) end
    return '"' .. s:gsub('[\\"]', function(c) return '\\' .. c end) .. '"'
end

local function addObj(t)
    local ps = {}
    for k, v in pairs(t) do
        local vs
        if type(v) == "string" then vs = esc(v)
        elseif type(v) == "boolean" then vs = tostring(v)
        elseif type(v) == "table" then
            if #v > 0 then
                local items = {}
                for _, e in ipairs(v) do items[#items + 1] = addObj(e) end
                vs = "[" .. table.concat(items, ",") .. "]"
            else
                vs = addObj(v)
            end
        else vs = tostring(v) end
        ps[#ps + 1] = esc(k) .. ":" .. vs
    end
    return "{" .. table.concat(ps, ",") .. "}"
end

local function dumpState()
    dumpCount = dumpCount + 1
    local data = {}

    local ok1, me = pcall(GetLocal)
    if ok1 and me then
        data.localPlayer = {
            name = me.name or "",
            world = me.world or "",
            country = me.country or "",
            pos_x = me.pos_x or 0,
            pos_y = me.pos_y or 0,
            tile_x = me.tile_x or 0,
            tile_y = me.tile_y or 0,
            netid = me.netid or -1,
            userid = me.userid or 0,
            gems = me.gems or 0,
            size_x = me.size_x or 1,
            size_y = me.size_y or 1,
            facing_left = me.facing_left or false,
            flags = me.flags or 0,
            flags2 = me.flags2 or 0,
        }
    end

    local ok2, players = pcall(GetPlayers)
    if ok2 and type(players) == "table" then
        data.players = {}
        for _, p in pairs(players) do
            data.players[#data.players + 1] = {
                name = p.name or "",
                world = p.world or "",
                netid = p.netid or -1,
                userid = p.userid or 0,
                gems = p.gems or 0,
                pos_x = p.pos_x or 0,
                pos_y = p.pos_y or 0,
                tile_x = p.tile_x or 0,
                tile_y = p.tile_y or 0,
            }
        end
    end

    -- Heavy calls only every 4th dump
    if dumpCount % 4 == 1 then
        local ok3, inv = pcall(GetInventory)
        if ok3 and type(inv) == "table" then
            data.inventory = {}
            for _, it in pairs(inv) do
                data.inventory[#data.inventory + 1] = { id = it.id or 0, count = it.count or 0 }
            end
        end
        local ok4, objs = pcall(GetObjects)
        if ok4 and type(objs) == "table" then
            data.objects = {}
            for _, o in pairs(objs) do
                data.objects[#data.objects + 1] = {
                    id = o.id or 0, oid = o.oid or 0,
                    pos_x = o.pos_x or 0, pos_y = o.pos_y or 0,
                    count = o.count or 0,
                }
            end
        end
    end

    local ok5, ping = pcall(GetPing)
    if ok5 then data.ping = ping or 0 end
    data.timestamp = os.time and os.time() or 0

    local json = addObj(data)
    local f = io.open(bridgePath, "w")
    if f then
        f:write(json)
        f:close()
        if dumpCount <= 3 or dumpCount % 10 == 0 then
            log("[BRIDGE] dump #" .. dumpCount .. " ok, " .. #json .. " bytes")
        end
    else
        log("[BRIDGE] write failed")
    end
end

-- Use GrowPai's timer API (runs on main thread, no background thread freeze)
local okCB = pcall(function()
    AddCallback("timer", "OnUpdate", function(deltatime)
        timer.Update(deltatime)
    end)
end)
if not okCB then
    log("[BRIDGE] AddCallback failed, fallback single dump")
    pcall(dumpState)
    return
end

local okT = pcall(function()
    timer.Create("eps_bridge", 5, 0, function()
        pcall(dumpState)
    end)
end)
if not okT then
    log("[BRIDGE] timer.Create failed, single dump only")
    pcall(dumpState)
else
    log("[BRIDGE] timer started (every 5s)")
end
