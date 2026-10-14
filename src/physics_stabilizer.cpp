#include "physics_stabilizer.h"
#include <minhook.h>
#include <cstdio>
#include <cstdint>
#include <cmath>

// Phase 15.1: PhysX Fixed Physics Substep
// Phase 15.2: Damping & Clamping of Anomalous Bone Velocities (Ragdoll Stability)
// Eliminates ragdoll explosions, limb stretching, infinite NaN spinning, and orbital launches.

namespace {

// RVA offsets in Engine.dll
constexpr uintptr_t RVA_ECPHYSICSSCENE_SIMULATE   = 0x00173E90;
constexpr uintptr_t RVA_ECPHYSICSSCENE_CREATEACTOR = 0x0016F470;
constexpr uintptr_t RVA_ENGINE_CLOCK_PTR          = 0x00602A88;

// Member offsets inside eCPhysicsScene:
constexpr size_t OFFSET_PHYSICS_ENABLED  = 0x14; // bool (byte)
constexpr size_t OFFSET_NXSCENE_PTR      = 0x1C; // NxScene*
constexpr size_t OFFSET_SKELETON_ARRAY   = 0x4C; // eCPhysicObject** (skeleton list)
constexpr size_t OFFSET_SKELETON_COUNT   = 0x50; // int (skeleton count)
constexpr size_t OFFSET_ACCUMULATOR      = 0xA4; // float (substep accumulator)
constexpr size_t OFFSET_FIXED_DELTA      = 0xA8; // float (substep slice, default 0.01666667f)
constexpr size_t OFFSET_DISABLE_SUBSTEP  = 0xD9; // bool (byte, disable substepping flag)

// Member offsets inside eCPhysicObject (ragdoll / skeleton):
constexpr size_t OFFSET_OBJECT_BONE_ACTORS = 0x10; // NxActor** (array of bone actors)
constexpr size_t OFFSET_OBJECT_BONE_COUNT  = 0x20; // int (bone count)

// Offset inside engine clock:
constexpr size_t OFFSET_CLOCK_ELAPSED    = 0x5C0; // float (frame elapsed time in seconds)

// Physics simulation constants:
constexpr float FIXED_PHYSICS_TIMESTEP      = 1.0f / 60.0f; // 16.666667 ms (60 Hz target quantum)
constexpr float MAX_ALLOWED_FRAME_DELTA     = 0.0333334f;  // ~30 FPS floor (max 33.3ms per simulation tick)

// Bone velocity clamping limits (Phase 15.2):
constexpr float MAX_BONE_LINEAR_VELOCITY    = 20.0f;        // 20 m/s (~72 km/h max linear speed for a ragdoll bone)
constexpr float MAX_BONE_LINEAR_VELOCITY_SQ = MAX_BONE_LINEAR_VELOCITY * MAX_BONE_LINEAR_VELOCITY;
constexpr float MAX_BONE_ANGULAR_VELOCITY   = 12.0f;        // 12 rad/s (~1.9 rev/s max rotational spin)
constexpr float MAX_BONE_ANGULAR_VELOCITY_SQ= MAX_BONE_ANGULAR_VELOCITY * MAX_BONE_ANGULAR_VELOCITY;

// Damping constants:
constexpr float DEFAULT_BONE_LINEAR_DAMPING  = 0.20f; // Linear air drag
constexpr float DEFAULT_BONE_ANGULAR_DAMPING = 0.25f; // Rotational air drag

struct NxVec3 {
    float x;
    float y;
    float z;
};

// Typedefs for NxActor virtual methods (PhysX 2.4.1 vtable):
// vtbl[47] (0xBC): setLinearDamping(float)
// vtbl[49] (0xC4): setAngularDamping(float)
// vtbl[51] (0xCC): setLinearVelocity(const NxVec3&)
// vtbl[52] (0xD0): setAngularVelocity(const NxVec3&)
// vtbl[53] (0xD4): getLinearVelocity(NxVec3*)
// vtbl[54] (0xD8): getAngularVelocity(NxVec3*)
// vtbl[55] (0xDC): setMaxAngularVelocity(float)
// vtbl[77] (0x134): isSleeping()
typedef void (__thiscall *NxActor_SetDamping_t)(void* pActor, float damping);
typedef void (__thiscall *NxActor_SetVelocity_t)(void* pActor, const NxVec3* inVec);
typedef void (__thiscall *NxActor_GetVelocity_t)(void* pActor, NxVec3* outVec);
typedef void (__thiscall *NxActor_SetMaxAngVel_t)(void* pActor, float maxAngVel);
typedef bool (__thiscall *NxActor_IsSleeping_t)(void* pActor);

// Typedefs for Engine.dll hooks:
typedef void (__thiscall *eCPhysicsScene_Simulate_t)(void* pThis);
static eCPhysicsScene_Simulate_t Real_eCPhysicsScene_Simulate = nullptr;

typedef void* (__thiscall *eCPhysicsScene_CreateActor_t)(void* pThis, void* pDesc);
static eCPhysicsScene_CreateActor_t Real_eCPhysicsScene_CreateActor = nullptr;

// Typedef for NxScene::setTiming in PhysX 2.4.1 (vtable offset 0xAC, index 43)
typedef void (__thiscall *NxScene_SetTiming_t)(void* pScene, float maxTimestep, uint32_t maxIter, uint32_t method);

static void** g_ppEngineClock = nullptr;
static void*  g_pLastConfiguredNxScene = nullptr;
static bool   g_bSimulateHookInstalled = false;
static bool   g_bCreateActorHookInstalled = false;

// Clamp anomalous linear and angular velocities across all active ragdoll bones
static void ClampSkeletonBoneVelocities(void* pPhysicsScene) {
    if (!pPhysicsScene) return;

    int skeletonCount = *reinterpret_cast<int*>(reinterpret_cast<char*>(pPhysicsScene) + OFFSET_SKELETON_COUNT);
    void** skeletonArray = *reinterpret_cast<void***>(reinterpret_cast<char*>(pPhysicsScene) + OFFSET_SKELETON_ARRAY);

    if (!skeletonArray || skeletonCount <= 0 || skeletonCount > 2048) {
        return;
    }

    for (int i = 0; i < skeletonCount; ++i) {
        void* pSkeleton = skeletonArray[i];
        if (!pSkeleton) continue;

        int boneCount = *reinterpret_cast<int*>(reinterpret_cast<char*>(pSkeleton) + OFFSET_OBJECT_BONE_COUNT);
        void** boneActors = *reinterpret_cast<void***>(reinterpret_cast<char*>(pSkeleton) + OFFSET_OBJECT_BONE_ACTORS);

        if (!boneActors || boneCount <= 0 || boneCount > 256) {
            continue;
        }

        for (int b = 0; b < boneCount; ++b) {
            void* pActor = boneActors[b];
            if (!pActor) continue;

            void** vtable = *reinterpret_cast<void***>(pActor);
            if (!vtable) continue;

            // Check if bone is currently sleeping; if so, skip calculation
            NxActor_IsSleeping_t isSleeping = reinterpret_cast<NxActor_IsSleeping_t>(vtable[77]);
            if (isSleeping && isSleeping(pActor)) {
                continue;
            }

            // 1. Audit and clamp linear velocity
            NxActor_GetVelocity_t getLinVel = reinterpret_cast<NxActor_GetVelocity_t>(vtable[53]);
            NxActor_SetVelocity_t setLinVel = reinterpret_cast<NxActor_SetVelocity_t>(vtable[51]);

            if (getLinVel && setLinVel) {
                NxVec3 linVel{ 0.0f, 0.0f, 0.0f };
                getLinVel(pActor, &linVel);

                // Sanitize NaN / Inf degenerate states
                if (!std::isfinite(linVel.x) || !std::isfinite(linVel.y) || !std::isfinite(linVel.z)) {
                    linVel = { 0.0f, 0.0f, 0.0f };
                    setLinVel(pActor, &linVel);
                } else {
                    float speedSq = linVel.x * linVel.x + linVel.y * linVel.y + linVel.z * linVel.z;
                    if (speedSq > MAX_BONE_LINEAR_VELOCITY_SQ) {
                        float speed = std::sqrt(speedSq);
                        float scale = MAX_BONE_LINEAR_VELOCITY / speed;
                        linVel.x *= scale;
                        linVel.y *= scale;
                        linVel.z *= scale;
                        setLinVel(pActor, &linVel);
                    }
                }
            }

            // 2. Audit and clamp angular velocity
            NxActor_GetVelocity_t getAngVel = reinterpret_cast<NxActor_GetVelocity_t>(vtable[54]);
            NxActor_SetVelocity_t setAngVel = reinterpret_cast<NxActor_SetVelocity_t>(vtable[52]);

            if (getAngVel && setAngVel) {
                NxVec3 angVel{ 0.0f, 0.0f, 0.0f };
                getAngVel(pActor, &angVel);

                if (!std::isfinite(angVel.x) || !std::isfinite(angVel.y) || !std::isfinite(angVel.z)) {
                    angVel = { 0.0f, 0.0f, 0.0f };
                    setAngVel(pActor, &angVel);
                } else {
                    float angSpeedSq = angVel.x * angVel.x + angVel.y * angVel.y + angVel.z * angVel.z;
                    if (angSpeedSq > MAX_BONE_ANGULAR_VELOCITY_SQ) {
                        float angSpeed = std::sqrt(angSpeedSq);
                        float scale = MAX_BONE_ANGULAR_VELOCITY / angSpeed;
                        angVel.x *= scale;
                        angVel.y *= scale;
                        angVel.z *= scale;
                        setAngVel(pActor, &angVel);
                    }
                }
            }
        }
    }
}

// Hook on eCPhysicsScene::CreateActor: apply damping and maximum angular velocity upon actor creation
void* __fastcall Hooked_eCPhysicsScene_CreateActor(void* pThis, void* /*edx*/, void* pDesc) {
    if (pDesc) {
        // Offset 0x30 in NxActorDesc is NxBodyDesc*
        void* pBodyDesc = *reinterpret_cast<void**>(reinterpret_cast<char*>(pDesc) + 0x30);
        if (pBodyDesc) {
            float* pLinDamp = reinterpret_cast<float*>(reinterpret_cast<char*>(pBodyDesc) + 0x58);
            float* pAngDamp = reinterpret_cast<float*>(reinterpret_cast<char*>(pBodyDesc) + 0x5C);
            float* pMaxAng  = reinterpret_cast<float*>(reinterpret_cast<char*>(pBodyDesc) + 0x3C);

            if (*pLinDamp < DEFAULT_BONE_LINEAR_DAMPING) {
                *pLinDamp = DEFAULT_BONE_LINEAR_DAMPING;
            }
            if (*pAngDamp < DEFAULT_BONE_ANGULAR_DAMPING) {
                *pAngDamp = DEFAULT_BONE_ANGULAR_DAMPING;
            }
            if (*pMaxAng > MAX_BONE_ANGULAR_VELOCITY || *pMaxAng <= 0.0f) {
                *pMaxAng = MAX_BONE_ANGULAR_VELOCITY;
            }
        }
    }

    void* pActor = Real_eCPhysicsScene_CreateActor(pThis, pDesc);

    // Enforce damping directly on the instantiated NxActor
    if (pActor && pDesc && *reinterpret_cast<void**>(reinterpret_cast<char*>(pDesc) + 0x30)) {
        void** vtbl = *reinterpret_cast<void***>(pActor);
        if (vtbl) {
            NxActor_SetDamping_t setLinDamp = reinterpret_cast<NxActor_SetDamping_t>(vtbl[47]);
            NxActor_SetDamping_t setAngDamp = reinterpret_cast<NxActor_SetDamping_t>(vtbl[49]);
            NxActor_SetMaxAngVel_t setMaxAng = reinterpret_cast<NxActor_SetMaxAngVel_t>(vtbl[55]);

            if (setLinDamp) setLinDamp(pActor, DEFAULT_BONE_LINEAR_DAMPING);
            if (setAngDamp) setAngDamp(pActor, DEFAULT_BONE_ANGULAR_DAMPING);
            if (setMaxAng)  setMaxAng(pActor, MAX_BONE_ANGULAR_VELOCITY);
        }
    }

    return pActor;
}

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

