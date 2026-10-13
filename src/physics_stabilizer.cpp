#include "physics_stabilizer.h"
#include <minhook.h>
#include <cstdio>
#include <cstdint>

// Phase 15.1: PhysX Fixed Physics Substep & Ragdoll Stabilization
// Fixes ragdoll explosions, limb stretching, and orbital launches at high framerates (>60 FPS, 120/144/240 Hz)
// and sudden framerate fluctuations (144 -> 40 FPS).

namespace {

// RVA offsets in Engine.dll
constexpr uintptr_t RVA_ECPHYSICSSCENE_SIMULATE = 0x00173E90;
constexpr uintptr_t RVA_ENGINE_CLOCK_PTR        = 0x00602A88;

// Member offsets inside eCPhysicsScene:
constexpr size_t OFFSET_PHYSICS_ENABLED  = 0x14; // bool (byte)
constexpr size_t OFFSET_NXSCENE_PTR      = 0x1C; // NxScene*
constexpr size_t OFFSET_ACCUMULATOR      = 0xA4; // float (substep accumulator)
constexpr size_t OFFSET_FIXED_DELTA      = 0xA8; // float (substep slice, default 0.01666667f)
constexpr size_t OFFSET_DISABLE_SUBSTEP  = 0xD9; // bool (byte, disable substepping flag)

// Offset inside engine clock:
constexpr size_t OFFSET_CLOCK_ELAPSED    = 0x5C0; // float (frame elapsed time in seconds)

// Physics simulation constants:
constexpr float FIXED_PHYSICS_TIMESTEP   = 1.0f / 60.0f; // 16.666667 ms (60 Hz target quantum)
constexpr float MAX_ALLOWED_FRAME_DELTA  = 0.0333334f;  // ~30 FPS floor (max 33.3ms per simulation tick)

typedef void (__thiscall *eCPhysicsScene_Simulate_t)(void* pThis);
static eCPhysicsScene_Simulate_t Real_eCPhysicsScene_Simulate = nullptr;

static void** g_ppEngineClock = nullptr;
static void*  g_pLastConfiguredNxScene = nullptr;
static bool   g_bHookInstalled = false;

// Typedef for NxScene::setTiming in PhysX 2.4.1 (vtable offset 0xAC, index 43)
// virtual void setTiming(NxReal maxTimestep = 1.0f/60.0f, NxU32 maxIter = 8, NxTimeStepMethod method = NX_TIMESTEP_FIXED);
typedef void (__thiscall *NxScene_SetTiming_t)(void* pScene, float maxTimestep, uint32_t maxIter, uint32_t method);

void __fastcall Hooked_eCPhysicsScene_Simulate(void* pThis, void* /*edx*/) {
    if (!pThis) {
        return;
    }

    // 1. Ensure substepping is NOT disabled
    BYTE* pDisableSubstepping = reinterpret_cast<BYTE*>(reinterpret_cast<char*>(pThis) + OFFSET_DISABLE_SUBSTEP);
    if (*pDisableSubstepping != 0) {
        *pDisableSubstepping = 0;
    }

    // 2. Ensure fixed delta slice is set to 60 Hz fixed quantum (16.6 ms)
    float* pFixedDelta = reinterpret_cast<float*>(reinterpret_cast<char*>(pThis) + OFFSET_FIXED_DELTA);
    if (*pFixedDelta <= 0.0f || *pFixedDelta > 0.05f) {
        *pFixedDelta = FIXED_PHYSICS_TIMESTEP;
    }

    // 3. Configure underlying PhysX 2.4.1 NxScene solver parameters
    void* pNxScene = *reinterpret_cast<void**>(reinterpret_cast<char*>(pThis) + OFFSET_NXSCENE_PTR);
    if (pNxScene && pNxScene != g_pLastConfiguredNxScene) {
        g_pLastConfiguredNxScene = pNxScene;
        void** vtable = *reinterpret_cast<void***>(pNxScene);
        if (vtable) {
            // Index 43 (0xAC / 4) is NxScene::setTiming
            NxScene_SetTiming_t setTiming = reinterpret_cast<NxScene_SetTiming_t>(vtable[0xAC / 4]);
            if (setTiming) {
                // Method 0 = NX_TIMESTEP_FIXED, maxIter = 4
                setTiming(pNxScene, FIXED_PHYSICS_TIMESTEP, 4, 0);
                OutputDebugStringA("[PhysicsStabilizer] NxScene timing locked: 60Hz fixed quantum, maxIter=4\n");
            }
        }
    }

    // 4. Bound frame delta time & accumulator to eliminate impulse explosions on framerate drops
    void* pClock = (g_ppEngineClock && *g_ppEngineClock) ? *g_ppEngineClock : nullptr;
    if (pClock) {
        float* pElapsedTime = reinterpret_cast<float*>(reinterpret_cast<char*>(pClock) + OFFSET_CLOCK_ELAPSED);
        float origElapsed = *pElapsedTime;

        // Clamp frame delta time fed to physics simulation step
        if (*pElapsedTime > MAX_ALLOWED_FRAME_DELTA) {
            *pElapsedTime = MAX_ALLOWED_FRAME_DELTA;
        }

        // Clamp accumulator backlog so excess time never produces more than 2 substeps in one frame
        float* pAccumulator = reinterpret_cast<float*>(reinterpret_cast<char*>(pThis) + OFFSET_ACCUMULATOR);
        float maxAllowedAcc = MAX_ALLOWED_FRAME_DELTA - *pElapsedTime;
        if (maxAllowedAcc < 0.0f) {
            maxAllowedAcc = 0.0f;
        }
        if (*pAccumulator > maxAllowedAcc) {
            *pAccumulator = maxAllowedAcc;
        }
        if (*pAccumulator < 0.0f) {
            *pAccumulator = 0.0f;
        }

        Real_eCPhysicsScene_Simulate(pThis);

        // Restore real elapsed time for uninhibited render pacing
        *pElapsedTime = origElapsed;
    } else {
        Real_eCPhysicsScene_Simulate(pThis);
    }
}

} // namespace

