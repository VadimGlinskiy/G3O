#include "d3d9_hook.h"
#include "diagnostic_logger.h"
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

static bool s_deviceFilterInstalled = false;

typedef HRESULT (STDMETHODCALLTYPE* SetRenderState_t)(IDirect3DDevice9* This, D3DRENDERSTATETYPE State, DWORD Value);
static SetRenderState_t Real_SetRenderState = NULL;

typedef HRESULT (STDMETHODCALLTYPE* SetTextureStageState_t)(IDirect3DDevice9* This, DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value);
static SetTextureStageState_t Real_SetTextureStageState = NULL;

typedef HRESULT (STDMETHODCALLTYPE* SetSamplerState_t)(IDirect3DDevice9* This, DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value);
static SetSamplerState_t Real_SetSamplerState = NULL;

typedef HRESULT (STDMETHODCALLTYPE* Reset_t)(IDirect3DDevice9* This, D3DPRESENT_PARAMETERS* pPresentationParameters);
static Reset_t Real_Reset = NULL;

static void InvalidateStateCaches() {
    memset(s_renderStateValid, 0, sizeof(s_renderStateValid));
    memset(s_stageStateValid, 0, sizeof(s_stageStateValid));
    memset(s_samplerStateValid, 0, sizeof(s_samplerStateValid));
}

static HRESULT STDMETHODCALLTYPE Hooked_SetRenderState(IDirect3DDevice9* This, D3DRENDERSTATETYPE State, DWORD Value) {
    if ((DWORD)State < MAX_RENDER_STATES) {
        if (s_renderStateValid[State] && s_renderStateCache[State] == Value) {
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

static void InstallDeviceStateFilter(IDirect3DDevice9* pDevice) {
    if (s_deviceFilterInstalled || !pDevice) return;

    void** vtbl = *(void***)pDevice;
    if (!vtbl) return;

    // Direct3D 9 vtable indices:
    // 57: SetRenderState
    // 67: SetTextureStageState
    // 69: SetSamplerState
    // 16: Reset
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

    s_deviceFilterInstalled = true;
    Log("G3O", "Direct3D 9 State Redundancy Filter installed on IDirect3DDevice9.");
}

typedef void* (__thiscall* eCDX9Device_ctor_t)(void* pThis, IDirect3DDevice9* pDevice);
static eCDX9Device_ctor_t Real_eCDX9Device_ctor = NULL;

static void* __fastcall Hooked_eCDX9Device_ctor(void* pThis, void* /*edx*/, IDirect3DDevice9* pDevice) {
    void* result = Real_eCDX9Device_ctor(pThis, pDevice);
    if (pDevice) {
        InstallDeviceStateFilter(pDevice);
    }
    return result;
}

void InitializeD3D9Optimizer() {
    HMODULE hEngine = GetModuleHandleA("Engine.dll");
    if (!hEngine) return;

    void* pCtor = (void*)GetProcAddress(hEngine, "??0eCDX9Device@@IAE@PAUIDirect3DDevice9@@@Z");
    if (pCtor) {
        if (MH_CreateHook(pCtor, (LPVOID)Hooked_eCDX9Device_ctor, (LPVOID*)&Real_eCDX9Device_ctor) == MH_OK) {
            MH_EnableHook(pCtor);
            Log("G3O", "Hooked Engine.dll!eCDX9Device constructor for Direct3D 9 state filtering.");
        }
    }
}

void ShutdownD3D9Optimizer() {
    HMODULE hEngine = GetModuleHandleA("Engine.dll");
    if (hEngine) {
        void* pCtor = (void*)GetProcAddress(hEngine, "??0eCDX9Device@@IAE@PAUIDirect3DDevice9@@@Z");
        if (pCtor) MH_DisableHook(pCtor);
    }
}

} // namespace G3O
