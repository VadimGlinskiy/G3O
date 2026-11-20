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
    bool enableFrustumDatabaseOptimizer = true;
    bool enableSceneNodeOptimizer = true;
    bool enableShadowOptimizer = true;

    bool enableSessionStartOptimizer = true;
    uint32_t sessionStartSleepMs = 1;

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
    int cpuCores = 0;                  // 0 = Auto-detect
    int cpuThreads = 0;                // 0 = Auto-detect
    DWORD_PTR cpuAffinityMask = 0;     // 0 = Auto-detect

    // [Memory]
    int allocatorMode = 1;             // 0 = Vanilla, 1 = ShortUsage / ScrapHeap, 2 = Full MemHeap

    // [Logging]
    bool enableDiagnosticLog = false;
    bool interceptOutputDebugString = false;
    bool enableProfiler = false;
    bool enableSavegameAudit = false;
    bool enableLayerAudit = false;     // Deep per-entity/property QPC hooks (19 hooks, ~0.2-0.3s overhead)

    // [AutoTest] (env-first: G3O_AUTOTEST*, optional INI fallback; not emitted to default G3O.ini)
    bool autoTestEnabled = false;
    std::string autoTestSlot = "Autosave_Slot_0001";
    int autoTestLoads = 2;
    int autoTestSettleFrames = 60;
    int autoTestMenuSettleTicks = 30;
    int autoTestTimeoutSec = 180;
    bool autoTestSkipIntro = true;
    bool autoTestDisableReShade = true;
    bool autoTestEnableLayerAudit = false;
    std::string autoTestRunId;

    std::string iniFilePath;
};

const Config& GetConfig();
void LoadConfig();
void LogLoadedConfig();

} // namespace G3O