void InitializePhysicsStabilizer() {
    HMODULE hEngine = GetModuleHandleA("Engine.dll");
    if (!hEngine) {
        OutputDebugStringA("[PhysicsStabilizer] Engine.dll not loaded, skipping.\n");
        return;
    }

    uintptr_t pSimulate = reinterpret_cast<uintptr_t>(hEngine) + RVA_ECPHYSICSSCENE_SIMULATE;
    g_ppEngineClock = reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(hEngine) + RVA_ENGINE_CLOCK_PTR);

    MH_STATUS status = MH_CreateHook(
        reinterpret_cast<LPVOID>(pSimulate),
        reinterpret_cast<LPVOID>(&Hooked_eCPhysicsScene_Simulate),
        reinterpret_cast<LPVOID*>(&Real_eCPhysicsScene_Simulate)
    );

    if (status == MH_OK) {
        MH_EnableHook(reinterpret_cast<LPVOID>(pSimulate));
        g_bHookInstalled = true;
        OutputDebugStringA("[PhysicsStabilizer] Hooked eCPhysicsScene::Simulate at Engine.dll+0x173E90 (Fixed Physics Substep active)\n");
    } else {
        char buf[128];
        sprintf_s(buf, "[PhysicsStabilizer] Failed to hook eCPhysicsScene::Simulate (MH_STATUS: %d)\n", status);
        OutputDebugStringA(buf);
    }
}

void ShutdownPhysicsStabilizer() {
    if (g_bHookInstalled) {
        HMODULE hEngine = GetModuleHandleA("Engine.dll");
        if (hEngine) {
            uintptr_t pSimulate = reinterpret_cast<uintptr_t>(hEngine) + RVA_ECPHYSICSSCENE_SIMULATE;
            MH_DisableHook(reinterpret_cast<LPVOID>(pSimulate));
        }
        g_bHookInstalled = false;
        g_pLastConfiguredNxScene = nullptr;
    }
}
