#include <windows.h>
#include <iostream>
#include "dinput8_proxy.h"
#include "memory_allocator.h"
#include "lfh_enforcer.h"
#include "d3d9_hook.h"
#include "cpu_topology.h"
#include "streaming_optimizer.h"
#include "media_optimizer.h"
#include "math_simd_optimizer.h"
#include "savegame_guard.h"
#include "version_watermark.h"
#include "fall_guard.h"
#include "lru_cache_manager.h"
#include "high_memory_cache.h"
#include "physics_stabilizer.h"
#include "combat_guard.h"
#include "multicore_manager.h"
#include <minhook.h>

static DWORD WINAPI OptimaInitThread(LPVOID) {
    // 1. Initialize MinHook framework outside of loader lock
    MH_STATUS mhStatus = MH_Initialize();
    if (mhStatus != MH_OK && mhStatus != MH_ERROR_ALREADY_INITIALIZED) {
        OutputDebugStringA("[Gothic3Optima] Failed to initialize MinHook!\n");
        return 1;
    }
    OutputDebugStringA("[Gothic3Optima] MinHook initialized successfully.\n");

    // 2. Preload system dinput8 library
    Proxy::LoadSystemDirectInput();

    // 3. Lock CPU affinity to P-cores early
    InitializeCPUTopology();

    // 5. Wait until game engine libraries (SharedBase.dll, Engine.dll, FileSystem.dll, Game.dll) are mapped into memory
    int maxWaitMs = 5000;
    while ((!GetModuleHandleA("SharedBase.dll") || !GetModuleHandleA("Engine.dll") || !GetModuleHandleA("FileSystem.dll") || !GetModuleHandleA("Game.dll")) && maxWaitMs > 0) {
        Sleep(2);
        maxWaitMs -= 2;
    }

    // 6. Initialize Low Fragmentation Heap (LFH) Enforcer
    // LFHEnforcer::Initialize();

    // 7. Initialize mimalloc hooks on bCMemoryAdmin & MemHeap (Disabled: Genome Engine mixes CRT free with MemHeap)
    // MemoryOptimizer::Initialize();

    // 8. Initialize D3D9 hooks (PureDevice enforcer, State Redundancy Filter, D3DX mipmaps)
    InstallD3D9Hooks();

    // 9. Initialize Multicore Threading Engine (Phase 20)
    InitializeMulticoreManager();

    // 10. Initialize In-Memory Archive Streaming, ThreadPool Expander & Cache Anti-Thrashing
    InitializeStreamingOptimizer();

    // 10. Initialize hardware decompression (libdeflate), string hashing, FMOD tuning & precision timer
    InitializeMediaOptimizer();

    // 11. Initialize SIMD math vectorization (testing)
    // InitializeMathSIMDOptimizer();

    // 12. Initialize SaveGame Guard & Atomic Shadow-Writer
    InitializeSaveGameGuard();

    // 13. Hook Main Menu version watermark (displays "G3O v1.3.0" in main menu)
    InitializeVersionHook();

    // 14. Ground Recovery Fall Guard (Phase 12.2) - Disabled to preserve smooth character locomotion
    // InitializeFallGuard();

    // 15. Initialize Adaptive LRU Cache Eviction (Phase 13.2)
    LRUCacheManager::Initialize();

    // 16. PhysX Fixed Physics Substepping (Phase 15.1) - Disabled to prevent actor creation crash and preserve vanilla kinematics
    // InitializePhysicsStabilizer();

    // 17. Initialize Combatant Frustum Culling Exclusion (Phase 19.1) - Disabled to prevent LOD dissolve blur & floating weapon glitches
    // InitializeCombatGuard();

    OutputDebugStringA("[Gothic3Optima] All core engine subsystems initialized successfully!\n");
    return 0;
}

static void InitializeOptima() {
    // Launch background initialization thread to hook engine components as they load
    HANDLE hThread = CreateThread(nullptr, 0, OptimaInitThread, nullptr, 0, nullptr);
    if (hThread) {
        CloseHandle(hThread);
    }
}

static void ShutdownOptima() {
    OutputDebugStringA("[Gothic3Optima] Shutting down...\n");
    ShutdownCombatGuard();
    ShutdownMulticoreManager();
    // ShutdownPhysicsStabilizer();
    LRUCacheManager::Shutdown();
    // ShutdownFallGuard();
    ShutdownSaveGameGuard();
    ShutdownMediaOptimizer();
    ShutdownStreamingOptimizer();
    MemoryOptimizer::Shutdown();
    LFHEnforcer::Shutdown();
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    Proxy::UnloadSystemDirectInput();
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    switch (fdwReason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hinstDLL);
        InitializeOptima();
        break;

    case DLL_PROCESS_DETACH:
        if (!lpvReserved) { // Only if dynamically unloaded
            ShutdownOptima();
        }
        break;
    }
    return TRUE;
}
