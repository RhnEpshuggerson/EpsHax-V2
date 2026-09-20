#pragma once
#include <windows.h>
#include <cstdint>
#include <map>

struct HookEntry {
    void* target;
    void* hook;
    void* original;     // trampoline = call original
    uint8_t* trampoline;
    size_t patchSize;
    uint8_t originalBytes[32];
};

struct SavedRegs {
    uint64_t rax, rcx, rdx, r8, r9, r10, r11;
};

namespace native_hook {
    void SetDispatchCallback(void (*fn)(SavedRegs*));
    bool Install(void* target, void* hook_fn, void** original_fn);
    bool Remove(void* target);
    void RemoveAll();
    int GetHookCount();
}
