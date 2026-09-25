#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#include "lua_api.h"
#include "hook.h"
#include "scanner.h"
#include "native_hook.h"
#include "discord_rpc.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>
#include <mutex>
#include <intrin.h>
#include <dbghelp.h>
#include "epshook.h"
#pragma comment(lib, "dbghelp.lib")

// Set to 1 to enable native hook (code patching). DISABLED by default because
// Themida/VMProtect anti-tamper in Growtopia detects E9 JMP patches at game
// code addresses and crashes the process (ACCESS_VIOLATION at 0xD17DFFF5).
// The BCrypt/TLS/socket hooks already capture all packet data without patching
// game code, so the native hook is not needed for normal operation.
#define ENABLE_NATIVE_HOOK 0

float g_currentTime = 0;

// When true, suppress VEH crash logging (scanners hit unmapped memory intentionally)
volatile LONG g_ScanningActive = 0;

// ── Heap scanner for decrypted packet data ─────────────────────────
static bool g_HeapScanEnabled = true;
static int g_HeapScanHits = 0;
static uintptr_t g_LastScanRegion = 0;

static bool IsHeapReadable(uintptr_t addr, size_t len) {
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery((void*)addr, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (len > mbi.RegionSize) return false;
    DWORD bad = PAGE_NOACCESS | PAGE_GUARD | PAGE_EXECUTE | PAGE_EXECUTE_READ;
    return (mbi.Protect & bad) == 0 && (mbi.Protect != 0);
}

static bool IsAsciiPrintable(unsigned char c) {
    return (c >= 0x20 && c <= 0x7E) || c == '\n' || c == '\r' || c == '\t';
}

static bool SafeMemCmp(const BYTE* a, const BYTE* b, size_t len) {
    __try {
        return memcmp(a, b, len) == 0;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static int SafeAsciiLen(const BYTE* base, size_t maxSize) {
    int len = 0;
    __try {
        while (len < 512 && (size_t)len < maxSize) {
            unsigned char c = base[len];
            if (c == 0) break;
            if (!IsAsciiPrintable(c) && c != '\n' && c != '\r') return 0;
            len++;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return len;
}

// Extract a single packet from memory: reads until double newline or null
extern "C" {
    static const BYTE* s_extractBase = nullptr;
    static size_t s_extractMax = 0;
    static char s_extractBuf[512];
    static int s_extractLen = 0;

    static void __cdecl DoExtractPacket() {
        s_extractLen = 0;
        size_t maxLen = (s_extractMax < 512) ? s_extractMax : 512;
        size_t i = 0;
        int newlines = 0;
        while (i < maxLen) {
            unsigned char c = s_extractBase[i];
            if (c == 0) break;
            if (c == '\n') {
                newlines++;
                if (newlines >= 2) break;
            } else {
                newlines = 0;
            }
            if (!IsAsciiPrintable(c) && c != '\n' && c != '\r') break;
            s_extractBuf[s_extractLen++] = (char)c;
            i++;
        }
        s_extractBuf[s_extractLen] = '\0';
    }

    static int __cdecl TryExtractPacket(const BYTE* base, size_t maxSize) {
        s_extractBase = base;
        s_extractMax = maxSize;
        s_extractLen = 0;
        s_extractBuf[0] = '\0';
        __try {
            DoExtractPacket();
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            s_extractLen = 0;
            s_extractBuf[0] = '\0';
        }
        return s_extractLen;
    }
}

static std::string SafeExtractPacket(const BYTE* base, size_t maxSize) {
    int len = TryExtractPacket(base, maxSize);
    return std::string(s_extractBuf, len);
}

void ScanHeapForPackets() {
    if (!g_HeapScanEnabled) return;

    InterlockedExchange(&g_ScanningActive, 1);

    const char* markers[] = {
        "action|spawn",
        "action|on_spawn",
        "on_varlist",
        "set_field_init",
        "set_field_update",
        "on_requestWorldSelectMenu",
        "on_chat_message",
        "on_killed",
        "on_disconnect",
        "play_sfx",
    };
    const int markerCount = sizeof(markers) / sizeof(markers[0]);

    static std::vector<std::string> recentPackets;

    uintptr_t addr = 0;
    MEMORY_BASIC_INFORMATION mbi = {};
    int regionsScanned = 0;

    while (addr < 0x7FFFFFFFFFFFFFFF && regionsScanned < 500) {
        if (!VirtualQuery((void*)addr, &mbi, sizeof(mbi))) break;
        if (mbi.State == MEM_COMMIT && mbi.RegionSize >= 64 && mbi.RegionSize < 0x8000000) {
            DWORD prot = mbi.Protect;
            // Skip executable regions — text packets are in heap (RW) memory
            bool isExec = (prot & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
            bool noAccess = (prot & (PAGE_NOACCESS | PAGE_GUARD)) != 0;
            bool isReadable = (prot & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;

            if (isReadable && !isExec && !noAccess) {
                BYTE* base = (BYTE*)mbi.BaseAddress;
                size_t size = mbi.RegionSize;

                for (int m = 0; m < markerCount; m++) {
                    size_t markerLen = strlen(markers[m]);
                    if (markerLen + 20 >= size) continue;

                    for (size_t i = 0; i + markerLen + 10 < size; i++) {
                        if (!SafeMemCmp(base + i, (const BYTE*)markers[m], markerLen)) continue;

                        std::string text = SafeExtractPacket(base + i, size - i);
                        if (text.size() > markerLen + 5) {
                            size_t firstLineEnd = text.find('\n');
                            if (firstLineEnd != std::string::npos && firstLineEnd > 2) {
                                std::string firstLine = text.substr(0, firstLineEnd);
                                size_t pipe = firstLine.find('|');
                                if (pipe != std::string::npos) {
                                    std::string key = firstLine.substr(0, pipe);
                                    bool valid = true; // Found by specific marker, accept it
                                    if (valid) {
                                        std::string preview = text.substr(0, 80);
                                        bool dup = false;
                                        for (auto& r : recentPackets) { if (r == preview) { dup = true; break; } }
                                        if (!dup) {
                                            recentPackets.push_back(preview);
                                            if (recentPackets.size() > 100) recentPackets.erase(recentPackets.begin());
                                            g_HeapScanHits++;
                                            GameState::instance().parseTextPacket(text, true);
                                            consoleLog("[HEAPSCAN #" + std::to_string(g_HeapScanHits) + "] " + text.substr(0, 120));
                                        }
                                    }
                                }
                            }
                            break;
                        }
                    }
                }
            }
            regionsScanned++;
        }
        addr = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (addr <= (uintptr_t)mbi.BaseAddress) break;
    }

    InterlockedExchange(&g_ScanningActive, 0);
}

// Pure-C crash writer (no C++ objects in scope so __try works)
// Called from VehHandler with copied context so nested exceptions are safe
static void WriteCrashLog(DWORD excCode, void* excAddr, DWORD_PTR accessType, DWORD_PTR accessAddr, CONTEXT* ctx) {
    const char* dir = "C:\\Users\\LENOVO\\Documents\\groetopia\\cv dl script\\coems_executor\\package-scanner-output\\Crash log";
    CreateDirectoryA(dir, nullptr);

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
    if (hFile == INVALID_HANDLE_VALUE) return;

    DWORD written;
    char buf[512];

    const char* excName = "UNKNOWN";
    if (excCode == EXCEPTION_ACCESS_VIOLATION) excName = "ACCESS_VIOLATION";
    else if (excCode == EXCEPTION_STACK_OVERFLOW) excName = "STACK_OVERFLOW";
    else if (excCode == EXCEPTION_ILLEGAL_INSTRUCTION) excName = "ILLEGAL_INSTRUCTION";

    int n = snprintf(buf, sizeof(buf),
        "=== CRASH LOG ===\r\n"
        "Exception: %s (0x%08X)\r\n"
        "Address: 0x%p\r\n"
        "Access type: %s\r\n"
        "Access address: 0x%p\r\n\r\n"
        "=== REGISTERS ===\r\n",
        excName, excCode, excAddr,
        accessType ? "WRITE" : "READ",
        (void*)accessAddr);
    WriteFile(hFile, buf, n, &written, nullptr);

#ifdef _WIN64
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

    // Dump bytes around faulting instruction — wrapped in __try because the
    // address may be completely unmapped (anti-tamper redirect, etc.)
    {
        BYTE* rip = (BYTE*)ctx->Rip;
        n = snprintf(buf, sizeof(buf), "=== CODE AROUND RIP ===\r\n");
        WriteFile(hFile, buf, n, &written, nullptr);
        for (int i = -16; i < 32; i++) {
            BYTE* p = rip + i;
            BYTE val = 0;
            __try {
                val = *p;
                n = (i == 0)
                    ? snprintf(buf, sizeof(buf), ">>> %02X ", val)
                    : snprintf(buf, sizeof(buf), "    %02X ", val);
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                n = snprintf(buf, sizeof(buf), "    ?? ");
            }
            WriteFile(hFile, buf, n, &written, nullptr);
            if ((i + 1) % 8 == 0) WriteFile(hFile, "\r\n", 2, &written, nullptr);
        }
    }
#else
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
        uintptr_t val = 0;
        __try {
            val = sp[i];
            n = snprintf(buf, sizeof(buf), "  [RSP+0x%02X] 0x%p\r\n", i * 8, (void*)val);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            n = snprintf(buf, sizeof(buf), "  [RSP+0x%02X] <unreadable>\r\n", i * 8);
        }
        WriteFile(hFile, buf, n, &written, nullptr);
    }

    // Dump game function addresses
    n = snprintf(buf, sizeof(buf),
        "\r\n=== RESOLVED ADDRESSES ===\r\n"
        "ProcessTankUpdatePacket = 0x%p\r\n"
        "SendPacket = 0x%p\r\n"
        "GetGameLogic = 0x%p\r\n",
        (void*)scanner::fn_ProcessTankUpdatePacket,
        (void*)scanner::fn_SendPacket,
        (void*)scanner::fn_GetGameLogic);
    WriteFile(hFile, buf, n, &written, nullptr);

    FlushFileBuffers(hFile);
    CloseHandle(hFile);
}

static LONG WINAPI VehHandler(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
        ep->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW ||
        ep->ExceptionRecord->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION) {

        // Suppress crash logs during memory scans — scanners intentionally hit unmapped memory
        if (InterlockedCompareExchange(&g_ScanningActive, 0, 0) != 0) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        // Rate-limit crash logs to at most 1 per 5 seconds
        static DWORD lastCrashLogTime = 0;
        DWORD now = GetTickCount();
        if (now - lastCrashLogTime < 5000) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        lastCrashLogTime = now;

        // Prevent nested VEH calls (our handler crashing triggers another exception)
        static LONG g_inVeh = 0;
        if (InterlockedCompareExchange(&g_inVeh, 1, 0) != 0) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        // Extract exception info and copy context before calling C function
        DWORD excCode = ep->ExceptionRecord->ExceptionCode;
        void* excAddr = ep->ExceptionRecord->ExceptionAddress;
        DWORD_PTR accessType = ep->ExceptionRecord->ExceptionInformation[0];
        DWORD_PTR accessAddr = ep->ExceptionRecord->ExceptionInformation[1];
        CONTEXT ctxCopy = {};
#ifdef _WIN64
        memcpy(&ctxCopy, ep->ContextRecord, sizeof(CONTEXT));
#else
        memcpy(&ctxCopy, ep->ContextRecord, sizeof(CONTEXT));
#endif

        WriteCrashLog(excCode, excAddr, accessType, accessAddr, &ctxCopy);

        InterlockedExchange(&g_inVeh, 0);
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
        // Parse plaintext game packets directly (skip TLS records)
        bool isTls = (result >= 3 && (unsigned char)buf[0] == 0x17 &&
                      (unsigned char)buf[1] == 0x03 && (unsigned char)buf[2] == 0x03);
        if (!isTls) {
            GameState::instance().parseIncoming(buf, result);
        }
        static int heapScanCounter = 0;
        if (++heapScanCounter % 10 == 0) {
            ScanHeapForPackets();
        }
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
        epshook::Status st = epshook::Create(addr, hook, original);
        if (st == epshook::OK) {
            debugLog("[HOOK] " + std::string(name) + " hooked OK");
        } else {
            debugLog("[HOOK] " + std::string(name) + " hook FAILED: " + epshook::StatusString(st));
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
        epshook::Status st = epshook::Create(addr, (void*)hk_DecryptMessage, (void**)&o_DecryptMessage);
        if (st == epshook::OK) {
            consoleLog("[INFO] TLS decrypt hook installed OK");
        } else {
            char buf[160];
            snprintf(buf, sizeof(buf), "[WARN] TLS decrypt hook failed: %s", epshook::StatusString(st));
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
        consoleLog("[INFO] Socket hooks installed (send/recv/WSA/connect)");
    }
}

// ── BCrypt decrypt hook ────────────────────────────────────────────
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

typedef NTSTATUS (WINAPI* BCryptDecrypt_t)(
    BCRYPT_KEY_HANDLE hKey, PUCHAR pbInput, ULONG cbInput,
    void* pPaddingInfo, PUCHAR pbIV, ULONG cbIV,
    PUCHAR pbOutput, ULONG cbOutput, ULONG* pcbResult, ULONG dwFlags);

static BCryptDecrypt_t o_BCryptDecrypt = nullptr;
static int g_BCryptCount = 0;

NTSTATUS WINAPI hk_BCryptDecrypt(
    BCRYPT_KEY_HANDLE hKey, PUCHAR pbInput, ULONG cbInput,
    void* pPaddingInfo, PUCHAR pbIV, ULONG cbIV,
    PUCHAR pbOutput, ULONG cbOutput, ULONG* pcbResult, ULONG dwFlags)
{
    NTSTATUS status = o_BCryptDecrypt(hKey, pbInput, cbInput, pPaddingInfo,
        pbIV, cbIV, pbOutput, cbOutput, pcbResult, dwFlags);

    if (status >= 0 && pbOutput && cbOutput >= 4) {
        g_BCryptCount++;
        if (g_BCryptCount <= 20) {
            char hex[256] = {};
            int pos = 0;
            for (UINT i = 0; i < 64 && i < cbOutput && pos < 240; i++) {
                pos += snprintf(hex + pos, 240 - pos, "%02X ", pbOutput[i]);
                if ((i + 1) % 32 == 0) pos += snprintf(hex + pos, 240 - pos, "\n    ");
            }
            char msg[512];
            snprintf(msg, sizeof(msg), "[BCRYPT #%d] outLen=%d pcbResult=%d\n    %s",
                g_BCryptCount, cbOutput, pcbResult ? (int)*pcbResult : -1, hex);
            consoleLog(msg);

            uint32_t header = *(uint32_t*)pbOutput;
            int pktType = header & 0xFF;

            if (pktType == 4 && cbOutput > 4) {
                std::string text((char*)(pbOutput + 4), cbOutput - 4);
                if (text.size() > 2) {
                    GameState::instance().parseTextPacket(text, true);
                    consoleLog("[BCRYPT] Parsed type=4 text packet, len=" + std::to_string(text.size()));
                }
            }

            if ((pktType == 1 || pktType == 2 || pktType == 3) && cbOutput >= 16) {
                GameState::instance().parseIncoming((const char*)pbOutput, cbOutput);
                consoleLog("[BCRYPT] Parsed type=" + std::to_string(pktType) + " raw packet");
            }
        }
    }

    return status;
}

void TryInstallBCryptHooks() {
    const char* dlls[] = { "bcrypt.dll", "bcryptprimitives.dll" };
    for (const char* dll : dlls) {
        HMODULE h = GetModuleHandleA(dll);
        if (!h) h = LoadLibraryA(dll);
        if (!h) continue;

        auto addr = (void*)GetProcAddress(h, "BCryptDecrypt");
        if (addr && !o_BCryptDecrypt) {
            epshook::Status st = epshook::Create(addr, (void*)hk_BCryptDecrypt, (void**)&o_BCryptDecrypt);
            if (st == epshook::OK) {
                consoleLog("[INFO] BCryptDecrypt hooked in " + std::string(dll));
            } else {
                char buf[160];
                snprintf(buf, sizeof(buf), "[WARN] BCryptDecrypt hook failed: %s", epshook::StatusString(st));
                consoleLog(buf);
            }
        }
    }
    if (!o_BCryptDecrypt) {
        consoleLog("[INFO] BCryptDecrypt not found - game may use own crypto");
    }
}

// ── Main thread ──────────────────────────────────────────────────────

#if ENABLE_NATIVE_HOOK
// Native hook: packet dispatcher at 0x1417E89CB
// This function receives decrypted packets after TLS processing
// EAX = packet type, RCX = data pointer (varies by type)
static int g_NativeHookHits = 0;

extern "C" void hk_PacketDispatcher(SavedRegs* regs) {
    g_NativeHookHits++;

    char buf[512];

    if (g_NativeHookHits <= 200) {
        snprintf(buf, sizeof(buf),
            "[NATIVEHOOK #%d] RAX=0x%llX RCX=0x%llX RDX=0x%llX R8=0x%llX R9=0x%llX R10=0x%llX R11=0x%llX",
            g_NativeHookHits,
            (unsigned long long)regs->rax, (unsigned long long)regs->rcx,
            (unsigned long long)regs->rdx, (unsigned long long)regs->r8,
            (unsigned long long)regs->r9, (unsigned long long)regs->r10,
            (unsigned long long)regs->r11);
        consoleLog(buf);

        uintptr_t dataPtr = regs->rcx;
        MEMORY_BASIC_INFORMATION mbi = {};
        if (dataPtr > 0x10000 && dataPtr < 0x7FFFFFFFFFFF && VirtualQuery((void*)dataPtr, &mbi, sizeof(mbi))) {
            char hex[512] = {};
            int pos = 0;
            for (unsigned int i = 0; i < 64 && pos < 480; i++) {
                unsigned char b = ((unsigned char*)dataPtr)[i];
                pos += snprintf(hex + pos, 512 - pos, "%02X ", b);
                if ((i + 1) % 32 == 0) pos += snprintf(hex + pos, 512 - pos, "\n    ");
            }
            snprintf(buf, sizeof(buf), "[NATIVEHOOK] RCX data: %s", hex);
            consoleLog(buf);

            uint32_t header = *(uint32_t*)dataPtr;
            int pktType = header & 0xFF;
            if (pktType == 4 && dataPtr > 0x10000) {
                const char* text = (const char*)(dataPtr + 4);
                bool valid = true;
                for (size_t i = 0; i < 256 && text[i]; i++) {
                    if ((unsigned char)text[i] < 0x20 && text[i] != '\n' && text[i] != '\r') { valid = false; break; }
                }
                if (valid && strlen(text) > 4) {
                    std::string pktText(text, strnlen(text, 256));
                    GameState::instance().parseTextPacket(pktText, true);
                    consoleLog("[NATIVEHOOK] PARSED TEXT: " + pktText.substr(0, 200));
                }
            }
        }
    }
}

void TryInstallNativeHooks() {
    native_hook::SetDispatchCallback(hk_PacketDispatcher);

    uintptr_t targets[] = {
        0x1417E89CB,
        0x1417E8360,
        0x1417C5CC1,
        0x1417F1DB0,
        0x1419EBD08,
    };
    char buf[256];

    for (auto target : targets) {
        snprintf(buf, sizeof(buf), "[NATIVEHOOK] Trying target 0x%p", (void*)target);
        consoleLog(buf);

        void* original = nullptr;
        if (native_hook::Install((void*)target, (void*)hk_PacketDispatcher, &original)) {
            snprintf(buf, sizeof(buf), "[NATIVEHOOK] SUCCESS - hooked at 0x%p", (void*)target);
            consoleLog(buf);
            return;
        }
    }
    consoleLog("[NATIVEHOOK] All targets failed");
}
#endif // ENABLE_NATIVE_HOOK

void MainThread(HMODULE hModule) {
    while (!GetModuleHandleA("opengl32.dll")) Sleep(100);
    Sleep(500);

    HMODULE hOGL = GetModuleHandleA("opengl32.dll");
    auto p_wglSwapBuffers = (void*)GetProcAddress(hOGL, "wglSwapBuffers");

    AddVectoredExceptionHandler(1, VehHandler);
    consoleLog("[INFO] VEH handler installed");

    if (p_wglSwapBuffers) {
        epshook::Status st = epshook::Create(p_wglSwapBuffers, (void*)hk_wglSwapBuffers, (void**)&o_wglSwapBuffers);
        if (st == epshook::OK) {
            consoleLog("[INFO] wglSwapBuffers hook installed");
        } else {
            char buf[160];
            snprintf(buf, sizeof(buf), "[WARN] wglSwapBuffers hook failed: %s", epshook::StatusString(st));
            consoleLog(buf);
        }
    }

    discordrpc::Start([](const char* m) {
        consoleLog(std::string("[RPC] ") + m);
    });

    // System DLL hooks only when GrowPai is NOT loaded.
    // If GrowPai is present, its bridge.json provides all game state —
    // patching ws2_32/bcrypt first makes GrowPai's sigs::init fail and crash.
    // GrowPai finishes sigs::init in ~1.5s; wait up to 15s re-checking so
    // late-loaded GrowPai still wins the race.
    auto isGrowPai = []() -> bool {
        return GetModuleHandleA("Growpai.dll") != nullptr ||
               GetModuleHandleA("GrowPai.dll") != nullptr;
    };
    if (isGrowPai()) {
        consoleLog("[INFO] GrowPai detected — skipping system DLL hooks (bridge provides data)");
    } else {
        bool gotGrowPai = false;
        for (int i = 0; i < 15; i++) {
            Sleep(1000);
            if (isGrowPai()) {
                gotGrowPai = true;
                consoleLog("[INFO] GrowPai loaded during delay — skipping system DLL hooks");
                break;
            }
        }
        if (!gotGrowPai) {
            TryInstallSocketHooks();
            TryInstallTLSHooks();
            TryInstallBCryptHooks();
            consoleLog("[INFO] System DLL hooks ENABLED (no GrowPai after 15s)");
        }
    }

    HMODULE hGame = GetModuleHandleA(NULL);
    if (hGame) {
        scanner::Install(hGame);
    }

#if ENABLE_NATIVE_HOOK
    Sleep(1000);
    TryInstallNativeHooks();
#else
    consoleLog("[INFO] Native hook DISABLED (ENABLE_NATIVE_HOOK=0) - packet capture via heap scanner");
#endif

    // Background scanner thread — scans heap every 500ms without blocking render
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        while (true) {
            Sleep(500);
            ScanHeapForPackets();
        }
        return 0;
    }, nullptr, 0, nullptr);
    consoleLog("[INFO] Background heap scanner started (500ms interval)");

    // Bridge reader thread — reads GrowPai's bridge.json for game state
    CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        const char* bridgePath = "C:\\temp\\growpai_bridge.json";
        while (true) {
                // Read and update, then sleep before next read
                HANDLE hFile = CreateFileA(bridgePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
                if (hFile == INVALID_HANDLE_VALUE) { Sleep(2000); continue; }
                DWORD fileSize = GetFileSize(hFile, nullptr);
                if (fileSize == 0 || fileSize > 262144) { CloseHandle(hFile); Sleep(2000); continue; }
            char* buf = (char*)malloc(fileSize + 1);
            DWORD read = 0;
            ReadFile(hFile, buf, fileSize, &read, nullptr);
            CloseHandle(hFile);
            buf[read] = '\0';
            std::string json(buf, read);
            free(buf);

            auto& gs = GameState::instance();
            std::lock_guard<std::mutex> lock(gs.mtx);

            // Extract a JSON object's body given a top-level key
            auto extractObject = [&](const std::string& key) -> std::string {
                std::string needle = "\"" + key + "\":";
                size_t pos = json.find(needle);
                if (pos == std::string::npos) return "";
                pos += needle.size();
                while (pos < json.size() && json[pos] == ' ') pos++;
                if (pos >= json.size() || json[pos] != '{') return "";
                int depth = 0;
                size_t start = pos;
                for (; pos < json.size(); pos++) {
                    if (json[pos] == '{') depth++;
                    else if (json[pos] == '}') { depth--; if (depth == 0) return json.substr(start, pos - start + 1); }
                }
                return "";
            };
            auto extractArray = [&](const std::string& key) -> std::string {
                std::string needle = "\"" + key + "\":";
                size_t pos = json.find(needle);
                if (pos == std::string::npos) return "";
                pos += needle.size();
                while (pos < json.size() && json[pos] == ' ') pos++;
                if (pos >= json.size() || json[pos] != '[') return "";
                int depth = 0;
                size_t start = pos;
                for (; pos < json.size(); pos++) {
                    if (json[pos] == '[') depth++;
                    else if (json[pos] == ']') { depth--; if (depth == 0) return json.substr(start, pos - start + 1); }
                }
                return "";
            };

            // Parse localPlayer object
            std::string lpObj = extractObject("localPlayer");
            auto findField = [&](const std::string& obj, const std::string& field) -> std::string {
                std::string needle = "\"" + field + "\"";
                size_t pos = obj.find(needle);
                if (pos == std::string::npos) return "";
                pos = obj.find(':', pos + needle.size());
                if (pos == std::string::npos) return "";
                pos++;
                while (pos < obj.size() && obj[pos] == ' ') pos++;
                if (pos >= obj.size()) return "";
                if (obj[pos] == '"') {
                    size_t end = obj.find('"', pos + 1);
                    if (end == std::string::npos) return "";
                    return obj.substr(pos + 1, end - pos - 1);
                }
                size_t end = pos;
                while (end < obj.size() && obj[end] != ',' && obj[end] != '}' && obj[end] != ']') end++;
                return obj.substr(pos, end - pos);
            };

            std::string name = findField(lpObj, "name");
            std::string world = findField(lpObj, "world");
            std::string gemsStr = findField(lpObj, "gems");
            std::string country = findField(lpObj, "country");
            std::string posX = findField(lpObj, "pos_x");
            std::string posY = findField(lpObj, "pos_y");
            std::string netidStr = findField(lpObj, "netid");
            std::string useridStr = findField(lpObj, "userid");
            std::string tileX = findField(lpObj, "tile_x");
            std::string tileY = findField(lpObj, "tile_y");

            if (!name.empty() && name != "null") {
                gs.localPlayer.name = name;
                if (!world.empty() && world != "null") gs.localPlayer.world = world;
                if (!country.empty() && country != "null") gs.localPlayer.country = country;
                if (!gemsStr.empty()) gs.localPlayer.gems = atoi(gemsStr.c_str());
                if (!posX.empty()) gs.localPlayer.pos_x = (float)atof(posX.c_str());
                if (!posY.empty()) gs.localPlayer.pos_y = (float)atof(posY.c_str());
                if (!netidStr.empty()) gs.localPlayer.netid = atoi(netidStr.c_str());
                if (!useridStr.empty()) gs.localPlayer.userid = atoi(useridStr.c_str());
                if (!tileX.empty()) gs.localPlayer.tile_x = atoi(tileX.c_str());
                if (!tileY.empty()) gs.localPlayer.tile_y = atoi(tileY.c_str());
                consoleLog("[BRIDGE] localPlayer: name=" + gs.localPlayer.name +
                    " gems=" + std::to_string(gs.localPlayer.gems) +
                    " world=" + gs.localPlayer.world +
                    " netid=" + std::to_string(gs.localPlayer.netid));
            }

            // Parse players array
            std::string playersArr = extractArray("players");
            if (!playersArr.empty()) {
                gs.players.clear();
                // Split objects in the array: find {...} groups
                size_t i = 0;
                while (i < playersArr.size()) {
                    if (playersArr[i] == '{') {
                        int depth = 0;
                        size_t start = i;
                        for (; i < playersArr.size(); i++) {
                            if (playersArr[i] == '{') depth++;
                            else if (playersArr[i] == '}') { depth--; if (depth == 0) break; }
                        }
                        if (i >= playersArr.size()) break;
                        std::string pobj = playersArr.substr(start, i - start + 1);
                        PlayerData pd;
                        pd.name = findField(pobj, "name");
                        pd.world = findField(pobj, "world");
                        std::string pnetid = findField(pobj, "netid");
                        std::string puserid = findField(pobj, "userid");
                        std::string pgems = findField(pobj, "gems");
                        std::string ppx = findField(pobj, "pos_x");
                        std::string ppy = findField(pobj, "pos_y");
                        std::string ptx = findField(pobj, "tile_x");
                        std::string pty = findField(pobj, "tile_y");
                        if (!pnetid.empty()) pd.netid = atoi(pnetid.c_str());
                        if (!puserid.empty()) pd.userid = atoi(puserid.c_str());
                        if (!pgems.empty()) pd.gems = atoi(pgems.c_str());
                        if (!ppx.empty()) pd.pos_x = (float)atof(ppx.c_str());
                        if (!ppy.empty()) pd.pos_y = (float)atof(ppy.c_str());
                        if (!ptx.empty()) pd.tile_x = atoi(ptx.c_str());
                        if (!pty.empty()) pd.tile_y = atoi(pty.c_str());
                        if (!pd.name.empty())                     gs.players.push_back(pd);
                        i++;
                    } else i++;
                }
                consoleLog("[BRIDGE] players: " + std::to_string(gs.players.size()));
            }
            std::string invArr = extractArray("inventory");
            if (!invArr.empty()) {
                gs.inventory.clear();
                size_t i = 0;
                while (i < invArr.size()) {
                    if (invArr[i] == '{') {
                        int depth = 0;
                        size_t start = i;
                        for (; i < invArr.size(); i++) {
                            if (invArr[i] == '{') depth++;
                            else if (invArr[i] == '}') { depth--; if (depth == 0) break; }
                        }
                        if (i >= invArr.size()) break;
                        std::string iobj = invArr.substr(start, i - start + 1);
                        InventoryItem it;
                        std::string iid = findField(iobj, "id");
                        std::string icount = findField(iobj, "count");
                        if (!iid.empty()) it.id = atoi(iid.c_str());
                        if (!icount.empty()) it.count = atoi(icount.c_str());
                        if (it.id > 0) gs.inventory.push_back(it);
                        i++;
                    } else i++;
                }
                consoleLog("[BRIDGE] inventory: " + std::to_string(gs.inventory.size()) + " items");
            }

            // Parse objects array
            std::string objArr = extractArray("objects");
            if (!objArr.empty()) {
                gs.objects.clear();
                size_t i = 0;
                while (i < objArr.size()) {
                    if (objArr[i] == '{') {
                        int depth = 0;
                        size_t start = i;
                        for (; i < objArr.size(); i++) {
                            if (objArr[i] == '{') depth++;
                            else if (objArr[i] == '}') { depth--; if (depth == 0) break; }
                        }
                        if (i >= objArr.size()) break;
                        std::string oobj = objArr.substr(start, i - start + 1);
                        WorldObject wo;
                        std::string oid = findField(oobj, "id");
                        std::string ooid = findField(oobj, "oid");
                        std::string opx = findField(oobj, "pos_x");
                        std::string opy = findField(oobj, "pos_y");
                        std::string ocount = findField(oobj, "count");
                        if (!oid.empty()) wo.id = atoi(oid.c_str());
                        if (!ooid.empty()) wo.oid = atoi(ooid.c_str());
                        if (!opx.empty()) wo.pos_x = (float)atof(opx.c_str());
                        if (!opy.empty()) wo.pos_y = (float)atof(opy.c_str());
                        if (!ocount.empty()) wo.count = atoi(ocount.c_str());
                        gs.objects.push_back(wo);
                        i++;
                    } else i++;
                }
                consoleLog("[BRIDGE] objects: " + std::to_string(gs.objects.size()));
            }

            // Ping
            std::string pingStr = findField(json, "ping");
            if (!pingStr.empty()) gs.ping_ms = atoi(pingStr.c_str());
            Sleep(3000);
        }
        return 0;
    }, nullptr, 0, nullptr);
    consoleLog("[INFO] Bridge reader started (reads GrowPai bridge.json)");

    auto t0 = std::chrono::steady_clock::now();

    while (true) {
        Sleep(100);
        auto now = std::chrono::steady_clock::now();
        g_currentTime = std::chrono::duration<float>(now - t0).count();
    }

    epshook::RemoveAll();
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
