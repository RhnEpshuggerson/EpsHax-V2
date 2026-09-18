#include "lua_api.h"
#include "hook.h"

#include <windows.h>
#include <string>
#include <mutex>

float g_currentTime = 0;

void debugLog(const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_debugMutex);
    g_debugLogs.push_back({msg, g_currentTime});
    if (g_debugLogs.size() > 1000) g_debugLogs.erase(g_debugLogs.begin());
}

// ── Inline hook: place JMP at start of target function ───────────────
static void* InlineHook(void* target, void* hook, void** original) {
    DWORD oldProtect;
    *original = target;

    // Allocate trampoline (14 bytes for JMP [RIP+0])
    BYTE* trampoline = (BYTE*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!trampoline) return nullptr;

    // Copy original bytes (14 bytes for x64 absolute JMP)
    memcpy(trampoline, target, 14);

    // Add JMP back to target+14 (return to original function after hook)
    trampoline[14] = 0xFF;
    trampoline[15] = 0x25;
    *(DWORD*)(trampoline + 16) = 0;
    *(UINT64*)(trampoline + 20) = (UINT64)((BYTE*)target + 14);

    *original = trampoline;

    // Write JMP to hook function
    VirtualProtect(target, 14, PAGE_EXECUTE_READWRITE, &oldProtect);
    BYTE* dst = (BYTE*)target;
    dst[0] = 0xFF;
    dst[1] = 0x25;
    *(DWORD*)(dst + 2) = 0;
    *(UINT64*)(dst + 6) = (UINT64)hook;
    VirtualProtect(target, 14, oldProtect, &oldProtect);

    return trampoline;
}

// ── Main thread ──────────────────────────────────────────────────────
void MainThread(HMODULE hModule) {
    // Wait for opengl32
    while (!GetModuleHandleA("opengl32.dll")) Sleep(100);
    Sleep(500);

    // Get real wglSwapBuffers address
    HMODULE hOGL = GetModuleHandleA("opengl32.dll");
    auto p_wglSwapBuffers = (void*)GetProcAddress(hOGL, "wglSwapBuffers");

    if (p_wglSwapBuffers) {
        InlineHook(p_wglSwapBuffers, (void*)hk_wglSwapBuffers, (void**)&o_wglSwapBuffers);
        consoleLog("[INFO] wglSwapBuffers hook installed via inline hook");
    } else {
        consoleLog("[ERROR] Could not find wglSwapBuffers");
        FreeLibraryAndExitThread(hModule, 1);
        return;
    }

    // Main loop
    auto t0 = std::chrono::steady_clock::now();
    while (true) {
        Sleep(50);

        auto now = std::chrono::steady_clock::now();
        g_currentTime = std::chrono::duration<float>(now - t0).count();

        // F1 toggle
        static bool f1Was = false;
        bool f1Now = (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
        if (f1Now && !f1Was) g_MenuOpen = !g_MenuOpen;
        f1Was = f1Now;
    }

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
