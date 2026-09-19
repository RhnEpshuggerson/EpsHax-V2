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
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

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
    if (g_SendCount <= 30) {
        void* retAddr = _ReturnAddress();
        char hex[128] = {};
        int pos = 0;
        for (int i = 0; i < 32 && i < len && pos < 120; i++) {
            pos += snprintf(hex + pos, 128 - pos, "%02X ", (unsigned char)buf[i]);
        }
        char buf2[512];
        snprintf(buf2, sizeof(buf2), "[SEND #%d] len=%d ret=0x%p %s",
            g_SendCount, len, retAddr, hex);
        consoleLog(buf2);
    }
    return o_send(s, buf, len, flags);
}

static int g_RecvCount = 0;
static int g_TlsAppDataCount = 0;

int WINAPI hk_recv(SOCKET s, char* buf, int len, int flags) {
    int result = o_recv(s, buf, len, flags);
    if (result > 4 && buf) {
        GameState::instance().parseIncoming(buf, result);
    }
    g_RecvCount++;
    if (result >= 5 && buf) {
        bool isTlsAppData = ((unsigned char)buf[0] == 0x17 &&
                             (unsigned char)buf[1] == 0x03 &&
                             (unsigned char)buf[2] == 0x03);
        if (isTlsAppData && g_TlsAppDataCount < 3) {
            g_TlsAppDataCount++;
            void* frames[64] = {};
            USHORT frameCount = CaptureStackBackTrace(0, 64, frames, NULL);
            char hex[256] = {};
            int pos = 0;
            for (int i = 0; i < 32 && i < result && pos < 240; i++) {
                pos += snprintf(hex + pos, 240 - pos, "%02X ", (unsigned char)buf[i]);
            }
            char msg[4096];
            int m = snprintf(msg, sizeof(msg),
                "[TLS-APPDATA #%d] len=%d frames=%d\n  DATA: %s\n  FRAMES:",
                g_TlsAppDataCount, result, frameCount, hex);
            for (USHORT i = 0; i < frameCount && m < 3900; i++) {
                m += snprintf(msg + m, 3900 - m, "\n  [%d] 0x%p", i, frames[i]);
                MEMORY_BASIC_INFORMATION mbi = {};
                if (VirtualQuery(frames[i], &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT) {
                    unsigned char* bytes = (unsigned char*)frames[i];
                    m += snprintf(msg + m, 3900 - m, " bytes=");
                    for (int b = 0; b < 16 && m < 3900; b++) {
                        m += snprintf(msg + m, 3900 - m, "%02X ", bytes[b]);
                    }
                } else {
                    m += snprintf(msg + m, 3900 - m, " [unmapped]");
                }
            }
            consoleLog(msg);
        }
    }
    if (g_RecvCount <= 30 && result > 0) {
        void* retAddr = _ReturnAddress();
        char hex[128] = {};
        int pos = 0;
        for (int i = 0; i < 32 && i < result && pos < 120; i++) {
            pos += snprintf(hex + pos, 128 - pos, "%02X ", (unsigned char)buf[i]);
        }
        char buf2[512];
        snprintf(buf2, sizeof(buf2), "[RECV #%d] len=%d ret=0x%p %s",
            g_RecvCount, result, retAddr, hex);
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

// ── TLS hooks (DecryptMessage) ──────────────────────────────────────
#define SECURITY_WIN32
#include <windows.h>
#include <sspi.h>
#include <security.h>

typedef SECURITY_STATUS(WINAPI* DecryptMessage_t)(PCtxtHandle, PSecBufferDesc, ULONG, PULONG);
static DecryptMessage_t o_DecryptMessage = nullptr;
static int g_TlsCount = 0;

SECURITY_STATUS WINAPI hk_DecryptMessage(PCtxtHandle phContext, PSecBufferDesc pMessage, ULONG MessageSeqNo, PULONG pulQOP) {
    SECURITY_STATUS status = o_DecryptMessage(phContext, pMessage, MessageSeqNo, pulQOP);

    if (status == SEC_E_OK && pMessage && pMessage->cBuffers >= 1) {
        for (ULONG i = 0; i < pMessage->cBuffers; i++) {
            SecBuffer* buf = &pMessage->pBuffers[i];
            if (buf->BufferType == SECBUFFER_DATA && buf->cbBuffer > 4 && buf->pvBuffer) {
                BYTE* data = (BYTE*)buf->pvBuffer;
                uint32_t header = *(uint32_t*)data;
                int pktType = header & 0xFF;

                g_TlsCount++;
                if (g_TlsCount <= 50) {
                    char hex[128] = {};
                    int pos = 0;
                    for (int j = 0; j < 32 && j < (int)buf->cbBuffer && pos < 120; j++) {
                        pos += snprintf(hex + pos, 128 - pos, "%02X ", data[j]);
                    }
                    char buf2[512];
                    snprintf(buf2, sizeof(buf2), "[TLS #%d] type=%d len=%d %s",
                        g_TlsCount, pktType, buf->cbBuffer, hex);
                    consoleLog(buf2);
                }

                if (pktType == 4 && buf->cbBuffer > 4) {
                    std::string text((char*)(data + 4), buf->cbBuffer - 4);
                    if (text.size() > 2) {
                        GameState::instance().parseTextPacket(text, true);
                    }
                }

                if ((pktType == 1 || pktType == 2 || pktType == 3) && buf->cbBuffer >= 16) {
                    GameState::instance().parseIncoming((const char*)data, buf->cbBuffer);
                }
            }
        }
    }

    return status;
}

void TryInstallTLSHooks() {
    HMODULE hSec = GetModuleHandleA("sspicli.dll");
    if (!hSec) hSec = GetModuleHandleA("secur32.dll");
    if (!hSec) hSec = GetModuleHandleA("schannel.dll");
    if (!hSec) {
        consoleLog("[INFO] No Windows TLS DLL loaded (sspicli/secur32/schannel) - game uses own TLS");
        return;
    }
    auto addr = (void*)GetProcAddress(hSec, "DecryptMessage");
    if (!addr) addr = (void*)GetProcAddress(hSec, "SslDecryptPacket");
    if (addr) {
        MH_STATUS st = MH_CreateHook(addr, (void*)hk_DecryptMessage, (void**)&o_DecryptMessage);
        if (st == MH_OK) {
            MH_EnableHook(addr);
            consoleLog("[INFO] TLS decrypt hook installed OK");
        } else {
            char buf[128];
            snprintf(buf, sizeof(buf), "[WARN] TLS decrypt hook failed: %d", (int)st);
            consoleLog(buf);
        }
    } else {
        consoleLog("[INFO] DecryptMessage/SslDecryptPacket not exported - game uses own TLS");
    }
}

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

// ── Main thread ──────────────────────────────────────────────────────

void MainThread(HMODULE hModule) {
    while (!GetModuleHandleA("opengl32.dll")) Sleep(100);
    Sleep(500);

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

    TryInstallSocketHooks();
    TryInstallTLSHooks();

    HMODULE hGame = GetModuleHandleA(NULL);
    if (hGame) {
        scanner::Install(hGame);
    }

    auto t0 = std::chrono::steady_clock::now();
    while (true) {
        Sleep(100);
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
