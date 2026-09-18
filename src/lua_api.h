#pragma once
#include <lua.hpp>
#include <string>
#include <vector>
#include <functional>
#include <mutex>

extern bool g_debugMode;
extern float g_currentTime;
void debugLog(const std::string& msg);

struct LogEntry {
    std::string message;
    float time;
};

extern std::mutex g_consoleMutex;
extern std::vector<LogEntry> g_consoleLogs;
void consoleLog(const std::string& msg);

struct CallbackInfo {
    std::string name;
    std::string type;
    int ref;
};

class LuaExecutor {
public:
    LuaExecutor();
    ~LuaExecutor();

    bool execute(const std::string& script);
    void stop();
    bool isRunning() const { return running; }

    void registerAPI();

private:
    lua_State* L = nullptr;
    bool running = false;
    std::vector<CallbackInfo> callbacks;
    float startTime = 0;

    static int lua_SendPacket(lua_State* L);
    static int lua_SendPacketRaw(lua_State* L);
    static int lua_SendVarlist(lua_State* L);
    static int lua_log(lua_State* L);
    static int lua_GetLocal(lua_State* L);
    static int lua_GetInventory(lua_State* L);
    static int lua_GetPlayers(lua_State* L);
    static int lua_GetObjects(lua_State* L);
    static int lua_GetTile(lua_State* L);
    static int lua_GetTiles(lua_State* L);
    static int lua_FindPath(lua_State* L);
    static int lua_PathFind(lua_State* L);
    static int lua_CheckPath(lua_State* L);
    static int lua_IsSolid(lua_State* L);
    static int lua_RunThread(lua_State* L);
    static int lua_Sleep(lua_State* L);
    static int lua_GetPing(lua_State* L);
    static int lua_GetItemCount(lua_State* L);
    static int lua_GetItemInfo(lua_State* L);
    static int lua_MessageBox(lua_State* L);
    static int lua_RemoveCallbacks(lua_State* L);
    static int lua_RemoveCallback(lua_State* L);
    static int lua_EditToggle(lua_State* L);
    static int lua_SendWebhook(lua_State* L);
    static int lua_timer_Create(lua_State* L);
    static int lua_timer_Destroy(lua_State* L);
    static int lua_timer_Update(lua_State* L);
    static int lua_AddCallback(lua_State* L);
};
