#include "d3d9_hook.h"
#include "diagnostic_logger.h"
#include "config_manager.h"
#include "profiler.h"
#include "d3d_state_cache.h"
#include "blackbox_recorder.h"
#include "hook_install_policy.h"
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
static IDirect3DDevice9* s_attachedDevice = nullptr;
static void** s_deviceVtable = nullptr;
static IDirect3DDevice9* s_stateBlockRecordingDevice = nullptr;

static bool s_deviceFilterInstalled = false;
static bool s_presentHooked = false;
static bool s_stateCacheEnabled = false;
static bool s_stateBlockApplyInProgress = false;
static bool s_stateBlockApplyHookUnavailable = false;
static void* s_deviceHookTargets[7] = {};
static void* s_stateBlockApplyHookTarget = nullptr;

typedef HRESULT (STDMETHODCALLTYPE* SetRenderState_t)(IDirect3DDevice9* This, D3DRENDERSTATETYPE State, DWORD Value);
static SetRenderState_t Real_SetRenderState = NULL;

typedef HRESULT (STDMETHODCALLTYPE* SetTextureStageState_t)(IDirect3DDevice9* This, DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value);
static SetTextureStageState_t Real_SetTextureStageState = NULL;

typedef HRESULT (STDMETHODCALLTYPE* SetSamplerState_t)(IDirect3DDevice9* This, DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value);
static SetSamplerState_t Real_SetSamplerState = NULL;

typedef HRESULT (STDMETHODCALLTYPE* Reset_t)(IDirect3DDevice9* This, D3DPRESENT_PARAMETERS* pPresentationParameters);
static Reset_t Real_Reset = NULL;
typedef HRESULT (STDMETHODCALLTYPE* CreateStateBlock_t)(IDirect3DDevice9* This, D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9** ppSB);
static CreateStateBlock_t Real_CreateStateBlock = NULL;
typedef HRESULT (STDMETHODCALLTYPE* BeginStateBlock_t)(IDirect3DDevice9* This);
static BeginStateBlock_t Real_BeginStateBlock = NULL;
typedef HRESULT (STDMETHODCALLTYPE* EndStateBlock_t)(IDirect3DDevice9* This, IDirect3DStateBlock9** ppSB);
static EndStateBlock_t Real_EndStateBlock = NULL;
typedef HRESULT (STDMETHODCALLTYPE* StateBlockApply_t)(IDirect3DStateBlock9* This);
static StateBlockApply_t Real_StateBlockApply = NULL;

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

static bool ShouldBypassD3D9StateCache() {
    return !s_stateCacheEnabled || s_stateBlockRecordingDevice != nullptr || s_stateBlockApplyInProgress;
}

static HRESULT STDMETHODCALLTYPE Hooked_SetRenderState(IDirect3DDevice9* This, D3DRENDERSTATETYPE State, DWORD Value) {
    if ((DWORD)State >= MAX_RENDER_STATES) return Real_SetRenderState(This, State, Value);
    PrepareD3D9StateCacheForDevice(This);
    return ApplyCachedD3DState(Value, s_renderStateCache[State], s_renderStateValid[State],
        [&]() { return Real_SetRenderState(This, State, Value); }, ShouldBypassD3D9StateCache());
}

static HRESULT STDMETHODCALLTYPE Hooked_SetTextureStageState(IDirect3DDevice9* This, DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) {
    if (Stage >= MAX_STAGES || (DWORD)Type >= MAX_STAGE_STATES) {
        return Real_SetTextureStageState(This, Stage, Type, Value);
    }
    PrepareD3D9StateCacheForDevice(This);
    return ApplyCachedD3DState(Value, s_stageStateCache[Stage][Type], s_stageStateValid[Stage][Type],
        [&]() { return Real_SetTextureStageState(This, Stage, Type, Value); }, ShouldBypassD3D9StateCache());
}

static HRESULT STDMETHODCALLTYPE Hooked_SetSamplerState(IDirect3DDevice9* This, DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) {
    if (Sampler >= MAX_SAMPLERS || (DWORD)Type >= MAX_SAMPLER_STATES) {
        return Real_SetSamplerState(This, Sampler, Type, Value);
    }
    PrepareD3D9StateCacheForDevice(This);
    return ApplyCachedD3DState(Value, s_samplerStateCache[Sampler][Type], s_samplerStateValid[Sampler][Type],
        [&]() { return Real_SetSamplerState(This, Sampler, Type, Value); }, ShouldBypassD3D9StateCache());
}

