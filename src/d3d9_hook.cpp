#include "d3d9_hook.h"
#include "diagnostic_logger.h"
#include "config_manager.h"
#include "profiler.h"
#include "blackbox_recorder.h"
#include "hook_install_policy.h"
#include <minhook.h>
#include <d3d9.h>
#include <stdio.h>

namespace G3O {

static bool s_presentHooked = false;
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
            if (Filter == D3DX_DEFAULT || (Filter & 0xF) == D3DX_FILTER_TRIANGLE) {
                Filter = D3DX_FILTER_POINT;
            }
        } else {
            // Mip generation needed: replace heavy CPU triangle filtering with fast 2x2 box filtering
            if (MipFilter == D3DX_DEFAULT || (MipFilter & 0xF) == D3DX_FILTER_TRIANGLE) {
                MipFilter = D3DX_FILTER_BOX;
            }
            if (Filter == D3DX_DEFAULT || (Filter & 0xF) == D3DX_FILTER_TRIANGLE) {
                Filter = D3DX_FILTER_BOX;
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
        if (Filter == D3DX_DEFAULT || (Filter & 0xF) == D3DX_FILTER_TRIANGLE) {
            Filter = D3DX_FILTER_BOX;
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

void AttachD3D9DeviceIfReady(void* rawDevice) {
    IDirect3DDevice9* pDevice = reinterpret_cast<IDirect3DDevice9*>(rawDevice);
    if (!pDevice) return;
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
    AttachD3D9DeviceIfReady(pDevice);
    return result;
}

void InitializeD3D9Optimizer() {
    if (GetConfig().enableProfiler) {
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
                        Log("G3O", "Hooked Engine.dll!eCDX9Device constructor for profiler device attachment.");
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
