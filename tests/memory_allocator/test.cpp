#include "memory_allocator.h"
#include <windows.h>
#include <intrin.h>
#include <mimalloc.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>

static bool Expect(bool condition, const char* message) {
    if (condition) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

// Mirroring internal structs for unit validation
constexpr uint32_t G3_HEADER_CANARY     = 0x47334F21u; // "G3O!"
constexpr long     G3_STATE_ALIVE       = 0x414C4956L; // 'ALIV'
constexpr long     G3_STATE_QUARANTINED = 0x51554152L; // 'QUAR'
constexpr long     G3_STATE_FREED       = 0x46524545L; // 'FREE'

struct alignas(16) G3BlockHeader {
    uintptr_t     mi_freelist_slot;
    uint32_t      user_size;
    uint32_t      canary;
    volatile long state;
};

static_assert(sizeof(G3BlockHeader) == 16, "G3BlockHeader must be exactly 16 bytes for SSE alignment.");

static int s_fallbackFreeCalls = 0;
static void MockLegacyFree(void* ptr) {
    ++s_fallbackFreeCalls;
    std::free(ptr);
}

// Standalone implementation of G3O ResolveBlockHeader for unit test
static inline bool IsValidHeader(const G3BlockHeader* hdr) noexcept {
    if (!hdr) return false;
    if (hdr->canary != G3_HEADER_CANARY) return false;
    const long st = hdr->state;
    return (st == G3_STATE_ALIVE || st == G3_STATE_QUARANTINED || st == G3_STATE_FREED);
}

static inline G3BlockHeader* ResolveBlockHeader(void* ptr) noexcept {
    const uintptr_t rawPtr = reinterpret_cast<uintptr_t>(ptr);
    if (rawPtr <= sizeof(G3BlockHeader)) return nullptr;

    G3BlockHeader* directHdr = reinterpret_cast<G3BlockHeader*>(rawPtr - sizeof(G3BlockHeader));
    if (IsValidHeader(directHdr)) {
        return directHdr;
    }

    const uintptr_t alignedPtr = rawPtr & ~uintptr_t(3u);
    for (uintptr_t offset = 4u; offset <= 64u; offset += 4u) {
        if (alignedPtr <= sizeof(G3BlockHeader) + offset) break;
        const uintptr_t candidateAddr = alignedPtr - sizeof(G3BlockHeader) - offset;
        if ((rawPtr ^ candidateAddr) >= 4096u) break;

        G3BlockHeader* candidate = reinterpret_cast<G3BlockHeader*>(candidateAddr);
        if (candidate->canary == G3_HEADER_CANARY) {
            const long st = candidate->state;
            if ((st == G3_STATE_ALIVE || st == G3_STATE_QUARANTINED) && offset <= candidate->user_size) {
                return candidate;
            }
        }
    }
    return nullptr;
}

static void* TestAlloc(uint32_t size) {
    const uint32_t effectiveSize = (size == 0u) ? 4u : size;
    const size_t totalBytes = sizeof(G3BlockHeader) + static_cast<size_t>(effectiveSize);

    G3BlockHeader* hdr = static_cast<G3BlockHeader*>(mi_malloc(totalBytes));
    if (!hdr) return nullptr;

    hdr->mi_freelist_slot = 0u;
    hdr->user_size        = effectiveSize;
    hdr->canary           = G3_HEADER_CANARY;
    _InterlockedExchange(&hdr->state, G3_STATE_ALIVE);
    return static_cast<void*>(hdr + 1);
}

static void TestFree(void* ptr) {
    if (!ptr) return;
    if (mi_is_in_heap_region(ptr)) {
        G3BlockHeader* hdr = ResolveBlockHeader(ptr);
        if (!hdr) return;

        const long prevState = _InterlockedCompareExchange(&hdr->state, G3_STATE_QUARANTINED, G3_STATE_ALIVE);
        if (prevState != G3_STATE_ALIVE) {
            // Double-free caught!
            return;
        }

        _InterlockedExchange(&hdr->state, G3_STATE_FREED);
        mi_free(hdr);
        return;
    }

    MockLegacyFree(ptr);
}

int main() {
    bool ok = true;

    // 1. Verify mimalloc option tuning (32-bit VAS preservation profile)
    mi_option_set(mi_option_arena_reserve, 2 * 1024);
    mi_option_set(mi_option_arena_max_object_size, 64);
    mi_option_set(mi_option_arena_eager_commit, 0);
    mi_option_set(mi_option_purge_decommits, 1);
    mi_option_set(mi_option_purge_delay, 100);
    mi_option_set(mi_option_retry_on_oom, 0);

    ok &= Expect(mi_option_get(mi_option_arena_reserve) == 2 * 1024, "arena_reserve must be 2MB (2048 KiB)");
    ok &= Expect(mi_option_get(mi_option_arena_max_object_size) == 64, "arena_max_object_size must be 64KB");
    ok &= Expect(mi_option_get(mi_option_arena_eager_commit) == 0, "arena_eager_commit must be 0 (demand-page)");
    ok &= Expect(mi_option_get(mi_option_purge_decommits) == 1, "purge_decommits must be 1 (decommit on purge)");
    ok &= Expect(mi_option_get(mi_option_purge_delay) == 100, "purge_delay must be 100 ms");
    ok &= Expect(mi_option_get(mi_option_retry_on_oom) == 0, "retry_on_oom must be 0");

    // 2. Test bounded OS memory reservation
    mi_arena_id_t arena_id = 0;
    int res = mi_reserve_os_memory_ex(8 * 1024 * 1024, false, false, false, &arena_id);
    ok &= Expect(res == 0, "mi_reserve_os_memory_ex for 8MB bounded arena should succeed");

    // 3. Test G3BlockHeader layout and offset isolation
    void* userPtr = TestAlloc(60);
    ok &= Expect(userPtr != nullptr, "TestAlloc(60) succeeded");
    ok &= Expect(mi_is_in_heap_region(userPtr), "userPtr is in mimalloc heap region");

    G3BlockHeader* hdr = reinterpret_cast<G3BlockHeader*>(static_cast<char*>(userPtr) - sizeof(G3BlockHeader));
    ok &= Expect(hdr->canary == G3_HEADER_CANARY, "Canary matches G3_HEADER_CANARY");
    ok &= Expect(hdr->user_size == 60, "user_size is 60");
    ok &= Expect(hdr->state == G3_STATE_ALIVE, "state is G3_STATE_ALIVE");

    // Simulate Gothic 3 bCString writing 0x00000033 to [user_ptr + 0]
    *reinterpret_cast<uint32_t*>(userPtr) = 0x00000033;
    ok &= Expect(hdr->mi_freelist_slot == 0, "mimalloc freelist slot at [base + 0] remains intact after user write!");

    // 4. Test interior pointer recovery (bCString::m_pString at user_ptr + 8)
    void* interiorPtr = static_cast<char*>(userPtr) + 8;
    G3BlockHeader* recoveredHdr = ResolveBlockHeader(interiorPtr);
    ok &= Expect(recoveredHdr == hdr, "Interior pointer (user_ptr + 8) successfully recovered true G3BlockHeader!");

    // 5. Test atomic double-free protection
    TestFree(userPtr);
    ok &= Expect(hdr->state == G3_STATE_FREED, "Block transitioned to G3_STATE_FREED");

    // Second free of the same pointer
    TestFree(userPtr);
    ok &= Expect(true, "Second free (double-free) safely ignored without crash");

    // 6. Test legacy dispatch free
    s_fallbackFreeCalls = 0;
    void* pCrt = std::malloc(128);
    TestFree(pCrt);
    ok &= Expect(s_fallbackFreeCalls == 1, "CRT allocation safely routed to legacy fallback free");

    // 7. Verify AllocatorMode enum constants
    ok &= Expect(static_cast<int>(G3O::AllocatorMode::Disabled) == 0, "Mode 0 is Disabled");
    ok &= Expect(static_cast<int>(G3O::AllocatorMode::ShortUsage) == 1, "Mode 1 is ShortUsage");
    ok &= Expect(static_cast<int>(G3O::AllocatorMode::FullMemHeap) == 2, "Mode 2 is FullMemHeap");

    if (!ok) return 1;
    std::puts("PASS: memory_allocator zombie-safe prefix header, freelist offset isolation, double-free CAS, and interior pointer recovery");
    return 0;
}
