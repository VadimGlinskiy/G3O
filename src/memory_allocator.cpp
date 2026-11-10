#include "memory_allocator.h"
#include "diagnostic_logger.h"
#include <windows.h>
#include <mimalloc.h>
#include <minhook.h>
#include <atomic>

namespace MemoryOptimizer {

// Telemetry counters
static std::atomic<uint64_t> s_miAllocCount{0};
static std::atomic<uint64_t> s_miFreeCount{0};
static std::atomic<uint64_t> s_origFreeCount{0};
static std::atomic<uint64_t> s_miReallocCount{0};
static std::atomic<uint64_t> s_origReallocCount{0};

uint64_t GetMiAllocCount() { return s_miAllocCount.load(std::memory_order_relaxed); }
uint64_t GetMiFreeCount() { return s_miFreeCount.load(std::memory_order_relaxed); }
uint64_t GetLegacyFreeCount() { return s_origFreeCount.load(std::memory_order_relaxed); }
uint64_t GetMiReallocCount() { return s_miReallocCount.load(std::memory_order_relaxed); }
uint64_t GetLegacyReallocCount() { return s_origReallocCount.load(std::memory_order_relaxed); }

// Pure __stdcall function pointer typedefs for SharedBase.dll MemHeap static methods
typedef void* (__stdcall* PFN_MemHeap_Alloc)(unsigned int size);
typedef void  (__stdcall* PFN_MemHeap_Free)(void* ptr);
typedef void* (__stdcall* PFN_MemHeap_ReAlloc)(void* ptr, unsigned int size);

// Trampolines
static PFN_MemHeap_Alloc    g_origAlloc = nullptr;
static PFN_MemHeap_Free     g_origFreeHeap = nullptr;
static PFN_MemHeap_ReAlloc  g_origReAllocHeap = nullptr;

static PFN_MemHeap_Alloc    g_origAllocUnsynced = nullptr;
static PFN_MemHeap_Free     g_origFreeUnsynced = nullptr;
static PFN_MemHeap_ReAlloc  g_origReAllocUnsynced = nullptr;

static PFN_MemHeap_Alloc    g_origAllocShortUsage = nullptr;
static PFN_MemHeap_Free     g_origFreeShortUsage = nullptr;
static PFN_MemHeap_Alloc    g_origAllocShortUsageUnsynced = nullptr;
static PFN_MemHeap_Free     g_origFreeShortUsageUnsynced = nullptr;

// -----------------------------------------------------------------------------
// MemHeap Hooks (SharedBase.dll) — pure __stdcall static methods
// All bCMemoryAdmin methods (Malloc, Free, Realloc, NewObject, DeleteObject)
// in Genome Engine simply forward to these MemHeap methods. Hooking MemHeap
// cleanly intercepts all allocations without requiring any __thiscall wrappers.
// -----------------------------------------------------------------------------

static void* __stdcall Hook_MemHeap_Alloc(unsigned int size) {
    void* ptr = mi_malloc(size);
    if (ptr) {
        s_miAllocCount.fetch_add(1, std::memory_order_relaxed);
        return ptr;
    }
    return g_origAlloc ? g_origAlloc(size) : nullptr;
}

static void __stdcall Hook_MemHeap_Free(void* ptr) {
    if (!ptr) return;
    if (mi_is_in_heap_region(ptr)) {
        s_miFreeCount.fetch_add(1, std::memory_order_relaxed);
        mi_free(ptr);
    } else {
        s_origFreeCount.fetch_add(1, std::memory_order_relaxed);
        if (g_origFreeHeap) {
            g_origFreeHeap(ptr);
        }
    }
}

static void* __stdcall Hook_MemHeap_ReAlloc(void* ptr, unsigned int size) {
    if (!ptr) {
        s_miAllocCount.fetch_add(1, std::memory_order_relaxed);
        return mi_malloc(size);
    }
    if (mi_is_in_heap_region(ptr)) {
        s_miReallocCount.fetch_add(1, std::memory_order_relaxed);
        return mi_realloc(ptr, size);
    }
    s_origReallocCount.fetch_add(1, std::memory_order_relaxed);
    if (g_origReAllocHeap) {
        return g_origReAllocHeap(ptr, size);
    }
    return nullptr;
}

static void* __stdcall Hook_MemHeap_AllocUnsynced(unsigned int size) {
    void* ptr = mi_malloc(size);
    if (ptr) {
        s_miAllocCount.fetch_add(1, std::memory_order_relaxed);
        return ptr;
    }
    return g_origAllocUnsynced ? g_origAllocUnsynced(size) : nullptr;
}

static void __stdcall Hook_MemHeap_FreeUnsynced(void* ptr) {
    if (!ptr) return;
    if (mi_is_in_heap_region(ptr)) {
        s_miFreeCount.fetch_add(1, std::memory_order_relaxed);
        mi_free(ptr);
    } else {
        s_origFreeCount.fetch_add(1, std::memory_order_relaxed);
        if (g_origFreeUnsynced) {
            g_origFreeUnsynced(ptr);
        }
    }
}

static void* __stdcall Hook_MemHeap_ReAllocUnsynced(void* ptr, unsigned int size) {
    if (!ptr) {
        s_miAllocCount.fetch_add(1, std::memory_order_relaxed);
        return mi_malloc(size);
    }
    if (mi_is_in_heap_region(ptr)) {
        s_miReallocCount.fetch_add(1, std::memory_order_relaxed);
        return mi_realloc(ptr, size);
    }
    s_origReallocCount.fetch_add(1, std::memory_order_relaxed);
    if (g_origReAllocUnsynced) {
        return g_origReAllocUnsynced(ptr, size);
    }
    return nullptr;
}

static void* __stdcall Hook_MemHeap_AllocShortUsage(unsigned int size) {
    void* ptr = mi_malloc(size);
    if (ptr) {
        s_miAllocCount.fetch_add(1, std::memory_order_relaxed);
        return ptr;
    }
    return g_origAllocShortUsage ? g_origAllocShortUsage(size) : nullptr;
}

static void __stdcall Hook_MemHeap_FreeShortUsage(void* ptr) {
    if (!ptr) return;
    if (mi_is_in_heap_region(ptr)) {
        s_miFreeCount.fetch_add(1, std::memory_order_relaxed);
        mi_free(ptr);
    } else {
        s_origFreeCount.fetch_add(1, std::memory_order_relaxed);
        if (g_origFreeShortUsage) {
            g_origFreeShortUsage(ptr);
        }
    }
}

static void* __stdcall Hook_MemHeap_AllocShortUsageUnsynced(unsigned int size) {
    void* ptr = mi_malloc(size);
    if (ptr) {
        s_miAllocCount.fetch_add(1, std::memory_order_relaxed);
        return ptr;
    }
    return g_origAllocShortUsageUnsynced ? g_origAllocShortUsageUnsynced(size) : nullptr;
}

static void __stdcall Hook_MemHeap_FreeShortUsageUnsynced(void* ptr) {
    if (!ptr) return;
    if (mi_is_in_heap_region(ptr)) {
        s_miFreeCount.fetch_add(1, std::memory_order_relaxed);
        mi_free(ptr);
    } else {
        s_origFreeCount.fetch_add(1, std::memory_order_relaxed);
        if (g_origFreeShortUsageUnsynced) {
            g_origFreeShortUsageUnsynced(ptr);
        }
    }
}

// -----------------------------------------------------------------------------
// Initialization & Maintenance
// -----------------------------------------------------------------------------
bool Initialize() {
    // 1. Configure mimalloc options for x86 32-bit Large Address Aware game execution
    mi_option_set(mi_option_purge_delay, -1);          // -1 = No memory purging/decommitting to OS
    mi_option_set(mi_option_purge_decommits, 0);       // Keep committed virtual memory in process VAS
    mi_option_set(mi_option_show_errors, 0);           // Disable error popups
    mi_option_set(mi_option_verbose, 0);

    HMODULE hSharedBase = GetModuleHandleA("SharedBase.dll");
    if (!hSharedBase) {
        hSharedBase = LoadLibraryA("SharedBase.dll");
        if (!hSharedBase) {
            G3O::Log("G3O", "MemoryOptimizer: SharedBase.dll not loaded.");
            return false;
        }
    }

    // Hook MemHeap standard methods (all callers of bCMemoryAdmin::Malloc/Free forward directly here)
    FARPROC pAlloc = GetProcAddress(hSharedBase, "?Alloc@MemHeap@@SGPAXI@Z");
    FARPROC pFreeHeap = GetProcAddress(hSharedBase, "?Free@MemHeap@@SGXPAX@Z");
    FARPROC pReAllocHeap = GetProcAddress(hSharedBase, "?ReAlloc@MemHeap@@SGPAXPAXI@Z");

    if (pAlloc) {
        MH_CreateHook(pAlloc, reinterpret_cast<LPVOID>(&Hook_MemHeap_Alloc), reinterpret_cast<LPVOID*>(&g_origAlloc));
        MH_EnableHook(pAlloc);
    }
    if (pFreeHeap) {
        MH_CreateHook(pFreeHeap, reinterpret_cast<LPVOID>(&Hook_MemHeap_Free), reinterpret_cast<LPVOID*>(&g_origFreeHeap));
        MH_EnableHook(pFreeHeap);
    }
    if (pReAllocHeap) {
        MH_CreateHook(pReAllocHeap, reinterpret_cast<LPVOID>(&Hook_MemHeap_ReAlloc), reinterpret_cast<LPVOID*>(&g_origReAllocHeap));
        MH_EnableHook(pReAllocHeap);
    }

    // Hook MemHeap Unsynced methods
    FARPROC pAllocUnsync = GetProcAddress(hSharedBase, "?AllocUnsynced@MemHeap@@SGPAXI@Z");
    FARPROC pFreeUnsync = GetProcAddress(hSharedBase, "?FreeUnsynced@MemHeap@@SGXPAX@Z");
    FARPROC pReAllocUnsync = GetProcAddress(hSharedBase, "?ReAllocUnsynced@MemHeap@@SGPAXPAXI@Z");

    if (pAllocUnsync) {
        MH_CreateHook(pAllocUnsync, reinterpret_cast<LPVOID>(&Hook_MemHeap_AllocUnsynced), reinterpret_cast<LPVOID*>(&g_origAllocUnsynced));
        MH_EnableHook(pAllocUnsync);
    }
    if (pFreeUnsync) {
        MH_CreateHook(pFreeUnsync, reinterpret_cast<LPVOID>(&Hook_MemHeap_FreeUnsynced), reinterpret_cast<LPVOID*>(&g_origFreeUnsynced));
        MH_EnableHook(pFreeUnsync);
    }
    if (pReAllocUnsync) {
        MH_CreateHook(pReAllocUnsync, reinterpret_cast<LPVOID>(&Hook_MemHeap_ReAllocUnsynced), reinterpret_cast<LPVOID*>(&g_origReAllocUnsynced));
        MH_EnableHook(pReAllocUnsync);
    }

    // Hook MemHeap ShortUsage methods
    FARPROC pAllocShort = GetProcAddress(hSharedBase, "?AllocShortUsage@MemHeap@@SGPAXI@Z");
    FARPROC pFreeShort = GetProcAddress(hSharedBase, "?FreeShortUsage@MemHeap@@SGXPAX@Z");
    FARPROC pAllocShortUnsync = GetProcAddress(hSharedBase, "?AllocShortUsageUnsynced@MemHeap@@SGPAXI@Z");
    FARPROC pFreeShortUnsync = GetProcAddress(hSharedBase, "?FreeShortUsageUnsynced@MemHeap@@SGXPAX@Z");

    if (pAllocShort) {
        MH_CreateHook(pAllocShort, reinterpret_cast<LPVOID>(&Hook_MemHeap_AllocShortUsage), reinterpret_cast<LPVOID*>(&g_origAllocShortUsage));
        MH_EnableHook(pAllocShort);
    }
    if (pFreeShort) {
        MH_CreateHook(pFreeShort, reinterpret_cast<LPVOID>(&Hook_MemHeap_FreeShortUsage), reinterpret_cast<LPVOID*>(&g_origFreeShortUsage));
        MH_EnableHook(pFreeShort);
    }
    if (pAllocShortUnsync) {
        MH_CreateHook(pAllocShortUnsync, reinterpret_cast<LPVOID>(&Hook_MemHeap_AllocShortUsageUnsynced), reinterpret_cast<LPVOID*>(&g_origAllocShortUsageUnsynced));
        MH_EnableHook(pAllocShortUnsync);
    }
    if (pFreeShortUnsync) {
        MH_CreateHook(pFreeShortUnsync, reinterpret_cast<LPVOID>(&Hook_MemHeap_FreeShortUsageUnsynced), reinterpret_cast<LPVOID*>(&g_origFreeShortUsageUnsynced));
        MH_EnableHook(pFreeShortUnsync);
    }

    G3O::Log("G3O", "Memory Optimizer initialized: MemHeap routed to mimalloc.");
    return true;
}

void Collect() {
    mi_collect(true);
}

void Shutdown() {
    G3O::Log("G3O", "Memory Optimizer shutdown stats: mimalloc allocs=%llu, mimalloc frees=%llu, legacy frees=%llu, mimalloc reallocs=%llu, legacy reallocs=%llu.",
        static_cast<unsigned long long>(GetMiAllocCount()),
        static_cast<unsigned long long>(GetMiFreeCount()),
        static_cast<unsigned long long>(GetLegacyFreeCount()),
        static_cast<unsigned long long>(GetMiReallocCount()),
        static_cast<unsigned long long>(GetLegacyReallocCount()));
}

} // namespace MemoryOptimizer
