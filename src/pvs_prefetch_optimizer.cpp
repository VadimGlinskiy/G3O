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

static const uint8_t* s_pTimeSlicingFlag = nullptr;
static PvsPolicyState s_policyState = {};
static CRITICAL_SECTION s_pvsCs;

static std::atomic<uint32_t> s_clampedCalls{0};
static std::atomic<uint32_t> s_skippedCalls{0};
static std::atomic<uint32_t> s_rampedCalls{0};

static void __fastcall Hooked_PvsExecute(void* pThis, void* /*edx*/, void* pFrustumDB, const GothicSphere* pSphere) {
    if (!pSphere || !Real_PvsExecute) {
        if (Real_PvsExecute) {
            Real_PvsExecute(pThis, pFrustumDB, pSphere);
        }
        return;
    }

    const auto& cfg = GetConfig();
    if (!cfg.enablePVSPrefetchOptimizer || pSphere->radius <= 0.0f) {
        Real_PvsExecute(pThis, pFrustumDB, pSphere);
        return;
    }

    const bool isSync = IsGotoLoadedPositionActive() || (s_pTimeSlicingFlag && *s_pTimeSlicingFlag == 0);

    EnterCriticalSection(&s_pvsCs);

    if (isSync) {
        GothicSphere targetSphere = *pSphere;
        const float redundantDeltaSq = cfg.pvsRedundantDistance * cfg.pvsRedundantDistance;
        const auto action = PvsPrefetchPolicy::EvaluateSync(
            s_policyState, *pSphere, cfg.pvsBootRadius, redundantDeltaSq, cfg.pvsRampUpCalls, targetSphere);

        LeaveCriticalSection(&s_pvsCs);

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
        s_policyState, *pSphere, cfg.pvsBootRadius, cfg.pvsRampUpCalls, targetSphere);

    LeaveCriticalSection(&s_pvsCs);

    if (isRamped) {
        s_rampedCalls.fetch_add(1, std::memory_order_relaxed);
        Real_PvsExecute(pThis, pFrustumDB, &targetSphere);
        return;
    }

    Real_PvsExecute(pThis, pFrustumDB, pSphere);
}

void InitializePVSPrefetchOptimizer() {
    InitializeCriticalSection(&s_pvsCs);

    const auto& cfg = GetConfig();
    if (!cfg.enablePVSPrefetchOptimizer) {
        Log("G3O", "PVS Prefetch Optimizer disabled in configuration.");
        return;
    }

    HMODULE hEngine = GetModuleHandleA("Engine.dll");
    if (!hEngine) {
        Log("G3O", "PVS Prefetch Optimizer: Engine.dll not loaded.");
        return;
    }

    const uintptr_t uEngine = reinterpret_cast<uintptr_t>(hEngine);
    s_pTimeSlicingFlag = reinterpret_cast<const uint8_t*>(uEngine + 0x005E3486);

    void* pExecute = reinterpret_cast<void*>(GetProcAddress(
        hEngine, "?Execute@eCPVSPrefetcher3@@IAEXAAVeCFrustumDatabase@@ABVbCSphere@@@Z"));

    if (!pExecute) {
        Log("G3O", "PVS Prefetch Optimizer: ?Execute@eCPVSPrefetcher3 not found.");
        return;
    }

    if (MH_CreateHook(pExecute, reinterpret_cast<LPVOID>(Hooked_PvsExecute),
                      reinterpret_cast<LPVOID*>(&Real_PvsExecute)) == MH_OK) {
        if (MH_EnableHook(pExecute) == MH_OK) {
            Log("G3O", "PVS Prefetch Optimizer: Hooked eCPVSPrefetcher3::Execute (0x%p) [BootRadius=%.0f, RedundantDelta=%.0f, RampUp=%u]",
                pExecute, cfg.pvsBootRadius, cfg.pvsRedundantDistance, cfg.pvsRampUpCalls);
        } else {
            Log("G3O", "PVS Prefetch Optimizer: Failed to enable hook on eCPVSPrefetcher3::Execute.");
        }
    } else {
        Log("G3O", "PVS Prefetch Optimizer: Failed to create hook on eCPVSPrefetcher3::Execute.");
    }
}

void ShutdownPVSPrefetchOptimizer() {
    Log("G3O", "PVS Prefetch Optimizer shutdown (clamped=%u, skipped=%u, ramped=%u).",
        s_clampedCalls.load(std::memory_order_relaxed),
        s_skippedCalls.load(std::memory_order_relaxed),
        s_rampedCalls.load(std::memory_order_relaxed));
    DeleteCriticalSection(&s_pvsCs);
}

void ResetPVSPrefetchOptimizerSession() {
    EnterCriticalSection(&s_pvsCs);
    PvsPrefetchPolicy::Reset(s_policyState);
    LeaveCriticalSection(&s_pvsCs);
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

} // namespace G3O
