#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <stdio.h>
#include <string>
#include <vector>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "psapi.lib")

static void Log(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vprintf(fmt, a);
    va_end(a);
    printf("\n");
    fflush(stdout);
}

static void Fail(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    printf("[FAIL] ");
    vprintf(fmt, a);
    va_end(a);
    printf(" (error %lu)\n", GetLastError());
    fflush(stdout);
}

// ── 1. SeDebugPrivilege ────────────────────────────────────────────────
static bool EnableDebugPrivilege() {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        Fail("OpenProcessToken");
        return false;
    }
    TOKEN_PRIVILEGES tp{};
    if (!LookupPrivilegeValueA(nullptr, SE_DEBUG_NAME, &tp.Privileges[0].Luid)) {
        Fail("LookupPrivilegeValue");
        CloseHandle(hToken);
        return false;
    }
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    SetLastError(0);
    BOOL ok = AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    DWORD err = GetLastError();
    CloseHandle(hToken);
    if (!ok || err != ERROR_SUCCESS) {
        Fail("AdjustTokenPrivileges (not elevated?)");
        return false;
    }
    Log("[+] SeDebugPrivilege enabled");
    return true;
}

// ── 2. Process / window helpers ───────────────────────────────────────
static std::vector<DWORD> FindAllPidsByName(const wchar_t* name) {
    std::vector<DWORD> pids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return pids;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name) == 0) pids.push_back(pe.th32ProcessID);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pids;
}

static BOOL CALLBACK EnumWindowsProc(HWND hw, LPARAM lParam) {
    DWORD pid = (DWORD)lParam;
    DWORD wpid = 0;
    GetWindowThreadProcessId(hw, &wpid);
    if (wpid == pid && IsWindowVisible(hw) && GetWindowTextLengthA(hw) > 0)
        return FALSE; // found
    return TRUE;
}

static bool ProcessHasWindow(DWORD pid) {
    return EnumWindows(EnumWindowsProc, (LPARAM)pid) == 0;
}

static bool WaitForProcessWindow(DWORD pid, int timeoutSec) {
    for (int i = 0; i < timeoutSec * 10; i++) {
        if (ProcessHasWindow(pid)) return true;
        Sleep(100);
    }
    return false;
}

// ── 3. Disable BlockDynamicCode (Themida ACG) ─────────────────────────
typedef BOOL(WINAPI* SetMitigationFn)(DWORD, PVOID, SIZE_T);
typedef BOOL(WINAPI* GetMitigationFn)(DWORD, PVOID, SIZE_T);

