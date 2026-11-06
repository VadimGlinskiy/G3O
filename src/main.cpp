#include <windows.h>
#include <minhook.h>
#include <timeapi.h>
#include "dinput8_proxy.h"
#include "config_manager.h"
#include "version_watermark.h"
#include "diagnostic_logger.h"
#include "crash_handler.h"
#include "borderless_window.h"
#include "camera_controller.h"
#include "math_simd_optimizer.h"
#include "window_input_policy.h"
#include "d3d9_hook.h"
#include "profiler.h"
#include "cpu_topology.h"
#include "vegetation_optimizer.h"
#include "fast_pak_decompressor.h"
#include "property_cache_optimizer.h"
#include "vfs_cache_optimizer.h"
#include "savegame_profiler.h"
#include "pak_prefetch.h"

#pragma comment(lib, "winmm.lib")

static volatile LONG s_runtimeActive = 0;
static volatile LONG s_timerResolutionActive = 0;

static DWORD WINAPI OptimaInitThread(LPVOID) {
    Proxy::LoadSystemDirectInput();
    G3O::LoadConfig();
    const G3O::Config& cfg = G3O::GetConfig();

    if (!cfg.enableG3O) {
        return 0;
    }

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        G3O::Log("G3O", "MinHook initialization failed (code %d).", (int)status);
        OutputDebugStringA("[Gothic3Optima] MinHook initialization failed.\n");
        return 1;
    }
    InterlockedExchange(&s_runtimeActive, 1);

    if (cfg.enableHighProcessPriority) {
        SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    }
    if (cfg.enableHighPrecisionTimer && timeBeginPeriod(1) == TIMERR_NOERROR) {
        InterlockedExchange(&s_timerResolutionActive, 1);
    }
    if (cfg.enableCPUTopologyAffinity) {
        G3O::InitializeCPUTopology();
    }
    if (cfg.fixVulkanLayerConflicts) {
        SetEnvironmentVariableA("DISABLE_VK_LAYER_bandicam_helper_1", "1");
    }
    if (cfg.enableDiagnosticLog) {
        G3O::InitializeDiagnosticLogger();
    }
    G3O::Log("G3O", "Gothic 3 Optima loaded into process PID %lu.", GetCurrentProcessId());
    G3O::LogLoadedConfig();

    // Initialize crash handler and diagnostic logging hooks
    if (cfg.enableCrashInvestigator) {
        G3O::InitializeCrashHandler();
    }
    if (cfg.interceptOutputDebugString) {
        G3O::HookOutputDebugStringA();
    }

    // Initialize Borderless Fullscreen manager
    if (G3O::ShouldInstallWindowInputHook(cfg)) {
        G3O::InitializeBorderlessWindow();
    }

    // Wait for engine libraries; hook DiagOutput as soon as SharedBase.dll appears.
    int maxWaitMs = 5000;
    while ((!GetModuleHandleA("SharedBase.dll") || !GetModuleHandleA("GFC.dll") || !GetModuleHandleA("FileSystem.dll") || !GetModuleHandleA("Game.dll") || !GetModuleHandleA("Engine.dll")) && maxWaitMs > 0) {
        if (cfg.enableCrashInvestigator || cfg.interceptOutputDebugString) {
            G3O::HookEngineDiagOutput();
        }
        Sleep(2);
        maxWaitMs -= 2;
    }
    if (cfg.enableCrashInvestigator || cfg.interceptOutputDebugString) {
        G3O::HookEngineDiagOutput();
    }

    // Hook engine fatal handlers after SharedBase.dll is available
    if (cfg.enableCrashInvestigator) {
        G3O::HookEngineFatalErrors();
    }

    // Fast Pak Decompressor: SIMD libdeflate zlib decompression for .pak archives & savegames
    if (cfg.enableFastPakDecompressor) {
        G3O::InitializeFastPakDecompressor();
    }

    // Property Cache Optimizer: O(1) hash cache for entity property resolution during save loading
    if (cfg.enablePropertyCache) {
        G3O::InitializePropertyCacheOptimizer();
    }

    // VFS Cache Optimizer: O(1) hash cache for virtual file system directory lookups across 395 mod archives
    if (cfg.enableVFSCache) {
        G3O::InitializeVFSCacheOptimizer();
    }

    // Savegame Profiler: High-resolution telemetry for savegame loading phases
    G3O::InitializeSavegameProfiler();
    G3O::InitializePakPrefetch();

    if (cfg.enableSharedBaseOptimizations) {
        G3O::InitializeSharedBaseOptimizations();
    }

    // Direct3D 9 State Redundancy Filter & Fast Texture Loader
    if (cfg.enableD3D9StateFilter || cfg.enableFastTextureLoader || cfg.enableProfiler) {
        G3O::InitializeD3D9Optimizer();
    }

    // Vegetation & SpeedTree Optimizer (clamps invisible distant tree shadows on terrain)
    if (cfg.enableVegetationOptimizer) {
        G3O::InitializeVegetationOptimizer();
    }

    if (cfg.enableProfiler) {
        G3O::InitializeProfiler();
    }

    // Hook camera controller for dynamic widescreen FOV
    if (cfg.enableDynamicFOV) {
        G3O::InitializeCameraController();
    }

    // Main menu version watermark
    if (cfg.showWatermark) {
        InitializeVersionHook();
    }

    // Optional legacy G3SE plugin loader (not part of gothic3sdk).
    if (cfg.loadG3SEPlugins) {
        HMODULE hG3SE = LoadLibraryA("G3SDK.dll");
        if (!hG3SE) hG3SE = LoadLibraryA("G3SE.dll");
        if (hG3SE) {
            typedef void (*LoadPlugins_t)(const char*);
            LoadPlugins_t loadPlugins = (LoadPlugins_t)GetProcAddress(hG3SE, "?LoadPlugins@G3SDK@@YAXPBD@Z");
            if (loadPlugins) {
                loadPlugins("Data\\G3SE\\Plugins");
                G3O::Log("G3O", "Legacy G3SE plugins loaded from Data\\G3SE\\Plugins.");
                OutputDebugStringA("[Gothic3Optima] G3SE plugins loaded.\n");
            }
        }
    }

    G3O::Log("G3O", "G3O core initialized.");
    OutputDebugStringA("[Gothic3Optima] G3O core initialized.\n");
    return 0;
}

