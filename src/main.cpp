#include <windows.h>
#include <minhook.h>
#include "dinput8_proxy.h"
#include "version_watermark.h"

static DWORD WINAPI OptimaInitThread(LPVOID) {
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        OutputDebugStringA("[Gothic3Optima] MinHook initialization failed.\n");
        return 1;
    }

    Proxy::LoadSystemDirectInput();
    // Only the UI label needs engine exports; leave PUP's subsystems untouched.
    int maxWaitMs = 5000;
    while ((!GetModuleHandleA("SharedBase.dll") || !GetModuleHandleA("GFC.dll")) && maxWaitMs > 0) {
        Sleep(2);
        maxWaitMs -= 2;
    }
    InitializeVersionHook();

    // Optional legacy G3SE plugin loader (not part of gothic3sdk).
    HMODULE hG3SE = LoadLibraryA("G3SDK.dll");
    if (!hG3SE) hG3SE = LoadLibraryA("G3SE.dll");
    if (hG3SE) {
        typedef void (*LoadPlugins_t)(const char*);
        LoadPlugins_t loadPlugins = (LoadPlugins_t)GetProcAddress(hG3SE, "?LoadPlugins@G3SDK@@YAXPBD@Z");
        if (loadPlugins) {
            loadPlugins("Data\\G3SE\\Plugins");
            OutputDebugStringA("[Gothic3Optima] G3SE plugins loaded.\n");
        }
    }

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
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    Proxy::UnloadSystemDirectInput();
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD reason, LPVOID reserved) {
    switch (reason) {
    case DLL_PROCESS_ATTACH: {
        DisableThreadLibraryCalls(hinstDLL);
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
