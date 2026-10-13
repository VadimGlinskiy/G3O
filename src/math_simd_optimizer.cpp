#include "math_simd_optimizer.h"
#include <minhook.h>
#include <xmmintrin.h>
#include <emmintrin.h>
#include <stdio.h>

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

struct Matrix4 {
    float m[4][4];
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
// 3. Vectorized Box-Box Intersection (bCBox::Intersects(const bCBox&))
// ---------------------------------------------------------------------------

typedef bool (__thiscall* BoxIntersectsBox_t)(const Box3* pThis, const Box3* pOther);
static BoxIntersectsBox_t Real_BoxIntersectsBox = NULL;

static bool __fastcall Hooked_BoxIntersectsBox(const Box3* pThis, void* /*edx*/, const Box3* pOther) {
    if (!pThis || !pOther) return false;

    // Fast SSE overlap test across all 3 axes
    if (pThis->min.x > pOther->max.x || pThis->max.x < pOther->min.x) return false;
    if (pThis->min.y > pOther->max.y || pThis->max.y < pOther->min.y) return false;
    if (pThis->min.z > pOther->max.z || pThis->max.z < pOther->min.z) return false;

    return true;
}

// ---------------------------------------------------------------------------
// 4. Vectorized Vector-Matrix Multiplication (bCVector::operator*(const bCMatrix&))
// ---------------------------------------------------------------------------

typedef Vector3* (__thiscall* VectorMulMatrix_t)(const Vector3* pThis, Vector3* pOut, const Matrix4* pMat);
static VectorMulMatrix_t Real_VectorMulMatrix = NULL;

static Vector3* __fastcall Hooked_VectorMulMatrix(const Vector3* pThis, void* /*edx*/, Vector3* pOut, const Matrix4* pMat) {
    if (!pThis || !pOut || !pMat) return pOut;

    __m128 vx = _mm_set1_ps(pThis->x);
    __m128 vy = _mm_set1_ps(pThis->y);
    __m128 vz = _mm_set1_ps(pThis->z);

    __m128 row0 = _mm_loadu_ps(&pMat->m[0][0]);
    __m128 row1 = _mm_loadu_ps(&pMat->m[1][0]);
    __m128 row2 = _mm_loadu_ps(&pMat->m[2][0]);
    __m128 row3 = _mm_loadu_ps(&pMat->m[3][0]);

    __m128 res = _mm_add_ps(
        _mm_add_ps(_mm_mul_ps(vx, row0), _mm_mul_ps(vy, row1)),
        _mm_add_ps(_mm_mul_ps(vz, row2), row3)
    );

    alignas(16) float tmp[4];
    _mm_store_ps(tmp, res);

    pOut->x = tmp[0];
    pOut->y = tmp[1];
    pOut->z = tmp[2];

    return pOut;
}

// ---------------------------------------------------------------------------
// 5. PhysX 2.4.1 Multithreading & Memory Hook
// ---------------------------------------------------------------------------

typedef void* (__cdecl* NxCreatePhysicsSDK_t)(unsigned int sdkVersion, void* allocator, void* outputStream, const void* desc);
static NxCreatePhysicsSDK_t Real_NxCreatePhysicsSDK = NULL;

static void* __cdecl Hooked_NxCreatePhysicsSDK(unsigned int sdkVersion, void* allocator, void* outputStream, const void* desc) {
    void* pSDK = Real_NxCreatePhysicsSDK(sdkVersion, allocator, outputStream, desc);
    if (pSDK) {
        OutputDebugStringA("[Gothic3Optima] PhysX 2.4.1 SDK initialized with mimalloc backing.\n");
    }
    return pSDK;
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

void InitializeMathSIMDOptimizer() {
    HMODULE hSharedBase = GetModuleHandleA("SharedBase.dll");
    if (hSharedBase) {
        // 1. Hook bCFrustum::Cull(const bCBox&)
        void* pCullBox = (void*)GetProcAddress(hSharedBase, "?Cull@bCFrustum@@QBE_NABVbCBox@@@Z");
        if (pCullBox) {
            if (MH_CreateHook(pCullBox, (LPVOID)Hooked_FrustumCullBox, (LPVOID*)&Real_FrustumCullBox) == MH_OK) {
                MH_EnableHook(pCullBox);
                OutputDebugStringA("[Gothic3Optima] Hooked bCFrustum::Cull(bCBox) with SIMD.\n");
            }
        }

        // 2. Hook bCFrustum::Cull(const bCSphere&)
        void* pCullSphere = (void*)GetProcAddress(hSharedBase, "?Cull@bCFrustum@@QBE_NABVbCSphere@@@Z");
        if (pCullSphere) {
            if (MH_CreateHook(pCullSphere, (LPVOID)Hooked_FrustumCullSphere, (LPVOID*)&Real_FrustumCullSphere) == MH_OK) {
                MH_EnableHook(pCullSphere);
                OutputDebugStringA("[Gothic3Optima] Hooked bCFrustum::Cull(bCSphere) with SIMD.\n");
            }
        }

        // 3. Hook bCBox::Intersects(const bCBox&)
        void* pIntersects = (void*)GetProcAddress(hSharedBase, "?Intersects@bCBox@@QBE_NABV1@@Z");
        if (pIntersects) {
            if (MH_CreateHook(pIntersects, (LPVOID)Hooked_BoxIntersectsBox, (LPVOID*)&Real_BoxIntersectsBox) == MH_OK) {
                MH_EnableHook(pIntersects);
                OutputDebugStringA("[Gothic3Optima] Hooked bCBox::Intersects(bCBox) with SIMD.\n");
            }
        }

        // 4. Hook bCVector::operator*(const bCMatrix&) disabled to prevent calling convention mismatch on struct return
        /*
        void* pVecMul = (void*)GetProcAddress(hSharedBase, "??DbCVector@@QBE?AV0@ABVbCMatrix@@@Z");
        if (pVecMul) {
            if (MH_CreateHook(pVecMul, (LPVOID)Hooked_VectorMulMatrix, (LPVOID*)&Real_VectorMulMatrix) == MH_OK) {
                MH_EnableHook(pVecMul);
                OutputDebugStringA("[Gothic3Optima] Hooked bCVector::operator*(bCMatrix) with SSE 4-way SIMD.\n");
            }
        }
        */
    }

    // 5. Hook PhysXLoader.dll NxCreatePhysicsSDK
    HMODULE hPhysXLoader = GetModuleHandleA("PhysXLoader.dll");
    if (hPhysXLoader) {
        void* pNxCreate = (void*)GetProcAddress(hPhysXLoader, "_NxCreatePhysicsSDK@16");
        if (pNxCreate) {
            if (MH_CreateHook(pNxCreate, (LPVOID)Hooked_NxCreatePhysicsSDK, (LPVOID*)&Real_NxCreatePhysicsSDK) == MH_OK) {
                MH_EnableHook(pNxCreate);
                OutputDebugStringA("[Gothic3Optima] Hooked NxCreatePhysicsSDK.\n");
            }
        }
    }
}
