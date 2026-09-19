#pragma once
#include <windows.h>
#include <cstdint>
#include <vector>

namespace scanner {
    bool Install(HMODULE gameModule);
    bool HooksInstalled();

    extern uintptr_t fn_ProcessTankUpdatePacket;
    extern uintptr_t fn_SendPacket;
    extern uintptr_t fn_GetGameLogic;
    extern uintptr_t fn_RecvWrapper;

    void* CallGetGameLogic();
    void CallSendPacket(int type, const char* text);

    void ScanRecvWrapper();

    extern uintptr_t g_GameBase;
    extern size_t g_GameImageSize;
}