static bool DisableBlockDynamicCode(HANDLE hProc, DWORD pid) {
    HMODULE hK32 = GetModuleHandleA("kernel32.dll");
    auto SetPol = (SetMitigationFn)GetProcAddress(hK32, "SetProcessMitigationPolicy");
    auto GetPol = (GetMitigationFn)GetProcAddress(hK32, "GetProcessMitigationPolicy");
    if (!SetPol || !GetPol) { Log("[-] mitigation APIs missing"); return false; }

    // Check current state (works cross-process)
    PROCESS_MITIGATION_DYNAMIC_CODE_POLICY cur{};
    if (GetPol && GetProcessMitigationPolicy(hProc, ProcessDynamicCodePolicy, &cur, sizeof(cur))) {
        if (!cur.ProhibitDynamicCode) {
            Log("[+] BlockDynamicCode already off");
            return true;
        }
        Log("[*] BlockDynamicCode is ON — attempting remote disable...");
    }

    // Run SetProcessMitigationPolicy(ProcessDynamicCodePolicy, {0}, 4) inside target.
    // Kernel32 is at the same base across processes on one boot.
    HMODULE remoteK32 = nullptr;
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snap != INVALID_HANDLE_VALUE) {
            MODULEENTRY32W me{};
            me.dwSize = sizeof(me);
            if (Module32FirstW(snap, &me)) {
                do {
                    if (_wcsicmp(me.szModule, L"kernel32.dll") == 0) {
                        remoteK32 = (HMODULE)me.modBaseAddr;
                        break;
                    }
                } while (Module32NextW(snap, &me));
            }
            CloseHandle(snap);
        }
    }
    if (!remoteK32) { Log("[-] remote kernel32 not found"); return false; }

    uintptr_t fnOff = (uintptr_t)SetPol - (uintptr_t)hK32;
    auto remoteSetPol = (LPVOID)((uintptr_t)remoteK32 + fnOff);

    // shellcode: sub rsp,28h; mov ecx,2; lea rdx,[rsp+20h]; mov r8d,4;
    //            xor eax,eax; mov [rsp+20h],eax; call remoteSetPol; add rsp,28h; ret
    // policy at [rsp+20h] zeroed = ProhibitDynamicCode off
    BYTE sc[64];
    int n = 0;
    sc[n++] = 0x48; sc[n++] = 0x83; sc[n++] = 0xEC; sc[n++] = 0x28;       // sub rsp,28h
    sc[n++] = 0xB9; sc[n++] = 0x02; sc[n++] = 0x00; sc[n++] = 0x00; sc[n++] = 0x00; // mov ecx,2
    sc[n++] = 0x48; sc[n++] = 0x8D; sc[n++] = 0x54; sc[n++] = 0x24; sc[n++] = 0x20; // lea rdx,[rsp+20h]
    sc[n++] = 0x41; sc[n++] = 0xB8; sc[n++] = 0x04; sc[n++] = 0x00; sc[n++] = 0x00; sc[n++] = 0x00; // mov r8d,4
    sc[n++] = 0x31; sc[n++] = 0xC0;                                         // xor eax,eax
    sc[n++] = 0x48; sc[n++] = 0x89; sc[n++] = 0x44; sc[n++] = 0x24; sc[n++] = 0x20; // mov [rsp+20h],rax
    DWORD_PTR callTarget = (DWORD_PTR)remoteSetPol;
    DWORD_PTR nextInsn = 0; // filled after we know size — E8 rel32
    // We'll emit call after computing offset; placeholder then patch.
    int callPos = n;
    sc[n++] = 0xE8;
    sc[n++] = 0x00; sc[n++] = 0x00; sc[n++] = 0x00; sc[n++] = 0x00;
    sc[n++] = 0x48; sc[n++] = 0x83; sc[n++] = 0xC4; sc[n++] = 0x28;       // add rsp,28h
    sc[n++] = 0xC3;                                                       // ret
    nextInsn = (DWORD_PTR)callPos + 5; // relative from sc start — patch below
    {
        // rel32 = target - (addr_of_next_insn); we don't know remote addr yet, patch after alloc
    }

    LPVOID remoteSc = VirtualAllocEx(hProc, nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteSc) { Fail("VirtualAllocEx (mitigation stub)"); return false; }

    // patch call rel32: rel = remoteSetPol - (remoteSc + callPos + 5)
    {
        intptr_t rel = (intptr_t)remoteSetPol - ((intptr_t)remoteSc + callPos + 5);
        memcpy(&sc[callPos + 1], &rel, 4);
    }

    if (!WriteProcessMemory(hProc, remoteSc, sc, n, nullptr)) {
        Fail("WriteProcessMemory (mitigation stub)");
        VirtualFreeEx(hProc, remoteSc, 0, MEM_RELEASE);
        return false;
    }
    DWORD oldProt = 0;
    if (!VirtualProtectEx(hProc, remoteSc, n, PAGE_EXECUTE_READ, &oldProt)) {
        Fail("VirtualProtectEx (mitigation stub) — ACG may already block this");
        VirtualFreeEx(hProc, remoteSc, 0, MEM_RELEASE);
        return false;
    }
    HANDLE hTh = CreateRemoteThread(hProc, nullptr, 0, (LPTHREAD_START_ROUTINE)remoteSc, nullptr, 0, nullptr);
    if (!hTh) {
        Fail("CreateRemoteThread (mitigation stub)");
        VirtualFreeEx(hProc, remoteSc, 0, MEM_RELEASE);
        return false;
    }
    WaitForSingleObject(hTh, 5000);
    CloseHandle(hTh);
    VirtualFreeEx(hProc, remoteSc, 0, MEM_RELEASE);

    PROCESS_MITIGATION_DYNAMIC_CODE_POLICY after{};
    if (GetProcessMitigationPolicy(hProc, ProcessDynamicCodePolicy, &after, sizeof(after))) {
        if (!after.ProhibitDynamicCode) {
            Log("[+] BlockDynamicCode disabled");
            return true;
        }
        Log("[-] BlockDynamicCode still ON (locked by game) — continuing anyway");
        return false;
    }
    Log("[-] could not re-check mitigation policy — continuing");
    return false;
}

