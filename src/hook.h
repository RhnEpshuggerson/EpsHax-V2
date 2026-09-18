#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <mutex>
#include <queue>

// ── wglSwapBuffers hook ──────────────────────────────────────────────
typedef BOOL(WINAPI* wglSwapBuffers_t)(HDC hdc);
extern wglSwapBuffers_t o_wglSwapBuffers;
BOOL WINAPI hk_wglSwapBuffers(HDC hdc);

extern bool g_Initialized;
extern HWND g_GameHWND;
extern bool g_MenuOpen;

// ── Player / Game data ───────────────────────────────────────────────
struct PlayerData {
    std::string name;
    std::string world;
    std::string country;
    float pos_x = 0, pos_y = 0;
    int tile_x = 0, tile_y = 0;
    float size_x = 0, size_y = 0;
    int netid = 0;
    int userid = 0;
    int gems = 0;
    bool facing_left = false;
    int flags = 0, flags2 = 0;
};

struct TileData {
    int fg = 0, bg = 0;
    int pos_x = 0, pos_y = 0;
    int flags = 0;
    bool water = false, fire = false, ready = false;
};

// ── Packet event for Lua callbacks ───────────────────────────────────
struct PacketEvent {
    std::string type;  // "OnPacket", "OnVarlist", "OnRawPacket", "OnIncomingRawPacket"
    int packet_type = 0;
    std::string text;
    int int_data = 0;
    int netid = 0;
    float pos_x = 0, pos_y = 0;
    float pos2_x = 0, pos2_y = 0;
    int flags = 0;
    int item = 0;
    float delta_time = 0;
};

// ── Game state (thread-safe singleton) ───────────────────────────────
class GameState {
public:
    static GameState& instance();

    PlayerData localPlayer;
    std::vector<PlayerData> players;
    std::vector<std::vector<TileData>> tiles;
    int world_size_x = 0, world_size_y = 0;

    std::mutex mtx;
    std::queue<PacketEvent> events;

    void parseIncoming(const char* data, int len);
    void parseOutgoing(const char* data, int len);
    void pushEvent(const PacketEvent& ev);
    bool popEvent(PacketEvent& ev);
    void parseTextPacket(const std::string& text, bool incoming);
};
