#pragma once

#include "config_manager.h"
#include "fixed_text_buffer.h"

namespace G3O {

inline void AppendAppliedConfigSnapshot(FixedTextBuffer& output, const Config& config) noexcept {
    output.Reset();
    output.Append("[Фактически загруженная конфигурация]\n");
    output.AppendFormat("INI=%s\n", config.iniFilePath.empty() ? "<не указан>" : config.iniFilePath.c_str());

    output.Append("[General]\n");
    output.AppendFormat("EnableG3O=%s\nShowWatermark=%s\nLoadG3SEPlugins=%s\nFixVulkanLayerConflicts=%s\n",
        config.enableG3O ? "true" : "false", config.showWatermark ? "true" : "false",
        config.loadG3SEPlugins ? "true" : "false", config.fixVulkanLayerConflicts ? "true" : "false");

    output.Append("[Display]\n");
    output.AppendFormat("EnableBorderless=%s\nToggleBorderlessKey=0x%X\nAutoBorderless=%s\nPreventMinimizeOnFocusLoss=%s\n",
        config.enableBorderless ? "true" : "false", config.toggleBorderlessKey,
        config.autoBorderless ? "true" : "false", config.preventMinimizeOnFocusLoss ? "true" : "false");

    output.Append("[Camera]\n");
    output.AppendFormat("EnableDynamicFOV=%s\nDefaultFOV=%.9g\nEnableFOVHotkeys=%s\nDecreaseFOVKey=0x%X\nIncreaseFOVKey=0x%X\nFOVStep=%.9g\n",
        config.enableDynamicFOV ? "true" : "false", config.defaultFOV,
        config.enableFOVHotkeys ? "true" : "false", config.decreaseFOVKey,
        config.increaseFOVKey, config.fovStep);

    output.Append("[CrashInvestigator]\n");
    output.AppendFormat("EnableCrashInvestigator=%s\nShowCrashDialog=%s\nWriteCrashDump=%s\nWriteCrashLog=%s\n",
        config.enableCrashInvestigator ? "true" : "false", config.showCrashDialog ? "true" : "false",
        config.writeCrashDump ? "true" : "false", config.writeCrashLog ? "true" : "false");

    output.Append("[Performance]\n");
    output.AppendFormat(
        "EnableSharedBaseOptimizations=%s\nEnableMemoryAllocator=%s\nEnableEntitySearchOptimizer=%s\nEnableFastPakDecompressor=%s\nEnablePropertyCache=%s\nEnablePakPrefetch=%s\n"
        "EnableFrustumDatabaseOptimizer=%s\nEnableHashProxyOptimizer=%s\nEnableNavigationOptimizer=%s\n"
        "EnableSceneNodeOptimizer=%s\nEnableShadowOptimizer=%s\nEnableClassFactoryOptimizer=%s\nEnablePVSPrefetchOptimizer=%s\nPVSBootRadius=%.9g\nEnableQuestLoadOptimizer=%s\nEnableInfoManagerOptimizer=%s\n"
        "EnableD3D9StateFilter=%s\nEnableFastTextureLoader=%s\nEnableVegetationOptimizer=%s\n"
        "TreeShadowDistance=%.9g\nVegetationGridRangeClamp=%.9g\nEnableHighPrecisionTimer=%s\nEnableHighProcessPriority=%s\n"
        "EnableCPUTopologyAffinity=%s\nEnableVFSCache=%s\nCPUCores=%d\nCPUThreads=%d\nCPUAffinityMask=0x%llX\n"
        "PakCacheBudgetMB=%d\nPakPrefetchWorkers=%d\n",
        config.enableSharedBaseOptimizations ? "true" : "false",
        config.enableMemoryAllocator ? "true" : "false",
        config.enableEntitySearchOptimizer ? "true" : "false",
        config.enableFastPakDecompressor ? "true" : "false",
        config.enablePropertyCache ? "true" : "false", config.enablePakPrefetch ? "true" : "false",
        config.enableFrustumDatabaseOptimizer ? "true" : "false",
        config.enableHashProxyOptimizer ? "true" : "false",
        config.enableNavigationOptimizer ? "true" : "false",
        config.enableSceneNodeOptimizer ? "true" : "false",
        config.enableShadowOptimizer ? "true" : "false",
        config.enableClassFactoryOptimizer ? "true" : "false",
        config.enablePVSPrefetchOptimizer ? "true" : "false",
        config.pvsBootRadius,
        config.enableQuestLoadOptimizer ? "true" : "false",
        config.enableInfoManagerOptimizer ? "true" : "false",
        config.enableD3D9StateFilter ? "true" : "false",
        config.enableFastTextureLoader ? "true" : "false",
        config.enableVegetationOptimizer ? "true" : "false",
        config.treeShadowDistance, config.vegetationGridRangeClamp,
        config.enableHighPrecisionTimer ? "true" : "false",
        config.enableHighProcessPriority ? "true" : "false",
        config.enableCPUTopologyAffinity ? "true" : "false",
        config.enableVFSCache ? "true" : "false", config.cpuCores, config.cpuThreads,
        static_cast<unsigned long long>(config.cpuAffinityMask),
        config.pakCacheBudgetMB, config.pakPrefetchWorkers);

    output.Append("[Logging]\n");
    output.AppendFormat("EnableDiagnosticLog=%s\nInterceptOutputDebugString=%s\nEnableProfiler=%s\nEnableSavegameAudit=%s\n",
        config.enableDiagnosticLog ? "true" : "false",
        config.interceptOutputDebugString ? "true" : "false",
        config.enableProfiler ? "true" : "false", config.enableSavegameAudit ? "true" : "false");
    output.Finish();
}

} // namespace G3O
