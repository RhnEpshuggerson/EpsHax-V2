#include "lua_api.h"
#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>

std::mutex g_consoleMutex;
std::vector<LogEntry> g_consoleLogs;

void consoleLog(const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_consoleMutex);
    g_consoleLogs.push_back({msg, g_currentTime});
    if (g_consoleLogs.size() > 500) g_consoleLogs.erase(g_consoleLogs.begin());
}

LuaExecutor::LuaExecutor() {
    L = luaL_newstate();
    luaL_openlibs(L);
    registerAPI();
}

LuaExecutor::~LuaExecutor() {
    if (L) lua_close(L);
}

bool LuaExecutor::execute(const std::string& script) {
    if (running) stop();
    running = true;
    startTime = g_currentTime;

    int err = luaL_loadstring(L, script.c_str()) || lua_pcall(L, 0, 0, 0);
    if (err) {
        consoleLog("[ERROR] " + std::string(lua_tostring(L, -1)));
        debugLog("[SYSTEM] Script error: " + std::string(lua_tostring(L, -1)));
        lua_pop(L, 1);
        running = false;
        return false;
    }
    running = false;
    consoleLog("[INFO] Script finished.");
    debugLog("[SYSTEM] Script finished.");
    return true;
}

void LuaExecutor::stop() {
    running = false;
    if (L) lua_close(L);
    L = luaL_newstate();
    luaL_openlibs(L);
    registerAPI();
}

// ── API Implementation ───────────────────────────────────────────────

int LuaExecutor::lua_log(lua_State* L) {
    const char* msg = luaL_checkstring(L, 1);
    consoleLog("[LUA] " + std::string(msg));
    return 0;
}

int LuaExecutor::lua_SendPacket(lua_State* L) {
    int type = (int)luaL_checkinteger(L, 1);
    const char* packet = luaL_checkstring(L, 2);
    debugLog("[PACKET] SendPacket type=" + std::to_string(type) + " data=" + packet);
    return 0;
}

int LuaExecutor::lua_SendPacketRaw(lua_State* L) {
    debugLog("[PACKET] SendPacketRaw");
    return 0;
}

int LuaExecutor::lua_SendVarlist(lua_State* L) {
    debugLog("[PACKET] SendVarlist");
    return 0;
}

int LuaExecutor::lua_GetLocal(lua_State* L) {
    debugLog("[PLAYER] GetLocal called");
    lua_newtable(L);
    lua_pushstring(L, "Player");
    lua_setfield(L, -2, "name");
    lua_pushstring(L, "STUB");
    lua_setfield(L, -2, "world");
    lua_pushstring(L, "us");
    lua_setfield(L, -2, "country");
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "pos_x");
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "pos_y");
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "tile_x");
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "tile_y");
    lua_pushinteger(L, 20);
    lua_setfield(L, -2, "size_x");
    lua_pushinteger(L, 20);
    lua_setfield(L, -2, "size_y");
    lua_pushinteger(L, 1);
    lua_setfield(L, -2, "netid");
    lua_pushinteger(L, 100);
    lua_setfield(L, -2, "userid");
    lua_pushinteger(L, 9999);
    lua_setfield(L, -2, "gems");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "facing_left");
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "flags");
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "flags2");
    return 1;
}

int LuaExecutor::lua_GetInventory(lua_State* L) {
    debugLog("[INV] GetInventory called");
    lua_newtable(L);
    return 1;
}

int LuaExecutor::lua_GetPlayers(lua_State* L) {
    debugLog("[PLAYER] GetPlayers called");
    lua_newtable(L);
    return 1;
}

int LuaExecutor::lua_GetObjects(lua_State* L) {
    debugLog("[INV] GetObjects called");
    lua_newtable(L);
    return 1;
}

int LuaExecutor::lua_GetTile(lua_State* L) {
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    debugLog("[PATH] GetTile(" + std::to_string(x) + ", " + std::to_string(y) + ")");
    lua_newtable(L);
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "fg");
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "bg");
    lua_pushinteger(L, x);
    lua_setfield(L, -2, "pos_x");
    lua_pushinteger(L, y);
    lua_setfield(L, -2, "pos_y");
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "flags");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "water");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "fire");
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "ready");
    return 1;
}

int LuaExecutor::lua_GetTiles(lua_State* L) {
    debugLog("[PATH] GetTiles called");
    lua_newtable(L);
    return 1;
}

int LuaExecutor::lua_FindPath(lua_State* L) {
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    debugLog("[PATH] FindPath -> (" + std::to_string(x) + ", " + std::to_string(y) + ")");
    return 0;
}

int LuaExecutor::lua_PathFind(lua_State* L) {
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    debugLog("[PATH] PathFind -> (" + std::to_string(x) + ", " + std::to_string(y) + ")");
    lua_newtable(L);
    return 1;
}

int LuaExecutor::lua_CheckPath(lua_State* L) {
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    debugLog("[PATH] CheckPath(" + std::to_string(x) + ", " + std::to_string(y) + ") -> true");
    lua_pushboolean(L, 1);
    return 1;
}

int LuaExecutor::lua_IsSolid(lua_State* L) {
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    debugLog("[PATH] IsSolid(" + std::to_string(x) + ", " + std::to_string(y) + ") -> false");
    lua_pushboolean(L, 0);
    return 1;
}

