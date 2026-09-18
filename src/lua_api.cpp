#include "lua_api.h"
#include "hook.h"
#include <lua.h>
#include <lauxlib.h>
#include <iostream>
#include <sstream>
#include <thread>
#include <chrono>
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

std::mutex g_consoleMutex;
std::vector<LogEntry> g_consoleLogs;
std::mutex g_debugMutex;
std::vector<LogEntry> g_debugLogs;

void consoleLog(const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_consoleMutex);
    g_consoleLogs.push_back({msg, g_currentTime});
    if (g_consoleLogs.size() > 500) g_consoleLogs.erase(g_consoleLogs.begin());
}

LuaExecutor::LuaExecutor() {
    L = luaL_newstate();
    luaL_openlibs(L);
    lua_pushlightuserdata(L, this);
    lua_setfield(L, LUA_REGISTRYINDEX, "__executor");
    registerAPI();
}

LuaExecutor::~LuaExecutor() { if (L) lua_close(L); }

bool LuaExecutor::execute(const std::string& script) {
    if (running) stop();
    running = true;
    startTime = g_currentTime;
    callbacks.clear();
    timers.clear();
    threads.clear();
    int err = luaL_loadstring(L, script.c_str()) || lua_pcall(L, 0, 0, 0);
    if (err) {
        std::string e = lua_tostring(L, -1);
        consoleLog("[ERROR] " + e);
        lua_pop(L, 1);
        running = false;
        return false;
    }
    consoleLog("[INFO] Script loaded. Running event loop...");
    float lastFrame = g_currentTime;
    while (running) {
        float now = g_currentTime;
        float dt = now - lastFrame;
        lastFrame = now;
        for (auto it = threads.begin(); it != threads.end(); ) {
            if (now >= it->resumeTime) {
                int nres;
                int status = lua_resume(it->co, L, 0, &nres);
                if (status == LUA_OK) { luaL_unref(L, LUA_REGISTRYINDEX, it->ref); it = threads.erase(it); }
                else if (status == LUA_YIELD) { ++it; }
                else { consoleLog("[ERROR] Thread: " + std::string(lua_tostring(it->co, -1))); lua_pop(it->co, 1); luaL_unref(L, LUA_REGISTRYINDEX, it->ref); it = threads.erase(it); }
            } else { ++it; }
        }
        for (auto& cb : callbacks) {
            if (cb.type == "OnUpdate") {
                lua_rawgeti(L, LUA_REGISTRYINDEX, cb.ref);
                lua_pushnumber(L, dt);
                if (lua_pcall(L, 1, 1, 0) != 0) { consoleLog("[ERROR] OnUpdate: " + std::string(lua_tostring(L, -1))); lua_pop(L, 1); } else { lua_pop(L, 1); }
            }
        }
        PacketEvent ev;
        while (GameState::instance().popEvent(ev)) {
            for (auto& cb : callbacks) {
                if (cb.type == ev.type) {
                    lua_rawgeti(L, LUA_REGISTRYINDEX, cb.ref);
                    if (ev.type == "OnVarlist") {
                        lua_newtable(L);
                        std::istringstream stream(ev.text);
                        std::string line;
                        int idx = 0;
                        while (std::getline(stream, line)) {
                            size_t pipe = line.find('|');
                            std::string key, val;
                            if (pipe != std::string::npos) { key = line.substr(0, pipe); val = line.substr(pipe + 1); }
                            else { key = std::to_string(idx); val = line; }
                            lua_pushstring(L, val.c_str()); lua_setfield(L, -2, key.c_str());
                            lua_pushstring(L, val.c_str()); lua_rawseti(L, -2, idx);
                            idx++;
                        }
                        lua_pushstring(L, ev.text.c_str());
                        if (lua_pcall(L, 2, 1, 0) != 0) lua_pop(L, 1); else lua_pop(L, 1);
                    } else {
                        lua_newtable(L);
                        lua_pushinteger(L, ev.packet_type); lua_setfield(L, -2, "type");
                        lua_pushinteger(L, ev.netid); lua_setfield(L, -2, "netid");
                        lua_pushinteger(L, ev.flags); lua_setfield(L, -2, "flags");
                        lua_pushinteger(L, ev.item); lua_setfield(L, -2, "item");
                        lua_pushnumber(L, ev.pos_x); lua_setfield(L, -2, "pos_x");
                        lua_pushnumber(L, ev.pos_y); lua_setfield(L, -2, "pos_y");
                        lua_pushstring(L, ev.text.c_str()); lua_setfield(L, -2, "data");
                        if (lua_pcall(L, 1, 1, 0) != 0) lua_pop(L, 1); else lua_pop(L, 1);
                    }
                }
            }
        }
        for (auto it = timers.begin(); it != timers.end(); ) {
            if (now - it->lastTick >= it->interval_ms / 1000.0f) {
                it->lastTick = now;
                lua_rawgeti(L, LUA_REGISTRYINDEX, it->ref);
                if (lua_pcall(L, 0, 0, 0) != 0) { consoleLog("[ERROR] Timer: " + std::string(lua_tostring(L, -1))); lua_pop(L, 1); }
                if (it->repeat > 0) { it->repeat--; if (it->repeat == 0) { luaL_unref(L, LUA_REGISTRYINDEX, it->ref); it = timers.erase(it); continue; } }
            }
            ++it;
        }
        Sleep(10);
    }
    for (auto& cb : callbacks) luaL_unref(L, LUA_REGISTRYINDEX, cb.ref); callbacks.clear();
    for (auto& t : timers) luaL_unref(L, LUA_REGISTRYINDEX, t.ref); timers.clear();
    for (auto& t : threads) luaL_unref(L, LUA_REGISTRYINDEX, t.ref); threads.clear();
    consoleLog("[INFO] Script stopped.");
    return true;
}