static void ShutdownOptima() {
    if (InterlockedExchange(&s_runtimeActive, 0) == 0) {
        Proxy::UnloadSystemDirectInput();
        return;
    }

    HMODULE hG3SE = GetModuleHandleA("G3SDK.dll");
    if (!hG3SE) hG3SE = GetModuleHandleA("G3SE.dll");
    if (hG3SE) {
        typedef void (*UnloadAll_t)();
        UnloadAll_t unload = (UnloadAll_t)GetProcAddress(hG3SE, "?UnloadAllPlugins@G3SDK@@YAXXZ");
        if (unload) unload();
    }
    G3O::ShutdownSharedBaseOptimizations();
    G3O::ShutdownVegetationOptimizer();
    G3O::ShutdownFastPakDecompressor();
    G3O::ShutdownPropertyCacheOptimizer();
    G3O::ShutdownVFSCacheOptimizer();
    G3O::ShutdownSavegameProfiler();
    G3O::ShutdownD3D9Optimizer();
    G3O::ShutdownProfiler();
    G3O::ShutdownCameraController();
    G3O::ShutdownBorderlessWindow();
    G3O::ShutdownCrashHandler();
    G3O::ShutdownDiagnosticLogger();
    if (InterlockedExchange(&s_timerResolutionActive, 0) != 0) {
        timeEndPeriod(1);
    }
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    Proxy::UnloadSystemDirectInput();
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD reason, LPVOID reserved) {
    switch (reason) {
    case DLL_PROCESS_ATTACH: {
        DisableThreadLibraryCalls(hinstDLL);
        HANDLE thread = CreateThread(nullptr, 0, OptimaInitThread, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
        break;
    }
    case DLL_PROCESS_DETACH:
        ShutdownOptima();
        break;
    }
    return TRUE;
}