int LuaExecutor::lua_RunThread(lua_State* L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    debugLog("[SYSTEM] RunThread");
    lua_pushvalue(L, 1);
    lua_pcall(L, 0, 0, 0);
    return 0;
}

int LuaExecutor::lua_Sleep(lua_State* L) {
    int ms = (int)luaL_checkinteger(L, 1);
    debugLog("[TIMER] Sleep(" + std::to_string(ms) + "ms)");
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    return 0;
}

int LuaExecutor::lua_GetPing(lua_State* L) {
    lua_pushinteger(L, 42);
    return 1;
}

int LuaExecutor::lua_GetItemCount(lua_State* L) {
    int id = (int)luaL_checkinteger(L, 1);
    debugLog("[INV] GetItemCount(" + std::to_string(id) + ") -> 0");
    lua_pushinteger(L, 0);
    return 1;
}

int LuaExecutor::lua_GetItemInfo(lua_State* L) {
    int id = (int)luaL_checkinteger(L, 1);
    debugLog("[INV] GetItemInfo(" + std::to_string(id) + ")");
    lua_newtable(L);
    lua_pushstring(L, "Unknown");
    lua_setfield(L, -2, "name");
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "item_type");
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "growth");
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "rarity");
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "size");
    return 1;
}

int LuaExecutor::lua_MessageBox(lua_State* L) {
    const char* title = luaL_checkstring(L, 1);
    const char* content = luaL_checkstring(L, 2);
    debugLog("[SYSTEM] MessageBox: " + std::string(title) + " - " + std::string(content));
    return 0;
}

int LuaExecutor::lua_RemoveCallbacks(lua_State* L) {
    debugLog("[CALLBACK] RemoveCallbacks");
    return 0;
}

int LuaExecutor::lua_RemoveCallback(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    debugLog("[CALLBACK] RemoveCallback: " + std::string(name));
    return 0;
}

int LuaExecutor::lua_EditToggle(lua_State* L) {
    const char* module = luaL_checkstring(L, 1);
    bool toggle = lua_toboolean(L, 2);
    debugLog("[SYSTEM] EditToggle: " + std::string(module) + " = " + (toggle ? "ON" : "OFF"));
    return 0;
}

int LuaExecutor::lua_SendWebhook(lua_State* L) {
    const char* webhook = luaL_checkstring(L, 1);
    debugLog("[SYSTEM] SendWebhook to " + std::string(webhook));
    return 0;
}

int LuaExecutor::lua_timer_Create(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    int interval = (int)luaL_checkinteger(L, 2);
    int repeat_count = (int)luaL_checkinteger(L, 3);
    luaL_checktype(L, 4, LUA_TFUNCTION);
    debugLog("[TIMER] Create: " + std::string(name) + " interval=" + std::to_string(interval) + "ms repeat=" + std::to_string(repeat_count));
    return 0;
}

int LuaExecutor::lua_timer_Destroy(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    debugLog("[TIMER] Destroy: " + std::string(name));
    return 0;
}

int LuaExecutor::lua_timer_Update(lua_State* L) {
    return 0;
}

int LuaExecutor::lua_AddCallback(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const char* type = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    debugLog("[CALLBACK] AddCallback: " + std::string(name) + " type=" + std::string(type));
    return 0;
}

// ── Register all API functions ────────────────────────────────────────

void LuaExecutor::registerAPI() {
    lua_register(L, "SendPacket", lua_SendPacket);
    lua_register(L, "SendPacketRaw", lua_SendPacketRaw);
    lua_register(L, "SendPacketRawClient", lua_SendPacketRaw);
    lua_register(L, "SendVarlist", lua_SendVarlist);
    lua_register(L, "log", lua_log);
    lua_register(L, "FindPath", lua_FindPath);
    lua_register(L, "PathFind", lua_PathFind);
    lua_register(L, "CheckPath", lua_CheckPath);
    lua_register(L, "IsSolid", lua_IsSolid);
    lua_register(L, "GetLocal", lua_GetLocal);
    lua_register(L, "GetInventory", lua_GetInventory);
    lua_register(L, "GetPlayers", lua_GetPlayers);
    lua_register(L, "GetObjects", lua_GetObjects);
    lua_register(L, "GetTile", lua_GetTile);
    lua_register(L, "GetTiles", lua_GetTiles);
    lua_register(L, "RunThread", lua_RunThread);
    lua_register(L, "Sleep", lua_Sleep);
    lua_register(L, "GetPing", lua_GetPing);
    lua_register(L, "GetItemCount", lua_GetItemCount);
    lua_register(L, "GetItemInfo", lua_GetItemInfo);
    lua_register(L, "MessageBox", lua_MessageBox);
    lua_register(L, "RemoveCallbacks", lua_RemoveCallbacks);
    lua_register(L, "RemoveCallback", lua_RemoveCallback);
    lua_register(L, "EditToggle", lua_EditToggle);
    lua_register(L, "SendWebhook", lua_SendWebhook);
    lua_register(L, "AddCallback", lua_AddCallback);

    // Register timer sub-table
    lua_newtable(L);
    lua_pushcfunction(L, lua_timer_Create);
    lua_setfield(L, -2, "Create");
    lua_pushcfunction(L, lua_timer_Destroy);
    lua_setfield(L, -2, "Destroy");
    lua_pushcfunction(L, lua_timer_Update);
    lua_setfield(L, -2, "Update");
    lua_setglobal(L, "timer");
}
