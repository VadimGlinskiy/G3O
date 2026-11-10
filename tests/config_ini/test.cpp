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
    ok &= Expect(defaults.enablePropertyCache &&
                 defaults.enableVFSCache && defaults.pakCacheBudgetMB == 768 &&
                 defaults.enableDiagnosticLog && defaults.interceptOutputDebugString &&
                 defaults.enableProfiler && defaults.enableSavegameAudit && defaults.enableLayerAudit,
                 "Config defaults should match the active release profile");
    ok &= Expect(G3O::ParseKeyCode("F2junk", VK_F10) == VK_F10,
                 "partial function-key tokens should use the configured default");
    ok &= Expect(G3O::ParseKeyCode("0x79junk", VK_F10) == VK_F10,
                 "partial hexadecimal key tokens should use the configured default");
    ok &= Expect(G3O::ParseKeyCode("0x79", VK_F10) == 0x79,
                 "valid hexadecimal key codes should be accepted");

    DeleteFileA(path.c_str());
    {
        std::ofstream ini(path);
        ini << "[General]\nEnableG3O=false\n"
            << "[Camera]\nDefaultFOV=nan\nFOVStep=0\n"
            << "[Performance]\nEnableSharedBaseOptimizations=true\nEnableSIMDFrustumCulling=false\nTreeShadowDistance=-1\n"
            << "VegetationGridRangeClamp=1234.0junk\n"
            << "CPUCores=999\nCPUThreads=-1\n"
            << "CPUAffinityMask=0x100000000\n"
            << "PakCacheBudgetMB=-1\nPakPrefetchWorkers=999\n";
    }
    G3O::LoadConfig();
    const G3O::Config invalid = G3O::GetConfig();
    ok &= Expect(!invalid.enableG3O, "master switch should load from INI");
    ok &= Expect(G3O::g_logCallCount == 0, "loading configuration must not log before the master switch is checked");
    ok &= Expect(invalid.enableSharedBaseOptimizations,
                 "new SharedBase option should take precedence over the legacy key");
    ok &= Expect(invalid.defaultFOV == 85.0f, "NaN FOV should use default");
    ok &= Expect(invalid.fovStep == 5.0f, "zero FOV step should use default");
    ok &= Expect(invalid.treeShadowDistance == 6500.0f, "negative shadow distance should use default");
    ok &= Expect(invalid.vegetationGridRangeClamp == 7500.0f, "partial float should use default");
    ok &= Expect(invalid.cpuCores == 0 && invalid.cpuThreads == 0, "invalid CPU counts should use defaults");
    ok &= Expect(invalid.cpuAffinityMask == 0, "mask wider than the x86 pointer should use default");
    ok &= Expect(invalid.pakCacheBudgetMB == 768, "negative cache budget should use default");
    ok &= Expect(invalid.pakPrefetchWorkers == 0, "worker count above maximum should use default");
    ok &= Expect(invalid.enablePropertyCache && invalid.enableVFSCache,
                 "missing performance keys should use the active release profile");
    ok &= Expect(invalid.enableDiagnosticLog && invalid.interceptOutputDebugString && invalid.enableProfiler &&
                 invalid.enableSavegameAudit && invalid.enableLayerAudit,
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
    std::ifstream generated(path);
    const std::string text((std::istreambuf_iterator<char>(generated)), {});
    const char* required[] = {
        "EnableSharedBaseOptimizations=",
        "VegetationGridRangeClamp=", "EnableVFSCache=", "CPUCores=", "CPUThreads=",
        "CPUAffinityMask=", "PakCacheBudgetMB=768", "PakPrefetchWorkers=",
        "EnablePropertyCache=true", "EnableVFSCache=true",
        "EnableD3D9StateFilter=true", "EnableDiagnosticLog=true", "InterceptOutputDebugString=true",
        "EnableProfiler=true", "EnableSavegameAudit=true", "EnableLayerAudit=true"
    };
    for (const char* item : required) {
        ok &= Expect(text.find(item) != std::string::npos, item);
    }
    ok &= Expect(text.find("статистики кучи") == std::string::npos,
                 "generated INI should not claim heap-statistics bypasses");
    ok &= Expect(text.find("EnableInfoManagerBinaryCache=") == std::string::npos,
                 "generated INI should not expose the removed compiled-info cache");
    ok &= Expect(text.find("EnableGuidMapOptimizer=") == std::string::npos,
                 "generated INI should not expose the removed GUID map optimizer");
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
    DeleteFileA(path.c_str());

    if (!ok) return 1;
    std::puts("PASS: invalid INI values fall back safely and generated INI exposes supported settings");
    return 0;
}
