#include "memheap_fast_lock.h"
#include "diagnostic_logger.h"

#include <windows.h>
#include <intrin.h>
#include <cstdint>
#include <cstring>

namespace G3O {

namespace {

// RVA of MemHeap's critical section inside SharedBase.dll (0x1011D018 at the preferred base).
constexpr std::uintptr_t kMemHeapCsRva = 0x11D018;

using EnterFn = void(WINAPI*)(LPCRITICAL_SECTION);
using LeaveFn = void(WINAPI*)(LPCRITICAL_SECTION);

EnterFn g_realEnter = nullptr;
LeaveFn g_realLeave = nullptr;
void* g_csAddress = nullptr;
void** g_enterSlot = nullptr;
void** g_leaveSlot = nullptr;

struct alignas(64) FastLock {
    volatile long owner = 0;  // thread id, 0 = free
    long count = 0;           // recursion depth, touched only by the owner
};
FastLock g_lock;

inline long CurrentTid() { return static_cast<long>(__readfsdword(0x24)); }

void WINAPI FastEnter(LPCRITICAL_SECTION cs) {
    if (cs != g_csAddress) { g_realEnter(cs); return; }
    const long tid = CurrentTid();
    if (g_lock.owner == tid) { ++g_lock.count; return; }
    unsigned spins = 0;
    while (_InterlockedCompareExchange(&g_lock.owner, tid, 0) != 0) {
        if (++spins < 2000) {
            _mm_pause();
        } else if (spins < 2100) {
            SwitchToThread();
        } else {
            Sleep(0);
        }
    }
    g_lock.count = 1;
}

void WINAPI FastLeave(LPCRITICAL_SECTION cs) {
    if (cs != g_csAddress) { g_realLeave(cs); return; }
    const long tid = CurrentTid();
    if (g_lock.owner != tid) {
        // Lock was taken through the original critical section before the patch was applied.
        g_realLeave(cs);
        return;
    }
    if (--g_lock.count == 0) {
        _ReadWriteBarrier();
        g_lock.owner = 0;
    }
}

bool PatchSlot(void** slot, void* expected, void* replacement) {
    if (*slot != expected) return false;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *slot = replacement;
    VirtualProtect(slot, sizeof(void*), old, &old);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    return true;
}

// Finds SharedBase's IAT slots for KERNEL32 Enter/LeaveCriticalSection.
bool FindImportSlots(HMODULE module, void*** enterSlot, void*** leaveSlot) {
    auto* base = reinterpret_cast<std::uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    for (; desc->Name; ++desc) {
        const char* dll = reinterpret_cast<const char*>(base + desc->Name);
        if (_stricmp(dll, "KERNEL32.dll") != 0) continue;
        auto* thunkName = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));
        auto* thunkIat = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + desc->FirstThunk);
        for (; thunkName->u1.AddressOfData; ++thunkName, ++thunkIat) {
            if (IMAGE_SNAP_BY_ORDINAL32(thunkName->u1.Ordinal)) continue;
            auto* ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + thunkName->u1.AddressOfData);
            if (strcmp(ibn->Name, "EnterCriticalSection") == 0) *enterSlot = reinterpret_cast<void**>(&thunkIat->u1.Function);
            else if (strcmp(ibn->Name, "LeaveCriticalSection") == 0) *leaveSlot = reinterpret_cast<void**>(&thunkIat->u1.Function);
        }
    }
    return *enterSlot && *leaveSlot;
}

} // namespace

bool InitializeMemHeapFastLock() {
    if (g_enterSlot) return true;
    HMODULE shared = GetModuleHandleA("SharedBase.dll");
    if (!shared) return false;

    void** enterSlot = nullptr;
    void** leaveSlot = nullptr;
    if (!FindImportSlots(shared, &enterSlot, &leaveSlot)) {
        Log("G3O", "MemHeapFastLock: SharedBase import slots not found; disabled.");
        return false;
    }

    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    void* k32Enter = k32 ? reinterpret_cast<void*>(GetProcAddress(k32, "EnterCriticalSection")) : nullptr;
    void* k32Leave = k32 ? reinterpret_cast<void*>(GetProcAddress(k32, "LeaveCriticalSection")) : nullptr;
    if (*enterSlot != k32Enter || *leaveSlot != k32Leave) {
        Log("G3O", "MemHeapFastLock: IAT entries were already redirected; disabled for safety.");
        return false;
    }

    g_csAddress = reinterpret_cast<std::uint8_t*>(shared) + kMemHeapCsRva;
    g_realEnter = reinterpret_cast<EnterFn>(*enterSlot);
    g_realLeave = reinterpret_cast<LeaveFn>(*leaveSlot);

    if (!PatchSlot(enterSlot, *enterSlot, reinterpret_cast<void*>(&FastEnter))) return false;
    if (!PatchSlot(leaveSlot, *leaveSlot, reinterpret_cast<void*>(&FastLeave))) {
        PatchSlot(enterSlot, reinterpret_cast<void*>(&FastEnter), reinterpret_cast<void*>(g_realEnter));
        return false;
    }
    g_enterSlot = enterSlot;
    g_leaveSlot = leaveSlot;
    Log("G3O", "MemHeapFastLock: MemHeap critical section replaced by a user-mode spin lock (cs=%p).", g_csAddress);
    return true;
}

void ShutdownMemHeapFastLock() {
    // Intentionally left patched: other threads may still be inside the lock during shutdown.
}

} // namespace G3O
