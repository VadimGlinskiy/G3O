#include "d3d9_hook.h"
#include <MinHook.h>
#include <stdio.h>

#define MAX_RENDER_STATES 256
#define MAX_STAGES 8
#define MAX_STAGE_STATES 36
#define MAX_SAMPLERS 16
#define MAX_SAMPLER_STATES 16

#ifndef D3DUSAGE_AUTOGENMIPMAP
#define D3DUSAGE_AUTOGENMIPMAP (0x00000400L)
#endif

// Cache structures for state redundancy filter
static DWORD s_renderStateCache[MAX_RENDER_STATES];
static BYTE  s_renderStateValid[MAX_RENDER_STATES] = { 0 };

static DWORD s_stageStateCache[MAX_STAGES][MAX_STAGE_STATES];
static BYTE  s_stageStateValid[MAX_STAGES][MAX_STAGE_STATES] = { { 0 } };

static DWORD s_samplerStateCache[MAX_SAMPLERS][MAX_SAMPLER_STATES];
static BYTE  s_samplerStateValid[MAX_SAMPLERS][MAX_SAMPLER_STATES] = { { 0 } };

// Statistics
static volatile DWORD g_filteredRenderStates = 0;
static volatile DWORD g_filteredStageStates = 0;
static volatile DWORD g_filteredSamplerStates = 0;

void GetD3D9FilterStatistics(DWORD* pFilteredRenderStates, DWORD* pFilteredStageStates, DWORD* pFilteredSamplerStates) {
    if (pFilteredRenderStates) *pFilteredRenderStates = g_filteredRenderStates;
    if (pFilteredStageStates) *pFilteredStageStates = g_filteredStageStates;
    if (pFilteredSamplerStates) *pFilteredSamplerStates = g_filteredSamplerStates;
}

// Real function pointers
typedef IDirect3D9* (WINAPI* Direct3DCreate9_t)(UINT SDKVersion);
static Direct3DCreate9_t Real_Direct3DCreate9 = NULL;

typedef HRESULT (STDMETHODCALLTYPE* CreateDevice_t)(
    IDirect3D9* This,
    UINT Adapter,
    D3DDEVTYPE DeviceType,
    HWND hFocusWindow,
    DWORD BehaviorFlags,
    D3DPRESENT_PARAMETERS* pPresentationParameters,
    IDirect3DDevice9** ppReturnedDeviceInterface
);
static CreateDevice_t Real_CreateDevice = NULL;

typedef HRESULT (STDMETHODCALLTYPE* SetRenderState_t)(IDirect3DDevice9* This, D3DRENDERSTATETYPE State, DWORD Value);
static SetRenderState_t Real_SetRenderState = NULL;

typedef HRESULT (STDMETHODCALLTYPE* SetTextureStageState_t)(IDirect3DDevice9* This, DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value);
static SetTextureStageState_t Real_SetTextureStageState = NULL;

typedef HRESULT (STDMETHODCALLTYPE* SetSamplerState_t)(IDirect3DDevice9* This, DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value);
static SetSamplerState_t Real_SetSamplerState = NULL;

typedef HRESULT (STDMETHODCALLTYPE* Reset_t)(IDirect3DDevice9* This, D3DPRESENT_PARAMETERS* pPresentationParameters);
static Reset_t Real_Reset = NULL;

typedef HRESULT (STDMETHODCALLTYPE* Present_t)(IDirect3DDevice9* This, const RECT* pSourceRect, const RECT* pDestRect, HWND hDestWindowOverride, const RGNDATA* pDirtyRegion);
static Present_t Real_Present = NULL;

// D3DX9 function pointers
typedef HRESULT (WINAPI* D3DXFilterTexture_t)(LPDIRECT3DBASETEXTURE9 pBaseTexture, const PALETTEENTRY* pPalette, UINT SrcLevel, DWORD Filter);
static D3DXFilterTexture_t Real_D3DXFilterTexture = NULL;

static bool g_DeviceHooksInstalled = false;
static bool g_CreateDeviceHooked = false;

static void InvalidateStateCaches() {
    memset(s_renderStateValid, 0, sizeof(s_renderStateValid));
    memset(s_stageStateValid, 0, sizeof(s_stageStateValid));
    memset(s_samplerStateValid, 0, sizeof(s_samplerStateValid));
}

