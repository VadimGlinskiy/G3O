#include "d3d9_hook.h"
#include "diagnostic_logger.h"
#include "config_manager.h"
#include "profiler.h"
#include "d3d_state_cache.h"
#include <minhook.h>
#include <d3d9.h>
#include <stdio.h>

namespace G3O {

#define MAX_RENDER_STATES 256
#define MAX_STAGES 8
#define MAX_STAGE_STATES 36
#define MAX_SAMPLERS 16
#define MAX_SAMPLER_STATES 16

static DWORD s_renderStateCache[MAX_RENDER_STATES];
static BYTE  s_renderStateValid[MAX_RENDER_STATES] = { 0 };

static DWORD s_stageStateCache[MAX_STAGES][MAX_STAGE_STATES];
static BYTE  s_stageStateValid[MAX_STAGES][MAX_STAGE_STATES] = { { 0 } };

static DWORD s_samplerStateCache[MAX_SAMPLERS][MAX_SAMPLER_STATES];
static BYTE  s_samplerStateValid[MAX_SAMPLERS][MAX_SAMPLER_STATES] = { { 0 } };
static IDirect3DDevice9* s_cachedDevice = nullptr;

static bool s_deviceFilterInstalled = false;
static bool s_presentHooked = false;
static void* s_deviceHookTargets[4] = {};

typedef HRESULT (STDMETHODCALLTYPE* SetRenderState_t)(IDirect3DDevice9* This, D3DRENDERSTATETYPE State, DWORD Value);
static SetRenderState_t Real_SetRenderState = NULL;

typedef HRESULT (STDMETHODCALLTYPE* SetTextureStageState_t)(IDirect3DDevice9* This, DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value);
static SetTextureStageState_t Real_SetTextureStageState = NULL;

typedef HRESULT (STDMETHODCALLTYPE* SetSamplerState_t)(IDirect3DDevice9* This, DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value);
static SetSamplerState_t Real_SetSamplerState = NULL;

typedef HRESULT (STDMETHODCALLTYPE* Reset_t)(IDirect3DDevice9* This, D3DPRESENT_PARAMETERS* pPresentationParameters);
static Reset_t Real_Reset = NULL;

void InvalidateD3D9StateCaches() {
    memset(s_renderStateValid, 0, sizeof(s_renderStateValid));
    memset(s_stageStateValid, 0, sizeof(s_stageStateValid));
    memset(s_samplerStateValid, 0, sizeof(s_samplerStateValid));
    s_cachedDevice = nullptr;
}

static void PrepareD3D9StateCacheForDevice(IDirect3DDevice9* device) {
    PrepareD3DStateCacheForDevice(device, s_cachedDevice, [] {
        InvalidateD3D9StateCaches();
    });
}

static HRESULT STDMETHODCALLTYPE Hooked_SetRenderState(IDirect3DDevice9* This, D3DRENDERSTATETYPE State, DWORD Value) {
    if ((DWORD)State >= MAX_RENDER_STATES) return Real_SetRenderState(This, State, Value);
    PrepareD3D9StateCacheForDevice(This);
    return ApplyCachedD3DState(Value, s_renderStateCache[State], s_renderStateValid[State],
        [&]() { return Real_SetRenderState(This, State, Value); });
}

static HRESULT STDMETHODCALLTYPE Hooked_SetTextureStageState(IDirect3DDevice9* This, DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) {
    if (Stage >= MAX_STAGES || (DWORD)Type >= MAX_STAGE_STATES) {
        return Real_SetTextureStageState(This, Stage, Type, Value);
    }
    PrepareD3D9StateCacheForDevice(This);
    return ApplyCachedD3DState(Value, s_stageStateCache[Stage][Type], s_stageStateValid[Stage][Type],
        [&]() { return Real_SetTextureStageState(This, Stage, Type, Value); });
}

static HRESULT STDMETHODCALLTYPE Hooked_SetSamplerState(IDirect3DDevice9* This, DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) {
    if (Sampler >= MAX_SAMPLERS || (DWORD)Type >= MAX_SAMPLER_STATES) {
        return Real_SetSamplerState(This, Sampler, Type, Value);
    }
    PrepareD3D9StateCacheForDevice(This);
    return ApplyCachedD3DState(Value, s_samplerStateCache[Sampler][Type], s_samplerStateValid[Sampler][Type],
        [&]() { return Real_SetSamplerState(This, Sampler, Type, Value); });
}

static HRESULT STDMETHODCALLTYPE Hooked_Reset(IDirect3DDevice9* This, D3DPRESENT_PARAMETERS* pPresentationParameters) {
    InvalidateD3D9StateCaches();
    return Real_Reset(This, pPresentationParameters);
}

// ---------------------------------------------------------------------------
// Fast Texture Loader (D3DX Optimization)
// Eliminates CPU triangle mip-filtering and dither calculations during save loading
// ---------------------------------------------------------------------------
#define D3DX_FILTER_NONE     (1 << 0)
#define D3DX_FILTER_POINT    (2 << 0)
#define D3DX_FILTER_LINEAR   (3 << 0)
#define D3DX_FILTER_TRIANGLE (4 << 0)
#define D3DX_FILTER_BOX      (5 << 0)
#define D3DX_DEFAULT         ((DWORD)-1)

typedef HRESULT (WINAPI* D3DXCreateTextureFromFileInMemoryEx_t)(
    LPDIRECT3DDEVICE9         pDevice,
    LPCVOID                   pSrcData,
    UINT                      SrcDataSize,
    UINT                      Width,
    UINT                      Height,
    UINT                      MipLevels,
    DWORD                     Usage,
    D3DFORMAT                 Format,
    D3DPOOL                   Pool,
    DWORD                     Filter,
    DWORD                     MipFilter,
    D3DCOLOR                  ColorKey,
    void*                     pSrcInfo,
    PALETTEENTRY*             pPalette,
    LPDIRECT3DTEXTURE9*       ppTexture
);
static D3DXCreateTextureFromFileInMemoryEx_t Real_D3DXCreateTextureFromFileInMemoryEx = NULL;

static HRESULT WINAPI Hooked_D3DXCreateTextureFromFileInMemoryEx(
    LPDIRECT3DDEVICE9         pDevice,
    LPCVOID                   pSrcData,
    UINT                      SrcDataSize,
    UINT                      Width,
    UINT                      Height,
    UINT                      MipLevels,
    DWORD                     Usage,
    D3DFORMAT                 Format,
    D3DPOOL                   Pool,
    DWORD                     Filter,
    DWORD                     MipFilter,
    D3DCOLOR                  ColorKey,
    void*                     pSrcInfo,
    PALETTEENTRY*             pPalette,
    LPDIRECT3DTEXTURE9*       ppTexture
) {
    if (GetConfig().enableFastTextureLoader) {
        bool isDDS = false;
        DWORD ddsMipCount = 0;
        if (pSrcData && SrcDataSize >= 128) {
            const DWORD* dw = (const DWORD*)pSrcData;
            if (dw[0] == 0x20534444) { // 'DDS '
                isDDS = true;
                ddsMipCount = dw[7]; // dwMipMapCount in DDS_HEADER
            }
        }

        if (isDDS && ddsMipCount > 1) {
            // Pre-generated DDS mipmaps: skip expensive CPU filtering entirely
            if (MipFilter == D3DX_DEFAULT || (MipFilter & 0xF) == D3DX_FILTER_TRIANGLE) {
                MipFilter = D3DX_FILTER_NONE;
            }
        } else {
            // Mip generation needed: replace heavy CPU triangle filtering with fast 2x2 box filtering
            if (MipFilter == D3DX_DEFAULT || (MipFilter & 0xF) == D3DX_FILTER_TRIANGLE) {
                MipFilter = D3DX_FILTER_BOX;
            }
        }
    }

    return Real_D3DXCreateTextureFromFileInMemoryEx(
        pDevice, pSrcData, SrcDataSize, Width, Height,
        MipLevels, Usage, Format, Pool, Filter, MipFilter,
        ColorKey, pSrcInfo, pPalette, ppTexture
    );
}

typedef HRESULT (WINAPI* D3DXCreateTextureFromFileExA_t)(
    LPDIRECT3DDEVICE9         pDevice,
    LPCSTR                    pSrcFile,
    UINT                      Width,
    UINT                      Height,
    UINT                      MipLevels,
    DWORD                     Usage,
    D3DFORMAT                 Format,
    D3DPOOL                   Pool,
    DWORD                     Filter,
    DWORD                     MipFilter,
    D3DCOLOR                  ColorKey,
    void*                     pSrcInfo,
    PALETTEENTRY*             pPalette,
    LPDIRECT3DTEXTURE9*       ppTexture
);
static D3DXCreateTextureFromFileExA_t Real_D3DXCreateTextureFromFileExA = NULL;

static HRESULT WINAPI Hooked_D3DXCreateTextureFromFileExA(
    LPDIRECT3DDEVICE9         pDevice,
    LPCSTR                    pSrcFile,
    UINT                      Width,
    UINT                      Height,
    UINT                      MipLevels,
    DWORD                     Usage,
    D3DFORMAT                 Format,
    D3DPOOL                   Pool,
    DWORD                     Filter,
    DWORD                     MipFilter,
    D3DCOLOR                  ColorKey,
    void*                     pSrcInfo,
    PALETTEENTRY*             pPalette,
    LPDIRECT3DTEXTURE9*       ppTexture
) {
    if (GetConfig().enableFastTextureLoader) {
        if (MipFilter == D3DX_DEFAULT || (MipFilter & 0xF) == D3DX_FILTER_TRIANGLE) {
            MipFilter = D3DX_FILTER_BOX;
        }
    }

    return Real_D3DXCreateTextureFromFileExA(
        pDevice, pSrcFile, Width, Height,
        MipLevels, Usage, Format, Pool, Filter, MipFilter,
        ColorKey, pSrcInfo, pPalette, ppTexture
    );
}

static bool CreateEnableDeviceHook(void* target, LPVOID detour, LPVOID* original, void** installedTarget) {
    if (!target || MH_CreateHook(target, detour, original) != MH_OK) return false;
    if (MH_EnableHook(target) != MH_OK) {
        MH_RemoveHook(target);
        *original = nullptr;
        return false;
    }
    *installedTarget = target;
    return true;
}

static void RemoveDeviceStateFilterHooks() {
    for (void*& target : s_deviceHookTargets) {
        if (!target) continue;
        MH_DisableHook(target);
        MH_RemoveHook(target);
        target = nullptr;
    }
    Real_SetRenderState = nullptr;
    Real_SetTextureStageState = nullptr;
    Real_SetSamplerState = nullptr;
    Real_Reset = nullptr;
    s_deviceFilterInstalled = false;
    InvalidateD3D9StateCaches();
}

static void InstallDeviceStateFilter(IDirect3DDevice9* pDevice) {
    if (!pDevice) return;
    ResetD3DStateCacheForNewDevice(pDevice, s_cachedDevice, [] {
        InvalidateD3D9StateCaches();
    });
    if (s_deviceFilterInstalled) return;

    void** vtbl = *(void***)pDevice;
    if (!vtbl) return;

    // Direct3D 9 vtable indices:
    // 57: SetRenderState
    // 67: SetTextureStageState
    // 69: SetSamplerState
    // 16: Reset
    const bool installed =
        CreateEnableDeviceHook(vtbl[57], (LPVOID)Hooked_SetRenderState,
                               (LPVOID*)&Real_SetRenderState, &s_deviceHookTargets[0]) &&
        CreateEnableDeviceHook(vtbl[67], (LPVOID)Hooked_SetTextureStageState,
                               (LPVOID*)&Real_SetTextureStageState, &s_deviceHookTargets[1]) &&
        CreateEnableDeviceHook(vtbl[69], (LPVOID)Hooked_SetSamplerState,
                               (LPVOID*)&Real_SetSamplerState, &s_deviceHookTargets[2]) &&
        CreateEnableDeviceHook(vtbl[16], (LPVOID)Hooked_Reset,
                               (LPVOID*)&Real_Reset, &s_deviceHookTargets[3]);
    if (!installed) {
        RemoveDeviceStateFilterHooks();
        Log("G3O", "Direct3D 9 state filter not installed: a required hook failed.");
        return;
    }

    InvalidateD3D9StateCaches();
    s_deviceFilterInstalled = true;
    Log("G3O", "Direct3D 9 State Redundancy Filter installed on IDirect3DDevice9.");
}

void AttachD3D9DeviceIfReady(void* rawDevice) {
    IDirect3DDevice9* pDevice = reinterpret_cast<IDirect3DDevice9*>(rawDevice);
    if (!pDevice) return;
    if (GetConfig().enableD3D9StateFilter && !s_deviceFilterInstalled) {
        InstallDeviceStateFilter(pDevice);
    }
    if (GetConfig().enableProfiler && !s_presentHooked) {
        void** vtbl = *(void***)pDevice;
        if (vtbl) {
            ProfilerHookPresent(vtbl);
            s_presentHooked = true;
        }
    }
}

typedef void* (__thiscall* eCDX9Device_ctor_t)(void* pThis, IDirect3DDevice9* pDevice);
static eCDX9Device_ctor_t Real_eCDX9Device_ctor = NULL;

static void* __fastcall Hooked_eCDX9Device_ctor(void* pThis, void* /*edx*/, IDirect3DDevice9* pDevice) {
    void* result = Real_eCDX9Device_ctor(pThis, pDevice);
    AttachD3D9DeviceIfReady(pDevice);
    return result;
}

void InitializeD3D9Optimizer() {
    if (GetConfig().enableD3D9StateFilter || GetConfig().enableProfiler) {
        HMODULE hEngine = GetModuleHandleA("Engine.dll");
        if (hEngine) {
            void* pCtor = (void*)GetProcAddress(hEngine, "??0eCDX9Device@@IAE@PAUIDirect3DDevice9@@@Z");
            if (pCtor) {
                if (MH_CreateHook(pCtor, (LPVOID)Hooked_eCDX9Device_ctor, (LPVOID*)&Real_eCDX9Device_ctor) == MH_OK) {
                    MH_EnableHook(pCtor);
                    Log("G3O", "Hooked Engine.dll!eCDX9Device constructor for Direct3D 9 state filtering.");
                }
            }
        }
    }

    // Fast Texture Loader in d3dx9_40.dll
    if (GetConfig().enableFastTextureLoader) {
        HMODULE hD3DX = GetModuleHandleA("d3dx9_40.dll");
        if (!hD3DX) hD3DX = LoadLibraryA("d3dx9_40.dll");
        if (hD3DX) {
            void* pMemEx = (void*)GetProcAddress(hD3DX, "D3DXCreateTextureFromFileInMemoryEx");
            if (pMemEx) {
                if (MH_CreateHook(pMemEx, (LPVOID)Hooked_D3DXCreateTextureFromFileInMemoryEx, (LPVOID*)&Real_D3DXCreateTextureFromFileInMemoryEx) == MH_OK) {
                    MH_EnableHook(pMemEx);
                    Log("G3O", "Fast Texture Loader hooked (d3dx9_40.dll!D3DXCreateTextureFromFileInMemoryEx).");
                }
            }

            void* pFileEx = (void*)GetProcAddress(hD3DX, "D3DXCreateTextureFromFileExA");
            if (pFileEx) {
                if (MH_CreateHook(pFileEx, (LPVOID)Hooked_D3DXCreateTextureFromFileExA, (LPVOID*)&Real_D3DXCreateTextureFromFileExA) == MH_OK) {
                    MH_EnableHook(pFileEx);
                    Log("G3O", "Fast Texture Loader hooked (d3dx9_40.dll!D3DXCreateTextureFromFileExA).");
                }
            }
        }
    }
}

void ShutdownD3D9Optimizer() {
    RemoveDeviceStateFilterHooks();
    HMODULE hEngine = GetModuleHandleA("Engine.dll");
    if (hEngine) {
        void* pCtor = (void*)GetProcAddress(hEngine, "??0eCDX9Device@@IAE@PAUIDirect3DDevice9@@@Z");
        if (pCtor) MH_DisableHook(pCtor);
    }
    HMODULE hD3DX = GetModuleHandleA("d3dx9_40.dll");
    if (hD3DX) {
        void* pMemEx = (void*)GetProcAddress(hD3DX, "D3DXCreateTextureFromFileInMemoryEx");
        if (pMemEx) MH_DisableHook(pMemEx);
        void* pFileEx = (void*)GetProcAddress(hD3DX, "D3DXCreateTextureFromFileExA");
        if (pFileEx) MH_DisableHook(pFileEx);
    }
}

} // namespace G3O