void LuaExecutor::stop() {
    running = false;
    if (L) lua_close(L);
    L = luaL_newstate(); luaL_openlibs(L);
    lua_pushlightuserdata(L, this); lua_setfield(L, LUA_REGISTRYINDEX, "__executor");
    registerAPI(); callbacks.clear(); timers.clear(); threads.clear();
}

int LuaExecutor::lua_log(lua_State* L) { consoleLog("[LUA] " + std::string(luaL_checkstring(L, 1))); return 0; }
int LuaExecutor::lua_SendPacket(lua_State* L) { debugLog("[PACKET] SendPacket type=" + std::to_string((int)luaL_checkinteger(L, 1))); return 0; }
int LuaExecutor::lua_SendPacketRaw(lua_State* L) { return 0; }
int LuaExecutor::lua_SendVarlist(lua_State* L) { return 0; }

int LuaExecutor::lua_GetLocal(lua_State* L) {
    auto& gs = GameState::instance();
    std::lock_guard<std::mutex> lock(gs.mtx);
    auto& p = gs.localPlayer;
    debugLog("[PLAYER] GetLocal: name=" + p.name + " world=" + p.world + " gems=" + std::to_string(p.gems));
    lua_newtable(L);
    lua_pushstring(L, p.name.c_str()); lua_setfield(L, -2, "name");
    lua_pushstring(L, p.world.c_str()); lua_setfield(L, -2, "world");
    lua_pushstring(L, p.country.c_str()); lua_setfield(L, -2, "country");
    lua_pushnumber(L, p.pos_x); lua_setfield(L, -2, "pos_x");
    lua_pushnumber(L, p.pos_y); lua_setfield(L, -2, "pos_y");
    lua_pushinteger(L, p.tile_x); lua_setfield(L, -2, "tile_x");
    lua_pushinteger(L, p.tile_y); lua_setfield(L, -2, "tile_y");
    lua_pushnumber(L, p.size_x); lua_setfield(L, -2, "size_x");
    lua_pushnumber(L, p.size_y); lua_setfield(L, -2, "size_y");
    lua_pushinteger(L, p.netid); lua_setfield(L, -2, "netid");
    lua_pushinteger(L, p.userid); lua_setfield(L, -2, "userid");
    lua_pushinteger(L, p.gems); lua_setfield(L, -2, "gems");
    lua_pushboolean(L, p.facing_left); lua_setfield(L, -2, "facing_left");
    lua_pushinteger(L, p.flags); lua_setfield(L, -2, "flags");
    lua_pushinteger(L, p.flags2); lua_setfield(L, -2, "flags2");
    return 1;
}

