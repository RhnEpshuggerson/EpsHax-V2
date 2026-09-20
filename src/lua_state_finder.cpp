#include "lua_state_finder.h"
#include "hook.h"
#include <vector>
#include <cstring>

extern void debugLog(const std::string& msg);
extern void consoleLog(const std::string& msg);
extern volatile LONG g_ScanningActive;

namespace lua_state_finder {

    static void* g_GameLuaState = nullptr;
    static void* g_GameGlobalState = nullptr;

    static bool IsReadable(uintptr_t addr, size_t len) {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (!VirtualQuery((void*)addr, &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (len > mbi.RegionSize) return false;
        DWORD bad = PAGE_NOACCESS | PAGE_GUARD;
        return (mbi.Protect & bad) == 0;
    }

    static bool IsValidPointer(uintptr_t ptr) {
        if (ptr < 0x10000 || ptr > 0x7FFFFFFFFFFF) return false;
        MEMORY_BASIC_INFORMATION mbi = {};
        if (!VirtualQuery((void*)ptr, &mbi, sizeof(mbi))) return false;
        return mbi.State == MEM_COMMIT;
    }

    // Pure-C SEH-safe reader (no C++ objects in scope)
    extern "C" {
        static uintptr_t s_readTop, s_readBase, s_readLG, s_readStack, s_readStackLast;
        static uint8_t s_readStatus;
        static uint32_t s_readGCheck;
        static int s_readOk;

        static void __cdecl DoReadLuaState() {
            s_readOk = 0;
            uintptr_t addr = s_readTop; // reuse as input
            s_readTop = *(uintptr_t*)(addr + 24);
            s_readBase = *(uintptr_t*)(addr + 32);
            s_readLG = *(uintptr_t*)(addr + 40);
            s_readStackLast = *(uintptr_t*)(addr + 48);
            s_readStack = *(uintptr_t*)(addr + 56);
            s_readStatus = *(uint8_t*)(addr + 8);
            s_readGCheck = *(uint32_t*)s_readLG;
            s_readOk = 1;
        }

        static int __cdecl TryReadLuaState(uintptr_t addr) {
            s_readTop = addr;
            s_readOk = 0;
            __try {
                DoReadLuaState();
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                s_readOk = 0;
            }
            return s_readOk;
        }

        static int __cdecl TryMemCmp(uintptr_t addr, const void* pattern, size_t len) {
            __try {
                return memcmp((void*)addr, pattern, len) == 0;
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                return 0;
            }
        }
    }

    static bool LooksLikeLuaState(uintptr_t addr) {
        if (!IsReadable(addr, 128)) return false;

        if (!TryReadLuaState(addr)) return false;

        if (s_readStatus > 10) return false;
        if (!IsValidPointer(s_readTop)) return false;
        if (!IsValidPointer(s_readBase)) return false;
        if (!IsValidPointer(s_readLG)) return false;
        if (!IsValidPointer(s_readStack)) return false;
        if (s_readStackLast && s_readStack >= s_readStackLast) return false;
        if (s_readTop < s_readBase) return false;
        if (s_readGCheck == 0) return false;

        return true;
    }

    bool Scan() {
        consoleLog("[LUA] Scanning for game Lua state...");

        InterlockedExchange(&g_ScanningActive, 1);

        uintptr_t gameBase = (uintptr_t)GetModuleHandleA(NULL);
        if (!gameBase) return false;

        PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)gameBase;
        PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)((BYTE*)gameBase + dos->e_lfanew);
        uintptr_t gameEnd = gameBase + nt->OptionalHeader.SizeOfImage;

        const char* luaStrings[] = {
            "Lua 5.3", "Lua 5.4", "lua53.dll", "lua54.dll",
            "luaopen_", "lua_pcall", "luaL_newstate",
            "_G", "lua_registryindex",
        };

        int stringCount = 0;
        for (const char* targetStr : luaStrings) {
            size_t targetLen = strlen(targetStr);
            for (uintptr_t addr = gameBase; addr < gameEnd - targetLen; addr++) {
                if (TryMemCmp(addr, targetStr, targetLen)) {
                    stringCount++;
                }
            }
        }

        char buf[256];
        snprintf(buf, sizeof(buf), "[LUA] Found %d Lua strings in game image", stringCount);
        consoleLog(buf);

        // Scan heap for lua_State candidates
        int candidates = 0;
        int found = 0;
        uintptr_t scanAddr = 0;
        while (scanAddr < 0x7FFFFFFFFFFFFFFF && candidates < 5000) {
            MEMORY_BASIC_INFORMATION mbi = {};
            if (!VirtualQuery((void*)scanAddr, &mbi, sizeof(mbi))) break;
            if (mbi.State == MEM_COMMIT && mbi.RegionSize >= 256 && mbi.RegionSize < 0x10000000) {
                DWORD prot = mbi.Protect;
                bool safe = (prot & (PAGE_NOACCESS | PAGE_GUARD)) == 0 &&
                            (prot & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                     PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
                if (safe) {
                    uintptr_t start = (uintptr_t)mbi.BaseAddress;
                    uintptr_t end = start + mbi.RegionSize - 64;
                    for (uintptr_t addr = start; addr <= end; addr += sizeof(void*)) {
                        candidates++;
                        if (LooksLikeLuaState(addr)) {
                            found++;
                            snprintf(buf, sizeof(buf),
                                "[LUA] Candidate at 0x%llX: top=0x%llX base=0x%llX l_G=0x%llX stack=0x%llX status=%d",
                                (unsigned long long)addr,
                                (unsigned long long)s_readTop,
                                (unsigned long long)s_readBase,
                                (unsigned long long)s_readLG,
                                (unsigned long long)s_readStack,
                                s_readStatus);
                            consoleLog(buf);

                            if (!g_GameLuaState) {
                                g_GameLuaState = (void*)addr;
                                g_GameGlobalState = (void*)s_readLG;
                                consoleLog("[LUA] Using first valid lua_State candidate");
                            }
                        }
                    }
                }
            }
            scanAddr = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
            if (scanAddr <= (uintptr_t)mbi.BaseAddress) break;
        }

        snprintf(buf, sizeof(buf), "[LUA] Scanned %d candidates, found %d lua_State", candidates, found);
        consoleLog(buf);

        if (g_GameLuaState) {
            snprintf(buf, sizeof(buf),
                "[LUA] Game lua_State at 0x%llX, global_State at 0x%llX",
                (unsigned long long)g_GameLuaState,
                (unsigned long long)g_GameGlobalState);
            consoleLog(buf);
            InterlockedExchange(&g_ScanningActive, 0);
            return true;
        }

        consoleLog("[LUA] No lua_State found");
        InterlockedExchange(&g_ScanningActive, 0);
        return false;
    }

    void* GetGameState() { return g_GameGlobalState; }
    void* GetLuaState() { return g_GameLuaState; }
}
