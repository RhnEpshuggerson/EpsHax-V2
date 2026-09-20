#pragma once
#include <windows.h>
#include <cstdint>

namespace lua_state_finder {
    bool Scan();
    void* GetGameState();
    void* GetLuaState();
}