int LuaExecutor::lua_GetInventory(lua_State* L) { lua_newtable(L); return 1; }

int LuaExecutor::lua_GetPlayers(lua_State* L) {
    auto& gs = GameState::instance();
    std::lock_guard<std::mutex> lock(gs.mtx);
    lua_newtable(L);
    int idx = 1;
    for (auto& p : gs.players) {
        lua_newtable(L);
        lua_pushstring(L, p.name.c_str()); lua_setfield(L, -2, "name");
        lua_pushstring(L, p.world.c_str()); lua_setfield(L, -2, "world");
        lua_pushstring(L, p.country.c_str()); lua_setfield(L, -2, "country");
        lua_pushnumber(L, p.pos_x); lua_setfield(L, -2, "pos_x");
        lua_pushnumber(L, p.pos_y); lua_setfield(L, -2, "pos_y");
        lua_pushinteger(L, p.tile_x); lua_setfield(L, -2, "tile_x");
        lua_pushinteger(L, p.tile_y); lua_setfield(L, -2, "tile_y");
        lua_pushinteger(L, p.netid); lua_setfield(L, -2, "netid");
        lua_pushinteger(L, p.userid); lua_setfield(L, -2, "userid");
        lua_pushinteger(L, p.gems); lua_setfield(L, -2, "gems");
        lua_rawseti(L, -2, idx++);
    }
    return 1;
}

int LuaExecutor::lua_GetObjects(lua_State* L) { lua_newtable(L); return 1; }

int LuaExecutor::lua_GetTile(lua_State* L) {
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    lua_newtable(L);
    lua_pushinteger(L, 0); lua_setfield(L, -2, "fg");
    lua_pushinteger(L, 0); lua_setfield(L, -2, "bg");
    lua_pushinteger(L, x); lua_setfield(L, -2, "pos_x");
    lua_pushinteger(L, y); lua_setfield(L, -2, "pos_y");
    lua_pushinteger(L, 0); lua_setfield(L, -2, "flags");
    lua_pushboolean(L, 0); lua_setfield(L, -2, "water");
    lua_pushboolean(L, 0); lua_setfield(L, -2, "fire");
    lua_pushboolean(L, 0); lua_setfield(L, -2, "ready");
    return 1;
}

int LuaExecutor::lua_GetTiles(lua_State* L) { lua_newtable(L); return 1; }
int LuaExecutor::lua_FindPath(lua_State* L) { return 0; }
int LuaExecutor::lua_PathFind(lua_State* L) { lua_newtable(L); return 1; }
int LuaExecutor::lua_CheckPath(lua_State* L) { lua_pushboolean(L, 1); return 1; }
int LuaExecutor::lua_IsSolid(lua_State* L) { lua_pushboolean(L, 0); return 1; }