// ── 4. Restore NtProtectVirtualMemory original bytes ──────────────────
static bool RvaToFileOffset(BYTE* file, DWORD rva, DWORD* outOff) {
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)file;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(file + dos->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (rva >= sec[i].VirtualAddress && rva < sec[i].VirtualAddress + sec[i].Misc.VirtualSize) {
            *outOff = (rva - sec[i].VirtualAddress) + sec[i].PointerToRawData;
            return true;
        }
    }
    return false;
}

static bool RestoreNtProtectVirtualMemory(HANDLE hProc, DWORD pid) {
    HMODULE hLocalNtdll = GetModuleHandleA("ntdll.dll");
    auto pLocal = (BYTE*)GetProcAddress(hLocalNtdll, "NtProtectVirtualMemory");
    if (!pLocal) { Log("[-] NtProtectVirtualMemory not found locally"); return false; }
    DWORD rva = (DWORD)(pLocal - (BYTE*)hLocalNtdll);

    // Remote ntdll base
    uintptr_t remoteNtdll = 0;
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snap == INVALID_HANDLE_VALUE) { Fail("SNAPMODULE"); return false; }
        MODULEENTRY32W me{};
        me.dwSize = sizeof(me);
        if (Module32FirstW(snap, &me)) {
            do {
                if (_wcsicmp(me.szModule, L"ntdll.dll") == 0) {
                    remoteNtdll = (uintptr_t)me.modBaseAddr;
                    break;
                }
            } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
    }
    if (!remoteNtdll) { Log("[-] remote ntdll not found"); return false; }

    // Read clean bytes from on-disk ntdll
    wchar_t ntdllPath[MAX_PATH]{};
    GetSystemDirectoryW(ntdllPath, MAX_PATH);
    wcscat_s(ntdllPath, L"\\ntdll.dll");

    HANDLE hf = CreateFileW(ntdllPath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hf == INVALID_HANDLE_VALUE) { Fail("open disk ntdll"); return false; }
    DWORD fSize = GetFileSize(hf, nullptr);
    std::vector<BYTE> file(fSize);
    DWORD rd = 0;
    ReadFile(hf, file.data(), fSize, &rd, nullptr);
    CloseHandle(hf);

    DWORD fileOff = 0;
    if (!RvaToFileOffset(file.data(), rva, &fileOff)) { Log("[-] RVA→offset failed"); return false; }

    BYTE orig[16];
    memcpy(orig, file.data() + fileOff, sizeof(orig));

    // Compare with what's currently in target
    BYTE current[16]{};
    ReadProcessMemory(hProc, (LPCVOID)(remoteNtdll + rva), current, sizeof(current), nullptr);
    if (memcmp(orig, current, sizeof(orig)) == 0) {
        Log("[+] NtProtectVirtualMemory already original");
        return true;
    }

    DWORD oldProt = 0;
    if (!VirtualProtectEx(hProc, (LPVOID)(remoteNtdll + rva), sizeof(orig), PAGE_EXECUTE_READWRITE, &oldProt)) {
        Fail("VirtualProtectEx on remote ntdll");
        return false;
    }
    SIZE_T written = 0;
    if (!WriteProcessMemory(hProc, (LPVOID)(remoteNtdll + rva), orig, sizeof(orig), &written)) {
        Fail("WriteProcessMemory on remote ntdll");
        VirtualProtectEx(hProc, (LPVOID)(remoteNtdll + rva), sizeof(orig), oldProt, &oldProt);
        return false;
    }
    DWORD tmp = 0;
    VirtualProtectEx(hProc, (LPVOID)(remoteNtdll + rva), sizeof(orig), oldProt, &tmp);
    Log("[+] NtProtectVirtualMemory restored to original bytes");
    return true;
}