static HRESULT STDMETHODCALLTYPE Hooked_Reset(IDirect3DDevice9* This, D3DPRESENT_PARAMETERS* pPresentationParameters) {
    InvalidateD3D9StateCaches();
    const HRESULT result = Real_Reset(This, pPresentationParameters);
    if (SUCCEEDED(result) && s_stateBlockRecordingDevice == This) {
        s_stateBlockRecordingDevice = nullptr;
    }
    InvalidateD3D9StateCaches();
    return result;
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

    BlackboxScope bb(BB_OP_CREATING_TEXTURE, "TextureInMemory", (uint64_t)(uintptr_t)pSrcData, SrcDataSize);
    HRESULT hr = Real_D3DXCreateTextureFromFileInMemoryEx(
        pDevice, pSrcData, SrcDataSize, Width, Height,
        MipLevels, Usage, Format, Pool, Filter, MipFilter,
        ColorKey, pSrcInfo, pPalette, ppTexture
    );
    if (FAILED(hr)) {
        const ThreadBlackboxSlot* slot = Blackbox_GetSlotForThread(GetCurrentThreadId());
        const char* corrArchive = (slot && slot->archive_name[0]) ? slot->archive_name : "None";
        uint64_t corrOffset = slot ? slot->offset : 0;
        Log("Texture", "[FAILED hr=0x%08X] D3DXCreateTextureFromFileInMemoryEx failed: size=%u, %ux%u, fmt=%u, pool=%u. Correlated PAK: '%s' (offset 0x%llX)",
            hr, SrcDataSize, Width, Height, (unsigned int)Format, (unsigned int)Pool, corrArchive, corrOffset);
    }
    return hr;
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

    BlackboxScope bb(BB_OP_CREATING_TEXTURE, "TextureFile", 0, 0, pSrcFile ? pSrcFile : "");
    HRESULT hr = Real_D3DXCreateTextureFromFileExA(
        pDevice, pSrcFile, Width, Height,
        MipLevels, Usage, Format, Pool, Filter, MipFilter,
        ColorKey, pSrcInfo, pPalette, ppTexture
    );
    if (FAILED(hr)) {
        Log("Texture", "[FAILED hr=0x%08X] D3DXCreateTextureFromFileExA failed for file '%s': %ux%u, fmt=%u, pool=%u",
            hr, pSrcFile ? pSrcFile : "NULL", Width, Height, (unsigned int)Format, (unsigned int)Pool);
    }
    return hr;
}

static bool CreateEnableDeviceHook(void* target, LPVOID detour, LPVOID* original, void** installedTarget) {
    if (!original || !installedTarget) return false;
    *original = nullptr;
    *installedTarget = nullptr;
    if (!target) return false;
    if (MH_CreateHook(target, detour, original) != MH_OK) {
        *original = nullptr;
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        MH_RemoveHook(target);
        *original = nullptr;
        return false;
    }
    *installedTarget = target;
    return true;
}

static void DisableD3D9StateCache(const char* reason) {
    if (!s_stateCacheEnabled) return;
    s_stateCacheEnabled = false;
    InvalidateD3D9StateCaches();
    Log("G3O", "Direct3D 9 state cache disabled; calls now pass through: %s", reason);
}

static HRESULT STDMETHODCALLTYPE Hooked_StateBlockApply(IDirect3DStateBlock9* This) {
    return ApplyD3DStateBlockAndInvalidateCache(
        [&]() { return Real_StateBlockApply(This); },
        []() { InvalidateD3D9StateCaches(); },
        s_stateBlockApplyInProgress);
}

