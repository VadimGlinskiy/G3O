#include "streaming_optimizer.h"
#include <minhook.h>
#include <windows.h>
#include <stdio.h>
#include <math.h>

// ---------------------------------------------------------------------------
// Predictive World Cell Prefetching by Motion Vector (Phase 11.3)
// Intercepts Gothic 3's PVS (Potentially Visible Set) scene queries in Engine.dll
// and smoothly advances the query sphere along the player's movement vector.
// This triggers cell loading ~1.8 seconds before the player reaches boundaries,
// eliminating streaming stutters without invasive OS file-system hooks.
// ---------------------------------------------------------------------------

#pragma pack(push, 1)
struct GothicSphere {
    float radius;
    float x;
    float y;
    float z;
};
#pragma pack(pop)

typedef void (__thiscall* PVSPrefetcher_Execute_t)(void* pThis, void* pDb, const void* pSphere);
static PVSPrefetcher_Execute_t Real_PVSPrefetcher_Execute = NULL;

static float s_lastX = 0.0f;
static float s_lastZ = 0.0f;
static ULONGLONG s_lastTime = 0;
static bool s_hasHistory = false;
static CRITICAL_SECTION s_motionCs;
static volatile DWORD g_prefetchPredictions = 0;

static void __fastcall Hooked_PVSPrefetcher_Execute(void* pThis, void* /*edx*/, void* pDb, const void* pSphereRaw) {
    if (!pSphereRaw || !pDb || !Real_PVSPrefetcher_Execute) {
        if (Real_PVSPrefetcher_Execute) {
            Real_PVSPrefetcher_Execute(pThis, pDb, pSphereRaw);
        }
        return;
    }

    const GothicSphere* pSphere = (const GothicSphere*)pSphereRaw;
    GothicSphere predictedSphere = *pSphere;

    ULONGLONG now = GetTickCount64();

    EnterCriticalSection(&s_motionCs);
    if (s_hasHistory && now > s_lastTime) {
        float dt = (float)(now - s_lastTime) * 0.001f;
        // Check reasonable delta frame time
        if (dt >= 0.008f && dt <= 1.0f) {
            float dx = pSphere->x - s_lastX;
            float dz = pSphere->z - s_lastZ;
            float distSq = dx * dx + dz * dz;

            // Teleport check: if player jumped > 100 meters (10,000 units), reset history
            if (distSq < 100000000.0f) {
                float dist = sqrtf(distSq);
                float speed = dist / dt; // units per second (1m/s = 100 units/s)

                // If moving faster than ~0.8 m/s
                if (speed > 80.0f && dist > 1.0f) {
                    float dirX = dx / dist;
                    float dirZ = dz / dist;

                    // Lead distance: ~1.8 seconds ahead of moving player/camera
                    // Normal run (600 u/s) -> ~1080 units (10.8m) lead
                    // Sprint (900 u/s) -> ~1620 units (16.2m) lead
                    float lead = speed * 1.8f;
                    if (lead > 3000.0f) lead = 3000.0f;
                    if (lead < 400.0f) lead = 400.0f;

                    predictedSphere.x += dirX * lead;
                    predictedSphere.z += dirZ * lead;

                    // Expand radius by 15% in direction of travel to prefetch cell boundaries
                    predictedSphere.radius = pSphere->radius * 1.15f;

                    InterlockedIncrement((volatile LONG*)&g_prefetchPredictions);
                }
            } else {
                s_hasHistory = false;
            }
        }
    }

    s_lastX = pSphere->x;
    s_lastZ = pSphere->z;
    s_lastTime = now;
    s_hasHistory = true;
    LeaveCriticalSection(&s_motionCs);

    Real_PVSPrefetcher_Execute(pThis, pDb, &predictedSphere);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void InitializeStreamingOptimizer() {
    InitializeCriticalSection(&s_motionCs);

    // Hook eCPVSPrefetcher3::Execute in Engine.dll (Phase 11.3)
    HMODULE hEngine = GetModuleHandleA("Engine.dll");
    if (hEngine) {
        void* pExecute = (void*)GetProcAddress(hEngine, "?Execute@eCPVSPrefetcher3@@IAEXAAVeCFrustumDatabase@@ABVbCSphere@@@Z");
        if (pExecute) {
            if (MH_CreateHook(pExecute, (LPVOID)Hooked_PVSPrefetcher_Execute, (LPVOID*)&Real_PVSPrefetcher_Execute) == MH_OK) {
                MH_EnableHook(pExecute);
                OutputDebugStringA("[Gothic3Optima] Phase 11.3: Predictive World Cell Prefetching active (?Execute@eCPVSPrefetcher3).\n");
            }
        }
    }
}

void ShutdownStreamingOptimizer() {
    DeleteCriticalSection(&s_motionCs);
}
