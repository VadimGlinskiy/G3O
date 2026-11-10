#include <mimalloc.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>

static bool Expect(bool condition, const char* message) {
    if (condition) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

// Test the core decision logic that protects the engine against mixed heaps
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

    // 1. Verify mimalloc option tuning
    mi_option_set(mi_option_purge_delay, -1);
    mi_option_set(mi_option_purge_decommits, 0);
    ok &= Expect(mi_option_get(mi_option_purge_delay) == -1, "purge_delay must be -1 (no purging)");
    ok &= Expect(mi_option_get(mi_option_purge_decommits) == 0, "purge_decommits must be 0");

    // 2. Test mimalloc allocations are recognized by mi_is_in_heap_region
    void* pMi1 = mi_malloc(64);
    void* pMi2 = mi_malloc(1024 * 1024);
    ok &= Expect(pMi1 != nullptr, "mi_malloc(64) succeeded");
    ok &= Expect(pMi2 != nullptr, "mi_malloc(1MB) succeeded");
    ok &= Expect(mi_is_in_heap_region(pMi1), "pMi1 must be in heap region");
    ok &= Expect(mi_is_in_heap_region(pMi2), "pMi2 must be in heap region");

    // 3. Test non-mimalloc pointers (stack, CRT malloc) are NOT recognized as mimalloc
    int stackVar = 42;
    void* pCrt = std::malloc(128);
    ok &= Expect(!mi_is_in_heap_region(&stackVar), "Stack variable must NOT be in heap region");
    ok &= Expect(!mi_is_in_heap_region(pCrt), "CRT malloc must NOT be in heap region");

    // 4. Test safe dispatch free
    s_fallbackFreeCalls = 0;
    DispatchFree(pMi1);
    ok &= Expect(s_fallbackFreeCalls == 0, "mimalloc block should be freed without legacy fallback");

    DispatchFree(pCrt);
    ok &= Expect(s_fallbackFreeCalls == 1, "CRT block must be routed to legacy fallback free");

    DispatchFree(pMi2);
    ok &= Expect(s_fallbackFreeCalls == 1, "Large mimalloc block freed without fallback");

    // 5. Test realloc behavior
    void* pMi3 = mi_malloc(32);
    void* pRealloc = mi_realloc(pMi3, 256);
    ok &= Expect(pRealloc != nullptr, "mi_realloc succeeded");
    ok &= Expect(mi_is_in_heap_region(pRealloc), "reallocated block is in heap region");
    mi_free(pRealloc);

    if (!ok) return 1;
    std::puts("PASS: mimalloc heap region detection, safe dispatch free, and tuning verified.");
    return 0;
}
