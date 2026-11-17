#include "static_slab_arena.h"
#include <cstdio>
#include <cassert>
#include <vector>

namespace G3O {
void Log(const char*, const char*, ...) {}
}

static bool Expect(bool cond, const char* msg) {
    if (cond) return true;
    std::fprintf(stderr, "FAIL: %s\n", msg);
    return false;
}

int main() {
    G3O::StaticBoundedSlabArena arena;
    bool ok = true;

    ok &= Expect(!arena.IsInitialized(), "arena uninitialized before Initialize()");
    ok &= Expect(arena.Initialize(), "arena initializes successfully");
    ok &= Expect(arena.IsInitialized(), "arena is initialized");
    ok &= Expect(arena.GetTotalArenaBytes() == 72 * 1024 * 1024, "total arena is 72 MB");

    // Size classification
    ok &= Expect(G3O::StaticBoundedSlabArena::ClassifySize(1024) == G3O::SlabTier::Small, "1 KB is Small");
    ok &= Expect(G3O::StaticBoundedSlabArena::ClassifySize(64 * 1024) == G3O::SlabTier::Small, "64 KB is Small");
    ok &= Expect(G3O::StaticBoundedSlabArena::ClassifySize(64 * 1024 + 1) == G3O::SlabTier::Medium, "65 KB is Medium");
    ok &= Expect(G3O::StaticBoundedSlabArena::ClassifySize(512 * 1024) == G3O::SlabTier::Medium, "512 KB is Medium");
    ok &= Expect(G3O::StaticBoundedSlabArena::ClassifySize(512 * 1024 + 1) == G3O::SlabTier::Large, "513 KB is Large");
    ok &= Expect(G3O::StaticBoundedSlabArena::ClassifySize(4 * 1024 * 1024) == G3O::SlabTier::Large, "4 MB is Large");
    ok &= Expect(G3O::StaticBoundedSlabArena::ClassifySize(4 * 1024 * 1024 + 1) == G3O::SlabTier::Oversize, ">4 MB is Oversize");

    // Acquire and release single slot
    auto hSmall = arena.TryAcquire(32 * 1024);
    ok &= Expect(hSmall.ptr != nullptr, "acquire small slot");
    ok &= Expect(hSmall.tier == G3O::SlabTier::Small, "tier is Small");
    arena.Release(hSmall);

    auto hMed = arena.TryAcquire(256 * 1024);
    ok &= Expect(hMed.ptr != nullptr, "acquire medium slot");
    ok &= Expect(hMed.tier == G3O::SlabTier::Medium, "tier is Medium");
    arena.Release(hMed);

    auto hLarge = arena.TryAcquire(2 * 1024 * 1024);
    ok &= Expect(hLarge.ptr != nullptr, "acquire large slot");
    ok &= Expect(hLarge.tier == G3O::SlabTier::Large, "tier is Large");
    arena.Release(hLarge);

    // Acquire all 8 large slots
    std::vector<G3O::SlabHandle> largeHandles;
    for (int i = 0; i < 8; ++i) {
        auto h = arena.TryAcquire(4 * 1024 * 1024);
        ok &= Expect(h.ptr != nullptr, "acquire large slot within limit");
        largeHandles.push_back(h);
    }
    // 9th large slot must return null (exhausted)
    auto hExhausted = arena.TryAcquire(4 * 1024 * 1024);
    ok &= Expect(hExhausted.ptr == nullptr, "9th large slot rejected");

    // Release 1 and re-acquire
    arena.Release(largeHandles.back());
    largeHandles.pop_back();

    auto hReacquire = arena.TryAcquire(4 * 1024 * 1024);
    ok &= Expect(hReacquire.ptr != nullptr, "re-acquire released large slot");
    largeHandles.push_back(hReacquire);

    for (const auto& h : largeHandles) {
        arena.Release(h);
    }

    // ResetAll test
    arena.ResetAll();

    // Shutdown
    arena.Shutdown();
    ok &= Expect(!arena.IsInitialized(), "shutdown clears initialization");

    if (ok) {
        std::printf("PASS: StaticBoundedSlabArena tests passed.\n");
        return 0;
    } else {
        std::printf("FAIL: Some StaticBoundedSlabArena tests failed.\n");
        return 1;
    }
}
