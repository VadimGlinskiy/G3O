#include <windows.h>
#include <minhook.h>
#include <timeapi.h>
#include "dinput8_proxy.h"
#include "config_manager.h"
#include "diagnostic_logger.h"
#include "crash_handler.h"
#include "borderless_window.h"
#include "math_simd_optimizer.h"
#include "window_input_policy.h"
#include "d3d9_hook.h"
#include "profiler.h"
#include "cpu_topology.h"
#include "vegetation_optimizer.h"
#include "fast_pak_decompressor.h"
#include "property_cache_optimizer.h"
#include "entity_search_optimizer.h"
#include "frustum_database_optimizer.h"
#include "hash_proxy_optimizer.h"
#include "navigation_optimizer.h"
#include "scene_node_optimizer.h"
#include "shadow_caster_optimizer.h"
#include "class_factory_optimizer.h"
#include "pvs_prefetch_optimizer.h"
#include "info_manager_optimizer.h"
#include "session_start_optimizer.h"
#include "dynamic_roi_optimizer.h"
#include "vfs_cache_optimizer.h"
#include "savegame_profiler.h"
#include "pak_prefetch.h"
#include "selective_culling_optimizer.h"
#include "fog_optimizer.h"
#include "global_visual_lod_probe.h"
#include "focus_name_guard.h"
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
    if (cfg.enableBorderless) {
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

    G3O::InitializeFocusNameGuard();

    // Fast Pak Decompressor: SIMD libdeflate zlib decompression for .pak archives & savegames
    if (cfg.enableFastPakDecompressor) {
        G3O::InitializeFastPakDecompressor();
    }

    // Property Cache Optimizer: O(1) hash cache for entity property resolution during save loading
    if (cfg.enablePropertyCache) {
        G3O::InitializePropertyCacheOptimizer();
    }

    if (cfg.enableEntitySearchOptimizer) {
        G3O::InitializeEntitySearchOptimizer();
    }

    // VFS Cache Optimizer: O(1) hash cache for virtual file system directory lookups across 395 mod archives
    if (cfg.enableVFSCache) {
        G3O::InitializeVFSCacheOptimizer();
    }

    // Hash & Proxy Optimizer: Fast djb2 string hashing & template entity proxy resolution (Phase 1)
    if (cfg.enableHashProxyOptimizer) {
        G3O::InitializeHashProxyOptimizer();
    }

    // Frustum Database Optimizer: Deferred spatial tree updates during entity streaming (Phase 3)
    if (cfg.enableFrustumDatabaseOptimizer) {
        G3O::InitializeFrustumDatabaseOptimizer();
    }

    // Navigation Optimizer: Suppress redundant nav-mesh iterations during save loading
    if (cfg.enableNavigationOptimizer) {
        G3O::InitializeNavigationOptimizer();
    }

    // Scene Node Optimizer: Preallocate m_ppChildren in eCNode::AttachChild for massive sectors (SysDyn)
    if (cfg.enableSceneNodeOptimizer) {
        G3O::InitializeSceneNodeOptimizer();
    }

    // Shadow Caster Optimizer: O(1) Swap-and-Pop + Robin Hood Hash Deduplication for directional shadows
    if (cfg.enableShadowOptimizer) {
        G3O::InitializeShadowCasterOptimizer();
    }

    // Class Factory Optimizer: L1-cache and Zero-Alloc string reuse for bCPropertyObjectSingleton::ReadObject
    if (cfg.enableClassFactoryOptimizer) {
        G3O::InitializeClassFactoryOptimizer();
    }

    // Initialize its shared state even when the PVS hook is off; ROI and savegame paths still use it.
    G3O::InitializePVSPrefetchOptimizer();


    // Info Manager Optimizer: O(1) Shadow Table for gCInfoManager_PS::GetInfo
    if (cfg.enableInfoManagerOptimizer) {
        G3O::InitializeInfoManagerOptimizer();
    }

    // Session Start Optimizer: Eliminates 12x Sleep(100) idle warm-up in gCSession::Start
    if (cfg.enableSessionStartOptimizer) {
        G3O::InitializeSessionStartOptimizer();
    }

    // Dynamic ROI: changes Entity.ROI while the crosshair is visible.
    if (cfg.enableDynamicROI) {
        G3O::InitializeDynamicROIOptimizer();
    }

    // Savegame Profiler: High-resolution telemetry for savegame loading phases
    G3O::InitializeSavegameProfiler();
    G3O::InitializePakPrefetch();

    if (cfg.enableSharedBaseOptimizations) {
        G3O::InitializeSharedBaseOptimizations();
    }

    // Direct3D 9 State Redundancy Filter, Fast Texture Loader & Texture Crash Investigator
    if (cfg.enableD3D9StateFilter || cfg.enableFastTextureLoader || cfg.enableProfiler || cfg.enableCrashInvestigator) {
        G3O::InitializeD3D9Optimizer();
    }

    // Vegetation & SpeedTree Optimizer (clamps invisible distant tree shadows on terrain)
    if (cfg.enableVegetationOptimizer) {
        G3O::InitializeVegetationOptimizer();
    }

    // Selective Frustum Screen-Size Culling (G3O-TASK-06: differentiates interactive items from clutter)
    if (cfg.enableSelectiveCulling) {
        G3O::InitializeSelectiveCullingOptimizer();
    }

    // Atmospheric Fog Expander (G3O-TASK-08: expands D3D9 fog distance for scenic vistas)
    if (cfg.enableAtmosphericFogControl) {
        G3O::InitializeFogOptimizer();
    }

    if (cfg.enableGlobalVisualLodProbe) {
        G3O::InitializeGlobalVisualLodProbe();
    }

    if (cfg.enableProfiler) {
        G3O::InitializeProfiler();
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
    G3O::ShutdownEntitySearchOptimizer();
    G3O::ShutdownPropertyCacheOptimizer();
    G3O::ShutdownFrustumDatabaseOptimizer();
    G3O::ShutdownHashProxyOptimizer();
    G3O::ShutdownSceneNodeOptimizer();
    G3O::ShutdownShadowCasterOptimizer();
    G3O::ShutdownClassFactoryOptimizer();
    G3O::ShutdownInfoManagerOptimizer();
    G3O::ShutdownSessionStartOptimizer();
    G3O::ShutdownDynamicROIOptimizer();
    G3O::ShutdownPVSPrefetchOptimizer();
    G3O::ShutdownSelectiveCullingOptimizer();
    G3O::ShutdownGlobalVisualLodProbe();
    G3O::ShutdownFogOptimizer();
    G3O::ShutdownNavigationOptimizer();
    G3O::ShutdownVFSCacheOptimizer();
    G3O::ShutdownSavegameProfiler();
    G3O::ShutdownD3D9Optimizer();
    G3O::ShutdownProfiler();
    G3O::ShutdownFocusNameGuard();
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
