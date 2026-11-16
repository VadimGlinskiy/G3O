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

    // Rate-limiting during gameplay:
    // Dynamic collision circles and NPC pathfinding do not need to update faster than ~60 Hz.
    // Rate-limiting to ~15ms (66.6 Hz) prevents CPU spikes during camera rotation and dense NPC scenes.
    static LARGE_INTEGER s_lastNavQpc = {0};
    static LARGE_INTEGER s_qpcFreq = {0};
    if (s_qpcFreq.QuadPart == 0) {
        QueryPerformanceFrequency(&s_qpcFreq);
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (s_lastNavQpc.QuadPart != 0) {
        int64_t elapsedUs = (now.QuadPart - s_lastNavQpc.QuadPart) * 1000000 / s_qpcFreq.QuadPart;
        if (elapsedUs < 15000) { // less than 15ms elapsed since last nav sweep
            s_navSkippedTicks.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    s_lastNavQpc = now;

    // Execute navigation step
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

    HMODULE hGame = GetModuleHandleA("Game.dll");
    if (!hGame) {
        Log("G3O", "Navigation Optimizer: Game.dll not loaded; hook skipped.");
        return;
    }

    void* pProcess = reinterpret_cast<void*>(GetProcAddress(hGame, "?Process@gCNavigationAdmin@@UAEXXZ"));
    if (pProcess) {
        MH_STATUS createStatus = MH_OK, enableStatus = MH_OK, removeStatus = MH_OK;
        const HookInstallResult result = InstallHookSafely(
            [&] { createStatus = MH_CreateHook(pProcess, reinterpret_cast<LPVOID>(Hooked_NavigationProcess),
                                               reinterpret_cast<LPVOID*>(&Real_NavigationProcess)); return createStatus == MH_OK; },
            [&] { enableStatus = MH_EnableHook(pProcess); return enableStatus == MH_OK; },
            [&] { removeStatus = MH_RemoveHook(pProcess); },
            [&] { Real_NavigationProcess = nullptr; });
        if (result == HookInstallResult::Installed) {
            s_targetNavProcess = pProcess;
            Log("G3O", "Navigation Optimizer: Hooked gCNavigationAdmin::Process (0x%p)", pProcess);
        } else {
            Log("G3O", "Navigation Optimizer: Process install failed (create=%d, enable=%d, remove=%d).",
                createStatus, enableStatus, removeStatus);
        }
    } else {
        Log("G3O", "Navigation Optimizer: gCNavigationAdmin::Process export missing.");
    }
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