static HRESULT STDMETHODCALLTYPE Hooked_SetRenderState(IDirect3DDevice9* This, D3DRENDERSTATETYPE State, DWORD Value) {
    if ((DWORD)State < MAX_RENDER_STATES) {
        if (s_renderStateValid[State] && s_renderStateCache[State] == Value) {
            InterlockedIncrement((volatile LONG*)&g_filteredRenderStates);
            return D3D_OK;
        }
        s_renderStateCache[State] = Value;
        s_renderStateValid[State] = 1;
    }
    return Real_SetRenderState(This, State, Value);
}

static HRESULT STDMETHODCALLTYPE Hooked_SetTextureStageState(IDirect3DDevice9* This, DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) {
    if (Stage < MAX_STAGES && (DWORD)Type < MAX_STAGE_STATES) {
        if (s_stageStateValid[Stage][Type] && s_stageStateCache[Stage][Type] == Value) {
            InterlockedIncrement((volatile LONG*)&g_filteredStageStates);
            return D3D_OK;
        }
        s_stageStateCache[Stage][Type] = Value;
        s_stageStateValid[Stage][Type] = 1;
    }
    return Real_SetTextureStageState(This, Stage, Type, Value);
}

static HRESULT STDMETHODCALLTYPE Hooked_SetSamplerState(IDirect3DDevice9* This, DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) {
    if (Sampler < MAX_SAMPLERS && (DWORD)Type < MAX_SAMPLER_STATES) {
        if (s_samplerStateValid[Sampler][Type] && s_samplerStateCache[Sampler][Type] == Value) {
            InterlockedIncrement((volatile LONG*)&g_filteredSamplerStates);
            return D3D_OK;
        }
        s_samplerStateCache[Sampler][Type] = Value;
        s_samplerStateValid[Sampler][Type] = 1;
    }
    return Real_SetSamplerState(This, Sampler, Type, Value);
}

static HRESULT STDMETHODCALLTYPE Hooked_Reset(IDirect3DDevice9* This, D3DPRESENT_PARAMETERS* pPresentationParameters) {
    InvalidateStateCaches();
    return Real_Reset(This, pPresentationParameters);
}

