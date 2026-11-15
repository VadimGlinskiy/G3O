#include "memory_allocator.h"
#include "diagnostic_logger.h"

#include <windows.h>
#include <intrin.h>
#include <mimalloc.h>
#include <MinHook.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <algorithm>

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
using PFN_MemHeap_Alloc   = void* (__stdcall*)(unsigned int size);
using PFN_MemHeap_Free    = void  (__stdcall*)(void* ptr);
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
// G3O Zombie-Safe Header & Lock-Free Quarantine Architecture
// =============================================================================
namespace {

constexpr uint32_t G3_HEADER_CANARY       = 0x47334F21u; // "G3O!"
constexpr long     G3_STATE_ALIVE         = 0x414C4956L; // 'ALIV'
constexpr long     G3_STATE_QUARANTINED   = 0x51554152L; // 'QUAR'
constexpr long     G3_STATE_FREED         = 0x46524545L; // 'FREE'

// Matches Vanilla Gothic 3's s_arrSimpleAllocs slab threshold (<= 4096 bytes).
// 65,536 slots hold recently freed blocks out of mimalloc's freelist so stale
// readers, zombie writers, and concurrent bCString::Release races cannot corrupt
// mimalloc's in-band freelist or trigger ABA reuse collisions.
constexpr size_t   QUARANTINE_MAX_SIZE    = 4096u;
constexpr uint32_t QUARANTINE_CAPACITY    = 65536u; // Power of 2
constexpr uint32_t QUARANTINE_MASK        = QUARANTINE_CAPACITY - 1u;
constexpr size_t   MAX_SANE_ALLOC_SIZE    = 0x70000000u; // 1.75 GB sanity bound

struct alignas(16) G3BlockHeader {
    uintptr_t     mi_freelist_slot; // [base + 0x00]: Overwritten by mimalloc ONLY on mi_free(base)
    uint32_t      user_size;        // [base + 0x04]: Exact byte size requested by the engine
    uint32_t      canary;           // [base + 0x08]: Must equal G3_HEADER_CANARY (0x47334F21)
    volatile long state;            // [base + 0x0C]: Atomic lifecycle state (ALIVE / QUARANTINED / FREED)
};
static_assert(sizeof(G3BlockHeader) == 16, "G3BlockHeader must be exactly 16 bytes for SSE alignment.");

alignas(64) static void* volatile g_quarantineRing[QUARANTINE_CAPACITY] = {};
alignas(64) static volatile long  g_quarantineCursor = 0;

// Helper to check if two addresses reside on the same 4 KB x86 page (avoids page-fault on probe)
__forceinline bool IsSameMemoryPage(uintptr_t addrA, uintptr_t addrB) noexcept {
    return (addrA ^ addrB) < 4096u;
}

// Validates whether a candidate header pointer is a genuine G3BlockHeader
__forceinline bool IsValidHeader(const G3BlockHeader* hdr) noexcept {
    if (!hdr) return false;
    if (hdr->canary != G3_HEADER_CANARY) return false;
    const long st = hdr->state;
    return (st == G3_STATE_ALIVE || st == G3_STATE_QUARANTINED || st == G3_STATE_FREED) &&
           (hdr->user_size <= MAX_SANE_ALLOC_SIZE);
}

// Resolves a user pointer (or an interior pointer such as bCString::m_pString at user_ptr + 8)
// back to its owning G3BlockHeader.
__forceinline G3BlockHeader* ResolveBlockHeader(void* ptr) noexcept {
    const uintptr_t rawPtr = reinterpret_cast<uintptr_t>(ptr);
    if (rawPtr <= sizeof(G3BlockHeader)) return nullptr;

    // Fast Path: Standard base user_ptr (base + 16)
    G3BlockHeader* directHdr = reinterpret_cast<G3BlockHeader*>(rawPtr - sizeof(G3BlockHeader));
    if (IsValidHeader(directHdr)) {
        return directHdr;
    }

    // Slow Path: Interior pointer recovery (e.g., bCString::m_pString passed directly at user_ptr + 8,
    // or C++ multiple-inheritance subobject offset). Scan backward in 4-byte steps within the same
    // 4 KB memory page up to 64 bytes so we cannot cross into an unmapped page.
    const uintptr_t alignedPtr = rawPtr & ~uintptr_t(3u);
    for (uintptr_t offset = 4u; offset <= 64u; offset += 4u) {
        if (alignedPtr <= sizeof(G3BlockHeader) + offset) break;
        const uintptr_t candidateAddr = alignedPtr - sizeof(G3BlockHeader) - offset;
        if (!IsSameMemoryPage(rawPtr, candidateAddr)) break;

        G3BlockHeader* candidate = reinterpret_cast<G3BlockHeader*>(candidateAddr);
        if (candidate->canary == G3_HEADER_CANARY) {
            const long st = candidate->state;
            if ((st == G3_STATE_ALIVE || st == G3_STATE_QUARANTINED) &&
                offset <= candidate->user_size &&
                candidate->user_size <= MAX_SANE_ALLOC_SIZE) {
                return candidate;
            }
        }
    }

    return nullptr;
}

// ============================================================================
// Core Protected Allocator Operations
// ============================================================================

__forceinline void* InternalAlloc(unsigned int size) noexcept {
    // Vanilla MemHeap::Alloc(0) indexes s_arrSimpleAllocs[0] and returns a valid non-null block.
    const uint32_t effectiveSize = (size == 0u) ? 4u : size;
    const size_t totalBytes = sizeof(G3BlockHeader) + static_cast<size_t>(effectiveSize);

    G3BlockHeader* hdr = static_cast<G3BlockHeader*>(mi_malloc(totalBytes));
    if (!hdr) {
        return nullptr;
    }

    hdr->mi_freelist_slot = 0u;
    hdr->user_size        = effectiveSize;
    hdr->canary           = G3_HEADER_CANARY;
    _InterlockedExchange(&hdr->state, G3_STATE_ALIVE);

    s_miAllocCount.fetch_add(1, std::memory_order_relaxed);
    return static_cast<void*>(hdr + 1);
}

__forceinline void InternalFree(void* ptr, PFN_MemHeap_Free origFallbackFree) noexcept {
    if (!ptr) return;

    if (mi_is_in_heap_region(ptr)) {
        __try {
            G3BlockHeader* hdr = ResolveBlockHeader(ptr);
            if (!hdr) {
                // Pointer is inside mimalloc's virtual address space but has no valid header
                // (corrupt or unowned pointer). Do NOT pass to mi_free or Windows HeapFree.
                return;
            }

            // Atomically transition from ALIVE -> QUARANTINED.
            // If another thread raced us (e.g., non-atomic bCString::Release refcount bug)
            // or this is a double-free, _InterlockedCompareExchange fails and we safely no-op.
            const long prevState = _InterlockedCompareExchange(&hdr->state, G3_STATE_QUARANTINED, G3_STATE_ALIVE);
            if (prevState != G3_STATE_ALIVE) {
                return;
            }

            s_miFreeCount.fetch_add(1, std::memory_order_relaxed);

            if (hdr->user_size <= QUARANTINE_MAX_SIZE) {
                // Push into lock-free Quarantine Ring Buffer; evict and mi_free the oldest entry.
                const uint32_t slot = static_cast<uint32_t>(_InterlockedIncrement(&g_quarantineCursor)) & QUARANTINE_MASK;
                G3BlockHeader* evicted = reinterpret_cast<G3BlockHeader*>(
                    _InterlockedExchange(
                        reinterpret_cast<volatile long*>(&g_quarantineRing[slot]),
                        reinterpret_cast<long>(hdr)
                    )
                );
                if (evicted) {
                    _InterlockedExchange(&evicted->state, G3_STATE_FREED);
                    mi_free(evicted);
                }
            } else {
                // Large blocks (> 4096 bytes) still benefit from the 16-byte header offset
                // ([base + 0] isolated from [user_ptr + 0]) and atomic double-free protection.
                _InterlockedExchange(&hdr->state, G3_STATE_FREED);
                mi_free(hdr);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        return;
    }

    // Pre-hook allocation owned by Vanilla MemHeap (s_arrSimpleAllocs or s_hHeap)
    s_origFreeCount.fetch_add(1, std::memory_order_relaxed);
    if (origFallbackFree) {
        __try {
            origFallbackFree(ptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
}

__forceinline void* InternalReAlloc(
    void* ptr,
    unsigned int size,
    PFN_MemHeap_ReAlloc origFallbackReAlloc,
    PFN_MemHeap_Free origFallbackFree) noexcept
{
    if (!ptr) {
        return InternalAlloc(size);
    }
    if (size == 0u) {
        InternalFree(ptr, origFallbackFree);
        return nullptr;
    }

    if (mi_is_in_heap_region(ptr)) {
        __try {
            G3BlockHeader* hdr = ResolveBlockHeader(ptr);
            if (!hdr) {
                return nullptr;
            }

            const long currentState = hdr->state;
            if (currentState != G3_STATE_ALIVE && currentState != G3_STATE_QUARANTINED) {
                return nullptr;
            }

            const uint32_t oldSize = hdr->user_size;
            void* const baseUserPtr = static_cast<void*>(hdr + 1);

            // Fast in-place path: if the block is ALIVE and fits within the existing mimalloc
            // size class without wasting more than 50% capacity, update user_size in O(1).
            if (currentState == G3_STATE_ALIVE) {
                const size_t usableTotal = mi_usable_size(hdr);
                const size_t usableUser  = (usableTotal > sizeof(G3BlockHeader))
                                             ? (usableTotal - sizeof(G3BlockHeader))
                                             : static_cast<size_t>(oldSize);
                if (size <= usableUser && size >= (oldSize >> 1u)) {
                    hdr->user_size = size;
                    s_miReallocCount.fetch_add(1, std::memory_order_relaxed);
                    return baseUserPtr;
                }
            }

            // Out-of-place reallocation: allocate new block, copy payload, and send old block
            // through InternalFree (Quarantine) rather than mi_realloc so concurrent readers
            // or self-referential array appends never read invalidated memory.
            void* newPtr = InternalAlloc(size);
            if (!newPtr) {
                return nullptr;
            }

            const size_t copyBytes = (oldSize < size) ? oldSize : size;
            if (copyBytes > 0u) {
                std::memcpy(newPtr, baseUserPtr, copyBytes);
            }

            if (currentState == G3_STATE_ALIVE) {
                InternalFree(baseUserPtr, origFallbackFree);
            }
            s_miReallocCount.fetch_add(1, std::memory_order_relaxed);
            return newPtr;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    // Legacy Vanilla MemHeap block allocated before hooks were installed
    s_origReallocCount.fetch_add(1, std::memory_order_relaxed);
    if (origFallbackReAlloc) {
        __try {
            return origFallbackReAlloc(ptr, size);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }
    return nullptr;
}

// Drain Quarantine ring buffer cleanly on shutdown
static void DrainQuarantine() noexcept {
    for (uint32_t i = 0; i < QUARANTINE_CAPACITY; ++i) {
        G3BlockHeader* evicted = reinterpret_cast<G3BlockHeader*>(
            _InterlockedExchange(
                reinterpret_cast<volatile long*>(&g_quarantineRing[i]),
                0
            )
        );
        if (evicted) {
            _InterlockedExchange(&evicted->state, G3_STATE_FREED);
            mi_free(evicted);
        }
    }
}

} // anonymous namespace

// =============================================================================
// Hook Callbacks: Synced MemHeap (Mode 2)
// =============================================================================
static void* __stdcall Hook_MemHeap_Alloc(unsigned int size) {
    void* ptr = InternalAlloc(size);
    if (ptr) return ptr;
    return g_origAlloc ? g_origAlloc(size) : nullptr;
}

static void __stdcall Hook_MemHeap_Free(void* ptr) {
    InternalFree(ptr, g_origFreeHeap);
}

static void* __stdcall Hook_MemHeap_ReAlloc(void* ptr, unsigned int size) {
    return InternalReAlloc(ptr, size, g_origReAllocHeap, g_origFreeHeap);
}

// =============================================================================
// Hook Callbacks: Unsynced MemHeap (Mode 2)
// =============================================================================
static void* __stdcall Hook_MemHeap_AllocUnsynced(unsigned int size) {
    void* ptr = InternalAlloc(size);
    if (ptr) return ptr;
    return g_origAllocUnsynced ? g_origAllocUnsynced(size) : nullptr;
}

static void __stdcall Hook_MemHeap_FreeUnsynced(void* ptr) {
    InternalFree(ptr, g_origFreeUnsynced ? g_origFreeUnsynced : g_origFreeHeap);
}

static void* __stdcall Hook_MemHeap_ReAllocUnsynced(void* ptr, unsigned int size) {
    return InternalReAlloc(
        ptr,
        size,
        g_origReAllocUnsynced ? g_origReAllocUnsynced : g_origReAllocHeap,
        g_origFreeUnsynced    ? g_origFreeUnsynced    : g_origFreeHeap
    );
}

// =============================================================================
// Hook Callbacks: ShortUsage / ScrapHeap (Mode 1 & Mode 2)
// Uses the same G3BlockHeader format so cross-pool frees between ShortUsage
// and general MemHeap are 100% compatible.
// =============================================================================
static void* __stdcall Hook_MemHeap_AllocShortUsage(unsigned int size) {
    void* ptr = InternalAlloc(size);
    if (ptr) return ptr;
    return g_origAllocShortUsage ? g_origAllocShortUsage(size) : nullptr;
}

static void __stdcall Hook_MemHeap_FreeShortUsage(void* ptr) {
    InternalFree(ptr, g_origFreeShortUsage ? g_origFreeShortUsage : g_origFreeHeap);
}

static void* __stdcall Hook_MemHeap_AllocShortUsageUnsynced(unsigned int size) {
    void* ptr = InternalAlloc(size);
    if (ptr) return ptr;
    return g_origAllocShortUsageUnsynced ? g_origAllocShortUsageUnsynced(size) : nullptr;
}

static void __stdcall Hook_MemHeap_FreeShortUsageUnsynced(void* ptr) {
    InternalFree(ptr, g_origFreeShortUsageUnsynced ? g_origFreeShortUsageUnsynced : g_origFreeShortUsage);
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
            Log("G3O", "MemoryAllocator: mimalloc bounded arena reserved: %zu MB (id %p).",
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

    // 1. Resolve target entrypoints
    // ShortUsage hooks (Mode 1 & Mode 2)
    s_targetAllocShort = reinterpret_cast<void*>(GetProcAddress(hSharedBase, "?AllocShortUsage@MemHeap@@SGPAXI@Z"));
    s_targetFreeShort = reinterpret_cast<void*>(GetProcAddress(hSharedBase, "?FreeShortUsage@MemHeap@@SGXPAX@Z"));
    s_targetAllocShortUnsync = reinterpret_cast<void*>(GetProcAddress(hSharedBase, "?AllocShortUsageUnsynced@MemHeap@@SGPAXI@Z"));
    s_targetFreeShortUnsync = reinterpret_cast<void*>(GetProcAddress(hSharedBase, "?FreeShortUsageUnsynced@MemHeap@@SGXPAX@Z"));

    if (mode >= 2) {
        s_targetAlloc = reinterpret_cast<void*>(GetProcAddress(hSharedBase, "?Alloc@MemHeap@@SGPAXI@Z"));
        s_targetFree = reinterpret_cast<void*>(GetProcAddress(hSharedBase, "?Free@MemHeap@@SGXPAX@Z"));
        s_targetReAlloc = reinterpret_cast<void*>(GetProcAddress(hSharedBase, "?ReAlloc@MemHeap@@SGPAXPAXI@Z"));
        s_targetAllocUnsync = reinterpret_cast<void*>(GetProcAddress(hSharedBase, "?AllocUnsynced@MemHeap@@SGPAXI@Z"));
        s_targetFreeUnsync = reinterpret_cast<void*>(GetProcAddress(hSharedBase, "?FreeUnsynced@MemHeap@@SGXPAX@Z"));
        s_targetReAllocUnsync = reinterpret_cast<void*>(GetProcAddress(hSharedBase, "?ReAllocUnsynced@MemHeap@@SGPAXPAXI@Z"));
    }

    // 2. Create hooks without enabling yet
    if (s_targetFreeShort) {
        MH_CreateHook(s_targetFreeShort, reinterpret_cast<void*>(&Hook_MemHeap_FreeShortUsage),
                      reinterpret_cast<void**>(&g_origFreeShortUsage));
    }
    if (s_targetFreeShortUnsync) {
        MH_CreateHook(s_targetFreeShortUnsync, reinterpret_cast<void*>(&Hook_MemHeap_FreeShortUsageUnsynced),
                      reinterpret_cast<void**>(&g_origFreeShortUsageUnsynced));
    }
    if (s_targetAllocShort) {
        MH_CreateHook(s_targetAllocShort, reinterpret_cast<void*>(&Hook_MemHeap_AllocShortUsage),
                      reinterpret_cast<void**>(&g_origAllocShortUsage));
    }
    if (s_targetAllocShortUnsync) {
        MH_CreateHook(s_targetAllocShortUnsync, reinterpret_cast<void*>(&Hook_MemHeap_AllocShortUsageUnsynced),
                      reinterpret_cast<void**>(&g_origAllocShortUsageUnsynced));
    }

    if (mode >= 2) {
        if (s_targetFree) {
            MH_CreateHook(s_targetFree, reinterpret_cast<void*>(&Hook_MemHeap_Free),
                          reinterpret_cast<void**>(&g_origFreeHeap));
        }
        if (s_targetFreeUnsync) {
            MH_CreateHook(s_targetFreeUnsync, reinterpret_cast<void*>(&Hook_MemHeap_FreeUnsynced),
                          reinterpret_cast<void**>(&g_origFreeUnsynced));
        }
        if (s_targetReAlloc) {
            MH_CreateHook(s_targetReAlloc, reinterpret_cast<void*>(&Hook_MemHeap_ReAlloc),
                          reinterpret_cast<void**>(&g_origReAllocHeap));
        }
        if (s_targetReAllocUnsync) {
            MH_CreateHook(s_targetReAllocUnsync, reinterpret_cast<void*>(&Hook_MemHeap_ReAllocUnsynced),
                          reinterpret_cast<void**>(&g_origReAllocUnsynced));
        }
        if (s_targetAlloc) {
            MH_CreateHook(s_targetAlloc, reinterpret_cast<void*>(&Hook_MemHeap_Alloc),
                          reinterpret_cast<void**>(&g_origAlloc));
        }
        if (s_targetAllocUnsync) {
            MH_CreateHook(s_targetAllocUnsync, reinterpret_cast<void*>(&Hook_MemHeap_AllocUnsynced),
                          reinterpret_cast<void**>(&g_origAllocUnsynced));
        }
    }

    // 3. Queue enable all created allocator hooks
    void* activeTargets[] = {
        s_targetFreeShort, s_targetFreeShortUnsync,
        s_targetAllocShort, s_targetAllocShortUnsync,
        s_targetFree, s_targetFreeUnsync,
        s_targetReAlloc, s_targetReAllocUnsync,
        s_targetAlloc, s_targetAllocUnsync
    };

    for (void* target : activeTargets) {
        if (target) {
            MH_QueueEnableHook(target);
        }
    }

    // 4. Atomically apply all queued hooks simultaneously in one thread-freeze pass
    MH_STATUS status = MH_ApplyQueued();
    if (status != MH_OK) {
        Log("G3O", "MemoryAllocator: MH_ApplyQueued failed (%d); activating individually (Frees first).", status);
        // Fallback: strictly activate Free hooks before Alloc hooks
        for (void* target : activeTargets) {
            if (target) MH_EnableHook(target);
        }
    }

    if (mode >= 2) {
        Log("G3O", "MemoryAllocator: AllocatorMode=2 (Full MemHeap + Zombie-Safe Header & Quarantine Ring active).");
    } else {
        Log("G3O", "MemoryAllocator: AllocatorMode=1 (ShortUsage / ScrapHeap mimalloc replacement active).");
    }

    s_hooksInstalled = true;
    return true;
}

void ShutdownMemoryAllocator() {
    if (!s_hooksInstalled) return;

    void* hooks[] = {
        s_targetAllocShort, s_targetAllocShortUnsync,
        s_targetAlloc, s_targetAllocUnsync,
        s_targetReAlloc, s_targetReAllocUnsync,
        s_targetFreeShort, s_targetFreeShortUnsync,
        s_targetFree, s_targetFreeUnsync
    };

    for (void* target : hooks) {
        if (target) {
            MH_QueueDisableHook(target);
        }
    }
    MH_ApplyQueued();

    for (void* target : hooks) {
        if (target) {
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

    DrainQuarantine();

    Log("G3O", "MemoryAllocator: shutdown stats: mi_allocs=%llu, mi_frees=%llu, legacy_frees=%llu, mi_reallocs=%llu, legacy_reallocs=%llu.",
        static_cast<unsigned long long>(GetMiAllocCount()),
        static_cast<unsigned long long>(GetMiFreeCount()),
        static_cast<unsigned long long>(GetLegacyFreeCount()),
        static_cast<unsigned long long>(GetMiReallocCount()),
        static_cast<unsigned long long>(GetLegacyReallocCount()));
}

} // namespace G3O
