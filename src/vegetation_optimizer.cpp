#include "vegetation_optimizer.h"
#include "config_manager.h"
#include "diagnostic_logger.h"
#include <minhook.h>
#include <windows.h>
#include <cmath>

namespace G3O {

// 1. Tree terrain shadow distance clamping
typedef void (__thiscall* AddTreeShadow_t)(void* pThis, void* pTree);
static AddTreeShadow_t Real_AddTreeShadow = nullptr;

static float s_treeShadowDistSq = 6500.0f * 6500.0f;
static bool s_enableTreeShadowClamping = true;
static HMODULE s_hEngine = nullptr;
static const uintptr_t s_activeCameraRva = 0x602aac; // RVA of active eCGfxCamera* in Engine.dll

static void __fastcall Hooked_AddTreeShadow(void* pThis, void* /*edx*/, void* pTree) {
    if (!pTree || !Real_AddTreeShadow) return;

    if (s_enableTreeShadowClamping) {
        if (s_treeShadowDistSq <= 0.0f) {
            // Distance set to 0: completely bypass terrain tree shadow pass for max FPS
            return;
        }

        __try {
            if (s_hEngine) {
                void** ppCam = (void**)((char*)s_hEngine + s_activeCameraRva);
                if (ppCam && *ppCam) {
                    char* pCam = (char*)*ppCam;
                    // Camera world position in inverse view matrix is at +0x144
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

                        // Clamping: if tree is beyond user shadow distance, skip shadow queue
                        if (distSq > s_treeShadowDistSq) {
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
    if (!GetConfig().enableVegetationOptimizer) return;

    s_hEngine = GetModuleHandleA("Engine.dll");
    if (!s_hEngine) return;

    float dist = GetConfig().treeShadowDistance;
    s_treeShadowDistSq = dist * dist;
    s_enableTreeShadowClamping = true;

    // Hook tree terrain shadow registration
    void* pAddTreeShadow = (void*)GetProcAddress(s_hEngine, "?AddTreeShadow@eCTerrainShadowMap@@QAEXPAVeCSpeedTree_PS@@@Z");
    if (pAddTreeShadow) {
        if (MH_CreateHook(pAddTreeShadow, (LPVOID)Hooked_AddTreeShadow, (LPVOID*)&Real_AddTreeShadow) == MH_OK) {
            MH_EnableHook(pAddTreeShadow);
            Log("G3O", "Vegetation Optimizer: Tree terrain shadow distance clamped to %.1f units.", dist);
        }
    }

    // Hook vegetation grid caching range
    void* pCalcRect = (void*)GetProcAddress(s_hEngine, "?CalculateCacheRect@eCVegetation_Grid@@IAEXABVeCCameraBase@@AAVbCRect@@@Z");
    if (pCalcRect) {
        if (MH_CreateHook(pCalcRect, (LPVOID)Hooked_CalculateCacheRect, (LPVOID*)&Real_CalculateCacheRect) == MH_OK) {
            MH_EnableHook(pCalcRect);
            Log("G3O", "Vegetation Optimizer: Grass cache grid radius clamped to %.1f units.", GetConfig().vegetationGridRangeClamp);
        }
    }
}

void ShutdownVegetationOptimizer() {
    if (s_hEngine) {
        void* pAddTreeShadow = (void*)GetProcAddress(s_hEngine, "?AddTreeShadow@eCTerrainShadowMap@@QAEXPAVeCSpeedTree_PS@@@Z");
        if (pAddTreeShadow) MH_DisableHook(pAddTreeShadow);

        void* pCalcRect = (void*)GetProcAddress(s_hEngine, "?CalculateCacheRect@eCVegetation_Grid@@IAEXABVeCCameraBase@@AAVbCRect@@@Z");
        if (pCalcRect) MH_DisableHook(pCalcRect);
    }
}

} // namespace G3O
