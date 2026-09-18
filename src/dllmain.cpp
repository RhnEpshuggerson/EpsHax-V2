#include "lua_api.h"
#include "hook.h"

#include <windows.h>
#include <string>
#include <mutex>
#include <MinHook.h>

float g_currentTime = 0;

void debugLog(const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_debugMutex);
    g_debugLogs.push_back({msg, g_currentTime});
    if (g_debugLogs.size() > 1000) g_debugLogs.erase(g_debugLogs.begin());
}

// ── Main thread ──────────────────────────────────────────────────────
void MainThread(HMODULE hModule) {
    // Wait for opengl32.dll
    while (!GetModuleHandleA("opengl32.dll")) Sleep(100);
    Sleep(500);

    HMODULE hOGL = GetModuleHandleA("opengl32.dll");
    if (!hOGL) {
        FreeLibraryAndExitThread(hModule, 1);
        return;
    }

    auto p_wglSwapBuffers = (void*)GetProcAddress(hOGL, "wglSwapBuffers");
    if (!p_wglSwapBuffers) {
        FreeLibraryAndExitThread(hModule, 1);
        return;
    }

    // Init MinHook
    if (MH_Initialize() != MH_OK) {
        FreeLibraryAndExitThread(hModule, 1);
        return;
    }

    if (MH_CreateHook(p_wglSwapBuffers, (void*)hk_wglSwapBuffers, (void**)&o_wglSwapBuffers) != MH_OK) {
        MH_Uninitialize();
        FreeLibraryAndExitThread(hModule, 1);
        return;
    }

    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
        MH_Uninitialize();
        FreeLibraryAndExitThread(hModule, 1);
        return;
    }

    consoleLog("[INFO] Hook installed via MinHook");

    // Main loop — keep alive, update timer
    auto t0 = std::chrono::steady_clock::now();
    while (true) {
        Sleep(50);
        auto now = std::chrono::steady_clock::now();
        g_currentTime = std::chrono::duration<float>(now - t0).count();
    }

    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    FreeLibraryAndExitThread(hModule, 0);
}

// ── DLL Entry ────────────────────────────────────────────────────────
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        HANDLE hThread = CreateThread(nullptr, 0, (LPTHREAD_START_ROUTINE)MainThread, hModule, 0, nullptr);
        if (hThread) CloseHandle(hThread);
    }
    return TRUE;
}
