#include "scanner.h"
#include "hook.h"
#include <vector>
#include <string>
#include <cstring>
#include <cstdio>

extern void debugLog(const std::string& msg);
extern void consoleLog(const std::string& msg);

namespace scanner {

    static bool g_Installed = false;

    uintptr_t fn_ProcessTankUpdatePacket = 0;
    uintptr_t fn_SendPacket = 0;
    uintptr_t fn_GetGameLogic = 0;
    uintptr_t g_GameBase = 0;
    size_t g_GameImageSize = 0;

    bool HooksInstalled() { return g_Installed; }

    static FILE* g_scanLogFile = nullptr;

    static void Log(const char* msg) {
        debugLog(std::string("[SCAN] ") + msg);
        consoleLog(std::string("[SCAN] ") + msg);
        if (!g_scanLogFile) {
            CreateDirectoryA("C:\\Users\\LENOVO\\Documents\\groetopia\\cv dl script\\coems_executor\\package-scanner-output\\scan log", nullptr);
            g_scanLogFile = fopen("C:\\Users\\LENOVO\\Documents\\groetopia\\cv dl script\\coems_executor\\package-scanner-output\\scan log\\scan.txt", "w");
        }
        if (g_scanLogFile) {
            fprintf(g_scanLogFile, "%s\n", msg);
            fflush(g_scanLogFile);
        }
    }

    static void LogHex(const char* label, uintptr_t addr, int len) {
        std::string hex;
        BYTE* p = (BYTE*)addr;
        for (int i = 0; i < len && i < 64; i++) {
            char buf[4];
            snprintf(buf, sizeof(buf), "%02X ", p[i]);
            hex += buf;
        }
        Log((std::string(label) + ": " + hex).c_str());
    }

    static bool IsExecutable(DWORD prot) {
        return (prot & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
    }

    static bool IsReadable(DWORD prot) {
        return (prot & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
            PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)) != 0;
    }

    struct MemRegion { uintptr_t start; size_t size; DWORD protect; };

    static bool FindPatternInRegion(uintptr_t start, size_t size, const char* pattern, uintptr_t& outAddr) {
        std::vector<int> bytes;
        const char* p = pattern;
        while (*p) {
            if (*p == ' ') { p++; continue; }
            if (*p == '?') {
                bytes.push_back(-1);
                p++;
                if (*p == '?') p++;
            } else {
                bytes.push_back((int)strtoul(p, (char**)&p, 16));
            }
        }
        if (bytes.empty()) return false;

        uintptr_t end = start + size - bytes.size();
        for (uintptr_t addr = start; addr <= end; addr++) {
            bool match = true;
            for (size_t j = 0; j < bytes.size(); j++) {
                if (bytes[j] != -1 && *(BYTE*)(addr + j) != (BYTE)bytes[j]) {
                    match = false;
                    break;
                }
            }
            if (match) { outAddr = addr; return true; }
        }
        return false;
    }

    static uintptr_t FindFuncStart(uintptr_t addr) {
        for (uintptr_t a = addr; a > addr - 0x5000 && a > 0x10000; a--) {
            BYTE* p = (BYTE*)a;
            if (p[0] == 0xC3 || p[0] == 0xCC) return a + 1;
            if (p[0] == 0x55) return a;
            if (p[0] == 0x48 && p[1] == 0x83 && p[2] == 0xEC) return a;
            if (p[0] == 0x48 && p[1] == 0x81 && p[2] == 0xEC) return a;
            if (p[0] == 0x40 && p[1] == 0x53 && p[2] == 0x48 && p[3] == 0x83) return a;
            if (p[0] == 0x48 && p[1] == 0x89 && (p[2] & 0x38) == 0x18) return a;
        }
        return addr;
    }

    static bool LooksLikeFuncStart(uintptr_t addr) {
        BYTE* p = (BYTE*)addr;
        if (p[0] == 0x48 && p[1] == 0x8B && p[2] == 0xC4) return true;
        if (p[0] == 0x48 && p[1] == 0x89 && (p[2] & 0x38) == 0x18) return true;
        if (p[0] == 0x40 && p[1] == 0x53 && p[2] == 0x48) return true;
        if (p[0] == 0x48 && p[1] == 0x83 && p[2] == 0xEC) return true;
        if (p[0] == 0x48 && p[1] == 0x81 && p[2] == 0xEC) return true;
        if (p[0] == 0x55 && p[1] == 0x48 && p[2] == 0x89) return true;
        if (p[0] == 0x53) return true;
        return false;
    }

