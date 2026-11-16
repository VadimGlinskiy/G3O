#pragma once

#include "config_manager.h"
#include "fixed_text_buffer.h"

namespace G3O {

inline void AppendAppliedConfigSnapshot(FixedTextBuffer& output, const Config& config) noexcept {
    output.Reset();
    output.Append("[Фактически загруженная конфигурация]\n");
    output.AppendFormat("INI=%s\n", config.iniFilePath.empty() ? "<не указан>" : config.iniFilePath.c_str());

    output.Append("[General]\n");
    output.AppendFormat("EnableG3O=%s\nLoadG3SEPlugins=%s\nLoadReShade=%s\nFixVulkanLayerConflicts=%s\n",
        config.enableG3O ? "true" : "false",
        config.loadG3SEPlugins ? "true" : "false",
        config.loadReShade ? "true" : "false",
        config.fixVulkanLayerConflicts ? "true" : "false");

    output.Append("[Display]\n");
    output.AppendFormat("EnableBorderless=%s\nAutoBorderless=%s\nPreventMinimizeOnFocusLoss=%s\nWindowCursorLock=%s\nInputSystemKeyPassthrough=%s\n",
        config.enableBorderless ? "true" : "false",
        config.autoBorderless ? "true" : "false", config.preventMinimizeOnFocusLoss ? "true" : "false",
        config.windowCursorLock ? "true" : "false", config.inputSystemKeyPassthrough ? "true" : "false");

    output.Append("[CrashInvestigator]\n");
    output.AppendFormat("EnableCrashInvestigator=%s\nEnableCrashGuards=%s\nShowCrashDialog=%s\nWriteCrashDump=%s\nWriteCrashLog=%s\n",
        config.enableCrashInvestigator ? "true" : "false",
        config.enableCrashGuards ? "true" : "false",
        config.showCrashDialog ? "true" : "false",
        config.writeCrashDump ? "true" : "false", config.writeCrashLog ? "true" : "false");

    output.Append("[Performance]\n");
    output.AppendFormat(
        "EnableSharedBaseOptimizations=%s\nEnableEntitySearchOptimizer=%s\nEnableFastPakDecompressor=%s\nEnableLayerRingArena=%s\nEnablePropertyCache=%s\nEnablePakPrefetch=%s\n"
        "EnableFrustumDatabaseOptimizer=%s\nEnableHashProxyOptimizer=%s\nEnableNavigationOptimizer=%s\n"
        "EnableSceneNodeOptimizer=%s\nEnableShadowOptimizer=%s\nEnableClassFactoryOptimizer=%s\n"
        "EnableParallelDynamicLayers=%s\n"
        "EnableSessionStartOptimizer=%s\nSessionStartSleepMs=%u\n"
        "EnableD3D9StateFilter=%s\nEnableFastTextureLoader=%s\nEnableVegetationOptimizer=%s\nEnableAdaptiveTreeShadows=%s\n"
        "TreeShadowDistance=%.9g\nVegetationGridRangeClamp=%.9g\n"
        "EnableSelectiveCulling=%s\nClutterCullThreshold=%.9g\nImportantCullThreshold=%.9g\n"
        "EnableAtmosphericFogControl=%s\nFogDistanceMultiplier=%.9g\nFogEndMinDistance=%.9g\nPreserveUnderwaterFog=%s\n"
        "EnableHighPrecisionTimer=%s\nEnableHighProcessPriority=%s\n"
        "EnableCPUTopologyAffinity=%s\nEnableVFSCache=%s\nCPUCores=%d\nCPUThreads=%d\nCPUAffinityMask=0x%llX\n"
        "PakCacheBudgetMB=%d\nPakPrefetchWorkers=%d\n",
        config.enableSharedBaseOptimizations ? "true" : "false",
        config.enableEntitySearchOptimizer ? "true" : "false",
        config.enableFastPakDecompressor ? "true" : "false",
        config.enableLayerRingArena ? "true" : "false",
        config.enablePropertyCache ? "true" : "false", config.enablePakPrefetch ? "true" : "false",
        config.enableFrustumDatabaseOptimizer ? "true" : "false",
        config.enableHashProxyOptimizer ? "true" : "false",
        config.enableNavigationOptimizer ? "true" : "false",
        config.enableSceneNodeOptimizer ? "true" : "false",
        config.enableShadowOptimizer ? "true" : "false",
        config.enableClassFactoryOptimizer ? "true" : "false",
        config.enableParallelDynamicLayers ? "true" : "false",
        config.enableSessionStartOptimizer ? "true" : "false",
        config.sessionStartSleepMs,
        config.enableD3D9StateFilter ? "true" : "false",
        config.enableFastTextureLoader ? "true" : "false",
        config.enableVegetationOptimizer ? "true" : "false",
        config.enableAdaptiveTreeShadows ? "true" : "false",
        config.treeShadowDistance, config.vegetationGridRangeClamp,
        config.enableSelectiveCulling ? "true" : "false",
        config.clutterCullThreshold, config.importantCullThreshold,
        config.enableAtmosphericFogControl ? "true" : "false",
        config.fogDistanceMultiplier, config.fogEndMinDistance,
        config.preserveUnderwaterFog ? "true" : "false",
        config.enableHighPrecisionTimer ? "true" : "false",
        config.enableHighProcessPriority ? "true" : "false",
        config.enableCPUTopologyAffinity ? "true" : "false",
        config.enableVFSCache ? "true" : "false", config.cpuCores, config.cpuThreads,
        static_cast<unsigned long long>(config.cpuAffinityMask),
        config.pakCacheBudgetMB, config.pakPrefetchWorkers);

    output.Append("[Memory]\n");
    output.AppendFormat("AllocatorMode=%d\n", config.allocatorMode);

    output.Append("[Logging]\n");
    output.AppendFormat("EnableDiagnosticLog=%s\nInterceptOutputDebugString=%s\nEnableProfiler=%s\nEnableSavegameAudit=%s\n",
        config.enableDiagnosticLog ? "true" : "false",
        config.interceptOutputDebugString ? "true" : "false",
        config.enableProfiler ? "true" : "false", config.enableSavegameAudit ? "true" : "false");
    output.Finish();
}

} // namespace G3O
