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
#include "entity_search_optimizer.h"
#include "frustum_database_optimizer.h"
#include "scene_node_optimizer.h"
#include "shadow_caster_optimizer.h"
#include "session_start_optimizer.h"
#include "savegame_profiler.h"
#include "autotest_runner.h"
#include "selective_culling_optimizer.h"
#include "fog_optimizer.h"
#include "crash_guards.h"
#include "memory_allocator.h"
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

    if (cfg.autoTestEnabled) {
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
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
    // Optional ReShade integration: load ReShade32.dll early if present
    if (cfg.loadReShade && GetFileAttributesA("ReShade32.dll") != INVALID_FILE_ATTRIBUTES) {
        HMODULE hReShade = LoadLibraryA("ReShade32.dll");
        if (hReShade) {
            G3O::Log("G3O", "Optional ReShade runtime loaded successfully from ReShade32.dll.");
        } else {
            G3O::Log("G3O", "Failed to load ReShade32.dll (error %lu).", GetLastError());
        }
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

    // 1. Wait for SharedBase.dll (core foundation of Gothic 3 engine: MemHeap, DiagOutput, error handlers)
    int maxWaitSharedBaseMs = 2000;
    while (!GetModuleHandleA("SharedBase.dll") && maxWaitSharedBaseMs > 0) {
        Sleep(1);
        maxWaitSharedBaseMs -= 1;
    }

    if (cfg.enableCrashInvestigator || cfg.interceptOutputDebugString) {
        G3O::HookEngineDiagOutput();
    }

    // Hook engine fatal handlers after SharedBase.dll is available
    if (cfg.enableCrashInvestigator) {
        G3O::HookEngineFatalErrors();
    }

    // Initialize Memory Allocator immediately when SharedBase.dll is loaded,
    // BEFORE game subsystems (FileSystem, Engine, Game) start mounting pak files and allocating buffers.
    if (cfg.allocatorMode > 0) {
        G3O::InitializeMemoryAllocator(cfg.allocatorMode);
    }

    if (cfg.enableCrashGuards) {
        G3O::InitializeCrashGuards();
    }

    // 2. Wait for remaining engine libraries (GFC.dll, FileSystem.dll, Game.dll, Engine.dll)
    int maxWaitEngineMs = 5000;
    while ((!GetModuleHandleA("GFC.dll") || !GetModuleHandleA("FileSystem.dll") || !GetModuleHandleA("Game.dll") || !GetModuleHandleA("Engine.dll")) && maxWaitEngineMs > 0) {
        Sleep(2);
        maxWaitEngineMs -= 2;
    }

    // Fast Pak Decompressor: SIMD libdeflate zlib decompression for .pak archives & savegames
    if (cfg.enableFastPakDecompressor) {
        G3O::InitializeFastPakDecompressor();
    }

    if (cfg.enableEntitySearchOptimizer) {
        G3O::InitializeEntitySearchOptimizer();
    }

    // Frustum Database Optimizer: Deferred spatial tree updates during entity streaming (Phase 3)
    if (cfg.enableFrustumDatabaseOptimizer) {
        G3O::InitializeFrustumDatabaseOptimizer();
    }

    // Scene Node Optimizer: Preallocate m_ppChildren in eCNode::AttachChild for massive sectors (SysDyn)
    if (cfg.enableSceneNodeOptimizer) {
        G3O::InitializeSceneNodeOptimizer();
    }

    // Shadow Caster Optimizer: O(1) Swap-and-Pop + Robin Hood Hash Deduplication for directional shadows
    if (cfg.enableShadowOptimizer) {
        G3O::InitializeShadowCasterOptimizer();
    }


    // Session Start Optimizer: Eliminates 12x Sleep(100) idle warm-up in gCSession::Start
    if (cfg.enableSessionStartOptimizer) {
        G3O::InitializeSessionStartOptimizer();
    }

    // Savegame Profiler: High-resolution telemetry for savegame loading phases
    G3O::InitializeSavegameProfiler();

    // Optional AutoTest harness (active only when G3O_AUTOTEST=1 or [AutoTest] Enabled=true)
    if (cfg.autoTestEnabled) {
        G3O::InitializeAutoTestRunner();
    }

    if (cfg.enableSharedBaseOptimizations) {
        G3O::InitializeSharedBaseOptimizations();
    }

    // Fast Texture Loader, texture diagnostics, and profiler device attachment
    if (cfg.enableFastTextureLoader || cfg.enableProfiler || cfg.enableCrashInvestigator) {
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
    G3O::ShutdownFrustumDatabaseOptimizer();
    G3O::ShutdownSceneNodeOptimizer();
    G3O::ShutdownShadowCasterOptimizer();
    G3O::ShutdownSessionStartOptimizer();
    G3O::ShutdownSelectiveCullingOptimizer();
    G3O::ShutdownFogOptimizer();

    G3O::ShutdownAutoTestRunner();
    G3O::ShutdownSavegameProfiler();
    G3O::ShutdownD3D9Optimizer();
    G3O::ShutdownProfiler();
    G3O::ShutdownMemoryAllocator();
    G3O::ShutdownCrashGuards();
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
