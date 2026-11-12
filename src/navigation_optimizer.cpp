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

    HMODULE hGame = GetModuleHandleA("Game.dll");
    if (!hGame) {
        Log("G3O", "Navigation Optimizer: Game.dll not loaded.");
        return;
    }

    // ?Process@gCNavigationAdmin@@UAEXXZ (RVA 0x00120720 in Game.dll)
    void* pProc = reinterpret_cast<void*>(GetProcAddress(hGame, "?Process@gCNavigationAdmin@@UAEXXZ"));
    if (!pProc) {
        Log("G3O", "Navigation Optimizer: ?Process@gCNavigationAdmin@@UAEXXZ not found in Game.dll.");
        return;
    }

    MH_STATUS createStatus = MH_OK, enableStatus = MH_OK, removeStatus = MH_OK;
    const HookInstallResult result = InstallHookSafely(
        [&] { createStatus = MH_CreateHook(pProc, reinterpret_cast<LPVOID>(Hooked_NavigationProcess),
                                           reinterpret_cast<LPVOID*>(&Real_NavigationProcess)); return createStatus == MH_OK; },
        [&] { enableStatus = MH_EnableHook(pProc); return enableStatus == MH_OK; },
        [&] { removeStatus = MH_RemoveHook(pProc); },
        [&] { Real_NavigationProcess = nullptr; });
    if (result == HookInstallResult::Installed) {
        s_targetNavProcess = pProc;
        Log("G3O", "Navigation Optimizer initialized (gCNavigationAdmin::Process hooked at 0x%p).", pProc);
    } else {
        Log("G3O", "Navigation Optimizer: hook install failed (create=%d, enable=%d, remove=%d).",
            createStatus, enableStatus, removeStatus);
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
