#include "config_manager.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

namespace G3O {
static int g_logCallCount = 0;
void Log(const char*, const char*, ...) { ++g_logCallCount; }
}

static std::string ConfigPathBesideTestExe() {
    char path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    char* slash = std::strrchr(path, '\\');
    if (!slash) return {};
    *(slash + 1) = '\0';
    return std::string(path) + "G3O.ini";
}

static bool Expect(bool condition, const char* message) {
    if (condition) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

int main() {
    const std::string path = ConfigPathBesideTestExe();
    if (path.empty()) return 2;
    bool ok = true;
    const G3O::Config defaults{};
    ok &= Expect(defaults.allocatorMode == 1 &&
                 !defaults.enableDiagnosticLog && !defaults.interceptOutputDebugString &&
                 !defaults.enableProfiler && !defaults.enableSavegameAudit && !defaults.enableLayerAudit,
                 "Config defaults should match the active release profile");
    DeleteFileA(path.c_str());
    {
        std::ofstream ini(path);
        ini << "[General]\nEnableG3O=false\n"
            << "[Performance]\nEnableSharedBaseOptimizations=true\nEnableSIMDFrustumCulling=false\nTreeShadowDistance=-1\n"
            << "VegetationGridRangeClamp=1234.0junk\n"
            << "CPUCores=999\nCPUThreads=-1\n"
            << "CPUAffinityMask=0x100000000\n"
            << "FogDistanceMultiplier=-5.0\nFogEndMinDistance=-100.0\n";
    }
    G3O::LoadConfig();
    const G3O::Config invalid = G3O::GetConfig();
    ok &= Expect(!invalid.enableG3O, "master switch should load from INI");
    ok &= Expect(G3O::g_logCallCount == 0, "loading configuration must not log before the master switch is checked");
    ok &= Expect(invalid.enableSharedBaseOptimizations,
                 "new SharedBase option should take precedence over the legacy key");

    ok &= Expect(invalid.treeShadowDistance == 6500.0f, "negative shadow distance should use default");
    ok &= Expect(invalid.vegetationGridRangeClamp == 7500.0f, "partial float should use default");
    ok &= Expect(invalid.cpuCores == 0 && invalid.cpuThreads == 0, "invalid CPU counts should use defaults");
    ok &= Expect(invalid.cpuAffinityMask == 0, "mask wider than the x86 pointer should use default");

    ok &= Expect(invalid.fogDistanceMultiplier == 2.5f, "negative fog distance multiplier should use default");
    ok &= Expect(invalid.fogEndMinDistance == 250000.0f, "negative fog end min distance should use default");

    ok &= Expect(invalid.allocatorMode == 1,
                 "missing memory keys should use the active release profile");
    ok &= Expect(!invalid.enableDiagnosticLog && !invalid.interceptOutputDebugString && !invalid.enableProfiler &&
                 !invalid.enableSavegameAudit && !invalid.enableLayerAudit,
                 "missing logging keys should use the active release profile");

    {
        std::ofstream ini(path);
        ini << "[General]\nEnableG3O=false\n"
            << "[Performance]\nEnableSIMDFrustumCulling=false\n";
    }
    G3O::LoadConfig();
    const G3O::Config legacy = G3O::GetConfig();
    ok &= Expect(!legacy.enableSharedBaseOptimizations,
                 "legacy bundled option should map to the SharedBase option");

    DeleteFileA(path.c_str());
    G3O::LoadConfig();
    std::string text;
    {
        std::ifstream generated(path);
        text.assign(std::istreambuf_iterator<char>(generated), std::istreambuf_iterator<char>());
    }
    const char* required[] = {
        "EnableSharedBaseOptimizations=",
        "VegetationGridRangeClamp=", "CPUCores=", "CPUThreads=", "CPUAffinityMask=0x",
        "EnableFastPakDecompressor=true",
        "EnableSessionStartOptimizer=true", "SessionStartSleepMs=1",
        "EnableSelectiveCulling=true", "ClutterCullThreshold=0.008", "ImportantCullThreshold=0.002",
        "EnableAtmosphericFogControl=true", "FogDistanceMultiplier=2.5", "FogEndMinDistance=250000", "PreserveUnderwaterFog=true",
        "EnableVegetationOptimizer=true", "EnableAdaptiveTreeShadows=true",
        "WindowCursorLock=true", "InputSystemKeyPassthrough=true", "EnableCrashGuards=true", "LoadReShade=true",
        "AllocatorMode=1",
        "EnableDiagnosticLog=false", "InterceptOutputDebugString=false",
        "EnableProfiler=false", "EnableSavegameAudit=false", "EnableLayerAudit=false"
    };
    for (const char* item : required) {
        ok &= Expect(text.find(item) != std::string::npos, item);
    }
    const char* removedCacheSettings[] = {
        "EnableLayerRingArena=", "EnableParallelDynamicLayers=", "EnablePropertyCache=",
        "EnablePakPrefetch=", "PakCacheBudgetMB=", "PakPrefetchWorkers=",
        "EnableClassFactoryOptimizer=", "EnableVFSCache=", "EnableD3D9StateFilter="
    };
    for (const char* item : removedCacheSettings) {
        ok &= Expect(text.find(item) == std::string::npos,
                     "generated INI should not expose removed cache settings");
    }
    const char* removedHotkeys[] = {
        "ToggleBorderlessKey=", "EnableFOVHotkeys=", "DecreaseFOVKey=", "IncreaseFOVKey=", "FOVStep=",
        "EnableDynamicFOV=", "DefaultFOV=", "[Camera]",
        "EnablePVSPrefetchOptimizer=", "EnableAdaptiveROI=", "ROIBootRadius=", "PVSBootRadius=",
        "PVSRedundantDistance=", "PVSRampUpCalls=", "EnableDynamicROI=", "BaseROI=", "AimingROI=",
        "EnableGlobalVisualLodProbe=", "GlobalVisualLodProbeScale="
    };
    for (const char* item : removedHotkeys) {
        ok &= Expect(text.find(item) == std::string::npos, item);
    }
    ok &= Expect(text.find("статистики кучи") == std::string::npos,
                 "generated INI should not claim heap-statistics bypasses");
    ok &= Expect(text.find("ShowWatermark=") == std::string::npos,
                 "generated INI should not expose the no-op watermark setting");
    ok &= Expect(text.find("EnableQuestLoadOptimizer=") == std::string::npos,
                 "generated INI should not expose quest checks that return false during save loading");
    ok &= Expect(text.find("EnableHashProxyOptimizer=") == std::string::npos,
                 "generated INI should not expose the removed HashProxy optimizer");
    ok &= Expect(text.find("EnableNavigationOptimizer=") == std::string::npos,
                 "generated INI should not expose the removed Navigation optimizer");
    ok &= Expect(text.find("EnableInfoManagerOptimizer=") == std::string::npos,
                 "generated INI should not expose the removed InfoManager optimizer");
    ok &= Expect(text.find("EnableInfoManagerBinaryCache=") == std::string::npos,
                 "generated INI should not expose the removed compiled-info cache");
    ok &= Expect(text.find("без проверки контрольной суммы zlib") != std::string::npos,
                 "generated INI should explain the raw-DEFLATE fallback and its checksum behavior");
    ok &= Expect(text.find("прогревает файловый кэш Windows") == std::string::npos &&
                 text.find("заранее распаковывает отмеченные PAK-блоки") == std::string::npos,
                 "generated INI should not describe removed archive caches");
    ok &= Expect(text.find("наивысшим классом производительности Windows") != std::string::npos &&
                 text.find("по одному логическому процессору с каждого ядра") != std::string::npos,
                 "generated INI should explain hybrid-core selection and CPUCores affinity mapping");
    ok &= Expect(text.find("заполнен и содержит не менее 64 элементов") != std::string::npos &&
                 text.find("до 1024, 8192 или 32768") != std::string::npos,
                 "generated INI should explain when the scene-node array grows and its capacity steps");
    ok &= Expect(text.find("EnableFlatEntityIndex=") == std::string::npos,
                 "generated INI should not expose the removed flat entity index");
    ok &= Expect(text.find("кольцевой буфер предзагрузки слоев") == std::string::npos &&
                 text.find("полном совпадении сжатого блока") == std::string::npos,
                 "generated INI should not describe the removed layer cache");
    ok &= Expect(text.find("EnableGuidMapOptimizer=") == std::string::npos,
                 "generated INI should not expose the removed GUID map optimizer");
    const char* removedNpcOptions[] = {
        "EnableCompanionPinning=", "CompanionPinMaxDistance=", "CombatantPinMaxDistance=",
        "EnableDistanceBoneThrottling=", "BoneThrottlingDistance="
    };
    for (const char* option : removedNpcOptions) {
        ok &= Expect(text.find(option) == std::string::npos,
                     "generated INI should not expose removed NPC pinning or bone throttling options");
    }
    ok &= Expect(text.find("EnableFrustumCulling=") == std::string::npos,
                 "unverified frustum hooks should not be exposed as an option");
    ok &= Expect(text.find("EnableSIMDFrustumCulling=") == std::string::npos,
                 "generated INI should not advertise the obsolete SIMD name");
    ok &= Expect(text.find("ShowCrashDialog=true") != std::string::npos &&
                 text.find("необработанного Win32-исключения") != std::string::npos,
                 "generated INI should explain the crash dialog scope");
    ok &= Expect(text.find("WriteCrashDump=true") != std::string::npos &&
                 text.find("FatalError") != std::string::npos,
                 "generated INI should explain the engine FatalError dump limitation");
    ok &= Expect(text.find("SkipRedundantPreLoadActivate=") == std::string::npos,
                 "removed no-op option should not be advertised in the generated INI");
    ok &= Expect(text.find("[AutoTest]") == std::string::npos,
                 "generated default INI should not expose developer AutoTest section");

    // Verify AutoTest env overrides beat INI and enforce safe runtime overrides
    SetEnvironmentVariableA("G3O_AUTOTEST", "1");
    SetEnvironmentVariableA("G3O_AUTOTEST_SLOT", "QuickSave");
    SetEnvironmentVariableA("G3O_AUTOTEST_LOADS", "3");
    SetEnvironmentVariableA("G3O_AUTOTEST_SETTLE_FRAMES", "45");
    SetEnvironmentVariableA("G3O_AUTOTEST_RUN_ID", "test_run_01");
    G3O::LoadConfig();
    const G3O::Config autoCfg = G3O::GetConfig();
    SetEnvironmentVariableA("G3O_AUTOTEST", nullptr);
    SetEnvironmentVariableA("G3O_AUTOTEST_SLOT", nullptr);
    SetEnvironmentVariableA("G3O_AUTOTEST_LOADS", nullptr);
    SetEnvironmentVariableA("G3O_AUTOTEST_SETTLE_FRAMES", nullptr);
    SetEnvironmentVariableA("G3O_AUTOTEST_RUN_ID", nullptr);
    ok &= Expect(autoCfg.autoTestEnabled && autoCfg.autoTestSlot == "QuickSave" &&
                 autoCfg.autoTestLoads == 3 && autoCfg.autoTestSettleFrames == 45 &&
                 autoCfg.autoTestRunId == "test_run_01",
                 "AutoTest env vars should override defaults and INI");
    ok &= Expect(autoCfg.enableDiagnosticLog && autoCfg.enableSavegameAudit &&
                 autoCfg.enableCrashInvestigator && autoCfg.writeCrashLog &&
                 !autoCfg.showCrashDialog && !autoCfg.windowCursorLock && !autoCfg.loadReShade,
                 "AutoTest should enforce non-blocking runtime overrides");
    DeleteFileA(path.c_str());

    if (!ok) return 1;
    std::puts("PASS: invalid INI values fall back safely and generated INI exposes supported settings");
    return 0;
}
