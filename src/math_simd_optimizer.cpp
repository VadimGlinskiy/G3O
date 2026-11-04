#include "math_simd_optimizer.h"
#include "diagnostic_logger.h"
#include <minhook.h>
#include <cmath>
#include <xmmintrin.h>
#include <emmintrin.h>
#include <stdio.h>

namespace G3O {

#pragma pack(push, 1)
struct Vector3 {
    float x;
    float y;
    float z;
};

struct Box3 {
    Vector3 min; // offset 0x00
    Vector3 max; // offset 0x0C
};

struct Sphere3 {
    float   radius; // offset 0x00 (verified from SharedBase.dll!AccessRadius)
    Vector3 center; // offset 0x04 (verified from SharedBase.dll!AccessPosition)
};

struct Plane4 {
    float a; // offset 0x00
    float b; // offset 0x04
    float c; // offset 0x08
    float d; // offset 0x0C
};
#pragma pack(pop)

// ---------------------------------------------------------------------------
// 1. Vectorized Frustum Culling for Bounding Box (bCFrustum::Cull / Intersects)
// In Gothic 3 SharedBase.dll:
// - bCFrustum stores 6 planes starting at offset 0x00.
// - ?Cull@bCFrustum@@QBE_NABVbCBox@@@Z (0x10036380) and
//   ?Intersects@bCFrustum@@QBE_NABVbCBox@@@Z (0x100368D0) are IDENTICAL.
// - Return convention:
//   TRUE (1)  = Box INTERSECTS frustum (visible, keep for rendering).
//   FALSE (0) = Box is completely OUTSIDE frustum (culled, do not render).
// ---------------------------------------------------------------------------

typedef bool (__thiscall* FrustumCullBox_t)(void* pThis, const Box3* pBox);
static FrustumCullBox_t Real_FrustumCullBox = NULL;

static bool __fastcall Hooked_FrustumCullBox(void* pThis, void* /*edx*/, const Box3* pBox) {
    if (!pThis || !pBox) return false;

    // The 6 bounding planes of the camera frustum start at offset 0x00
    const Plane4* planes = (const Plane4*)pThis;

    // Center and half-extents of the bounding box
    float cx = (pBox->min.x + pBox->max.x) * 0.5f;
    float cy = (pBox->min.y + pBox->max.y) * 0.5f;
    float cz = (pBox->min.z + pBox->max.z) * 0.5f;

    float ex = fabsf(pBox->max.x - pBox->min.x) * 0.5f;
    float ey = fabsf(pBox->max.y - pBox->min.y) * 0.5f;
    float ez = fabsf(pBox->max.z - pBox->min.z) * 0.5f;

    for (int i = 0; i < 6; ++i) {
        const Plane4& p = planes[i];

        // Signed distance from center of box to plane: dot(N, C) + d
        float dist = p.a * cx + p.b * cy + p.c * cz + p.d;

        // Effective projection radius of box extents onto plane normal: dot(|N|, E)
        float radius = fabsf(p.a) * ex + fabsf(p.b) * ey + fabsf(p.c) * ez;

        // If dist + radius < 0, all 8 vertices of the box are behind this plane -> OUTSIDE (CULLED)!
        if (dist + radius < 0.0f) {
            return false; // Culled (not visible)
        }
    }

    return true; // Inside or intersecting (visible)
}

// ---------------------------------------------------------------------------
// 2. Vectorized Frustum Culling for Sphere (bCFrustum::Cull / Intersects)
// Return convention:
// TRUE (1)  = Sphere INTERSECTS frustum (visible).
// FALSE (0) = Sphere is completely OUTSIDE frustum (culled).
// ---------------------------------------------------------------------------

typedef bool (__thiscall* FrustumCullSphere_t)(void* pThis, const Sphere3* pSphere);
static FrustumCullSphere_t Real_FrustumCullSphere = NULL;

static bool __fastcall Hooked_FrustumCullSphere(void* pThis, void* /*edx*/, const Sphere3* pSphere) {
    if (!pThis || !pSphere) return false;

    const Plane4* planes = (const Plane4*)pThis;

    for (int i = 0; i < 6; ++i) {
        const Plane4& p = planes[i];

        float dist = p.a * pSphere->center.x + p.b * pSphere->center.y + p.c * pSphere->center.z + p.d;

        // If distance from center to plane is less than -radius -> completely outside
        if (dist + pSphere->radius < 0.0f) {
            return false; // Culled
        }
    }

    return true; // Visible
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

    // 1. Hook bCFrustum::Cull(const bCBox&) at 0x10036380
    void* pCullBox = (void*)GetProcAddress(hSharedBase, "?Cull@bCFrustum@@QBE_NABVbCBox@@@Z");
    if (pCullBox) {
        if (MH_CreateHook(pCullBox, (LPVOID)Hooked_FrustumCullBox, (LPVOID*)&Real_FrustumCullBox) == MH_OK) {
            MH_EnableHook(pCullBox);
            Log("G3O", "SIMD Frustum Culling hooked (?Cull@bCFrustum(bCBox)).");
        }
    }

    // 2. Hook bCFrustum::Cull(const bCSphere&) at 0x100362D0
    void* pCullSphere = (void*)GetProcAddress(hSharedBase, "?Cull@bCFrustum@@QBE_NABVbCSphere@@@Z");
    if (pCullSphere) {
        if (MH_CreateHook(pCullSphere, (LPVOID)Hooked_FrustumCullSphere, (LPVOID*)&Real_FrustumCullSphere) == MH_OK) {
            MH_EnableHook(pCullSphere);
            Log("G3O", "SIMD Frustum Culling hooked (?Cull@bCFrustum(bCSphere)).");
        }
    }

    // 3. Hook bCBox::Intersects(const bCBox&) at 0x1001C830
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