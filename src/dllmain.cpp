#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#include "lua_api.h"
#include "hook.h"

#define WIN32_LEAN_AND_MEAN
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

// ── send/recv hooks ──────────────────────────────────────────────────
typedef int (WINAPI* send_t)(SOCKET s, const char* buf, int len, int flags);
typedef int (WINAPI* recv_t)(SOCKET s, char* buf, int len, int flags);
typedef int (WINAPI* WSASend_t)(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount,
    LPDWORD lpNumberOfBytesSent, DWORD dwFlags, LPWSAOVERLAPPED lpOverlapped,
    LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);
typedef int (WINAPI* WSARecv_t)(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount,
    LPDWORD lpNumberOfBytesRecvd, LPDWORD lpFlags, LPWSAOVERLAPPED lpOverlapped,
    LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);

send_t o_send = nullptr;
recv_t o_recv = nullptr;
WSASend_t o_WSASend = nullptr;
WSARecv_t o_WSARecv = nullptr;
SOCKET g_GameSocket = INVALID_SOCKET;

int WINAPI hk_send(SOCKET s, const char* buf, int len, int flags) {
    if (len > 4 && buf) {
        GameState::instance().parseOutgoing(buf, len);
    }
    return o_send(s, buf, len, flags);
}

int WINAPI hk_recv(SOCKET s, char* buf, int len, int flags) {
    int result = o_recv(s, buf, len, flags);
    if (result > 4 && buf) {
        GameState::instance().parseIncoming(buf, result);
    }
    return result;
}

int WINAPI hk_WSASend(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount,
    LPDWORD lpNumberOfBytesSent, DWORD dwFlags,
    LPWSAOVERLAPPED lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine) {
    for (DWORD i = 0; i < dwBufferCount; i++) {
        if (lpBuffers[i].len > 4 && lpBuffers[i].buf) {
            GameState::instance().parseOutgoing(lpBuffers[i].buf, lpBuffers[i].len);
        }
    }
    return o_WSASend(s, lpBuffers, dwBufferCount, lpNumberOfBytesSent, dwFlags, lpOverlapped, lpCompletionRoutine);
}

int WINAPI hk_WSARecv(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount,
    LPDWORD lpNumberOfBytesRecvd, LPDWORD lpFlags,
    LPWSAOVERLAPPED lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine) {
    int result = o_WSARecv(s, lpBuffers, dwBufferCount, lpNumberOfBytesRecvd, lpFlags, lpOverlapped, lpCompletionRoutine);
    if (result == 0) {
        for (DWORD i = 0; i < dwBufferCount; i++) {
            if (lpBuffers[i].len > 4 && lpBuffers[i].buf) {
                GameState::instance().parseIncoming(lpBuffers[i].buf, lpBuffers[i].len);
            }
        }
    }
    return result;
}

// ── Main thread ──────────────────────────────────────────────────────
void MainThread(HMODULE hModule) {
    while (!GetModuleHandleA("opengl32.dll")) Sleep(100);
    Sleep(500);

    // ── Hook wglSwapBuffers ──────────────────────────────────────
    HMODULE hOGL = GetModuleHandleA("opengl32.dll");
    auto p_wglSwapBuffers = (void*)GetProcAddress(hOGL, "wglSwapBuffers");

    if (MH_Initialize() != MH_OK) {
        FreeLibraryAndExitThread(hModule, 1);
        return;
    }

    if (p_wglSwapBuffers) {
        MH_CreateHook(p_wglSwapBuffers, (void*)hk_wglSwapBuffers, (void**)&o_wglSwapBuffers);
        MH_EnableHook(p_wglSwapBuffers);
        consoleLog("[INFO] wglSwapBuffers hook installed");
    }

    // ── Hook send/recv ───────────────────────────────────────────
    HMODULE hWS2 = GetModuleHandleA("ws2_32.dll");
    if (hWS2) {
        auto p_send = (void*)GetProcAddress(hWS2, "send");
        auto p_recv = (void*)GetProcAddress(hWS2, "recv");
        auto p_WSASend = (void*)GetProcAddress(hWS2, "WSASend");
        auto p_WSARecv = (void*)GetProcAddress(hWS2, "WSARecv");

        if (p_send) {
            MH_CreateHook(p_send, (void*)hk_send, (void**)&o_send);
            MH_EnableHook(p_send);
        }
        if (p_recv) {
            MH_CreateHook(p_recv, (void*)hk_recv, (void**)&o_recv);
            MH_EnableHook(p_recv);
        }
        if (p_WSASend) {
            MH_CreateHook(p_WSASend, (void*)hk_WSASend, (void**)&o_WSASend);
            MH_EnableHook(p_WSASend);
        }
        if (p_WSARecv) {
            MH_CreateHook(p_WSARecv, (void*)hk_WSARecv, (void**)&o_WSARecv);
            MH_EnableHook(p_WSARecv);
        }
        consoleLog("[INFO] send/recv hooks installed");
    } else {
        consoleLog("[INFO] ws2_32.dll not loaded yet, will retry...");
        Sleep(2000);
        hWS2 = GetModuleHandleA("ws2_32.dll");
        if (hWS2) {
            auto p_send = (void*)GetProcAddress(hWS2, "send");
            auto p_recv = (void*)GetProcAddress(hWS2, "recv");
            auto p_WSASend = (void*)GetProcAddress(hWS2, "WSASend");
            auto p_WSARecv = (void*)GetProcAddress(hWS2, "WSARecv");
            if (p_send) { MH_CreateHook(p_send, (void*)hk_send, (void**)&o_send); MH_EnableHook(p_send); }
            if (p_recv) { MH_CreateHook(p_recv, (void*)hk_recv, (void**)&o_recv); MH_EnableHook(p_recv); }
            if (p_WSASend) { MH_CreateHook(p_WSASend, (void*)hk_WSASend, (void**)&o_WSASend); MH_EnableHook(p_WSASend); }
            if (p_WSARecv) { MH_CreateHook(p_WSARecv, (void*)hk_WSARecv, (void**)&o_WSARecv); MH_EnableHook(p_WSARecv); }
            consoleLog("[INFO] send/recv hooks installed (delayed)");
        }
    }

    // ── Main loop ────────────────────────────────────────────────
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
