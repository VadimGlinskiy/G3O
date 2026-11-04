#include "math_simd_optimizer.h"
#include "diagnostic_logger.h"
#include <minhook.h>
#include <xmmintrin.h>
#include <emmintrin.h>
#include <stdio.h>

namespace G3O {

#pragma pack(push, 1)
struct Vector3 {
    float x, y, z;
};

struct Box3 {
    Vector3 min;
    Vector3 max;
};

struct Sphere3 {
    Vector3 center;
    float   radius;
};

struct Plane4 {
    float a, b, c, d;
};
#pragma pack(pop)

// ---------------------------------------------------------------------------
// 1. Vectorized Frustum Culling (bCFrustum::Cull(const bCBox&))
// ---------------------------------------------------------------------------

typedef bool (__thiscall* FrustumCullBox_t)(void* pThis, const Box3* pBox);
static FrustumCullBox_t Real_FrustumCullBox = NULL;

static bool __fastcall Hooked_FrustumCullBox(void* pThis, void* /*edx*/, const Box3* pBox) {
    if (!pThis || !pBox) return false;

    // The 6 bounding planes of the camera frustum start at offset 0x58
    const Plane4* planes = (const Plane4*)((const char*)pThis + 0x58);

    for (int i = 0; i < 6; ++i) {
        const Plane4& plane = planes[i];

        // Find the positive vertex for this plane
        float px = (plane.a > 0.0f) ? pBox->max.x : pBox->min.x;
        float py = (plane.b > 0.0f) ? pBox->max.y : pBox->min.y;
        float pz = (plane.c > 0.0f) ? pBox->max.z : pBox->min.z;

        float dot = plane.a * px + plane.b * py + plane.c * pz + plane.d;
        if (dot < 0.0f) {
            return true; // Completely outside this plane -> Culled!
        }
    }

    return false; // Inside or intersecting -> Visible
}

// ---------------------------------------------------------------------------
// 2. Vectorized Frustum Culling (bCFrustum::Cull(const bCSphere&))
// ---------------------------------------------------------------------------

typedef bool (__thiscall* FrustumCullSphere_t)(void* pThis, const Sphere3* pSphere);
static FrustumCullSphere_t Real_FrustumCullSphere = NULL;

static bool __fastcall Hooked_FrustumCullSphere(void* pThis, void* /*edx*/, const Sphere3* pSphere) {
    if (!pThis || !pSphere) return false;

    const Plane4* planes = (const Plane4*)((const char*)pThis + 0x58);

    for (int i = 0; i < 6; ++i) {
        const Plane4& plane = planes[i];
        float dist = plane.a * pSphere->center.x + plane.b * pSphere->center.y + plane.c * pSphere->center.z + plane.d;
        if (dist < -pSphere->radius) {
            return true; // Culled!
        }
    }

    return false;
}

// ---------------------------------------------------------------------------
// 3. Fast Overlap Box-Box Intersection (bCBox::Intersects(const bCBox&))
// ---------------------------------------------------------------------------

typedef bool (__thiscall* BoxIntersectsBox_t)(const Box3* pThis, const Box3* pOther);
static BoxIntersectsBox_t Real_BoxIntersectsBox = NULL;

static bool __fastcall Hooked_BoxIntersectsBox(const Box3* pThis, void* /*edx*/, const Box3* pOther) {
    if (!pThis || !pOther) return false;

    if (pThis->min.x > pOther->max.x || pThis->max.x < pOther->min.x) return false;
    if (pThis->min.y > pOther->max.y || pThis->max.y < pOther->min.y) return false;
    if (pThis->min.z > pOther->max.z || pThis->max.z < pOther->min.z) return false;

    return true;
}

// ---------------------------------------------------------------------------
// Initialization & Shutdown
// ---------------------------------------------------------------------------

void InitializeMathSIMDOptimizer() {
    HMODULE hSharedBase = GetModuleHandleA("SharedBase.dll");
    if (!hSharedBase) return;

    // 1. Hook bCFrustum::Cull(const bCBox&)
    void* pCullBox = (void*)GetProcAddress(hSharedBase, "?Cull@bCFrustum@@QBE_NABVbCBox@@@Z");
    if (pCullBox) {
        if (MH_CreateHook(pCullBox, (LPVOID)Hooked_FrustumCullBox, (LPVOID*)&Real_FrustumCullBox) == MH_OK) {
            MH_EnableHook(pCullBox);
            Log("G3O", "SIMD Frustum Culling hooked (?Cull@bCFrustum(bCBox)).");
        }
    }

    // 2. Hook bCFrustum::Cull(const bCSphere&)
    void* pCullSphere = (void*)GetProcAddress(hSharedBase, "?Cull@bCFrustum@@QBE_NABVbCSphere@@@Z");
    if (pCullSphere) {
        if (MH_CreateHook(pCullSphere, (LPVOID)Hooked_FrustumCullSphere, (LPVOID*)&Real_FrustumCullSphere) == MH_OK) {
            MH_EnableHook(pCullSphere);
            Log("G3O", "SIMD Frustum Culling hooked (?Cull@bCFrustum(bCSphere)).");
        }
    }

    // 3. Hook bCBox::Intersects(const bCBox&)
    void* pIntersects = (void*)GetProcAddress(hSharedBase, "?Intersects@bCBox@@QBE_NABV1@@Z");
    if (pIntersects) {
        if (MH_CreateHook(pIntersects, (LPVOID)Hooked_BoxIntersectsBox, (LPVOID*)&Real_BoxIntersectsBox) == MH_OK) {
            MH_EnableHook(pIntersects);
            Log("G3O", "SIMD Box Intersection hooked (?Intersects@bCBox).");
        }
    }
}

void ShutdownMathSIMDOptimizer() {
    HMODULE hSharedBase = GetModuleHandleA("SharedBase.dll");
    if (!hSharedBase) return;

    void* pCullBox = (void*)GetProcAddress(hSharedBase, "?Cull@bCFrustum@@QBE_NABVbCBox@@@Z");
    if (pCullBox) MH_DisableHook(pCullBox);

    void* pCullSphere = (void*)GetProcAddress(hSharedBase, "?Cull@bCFrustum@@QBE_NABVbCSphere@@@Z");
    if (pCullSphere) MH_DisableHook(pCullSphere);

    void* pIntersects = (void*)GetProcAddress(hSharedBase, "?Intersects@bCBox@@QBE_NABV1@@Z");
    if (pIntersects) MH_DisableHook(pIntersects);
}

} // namespace G3O
