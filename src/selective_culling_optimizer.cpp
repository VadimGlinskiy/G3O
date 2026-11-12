#include "selective_culling_optimizer.h"
#include "selective_culling_policy.h"
#include "config_manager.h"
#include "diagnostic_logger.h"
#include "hook_install_policy.h"
#include <windows.h>
#include <minhook.h>
#include <atomic>

namespace G3O {

using CullSmallObjects_t = float (__thiscall*)(void* pThis, float fAreaFactor, const void* pEntity);
static CullSmallObjects_t Real_CullSmallObjects = nullptr;
static void* s_targetCullSmallObjects = nullptr;

static std::atomic<std::uint64_t> s_totalEvaluations{0};
static std::atomic<std::uint64_t> s_importantEvaluations{0};
static std::atomic<std::uint64_t> s_clutterEvaluations{0};
static std::atomic<std::uint64_t> s_culledCount{0};

static bool s_enabled = true;
static float s_clutterThreshold = 0.008f;
static float s_importantThreshold = 0.002f;

static float __fastcall Hooked_CullSmallObjects(void* pThis, void* /*edx*/, float fAreaFactor, const void* pEntity) {
    if (!pThis || !Real_CullSmallObjects) {
        return 0.0f;
    }

    if (!s_enabled) {
        return Real_CullSmallObjects(pThis, fAreaFactor, pEntity);
    }

    s_totalEvaluations.fetch_add(1, std::memory_order_relaxed);

    const auto category = ClassifyEntity(pEntity);
    if (category == EntityCullCategory::Important) {
        s_importantEvaluations.fetch_add(1, std::memory_order_relaxed);
    } else {
        s_clutterEvaluations.fetch_add(1, std::memory_order_relaxed);
    }

    const float baseThreshold = *reinterpret_cast<const float*>(
        reinterpret_cast<const char*>(pThis) + 0x34
    );

    const float adjustedArea = CalculateAdjustedAreaFactor(
        fAreaFactor, category, baseThreshold, s_importantThreshold, s_clutterThreshold, true
    );

    const float result = Real_CullSmallObjects(pThis, adjustedArea, pEntity);
    if (result <= 0.0001f) {
        s_culledCount.fetch_add(1, std::memory_order_relaxed);
    }

    return result;
}

void InitializeSelectiveCullingOptimizer() {
    const auto& cfg = GetConfig();
    s_enabled = cfg.enableSelectiveCulling;
    s_clutterThreshold = cfg.clutterCullThreshold;
    s_importantThreshold = cfg.importantCullThreshold;

    if (!s_enabled) {
        Log("SelectiveCulling", "Disabled by G3O.ini (EnableSelectiveCulling=false).");
        return;
    }

    HMODULE hEngine = GetModuleHandleA("Engine.dll");
    if (!hEngine) {
        Log("SelectiveCulling", "Failed to resolve Engine.dll handle.");
        return;
    }

    // Attempt export lookup first, fallback to verified RVA 0x1AC6D0
    FARPROC pfnTarget = GetProcAddress(hEngine, "?CullSmallObjects@eCFrustumFilter@@QAEMMABVeCEntity@@@Z");
    if (!pfnTarget) {
        pfnTarget = reinterpret_cast<FARPROC>(reinterpret_cast<char*>(hEngine) + 0x1AC6D0);
    }

    if (!pfnTarget) {
        Log("SelectiveCulling", "Could not resolve eCFrustumFilter::CullSmallObjects; hook skipped.");
        return;
    }

    MH_STATUS createStatus = MH_OK, enableStatus = MH_OK, removeStatus = MH_OK;
    const HookInstallResult result = InstallHookSafely(
        [&] {
            createStatus = MH_CreateHook(reinterpret_cast<LPVOID>(pfnTarget),
                                         reinterpret_cast<LPVOID>(&Hooked_CullSmallObjects),
                                         reinterpret_cast<LPVOID*>(&Real_CullSmallObjects));
            return createStatus == MH_OK;
        },
        [&] { enableStatus = MH_EnableHook(reinterpret_cast<LPVOID>(pfnTarget)); return enableStatus == MH_OK; },
        [&] { removeStatus = MH_RemoveHook(reinterpret_cast<LPVOID>(pfnTarget)); },
        [&] { Real_CullSmallObjects = nullptr; });

    if (result == HookInstallResult::Installed) {
        s_targetCullSmallObjects = reinterpret_cast<LPVOID>(pfnTarget);
        Log("SelectiveCulling",
            "Installed hook on eCFrustumFilter::CullSmallObjects (Target=%p, ClutterCutoff=%.4f, ImportantCutoff=%.4f).",
            pfnTarget, s_clutterThreshold, s_importantThreshold);
    } else {
        Log("SelectiveCulling", "Failed to install eCFrustumFilter::CullSmallObjects hook (create=%d, enable=%d, remove=%d).",
            static_cast<int>(createStatus), static_cast<int>(enableStatus), static_cast<int>(removeStatus));
    }
}

void ShutdownSelectiveCullingOptimizer() {
    if (s_targetCullSmallObjects) {
        MH_DisableHook(s_targetCullSmallObjects);
        MH_RemoveHook(s_targetCullSmallObjects);
        s_targetCullSmallObjects = nullptr;
        Real_CullSmallObjects = nullptr;
    }
}

SelectiveCullingStats GetSelectiveCullingStats() noexcept {
    return {
        s_totalEvaluations.load(std::memory_order_relaxed),
        s_importantEvaluations.load(std::memory_order_relaxed),
        s_clutterEvaluations.load(std::memory_order_relaxed),
        s_culledCount.load(std::memory_order_relaxed)
    };
}

void ResetSelectiveCullingStats() noexcept {
    s_totalEvaluations.store(0, std::memory_order_relaxed);
    s_importantEvaluations.store(0, std::memory_order_relaxed);
    s_clutterEvaluations.store(0, std::memory_order_relaxed);
    s_culledCount.store(0, std::memory_order_relaxed);
}

} // namespace G3O