static void EnsureStateBlockApplyHook(IDirect3DStateBlock9* pStateBlock) {
    if (s_stateBlockApplyHookUnavailable) return;
    void** vtbl = pStateBlock ? *reinterpret_cast<void***>(pStateBlock) : nullptr;
    void* target = vtbl ? vtbl[5] : nullptr;
    if (!target) {
        s_stateBlockApplyHookUnavailable = true;
        DisableD3D9StateCache("state-block Apply entry point was unavailable");
        return;
    }
    if (s_stateBlockApplyHookTarget == target) return;
    if (s_stateBlockApplyHookTarget) {
        s_stateBlockApplyHookUnavailable = true;
        DisableD3D9StateCache("multiple state-block implementations were encountered");
        return;
    }
    if (!CreateEnableDeviceHook(target, (LPVOID)Hooked_StateBlockApply,
                                (LPVOID*)&Real_StateBlockApply, &s_stateBlockApplyHookTarget)) {
        s_stateBlockApplyHookUnavailable = true;
        DisableD3D9StateCache("state-block Apply hook could not be installed");
    }
}

static HRESULT STDMETHODCALLTYPE Hooked_CreateStateBlock(
    IDirect3DDevice9* This, D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9** ppSB) {
    const HRESULT result = Real_CreateStateBlock(This, Type, ppSB);
    if (SUCCEEDED(result) && ppSB && *ppSB) EnsureStateBlockApplyHook(*ppSB);
    return result;
}

static HRESULT STDMETHODCALLTYPE Hooked_BeginStateBlock(IDirect3DDevice9* This) {
    const HRESULT result = Real_BeginStateBlock(This);
    if (SUCCEEDED(result)) {
        if (s_stateBlockRecordingDevice && s_stateBlockRecordingDevice != This) {
            DisableD3D9StateCache("state-block recording began on multiple devices");
        }
        s_stateBlockRecordingDevice = This;
    }
    InvalidateD3D9StateCaches();
    return result;
}

static HRESULT STDMETHODCALLTYPE Hooked_EndStateBlock(IDirect3DDevice9* This, IDirect3DStateBlock9** ppSB) {
    const HRESULT result = Real_EndStateBlock(This, ppSB);
    if (s_stateBlockRecordingDevice == This) {
        s_stateBlockRecordingDevice = nullptr;
        if (FAILED(result)) {
            DisableD3D9StateCache("state-block recording could not be finalized");
        }
    }
    InvalidateD3D9StateCaches();
    if (SUCCEEDED(result) && ppSB && *ppSB) EnsureStateBlockApplyHook(*ppSB);
    return result;
}

static void RemoveDeviceStateFilterHooks() {
    for (void*& target : s_deviceHookTargets) {
        if (!target) continue;
        MH_DisableHook(target);
        MH_RemoveHook(target);
        target = nullptr;
    }
    if (s_stateBlockApplyHookTarget) {
        MH_DisableHook(s_stateBlockApplyHookTarget);
        MH_RemoveHook(s_stateBlockApplyHookTarget);
        s_stateBlockApplyHookTarget = nullptr;
    }
    Real_SetRenderState = nullptr;
    Real_SetTextureStageState = nullptr;
    Real_SetSamplerState = nullptr;
    Real_Reset = nullptr;
    Real_CreateStateBlock = nullptr;
    Real_BeginStateBlock = nullptr;
    Real_EndStateBlock = nullptr;
    Real_StateBlockApply = nullptr;
    s_stateBlockRecordingDevice = nullptr;
    s_stateCacheEnabled = false;
    s_stateBlockApplyInProgress = false;
    s_stateBlockApplyHookUnavailable = false;
    s_deviceFilterInstalled = false;
    s_attachedDevice = nullptr;
    s_deviceVtable = nullptr;
    InvalidateD3D9StateCaches();
}

