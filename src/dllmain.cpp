#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#include "lua_api.h"
#include "hook.h"
#include "scanner.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <mutex>
#include <MinHook.h>
#include <intrin.h>

float g_currentTime = 0;

static LONG WINAPI VehHandler(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
        ep->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW ||
        ep->ExceptionRecord->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION) {

        const char* dir = "C:\\Users\\LENOVO\\Documents\\groetopia\\cv dl script\\coems_executor\\package-scanner-output";

        // Find next available crash log file
        char path[MAX_PATH];
        int idx = 0;
        for (;;) {
            if (idx == 0)
                snprintf(path, sizeof(path), "%s\\crash.txt", dir);
            else
                snprintf(path, sizeof(path), "%s\\crash%d.txt", dir, idx);
            WIN32_FIND_DATAA fd;
            HANDLE hFind = FindFirstFileA(path, &fd);
            if (hFind == INVALID_HANDLE_VALUE) break;
            FindClose(hFind);
            idx++;
        }

        HANDLE hFile = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile != INVALID_HANDLE_VALUE) {
            DWORD written;
            char buf[512];

            const char* excName = "UNKNOWN";
            if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) excName = "ACCESS_VIOLATION";
            else if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW) excName = "STACK_OVERFLOW";
            else if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION) excName = "ILLEGAL_INSTRUCTION";

            int n = snprintf(buf, sizeof(buf),
                "=== CRASH LOG ===\r\n"
                "Exception: %s (0x%08X)\r\n"
                "Address: 0x%p\r\n"
                "Access type: %s\r\n"
                "Access address: 0x%p\r\n\r\n"
                "=== REGISTERS ===\r\n",
                excName, ep->ExceptionRecord->ExceptionCode,
                ep->ExceptionRecord->ExceptionAddress,
                ep->ExceptionRecord->ExceptionInformation[0] ? "WRITE" : "READ",
                (void*)ep->ExceptionRecord->ExceptionInformation[1]);
            WriteFile(hFile, buf, n, &written, nullptr);

#ifdef _WIN64
            CONTEXT* ctx = ep->ContextRecord;
            n = snprintf(buf, sizeof(buf),
                "RAX=0x%016llX  RBX=0x%016llX\r\n"
                "RCX=0x%016llX  RDX=0x%016llX\r\n"
                "RSI=0x%016llX  RDI=0x%016llX\r\n"
                "RSP=0x%016llX  RBP=0x%016llX\r\n"
                "R8 =0x%016llX  R9 =0x%016llX\r\n"
                "R10=0x%016llX  R11=0x%016llX\r\n"
                "R12=0x%016llX  R13=0x%016llX\r\n"
                "R14=0x%016llX  R15=0x%016llX\r\n"
                "RIP=0x%016llX  EFLAGS=0x%08X\r\n\r\n",
                ctx->Rax, ctx->Rbx, ctx->Rcx, ctx->Rdx,
                ctx->Rsi, ctx->Rdi, ctx->Rsp, ctx->Rbp,
                ctx->R8, ctx->R9, ctx->R10, ctx->R11,
                ctx->R12, ctx->R13, ctx->R14, ctx->R15,
                ctx->Rip, ctx->EFlags);
            WriteFile(hFile, buf, n, &written, nullptr);

            // Dump bytes around faulting instruction
            BYTE* rip = (BYTE*)ctx->Rip;
            n = snprintf(buf, sizeof(buf), "=== CODE AROUND RIP ===\r\n");
            WriteFile(hFile, buf, n, &written, nullptr);
            for (int i = -16; i < 32; i++) {
                BYTE* p = rip + i;
                if (i == 0) n = snprintf(buf, sizeof(buf), ">>> %02X ", *p);
                else n = snprintf(buf, sizeof(buf), "    %02X ", *p);
                WriteFile(hFile, buf, n, &written, nullptr);
                if ((i + 1) % 8 == 0) WriteFile(hFile, "\r\n", 2, &written, nullptr);
            }