    bool Install(HMODULE gameModule) {
        if (g_Installed) return true;
        if (!gameModule) return false;

        uintptr_t base = (uintptr_t)gameModule;
        g_GameBase = base;

        PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)gameModule;
        PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)((BYTE*)gameModule + dos->e_lfanew);
        g_GameImageSize = nt->OptionalHeader.SizeOfImage;

        char buf[256];
        snprintf(buf, sizeof(buf), "Base: 0x%llX, Image: 0x%llX", (unsigned long long)base, (unsigned long long)g_GameImageSize);
        Log(buf);

        std::vector<MemRegion> allExecRegions;
        std::vector<MemRegion> allReadRegions;

        uintptr_t addr = 0;
        while (addr < 0x7FFFFFFFFFFFFFFF) {
            MEMORY_BASIC_INFORMATION mbi;
            memset(&mbi, 0, sizeof(mbi));
            if (!VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi))) break;
            if (mbi.State == MEM_COMMIT && mbi.RegionSize > 0 && mbi.RegionSize < 0x10000000) {
                if (IsExecutable(mbi.Protect))
                    allExecRegions.push_back({ (uintptr_t)mbi.BaseAddress, mbi.RegionSize, mbi.Protect });
                if (IsReadable(mbi.Protect) && !IsExecutable(mbi.Protect))
                    allReadRegions.push_back({ (uintptr_t)mbi.BaseAddress, mbi.RegionSize, mbi.Protect });
            }
            addr = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
            if (addr <= (uintptr_t)mbi.BaseAddress) break;
        }

        snprintf(buf, sizeof(buf), "Exec regions: %d, Read regions: %d", (int)allExecRegions.size(), (int)allReadRegions.size());
        Log(buf);

        for (auto& r : allExecRegions) {
            snprintf(buf, sizeof(buf), "  Exec: 0x%llX size=0x%llX prot=0x%X",
                (unsigned long long)r.start, (unsigned long long)r.size, r.protect);
            Log(buf);
        }

        Log("=== PATTERN SCAN (ALL MEMORY) ===");

        // Find PTUP by looking for ALL matches and picking the LAST one
        {
            uintptr_t lastFound = 0;
            for (auto& r : allExecRegions) {
                uintptr_t found = 0;
                if (FindPatternInRegion(r.start, r.size, "83 78 04 71 75", found)) {
                    uintptr_t funcStart = FindFuncStart(found);
                    char buf2[256];
                    snprintf(buf2, sizeof(buf2), "PTUP candidate: 0x%llX (region 0x%llX)",
                        (unsigned long long)funcStart, (unsigned long long)r.start);
                    Log(buf2);
                    lastFound = funcStart;
                }
            }
            if (lastFound) {
                fn_ProcessTankUpdatePacket = lastFound;
                snprintf(buf, sizeof(buf), "ProcessTankUpdatePacket: 0x%llX", (unsigned long long)fn_ProcessTankUpdatePacket);
                Log(buf);
                LogHex("  PTUP", fn_ProcessTankUpdatePacket, 32);
            }
        }

        // String xref: find "OnSpawn" in read regions, trace LEA xrefs in exec regions
        {
            Log("String xref scan for PTUP...");
            const char* targetStr = "OnSpawn";
            size_t targetLen = strlen(targetStr);
            for (auto& r : allReadRegions) {
                for (uintptr_t sa = r.start; sa <= r.start + r.size - targetLen; sa++) {
                    if (memcmp((void*)sa, targetStr, targetLen) == 0) {
                        char buf2[256];
                        snprintf(buf2, sizeof(buf2), "Found '%s' at 0x%llX", targetStr, (unsigned long long)sa);
                        Log(buf2);
                        // Search exec regions for LEA instructions referencing this string
                        uintptr_t strAddr = sa;
                        for (auto& er : allExecRegions) {
                            uintptr_t end = er.start + er.size - 7;
                            for (uintptr_t ea = er.start; ea <= end; ea++) {
                                BYTE* pp = (BYTE*)ea;
                                // LEA reg, [rip+disp32] = 48 8D 0D/05/15/1D/25/2D/35/3D xx xx xx xx
                                if (pp[0] == 0x48 && pp[1] == 0x8D && (pp[2] & 0xC7) == 0x05) {
                                    int32_t disp = *(int32_t*)(pp + 3);
                                    uintptr_t resolved = ea + 7 + disp;
                                    if (resolved == strAddr) {
                                        uintptr_t funcStart = FindFuncStart(ea);
                                        snprintf(buf2, sizeof(buf2), "LEA xref to '%s' at 0x%llX (func 0x%llX)",
                                            targetStr, (unsigned long long)ea, (unsigned long long)funcStart);
                                        Log(buf2);
                                        // Use string xref result instead of pattern result
                                        fn_ProcessTankUpdatePacket = funcStart;
                                        break;
                                    }
                                }
                            }
                            if (fn_ProcessTankUpdatePacket) break;
                        }
                        if (fn_ProcessTankUpdatePacket) break;
                    }
                }
                if (fn_ProcessTankUpdatePacket) break;
            }
            if (fn_ProcessTankUpdatePacket) {
                snprintf(buf, sizeof(buf), "PTUP (string xref): 0x%llX", (unsigned long long)fn_ProcessTankUpdatePacket);
                Log(buf);
                LogHex("  PTUP", fn_ProcessTankUpdatePacket, 32);
            }
        }

        {
            const char* spPatterns[] = {
                "02 00 00 00 E8 ?? ?? ?? ?? 90 48 8D 4C 24 50",
                "02 00 00 00 E8 ?? ?? ?? ?? 90 48",
                "02 00 00 00 E8 ?? ?? ?? ?? 8B",
            };

            for (auto& pat : spPatterns) {
                if (fn_SendPacket) break;
                uintptr_t found = 0;
                for (auto& r : allExecRegions) {
                    if (FindPatternInRegion(r.start, r.size, pat, found)) {
                        uintptr_t callAddr = found + 4;
                        int32_t disp = *(int32_t*)(callAddr + 1);
                        uintptr_t target = callAddr + 5 + disp;
                        if (LooksLikeFuncStart(target)) {
                            fn_SendPacket = target;
                            snprintf(buf, sizeof(buf), "SendPacket: 0x%llX", (unsigned long long)fn_SendPacket);
                            Log(buf);
                            LogHex("  SP", fn_SendPacket, 32);
                            break;
                        }
                    }
                }
            }

            if (!fn_SendPacket) {
                for (auto& r : allExecRegions) {
                    uintptr_t searchEnd = r.start + r.size - 9;
                    for (uintptr_t sa = r.start; sa <= searchEnd; sa++) {
                        BYTE* pp = (BYTE*)sa;
                        if (pp[0] == 0x02 && pp[1] == 0x00 && pp[2] == 0x00 && pp[3] == 0x00 && pp[4] == 0xE8) {
                            int32_t disp = *(int32_t*)(pp + 5);
                            uintptr_t callAddr = sa + 4;
                            uintptr_t target = callAddr + 5 + disp;
                            if (LooksLikeFuncStart(target) && target != fn_ProcessTankUpdatePacket) {
                                fn_SendPacket = target;
                                snprintf(buf, sizeof(buf), "SendPacket (generic): 0x%llX", (unsigned long long)fn_SendPacket);
                                Log(buf);
                                LogHex("  SP", fn_SendPacket, 32);
                                break;
                            }
                        }
                    }
                    if (fn_SendPacket) break;
                }
            }

            if (!fn_SendPacket) Log("SendPacket: NOT RESOLVED");
        }

        {
            const char* glPatterns[] = {
                "E8 ?? ?? ?? ?? 48 8D ? ? ? ? ? E8 ?? ?? ?? ?? 48 8B",
                "48 8B C4 4C 89 48 20 4C 89 40 18 48 89 50 10 53 56 57 41 56 48 83 EC 38 4D 8B F1 49 8B D8 48 8B",
            };

            for (auto& pat : glPatterns) {
                if (fn_GetGameLogic) break;
                uintptr_t found = 0;
                for (auto& r : allExecRegions) {
                    if (FindPatternInRegion(r.start, r.size, pat, found)) {
                        if (pat[0] == 'E') {
                            int32_t disp = *(int32_t*)(found + 1);
                            uintptr_t target = found + 5 + disp;
                            if (LooksLikeFuncStart(target)) {
                                fn_GetGameLogic = target;
                            }
                        } else {
                            fn_GetGameLogic = found;
                        }
                        if (fn_GetGameLogic) {
                            snprintf(buf, sizeof(buf), "GetGameLogic: 0x%llX", (unsigned long long)fn_GetGameLogic);
                            Log(buf);
                            LogHex("  GL", fn_GetGameLogic, 32);
                            break;
                        }
                    }
                }
            }

            if (!fn_GetGameLogic) Log("GetGameLogic: NOT RESOLVED");
        }

        g_Installed = true;
        Log("=== RESOLVED FUNCTIONS ===");
        snprintf(buf, sizeof(buf), "  PTUP = 0x%llX", (unsigned long long)fn_ProcessTankUpdatePacket);
        Log(buf);
        snprintf(buf, sizeof(buf), "  SendPacket = 0x%llX", (unsigned long long)fn_SendPacket);
        Log(buf);
        snprintf(buf, sizeof(buf), "  GetGameLogic = 0x%llX", (unsigned long long)fn_GetGameLogic);
        Log(buf);
        Log("=== SCAN COMPLETE ===");
        if (g_scanLogFile) { fclose(g_scanLogFile); g_scanLogFile = nullptr; }

        return true;
    }

    // SEH-safe wrappers (no C++ objects - C only)
    extern "C" {
        static void* g_pfnGetGameLogic = nullptr;
        static void* g_pfnSendPacket = nullptr;
        static char g_sendStrBuf[32] = {};
        static size_t g_sendLen = 0;
        static int g_sendType = 0;

        static void* __cdecl CallGL_Internal() {
            typedef void*(__cdecl* fn_t)();
            return ((fn_t)g_pfnGetGameLogic)();
        }

        static void __fastcall CallSP_Internal() {
            typedef void(__fastcall* fn_t)(int, void*, void*);
            ((fn_t)g_pfnSendPacket)(g_sendType, g_sendStrBuf, nullptr);
        }

        static void* DoCallGetGameLogic() {
            void* result = nullptr;
            __try {
                result = CallGL_Internal();
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                return nullptr;
            }
            return result;
        }

        static void DoCallSendPacket() {
            __try {
                CallSP_Internal();
            } __except(EXCEPTION_EXECUTE_HANDLER) {
            }
        }
    }

    void* CallGetGameLogic() {
        if (!fn_GetGameLogic) return nullptr;
        g_pfnGetGameLogic = (void*)fn_GetGameLogic;
        void* result = DoCallGetGameLogic();
        if (!result) debugLog("[SCAN] GetGameLogic call crashed");
        return result;
    }

    void CallSendPacket(int type, const char* text) {
        if (!fn_SendPacket || !text) return;

        memset(g_sendStrBuf, 0, 32);
        g_sendLen = strlen(text);
        if (g_sendLen < 16) {
            memcpy(g_sendStrBuf, text, g_sendLen);
            g_sendStrBuf[g_sendLen] = '\0';
            *(size_t*)(g_sendStrBuf + 16) = g_sendLen;
            *(size_t*)(g_sendStrBuf + 24) = 15;
        } else {
            char* heap = _strdup(text);
            *(char**)g_sendStrBuf = heap;
            *(size_t*)(g_sendStrBuf + 16) = g_sendLen;
            *(size_t*)(g_sendStrBuf + 24) = g_sendLen;
        }

        g_pfnSendPacket = (void*)fn_SendPacket;
        g_sendType = type;

        DoCallSendPacket();

        if (g_sendLen >= 16 && *(char**)g_sendStrBuf) {
            free(*(char**)g_sendStrBuf);
        }
        memset(g_sendStrBuf, 0, 32);
    }
}
