#include "vegetation_optimizer.h"
#include "vegetation_shadow_policy.h"
#include "config_manager.h"
#include "diagnostic_logger.h"
#include "hook_install_policy.h"
#include <minhook.h>
#include <windows.h>
#include <cmath>

namespace G3O {

// 1. Tree terrain shadow distance clamping
typedef void (__thiscall* AddTreeShadow_t)(void* pThis, void* pTree);
static AddTreeShadow_t Real_AddTreeShadow = nullptr;

static VegetationShadowThresholds s_shadowThresholds{};
static bool s_enableTreeShadowClamping = true;
static bool s_enableAdaptiveTreeShadows = true;
static HMODULE s_hEngine = nullptr;
static const uintptr_t s_activeCameraRva = 0x602aac; // RVA of active eCGfxCamera* in Engine.dll
static void* s_targetAddTreeShadow = nullptr;
static void* s_targetCalculateCacheRect = nullptr;

static void __fastcall Hooked_AddTreeShadow(void* pThis, void* /*edx*/, void* pTree) {
    if (!pTree || !Real_AddTreeShadow) return;

    if (s_enableTreeShadowClamping) {
        if (s_shadowThresholds.mediumDistSq <= 0.0f) {
            // Distance set to 0: completely bypass terrain tree shadow pass for max FPS
            return;
        }

        __try {
            if (s_hEngine) {
                void** ppCam = (void**)((char*)s_hEngine + s_activeCameraRva);
                if (ppCam && *ppCam) {
                    char* pCam = (char*)*ppCam;
                    // Camera world position in inverse view matrix is at +0x144 (X) and +0x14C (Z)
                    float camX = *(const float*)(pCam + 0x144);
                    float camZ = *(const float*)(pCam + 0x14C);

                    // Tree entity is at pTree + 0x0c (eCEntityPropertySet::GetEntity)
                    char* pEntity = *(char**)((char*)pTree + 0x0c);
                    if (pEntity) {
                        // Entity position is at +0xb0 (eCEntity::GetLocalPosition)
                        float treeX = *(const float*)(pEntity + 0xb0);
                        float treeZ = *(const float*)(pEntity + 0xb8);

                        float dx = treeX - camX;
                        float dz = treeZ - camZ;
                        float distSq = dx * dx + dz * dz;

                        // Local node boundary bCBox is at pEntity + 0x110
                        // minY = +0x114, maxY = +0x120 (GetHeight = maxY - minY)
                        float minY = *(const float*)(pEntity + 0x114);
                        float maxY = *(const float*)(pEntity + 0x120);
                        float height = (maxY > minY) ? (maxY - minY) : 0.0f;

                        if (ShouldCullTreeShadow(distSq, height, s_enableAdaptiveTreeShadows, s_shadowThresholds)) {
                            return;
                        }
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // Safe fallback on any unexpected memory layout
        }
    }

    Real_AddTreeShadow(pThis, pTree);
}

// 2. Procedural grass & vegetation grid bounding rect clamping
// Clamps the effective grass caching radius in eCVegetation_Grid::CalculateCacheRect,
// preventing tens of thousands of sub-pixel distant grass culling & rendering passes
// while preserving full view distance for trees (SpeedTree) and terrain (DistanceHigh).
typedef void (__thiscall* CalculateCacheRect_t)(void* pGrid, const void* pCamera, void* pRect);
static CalculateCacheRect_t Real_CalculateCacheRect = nullptr;

static void __fastcall Hooked_CalculateCacheRect(void* pGrid, void* /*edx*/, const void* pCamera, void* pRect) {
    if (!pGrid || !Real_CalculateCacheRect) return;

    float* pRange = (float*)((BYTE*)pGrid + 0x10);
    float origRange = *pRange;
    float maxRange = GetConfig().vegetationGridRangeClamp;

    if (maxRange > 0.0f && origRange > maxRange) {
        *pRange = maxRange;
    }

    Real_CalculateCacheRect(pGrid, pCamera, pRect);

    *pRange = origRange;
}

void InitializeVegetationOptimizer() {
    const auto& cfg = GetConfig();
    if (!cfg.enableVegetationOptimizer) return;

    s_hEngine = GetModuleHandleA("Engine.dll");
    if (!s_hEngine) return;

    float dist = cfg.treeShadowDistance;
    s_shadowThresholds = ComputeVegetationShadowThresholds(dist);
    s_enableTreeShadowClamping = true;
    s_enableAdaptiveTreeShadows = cfg.enableAdaptiveTreeShadows;

    // Hook tree terrain shadow registration
    void* pAddTreeShadow = (void*)GetProcAddress(s_hEngine, "?AddTreeShadow@eCTerrainShadowMap@@QAEXPAVeCSpeedTree_PS@@@Z");
    if (pAddTreeShadow) {
        MH_STATUS createStatus = MH_OK, enableStatus = MH_OK, removeStatus = MH_OK;
        const HookInstallResult result = InstallHookSafely(
            [&] { createStatus = MH_CreateHook(pAddTreeShadow, (LPVOID)Hooked_AddTreeShadow, (LPVOID*)&Real_AddTreeShadow); return createStatus == MH_OK; },
            [&] { enableStatus = MH_EnableHook(pAddTreeShadow); return enableStatus == MH_OK; },
            [&] { removeStatus = MH_RemoveHook(pAddTreeShadow); },
            [&] { Real_AddTreeShadow = nullptr; });
        if (result == HookInstallResult::Installed) {
            s_targetAddTreeShadow = pAddTreeShadow;
            Log("G3O", "Vegetation Optimizer: Tree terrain shadow distance clamped to %.1f units (Adaptive=%s, Bush=%.1f, Giant=%.1f).",
                dist, s_enableAdaptiveTreeShadows ? "true" : "false",
                std::sqrt(s_shadowThresholds.bushDistSq), std::sqrt(s_shadowThresholds.giantDistSq));
        } else {
            Log("G3O", "Vegetation Optimizer: AddTreeShadow hook failed (create=%d, enable=%d, remove=%d).",
                createStatus, enableStatus, removeStatus);
        }
    }

    // Hook vegetation grid caching range only if clamping is actively configured (> 0.0f)
    if (cfg.vegetationGridRangeClamp > 0.0f) {
        void* pCalcRect = (void*)GetProcAddress(s_hEngine, "?CalculateCacheRect@eCVegetation_Grid@@IAEXABVeCCameraBase@@AAVbCRect@@@Z");
        if (pCalcRect) {
            MH_STATUS createStatus = MH_OK, enableStatus = MH_OK, removeStatus = MH_OK;
            const HookInstallResult result = InstallHookSafely(
                [&] { createStatus = MH_CreateHook(pCalcRect, (LPVOID)Hooked_CalculateCacheRect, (LPVOID*)&Real_CalculateCacheRect); return createStatus == MH_OK; },
                [&] { enableStatus = MH_EnableHook(pCalcRect); return enableStatus == MH_OK; },
                [&] { removeStatus = MH_RemoveHook(pCalcRect); },
                [&] { Real_CalculateCacheRect = nullptr; });
            if (result == HookInstallResult::Installed) {
                s_targetCalculateCacheRect = pCalcRect;
                Log("G3O", "Vegetation Optimizer: Grass cache grid radius clamped to %.1f units.", cfg.vegetationGridRangeClamp);
            } else {
                Log("G3O", "Vegetation Optimizer: CalculateCacheRect hook failed (create=%d, enable=%d, remove=%d).",
                    createStatus, enableStatus, removeStatus);
            }
        }
    } else {
        Log("G3O", "Vegetation Optimizer: CalculateCacheRect hook skipped (VegetationGridRangeClamp=0; full view distance preserved without detour overhead).");
    }
}

void ShutdownVegetationOptimizer() {
    if (s_targetAddTreeShadow) {
        MH_DisableHook(s_targetAddTreeShadow);
        MH_RemoveHook(s_targetAddTreeShadow);
        s_targetAddTreeShadow = nullptr;
        Real_AddTreeShadow = nullptr;
    }
    if (s_targetCalculateCacheRect) {
        MH_DisableHook(s_targetCalculateCacheRect);
        MH_RemoveHook(s_targetCalculateCacheRect);
        s_targetCalculateCacheRect = nullptr;
        Real_CalculateCacheRect = nullptr;
    }
}

} // namespace G3O
