#include "native_hook.h"
#include <vector>
#include <map>
#include <cstring>
#include <string>

extern void consoleLog(const std::string& msg);
extern void debugLog(const std::string& msg);

static std::map<void*, HookEntry> g_hooks;

static void (*g_hookDispatchFn)(SavedRegs*) = nullptr;

static int DecodeModRM(uint8_t* code, int pos) {
    uint8_t modrm = code[pos++];
    uint8_t mod = modrm >> 6;
    uint8_t rm = modrm & 7;

    if (mod == 3) return pos;

    if (rm == 4 && mod != 3) {
        pos++;
        uint8_t sib = code[pos - 1];
        uint8_t base = sib & 7;
        if (base == 5 && mod == 0) pos += 4;
    }

    if (mod == 0 && rm == 5) pos += 4;
    else if (mod == 1) pos += 1;
    else if (mod == 2) pos += 4;

    return pos;
}

static int DecodeInstructionLength(uint8_t* code) {
    int pos = 0;
    bool hasRex = false;

    if (code[pos] >= 0x40 && code[pos] <= 0x4F) {
        hasRex = true;
        pos++;
    }

    uint8_t op = code[pos++];

    switch (op) {
    case 0x50: case 0x51: case 0x52: case 0x53:
    case 0x54: case 0x55: case 0x56: case 0x57:
    case 0x58: case 0x59: case 0x5A: case 0x5B:
    case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        return pos;

    case 0x60: case 0x61: return pos;
    case 0x63: return DecodeModRM(code, pos);
    case 0x68: return pos + 4;
    case 0x69: return DecodeModRM(code, pos) + 4;
    case 0x6A: return pos + 1;
    case 0x6B: return DecodeModRM(code, pos) + 1;

    case 0x66: {
        if (code[pos] >= 0x40 && code[pos] <= 0x4F) pos++;
        uint8_t op2 = code[pos++];
        switch (op2) {
        case 0x05: return pos;
        case 0x0F: {
            uint8_t op3 = code[pos++];
            switch (op3) {
            case 0x10: case 0x11: return DecodeModRM(code, pos);
            case 0x12: case 0x13: return DecodeModRM(code, pos);
            case 0x6E: case 0x6F: return DecodeModRM(code, pos);
            case 0x7E: case 0x7F: return DecodeModRM(code, pos);
            default: return DecodeModRM(code, pos);
            }
        }
        case 0x89: return DecodeModRM(code, pos);
        case 0x8B: return DecodeModRM(code, pos);
        case 0xC1: return DecodeModRM(code, pos) + 1;
        case 0xC7: return DecodeModRM(code, pos);
        case 0xD1: return DecodeModRM(code, pos);
        case 0xD4: return DecodeModRM(code, pos);
        case 0xD5: return DecodeModRM(code, pos) + 1;
        case 0xD6: return DecodeModRM(code, pos);
        case 0xD7: return DecodeModRM(code, pos);
        case 0xEF: return DecodeModRM(code, pos);
        default: return DecodeModRM(code, pos);
        }
    }

    case 0x70: case 0x71: case 0x72: case 0x73:
    case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B:
    case 0x7C: case 0x7D: case 0x7E: case 0x7F:
        return pos + 1;

    case 0x80: return DecodeModRM(code, pos) + 1;
    case 0x81: return DecodeModRM(code, pos) + 4;
    case 0x82: return DecodeModRM(code, pos) + 1;
    case 0x83: return DecodeModRM(code, pos) + 1;

    case 0x84: case 0x85: case 0x86: case 0x87:
    case 0x88: case 0x89: case 0x8A: case 0x8B:
    case 0x8D:
        return DecodeModRM(code, pos);

    case 0x90: return pos;

    case 0xA0: case 0xA1: case 0xA2: case 0xA3:
        return pos + (hasRex ? 8 : 4);

    case 0xA8: return pos + 1;
    case 0xA9: return pos + 4;

    case 0xB0: case 0xB1: case 0xB2: case 0xB3:
    case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        return pos + 1;

    case 0xB8: case 0xB9: case 0xBA: case 0xBB:
    case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        return pos + 4;

    case 0xC0: case 0xC1: return DecodeModRM(code, pos) + 1;
    case 0xC2: return pos + 2;
    case 0xC3: return pos;
    case 0xC6: return DecodeModRM(code, pos) + 1;
    case 0xC7: return DecodeModRM(code, pos) + 4;
    case 0xC8: return pos + 3;
    case 0xCC: return pos;
    case 0xCD: return pos + 2;

    case 0xD0: case 0xD1: case 0xD2: case 0xD3:
    case 0xD4: case 0xD5: case 0xD6: case 0xD7:
    case 0xD8: case 0xD9: case 0xDA: case 0xDB:
    case 0xDC: case 0xDD: case 0xDE: case 0xDF:
        return DecodeModRM(code, pos);

    case 0xE8: return pos + 4;
    case 0xE9: return pos + 4;
    case 0xEB: return pos + 1;

    case 0xF0: {
        uint8_t next = code[pos];
        if (next == 0x0F || next == 0x44 || next == 0x45 ||
            next == 0x80 || next == 0x81 || next == 0x82 || next == 0x83 ||
            next == 0x90 || next == 0x91 || next == 0x92 || next == 0x93 ||
            next == 0xA0 || next == 0xA1 || next == 0xA3 ||
            next == 0xC0 || next == 0xC1) {
            pos++;
            if (next == 0x0F) {
                pos++;
                return DecodeModRM(code, pos);
            }
            return DecodeModRM(code, pos);
        }
        return DecodeModRM(code, pos);
    }

    case 0xF2: {
        uint8_t next = code[pos];
        if (next == 0x0F) {
            pos++;
            uint8_t op2 = code[pos++];
            switch (op2) {
            case 0x10: case 0x11: return DecodeModRM(code, pos);
            case 0x12: case 0x13: return DecodeModRM(code, pos);
            case 0x2A: case 0x2B: return DecodeModRM(code, pos);
            case 0x51: case 0x58: case 0x59: case 0x5C:
            case 0x5D: case 0x5E: case 0x5F:
                return DecodeModRM(code, pos);
            default: return 0;
            }
        }
        return DecodeModRM(code, pos);
    }

    case 0xF3: {
        uint8_t next = code[pos];
        if (next == 0x0F) {
            pos++;
            uint8_t op2 = code[pos++];
            switch (op2) {
            case 0x10: case 0x11: return DecodeModRM(code, pos);
            case 0x12: case 0x13: return DecodeModRM(code, pos);
            case 0x1E: return DecodeModRM(code, pos);
            case 0x6E: case 0x6F: return DecodeModRM(code, pos);
            case 0x7E: case 0x7F: return DecodeModRM(code, pos);
            default: return 0;
            }
        }
        return DecodeModRM(code, pos);
    }

    case 0xF6: return DecodeModRM(code, pos);
    case 0xF7: return DecodeModRM(code, pos);
    case 0xFE: return DecodeModRM(code, pos);
    case 0xFF: return DecodeModRM(code, pos);

    case 0x0F: {
        uint8_t op2 = code[pos++];
        switch (op2) {
        case 0x05: return pos;
        case 0x0B: return pos;
        case 0x31: return pos;

        case 0x40: case 0x41: case 0x42: case 0x43:
        case 0x44: case 0x45: case 0x46: case 0x47:
        case 0x48: case 0x49: case 0x4A: case 0x4B:
        case 0x4C: case 0x4D: case 0x4E: case 0x4F:
            return DecodeModRM(code, pos);

        case 0x70: case 0x71: case 0x72: case 0x73:
        case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7A: case 0x7B:
        case 0x7C: case 0x7D: case 0x7E: case 0x7F:
            return pos + 1;

        case 0x80: case 0x81: case 0x82: case 0x83:
        case 0x84: case 0x85: case 0x86: case 0x87:
        case 0x88: case 0x89: case 0x8A: case 0x8B:
        case 0x8C: case 0x8D: case 0x8E: case 0x8F:
            return pos + 4;

        case 0x90: case 0x91: case 0x92: case 0x93:
        case 0x94: case 0x95: case 0x96: case 0x97:
        case 0x98: case 0x99: case 0x9A: case 0x9B:
        case 0x9C: case 0x9D: case 0x9E: case 0x9F:
            return DecodeModRM(code, pos);

        case 0xA0: case 0xA1: case 0xA2: case 0xA3:
            return pos + 1;

        case 0xAF: case 0xB6: case 0xB7:
        case 0xBE: case 0xBF:
            return DecodeModRM(code, pos);

        case 0xC0: case 0xC1:
            return DecodeModRM(code, pos);

        case 0x10: case 0x11:
        case 0x12: case 0x13:
        case 0x14: case 0x15:
        case 0x16: case 0x17:
        case 0x18: case 0x19: case 0x1A: case 0x1B:
        case 0x1C: case 0x1D: case 0x1E: case 0x1F:
        case 0x28: case 0x29:
        case 0x2E: case 0x2F:
        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57:
        case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        case 0x60: case 0x61: case 0x62: case 0x63:
        case 0x64: case 0x65: case 0x66: case 0x67:
        case 0x68: case 0x69: case 0x6A: case 0x6B:
        case 0x6C: case 0x6D: case 0x6E: case 0x6F:
            return DecodeModRM(code, pos);

        default: return 0;
        }
    }

    default: return 0;
    }
}

