#include "../../src/diagnostic_logger.cpp"
#include "applied_config_snapshot.h"
#include "timestamped_path.h"
#include "g3o_output_path.h"
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cstdlib>

static void requireContains(const char* text, const char* expected) {
    if (!text || std::strstr(text, expected) == nullptr) {
        std::fprintf(stderr, "Missing expected text: %s\n", expected);
        std::exit(1);
    }
}

static void requireAbsent(const char* text, const char* unexpected) {
    if (text && std::strstr(text, unexpected) != nullptr) {
        std::fprintf(stderr, "Unexpected text: %s\n", unexpected);
        std::exit(1);
    }
}

static bool s_forwardingSeenByOriginal = false;
static bool s_originalCalledBeforeLocalLog = false;
static int s_diagOutputCalls = 0;
static int s_outputDebugCalls = 0;
static char s_forwardedText[256] = {};

static void FakeDiagOutput(const char* format, ...) {
    s_forwardingSeenByOriginal = true;
    ++s_diagOutputCalls;
    char recentEvents[2048] = {};
    G3O::CopyRecentLogs(recentEvents, sizeof(recentEvents));
    s_originalCalledBeforeLocalLog =
        std::strstr(recentEvents, "Guru message 100%") == nullptr;
    va_list args;
    va_start(args, format);
    const char* message = va_arg(args, const char*);
    std::snprintf(s_forwardedText, sizeof(s_forwardedText), format, message);
    va_end(args);
}

extern "C" MH_STATUS WINAPI MH_CreateHook(LPVOID, LPVOID, LPVOID*) { return MH_OK; }
extern "C" MH_STATUS WINAPI MH_EnableHook(LPVOID) { return MH_OK; }
extern "C" MH_STATUS WINAPI MH_DisableHook(LPVOID) { return MH_OK; }
extern "C" MH_STATUS WINAPI MH_RemoveHook(LPVOID) { return MH_OK; }

static void FakeOutputDebugString(const char* message) {
    ++s_outputDebugCalls;
    std::snprintf(s_forwardedText, sizeof(s_forwardedText), "%s", message ? message : "");
}

