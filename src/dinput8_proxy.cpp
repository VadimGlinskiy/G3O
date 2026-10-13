#include "dinput8_proxy.h"

typedef HRESULT(WINAPI* PFN_DirectInput8Create)(
    HINSTANCE hinst,
    DWORD dwVersion,
    REFIID riidltf,
    LPVOID* ppvOut,
    LPUNKNOWN punkOuter
);

static HMODULE g_hRealDInput8 = nullptr;
static PFN_DirectInput8Create g_pfnDirectInput8Create = nullptr;

namespace Proxy {
    bool LoadSystemDirectInput() {
        if (g_hRealDInput8) return true;

        char systemDir[MAX_PATH];
        if (GetSystemDirectoryA(systemDir, MAX_PATH) == 0) {
            return false;
        }

        char dllPath[MAX_PATH];
        wsprintfA(dllPath, "%s\\dinput8.dll", systemDir);

        g_hRealDInput8 = LoadLibraryA(dllPath);
        if (!g_hRealDInput8) return false;

        g_pfnDirectInput8Create = reinterpret_cast<PFN_DirectInput8Create>(
            GetProcAddress(g_hRealDInput8, "DirectInput8Create"));

        return (g_pfnDirectInput8Create != nullptr);
    }

    void UnloadSystemDirectInput() {
        if (g_hRealDInput8) {
            FreeLibrary(g_hRealDInput8);
            g_hRealDInput8 = nullptr;
            g_pfnDirectInput8Create = nullptr;
        }
    }
}

// Forwarding export for DirectInput8Create
extern "C" __declspec(dllexport) HRESULT WINAPI DirectInput8Create(
    HINSTANCE hinst,
    DWORD dwVersion,
    REFIID riidltf,
    LPVOID* ppvOut,
    LPUNKNOWN punkOuter
) {
    if (!g_pfnDirectInput8Create) {
        if (!Proxy::LoadSystemDirectInput()) {
            return E_FAIL;
        }
    }
    return g_pfnDirectInput8Create(hinst, dwVersion, riidltf, ppvOut, punkOuter);
}
