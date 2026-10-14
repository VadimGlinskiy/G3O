#include "fall_guard.h"
#include "lru_cache_manager.h"
#include <minhook.h>
#include <math.h>

#pragma pack(push, 1)
struct GothicVector3 {
    float x;
    float y;
    float z;
};
#pragma pack(pop)

typedef void (__thiscall* SetCurrentPosition_t)(void* pThis, const GothicVector3* pPos, bool bFlag);
static SetCurrentPosition_t Real_SetCurrentPosition = NULL;

typedef bool (__thiscall* GetMovementIsControledByPlayer_t)(const void* pThis);
static GetMovementIsControledByPlayer_t Real_GetMovementIsControledByPlayer = NULL;

typedef void (__thiscall* SetLastFallVelocity_t)(void* pThis, const float* pVelocity);
static SetLastFallVelocity_t Real_SetLastFallVelocity = NULL;

static float s_lastValidX = 0.0f;
static float s_lastValidY = 0.0f;
static float s_lastValidZ = 0.0f;
static ULONGLONG s_lastGroundTime = 0;
static bool s_hasValidGround = false;
static CRITICAL_SECTION s_fallGuardLock;
static volatile DWORD g_fallRecoveries = 0;
static volatile DWORD g_multiRayRecoveries = 0;

typedef bool (__thiscall* TraceRayNearestHit_t)(
    void* pThis,
    const GothicVector3* pFrom,
    const GothicVector3* pTo,
    const void* pIgnoreEntity,
    unsigned long uCollisionGroup,
    unsigned long uQueryFlags,
    unsigned long ePSType,
    unsigned long eShapeGroup
);
static TraceRayNearestHit_t Real_TraceRayNearestHit = NULL;

static bool __fastcall Hooked_TraceRayNearestHit(
    void* pThis,
    void* /*edx*/,
    const GothicVector3* pFrom,
    const GothicVector3* pTo,
    const void* pIgnoreEntity,
    unsigned long uCollisionGroup,
    unsigned long uQueryFlags,
    unsigned long ePSType,
    unsigned long eShapeGroup
) {
    if (!Real_TraceRayNearestHit || !pThis || !pFrom || !pTo) {
        return false;
    }

    // 1. Perform primary central raycast
    bool bHit = Real_TraceRayNearestHit(pThis, pFrom, pTo, pIgnoreEntity, uCollisionGroup, uQueryFlags, ePSType, eShapeGroup);
    if (bHit) {
        return true;
    }

    // 2. Check if this is a downward ground-probing raycast
    float dy = pTo->y - pFrom->y;
    float dx = pTo->x - pFrom->x;
    float dz = pTo->z - pFrom->z;
    float horizDistSq = dx * dx + dz * dz;

    // Ground ray: downward trajectory (dy < -20.0f) and near-vertical orientation (dx^2 + dz^2 < 10000.0f = 1m radius)
    if (dy < -20.0f && horizDistSq < 10000.0f) {
        // Multi-Ray Footprint: cross cluster around foot contact area (radius 25 cm = 25.0f units)
        static const struct { float ox; float oz; } s_footprintOffsets[4] = {
            { +25.0f,   0.0f },
            { -25.0f,   0.0f },
            {   0.0f, +25.0f },
            {   0.0f, -25.0f }
        };

        for (int i = 0; i < 4; ++i) {
            GothicVector3 fromOff = { pFrom->x + s_footprintOffsets[i].ox, pFrom->y, pFrom->z + s_footprintOffsets[i].oz };
            GothicVector3 toOff   = { pTo->x   + s_footprintOffsets[i].ox, pTo->y,   pTo->z   + s_footprintOffsets[i].oz };

            if (Real_TraceRayNearestHit(pThis, &fromOff, &toOff, pIgnoreEntity, uCollisionGroup, uQueryFlags, ePSType, eShapeGroup)) {
                // Recovered solid ground contact through footprint ray cluster!
                BYTE* pSceneBytes = (BYTE*)pThis;
                void* pReport = *(void**)(pSceneBytes + 0xac);
                if (pReport) {
                    // Re-align lateral X and Z hit coordinates back to character central column
                    float* pHitX = (float*)((BYTE*)pReport + 0x68);
                    float* pHitZ = (float*)((BYTE*)pReport + 0x70);
                    *pHitX = pFrom->x;
                    *pHitZ = pFrom->z;
                }
                InterlockedIncrement((volatile LONG*)&g_multiRayRecoveries);
                return true;
            }
        }
    }

    return false;
}

