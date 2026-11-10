#pragma once

#include <windows.h>
#include <string>

namespace G3O {

struct Config {
    // [General]
    bool enableG3O = true;
    bool showWatermark = true;
    bool loadG3SEPlugins = true;
    bool fixVulkanLayerConflicts = true;

    // [Display]
    bool enableBorderless = true;
    UINT toggleBorderlessKey = VK_F10; // Default: F10
    bool autoBorderless = true;
    bool preventMinimizeOnFocusLoss = true;

    // [Camera]
    bool enableDynamicFOV = true;
    float defaultFOV = 85.0f;
    bool enableFOVHotkeys = true;
    UINT decreaseFOVKey = VK_OEM_4; // '['
    UINT increaseFOVKey = VK_OEM_6; // ']'
    float fovStep = 5.0f;

    // [CrashInvestigator]
    bool enableCrashInvestigator = true;
    bool showCrashDialog = true;
    bool writeCrashDump = true;
    bool writeCrashLog = true;

    // [Performance]
    bool enableSharedBaseOptimizations = true;
    bool enableMemoryAllocator = false;
    bool enableEntitySearchOptimizer = true;
    bool enableFastPakDecompressor = true;
    bool enablePropertyCache = true;
    bool enablePakPrefetch = true;
    bool enableFrustumDatabaseOptimizer = true;
    bool enableHashProxyOptimizer = true;
    bool enableNavigationOptimizer = true;
    bool enableSceneNodeOptimizer = true;
    bool enableShadowOptimizer = true;
    bool enableClassFactoryOptimizer = true;
    bool enablePVSPrefetchOptimizer = true;
    float pvsBootRadius = 3800.0f;
    float pvsRedundantDistance = 200.0f;
    uint32_t pvsRampUpCalls = 24;
    bool enableQuestLoadOptimizer = true;
    bool enableInfoManagerOptimizer = true;

    // Fail-open if device threading or state-block hooks cannot be verified.
    bool enableD3D9StateFilter = true;
    bool enableFastTextureLoader = true;
    bool enableVegetationOptimizer = true;
    float treeShadowDistance = 6500.0f;
    float vegetationGridRangeClamp = 7500.0f;
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
    bool enableDiagnosticLog = true;
    bool interceptOutputDebugString = true;
    bool enableProfiler = true;
    bool enableSavegameAudit = true;
    bool enableLayerAudit = true;      // Deep per-entity/property QPC hooks (19 hooks, ~0.2-0.3s overhead)

    std::string iniFilePath;
};

const Config& GetConfig();
void LoadConfig();
void LogLoadedConfig();
UINT ParseKeyCode(const std::string& keyStr, UINT defaultKey = 0);
std::string KeyCodeToString(UINT vkCode);

} // namespace G3O
