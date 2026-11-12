#include "fog_optimizer.h"
#include "fog_policy.h"
#include "config_manager.h"
#include "diagnostic_logger.h"
#include "hook_install_policy.h"
#include <windows.h>
#include <minhook.h>
#include <atomic>

namespace G3O {

using SetFogData_t = int (__thiscall*)(void* pThis, const GfxFogData* pFogData);
static SetFogData_t Real_SetFogData = nullptr;
static void* s_targetSetFogData = nullptr;

static std::atomic<std::uint64_t> s_totalSubmissions{0};
static std::atomic<std::uint64_t> s_expandedSubmissions{0};
static std::atomic<std::uint64_t> s_underwaterPreserved{0};
static std::atomic<float> s_lastStart{0.0f};
static std::atomic<float> s_lastEnd{0.0f};

static bool s_enabled = true;
static float s_distanceMultiplier = 2.5f;
static float s_endMinDistance = 250000.0f;
static bool s_preserveUnderwater = true;

static int __fastcall Hooked_SetFogData(void* pThis, void* /*edx*/, const GfxFogData* pFogData) {
    if (!pThis || !Real_SetFogData) {
        return 0;
    }

    if (!pFogData || !s_enabled) {
        return Real_SetFogData(pThis, pFogData);
    }

    s_totalSubmissions.fetch_add(1, std::memory_order_relaxed);

    if (s_preserveUnderwater && IsUnderwaterOrCloseRangeFog(*pFogData)) {
        s_underwaterPreserved.fetch_add(1, std::memory_order_relaxed);
        s_lastStart.store(pFogData->fStart, std::memory_order_relaxed);
        s_lastEnd.store(pFogData->fEnd, std::memory_order_relaxed);
        return Real_SetFogData(pThis, pFogData);
    }

    GfxFogData modified = *pFogData;
    ExpandAtmosphericFog(modified, s_distanceMultiplier, s_endMinDistance, s_preserveUnderwater);

    if (modified.fEnd > pFogData->fEnd) {
        s_expandedSubmissions.fetch_add(1, std::memory_order_relaxed);
    }

    s_lastStart.store(modified.fStart, std::memory_order_relaxed);
    s_lastEnd.store(modified.fEnd, std::memory_order_relaxed);

    return Real_SetFogData(pThis, &modified);
}

void InitializeFogOptimizer() {
    const auto& cfg = GetConfig();
    s_enabled = cfg.enableAtmosphericFogControl;
    s_distanceMultiplier = cfg.fogDistanceMultiplier;
    s_endMinDistance = cfg.fogEndMinDistance;
    s_preserveUnderwater = cfg.preserveUnderwaterFog;

    if (!s_enabled) {
        Log("FogOptimizer", "Disabled by G3O.ini (EnableAtmosphericFogControl=false).");
        return;
    }

    HMODULE hEngine = GetModuleHandleA("Engine.dll");
    if (!hEngine) {
        Log("FogOptimizer", "Failed to resolve Engine.dll handle.");
        return;
    }

    FARPROC pfnTarget = nullptr;

    // Resolve via eCDX9Device vtable ??_7eCDX9Device@@6B@ slot 83 (offset 0x14C)
    void** pVTable = reinterpret_cast<void**>(GetProcAddress(hEngine, "??_7eCDX9Device@@6B@"));
    if (pVTable) {
        pfnTarget = reinterpret_cast<FARPROC>(pVTable[83]);
    }

    // Fallback to verified RVA 0x000A26E0 if vtable lookup failed
    if (!pfnTarget) {
        pfnTarget = reinterpret_cast<FARPROC>(reinterpret_cast<char*>(hEngine) + 0x000A26E0);
    }

    if (!pfnTarget) {
        Log("FogOptimizer", "Could not resolve eCDX9Device::SetFogData; hook skipped.");
        return;
    }

    MH_STATUS createStatus = MH_OK, enableStatus = MH_OK, removeStatus = MH_OK;
    const HookInstallResult result = InstallHookSafely(
        [&] {
            createStatus = MH_CreateHook(reinterpret_cast<LPVOID>(pfnTarget),
                                         reinterpret_cast<LPVOID>(&Hooked_SetFogData),
                                         reinterpret_cast<LPVOID*>(&Real_SetFogData));
            return createStatus == MH_OK;
        },
        [&] { enableStatus = MH_EnableHook(reinterpret_cast<LPVOID>(pfnTarget)); return enableStatus == MH_OK; },
        [&] { removeStatus = MH_RemoveHook(reinterpret_cast<LPVOID>(pfnTarget)); },
        [&] { Real_SetFogData = nullptr; });

    if (result == HookInstallResult::Installed) {
        s_targetSetFogData = reinterpret_cast<LPVOID>(pfnTarget);
        Log("FogOptimizer",
            "Installed hook on eCDX9Device::SetFogData (Target=%p, Multiplier=%.2f, MinEnd=%.1f, PreserveUnderwater=%s).",
            pfnTarget, s_distanceMultiplier, s_endMinDistance, s_preserveUnderwater ? "true" : "false");
    } else {
        Log("FogOptimizer", "Failed to install eCDX9Device::SetFogData hook (create=%d, enable=%d, remove=%d).",
            static_cast<int>(createStatus), static_cast<int>(enableStatus), static_cast<int>(removeStatus));
    }
}

void ShutdownFogOptimizer() {
    if (s_targetSetFogData) {
        MH_DisableHook(s_targetSetFogData);
        MH_RemoveHook(s_targetSetFogData);
        s_targetSetFogData = nullptr;
        Real_SetFogData = nullptr;
    }
}

FogOptimizerStats GetFogOptimizerStats() noexcept {
    return {
        s_totalSubmissions.load(std::memory_order_relaxed),
        s_expandedSubmissions.load(std::memory_order_relaxed),
        s_underwaterPreserved.load(std::memory_order_relaxed),
        s_lastStart.load(std::memory_order_relaxed),
        s_lastEnd.load(std::memory_order_relaxed)
    };
}

void ResetFogOptimizerStats() noexcept {
    s_totalSubmissions.store(0, std::memory_order_relaxed);
    s_expandedSubmissions.store(0, std::memory_order_relaxed);
    s_underwaterPreserved.store(0, std::memory_order_relaxed);
    s_lastStart.store(0.0f, std::memory_order_relaxed);
    s_lastEnd.store(0.0f, std::memory_order_relaxed);
}

} // namespace G3O