static void* AllocNearby(void* target, size_t size) {
    uintptr_t targetAddr = (uintptr_t)target;
    uintptr_t searchStart, searchEnd;

    if (targetAddr > 0x70000000)
        searchStart = targetAddr - 0x70000000;
    else
        searchStart = 0x10000;

    searchEnd = targetAddr + 0x70000000;

    for (uintptr_t addr = searchStart; addr < searchEnd; addr += 0x10000) {
        void* p = VirtualAlloc((void*)addr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (p) return p;
    }

    return VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
}

static void WriteAbsJmp(uint8_t* buf, uintptr_t target) {
    int64_t rel = (int64_t)target - (int64_t)(buf + 5);
    if (rel >= -0x80000000LL && rel < 0x80000000LL) {
        buf[0] = 0xE9;
        *(int32_t*)(buf + 1) = (int32_t)rel;
    } else {
        buf[0] = 0xFF;
        buf[1] = 0x25;
        *(int32_t*)(buf + 2) = 0;
        *(uint64_t*)(buf + 6) = (uint64_t)target;
    }
}

static int JmpSize(uint8_t* from, uintptr_t to) {
    int64_t rel = (int64_t)to - (int64_t)(from + 5);
    return (rel >= -0x80000000LL && rel < 0x80000000LL) ? 5 : 14;
}

static DWORD GetProtectFlags(uint8_t* code) {
    MEMORY_BASIC_INFORMATION mbi = {};
    VirtualQuery(code, &mbi, sizeof(mbi));
    DWORD prot = mbi.Protect;
    if (prot & PAGE_EXECUTE_WRITECOPY) return PAGE_EXECUTE_WRITECOPY;
    if (prot & PAGE_EXECUTE_READWRITE) return PAGE_EXECUTE_READWRITE;
    if (prot & PAGE_EXECUTE_READ) return PAGE_EXECUTE_READ;
    return prot;
}

static void LogHook(const char* msg) {
    char buf[512];
    snprintf(buf, sizeof(buf), "[NATIVEHOOK] %s", msg);
    consoleLog(buf);
}

static bool TryHook(void* target, void* hook_fn, void** out_original) {
    uint8_t* code = (uint8_t*)target;
    char buf[512];

    snprintf(buf, sizeof(buf), "TryHook: target=0x%p, hook=0x%p", target, hook_fn);
    LogHook(buf);

    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(code, &mbi, sizeof(mbi))) {
        snprintf(buf, sizeof(buf), "TryHook: VirtualQuery FAILED for 0x%p (err=%d)", target, GetLastError());
        LogHook(buf);
        return false;
    }
    snprintf(buf, sizeof(buf), "TryHook: prot=0x%X allocBase=0x%p regionSize=0x%zX",
        mbi.Protect, mbi.AllocationBase, mbi.RegionSize);
    LogHook(buf);

    if (!(mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
        snprintf(buf, sizeof(buf), "TryHook: Bad protection 0x%X (need EXEC)", mbi.Protect);
        LogHook(buf);
        return false;
    }

    int totalLen = 0;
    while (totalLen < 5) {
        int len = DecodeInstructionLength(code + totalLen);
        if (len == 0) {
            snprintf(buf, sizeof(buf), "TryHook: Cannot decode instruction at 0x%p + 0x%X (byte=0x%02X)",
                code, totalLen, code[totalLen]);
            LogHook(buf);
            return false;
        }
        totalLen += len;
        if (totalLen > 32) {
            LogHook("TryHook: Instruction span > 32 bytes");
            return false;
        }
    }

    snprintf(buf, sizeof(buf), "TryHook: Decoded %d bytes for JMP patch", totalLen);
    LogHook(buf);

    if (totalLen < 5) {
        LogHook("TryHook: Need at least 5 bytes");
        return false;
    }

    uintptr_t returnAddr = (uintptr_t)(code + totalLen);

    // Build a self-contained relay stub near the target
    // Layout: save regs -> call dispatch callback -> restore regs -> stolen bytes -> JMP back
    uint8_t* relay = (uint8_t*)AllocNearby(target, 256);
    if (!relay) {
        LogHook("TryHook: AllocNearby relay FAILED");
        return false;
    }

    int off = 0;

    // push rax, rcx, rdx, r8, r9, r10, r11
    relay[off++] = 0x50;                                     // push rax
    relay[off++] = 0x51;                                     // push rcx
    relay[off++] = 0x52;                                     // push rdx
    relay[off++] = 0x41; relay[off++] = 0x50;               // push r8
    relay[off++] = 0x41; relay[off++] = 0x51;               // push r9
    relay[off++] = 0x41; relay[off++] = 0x52;               // push r10
    relay[off++] = 0x41; relay[off++] = 0x53;               // push r11

    // sub rsp, 0x28 (shadow space for callback call)
    relay[off++] = 0x48; relay[off++] = 0x83; relay[off++] = 0xEC; relay[off++] = 0x28;

    // lea rcx, [rsp+0x58] -> points to saved rax on stack
    // After 7 pushes (56 bytes) + sub rsp,0x28 (40 bytes) = 96 bytes below entry
    // rax is at entry_rsp-8 = rsp+88 = rsp+0x58
    // ModRM: mod=01 (disp8), reg=001 (rcx), rm=100 (SIB)
    relay[off++] = 0x48; relay[off++] = 0x8D; relay[off++] = 0x4C; relay[off++] = 0x24;
    relay[off++] = 0x58;

    if (g_hookDispatchFn) {
        // call [rip+0] ; FF 15 00 00 00 00 followed by 8-byte address
        relay[off++] = 0xFF; relay[off++] = 0x15;
        *(int32_t*)(relay + off) = 0; off += 4;
        *(uint64_t*)(relay + off) = (uint64_t)g_hookDispatchFn; off += 8;
    }

    // add rsp, 0x28
    relay[off++] = 0x48; relay[off++] = 0x83; relay[off++] = 0xC4; relay[off++] = 0x28;

    // pop r11, r10, r9, r8, rdx, rcx, rax
    relay[off++] = 0x41; relay[off++] = 0x5B;               // pop r11
    relay[off++] = 0x41; relay[off++] = 0x5A;               // pop r10
    relay[off++] = 0x41; relay[off++] = 0x59;               // pop r9
    relay[off++] = 0x41; relay[off++] = 0x58;               // pop r8
    relay[off++] = 0x5A;                                     // pop rdx
    relay[off++] = 0x59;                                     // pop rcx
    relay[off++] = 0x58;                                     // pop rax

    // Re-execute the stolen bytes so the game's flags and registers stay correct.
    // First, check if any stolen instruction uses relative addressing (E8/E9/EB/7x/0F8x).
    // If so, we can't safely copy them — skip and warn.
    bool canCopyStolen = true;
    {
        uint8_t* stolen = (uint8_t*)target;
        int sPos = 0;
        while (sPos < totalLen) {
            int instrStart = sPos;
            uint8_t b = stolen[sPos];

            // Skip REX prefix
            if (b >= 0x40 && b <= 0x4F) { sPos++; if (sPos >= totalLen) { canCopyStolen = false; break; } b = stolen[sPos]; }

            // Relative JMP/CALL
            if (b == 0xE8 || b == 0xE9 || b == 0xEB) { canCopyStolen = false; break; }
            // Short conditional JMPs (70-7F)
            if (b >= 0x70 && b <= 0x7F) { canCopyStolen = false; break; }
            // Near conditional JMPs (0F 80-0F 8F)
            if (b == 0x0F && sPos + 1 < totalLen) {
                uint8_t b2 = stolen[sPos + 1];
                if (b2 >= 0x80 && b2 <= 0x8F) { canCopyStolen = false; break; }
            }

            // Advance past this instruction using the decoder
            int len = DecodeInstructionLength(stolen + instrStart);
            if (len == 0) { canCopyStolen = false; break; }
            sPos = instrStart + len;
        }
    }

    if (canCopyStolen) {
        // Copy stolen bytes verbatim into the relay — they're safe, no relative addressing
        memcpy(relay + off, (uint8_t*)target, totalLen);
        off += totalLen;
        snprintf(buf, sizeof(buf), "TryHook: Copied %d stolen bytes to relay (safe)", totalLen);
        LogHook(buf);
    } else {
        LogHook("TryHook: Stolen bytes contain relative instructions, skipping (game may break)");
    }

    // JMP [rip+0] -> return address (continue after the stolen bytes)
    relay[off++] = 0xFF; relay[off++] = 0x25;
    *(int32_t*)(relay + off) = 0; off += 4;
    *(uint64_t*)(relay + off) = returnAddr; off += 8;

    snprintf(buf, sizeof(buf), "TryHook: Relay at 0x%p (%d bytes)", relay, off);
    LogHook(buf);

    // Now patch the target: 5-byte JMP to relay
    DWORD oldProtect = 0;
    if (!VirtualProtect(code, totalLen, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        snprintf(buf, sizeof(buf), "TryHook: VirtualProtect FAILED (err=%d)", GetLastError());
        LogHook(buf);
        return false;
    }

    WriteAbsJmp(code, (uintptr_t)relay);
    for (int i = 5; i < totalLen; i++)
        code[i] = 0x90;

    DWORD temp;
    VirtualProtect(code, totalLen, oldProtect, &temp);
    FlushInstructionCache(GetCurrentProcess(), code, totalLen);

    HookEntry entry = {};
    entry.target = target;
    entry.hook = hook_fn;
    entry.original = relay;
    entry.trampoline = relay;
    entry.patchSize = totalLen;
    memcpy(entry.originalBytes, (uint8_t*)target, totalLen);
    g_hooks[target] = entry;

    if (out_original) *out_original = relay;

    snprintf(buf, sizeof(buf), "TryHook: SUCCESS! hooked 0x%p -> relay 0x%p", target, relay);
    LogHook(buf);
    return true;
}

void native_hook::SetDispatchCallback(void (*fn)(SavedRegs*)) {
    g_hookDispatchFn = fn;
}

bool native_hook::Install(void* target, void* hook_fn, void** original_fn) {
    if (!target || !hook_fn) {
        LogHook("Install: null target or hook");
        return false;
    }
    if (g_hooks.count(target)) {
        LogHook("Install: already hooked");
        return false;
    }

    void* original = nullptr;
    __try {
        if (!TryHook(target, hook_fn, &original))
            return false;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Install: EXCEPTION at 0x%p (code=0x%X)", target, GetExceptionCode());
        LogHook(buf);
        return false;
    }

    if (original_fn) *original_fn = original;
    return true;
}

bool native_hook::Remove(void* target) {
    auto it = g_hooks.find(target);
    if (it == g_hooks.end()) return false;

    HookEntry entry = it->second;

    DWORD oldProtect = 0;
    VirtualProtect(entry.target, entry.patchSize, PAGE_EXECUTE_READWRITE, &oldProtect);
    memcpy(entry.target, entry.originalBytes, entry.patchSize);
    DWORD temp;
    VirtualProtect(entry.target, entry.patchSize, oldProtect, &temp);
    FlushInstructionCache(GetCurrentProcess(), entry.target, entry.patchSize);
    VirtualFree(entry.trampoline, 0, MEM_RELEASE);

    g_hooks.erase(it);
    return true;
}

void native_hook::RemoveAll() {
    std::vector<void*> targets;
    for (auto& it : g_hooks) targets.push_back(it.first);
    for (auto t : targets) Remove(t);
}

int native_hook::GetHookCount() {
    return (int)g_hooks.size();
}
