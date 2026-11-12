#include "pvs_prefetch_optimizer.h"
#include "pvs_prefetch_policy.h"
#include "config_manager.h"
#include "diagnostic_logger.h"
#include "savegame_profiler.h"
#include <minhook.h>
#include <windows.h>
#include <atomic>

namespace G3O {

using PvsExecuteFn = void (__thiscall*)(void* pThis, void* pFrustumDB, const GothicSphere* pSphere);
static PvsExecuteFn Real_PvsExecute = nullptr;

static HMODULE s_hEngine = nullptr;
static void* (__stdcall *s_pfnGetApp)() = nullptr;
static const void* (__thiscall *s_pfnGetSetupEngineDesc)(const void*) = nullptr;
static void* s_targetPvsExecute = nullptr;
static bool s_pvsHookCreated = false;
static bool s_pvsHookEnabled = false;
static bool s_pvsCsInitialized = false;

static volatile LONG* s_pEntityROI = nullptr;
static float s_originalTargetROI = 0.0f;
static bool s_hasCachedOriginalROI = false;

static PvsPolicyState s_policyState = {};
static CRITICAL_SECTION s_pvsCs;
static bool s_adaptiveRoiOverrideActive = false;

static std::atomic<uint32_t> s_clampedCalls{0};
static std::atomic<uint32_t> s_skippedCalls{0};
static std::atomic<uint32_t> s_rampedCalls{0};
static std::atomic<float> s_effectiveRoi{0.0f};

static void EnsureEnginePointers() {
    if (!s_hEngine) {
        s_hEngine = GetModuleHandleA("Engine.dll");
        if (s_hEngine) {
            s_pfnGetApp = reinterpret_cast<void* (__stdcall *)()>(
                GetProcAddress(s_hEngine, "?GetInstance@eCApplication@@SGAAV1@XZ"));
            s_pfnGetSetupEngineDesc = reinterpret_cast<const void* (__thiscall *)(const void*)>(
                GetProcAddress(s_hEngine, "?GetSetupEngineDesc@eCApplication@@QBEABUeSSetupEngine@@XZ"));
        }
    }

    if (!s_pEntityROI && s_pfnGetApp && s_pfnGetSetupEngineDesc) {
        void* pApp = s_pfnGetApp();
        if (pApp) {
            const void* pSetup = s_pfnGetSetupEngineDesc(pApp);
            if (pSetup) {
                // eSSetupEngine::Entity_ROI is a 32-bit integer at offset 0x8C.
                s_pEntityROI = reinterpret_cast<volatile LONG*>(
                    reinterpret_cast<uintptr_t>(pSetup) + 0x8C);
            }
            const LONG currentRoi = s_pEntityROI ? InterlockedCompareExchange(
                const_cast<LONG*>(s_pEntityROI), 0, 0) : 0;
            if (!s_hasCachedOriginalROI && currentRoi > 0) {
                s_originalTargetROI = static_cast<float>(currentRoi);
                s_hasCachedOriginalROI = true;
            }
        }
    }
}

static void ApplyEffectiveROISlow(float effectiveRoi) {
    if (effectiveRoi <= 0.0f) return;

    EnsureEnginePointers();

    __try {
        if (s_pEntityROI) {
            const LONG roi = static_cast<LONG>(effectiveRoi + 0.5f);
            InterlockedExchange(const_cast<LONG*>(s_pEntityROI), roi);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // The application can replace its setup descriptor during shutdown.
    }
}

static void __fastcall Hooked_PvsExecute(void* pThis, void* /*edx*/, void* pFrustumDB, const GothicSphere* pSphere) {
    if (!pSphere || !Real_PvsExecute) {
        if (Real_PvsExecute) {
            Real_PvsExecute(pThis, pFrustumDB, pSphere);
        }
        return;
    }

    const auto& cfg = GetConfig();
    const bool adaptiveRoiEnabled = cfg.enableAdaptiveROI && !cfg.enableDynamicROI;
    if (!cfg.enablePVSPrefetchOptimizer || pSphere->radius <= 0.0f) {
        Real_PvsExecute(pThis, pFrustumDB, pSphere);
        return;
    }

    EnsureEnginePointers();
    const bool isSync = IsGotoLoadedPositionActive();

    EnterCriticalSection(&s_pvsCs);

    if (isSync) {
        GothicSphere targetSphere = *pSphere;
        const float redundantDeltaSq = cfg.pvsRedundantDistance * cfg.pvsRedundantDistance;
        const float targetRoi = (cfg.enableDynamicROI && cfg.baseROI > 0.0f)
            ? cfg.baseROI
            : PVSOptimizer_GetOriginalTargetROI();

        const auto action = PvsPrefetchPolicy::EvaluateSync(
            s_policyState, *pSphere, cfg.pvsBootRadius, redundantDeltaSq, cfg.pvsRampUpCalls, targetSphere, cfg.roiBootRadius, targetRoi);

        const float effectiveRoi = s_policyState.effectiveRoiRadius;
        const bool applyAdaptiveRoi = adaptiveRoiEnabled && s_policyState.targetRoiRadius > 0.0f;
        s_adaptiveRoiOverrideActive = applyAdaptiveRoi;
        s_effectiveRoi.store(effectiveRoi, std::memory_order_relaxed);

        LeaveCriticalSection(&s_pvsCs);

        if (applyAdaptiveRoi) {
            ApplyEffectiveROISlow(effectiveRoi);
        }

        if (action == PvsSyncAction::SkipRedundant) {
            s_skippedCalls.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        if (action == PvsSyncAction::ExecuteClamped) {
            s_clampedCalls.fetch_add(1, std::memory_order_relaxed);
            Real_PvsExecute(pThis, pFrustumDB, &targetSphere);
            return;
        }

        Real_PvsExecute(pThis, pFrustumDB, pSphere);
        return;
    }

    // Async / gameplay mode
    GothicSphere targetSphere = *pSphere;
    const bool isRamped = PvsPrefetchPolicy::EvaluateAsync(
        s_policyState, *pSphere, cfg.pvsBootRadius, cfg.pvsRampUpCalls, targetSphere, cfg.roiBootRadius);

    const float effectiveRoi = s_policyState.effectiveRoiRadius;
    const float targetRoi = s_policyState.targetRoiRadius;
    const bool restoreAdaptiveRoi = adaptiveRoiEnabled && !isRamped && s_adaptiveRoiOverrideActive && targetRoi > 0.0f;
    const bool applyAdaptiveRoi = adaptiveRoiEnabled && isRamped && targetRoi > 0.0f;
    if (adaptiveRoiEnabled && ((!isRamped) || s_policyState.rampUpRemaining == 0)) {
        s_adaptiveRoiOverrideActive = false;
    }
    s_effectiveRoi.store(effectiveRoi, std::memory_order_relaxed);

    LeaveCriticalSection(&s_pvsCs);

    if (applyAdaptiveRoi) {
        ApplyEffectiveROISlow(effectiveRoi);
    } else if (restoreAdaptiveRoi && targetRoi > 0.0f) {
        ApplyEffectiveROISlow(targetRoi);
    }

    if (isRamped) {
        s_rampedCalls.fetch_add(1, std::memory_order_relaxed);
        Real_PvsExecute(pThis, pFrustumDB, &targetSphere);
        return;
    }

    Real_PvsExecute(pThis, pFrustumDB, pSphere);
}

void InitializePVSPrefetchOptimizer() {
    if (!s_pvsCsInitialized) {
        InitializeCriticalSection(&s_pvsCs);
        s_pvsCsInitialized = true;
    }

    EnsureEnginePointers();

    EnterCriticalSection(&s_pvsCs);
    PvsPrefetchPolicy::Reset(s_policyState);
    if (s_hasCachedOriginalROI && s_originalTargetROI > 0.0f) {
        s_policyState.targetRoiRadius = s_originalTargetROI;
        s_policyState.effectiveRoiRadius = s_originalTargetROI;
    }
    s_adaptiveRoiOverrideActive = false;
    LeaveCriticalSection(&s_pvsCs);

    const auto& cfg = GetConfig();
    if (!cfg.enablePVSPrefetchOptimizer) {
        Log("G3O", "PVS Prefetch Optimizer disabled in configuration.");
        return;
    }

    if (!s_hEngine) {
        Log("G3O", "PVS Prefetch Optimizer: Engine.dll not loaded.");
        return;
    }

    s_targetPvsExecute = reinterpret_cast<void*>(GetProcAddress(
        s_hEngine, "?Execute@eCPVSPrefetcher3@@IAEXAAVeCFrustumDatabase@@ABVbCSphere@@@Z"));

    if (!s_targetPvsExecute) {
        Log("G3O", "PVS Prefetch Optimizer: ?Execute@eCPVSPrefetcher3 not found.");
        return;
    }

    MH_STATUS status = MH_CreateHook(s_targetPvsExecute, reinterpret_cast<LPVOID>(Hooked_PvsExecute),
                                     reinterpret_cast<LPVOID*>(&Real_PvsExecute));
    if (status != MH_OK) {
        Log("G3O", "PVS Prefetch Optimizer: MH_CreateHook failed (%d).", static_cast<int>(status));
        s_targetPvsExecute = nullptr;
        Real_PvsExecute = nullptr;
        return;
    }
    s_pvsHookCreated = true;

    status = MH_EnableHook(s_targetPvsExecute);
    if (status != MH_OK) {
        const MH_STATUS removeStatus = MH_RemoveHook(s_targetPvsExecute);
        Log("G3O", "PVS Prefetch Optimizer: MH_EnableHook failed (%d); removing hook returned %d.",
            static_cast<int>(status), static_cast<int>(removeStatus));
        if (removeStatus == MH_OK) {
            s_pvsHookCreated = false;
            s_targetPvsExecute = nullptr;
            Real_PvsExecute = nullptr;
        }
        return;
    }
    s_pvsHookEnabled = true;

    Log("G3O", "PVS Prefetch Optimizer initialized (target=%p, PVS boot radius=%.0f, ROI boot radius=%.0f, repeat distance=%.0f, ramp calls=%u, adaptive Entity.ROI=%s).",
        s_targetPvsExecute, cfg.pvsBootRadius, cfg.roiBootRadius, cfg.pvsRedundantDistance, cfg.pvsRampUpCalls,
        cfg.enableAdaptiveROI ? "enabled" : "disabled");
}

void ShutdownPVSPrefetchOptimizer() {
    Log("G3O", "PVS Prefetch Optimizer shutdown (clamped=%u, skipped=%u, ramped=%u, effectiveROI=%.0f).",
        s_clampedCalls.load(std::memory_order_relaxed),
        s_skippedCalls.load(std::memory_order_relaxed),
        s_rampedCalls.load(std::memory_order_relaxed),
        s_effectiveRoi.load(std::memory_order_relaxed));

    if (s_targetPvsExecute && s_pvsHookEnabled) {
        const MH_STATUS status = MH_DisableHook(s_targetPvsExecute);
        if (status != MH_OK && status != MH_ERROR_DISABLED) {
            Log("G3O", "PVS Prefetch Optimizer: MH_DisableHook failed (%d).", static_cast<int>(status));
        }
    }
    if (s_targetPvsExecute && s_pvsHookCreated) {
        const MH_STATUS status = MH_RemoveHook(s_targetPvsExecute);
        if (status != MH_OK) {
            Log("G3O", "PVS Prefetch Optimizer: MH_RemoveHook failed (%d).", static_cast<int>(status));
        }
    }
    s_pvsHookEnabled = false;
    s_pvsHookCreated = false;
    s_targetPvsExecute = nullptr;
    Real_PvsExecute = nullptr;

    if (s_hasCachedOriginalROI) {
        ApplyEffectiveROISlow(s_originalTargetROI);
    }
    s_adaptiveRoiOverrideActive = false;
    if (s_pvsCsInitialized) {
        DeleteCriticalSection(&s_pvsCs);
        s_pvsCsInitialized = false;
    }
}

void ResetPVSPrefetchOptimizerSession() {
    if (!s_pvsCsInitialized) {
        return;
    }
    EnterCriticalSection(&s_pvsCs);
    PvsPrefetchPolicy::Reset(s_policyState);
    if (s_hasCachedOriginalROI && s_originalTargetROI > 0.0f) {
        s_policyState.targetRoiRadius = s_originalTargetROI;
        s_policyState.effectiveRoiRadius = s_originalTargetROI;
    }
    s_adaptiveRoiOverrideActive = false;
    const float resetRoi = s_policyState.effectiveRoiRadius;
    s_effectiveRoi.store(resetRoi, std::memory_order_relaxed);
    LeaveCriticalSection(&s_pvsCs);

    const auto& cfg = GetConfig();
    if (cfg.enableAdaptiveROI && !cfg.enableDynamicROI && resetRoi > 0.0f) {
        ApplyEffectiveROISlow(resetRoi);
    }
}

uint32_t PVSOptimizer_GetClampedCalls() noexcept {
    return s_clampedCalls.load(std::memory_order_relaxed);
}

uint32_t PVSOptimizer_GetSkippedCalls() noexcept {
    return s_skippedCalls.load(std::memory_order_relaxed);
}

uint32_t PVSOptimizer_GetRampedCalls() noexcept {
    return s_rampedCalls.load(std::memory_order_relaxed);
}

float PVSOptimizer_GetEffectiveROI() noexcept {
    return s_effectiveRoi.load(std::memory_order_relaxed);
}

float PVSOptimizer_GetOriginalTargetROI() noexcept {
    EnsureEnginePointers();
    if (s_hasCachedOriginalROI && s_originalTargetROI > 0.0f) {
        return s_originalTargetROI;
    }
    if (s_pEntityROI) {
        const LONG currentRoi = InterlockedCompareExchange(const_cast<LONG*>(s_pEntityROI), 0, 0);
        if (currentRoi > 0) {
            return static_cast<float>(currentRoi);
        }
    }
    return 0.0f;
}

bool PVSOptimizer_IsRampingUp() noexcept {
    if (!s_pvsCsInitialized) {
        return false;
    }
    EnterCriticalSection(&s_pvsCs);
    const bool ramping = s_adaptiveRoiOverrideActive;
    LeaveCriticalSection(&s_pvsCs);
    return ramping;
}

void PVSOptimizer_ApplyEffectiveROI(float roi) noexcept {
    s_effectiveRoi.store(roi, std::memory_order_relaxed);
    ApplyEffectiveROISlow(roi);
}

} // namespace G3O
