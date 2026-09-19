#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <mutex>
#include <queue>
#include <map>

typedef BOOL(WINAPI* wglSwapBuffers_t)(HDC hdc);
extern wglSwapBuffers_t o_wglSwapBuffers;
BOOL WINAPI hk_wglSwapBuffers(HDC hdc);

extern bool g_Initialized;
extern HWND g_GameHWND;
extern bool g_MenuOpen;
extern bool g_SocketHooksInstalled;
void TryInstallSocketHooks();

struct PlayerData {
    std::string name;
    std::string world;
    std::string country;
    float pos_x = 0, pos_y = 0;
    int tile_x = 0, tile_y = 0;
    float size_x = 0, size_y = 0;
    int netid = -1;
    int userid = 0;
    int gems = 0;
    int item = -1;
    bool facing_left = false;
    int flags = 0, flags2 = 0;
};

struct TileData {
    int fg = 0, bg = 0;
    int pos_x = 0, pos_y = 0;
    int flags = 0;
    bool water = false, fire = false, ready = false;
};

struct InventoryItem {
    int id = 0;
    int count = 0;
};

struct ItemInfo {
    std::string name = "Unknown";
    int item_type = 0;
    int growth = 0;
    int rarity = 0;
    int size = 0;
};

struct WorldObject {
    int id = 0;
    int oid = 0;
    float pos_x = 0, pos_y = 0;
    int count = 0;
    int flags = 0;
};

struct GamePacket {
    int type = 0;
    int objtype = 0;
    int count1 = 0;
    int count2 = 0;
    int netid = 0;
    int item = 0;
    int flags = 0;
    float float1 = 0;
    int int_data = 0;
    float pos_x = 0, pos_y = 0;
    float pos2_x = 0, pos2_y = 0;
    float float2 = 0;
    int int_x = 0, int_y = 0;
};

struct PacketEvent {
    std::string type;
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

class GameState {
public:
    static GameState& instance();

    PlayerData localPlayer;
    std::vector<PlayerData> players;
    std::vector<std::vector<TileData>> tiles;
    std::vector<InventoryItem> inventory;
    std::map<int, ItemInfo> itemDatabase;
    std::vector<WorldObject> objects;
    int world_size_x = 0, world_size_y = 0;
    int ping_ms = 0;

    std::mutex mtx;
    std::queue<PacketEvent> events;

    void parseIncoming(const char* data, int len);
    void parseOutgoing(const char* data, int len);
    void pushEvent(const PacketEvent& ev);
    bool popEvent(PacketEvent& ev);
    void parseTextPacket(const std::string& text, bool incoming);
};
