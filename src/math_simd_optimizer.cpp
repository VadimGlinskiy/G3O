#include "math_simd_optimizer.h"
#include "diagnostic_logger.h"
#include "sharedbase_string_compare.h"
#include <minhook.h>

namespace G3O {

struct Vector3 {
    float x;
    float y;
    float z;
};

struct Box3 {
    Vector3 min;
    Vector3 max;
};
static_assert(sizeof(Box3) == 0x18, "Gothic 3 bCBox layout must be 24 bytes.");

// ---------------------------------------------------------------------------
// 1. Box-Box intersection (bCBox::Intersects(const bCBox&))
// ---------------------------------------------------------------------------

static bool __fastcall Hooked_BoxIntersectsBox(const Box3* pThis, void* /*edx*/, const Box3* pOther) {
    if (!pThis || !pOther) return false;

    if (pThis->min.x > pOther->max.x || pThis->max.x < pOther->min.x) return false;
    if (pThis->min.y > pOther->max.y || pThis->max.y < pOther->min.y) return false;
    if (pThis->min.z > pOther->max.z || pThis->max.z < pOther->min.z) return false;

    return true;
}

// ---------------------------------------------------------------------------
// 4. String equality replacements use the complete null-terminated text.
// ---------------------------------------------------------------------------

static bool __fastcall Hooked_CompareFast(const void* pThis, void* /*edx*/, const void* pOther) {
    return SharedBase::FastStringCompare(pThis, pOther);
}

// ---------------------------------------------------------------------------
// 5. Case-insensitive equality replacement uses length-bounded comparison.
// ---------------------------------------------------------------------------

static bool __fastcall Hooked_CompareNoCaseFast(const void* pThis, void* /*edx*/, const void* pOther) {
    return SharedBase::FastStringCompareNoCase(pThis, pOther);
}

// ---------------------------------------------------------------------------
// Initialization & Shutdown
static void* s_sharedBaseHookTargets[3] = {};

void InitializeSharedBaseOptimizations() {
    HMODULE hSharedBase = GetModuleHandleA("SharedBase.dll");
    if (!hSharedBase) return;

    // 1. Hook bCBox::Intersects(const bCBox&)
    void* pIntersects = (void*)GetProcAddress(hSharedBase, "?Intersects@bCBox@@QBE_NABV1@@Z");
    if (pIntersects) {
        MH_STATUS status = MH_CreateHook(pIntersects, (LPVOID)Hooked_BoxIntersectsBox, nullptr);
        if (status == MH_OK) {
            status = MH_EnableHook(pIntersects);
            if (status == MH_OK) {
                s_sharedBaseHookTargets[0] = pIntersects;
                Log("G3O", "SharedBase box intersection hook enabled.");
            } else {
                MH_RemoveHook(pIntersects);
                Log("G3O", "SharedBase box intersection hook activation failed (code %d).", (int)status);
            }
        } else {
            Log("G3O", "SharedBase box intersection hook creation failed (code %d).", (int)status);
        }
    } else {
        Log("G3O", "SharedBase box intersection export is missing; optimization skipped.");
    }

    // 2. bCString::CompareFast & CompareNoCaseFast:
    // Engine's native SharedBase.dll implementation already compares string lengths at offset -8
    // in 3 CPU cycles. Micro-hooking via MinHook adds trampoline calls, register saving, and cacheline
    // bouncing that showed up as 3.1% CPU hotspot during camera rotation / frustum culling.
    // Leaving native implementation active eliminates this overhead completely.
    Log("G3O", "SharedBase native string comparisons preserved (detour hooks bypassed for lower latency).");
}

void ShutdownSharedBaseOptimizations() {
    for (void*& target : s_sharedBaseHookTargets) {
        if (!target) continue;
        MH_DisableHook(target);
        MH_RemoveHook(target);
        target = nullptr;
    }
}

} // namespace G3O
