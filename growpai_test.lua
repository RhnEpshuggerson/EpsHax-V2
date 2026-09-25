-- Minimal bridge test - paste this first to verify GrowPai script execution works
log("[BRIDGE-TEST] Script started!")

log("[BRIDGE-TEST] io = " .. tostring(io))
log("[BRIDGE-TEST] os = " .. tostring(os))
log("[BRIDGE-TEST] GetLocal = " .. tostring(GetLocal))

if io then
    local f = io.open("C:\\temp\\growpai_test.txt", "w")
    if f then
        f:write("hello from growpai")
        f:close()
        log("[BRIDGE-TEST] Write OK!")
    else
        log("[BRIDGE-TEST] Write FAILED - cannot open file")
    end
else
    log("[BRIDGE-TEST] io is nil, cannot write files")
end

if GetLocal then
    local ok, p = pcall(GetLocal)
    if ok and p then
        log("[BRIDGE-TEST] GetLocal name=" .. tostring(p.name) .. " gems=" .. tostring(p.gems))
    else
        log("[BRIDGE-TEST] GetLocal failed or returned nil")
    end
end

log("[BRIDGE-TEST] Done!")
