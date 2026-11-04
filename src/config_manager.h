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
    bool enableSIMDFrustumCulling = true;
    bool enableHighPrecisionTimer = true;
    bool enableHighProcessPriority = true;
    bool enableCPUTopologyAffinity = true;

    // [Logging]
    bool enableDiagnosticLog = true;
    bool interceptOutputDebugString = true;

    std::string iniFilePath;
};

const Config& GetConfig();
void LoadConfig();
UINT ParseKeyCode(const std::string& keyStr, UINT defaultKey = 0);
std::string KeyCodeToString(UINT vkCode);

} // namespace G3O