int LuaExecutor::lua_RunThread(lua_State* L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    LuaExecutor* self = nullptr;
    lua_getfield(L, LUA_REGISTRYINDEX, "__executor");
    if (lua_type(L, -1) == LUA_TLIGHTUSERDATA) self = (LuaExecutor*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    lua_State* co = lua_newthread(L);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pushvalue(L, 1);
    lua_xmove(L, co, 1);
    if (self) self->threads.push_back({co, 0.0f, ref});
    int nres;
    int status = lua_resume(co, L, 0, &nres);
    if (status == LUA_OK) {
        if (self) { for (auto it = self->threads.begin(); it != self->threads.end(); ++it) if (it->co == co) { self->threads.erase(it); break; } }
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
    } else if (status != LUA_YIELD) {
        consoleLog("[ERROR] Thread: " + std::string(lua_tostring(co, -1)));
        lua_pop(co, 1);
        if (self) { for (auto it = self->threads.begin(); it != self->threads.end(); ++it) if (it->co == co) { self->threads.erase(it); break; } }
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
    }
    return 0;
}

int LuaExecutor::lua_Sleep(lua_State* L) {
    int ms = (int)luaL_checkinteger(L, 1);
    LuaExecutor* self = nullptr;
    lua_getfield(L, LUA_REGISTRYINDEX, "__executor");
    if (lua_type(L, -1) == LUA_TLIGHTUSERDATA) self = (LuaExecutor*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (self) {
        float resumeTime = g_currentTime + ms / 1000.0f;
        for (auto& t : self->threads) if (t.co == L) { t.resumeTime = resumeTime; break; }
    }
    return lua_yield(L, 0);
}

int LuaExecutor::lua_GetPing(lua_State* L) { lua_pushinteger(L, 42); return 1; }
int LuaExecutor::lua_GetItemCount(lua_State* L) { lua_pushinteger(L, 0); return 1; }

int LuaExecutor::lua_GetItemInfo(lua_State* L) {
    lua_newtable(L);
    lua_pushstring(L, "Unknown"); lua_setfield(L, -2, "name");
    lua_pushinteger(L, 0); lua_setfield(L, -2, "item_type");
    lua_pushinteger(L, 0); lua_setfield(L, -2, "growth");
    lua_pushinteger(L, 0); lua_setfield(L, -2, "rarity");
    lua_pushinteger(L, 0); lua_setfield(L, -2, "size");
    return 1;
}

int LuaExecutor::lua_MessageBox(lua_State* L) { return 0; }

int LuaExecutor::lua_RemoveCallbacks(lua_State* L) {
    LuaExecutor* self = nullptr;
    lua_getfield(L, LUA_REGISTRYINDEX, "__executor");
    if (lua_type(L, -1) == LUA_TLIGHTUSERDATA) self = (LuaExecutor*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!self) return 0;
    for (auto& cb : self->callbacks) luaL_unref(L, LUA_REGISTRYINDEX, cb.ref);
    self->callbacks.clear();
    return 0;
}

int LuaExecutor::lua_RemoveCallback(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    LuaExecutor* self = nullptr;
    lua_getfield(L, LUA_REGISTRYINDEX, "__executor");
    if (lua_type(L, -1) == LUA_TLIGHTUSERDATA) self = (LuaExecutor*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!self) return 0;
    for (auto it = self->callbacks.begin(); it != self->callbacks.end(); ) {
        if (it->name == name) { luaL_unref(L, LUA_REGISTRYINDEX, it->ref); it = self->callbacks.erase(it); } else { ++it; }
    }
    return 0;
}

int LuaExecutor::lua_EditToggle(lua_State* L) { return 0; }

int LuaExecutor::lua_SendWebhook(lua_State* L) {
    const char* webhookUrl = luaL_checkstring(L, 1);
    const char* payload = luaL_checkstring(L, 2);
    std::string urlStr(webhookUrl);
    std::string payStr(payload);
    std::thread([urlStr, payStr]() {
        std::wstring wUrl(urlStr.begin(), urlStr.end());
        URL_COMPONENTS urlComp = {};
        urlComp.dwStructSize = sizeof(urlComp);
        urlComp.lpszHostName = new wchar_t[256]; urlComp.dwHostNameLength = 256;
        urlComp.lpszUrlPath = new wchar_t[1024]; urlComp.dwUrlPathLength = 1024;
        urlComp.lpszExtraInfo = new wchar_t[256]; urlComp.dwExtraInfoLength = 256;
        if (!WinHttpCrackUrl(wUrl.c_str(), 0, 0, &urlComp)) {
            delete[] urlComp.lpszHostName; delete[] urlComp.lpszUrlPath; delete[] urlComp.lpszExtraInfo; return;
        }
        std::wstring host(urlComp.lpszHostName, urlComp.dwHostNameLength);
        std::wstring path(urlComp.lpszUrlPath, urlComp.dwUrlPathLength);
        HINTERNET hSession = WinHttpOpen(L"CoemsExecutor/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) { delete[] urlComp.lpszHostName; delete[] urlComp.lpszUrlPath; delete[] urlComp.lpszExtraInfo; return; }
        HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), urlComp.nPort, 0);
        if (!hConnect) { WinHttpCloseHandle(hSession); delete[] urlComp.lpszHostName; delete[] urlComp.lpszUrlPath; delete[] urlComp.lpszExtraInfo; return; }
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, urlComp.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
        if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); delete[] urlComp.lpszHostName; delete[] urlComp.lpszUrlPath; delete[] urlComp.lpszExtraInfo; return; }
        BOOL sent = WinHttpSendRequest(hRequest, L"Content-Type: application/json", -1L, (LPVOID)payStr.c_str(), (DWORD)payStr.size(), (DWORD)payStr.size(), 0);
        if (sent) {
            WinHttpReceiveResponse(hRequest, nullptr);
            DWORD statusCode = 0, statusSize = sizeof(statusCode);
            WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
            debugLog("[WEBHOOK] Response: " + std::to_string((int)statusCode));
        }
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        delete[] urlComp.lpszHostName; delete[] urlComp.lpszUrlPath; delete[] urlComp.lpszExtraInfo;
    }).detach();
    return 0;
}