#else
            CONTEXT* ctx = ep->ContextRecord;
            n = snprintf(buf, sizeof(buf),
                "EAX=0x%08X  EBX=0x%08X\r\n"
                "ECX=0x%08X  EDX=0x%08X\r\n"
                "ESI=0x%08X  EDI=0x%08X\r\n"
                "ESP=0x%08X  EBP=0x%08X\r\n"
                "EIP=0x%08X  EFLAGS=0x%08X\r\n\r\n",
                ctx->Eax, ctx->Ebx, ctx->Ecx, ctx->Edx,
                ctx->Esi, ctx->Edi, ctx->Esp, ctx->Ebp,
                ctx->Eip, ctx->EFlags);
            WriteFile(hFile, buf, n, &written, nullptr);
#endif

            // Dump stack
            n = snprintf(buf, sizeof(buf), "\r\n=== STACK (64 bytes) ===\r\n");
            WriteFile(hFile, buf, n, &written, nullptr);
#ifdef _WIN64
            uintptr_t* sp = (uintptr_t*)ctx->Rsp;
#else
            uintptr_t* sp = (uintptr_t*)ctx->Esp;
#endif
            for (int i = 0; i < 8; i++) {
                MEMORY_BASIC_INFORMATION mbi;
                bool readable = VirtualQuery(sp + i, &mbi, sizeof(mbi)) &&
                    mbi.State == MEM_COMMIT &&
                    (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY));
                if (readable)
                    n = snprintf(buf, sizeof(buf), "  [RSP+0x%02X] 0x%p\r\n", i * 8, (void*)sp[i]);
                else
                    n = snprintf(buf, sizeof(buf), "  [RSP+0x%02X] <unreadable>\r\n", i * 8);
                WriteFile(hFile, buf, n, &written, nullptr);
            }

            // Dump game function addresses
            n = snprintf(buf, sizeof(buf),
                "\r\n=== RESOLVED ADDRESSES ===\r\n"
                "ProcessTankUpdatePacket = 0x%p\r\n"
                "SendPacket = 0x%p\r\n"
                "GetGameLogic = 0x%p\r\n\r\n"
                "=== DEBUG LOG (last 30) ===\r\n",
                (void*)scanner::fn_ProcessTankUpdatePacket,
                (void*)scanner::fn_SendPacket,
                (void*)scanner::fn_GetGameLogic);
            WriteFile(hFile, buf, n, &written, nullptr);

            {
                std::lock_guard<std::mutex> lock(g_debugMutex);
                int start = (int)g_debugLogs.size() - 30;
                if (start < 0) start = 0;
                for (int i = start; i < (int)g_debugLogs.size(); i++) {
                    n = snprintf(buf, sizeof(buf), "%s\r\n", g_debugLogs[i].message.c_str());
                    WriteFile(hFile, buf, n, &written, nullptr);
                }
            }

            // Also write to debug log
            debugLog("[CRASH] " + std::string(excName) + " at 0x" +
                std::to_string((uintptr_t)ep->ExceptionRecord->ExceptionAddress) +
                " -> saved to " + path);

            FlushFileBuffers(hFile);
            CloseHandle(hFile);
        }

        return EXCEPTION_CONTINUE_SEARCH;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void debugLog(const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_debugMutex);
    g_debugLogs.push_back({msg, g_currentTime});
    if (g_debugLogs.size() > 1000) g_debugLogs.erase(g_debugLogs.begin());
}

// ── send/recv/sendto/recvfrom hooks ──────────────────────────────────
typedef int (WINAPI* send_t)(SOCKET s, const char* buf, int len, int flags);
typedef int (WINAPI* recv_t)(SOCKET s, char* buf, int len, int flags);
typedef int (WINAPI* sendto_t)(SOCKET s, const char* buf, int len, int flags, const struct sockaddr* to, int tolen);
typedef int (WINAPI* recvfrom_t)(SOCKET s, char* buf, int len, int flags, struct sockaddr* from, int* fromlen);
typedef int (WINAPI* WSASend_t)(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount,
    LPDWORD lpNumberOfBytesSent, DWORD dwFlags, LPWSAOVERLAPPED lpOverlapped,
    LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);
