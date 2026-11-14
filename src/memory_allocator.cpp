#include "memory_allocator.h"
#include "diagnostic_logger.h"

#include <windows.h>
#include <mimalloc.h>
#include <MinHook.h>
#include <atomic>

namespace G3O {

static_assert(sizeof(void*) == 4, "Gothic 3 memory allocator requires Win32.");

// =============================================================================
// Telemetry counters
// =============================================================================
static std::atomic<std::uint64_t> s_miAllocCount{0};
static std::atomic<std::uint64_t> s_miFreeCount{0};
static std::atomic<std::uint64_t> s_origFreeCount{0};
static std::atomic<std::uint64_t> s_miReallocCount{0};
static std::atomic<std::uint64_t> s_origReallocCount{0};

std::uint64_t GetMiAllocCount() { return s_miAllocCount.load(std::memory_order_relaxed); }
std::uint64_t GetMiFreeCount() { return s_miFreeCount.load(std::memory_order_relaxed); }
std::uint64_t GetLegacyFreeCount() { return s_origFreeCount.load(std::memory_order_relaxed); }
std::uint64_t GetMiReallocCount() { return s_miReallocCount.load(std::memory_order_relaxed); }
std::uint64_t GetLegacyReallocCount() { return s_origReallocCount.load(std::memory_order_relaxed); }

// =============================================================================
// Function pointer typedefs (pure __stdcall static methods in SharedBase.dll)
// =============================================================================
using PFN_MemHeap_Alloc = void* (__stdcall*)(unsigned int size);
using PFN_MemHeap_Free = void (__stdcall*)(void* ptr);
using PFN_MemHeap_ReAlloc = void* (__stdcall*)(void* ptr, unsigned int size);

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

// Hook target pointers for clean unhooking
static void* s_targetAlloc = nullptr;
static void* s_targetFree = nullptr;
static void* s_targetReAlloc = nullptr;
static void* s_targetAllocUnsync = nullptr;
static void* s_targetFreeUnsync = nullptr;
static void* s_targetReAllocUnsync = nullptr;
static void* s_targetAllocShort = nullptr;
static void* s_targetFreeShort = nullptr;
static void* s_targetAllocShortUnsync = nullptr;
static void* s_targetFreeShortUnsync = nullptr;

static bool s_hooksInstalled = false;
static int s_activeMode = 0;

// =============================================================================
// MemHeap Hooks (SharedBase.dll) — pure __stdcall static methods
// bCMemoryAdmin (Malloc, Free, Realloc, NewObject, DeleteObject) forwards directly
// to these MemHeap entrypoints.
// =============================================================================

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
    if (size == 0) {
        Hook_MemHeap_Free(ptr);
        return nullptr;
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
    if (size == 0) {
        Hook_MemHeap_FreeUnsynced(ptr);
        return nullptr;
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

// =============================================================================
// Configuration & Mimalloc tuning
// =============================================================================
static void ConfigureMimallocTuning() {
    // 32-bit Large Address Aware VAS options (Psycho Engine Fixes architecture):
    mi_option_set(mi_option_arena_reserve, 8 * 1024);         // 8 MB reservation per arena (instead of 128 MB default)
    mi_option_set(mi_option_arena_max_object_size, 64);       // 64 KB ceiling for arena; larger requests use OS path
    mi_option_set(mi_option_arena_eager_commit, 0);          // Demand-page commit (commit on first touch)
    mi_option_set(mi_option_purge_decommits, 0);             // Reset memory on purge, do not decommit (avoids VAS fragmentation)
    mi_option_set(mi_option_purge_delay, 1000);              // 1000 ms delayed purge
    mi_option_set(mi_option_retry_on_oom, 0);                // Disable retry spin on OOM
    mi_option_set(mi_option_page_reclaim_on_free, 1);        // Reclaim on free
    mi_option_set(mi_option_page_full_retain, 1);            // Retain full page per size class
    mi_option_set(mi_option_show_errors, 0);                 // Silence popups
    mi_option_set(mi_option_verbose, 0);

    // Attempt small bounded private arena (16 MB / 8 MB)
    const size_t arena_sizes[] = { 16 * 1024 * 1024, 8 * 1024 * 1024 };
    for (size_t size : arena_sizes) {
        mi_arena_id_t arena_id = 0;
        int res = mi_reserve_os_memory_ex(size, false, false, false, &arena_id);
        if (res == 0) {
            Log("G3O", "MemoryAllocator: mimalloc bounded arena reserved: %zu MB (id %d).",
                size / (1024 * 1024), arena_id);
            break;
        }
    }
}

bool InitializeMemoryAllocator(int mode) {
    if (mode <= 0) {
        Log("G3O", "MemoryAllocator: AllocatorMode=0 (Vanilla engine allocators).");
        return true;
    }

    s_activeMode = mode;
    ConfigureMimallocTuning();

    HMODULE hSharedBase = GetModuleHandleA("SharedBase.dll");
    if (!hSharedBase) {
        Log("G3O", "MemoryAllocator: SharedBase.dll not loaded; allocator hooks skipped.");
        return false;
    }

    // ShortUsage hooks (Mode 1 & Mode 2)
    s_targetAllocShort = reinterpret_cast<void*>(GetProcAddress(hSharedBase, "?AllocShortUsage@MemHeap@@SGPAXI@Z"));
    s_targetFreeShort = reinterpret_cast<void*>(GetProcAddress(hSharedBase, "?FreeShortUsage@MemHeap@@SGXPAX@Z"));
    s_targetAllocShortUnsync = reinterpret_cast<void*>(GetProcAddress(hSharedBase, "?AllocShortUsageUnsynced@MemHeap@@SGPAXI@Z"));
    s_targetFreeShortUnsync = reinterpret_cast<void*>(GetProcAddress(hSharedBase, "?FreeShortUsageUnsynced@MemHeap@@SGXPAX@Z"));

    if (s_targetAllocShort) {
        MH_CreateHook(s_targetAllocShort, reinterpret_cast<void*>(&Hook_MemHeap_AllocShortUsage),
                      reinterpret_cast<void**>(&g_origAllocShortUsage));
        MH_EnableHook(s_targetAllocShort);
    }
    if (s_targetFreeShort) {
        MH_CreateHook(s_targetFreeShort, reinterpret_cast<void*>(&Hook_MemHeap_FreeShortUsage),
                      reinterpret_cast<void**>(&g_origFreeShortUsage));
        MH_EnableHook(s_targetFreeShort);
    }
    if (s_targetAllocShortUnsync) {
        MH_CreateHook(s_targetAllocShortUnsync, reinterpret_cast<void*>(&Hook_MemHeap_AllocShortUsageUnsynced),
                      reinterpret_cast<void**>(&g_origAllocShortUsageUnsynced));
        MH_EnableHook(s_targetAllocShortUnsync);
    }
    if (s_targetFreeShortUnsync) {
        MH_CreateHook(s_targetFreeShortUnsync, reinterpret_cast<void*>(&Hook_MemHeap_FreeShortUsageUnsynced),
                      reinterpret_cast<void**>(&g_origFreeShortUsageUnsynced));
        MH_EnableHook(s_targetFreeShortUnsync);
    }

    if (mode >= 2) {
        // Mode 2: Full MemHeap replacement with Safe Dispatch Free
        s_targetAlloc = reinterpret_cast<void*>(GetProcAddress(hSharedBase, "?Alloc@MemHeap@@SGPAXI@Z"));
        s_targetFree = reinterpret_cast<void*>(GetProcAddress(hSharedBase, "?Free@MemHeap@@SGXPAX@Z"));
        s_targetReAlloc = reinterpret_cast<void*>(GetProcAddress(hSharedBase, "?ReAlloc@MemHeap@@SGPAXPAXI@Z"));

        s_targetAllocUnsync = reinterpret_cast<void*>(GetProcAddress(hSharedBase, "?AllocUnsynced@MemHeap@@SGPAXI@Z"));
        s_targetFreeUnsync = reinterpret_cast<void*>(GetProcAddress(hSharedBase, "?FreeUnsynced@MemHeap@@SGXPAX@Z"));
        s_targetReAllocUnsync = reinterpret_cast<void*>(GetProcAddress(hSharedBase, "?ReAllocUnsynced@MemHeap@@SGPAXPAXI@Z"));

        if (s_targetAlloc) {
            MH_CreateHook(s_targetAlloc, reinterpret_cast<void*>(&Hook_MemHeap_Alloc),
                          reinterpret_cast<void**>(&g_origAlloc));
            MH_EnableHook(s_targetAlloc);
        }
        if (s_targetFree) {
            MH_CreateHook(s_targetFree, reinterpret_cast<void*>(&Hook_MemHeap_Free),
                          reinterpret_cast<void**>(&g_origFreeHeap));
            MH_EnableHook(s_targetFree);
        }
        if (s_targetReAlloc) {
            MH_CreateHook(s_targetReAlloc, reinterpret_cast<void*>(&Hook_MemHeap_ReAlloc),
                          reinterpret_cast<void**>(&g_origReAllocHeap));
            MH_EnableHook(s_targetReAlloc);
        }

        if (s_targetAllocUnsync) {
            MH_CreateHook(s_targetAllocUnsync, reinterpret_cast<void*>(&Hook_MemHeap_AllocUnsynced),
                          reinterpret_cast<void**>(&g_origAllocUnsynced));
            MH_EnableHook(s_targetAllocUnsync);
        }
        if (s_targetFreeUnsync) {
            MH_CreateHook(s_targetFreeUnsync, reinterpret_cast<void*>(&Hook_MemHeap_FreeUnsynced),
                          reinterpret_cast<void**>(&g_origFreeUnsynced));
            MH_EnableHook(s_targetFreeUnsync);
        }
        if (s_targetReAllocUnsync) {
            MH_CreateHook(s_targetReAllocUnsync, reinterpret_cast<void*>(&Hook_MemHeap_ReAllocUnsynced),
                          reinterpret_cast<void**>(&g_origReAllocUnsynced));
            MH_EnableHook(s_targetReAllocUnsync);
        }

        Log("G3O", "MemoryAllocator: AllocatorMode=2 (Full MemHeap + Safe Dispatch Free active).");
    } else {
        Log("G3O", "MemoryAllocator: AllocatorMode=1 (ShortUsage / ScrapHeap mimalloc replacement active).");
    }

    s_hooksInstalled = true;
    return true;
}

void ShutdownMemoryAllocator() {
    if (!s_hooksInstalled) return;

    void* hooks[] = {
        s_targetAllocShort, s_targetFreeShort, s_targetAllocShortUnsync, s_targetFreeShortUnsync,
        s_targetAlloc, s_targetFree, s_targetReAlloc,
        s_targetAllocUnsync, s_targetFreeUnsync, s_targetReAllocUnsync
    };

    for (void* target : hooks) {
        if (target) {
            MH_DisableHook(target);
            MH_RemoveHook(target);
        }
    }

    s_targetAllocShort = nullptr;
    s_targetFreeShort = nullptr;
    s_targetAllocShortUnsync = nullptr;
    s_targetFreeShortUnsync = nullptr;
    s_targetAlloc = nullptr;
    s_targetFree = nullptr;
    s_targetReAlloc = nullptr;
    s_targetAllocUnsync = nullptr;
    s_targetFreeUnsync = nullptr;
    s_targetReAllocUnsync = nullptr;

    g_origAlloc = nullptr;
    g_origFreeHeap = nullptr;
    g_origReAllocHeap = nullptr;
    g_origAllocUnsynced = nullptr;
    g_origFreeUnsynced = nullptr;
    g_origReAllocUnsynced = nullptr;
    g_origAllocShortUsage = nullptr;
    g_origFreeShortUsage = nullptr;
    g_origAllocShortUsageUnsynced = nullptr;
    g_origFreeShortUsageUnsynced = nullptr;

    s_hooksInstalled = false;

    Log("G3O", "MemoryAllocator: shutdown stats: mi_allocs=%llu, mi_frees=%llu, legacy_frees=%llu, mi_reallocs=%llu, legacy_reallocs=%llu.",
        static_cast<unsigned long long>(GetMiAllocCount()),
        static_cast<unsigned long long>(GetMiFreeCount()),
        static_cast<unsigned long long>(GetLegacyFreeCount()),
        static_cast<unsigned long long>(GetMiReallocCount()),
        static_cast<unsigned long long>(GetLegacyReallocCount()));
}

} // namespace G3O
