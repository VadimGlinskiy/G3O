#include <windows.h>
#include <minhook.h>
#include "dinput8_proxy.h"
#include "version_watermark.h"
#include "diagnostic_logger.h"
#include "crash_handler.h"

static DWORD WINAPI OptimaInitThread(LPVOID) {
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        G3O::Log("G3O", "MinHook initialization failed (code %d).", (int)status);
        OutputDebugStringA("[Gothic3Optima] MinHook initialization failed.\n");
        return 1;
    }

    Proxy::LoadSystemDirectInput();

    // Hook OutputDebugStringA and install crash interception hooks
    G3O::InitializeCrashHandler();
    G3O::HookLoggingFunctions();

    // Only the UI label needs engine exports; leave PUP's subsystems untouched.
    int maxWaitMs = 5000;
    while ((!GetModuleHandleA("SharedBase.dll") || !GetModuleHandleA("GFC.dll")) && maxWaitMs > 0) {
        Sleep(2);
        maxWaitMs -= 2;
    }

    // Hook engine DiagOutput and Fatal Error handlers once SharedBase.dll is available
    G3O::HookEngineFatalErrors();
    G3O::HookLoggingFunctions();

    InitializeVersionHook();

    // Optional legacy G3SE plugin loader (not part of gothic3sdk).
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

    G3O::Log("G3O", "PUP-compatible core with Crash Investigator and Diagnostic Logging initialized.");
    OutputDebugStringA("[Gothic3Optima] PUP-compatible core initialized.\n");
    return 0;
}

static void ShutdownOptima() {
    HMODULE hG3SE = GetModuleHandleA("G3SDK.dll");
    if (!hG3SE) hG3SE = GetModuleHandleA("G3SE.dll");
    if (hG3SE) {
        typedef void (*UnloadAll_t)();
        UnloadAll_t unload = (UnloadAll_t)GetProcAddress(hG3SE, "?UnloadAllPlugins@G3SDK@@YAXXZ");
        if (unload) unload();
    }
    G3O::ShutdownCrashHandler();
    G3O::ShutdownDiagnosticLogger();
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    Proxy::UnloadSystemDirectInput();
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD reason, LPVOID reserved) {
    switch (reason) {
    case DLL_PROCESS_ATTACH: {
        DisableThreadLibraryCalls(hinstDLL);

        // Prevent known unstable 32-bit Vulkan implicit layers (such as Bandicam's bdcamvk32.dll)
        // from crashing DXVK during swapchain creation, while keeping screen capture operational.
        SetEnvironmentVariableA("DISABLE_VK_LAYER_bandicam_helper_1", "1");

        G3O::InitializeDiagnosticLogger();
        G3O::Log("G3O", "Gothic 3 Optima loaded into process PID %lu.", GetCurrentProcessId());
        HANDLE thread = CreateThread(nullptr, 0, OptimaInitThread, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
        break;
    }
    case DLL_PROCESS_DETACH:
        if (!reserved) ShutdownOptima();
        break;
    }
    return TRUE;
}