static void InstallDeviceStateFilter(IDirect3DDevice9* pDevice) {
    if (!pDevice) return;
    void** vtbl = *reinterpret_cast<void***>(pDevice);
    if (!vtbl) return;
    if (s_deviceFilterInstalled) {
        if (!s_deviceVtable) return;
        if (vtbl != s_deviceVtable) {
            DisableD3D9StateCache("multiple Direct3D device implementations were encountered");
            return;
        }
        if (s_stateCacheEnabled && s_attachedDevice != pDevice) {
            D3DDEVICE_CREATION_PARAMETERS creationParameters = {};
            const HRESULT creationParametersResult = pDevice->GetCreationParameters(&creationParameters);
            if (!CanUseSingleThreadedD3D9StateCache(creationParametersResult,
                                                     creationParameters.BehaviorFlags)) {
                DisableD3D9StateCache("a new device is multithreaded or its threading flags are unavailable");
            }
            s_attachedDevice = pDevice;
            InvalidateD3D9StateCaches();
        }
        return;
    }
    ResetD3DStateCacheForNewDevice(pDevice, s_cachedDevice, [] {
        InvalidateD3D9StateCaches();
    });

    D3DDEVICE_CREATION_PARAMETERS creationParameters = {};
    const HRESULT creationParametersResult = pDevice->GetCreationParameters(&creationParameters);
    if (!CanUseSingleThreadedD3D9StateCache(creationParametersResult,
                                             creationParameters.BehaviorFlags)) {
        s_stateCacheEnabled = false;
        s_deviceFilterInstalled = true;
        if (FAILED(creationParametersResult)) {
            Log("G3O", "Direct3D 9 state filter disabled: device threading flags could not be verified (hr=0x%08X).",
                creationParametersResult);
        } else {
            Log("G3O", "Direct3D 9 state filter disabled: multithreaded device state is not safely cacheable.");
        }
        return;
    }

    // Direct3D 9 vtable indices:
    // 16: Reset
    // 57: SetRenderState
    // 59: CreateStateBlock
    // 60: BeginStateBlock
    // 61: EndStateBlock
    // 67: SetTextureStageState
    // 69: SetSamplerState
    const bool installed =
        CreateEnableDeviceHook(vtbl[57], (LPVOID)Hooked_SetRenderState,
                               (LPVOID*)&Real_SetRenderState, &s_deviceHookTargets[0]) &&
        CreateEnableDeviceHook(vtbl[67], (LPVOID)Hooked_SetTextureStageState,
                               (LPVOID*)&Real_SetTextureStageState, &s_deviceHookTargets[1]) &&
        CreateEnableDeviceHook(vtbl[69], (LPVOID)Hooked_SetSamplerState,
                               (LPVOID*)&Real_SetSamplerState, &s_deviceHookTargets[2]) &&
        CreateEnableDeviceHook(vtbl[16], (LPVOID)Hooked_Reset,
                               (LPVOID*)&Real_Reset, &s_deviceHookTargets[3]) &&
        CreateEnableDeviceHook(vtbl[59], (LPVOID)Hooked_CreateStateBlock,
                               (LPVOID*)&Real_CreateStateBlock, &s_deviceHookTargets[4]) &&
        CreateEnableDeviceHook(vtbl[60], (LPVOID)Hooked_BeginStateBlock,
                               (LPVOID*)&Real_BeginStateBlock, &s_deviceHookTargets[5]) &&
        CreateEnableDeviceHook(vtbl[61], (LPVOID)Hooked_EndStateBlock,
                               (LPVOID*)&Real_EndStateBlock, &s_deviceHookTargets[6]);
    if (!installed) {
        RemoveDeviceStateFilterHooks();
        Log("G3O", "Direct3D 9 state filter not installed: a required hook failed.");
        return;
    }

    s_attachedDevice = pDevice;
    s_deviceVtable = vtbl;
    InvalidateD3D9StateCaches();
    s_stateBlockApplyHookUnavailable = false;
    s_stateCacheEnabled = true;

    IDirect3DStateBlock9* stateBlockProbe = nullptr;
    const HRESULT probeResult = Real_CreateStateBlock(pDevice, D3DSBT_ALL, &stateBlockProbe);
    const bool stateBlockProbeSucceeded = SUCCEEDED(probeResult) && stateBlockProbe != nullptr;
    if (stateBlockProbeSucceeded) EnsureStateBlockApplyHook(stateBlockProbe);
    if (stateBlockProbe) stateBlockProbe->Release();
    if (!stateBlockProbeSucceeded) {
        s_stateBlockApplyHookUnavailable = true;
        DisableD3D9StateCache("could not validate the state-block Apply hook");
    }

    s_deviceFilterInstalled = true;
    if (s_stateCacheEnabled) {
        Log("G3O", "Direct3D 9 State Redundancy Filter installed with state-block tracking.");
    } else {
        Log("G3O", "Direct3D 9 state hooks installed, but state caching is disabled for safety.");
    }
}

