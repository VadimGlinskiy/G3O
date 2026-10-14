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
static volatile DWORD g_tunedDepthBiasCalls = 0;
static volatile DWORD g_scaledVertexBuffers = 0;
static volatile DWORD g_scaledIndexBuffers = 0;
static volatile DWORD g_promoted32BitIndexBuffers = 0;

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

typedef HRESULT (STDMETHODCALLTYPE* GetDeviceCaps_t)(
    IDirect3D9* This,
    UINT Adapter,
    D3DDEVTYPE DeviceType,
    D3DCAPS9* pCaps
);
static GetDeviceCaps_t Real_GetDeviceCaps = NULL;

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

typedef HRESULT (STDMETHODCALLTYPE* CreateVertexBuffer_t)(
    IDirect3DDevice9* This,
    UINT Length,
    DWORD Usage,
    DWORD FVF,
    D3DPOOL Pool,
    IDirect3DVertexBuffer9** ppVertexBuffer,
    HANDLE* pSharedHandle
);
static CreateVertexBuffer_t Real_CreateVertexBuffer = NULL;

typedef HRESULT (STDMETHODCALLTYPE* CreateIndexBuffer_t)(
    IDirect3DDevice9* This,
    UINT Length,
    DWORD Usage,
    D3DFORMAT Format,
    D3DPOOL Pool,
    IDirect3DIndexBuffer9** ppIndexBuffer,
    HANDLE* pSharedHandle
);
static CreateIndexBuffer_t Real_CreateIndexBuffer = NULL;

struct SIndexSubDesc {
    DWORD* pIndices;
    DWORD  numIndices;
};

struct SIndexDescArray {
    WORD capacity;
    WORD count;
    SIndexSubDesc** pItems;
};

typedef bool (__thiscall* eCVertexProcessorIndex_CreateBuffer_t)(void* pThis, SIndexDescArray* pDescs);
static eCVertexProcessorIndex_CreateBuffer_t Real_eCVertexProcessorIndex_CreateBuffer = NULL;
static BYTE* s_pIndex32CheckJump = NULL;
static CRITICAL_SECTION s_index32Lock;
static bool s_index32LockInit = false;

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
    // Phase 17.2: Shadow Acne & Depth Bias Tuning
    // Compensate self-shadowing artifacts on faces, cliffs, terrain, and foliage
    if (State == D3DRS_DEPTHBIAS) {
        float fBias = *reinterpret_cast<float*>(&Value);
        if (fBias != 0.0f) {
            // Apply 1.35x bias scaling to cleanly separate shadow receiver from occluder
            fBias *= 1.35f;
            Value = *reinterpret_cast<DWORD*>(&fBias);
            InterlockedIncrement((volatile LONG*)&g_tunedDepthBiasCalls);
        }
    } else if (State == D3DRS_SLOPESCALEDEPTHBIAS) {
        float fSlope = *reinterpret_cast<float*>(&Value);
        if (fSlope != 0.0f) {
            // Amplify slope-dependent bias by 1.50x to eliminate zebra-striping on grazing angles
            fSlope *= 1.50f;
            Value = *reinterpret_cast<DWORD*>(&fSlope);
            InterlockedIncrement((volatile LONG*)&g_tunedDepthBiasCalls);
        }
    }

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