int main() {
    G3O::Config config;
    config.iniFilePath = "E:\\Gothic3\\G3O.ini";
    char configStorage[8192] = {};
    G3O::FixedTextBuffer configText(configStorage, sizeof(configStorage));
    G3O::AppendAppliedConfigSnapshot(configText, config);

    const char* const expectedKeys[] = {
        "EnableG3O=", "LoadG3SEPlugins=", "FixVulkanLayerConflicts=",
        "EnableBorderless=", "AutoBorderless=", "PreventMinimizeOnFocusLoss=",
        "EnableCrashInvestigator=", "ShowCrashDialog=", "WriteCrashDump=", "WriteCrashLog=",
        "EnableSharedBaseOptimizations=", "EnableFastPakDecompressor=", "EnablePropertyCache=", "EnablePakPrefetch=",
        "EnableD3D9StateFilter=", "EnableFastTextureLoader=", "EnableVegetationOptimizer=",
        "TreeShadowDistance=", "VegetationGridRangeClamp=", "EnableHighPrecisionTimer=", "EnableHighProcessPriority=",
        "EnableCPUTopologyAffinity=", "EnableVFSCache=", "CPUCores=", "CPUThreads=", "CPUAffinityMask=",
        "PakCacheBudgetMB=", "PakPrefetchWorkers=", "EnableDiagnosticLog=", "InterceptOutputDebugString=",
        "EnableProfiler=", "EnableSavegameAudit="
    };
    for (const char* key : expectedKeys) requireContains(configText.CStr(), key);
    requireAbsent(configText.CStr(), "ShowWatermark=");
    requireAbsent(configText.CStr(), "ToggleBorderlessKey=");
    requireAbsent(configText.CStr(), "[Camera]");
    requireAbsent(configText.CStr(), "EnableDynamicFOV=");
    requireAbsent(configText.CStr(), "DefaultFOV=");
    requireAbsent(configText.CStr(), "EnableFOVHotkeys=");
    requireAbsent(configText.CStr(), "DecreaseFOVKey=");
    requireAbsent(configText.CStr(), "IncreaseFOVKey=");
    requireAbsent(configText.CStr(), "FOVStep=");
    if (configText.IsTruncated()) {
        std::fprintf(stderr, "Applied configuration snapshot was truncated\n");
        return 1;
    }
    requireContains(configText.CStr(), "E:\\Gothic3\\G3O.ini");

    char outputDirectory[MAX_PATH] = {};
    if (!G3O::BuildG3OOutputDirectoryPath(outputDirectory, sizeof(outputDirectory),
                                          "E:\\Gothic3\\Gothic3.exe") ||
        std::strcmp(outputDirectory, "E:\\Gothic3\\G3O") != 0) {
        std::fprintf(stderr, "G3O output directory is not beside the game executable\n");
        return 1;
    }
    char outputFile[MAX_PATH] = {};
    if (!G3O::BuildG3OOutputFilePath(outputFile, sizeof(outputFile), outputDirectory,
                                     "G3O_profile_20261106.log") ||
        std::strcmp(outputFile, "E:\\Gothic3\\G3O\\G3O_profile_20261106.log") != 0 ||
        G3O::BuildG3OOutputFilePath(outputFile, sizeof(outputFile), outputDirectory,
                                    "nested\\G3O_profile.log")) {
        std::fprintf(stderr, "G3O output path is not flat\n");
        return 1;
    }

    SYSTEMTIME time = {};
    time.wYear = 2026; time.wMonth = 11; time.wDay = 6;
    time.wHour = 13; time.wMinute = 14; time.wSecond = 15; time.wMilliseconds = 123;
    char pathA[MAX_PATH] = {};
    char pathB[MAX_PATH] = {};
    if (!G3O::BuildTimestampedFilePath(pathA, sizeof(pathA), outputDirectory, "G3O_CrashReport", "log", time, 17, 23, 4) ||
        !G3O::BuildTimestampedFilePath(pathB, sizeof(pathB), outputDirectory, "G3O_CrashReport", "log", time, 17, 23, 5)) {
        std::fprintf(stderr, "Failed to build timestamped report paths\n");
        return 1;
    }
    requireContains(pathA, "G3O_CrashReport_20261106_131415_123_17_23_4.log");
    requireContains(pathA, "E:\\Gothic3\\G3O\\");
    if (std::strcmp(pathA, pathB) == 0) {
        std::fprintf(stderr, "Report path sequence did not make the filename unique\n");
        return 1;
    }

    G3O::s_origDiagOutput = &FakeDiagOutput;
    SetLastError(ERROR_ACCESS_DENIED);
    G3O::HookedDiagOutput("Guru message %d%%", 100);
    char recentEvents[2048] = {};
    G3O::CopyRecentLogs(recentEvents, sizeof(recentEvents));
    if (!s_forwardingSeenByOriginal || !s_originalCalledBeforeLocalLog || s_diagOutputCalls != 1 ||
        GetLastError() != ERROR_ACCESS_DENIED ||
        G3O::s_forwardingOriginalDiagOutput ||
        std::strcmp(s_forwardedText, "Guru message 100%") != 0 ||
        std::strstr(recentEvents, "[EngineDiag] Guru message 100%") == nullptr) {
        std::fprintf(stderr, "HookedDiagOutput did not preserve the original call and log the Guru message\n");
        return 1;
    }
    const bool consumed = G3O::ForwardNestedDiagOutputDebugString(
        true, &FakeOutputDebugString, "engine diagnostic");
    if (!consumed || s_outputDebugCalls != 1 || std::strcmp(s_forwardedText, "engine diagnostic") != 0) {
        std::fprintf(stderr, "Nested OutputDebugStringA was not forwarded exactly once\n");
        return 1;
    }
    const bool consumedWithoutDebugger = G3O::ForwardNestedDiagOutputDebugString(
        true, &FakeOutputDebugString, "suppressed without debugger", false);
    if (!consumedWithoutDebugger || s_outputDebugCalls != 1) {
        std::fprintf(stderr, "Nested debug output was not suppressed without a debugger\n");
        return 1;
    }

    std::puts("PASS: flat G3O output paths, full applied config, unique timestamped paths, and original DiagOutput forwarding");
    return 0;
}