// ── 5. DLL injection (LoadLibraryA) ───────────────────────────────────
static bool InjectDll(HANDLE hProc, const char* dllPath) {
    size_t len = strlen(dllPath) + 1;
    LPVOID remoteMem = VirtualAllocEx(hProc, nullptr, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem) { Fail("VirtualAllocEx (dll path)"); return false; }

    if (!WriteProcessMemory(hProc, remoteMem, dllPath, len, nullptr)) {
        Fail("WriteProcessMemory (dll path)");
        VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        return false;
    }

    auto loadLib = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
    HANDLE hTh = CreateRemoteThread(hProc, nullptr, 0, loadLib, remoteMem, 0, nullptr);
    if (!hTh) {
        Fail("CreateRemoteThread (LoadLibraryA)");
        VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);
        return false;
    }
    WaitForSingleObject(hTh, 15000);
    DWORD code = 0;
    GetExitCodeThread(hTh, &code);
    CloseHandle(hTh);
    VirtualFreeEx(hProc, remoteMem, 0, MEM_RELEASE);

    if (!code) { Log("[-] LoadLibraryA returned NULL — path wrong or dependency missing"); return false; }
    Log("[+] DLL injected (module base 0x%lX)", code);
    return true;
}

// ── main ──────────────────────────────────────────────────────────────
static HANDLE OpenTarget(DWORD pid) {
    return OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION |
        PROCESS_VM_WRITE | PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
        FALSE, pid);
}