    // 5. Phase 15.2: Clamp anomalous bone velocities and suppress NaN spikes across all active ragdoll skeletons
    ClampSkeletonBoneVelocities(pThis);
}

} // namespace

void InitializePhysicsStabilizer() {
    HMODULE hEngine = GetModuleHandleA("Engine.dll");
    if (!hEngine) {
        OutputDebugStringA("[PhysicsStabilizer] Engine.dll not loaded, skipping.\n");
        return;
    }

    uintptr_t pSimulate = reinterpret_cast<uintptr_t>(hEngine) + RVA_ECPHYSICSSCENE_SIMULATE;
    uintptr_t pCreateActor = reinterpret_cast<uintptr_t>(hEngine) + RVA_ECPHYSICSSCENE_CREATEACTOR;
    g_ppEngineClock = reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(hEngine) + RVA_ENGINE_CLOCK_PTR);

    // 1. Hook eCPhysicsScene::Simulate
    MH_STATUS status = MH_CreateHook(
        reinterpret_cast<LPVOID>(pSimulate),
        reinterpret_cast<LPVOID>(&Hooked_eCPhysicsScene_Simulate),
        reinterpret_cast<LPVOID*>(&Real_eCPhysicsScene_Simulate)
    );

    if (status == MH_OK) {
        MH_EnableHook(reinterpret_cast<LPVOID>(pSimulate));
        g_bSimulateHookInstalled = true;
        OutputDebugStringA("[PhysicsStabilizer] Hooked eCPhysicsScene::Simulate at Engine.dll+0x173E90 (Fixed Physics Substep & Velocity Clamp active)\n");
    } else {
        char buf[128];
        sprintf_s(buf, "[PhysicsStabilizer] Failed to hook eCPhysicsScene::Simulate (MH_STATUS: %d)\n", status);
        OutputDebugStringA(buf);
    }

    // 2. Hook eCPhysicsScene::CreateActor
    status = MH_CreateHook(
        reinterpret_cast<LPVOID>(pCreateActor),
        reinterpret_cast<LPVOID>(&Hooked_eCPhysicsScene_CreateActor),
        reinterpret_cast<LPVOID*>(&Real_eCPhysicsScene_CreateActor)
    );

    if (status == MH_OK) {
        MH_EnableHook(reinterpret_cast<LPVOID>(pCreateActor));
        g_bCreateActorHookInstalled = true;
        OutputDebugStringA("[PhysicsStabilizer] Hooked eCPhysicsScene::CreateActor at Engine.dll+0x16F470 (Ragdoll Damping Enforcer active)\n");
    } else {
        char buf[128];
        sprintf_s(buf, "[PhysicsStabilizer] Failed to hook eCPhysicsScene::CreateActor (MH_STATUS: %d)\n", status);
        OutputDebugStringA(buf);
    }
}

void ShutdownPhysicsStabilizer() {
    HMODULE hEngine = GetModuleHandleA("Engine.dll");
    if (hEngine) {
        if (g_bSimulateHookInstalled) {
            uintptr_t pSimulate = reinterpret_cast<uintptr_t>(hEngine) + RVA_ECPHYSICSSCENE_SIMULATE;
            MH_DisableHook(reinterpret_cast<LPVOID>(pSimulate));
            g_bSimulateHookInstalled = false;
        }
        if (g_bCreateActorHookInstalled) {
            uintptr_t pCreateActor = reinterpret_cast<uintptr_t>(hEngine) + RVA_ECPHYSICSSCENE_CREATEACTOR;
            MH_DisableHook(reinterpret_cast<LPVOID>(pCreateActor));
            g_bCreateActorHookInstalled = false;
        }
    }
    g_pLastConfiguredNxScene = nullptr;
}