static HRESULT STDMETHODCALLTYPE Hooked_Present(IDirect3DDevice9* This, const RECT* pSourceRect, const RECT* pDestRect, HWND hDestWindowOverride, const RGNDATA* pDirtyRegion) {
    static DWORD s_presentFrame = 0;
    s_presentFrame++;
    if ((s_presentFrame % 3600) == 0) {
        char buf[256];
        sprintf_s(buf, "[Gothic3Optima] State Filter: Redundant calls eliminated - RS: %u, TSS: %u, SS: %u\n",
            g_filteredRenderStates, g_filteredStageStates, g_filteredSamplerStates);
        OutputDebugStringA(buf);
    }
    return Real_Present(This, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
}

static HRESULT WINAPI Hooked_D3DXFilterTexture(LPDIRECT3DBASETEXTURE9 pBaseTexture, const PALETTEENTRY* pPalette, UINT SrcLevel, DWORD Filter) {
    if (pBaseTexture) {
        // Hardware autogen mipmaps: invoke GPU mip generation
        pBaseTexture->GenerateMipSubLevels();
        return D3D_OK;
    }
    if (Real_D3DXFilterTexture) {
        return Real_D3DXFilterTexture(pBaseTexture, pPalette, SrcLevel, Filter);
    }
    return D3D_OK;
}

static void InstallDeviceHooks(IDirect3DDevice9* pDevice) {
    if (g_DeviceHooksInstalled || !pDevice) return;

    void** vtbl = *(void***)pDevice;

    // Vtable indices:
    // Reset: 16
    // Present: 17
    // SetRenderState: 57
    // SetTextureStageState: 67
    // SetSamplerState: 69

    if (MH_CreateHook(vtbl[57], (LPVOID)Hooked_SetRenderState, (LPVOID*)&Real_SetRenderState) == MH_OK) {
        MH_EnableHook(vtbl[57]);
    }
    if (MH_CreateHook(vtbl[67], (LPVOID)Hooked_SetTextureStageState, (LPVOID*)&Real_SetTextureStageState) == MH_OK) {
        MH_EnableHook(vtbl[67]);
    }
    if (MH_CreateHook(vtbl[69], (LPVOID)Hooked_SetSamplerState, (LPVOID*)&Real_SetSamplerState) == MH_OK) {
        MH_EnableHook(vtbl[69]);
    }
    if (MH_CreateHook(vtbl[16], (LPVOID)Hooked_Reset, (LPVOID*)&Real_Reset) == MH_OK) {
        MH_EnableHook(vtbl[16]);
    }
    if (MH_CreateHook(vtbl[17], (LPVOID)Hooked_Present, (LPVOID*)&Real_Present) == MH_OK) {
        MH_EnableHook(vtbl[17]);
    }

    g_DeviceHooksInstalled = true;
    OutputDebugStringA("[Gothic3Optima] Installed IDirect3DDevice9 State Redundancy Filter hooks.\n");
}

static HRESULT STDMETHODCALLTYPE Hooked_CreateDevice(
    IDirect3D9* This,
    UINT Adapter,
    D3DDEVTYPE DeviceType,
    HWND hFocusWindow,
    DWORD BehaviorFlags,
    D3DPRESENT_PARAMETERS* pPresentationParameters,
    IDirect3DDevice9** ppReturnedDeviceInterface
) {
    DWORD originalFlags = BehaviorFlags;

    // Phase 4.3: Enforce D3DCREATE_PUREDEVICE if hardware vertex processing is used
    if (BehaviorFlags & D3DCREATE_HARDWARE_VERTEXPROCESSING) {
        BehaviorFlags |= D3DCREATE_PUREDEVICE;
        OutputDebugStringA("[Gothic3Optima] Injected D3DCREATE_PUREDEVICE into BehaviorFlags.\n");
    }

    HRESULT hr = Real_CreateDevice(This, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppReturnedDeviceInterface);
    if (FAILED(hr) && (BehaviorFlags != originalFlags)) {
        // Driver / adapter fallback without PUREDEVICE
        OutputDebugStringA("[Gothic3Optima] CreateDevice with PUREDEVICE failed, falling back to original flags.\n");
        hr = Real_CreateDevice(This, Adapter, DeviceType, hFocusWindow, originalFlags, pPresentationParameters, ppReturnedDeviceInterface);
    }

    if (SUCCEEDED(hr) && ppReturnedDeviceInterface && *ppReturnedDeviceInterface) {
        InstallDeviceHooks(*ppReturnedDeviceInterface);
    }

    return hr;
}

static void HookD3D9Interface(IDirect3D9* pD3D) {
    if (g_CreateDeviceHooked || !pD3D) return;

    void** vtbl = *(void***)pD3D;
    // Index 16: CreateDevice
    if (MH_CreateHook(vtbl[16], (LPVOID)Hooked_CreateDevice, (LPVOID*)&Real_CreateDevice) == MH_OK) {
        MH_EnableHook(vtbl[16]);
        g_CreateDeviceHooked = true;
        OutputDebugStringA("[Gothic3Optima] Hooked IDirect3D9::CreateDevice.\n");
    }
}

static IDirect3D9* WINAPI Hooked_Direct3DCreate9(UINT SDKVersion) {
    IDirect3D9* pD3D = Real_Direct3DCreate9(SDKVersion);
    if (pD3D) {
        HookD3D9Interface(pD3D);
    }
    return pD3D;
}

void InstallD3D9Hooks() {
    HMODULE hD3D9 = GetModuleHandleA("d3d9.dll");
    if (!hD3D9) {
        hD3D9 = LoadLibraryA("d3d9.dll");
    }

    if (hD3D9) {
        void* pDirect3DCreate9 = (void*)GetProcAddress(hD3D9, "Direct3DCreate9");
        if (pDirect3DCreate9) {
            if (MH_CreateHook(pDirect3DCreate9, (LPVOID)Hooked_Direct3DCreate9, (LPVOID*)&Real_Direct3DCreate9) == MH_OK) {
                MH_EnableHook(pDirect3DCreate9);
                OutputDebugStringA("[Gothic3Optima] Hooked Direct3DCreate9 entry point.\n");
            }
        }
    }

    // Hook D3DXFilterTexture in d3dx9_40.dll if available
    HMODULE hD3DX9 = GetModuleHandleA("d3dx9_40.dll");
    if (!hD3DX9) {
        hD3DX9 = LoadLibraryA("d3dx9_40.dll");
    }
    if (hD3DX9) {
        void* pFilter = (void*)GetProcAddress(hD3DX9, "D3DXFilterTexture");
        if (pFilter) {
            if (MH_CreateHook(pFilter, (LPVOID)Hooked_D3DXFilterTexture, (LPVOID*)&Real_D3DXFilterTexture) == MH_OK) {
                MH_EnableHook(pFilter);
                OutputDebugStringA("[Gothic3Optima] Hooked D3DXFilterTexture (hardware mipmap bypass).\n");
            }
        }
    }
}