static void __fastcall Hooked_SetCurrentPosition(void* pThis, void* /*edx*/, const GothicVector3* pPos, bool bFlag) {
    if (!pThis || !pPos || !Real_SetCurrentPosition) {
        if (Real_SetCurrentPosition) {
            Real_SetCurrentPosition(pThis, pPos, bFlag);
        }
        return;
    }

    // Only monitor and protect player-controlled characters
    if (Real_GetMovementIsControledByPlayer && Real_GetMovementIsControledByPlayer(pThis)) {
        ULONGLONG now = GetTickCount64();
        GothicVector3 safePos = *pPos;
        LRUCacheManager::NotifyPlayerPosition(safePos.x, safePos.y, safePos.z);

        EnterCriticalSection(&s_fallGuardLock);
        if (s_hasValidGround) {
            float dt = (float)(now - s_lastGroundTime) * 0.001f;
            float dy = pPos->y - s_lastValidY;

            // Anomaly criteria:
            // 1. Extreme 1-frame downward drop (> 6.0 meters = 600 units in < 150 ms)
            //    Normal gravity cannot accelerate a character to drop 6m in a fraction of a second.
            // 2. Void floor breach: falling below y = -15000.0f (deep underneath game world mesh)
            bool isAbnormalDrop = (dt > 0.001f && dt < 0.15f && dy < -600.0f);
            bool isVoidBreach = (pPos->y < -15000.0f);

            if (isAbnormalDrop || isVoidBreach) {
                // Ground Recovery Triggered!
                // Restore hero to last known solid ground + 50 units (0.5m) above ground
                safePos.x = s_lastValidX;
                safePos.y = s_lastValidY + 50.0f;
                safePos.z = s_lastValidZ;

                // Reset fall velocity to zero to eliminate lethal fall damage
                if (Real_SetLastFallVelocity) {
                    float zeroVel = 0.0f;
                    Real_SetLastFallVelocity(pThis, &zeroVel);
                }

                InterlockedIncrement((volatile LONG*)&g_fallRecoveries);
                OutputDebugStringA("[Gothic3Optima] Ground Recovery Fall Guard: Prevented fall-through-world! Hero restored to safe ground.\n");

                LeaveCriticalSection(&s_fallGuardLock);
                Real_SetCurrentPosition(pThis, &safePos, bFlag);
                return;
            }
        }

        // Record valid ground position during normal movement
        if (pPos->y > -10000.0f) {
            s_lastValidX = pPos->x;
            s_lastValidY = pPos->y;
            s_lastValidZ = pPos->z;
            s_lastGroundTime = now;
            s_hasValidGround = true;
        }
        LeaveCriticalSection(&s_fallGuardLock);
    }

    Real_SetCurrentPosition(pThis, pPos, bFlag);
}

void InitializeFallGuard() {
    InitializeCriticalSection(&s_fallGuardLock);

    HMODULE hEngine = GetModuleHandleA("Engine.dll");
    if (hEngine) {
        void* pTraceRay = (void*)GetProcAddress(
            hEngine, "?TraceRayNearestHit@eCPhysicsScene@@QAE_NABVbCVector@@0PBVeCEntity@@K1W4eEPropertySetType@@W4eEShapeGroup@@@Z"
        );
        if (!pTraceRay) {
            pTraceRay = (void*)((uintptr_t)hEngine + 0x16d980);
        }
        if (pTraceRay) {
            if (MH_CreateHook(pTraceRay, (LPVOID)Hooked_TraceRayNearestHit, (LPVOID*)&Real_TraceRayNearestHit) == MH_OK) {
                MH_EnableHook(pTraceRay);
                OutputDebugStringA("[Gothic3Optima] Phase 12.1: Multi-Ray Footprint active (?TraceRayNearestHit@eCPhysicsScene).\n");
            }
        }
    }

    HMODULE hGame = GetModuleHandleA("Game.dll");
    if (hGame) {
        Real_GetMovementIsControledByPlayer = (GetMovementIsControledByPlayer_t)GetProcAddress(
            hGame, "?GetMovementIsControledByPlayer@gCCharacterMovement_PS@@QBE_NXZ"
        );
        Real_SetLastFallVelocity = (SetLastFallVelocity_t)GetProcAddress(
            hGame, "?SetLastFallVelocity@gCCharacterMovement_PS@@QAEXABM@Z"
        );

        void* pSetPos = (void*)GetProcAddress(
            hGame, "?SetCurrentPosition@gCCharacterMovement_PS@@IAEXABVbCVector@@_N@Z"
        );
        if (pSetPos) {
            if (MH_CreateHook(pSetPos, (LPVOID)Hooked_SetCurrentPosition, (LPVOID*)&Real_SetCurrentPosition) == MH_OK) {
                MH_EnableHook(pSetPos);
                OutputDebugStringA("[Gothic3Optima] Phase 12.2: Ground Recovery Fall Guard active (?SetCurrentPosition@gCCharacterMovement_PS).\n");
            }
        }
    }
}

void ShutdownFallGuard() {
    DeleteCriticalSection(&s_fallGuardLock);
}
