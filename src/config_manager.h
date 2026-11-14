#pragma once

#include <windows.h>
#include <string>

namespace G3O {

struct Config {
    // [General]
    bool enableG3O = true;
    bool loadG3SEPlugins = true;
    bool loadReShade = true;
    bool fixVulkanLayerConflicts = true;

    // [Display]
    bool enableBorderless = true;
    bool autoBorderless = true;
    bool preventMinimizeOnFocusLoss = true;
    bool windowCursorLock = true;
    bool inputSystemKeyPassthrough = true;

    // [CrashInvestigator]
    bool enableCrashInvestigator = true;
    bool enableCrashGuards = true;
    bool showCrashDialog = true;
    bool writeCrashDump = true;
    bool writeCrashLog = true;

    // [Performance]
    bool enableSharedBaseOptimizations = true;
    bool enableEntitySearchOptimizer = true;
    bool enableFastPakDecompressor = true;
    bool enableLayerRingArena = false;
    bool enablePropertyCache = true;
    bool enablePakPrefetch = true;
    bool enableFrustumDatabaseOptimizer = true;
    bool enableHashProxyOptimizer = true;
    bool enableNavigationOptimizer = true;
    bool enableSceneNodeOptimizer = true;
    bool enableShadowOptimizer = true;
    bool enableClassFactoryOptimizer = true;

    bool enableInfoManagerOptimizer = true;
    bool enableSessionStartOptimizer = true;
    uint32_t sessionStartSleepMs = 1;

    // Fail-open if device threading or state-block hooks cannot be verified.
    bool enableD3D9StateFilter = true;
    bool enableFastTextureLoader = true;
    bool enableVegetationOptimizer = true;
    bool enableAdaptiveTreeShadows = true;
    float treeShadowDistance = 6500.0f;
    float vegetationGridRangeClamp = 7500.0f;
    bool enableSelectiveCulling = true;
    float clutterCullThreshold = 0.008f;
    float importantCullThreshold = 0.002f;
    bool enableAtmosphericFogControl = true;
    float fogDistanceMultiplier = 2.5f;
    float fogEndMinDistance = 250000.0f;
    bool preserveUnderwaterFog = true;
    bool enableHighPrecisionTimer = true;
    bool enableHighProcessPriority = true;
    bool enableCPUTopologyAffinity = true;
    bool enableVFSCache = true;
    int cpuCores = 0;                  // 0 = Auto-detect
    int cpuThreads = 0;                // 0 = Auto-detect
    DWORD_PTR cpuAffinityMask = 0;     // 0 = Auto-detect
    int pakCacheBudgetMB = 768;        // Rolling pre-decompression memory budget (MB)
    int pakPrefetchWorkers = 0;        // 0 = Auto-detect based on CPU cores (up to 6)

    // [Logging]
    bool enableDiagnosticLog = false;
    bool interceptOutputDebugString = false;
    bool enableProfiler = false;
    bool enableSavegameAudit = false;
    bool enableLayerAudit = false;     // Deep per-entity/property QPC hooks (19 hooks, ~0.2-0.3s overhead)

    std::string iniFilePath;
};

const Config& GetConfig();
void LoadConfig();
void LogLoadedConfig();

} // namespace G3O