typedef int (WINAPI* WSARecv_t)(SOCKET s, LPWSABUF lpBuffers, DWORD dwBufferCount,
    LPDWORD lpNumberOfBytesRecvd, LPDWORD lpFlags, LPWSAOVERLAPPED lpOverlapped,
    LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);
typedef int (WINAPI* connect_t)(SOCKET s, const struct sockaddr* name, int namelen);
typedef SOCKET (WINAPI* WSASocketW_t)(int af, int type, int protocol, LPWSAPROTOCOL_INFOW lpProtocolInfo, GROUP g, DWORD dwFlags);

send_t o_send = nullptr;
recv_t o_recv = nullptr;
sendto_t o_sendto = nullptr;
recvfrom_t o_recvfrom = nullptr;
WSASend_t o_WSASend = nullptr;
WSARecv_t o_WSARecv = nullptr;
connect_t o_connect = nullptr;
WSASocketW_t o_WSASocketW = nullptr;
SOCKET g_GameSocket = INVALID_SOCKET;

static int g_SendCount = 0;

int WINAPI hk_send(SOCKET s, const char* buf, int len, int flags) {
    if (len > 4 && buf) {
        GameState::instance().parseOutgoing(buf, len);
    }
    g_SendCount++;
    if (g_SendCount <= 10) {
        void* retAddr = _ReturnAddress();
        char buf2[256];
        snprintf(buf2, sizeof(buf2), "[send #%d] len=%d ret=0x%p",
            g_SendCount, len, retAddr);
        consoleLog(buf2);
    }
    return o_send(s, buf, len, flags);
}

static int g_RecvCount = 0;

int WINAPI hk_recv(SOCKET s, char* buf, int len, int flags) {
    int result = o_recv(s, buf, len, flags);
    if (result > 4 && buf) {
        GameState::instance().parseIncoming(buf, result);
    }
    g_RecvCount++;
    if (g_RecvCount <= 10) {
        void* retAddr = _ReturnAddress();
        char buf2[256];
        snprintf(buf2, sizeof(buf2), "[recv #%d] len=%d ret=0x%p",
            g_RecvCount, result, retAddr);
        consoleLog(buf2);
    }
    return result;
}

int WINAPI hk_sendto(SOCKET s, const char* buf, int len, int flags, const struct sockaddr* to, int tolen) {
    if (len > 4 && buf) {
        GameState::instance().parseOutgoing(buf, len);
    }
    return o_sendto(s, buf, len, flags, to, tolen);
}

int WINAPI hk_recvfrom(SOCKET s, char* buf, int len, int flags, struct sockaddr* from, int* fromlen) {
    int result = o_recvfrom(s, buf, len, flags, from, fromlen);
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

int WINAPI hk_connect(SOCKET s, const struct sockaddr* name, int namelen) {
    if (name && namelen >= (int)sizeof(struct sockaddr_in)) {
        struct sockaddr_in* addr = (struct sockaddr_in*)name;
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip));
        int port = ntohs(addr->sin_port);
        debugLog("[NET] connect: " + std::string(ip) + ":" + std::to_string(port));
        if (port == 17100 || port == 17101) {
            g_GameSocket = s;
            debugLog("[NET] Game socket detected!");
        }
    }
    return o_connect(s, name, namelen);
}

SOCKET WINAPI hk_WSASocketW(int af, int type, int protocol, LPWSAPROTOCOL_INFOW lpProtocolInfo, GROUP g, DWORD dwFlags) {
    SOCKET s = o_WSASocketW(af, type, protocol, lpProtocolInfo, g, dwFlags);
    debugLog("[NET] WSASocketW: af=" + std::to_string(af) + " type=" + std::to_string(type) + " proto=" + std::to_string(protocol) + " socket=" + std::to_string((int)s));
    return s;
}