static bool IsAlreadyInjected(DWORD pid, const char* dllName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me)) {
        do {
            char modA[MAX_PATH]{};
            WideCharToMultiByte(CP_ACP, 0, me.szModule, -1, modA, MAX_PATH, nullptr, nullptr);
            if (_stricmp(modA, dllName) == 0) { found = true; break; }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

static bool LaunchGrowtopia(HANDLE* outProc, DWORD* outPid) {
    wchar_t exePath[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, 0x001C /*CSIDL_LOCAL_APPDATA*/, nullptr, 0, exePath)))
        wcscat_s(exePath, L"\\Growtopia\\Growtopia.exe");
    if (GetFileAttributesW(exePath) == INVALID_FILE_ATTRIBUTES) {
        Fail("Growtopia.exe not found at %ls", exePath);
        return false;
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    wchar_t cmd[MAX_PATH + 2]{};
    swprintf_s(cmd, L"\"%s\"", exePath);
    if (!CreateProcessW(exePath, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        Fail("CreateProcessW");
        return false;
    }
    CloseHandle(pi.hThread);
    *outProc = pi.hProcess;
    *outPid = pi.dwProcessId;
    Log("[+] Growtopia started (pid %lu)", pi.dwProcessId);
    return true;
}

static bool InjectOne(DWORD pid, const std::string& dllPath, const char* dllName) {
    printf("\n--- pid %lu ---\n", pid);

    if (IsAlreadyInjected(pid, dllName)) {
        Log("[+] pid %lu already has EpsHax — skipping", pid);
        return true;
    }

    HANDLE hProc = OpenTarget(pid);
    if (!hProc) { Fail("OpenProcess (pid %lu)", pid); return false; }

    // wait for main window (up to 90s)
    if (!ProcessHasWindow(pid)) {
        Log("[*] Waiting for Growtopia window (pid %lu)...", pid);
        if (!WaitForProcessWindow(pid, 90)) {
            Log("[-] pid %lu: no window after 90s — injecting anyway", pid);
        }
    }
    WaitForInputIdle(hProc, 10000);
    Sleep(1500); // let the game finish early init

    DisableBlockDynamicCode(hProc, pid);
    RestoreNtProtectVirtualMemory(hProc, pid);

    Log("[*] Injecting into pid %lu...", pid);
    bool ok = InjectDll(hProc, dllPath.c_str());
    CloseHandle(hProc);
    return ok;
}

int main(int argc, char** argv) {
    SetConsoleTitleA("EpsHax Loader");
    printf("============================\n");
    printf("   EpsHax Loader v3 (multi)\n");
    printf("============================\n\n");

    // Parse args:
    //   (none)          → inject all running; if none, launch 1
    //   -n N            → launch N new instances (keep existing, inject all)
    //   -n 0            → inject running only (do not launch)
    //   <dllpath>       → drag-drop DLL
    int launchCount = -1; // -1 = default behavior
    std::string dllPath;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            launchCount = atoi(argv[++i]);
        } else {
            dllPath = argv[i];
        }
    }
    if (dllPath.empty()) {
        char exePath[MAX_PATH]{};
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        std::string dir(exePath);
        size_t p = dir.find_last_of("\\/");
        if (p != std::string::npos) dir = dir.substr(0, p + 1);
        dllPath = dir + "EpsHax.dll";
    }
    if (GetFileAttributesA(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Fail("DLL not found: %s", dllPath.c_str());
        Log("Usage: EpsHaxLoader [-n instanceCount] [dllPath]");
        system("pause");
        return 1;
    }
    std::string dllName = dllPath.substr(dllPath.find_last_of("\\/") + 1);
    Log("[+] DLL: %s", dllPath.c_str());

    EnableDebugPrivilege();

    // Launch new instances
    if (launchCount > 0) {
        Log("[*] Launching %d new Growtopia instance(s)...", launchCount);
        std::vector<DWORD> fresh;
        for (int i = 0; i < launchCount; i++) {
            HANDLE hp = nullptr; DWORD pid = 0;
            if (LaunchGrowtopia(&hp, &pid)) {
                CloseHandle(hp);
                fresh.push_back(pid);
            }
        }
        if (fresh.empty()) {
            Fail("could not launch any instance");
            system("pause");
            return 1;
        }
        // wait a bit for processes to stabilize
        Sleep(2000);
    } else if (launchCount == -1) {
        // default: if no Growtopia running at all, launch one
        if (FindAllPidsByName(L"Growtopia.exe").empty()) {
            Log("[*] No Growtopia running — launching one...");
            HANDLE hp = nullptr; DWORD pid = 0;
            if (!LaunchGrowtopia(&hp, &pid)) { system("pause"); return 1; }
            CloseHandle(hp);
            Sleep(2000);
        }
    }
    // launchCount == 0 → inject only, never launch

    // Collect every Growtopia PID
    auto pids = FindAllPidsByName(L"Growtopia.exe");
    if (pids.empty()) {
        Log("[-] No Growtopia processes found");
        system("pause");
        return 1;
    }
    Log("[*] Found %zu Growtopia instance(s)", pids.size());

    int success = 0, failed = 0, skipped = 0;
    for (DWORD pid : pids) {
        if (IsAlreadyInjected(pid, dllName.c_str())) {
            printf("\n--- pid %lu ---\n", pid);
            Log("[+] already injected — skipping");
            skipped++;
            continue;
        }
        if (InjectOne(pid, dllPath, dllName.c_str())) success++;
        else failed++;
    }

    printf("\n============================\n");
    printf(" Done: %d injected, %d already had it, %d failed\n", success, skipped, failed);
    printf(" F1 toggles the menu in each instance\n");
    printf("============================\n");
    system("pause");
    return failed ? 1 : 0;
}
