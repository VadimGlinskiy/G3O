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
    const char* str1 = pThis ? *static_cast<const char* const*>(pThis) : nullptr;
    const char* str2 = pOther ? *static_cast<const char* const*>(pOther) : nullptr;
    return SharedBase::CompareString(str1, str2);
}

// ---------------------------------------------------------------------------
// 5. Case-insensitive equality replacement uses the complete null-terminated text.
// ---------------------------------------------------------------------------

static bool __fastcall Hooked_CompareNoCaseFast(const void* pThis, void* /*edx*/, const void* pOther) {
    const char* str1 = pThis ? *static_cast<const char* const*>(pThis) : nullptr;
    const char* str2 = pOther ? *static_cast<const char* const*>(pOther) : nullptr;
    return SharedBase::CompareStringNoCase(str1, str2);
}

// ---------------------------------------------------------------------------
// 6. Gothic 3 targets Windows NT; bypass the legacy platform check.
// ---------------------------------------------------------------------------

static BOOL __stdcall Hooked_IsNT() {
    return TRUE;
}

// ---------------------------------------------------------------------------
// Initialization & Shutdown
static void* s_sharedBaseHookTargets[4] = {};

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

    // 2. Hook bCString::CompareFast
    void* pCompareFast = (void*)GetProcAddress(hSharedBase, "?CompareFast@bCString@@QBE_NABV1@@Z");
    if (pCompareFast) {
        MH_STATUS status = MH_CreateHook(pCompareFast, (LPVOID)Hooked_CompareFast, nullptr);
        if (status == MH_OK) {
            status = MH_EnableHook(pCompareFast);
            if (status == MH_OK) {
                s_sharedBaseHookTargets[1] = pCompareFast;
                Log("G3O", "SharedBase string comparison hook enabled.");
            } else {
                MH_RemoveHook(pCompareFast);
                Log("G3O", "SharedBase string comparison hook activation failed (code %d).", (int)status);
            }
        } else {
            Log("G3O", "SharedBase string comparison hook creation failed (code %d).", (int)status);
        }
    } else {
        Log("G3O", "SharedBase string comparison export is missing; optimization skipped.");
    }

    // 3. Hook bCString::CompareNoCaseFast
    void* pCompareNoCaseFast = (void*)GetProcAddress(hSharedBase, "?CompareNoCaseFast@bCString@@QBE_NABV1@@Z");
    if (pCompareNoCaseFast) {
        MH_STATUS status = MH_CreateHook(pCompareNoCaseFast, (LPVOID)Hooked_CompareNoCaseFast, nullptr);
        if (status == MH_OK) {
            status = MH_EnableHook(pCompareNoCaseFast);
            if (status == MH_OK) {
                s_sharedBaseHookTargets[2] = pCompareNoCaseFast;
                Log("G3O", "SharedBase case-insensitive string comparison hook enabled.");
            } else {
                MH_RemoveHook(pCompareNoCaseFast);
                Log("G3O", "SharedBase case-insensitive string comparison hook activation failed (code %d).", (int)status);
            }
        } else {
            Log("G3O", "SharedBase case-insensitive string comparison hook creation failed (code %d).", (int)status);
        }
    } else {
        Log("G3O", "SharedBase case-insensitive string comparison export is missing; optimization skipped.");
    }

    // 4. Hook _IsNT@0
    void* pIsNT = (void*)GetProcAddress(hSharedBase, "_IsNT@0");
    if (pIsNT) {
        MH_STATUS status = MH_CreateHook(pIsNT, (LPVOID)Hooked_IsNT, nullptr);
        if (status == MH_OK) {
            status = MH_EnableHook(pIsNT);
            if (status == MH_OK) {
                s_sharedBaseHookTargets[3] = pIsNT;
                Log("G3O", "SharedBase Windows NT check hook enabled.");
            } else {
                MH_RemoveHook(pIsNT);
                Log("G3O", "SharedBase Windows NT check hook activation failed (code %d).", (int)status);
            }
        } else {
            Log("G3O", "SharedBase Windows NT check hook creation failed (code %d).", (int)status);
        }
    } else {
        Log("G3O", "SharedBase Windows NT check export is missing; optimization skipped.");
    }
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
