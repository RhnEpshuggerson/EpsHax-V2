local WEBHOOK_URL = "" -- Put your Discord webhook URL here

local initial_gems = 0
local current_gems = 0
local earned = 0
local start_time = os.time()
local last_gem_time = os.time()
local top_gains = {}
local check_interval = 1
local warn_timeout = 5

local function format_number(n)
    local formatted = tostring(math.floor(n))
    local k
    while true do
        formatted, k = string.gsub(formatted, "^(-?%d+)(%d%d%d)", "%1,%2")
        if k == 0 then break end
    end
    return formatted
end

local function json_escape(s)
    s = s:gsub("\\", "\\\\")
    s = s:gsub('"', '\\"')
    s = s:gsub("\n", "\\n")
    s = s:gsub("\r", "\\r")
    s = s:gsub("\t", "\\t")
    return s
end

local function get_runtime()
    local elapsed = os.time() - start_time
    local hours = math.floor(elapsed / 3600)
    local minutes = math.floor((elapsed % 3600) / 60)
    local seconds = elapsed % 60
    return string.format("%dh %dm %ds", hours, minutes, seconds)
end

local function get_earned_per_min()
    local elapsed = os.time() - start_time
    if elapsed <= 0 then return 0 end
    return math.floor((earned / elapsed) * 60)
end

local function add_gain(gain)
    table.insert(top_gains, gain)
    table.sort(top_gains, function(a, b) return a > b end)
    if #top_gains > 5 then
        table.remove(top_gains)
    end
end

local function build_status_payload()
    local player = GetLocal()
    local name = json_escape(player.name or "Unknown")
    local netid = player.netid or 0
    local world = json_escape(player.world or "N/A")

    local top5_lines = {}
    if #top_gains > 0 then
        for i, g in ipairs(top_gains) do
            if i <= 5 then
                table.insert(top5_lines, "• " .. format_number(g))
            end
        end
    else
        table.insert(top5_lines, "• No gains yet")
    end
    local top5 = table.concat(top5_lines, "\\n")

    local account_info = "NAME: " .. name .. " (" .. tostring(netid) .. ")\\nWORLD: " .. world
    local gems_stats = "INITIAL: " .. format_number(initial_gems) .. "\\nCURRENT: " .. format_number(current_gems) .. "\\nEARNED: " .. format_number(earned) .. "\\nEARNED/MIN: " .. format_number(get_earned_per_min())
    local runtime = get_runtime()
    local timestamp = os.date("!%Y-%m-%dT%H:%M:%S.000Z")

    local payload = '{'..
        '"content": "",'..
        '"embeds": [{'..
            '"title": "WORKER STATUS REPORT",'..
            '"color": 5814783,'..
            '"fields": ['..
                '{'..
                    '"name": "ACCOUNT INFO",'..
                    '"value": "' .. account_info .. '",'..
                    '"inline": false'..
                '},'..
                '{'..
                    '"name": "GEMS STATISTICS",'..
                    '"value": "' .. gems_stats .. '",'..
                    '"inline": false'..
                '},'..
                '{'..
                    '"name": "TOP 5 HIGHEST GAINS",'..
                    '"value": "' .. top5 .. '",'..
                    '"inline": false'..
                '},'..
                '{'..
                    '"name": "RUNTIME",'..
                    '"value": "' .. runtime .. '",'..
                    '"inline": false'..
                '}'..
            '],'..
            '"footer": {'..
                '"text": "Groetopia Worker Monitor"'..
            '},'..
            '"timestamp": "' .. timestamp .. '"'..
        '}]'..
    '}'

    return payload
end

local function build_warning_payload()
    local timestamp = os.date("!%Y-%m-%dT%H:%M:%S.000Z")
    local payload = '{'..
        '"content": "",'..
        '"embeds": [{'..
            '"title": "WARNING: NO GEMS EARNED",'..
            '"description": "No gems earned in the last ' .. warn_timeout .. ' seconds!",'..
            '"color": 16711680,'..
            '"fields": ['..
                '{'..
                    '"name": "Current Gems",'..
                    '"value": "' .. format_number(current_gems) .. '",'..
                    '"inline": true'..
                '},'..
                '{'..
                    '"name": "Total Earned",'..
                    '"value": "' .. format_number(earned) .. '",'..
                    '"inline": true'..
                '}'..
            '],'..
            '"timestamp": "' .. timestamp .. '"'..
        '}]'..
    '}'

    return payload
end

local function send_status()
    local ok, payload = pcall(build_status_payload)
    if ok then
        SendWebhook(WEBHOOK_URL, payload)
    else
        log("Status payload error: " .. tostring(payload))
    end
end

local function send_warning()
    local ok, payload = pcall(build_warning_payload)
    if ok then
        SendWebhook(WEBHOOK_URL, payload)
    else
        log("Warning payload error: " .. tostring(payload))
    end
end

AddCallback("gem_monitor", "OnUpdate", function(dt)
    current_gems = GetLocal().gems

    if initial_gems == 0 then
        initial_gems = current_gems
    end

    earned = current_gems - initial_gems
end)

timer.Create("gem_check", check_interval, 0, function()
    local now = os.time()
    local player = GetLocal()
    local gems = player.gems

    if gems > current_gems then
        local gain = gems - current_gems
        add_gain(gain)
        last_gem_time = now
        current_gems = gems
        earned = current_gems - initial_gems
    end

    if (now - last_gem_time) >= warn_timeout then
        send_warning()
        last_gem_time = now
    end
end)

timer.Create("status_report", 600, 0, function()
    if WEBHOOK_URL ~= "" then
        send_status()
    end
end)

RunThread(function()
    Sleep(2000)
    initial_gems = GetLocal().gems
    current_gems = initial_gems
    last_gem_time = os.time()
    log("Gem monitor started! Initial gems: " .. tostring(initial_gems))
    if WEBHOOK_URL ~= "" then
        send_status()
    end
end)
