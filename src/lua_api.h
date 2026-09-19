#pragma once
#include <lua.hpp>
#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <thread>
#include <atomic>

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

extern std::mutex g_debugMutex;
extern std::vector<LogEntry> g_debugLogs;

struct CallbackInfo {
    std::string name;
    std::string type;
    int ref;
};

struct TimerInfo {
    std::string name;
    int interval_ms;
    int repeat;
    int ref;
    float lastTick;
};

struct LuaThreadInfo {
    lua_State* co;
    float resumeTime;
    int ref;
};

class LuaExecutor {
public:
    LuaExecutor();
    ~LuaExecutor();

    bool execute(const std::string& script);
    void stop();
    bool isRunning() const { return running; }
    void tick(float dt);

    void registerAPI();

    lua_State* getLuaState() { return L; }
    void setTickInterval(float s) { tickInterval = s; }

    std::vector<CallbackInfo> callbacks;
    std::vector<TimerInfo> timers;
    std::vector<LuaThreadInfo> threads;

private:
    lua_State* L = nullptr;
    std::atomic<bool> running{false};
    float startTime = 0;
    float lastTickTime = 0;
    float tickInterval = 1.0f;

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
    static int lua_GetGhost(lua_State* L);
    static int lua_GetAccesslist(lua_State* L);
    static int lua_GetLocalObject(lua_State* L);
    static int lua_GetDroppedItems(lua_State* L);
    static int lua_AddCallback(lua_State* L);
};