static void HookSocketFunction(HMODULE hWS2, const char* name, void* hook, void** original) {
    auto addr = (void*)GetProcAddress(hWS2, name);
    if (addr) {
        MH_STATUS st = MH_CreateHook(addr, hook, original);
        if (st == MH_OK) {
            MH_EnableHook(addr);
            debugLog("[HOOK] " + std::string(name) + " hooked OK");
        } else {
            debugLog("[HOOK] " + std::string(name) + " hook FAILED: " + std::to_string(st));
        }
    } else {
        debugLog("[HOOK] " + std::string(name) + " not found in ws2_32");
    }
}

static void InstallSocketHooks(HMODULE hWS2) {
    if (!hWS2) return;
    HookSocketFunction(hWS2, "send", (void*)hk_send, (void**)&o_send);
    HookSocketFunction(hWS2, "recv", (void*)hk_recv, (void**)&o_recv);
    HookSocketFunction(hWS2, "sendto", (void*)hk_sendto, (void**)&o_sendto);
    HookSocketFunction(hWS2, "recvfrom", (void*)hk_recvfrom, (void**)&o_recvfrom);
    HookSocketFunction(hWS2, "WSASend", (void*)hk_WSASend, (void**)&o_WSASend);
    HookSocketFunction(hWS2, "WSARecv", (void*)hk_WSARecv, (void**)&o_WSARecv);
    HookSocketFunction(hWS2, "connect", (void*)hk_connect, (void**)&o_connect);
    HookSocketFunction(hWS2, "WSASocketW", (void*)hk_WSASocketW, (void**)&o_WSASocketW);
}

bool g_SocketHooksInstalled = false;

// ── Game function pointers (resolved by scanner) ──────────────────────
static void* g_GameLogicPtr = nullptr;
static bool g_GameLogicCaptured = false;

void TryInstallSocketHooks() {
    if (g_SocketHooksInstalled) return;
    HMODULE hWS2 = GetModuleHandleA("ws2_32.dll");
    if (!hWS2) {
        hWS2 = LoadLibraryA("ws2_32.dll");
        if (hWS2) debugLog("[NET] ws2_32.dll force-loaded");
    }
    if (hWS2) {
        InstallSocketHooks(hWS2);
        g_SocketHooksInstalled = true;
        consoleLog("[INFO] All socket hooks installed");
    }
}



// ── ProcessTankUpdatePacket hook (decrypted packets) ────────────────
typedef void(__fastcall* PTUP_t)(void* logic, void* packet);
static PTUP_t o_ProcessTankUpdatePacket = nullptr;
static int g_PTUPCount = 0;

