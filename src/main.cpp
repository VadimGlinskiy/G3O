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
#include <minhook.h>

static DWORD WINAPI OptimaInitThread(LPVOID) {
    // 1. Lock CPU affinity to P-cores early
    InitializeCPUTopology();

    // 2. Wait until game engine libraries (SharedBase.dll, Engine.dll, FileSystem.dll, Game.dll) are mapped into memory
    int maxWaitMs = 5000;
    while ((!GetModuleHandleA("SharedBase.dll") || !GetModuleHandleA("Engine.dll") || !GetModuleHandleA("FileSystem.dll") || !GetModuleHandleA("Game.dll")) && maxWaitMs > 0) {
        Sleep(2);
        maxWaitMs -= 2;
    }

    // 3. Initialize Low Fragmentation Heap (LFH) Enforcer
    LFHEnforcer::Initialize();

    // 4. Initialize mimalloc hooks on bCMemoryAdmin & MemHeap (testing)
    // MemoryOptimizer::Initialize();

    // 5. Initialize D3D9 hooks (PureDevice enforcer, State Redundancy Filter, D3DX mipmaps)
    InstallD3D9Hooks();

    // 6. Initialize In-Memory Archive Streaming, ThreadPool Expander & Cache Anti-Thrashing
    InitializeStreamingOptimizer();

    // 7. Initialize hardware decompression (libdeflate), string hashing, FMOD tuning & precision timer
    InitializeMediaOptimizer();

    // 8. Initialize SIMD math vectorization (testing)
    // InitializeMathSIMDOptimizer();

    // 9. Initialize SaveGame Guard & Atomic Shadow-Writer
    InitializeSaveGameGuard();

    // 10. Hook Main Menu version watermark (displays "G3O v1.0.0" in main menu)
    InitializeVersionHook();

    // 11. Initialize Ground Recovery Fall Guard (Phase 12.2)
    InitializeFallGuard();

    OutputDebugStringA("[Gothic3Optima] All core engine subsystems initialized successfully!\n");
    return 0;
}

static void InitializeOptima() {
    // 1. Initialize MinHook framework
    MH_STATUS mhStatus = MH_Initialize();
    if (mhStatus != MH_OK && mhStatus != MH_ERROR_ALREADY_INITIALIZED) {
        OutputDebugStringA("[Gothic3Optima] Failed to initialize MinHook!\n");
        return;
    }
    OutputDebugStringA("[Gothic3Optima] MinHook initialized successfully.\n");

    // 2. Preload system dinput8 library
    Proxy::LoadSystemDirectInput();

    // 3. Launch background initialization thread to hook engine components as they load
    HANDLE hThread = CreateThread(nullptr, 0, OptimaInitThread, nullptr, 0, nullptr);
    if (hThread) {
        CloseHandle(hThread);
    }
}

static void ShutdownOptima() {
    OutputDebugStringA("[Gothic3Optima] Shutting down...\n");
    ShutdownFallGuard();
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
