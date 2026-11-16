#include "savegame_profiler_policy.h"
#include "hook_install_policy.h"
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
    config.enableParallelDynamicLayers = false;
    bool ok = true;
    ok &= Expect(!G3O::ShouldInstallSavegameProfilerHooks(config), "all savegame hooks should be off when all related features are off");

    config.enableLayerRingArena = true;
    ok &= Expect(G3O::ShouldInstallSavegameProfilerHooks(config),
                 "LayerRingArena should request sector lifecycle hooks without the profiler");
    config.enableFastPakDecompressor = false;
    ok &= Expect(!G3O::ShouldInstallSavegameProfilerHooks(config),
                 "LayerRingArena should not request hooks without the PAK decompressor");
    config.enableFastPakDecompressor = true;
    config.enableLayerRingArena = false;
    ok &= Expect(G3O::ResolveSamplePhaseForSector(0, "SysDyn_{fixture}") == G3O::kSysDynSamplePhase,
                 "SysDyn activation should use its isolated sample phase");
    ok &= Expect(G3O::ResolveSamplePhaseForSector(2, "WorldSector") == 2,
                 "other sectors should preserve the enclosing sample phase");
    ok &= Expect(G3O::ResolveSamplePhaseForSector(2, nullptr) == 2,
                 "unknown sector names should preserve the enclosing sample phase");

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

    int createCalls = 0;
    int enableCalls = 0;
    int removeCalls = 0;
    int clearCalls = 0;
    const auto createFailure = G3O::InstallHookSafely(
        [&] { ++createCalls; return false; },
        [&] { ++enableCalls; return true; },
        [&] { ++removeCalls; },
        [&] { ++clearCalls; });
    ok &= Expect(createFailure == G3O::HookInstallResult::CreateFailed &&
                 createCalls == 1 && enableCalls == 0 && removeCalls == 0 && clearCalls == 1,
                 "failed creation should not enable or remove a hook and should clear its trampoline");

    createCalls = enableCalls = removeCalls = clearCalls = 0;
    const auto enableFailure = G3O::InstallHookSafely(
        [&] { ++createCalls; return true; },
        [&] { ++enableCalls; return false; },
        [&] { ++removeCalls; },
        [&] { ++clearCalls; });
    ok &= Expect(enableFailure == G3O::HookInstallResult::EnableFailed &&
                 createCalls == 1 && enableCalls == 1 && removeCalls == 1 && clearCalls == 1,
                 "failed enable should remove the created hook and clear its trampoline");

    createCalls = enableCalls = removeCalls = clearCalls = 0;
    const auto installed = G3O::InstallHookSafely(
        [&] { ++createCalls; return true; },
        [&] { ++enableCalls; return true; },
        [&] { ++removeCalls; },
        [&] { ++clearCalls; });
    ok &= Expect(installed == G3O::HookInstallResult::Installed &&
                 createCalls == 1 && enableCalls == 1 && removeCalls == 0 && clearCalls == 0,
                 "successful hook installation should keep the hook and trampoline");

    if (!ok) return 1;
    std::puts("PASS: savegame audit, profiler, and PAK prefetch remain independently gated");
    return 0;
}