void __fastcall hk_ProcessTankUpdatePacket(void* logic, void* packet) {
    if (!packet) { if (o_ProcessTankUpdatePacket) o_ProcessTankUpdatePacket(logic, packet); return; }

    BYTE* pkt = (BYTE*)packet;
    uint8_t pktType = pkt[0];
    uint16_t pktSize = *(uint16_t*)(pkt + 2);

    g_PTUPCount++;
    if (g_PTUPCount <= 50) {
        // Log first 64 bytes of every packet
        char hex[256] = {};
        int pos = 0;
        for (int i = 0; i < 64 && i < pktSize && pos < 250; i++) {
            pos += snprintf(hex + pos, 256 - pos, "%02X ", pkt[i]);
        }
        char buf[512];
        snprintf(buf, sizeof(buf), "[PTUP #%d] type=%d size=%d %s", g_PTUPCount, pktType, pktSize, hex);
        consoleLog(buf);
    }

    // Parse text packets (type 4)
    if (pktType == 4 && pktSize > 4) {
        std::string text((char*)(pkt + 4), pktSize - 4);
        if (text.size() > 2) {
            GameState::instance().parseTextPacket(text, true);
        }
    }

    // Parse tank update packets (type 1, 2, 3) for player positions
    if (pktType == 1 && pktSize >= 56) {
        int netid = *(int*)(pkt + 4);
        float pos_x = *(float*)(pkt + 16);
        float pos_y = *(float*)(pkt + 20);
        int item = *(int*)(pkt + 44);

        auto& gs = GameState::instance();
        std::lock_guard<std::mutex> lock(gs.mtx);
        for (auto& p : gs.players) {
            if (p.netid == netid) {
                p.pos_x = pos_x;
                p.pos_y = pos_y;
                p.tile_x = (int)(pos_x / 32);
                p.tile_y = (int)(pos_y / 32);
                if (item != -1) p.item = item;
                break;
            }
        }
        if (netid == gs.localPlayer.netid || netid == -1) {
            gs.localPlayer.pos_x = pos_x;
            gs.localPlayer.pos_y = pos_y;
            gs.localPlayer.tile_x = (int)(pos_x / 32);
            gs.localPlayer.tile_y = (int)(pos_y / 32);
            if (item != -1) gs.localPlayer.item = item;
        }
    }

    if (o_ProcessTankUpdatePacket)
        o_ProcessTankUpdatePacket(logic, packet);
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

    AddVectoredExceptionHandler(1, VehHandler);
    consoleLog("[INFO] VEH handler installed");

    if (p_wglSwapBuffers) {
        MH_CreateHook(p_wglSwapBuffers, (void*)hk_wglSwapBuffers, (void**)&o_wglSwapBuffers);
        MH_EnableHook(p_wglSwapBuffers);
        consoleLog("[INFO] wglSwapBuffers hook installed");
    }

    // ── Force load ws2_32 if not yet loaded ──────────────────────
    TryInstallSocketHooks();

    // ── Scan for game functions in ALL executable memory ──────────
    HMODULE hGame = GetModuleHandleA(NULL);
    if (hGame) {
        scanner::Install(hGame);
    }

    // ── Hook ProcessTankUpdatePacket (decrypted packets) ─────────
    if (scanner::fn_ProcessTankUpdatePacket) {
        uintptr_t ptupAddr = scanner::fn_ProcessTankUpdatePacket;

        MEMORY_BASIC_INFORMATION mbi;
        VirtualQuery((void*)ptupAddr, &mbi, sizeof(mbi));
        char buf2[256];
        snprintf(buf2, sizeof(buf2), "[INFO] PTUP addr=0x%p prot=0x%X state=0x%X",
            (void*)ptupAddr, mbi.Protect, mbi.State);
        consoleLog(buf2);

        BYTE* codeBytes = (BYTE*)ptupAddr;
        char hex[128] = {};
        int pos = 0;
        for (int i = 0; i < 32 && pos < 120; i++) {
            pos += snprintf(hex + pos, 128 - pos, "%02X ", codeBytes[i]);
        }
        snprintf(buf2, sizeof(buf2), "[INFO] PTUP bytes: %s", hex);
        consoleLog(buf2);

        auto pPTUP = (PTUP_t)ptupAddr;
        MH_STATUS st = MH_CreateHook((void*)pPTUP, (void*)hk_ProcessTankUpdatePacket, (void**)&o_ProcessTankUpdatePacket);
        snprintf(buf2, sizeof(buf2), "[INFO] MH_CreateHook PTUP: %d", (int)st);
        consoleLog(buf2);
        if (st == MH_OK) {
            MH_STATUS st2 = MH_EnableHook((void*)pPTUP);
            snprintf(buf2, sizeof(buf2), "[INFO] MH_EnableHook PTUP: %d", (int)st2);
            consoleLog(buf2);
        }
    } else {
        consoleLog("[FAIL] ProcessTankUpdatePacket NOT FOUND");
    }

    // ── Main loop ────────────────────────────────────────────────
    auto t0 = std::chrono::steady_clock::now();
    while (true) {
        Sleep(50);
        auto now = std::chrono::steady_clock::now();
        float elapsed = std::chrono::duration<float>(now - t0).count();
        g_currentTime = elapsed;
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