void AttachD3D9DeviceIfReady(void* rawDevice) {
    IDirect3DDevice9* pDevice = reinterpret_cast<IDirect3DDevice9*>(rawDevice);
    if (!pDevice) return;
    if (GetConfig().enableD3D9StateFilter) {
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
static void* s_targetD3D9DeviceCtor = nullptr;
static void* s_targetTextureFromMemory = nullptr;
static void* s_targetTextureFromFile = nullptr;

static bool TryAttachExistingD3D9Device(HMODULE hEngine) {
    if (!hEngine) return false;
    // Engine.dll + 0x602DA0 holds the global m_pAPIDevice pointer (eCDX9Device*)
    BYTE* pBase = reinterpret_cast<BYTE*>(hEngine);
    void** ppAPIDevice = reinterpret_cast<void**>(pBase + 0x602DA0);
    if (!IsBadReadPtr(ppAPIDevice, sizeof(void*)) && *ppAPIDevice) {
        BYTE* pAPIDevice = reinterpret_cast<BYTE*>(*ppAPIDevice);
        if (!IsBadReadPtr(pAPIDevice, 16)) {
            void* pVTable = *reinterpret_cast<void**>(pAPIDevice);
            void* expectedVTable = reinterpret_cast<void*>(pBase + 0x42AAFC); // ??_7eCDX9Device@@6B@
            if (pVTable == expectedVTable) {
                IDirect3DDevice9* pDevice = *reinterpret_cast<IDirect3DDevice9**>(pAPIDevice + 0x08);
                if (pDevice) {
                    Log("G3O", "Direct3D 9 device captured from active eCDX9Device singleton (device=%p).", pDevice);
                    AttachD3D9DeviceIfReady(pDevice);
                    return true;
                }
            }
        }
    }
    return false;
}

static void* __fastcall Hooked_eCDX9Device_ctor(void* pThis, void* /*edx*/, IDirect3DDevice9* pDevice) {
    void* result = Real_eCDX9Device_ctor(pThis, pDevice);
    InvalidateD3D9StateCaches();
    AttachD3D9DeviceIfReady(pDevice);
    return result;
}

void InitializeD3D9Optimizer() {
    if (GetConfig().enableD3D9StateFilter || GetConfig().enableProfiler) {
        HMODULE hEngine = GetModuleHandleA("Engine.dll");
        if (!hEngine) {
            Log("G3O", "D3D9 device attachment hook not installed: Engine.dll is not loaded.");
        } else {
            // First attempt to capture the device if eCDX9Device has already been constructed
            if (!TryAttachExistingD3D9Device(hEngine)) {
                void* pCtor = (void*)GetProcAddress(hEngine, "??0eCDX9Device@@IAE@PAUIDirect3DDevice9@@@Z");
                if (!pCtor) {
                    Log("G3O", "D3D9 device attachment hook not installed: eCDX9Device constructor export was not found.");
                } else {
                    MH_STATUS createStatus = MH_OK, enableStatus = MH_OK, removeStatus = MH_OK;
                    const HookInstallResult result = InstallHookSafely(
                        [&] { createStatus = MH_CreateHook(pCtor, (LPVOID)Hooked_eCDX9Device_ctor,
                                                           (LPVOID*)&Real_eCDX9Device_ctor); return createStatus == MH_OK; },
                        [&] { enableStatus = MH_EnableHook(pCtor); return enableStatus == MH_OK; },
                        [&] { removeStatus = MH_RemoveHook(pCtor); },
                        [&] { Real_eCDX9Device_ctor = nullptr; });
                    if (result == HookInstallResult::Installed) {
                        s_targetD3D9DeviceCtor = pCtor;
                        Log("G3O", "Hooked Engine.dll!eCDX9Device constructor for Direct3D 9 state filtering.");
                    } else {
                        Log("G3O", "D3D9 device attachment hook failed (create=%d, enable=%d, remove=%d).",
                            (int)createStatus, (int)enableStatus, (int)removeStatus);
                    }
                }
            }
        }
    }

    // Fast Texture Loader & Texture Diagnostics in d3dx9_40.dll
    if (GetConfig().enableFastTextureLoader || GetConfig().enableCrashInvestigator) {
        HMODULE hD3DX = GetModuleHandleA("d3dx9_40.dll");
        if (!hD3DX) hD3DX = LoadLibraryA("d3dx9_40.dll");
        if (hD3DX) {
            void* pMemEx = (void*)GetProcAddress(hD3DX, "D3DXCreateTextureFromFileInMemoryEx");
            if (pMemEx) {
                MH_STATUS createStatus = MH_OK;
                MH_STATUS enableStatus = MH_OK;
                MH_STATUS removeStatus = MH_OK;
                const HookInstallResult result = InstallHookSafely(
                    [&] {
                        createStatus = MH_CreateHook(pMemEx, (LPVOID)Hooked_D3DXCreateTextureFromFileInMemoryEx,
                                                     (LPVOID*)&Real_D3DXCreateTextureFromFileInMemoryEx);
                        return createStatus == MH_OK;
                    },
                    [&] {
                        enableStatus = MH_EnableHook(pMemEx);
                        return enableStatus == MH_OK;
                    },
                    [&] { removeStatus = MH_RemoveHook(pMemEx); },
                    [&] { Real_D3DXCreateTextureFromFileInMemoryEx = nullptr; });
                if (result == HookInstallResult::Installed) {
                    s_targetTextureFromMemory = pMemEx;
                    Log("G3O", "Fast Texture Loader hooked (d3dx9_40.dll!D3DXCreateTextureFromFileInMemoryEx).");
                } else if (result == HookInstallResult::CreateFailed) {
                    Log("G3O", "Fast Texture Loader: failed to create memory-texture hook (MinHook code %d).", (int)createStatus);
                } else {
                    Log("G3O", "Fast Texture Loader: failed to enable memory-texture hook (MinHook code %d; remove code %d).",
                        (int)enableStatus, (int)removeStatus);
                }
            }

            void* pFileEx = (void*)GetProcAddress(hD3DX, "D3DXCreateTextureFromFileExA");
            if (pFileEx) {
                MH_STATUS createStatus = MH_OK;
                MH_STATUS enableStatus = MH_OK;
                MH_STATUS removeStatus = MH_OK;
                const HookInstallResult result = InstallHookSafely(
                    [&] {
                        createStatus = MH_CreateHook(pFileEx, (LPVOID)Hooked_D3DXCreateTextureFromFileExA,
                                                     (LPVOID*)&Real_D3DXCreateTextureFromFileExA);
                        return createStatus == MH_OK;
                    },
                    [&] {
                        enableStatus = MH_EnableHook(pFileEx);
                        return enableStatus == MH_OK;
                    },
                    [&] { removeStatus = MH_RemoveHook(pFileEx); },
                    [&] { Real_D3DXCreateTextureFromFileExA = nullptr; });
                if (result == HookInstallResult::Installed) {
                    s_targetTextureFromFile = pFileEx;
                    Log("G3O", "Fast Texture Loader hooked (d3dx9_40.dll!D3DXCreateTextureFromFileExA).");
                } else if (result == HookInstallResult::CreateFailed) {
                    Log("G3O", "Fast Texture Loader: failed to create file-texture hook (MinHook code %d).", (int)createStatus);
                } else {
                    Log("G3O", "Fast Texture Loader: failed to enable file-texture hook (MinHook code %d; remove code %d).",
                        (int)enableStatus, (int)removeStatus);
                }
            }
        }
    }
}

void ShutdownD3D9Optimizer() {
    RemoveDeviceStateFilterHooks();
    if (s_targetD3D9DeviceCtor) {
        MH_DisableHook(s_targetD3D9DeviceCtor);
        MH_RemoveHook(s_targetD3D9DeviceCtor);
        s_targetD3D9DeviceCtor = nullptr;
        Real_eCDX9Device_ctor = nullptr;
    }
    if (s_targetTextureFromMemory) {
        MH_DisableHook(s_targetTextureFromMemory);
        MH_RemoveHook(s_targetTextureFromMemory);
        s_targetTextureFromMemory = nullptr;
        Real_D3DXCreateTextureFromFileInMemoryEx = nullptr;
    }
    if (s_targetTextureFromFile) {
        MH_DisableHook(s_targetTextureFromFile);
        MH_RemoveHook(s_targetTextureFromFile);
        s_targetTextureFromFile = nullptr;
        Real_D3DXCreateTextureFromFileExA = nullptr;
    }
}

} // namespace G3O
