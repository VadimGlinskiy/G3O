#include "navigation_optimizer.h"
#include "config_manager.h"
#include "diagnostic_logger.h"
#include "savegame_profiler.h"
#include "hook_install_policy.h"
#include <minhook.h>
#include <windows.h>
#include <atomic>

namespace G3O {

using NavigationProcessFn = void (__thiscall*)(void* pThis);
static NavigationProcessFn Real_NavigationProcess = nullptr;
static void* s_targetNavProcess = nullptr;

static std::atomic<uint32_t> s_navSkippedTicks{0};
static std::atomic<uint32_t> s_navGameplayTicks{0};

static void __fastcall Hooked_NavigationProcess(void* pThis, void* /*edx*/) {
    // During savegame loading (while the loading screen is active and the world graph is being assembled),
    // all NPCs and entities are static.
    // Suppress intermediate, redundant pathfinding and nav-mesh collision sweeps during entity placement.
    if (IsSavegameLoadActive()) {
        s_navSkippedTicks.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Once loading completes and gameplay begins, execute 100% full vanilla navigation every frame.
    s_navGameplayTicks.fetch_add(1, std::memory_order_relaxed);
    Real_NavigationProcess(pThis);
}

uint32_t NavigationOptimizer_GetSkippedTicks() noexcept {
    return s_navSkippedTicks.load(std::memory_order_relaxed);
}

uint32_t NavigationOptimizer_GetGameplayTicks() noexcept {
    return s_navGameplayTicks.load(std::memory_order_relaxed);
}

void InitializeNavigationOptimizer() {
    if (!GetConfig().enableNavigationOptimizer) {
        Log("G3O", "Navigation Optimizer is disabled by configuration.");
        return;
    }
    // gCNavigationAdmin::Process is only invoked by the main loop during active gameplay frames
    // (never during modal savegame loading). Hooking it adds unnecessary per-frame detour overhead.
    // Retired to zero-overhead no-op.
    Log("G3O", "Navigation Optimizer: gCNavigationAdmin::Process hook retired (zero per-frame detour overhead).");
}

void ShutdownNavigationOptimizer() {
    if (s_targetNavProcess) {
        MH_DisableHook(s_targetNavProcess);
        MH_RemoveHook(s_targetNavProcess);
        s_targetNavProcess = nullptr;
        Real_NavigationProcess = nullptr;
    }
}

} // namespace G3O
