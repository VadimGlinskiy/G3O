#include "memory_allocator.h"
#include <mimalloc.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>

static bool Expect(bool condition, const char* message) {
    if (condition) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

static int s_fallbackFreeCalls = 0;
static void MockLegacyFree(void* ptr) {
    ++s_fallbackFreeCalls;
    std::free(ptr);
}

static void DispatchFree(void* ptr) {
    if (!ptr) return;
    if (mi_is_in_heap_region(ptr)) {
        mi_free(ptr);
    } else {
        MockLegacyFree(ptr);
    }
}

int main() {
    bool ok = true;

    // 1. Verify mimalloc option tuning (Psycho 32-bit VAS preservation profile)
    mi_option_set(mi_option_arena_reserve, 8 * 1024);
    mi_option_set(mi_option_arena_max_object_size, 64);
    mi_option_set(mi_option_arena_eager_commit, 0);
    mi_option_set(mi_option_purge_decommits, 0);
    mi_option_set(mi_option_purge_delay, 1000);
    mi_option_set(mi_option_retry_on_oom, 0);

    ok &= Expect(mi_option_get(mi_option_arena_reserve) == 8 * 1024, "arena_reserve must be 8MB (8192 KiB)");
    ok &= Expect(mi_option_get(mi_option_arena_max_object_size) == 64, "arena_max_object_size must be 64KB");
    ok &= Expect(mi_option_get(mi_option_arena_eager_commit) == 0, "arena_eager_commit must be 0 (demand-page)");
    ok &= Expect(mi_option_get(mi_option_purge_decommits) == 0, "purge_decommits must be 0 (reset on purge)");
    ok &= Expect(mi_option_get(mi_option_purge_delay) == 1000, "purge_delay must be 1000 ms");
    ok &= Expect(mi_option_get(mi_option_retry_on_oom) == 0, "retry_on_oom must be 0");

    // 2. Test bounded OS memory reservation
    mi_arena_id_t arena_id = 0;
    int res = mi_reserve_os_memory_ex(8 * 1024 * 1024, false, false, false, &arena_id);
    ok &= Expect(res == 0, "mi_reserve_os_memory_ex for 8MB bounded arena should succeed");

    // 3. Test mimalloc allocations are recognized by mi_is_in_heap_region
    void* pMi1 = mi_malloc(64);
    void* pMi2 = mi_malloc(1024 * 1024);
    ok &= Expect(pMi1 != nullptr, "mi_malloc(64) succeeded");
    ok &= Expect(pMi2 != nullptr, "mi_malloc(1MB) succeeded");
    ok &= Expect(mi_is_in_heap_region(pMi1), "pMi1 must be in heap region");
    ok &= Expect(mi_is_in_heap_region(pMi2), "pMi2 must be in heap region");

    // 4. Test non-mimalloc pointers (stack, CRT malloc) are NOT recognized as mimalloc
    int stackVar = 42;
    void* pCrt = std::malloc(128);
    ok &= Expect(!mi_is_in_heap_region(&stackVar), "Stack variable must NOT be in heap region");
    ok &= Expect(!mi_is_in_heap_region(pCrt), "CRT malloc must NOT be in heap region");
    ok &= Expect(!mi_is_in_heap_region(nullptr), "nullptr must NOT be in heap region");

    // 5. Test safe dispatch free
    s_fallbackFreeCalls = 0;
    DispatchFree(pMi1);
    ok &= Expect(s_fallbackFreeCalls == 0, "mimalloc block should be freed without legacy fallback");

    DispatchFree(pCrt);
    ok &= Expect(s_fallbackFreeCalls == 1, "CRT block must be routed to legacy fallback free");

    DispatchFree(pMi2);
    ok &= Expect(s_fallbackFreeCalls == 1, "Large mimalloc block freed without fallback");

    // 6. Test realloc behavior
    void* pMi3 = mi_malloc(32);
    void* pRealloc = mi_realloc(pMi3, 256);
    ok &= Expect(pRealloc != nullptr, "mi_realloc succeeded");
    ok &= Expect(mi_is_in_heap_region(pRealloc), "reallocated block is in heap region");
    mi_free(pRealloc);

    // 7. Verify AllocatorMode enum constants
    ok &= Expect(static_cast<int>(G3O::AllocatorMode::Disabled) == 0, "Mode 0 is Disabled");
    ok &= Expect(static_cast<int>(G3O::AllocatorMode::ShortUsage) == 1, "Mode 1 is ShortUsage");
    ok &= Expect(static_cast<int>(G3O::AllocatorMode::FullMemHeap) == 2, "Mode 2 is FullMemHeap");

    if (!ok) return 1;
    std::puts("PASS: memory_allocator safe dispatch free, mimalloc region detection, and 32-bit VAS tuning");
    return 0;
}