static HRESULT STDMETHODCALLTYPE Hooked_CreateVertexBuffer(
    IDirect3DDevice9* This,
    UINT Length,
    DWORD Usage,
    DWORD FVF,
    D3DPOOL Pool,
    IDirect3DVertexBuffer9** ppVertexBuffer,
    HANDLE* pSharedHandle
) {
    UINT newLength = Length;
    if (Usage & D3DUSAGE_DYNAMIC) {
        // Enforce minimum capacity of 1 MB and double small/medium buffers up to 8 MB
        if (newLength < 1048576) {
            newLength = 1048576;
        } else if (newLength < 8388608) {
            newLength *= 2;
        }
        newLength = (newLength + 4095) & ~4095;
        InterlockedIncrement((volatile LONG*)&g_scaledVertexBuffers);
    }

    HRESULT hr = Real_CreateVertexBuffer(This, newLength, Usage, FVF, Pool, ppVertexBuffer, pSharedHandle);
    if (FAILED(hr) && newLength != Length) {
        hr = Real_CreateVertexBuffer(This, Length, Usage, FVF, Pool, ppVertexBuffer, pSharedHandle);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hooked_CreateIndexBuffer(
    IDirect3DDevice9* This,
    UINT Length,
    DWORD Usage,
    D3DFORMAT Format,
    D3DPOOL Pool,
    IDirect3DIndexBuffer9** ppIndexBuffer,
    HANDLE* pSharedHandle
) {
    UINT newLength = Length;
    if (Usage & D3DUSAGE_DYNAMIC) {
        // Enforce minimum capacity of 512 KB and double small/medium buffers up to 4 MB
        if (newLength < 524288) {
            newLength = 524288;
        } else if (newLength < 4194304) {
            newLength *= 2;
        }
        newLength = (newLength + 255) & ~255;
        InterlockedIncrement((volatile LONG*)&g_scaledIndexBuffers);
    }

    HRESULT hr = Real_CreateIndexBuffer(This, newLength, Usage, Format, Pool, ppIndexBuffer, pSharedHandle);
    if (FAILED(hr) && newLength != Length) {
        hr = Real_CreateIndexBuffer(This, Length, Usage, Format, Pool, ppIndexBuffer, pSharedHandle);
    }
    return hr;
}

static bool __fastcall Hooked_eCVertexProcessorIndex_CreateBuffer(void* pThis, void* /*edx*/, SIndexDescArray* pDescs) {
    bool force32Bit = false;
    if (pDescs && pDescs->pItems && pDescs->count > 0) {
        DWORD totalIndices = 0;
        for (WORD i = 0; i < pDescs->count; ++i) {
            SIndexSubDesc* pSub = pDescs->pItems[i];
            if (!pSub) continue;
            totalIndices += pSub->numIndices;
            if (pSub->pIndices) {
                for (DWORD k = 0; k < pSub->numIndices; ++k) {
                    if (pSub->pIndices[k] > 0xFFFF) {
                        force32Bit = true;
                        break;
                    }
                }
            }
            if (force32Bit) break;
        }
        if (totalIndices > 0xFFFF) {
            force32Bit = true;
        }
    }

    if (force32Bit && s_pIndex32CheckJump) {
        if (!s_index32LockInit) {
            InitializeCriticalSection(&s_index32Lock);
            s_index32LockInit = true;
        }
        EnterCriticalSection(&s_index32Lock);

        DWORD oldProt = 0;
        VirtualProtect(s_pIndex32CheckJump, 2, PAGE_EXECUTE_READWRITE, &oldProt);
        BYTE orig0 = s_pIndex32CheckJump[0];
        BYTE orig1 = s_pIndex32CheckJump[1];
        s_pIndex32CheckJump[0] = 0x90; // NOP
        s_pIndex32CheckJump[1] = 0x90; // NOP
        VirtualProtect(s_pIndex32CheckJump, 2, oldProt, &oldProt);

        InterlockedIncrement((volatile LONG*)&g_promoted32BitIndexBuffers);

        bool result = Real_eCVertexProcessorIndex_CreateBuffer(pThis, pDescs);

        VirtualProtect(s_pIndex32CheckJump, 2, PAGE_EXECUTE_READWRITE, &oldProt);
        s_pIndex32CheckJump[0] = orig0;
        s_pIndex32CheckJump[1] = orig1;
        VirtualProtect(s_pIndex32CheckJump, 2, oldProt, &oldProt);

        LeaveCriticalSection(&s_index32Lock);
        return result;
    }

    return Real_eCVertexProcessorIndex_CreateBuffer(pThis, pDescs);
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
        sprintf_s(buf, "[Gothic3Optima] D3D9 Filter: RS: %u, TSS: %u, SS: %u | Shadow Bias: %u | VB Scaled: %u, IB Scaled: %u, IB32: %u\n",
            g_filteredRenderStates, g_filteredStageStates, g_filteredSamplerStates, g_tunedDepthBiasCalls,
            g_scaledVertexBuffers, g_scaledIndexBuffers, g_promoted32BitIndexBuffers);
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
    // Reset: 19
    // Present: 20
    // CreateVertexBuffer: 26
    // CreateIndexBuffer: 27
    // SetRenderState: 60
    // SetTextureStageState: 70
    // SetSamplerState: 72

    if (MH_CreateHook(vtbl[60], (LPVOID)Hooked_SetRenderState, (LPVOID*)&Real_SetRenderState) == MH_OK) {
        MH_EnableHook(vtbl[60]);
    }
    if (MH_CreateHook(vtbl[70], (LPVOID)Hooked_SetTextureStageState, (LPVOID*)&Real_SetTextureStageState) == MH_OK) {
        MH_EnableHook(vtbl[70]);
    }
    if (MH_CreateHook(vtbl[72], (LPVOID)Hooked_SetSamplerState, (LPVOID*)&Real_SetSamplerState) == MH_OK) {
        MH_EnableHook(vtbl[72]);
    }
    if (MH_CreateHook(vtbl[26], (LPVOID)Hooked_CreateVertexBuffer, (LPVOID*)&Real_CreateVertexBuffer) == MH_OK) {
        MH_EnableHook(vtbl[26]);
    }
    if (MH_CreateHook(vtbl[27], (LPVOID)Hooked_CreateIndexBuffer, (LPVOID*)&Real_CreateIndexBuffer) == MH_OK) {
        MH_EnableHook(vtbl[27]);
    }
    if (MH_CreateHook(vtbl[19], (LPVOID)Hooked_Reset, (LPVOID*)&Real_Reset) == MH_OK) {
        MH_EnableHook(vtbl[19]);
    }
    if (MH_CreateHook(vtbl[20], (LPVOID)Hooked_Present, (LPVOID*)&Real_Present) == MH_OK) {
        MH_EnableHook(vtbl[20]);
    }

    g_DeviceHooksInstalled = true;
    OutputDebugStringA("[Gothic3Optima] Installed IDirect3DDevice9 State Filter and Vertex/Index Buffer scaling hooks.\n");
}

static IDirect3DDevice9* s_pActiveDevice = nullptr;

void EvictD3D9Resources() {
    InvalidateStateCaches();
    if (s_pActiveDevice) {
        s_pActiveDevice->EvictManagedResources();
    }
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
    if (pPresentationParameters && pPresentationParameters->EnableAutoDepthStencil) {
        // Phase 17.1: Upgrade any low-precision depth buffer (e.g. D16) to 24/32-bit depth
        // With DXVK active, d3d9.useD32forD24 = True seamlessly backs D24S8 with VK_FORMAT_D32_SFLOAT
        if (pPresentationParameters->AutoDepthStencilFormat == D3DFMT_D16) {
            pPresentationParameters->AutoDepthStencilFormat = D3DFMT_D24S8;
        }
    }

    // Phase 20: Ensure multithreaded device access for background thread pool resource generation
    BehaviorFlags |= D3DCREATE_MULTITHREADED;

    HRESULT hr = Real_CreateDevice(This, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppReturnedDeviceInterface);
    if (SUCCEEDED(hr) && ppReturnedDeviceInterface && *ppReturnedDeviceInterface) {
        s_pActiveDevice = *ppReturnedDeviceInterface;
        InstallDeviceHooks(*ppReturnedDeviceInterface);
        OutputDebugStringA("[Gothic3Optima] D3D9 Device initialized with high-precision Z-buffer support.\n");
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hooked_GetDeviceCaps(
    IDirect3D9* This,
    UINT Adapter,
    D3DDEVTYPE DeviceType,
    D3DCAPS9* pCaps
) {
    HRESULT hr = Real_GetDeviceCaps(This, Adapter, DeviceType, pCaps);
    if (SUCCEEDED(hr) && pCaps) {
        // Guarantee Shader Model 3.0 reporting to satisfy engine requirements
        if (pCaps->PixelShaderVersion < D3DPS_VERSION(3, 0)) {
            pCaps->PixelShaderVersion = D3DPS_VERSION(3, 0);
        }
        if (pCaps->VertexShaderVersion < D3DVS_VERSION(3, 0)) {
            pCaps->VertexShaderVersion = D3DVS_VERSION(3, 0);
        }
        // Phase 17.1: Report Slope-Scale Depth Bias & Depth Bias capabilities
        pCaps->RasterCaps |= D3DPRASTERCAPS_DEPTHBIAS | D3DPRASTERCAPS_SLOPESCALEDEPTHBIAS;

        // Phase 18.1: Expand vertex, primitive and index capacity for high-poly mods
        pCaps->MaxVertexIndex = 0x00FFFFFF;
        pCaps->MaxPrimitiveCount = 0x00FFFFFF;
        pCaps->MaxStreams = 16;
        pCaps->MaxStreamStride = 512;
    }
    return hr;
}

static void HookD3D9Interface(IDirect3D9* pD3D) {
    if (g_CreateDeviceHooked || !pD3D) return;

    void** vtbl = *(void***)pD3D;
    // Index 17: GetDeviceCaps
    if (MH_CreateHook(vtbl[17], (LPVOID)Hooked_GetDeviceCaps, (LPVOID*)&Real_GetDeviceCaps) == MH_OK) {
        MH_EnableHook(vtbl[17]);
        OutputDebugStringA("[Gothic3Optima] Hooked IDirect3D9::GetDeviceCaps.\n");
    }
    // Index 19: CreateDevice
    if (MH_CreateHook(vtbl[19], (LPVOID)Hooked_CreateDevice, (LPVOID*)&Real_CreateDevice) == MH_OK) {
        MH_EnableHook(vtbl[19]);
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

    // D3DXFilterTexture hook disabled - textures filtered normally
    /*
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
    */

    // Bypass restrictive Engine.dll Shader Model 3.0 assertions for DXVK / remote / hybrid setups
    HMODULE hEngine = GetModuleHandleA("Engine.dll");
    if (hEngine) {
        uintptr_t base = (uintptr_t)hEngine;
        BYTE* pPS = (BYTE*)(base + 0x0000c693);
        BYTE* pVS = (BYTE*)(base + 0x0000c6b8);
        BYTE* pMRT = (BYTE*)(base + 0x00012a20);
        BYTE* pRTT = (BYTE*)(base + 0x00012a7c);
        DWORD oldProtect = 0;
        if (pPS[0] == 0x73 && pPS[1] == 0x19) {
            VirtualProtect(pPS, 2, PAGE_EXECUTE_READWRITE, &oldProtect);
            pPS[0] = 0xEB; // force jump over Pixel Shader 3.0 assertion
            VirtualProtect(pPS, 2, oldProtect, &oldProtect);
        }
        if (pVS[0] == 0x73 && pVS[1] == 0x19) {
            VirtualProtect(pVS, 2, PAGE_EXECUTE_READWRITE, &oldProtect);
            pVS[0] = 0xEB; // force jump over Vertex Shader 3.0 assertion
            VirtualProtect(pVS, 2, oldProtect, &oldProtect);
        }
        if (pMRT[0] == 0x77 && pMRT[1] == 0x15) {
            VirtualProtect(pMRT, 2, PAGE_EXECUTE_READWRITE, &oldProtect);
            pMRT[0] = 0xEB; // force jump over Multiple Render Target assertion
            VirtualProtect(pMRT, 2, oldProtect, &oldProtect);
        }
        if (pRTT[0] == 0x75 && pRTT[1] == 0x15) {
            VirtualProtect(pRTT, 2, PAGE_EXECUTE_READWRITE, &oldProtect);
            pRTT[0] = 0xEB; // force jump over Render To Texture assertion
            VirtualProtect(pRTT, 2, oldProtect, &oldProtect);
        }
        OutputDebugStringA("[Gothic3Optima] Engine.dll GPU capability assertions bypassed.\n");

        // Phase 18.1: Expand geometry instance pool growth increment in Engine.dll
        // 0x101dbc6c: cmp eax, 0x400 -> cmp eax, 0x1000 (4096 instances per batch chunk)
        // 0x101dbc7f: mov eax, 0x400 -> mov eax, 0x1000
        // 0x101dbc78: mov eax, 8 -> mov eax, 256 (initial pool reservation)
        BYTE* pPoolCmp = (BYTE*)(base + 0x001dbc6c);
        BYTE* pPoolMov = (BYTE*)(base + 0x001dbc7f);
        BYTE* pPoolInit = (BYTE*)(base + 0x001dbc78);
        if (pPoolCmp[0] == 0x3D && *(DWORD*)(pPoolCmp + 1) == 0x00000400) {
            DWORD oldProt = 0;
            VirtualProtect(pPoolCmp, 5, PAGE_EXECUTE_READWRITE, &oldProt);
            *(DWORD*)(pPoolCmp + 1) = 0x00001000;
            VirtualProtect(pPoolCmp, 5, oldProt, &oldProt);
        }
        if (pPoolMov[0] == 0xB8 && *(DWORD*)(pPoolMov + 1) == 0x00000400) {
            DWORD oldProt = 0;
            VirtualProtect(pPoolMov, 5, PAGE_EXECUTE_READWRITE, &oldProt);
            *(DWORD*)(pPoolMov + 1) = 0x00001000;
            VirtualProtect(pPoolMov, 5, oldProt, &oldProt);
        }
        if (pPoolInit[0] == 0xB8 && *(DWORD*)(pPoolInit + 1) == 0x00000008) {
            DWORD oldProt = 0;
            VirtualProtect(pPoolInit, 5, PAGE_EXECUTE_READWRITE, &oldProt);
            *(DWORD*)(pPoolInit + 1) = 0x00000100;
            VirtualProtect(pPoolInit, 5, oldProt, &oldProt);
        }
        OutputDebugStringA("[Gothic3Optima] Engine.dll geometry instance pool growth expanded to 4096.\n");

        // Phase 18.1: Hook eCVertexProcessorIndex::CreateBuffer for automatic 32-bit index promotion
        void* pCreateBuffer = (void*)(base + 0x0020c980);
        s_pIndex32CheckJump = (BYTE*)(base + 0x0020c9ea);
        if (MH_CreateHook(pCreateBuffer, (LPVOID)Hooked_eCVertexProcessorIndex_CreateBuffer, (LPVOID*)&Real_eCVertexProcessorIndex_CreateBuffer) == MH_OK) {
            MH_EnableHook(pCreateBuffer);
            OutputDebugStringA("[Gothic3Optima] Hooked eCVertexProcessorIndex::CreateBuffer for 32-bit index auto-promotion.\n");
        }
    }
}