int LuaExecutor::lua_timer_Create(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    int interval = (int)luaL_checkinteger(L, 2);
    int repeat_count = (int)luaL_checkinteger(L, 3);
    luaL_checktype(L, 4, LUA_TFUNCTION);
    LuaExecutor* self = nullptr;
    lua_getfield(L, LUA_REGISTRYINDEX, "__executor");
    if (lua_type(L, -1) == LUA_TLIGHTUSERDATA) self = (LuaExecutor*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!self) return 0;
    lua_pushvalue(L, 4);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    self->timers.push_back({name, interval, repeat_count, ref, g_currentTime});
    debugLog("[TIMER] Create: " + std::string(name));
    return 0;
}

int LuaExecutor::lua_timer_Destroy(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    LuaExecutor* self = nullptr;
    lua_getfield(L, LUA_REGISTRYINDEX, "__executor");
    if (lua_type(L, -1) == LUA_TLIGHTUSERDATA) self = (LuaExecutor*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!self) return 0;
    for (auto it = self->timers.begin(); it != self->timers.end(); ) {
        if (it->name == name) { luaL_unref(L, LUA_REGISTRYINDEX, it->ref); it = self->timers.erase(it); } else { ++it; }
    }
    return 0;
}

int LuaExecutor::lua_timer_Update(lua_State* L) { return 0; }

int LuaExecutor::lua_AddCallback(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const char* type = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    LuaExecutor* self = nullptr;
    lua_getfield(L, LUA_REGISTRYINDEX, "__executor");
    if (lua_type(L, -1) == LUA_TLIGHTUSERDATA) self = (LuaExecutor*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!self) return 0;
    lua_pushvalue(L, 3);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    self->callbacks.push_back({name, type, ref});
    debugLog("[CALLBACK] AddCallback: " + std::string(name) + " type=" + std::string(type));
    return 0;
}

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
    lua_newtable(L);
    lua_pushcfunction(L, lua_timer_Create);
    lua_setfield(L, -2, "Create");
    lua_pushcfunction(L, lua_timer_Destroy);
    lua_setfield(L, -2, "Destroy");
    lua_pushcfunction(L, lua_timer_Update);
    lua_setfield(L, -2, "Update");
    lua_setglobal(L, "timer");
}
