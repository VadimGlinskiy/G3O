#include "savegame_profiler_policy.h"
#include <cstdio>

static bool Expect(bool condition, const char* message) {
    if (condition) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

int main() {
    G3O::Config config{};
    config.enableSavegameAudit = false;
    config.enableProfiler = false;
    config.enablePakPrefetch = false;
    bool ok = true;
    ok &= Expect(!G3O::ShouldInstallSavegameProfilerHooks(config), "all savegame hooks should be off when all related features are off");

    config.enableSavegameAudit = true;
    ok &= Expect(G3O::ShouldInstallSavegameProfilerHooks(config) && G3O::ShouldCollectSavegameAudit(config),
                 "audit should install hooks and produce its detailed report independently");

    config.enableSavegameAudit = false;
    config.enableProfiler = true;
    ok &= Expect(G3O::ShouldInstallSavegameProfilerHooks(config) && !G3O::ShouldCollectSavegameAudit(config),
                 "general profiling should not imply the savegame audit report");

    config.enableProfiler = false;
    config.enablePakPrefetch = true;
    config.enableFastPakDecompressor = false;
    ok &= Expect(!G3O::ShouldInstallSavegameProfilerHooks(config) && !G3O::ShouldTracePakPrefetch(config),
                 "trace-based pre-decompression must stay off without the matching decompressor hook");

    config.enableFastPakDecompressor = true;
    ok &= Expect(G3O::ShouldInstallSavegameProfilerHooks(config) && G3O::ShouldTracePakPrefetch(config),
                 "trace-based prefetch should remain available without profiler or audit");

    if (!ok) return 1;
    std::puts("PASS: savegame audit, profiler, and PAK prefetch remain independently gated");
    return 0;
}
