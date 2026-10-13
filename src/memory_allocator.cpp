#include "memory_allocator.h"
#include <windows.h>
#include <mimalloc.h>
#include <minhook.h>
#include <iostream>

// -----------------------------------------------------------------------------
// Isolated mimalloc Heaps
// Separate lifetimes prevent external memory fragmentation in x86 32-bit space:
// - g_pEngineHeap: Persistent world objects, bCMemoryAdmin, _new@8, CRT HeapAlloc
// - g_pMemHeap: MemHeap general structures, dynamic arrays, buffers
// - g_pShortUsageHeap: Frame-scoped scratch allocations (AllocShortUsage)
// -----------------------------------------------------------------------------
static mi_heap_t* g_pEngineHeap = nullptr;
static mi_heap_t* g_pMemHeap = nullptr;
static mi_heap_t* g_pShortUsageHeap = nullptr;

// -----------------------------------------------------------------------------
// Function pointer typedefs
// -----------------------------------------------------------------------------
typedef void* (__thiscall* PFN_bCMemoryAdmin_Malloc)(void* pThis, unsigned long size);
typedef void  (__thiscall* PFN_bCMemoryAdmin_Free)(void* pThis, void* ptr);
typedef void* (__thiscall* PFN_bCMemoryAdmin_Realloc)(void* pThis, void* ptr, unsigned long size);

typedef void* (__stdcall* PFN_SharedBase_New)(unsigned int size, unsigned int flags);

typedef void* (__stdcall* PFN_MemHeap_Alloc)(unsigned int size);
typedef void  (__stdcall* PFN_MemHeap_Free)(void* ptr);
typedef void* (__stdcall* PFN_MemHeap_ReAlloc)(void* ptr, unsigned int size);
typedef void  (__stdcall* PFN_MemHeap_GetUsage)(void* pUsage);

typedef SIZE_T (WINAPI* PFN_HeapSize)(HANDLE hHeap, DWORD dwFlags, LPCVOID lpMem);
typedef BOOL   (WINAPI* PFN_HeapFree)(HANDLE hHeap, DWORD dwFlags, LPVOID lpMem);
typedef LPVOID (WINAPI* PFN_HeapReAlloc)(HANDLE hHeap, DWORD dwFlags, LPVOID lpMem, SIZE_T dwBytes);
typedef LPVOID (WINAPI* PFN_HeapAlloc)(HANDLE hHeap, DWORD dwFlags, SIZE_T dwBytes);

// -----------------------------------------------------------------------------
// Trampolines
// -----------------------------------------------------------------------------
static PFN_bCMemoryAdmin_Malloc  g_origMalloc = nullptr;
static PFN_bCMemoryAdmin_Free    g_origFree = nullptr;
static PFN_bCMemoryAdmin_Realloc g_origRealloc = nullptr;
static PFN_bCMemoryAdmin_Malloc  g_origNewObject = nullptr;
static PFN_bCMemoryAdmin_Free    g_origDeleteObject = nullptr;

static PFN_SharedBase_New g_origNew = nullptr;

static PFN_MemHeap_Alloc    g_origAlloc = nullptr;
static PFN_MemHeap_Free     g_origFreeHeap = nullptr;
static PFN_MemHeap_ReAlloc  g_origReAllocHeap = nullptr;
static PFN_MemHeap_GetUsage g_origGetUsage = nullptr;

static PFN_MemHeap_Alloc   g_origAllocUnsynced = nullptr;
static PFN_MemHeap_Free    g_origFreeUnsynced = nullptr;
static PFN_MemHeap_ReAlloc g_origReAllocUnsynced = nullptr;

static PFN_MemHeap_Alloc g_origAllocShortUsage = nullptr;
static PFN_MemHeap_Free  g_origFreeShortUsage = nullptr;
static PFN_MemHeap_Alloc g_origAllocShortUsageUnsynced = nullptr;
static PFN_MemHeap_Free  g_origFreeShortUsageUnsynced = nullptr;

static PFN_HeapSize    g_origHeapSize = nullptr;
static PFN_HeapFree    g_origHeapFree = nullptr;
static PFN_HeapReAlloc g_origHeapReAlloc = nullptr;
static PFN_HeapAlloc   g_origHeapAlloc = nullptr;

// -----------------------------------------------------------------------------
// Kernel32 Safe Heap Hooks
// Prevents RtlSizeHeap / HeapFree crashes across all engine DLLs
// -----------------------------------------------------------------------------
static SIZE_T WINAPI Hook_HeapSize(HANDLE hHeap, DWORD dwFlags, LPCVOID lpMem) {
    if (!lpMem) return 0;
    if (mi_is_in_heap_region(lpMem)) {
        return mi_usable_size(lpMem);
    }
    __try {
        if (g_origHeapSize) {
            return g_origHeapSize(hHeap, dwFlags, lpMem);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return 0;
}

static BOOL WINAPI Hook_HeapFree(HANDLE hHeap, DWORD dwFlags, LPVOID lpMem) {
    if (!lpMem) return TRUE;
    if (mi_is_in_heap_region(lpMem)) {
        mi_free(lpMem);
        return TRUE;
    }
    __try {
        if (g_origHeapFree) {
            return g_origHeapFree(hHeap, dwFlags, lpMem);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return TRUE;
    }
    return TRUE;
}

static LPVOID WINAPI Hook_HeapReAlloc(HANDLE hHeap, DWORD dwFlags, LPVOID lpMem, SIZE_T dwBytes) {
    if (!lpMem) {
        return (dwFlags & HEAP_ZERO_MEMORY)
            ? (g_pEngineHeap ? mi_heap_zalloc(g_pEngineHeap, dwBytes) : mi_zalloc(dwBytes))
            : (g_pEngineHeap ? mi_heap_malloc(g_pEngineHeap, dwBytes) : mi_malloc(dwBytes));
    }
    if (mi_is_in_heap_region(lpMem)) {
        size_t oldSize = mi_usable_size(lpMem);
        void* newPtr = g_pEngineHeap ? mi_heap_realloc(g_pEngineHeap, lpMem, dwBytes) : mi_realloc(lpMem, dwBytes);
        if (newPtr && (dwFlags & HEAP_ZERO_MEMORY) && dwBytes > oldSize) {
            memset((BYTE*)newPtr + oldSize, 0, dwBytes - oldSize);
        }
        return newPtr;
    }
    __try {
        if (g_origHeapReAlloc) {
            return g_origHeapReAlloc(hHeap, dwFlags, lpMem, dwBytes);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    return nullptr;
}

// -----------------------------------------------------------------------------
// Direct C++ Operator New Hook (_new@8 in SharedBase.dll)
// Routes global new operator across all game modules directly to g_pEngineHeap
// -----------------------------------------------------------------------------
static void* __stdcall Hook_SharedBase_New(unsigned int size, unsigned int flags) {
    void* ptr = g_pEngineHeap ? mi_heap_malloc(g_pEngineHeap, size) : mi_malloc(size);
    if (!ptr && g_origNew) {
        return g_origNew(size, flags);
    }
    return ptr;
}

// -----------------------------------------------------------------------------
// bCMemoryAdmin Hooks (SharedBase.dll)
// -----------------------------------------------------------------------------
static void* __fastcall Hook_bCMemoryAdmin_Malloc(void* pThis, void* /*edx*/, unsigned long size) {
    void* ptr = g_pEngineHeap ? mi_heap_malloc(g_pEngineHeap, size) : mi_malloc(size);
    if (!ptr && g_origMalloc) {
        return g_origMalloc(pThis, size);
    }
    return ptr;
}

static void __fastcall Hook_bCMemoryAdmin_Free(void* pThis, void* /*edx*/, void* ptr) {
    if (!ptr) return;
    if (mi_is_in_heap_region(ptr)) {
        mi_free(ptr);
    } else if (g_origFree) {
        g_origFree(pThis, ptr);
    }
}

static void* __fastcall Hook_bCMemoryAdmin_Realloc(void* pThis, void* /*edx*/, void* ptr, unsigned long size) {
    if (!ptr) {
        return g_pEngineHeap ? mi_heap_malloc(g_pEngineHeap, size) : mi_malloc(size);
    }
    if (mi_is_in_heap_region(ptr)) {
        return g_pEngineHeap ? mi_heap_realloc(g_pEngineHeap, ptr, size) : mi_realloc(ptr, size);
    }
    if (g_origRealloc) {
        return g_origRealloc(pThis, ptr, size);
    }
    return nullptr;
}

static void* __fastcall Hook_bCMemoryAdmin_NewObject(void* pThis, void* /*edx*/, unsigned long size) {
    void* ptr = g_pEngineHeap ? mi_heap_malloc(g_pEngineHeap, size) : mi_malloc(size);
    if (!ptr && g_origNewObject) {
        return g_origNewObject(pThis, size);
    }
    return ptr;
}

static void __fastcall Hook_bCMemoryAdmin_DeleteObject(void* pThis, void* /*edx*/, void* ptr) {
    if (!ptr) return;
    if (mi_is_in_heap_region(ptr)) {
        mi_free(ptr);
    } else if (g_origDeleteObject) {
        g_origDeleteObject(pThis, ptr);
    }
}

// -----------------------------------------------------------------------------
// MemHeap Hooks (SharedBase.dll)
// -----------------------------------------------------------------------------
static void* __stdcall Hook_MemHeap_Alloc(unsigned int size) {
    void* ptr = g_pMemHeap ? mi_heap_malloc(g_pMemHeap, size) : mi_malloc(size);
    if (!ptr && g_origAlloc) return g_origAlloc(size);
    return ptr;
}

static void __stdcall Hook_MemHeap_Free(void* ptr) {
    if (!ptr) return;
    if (mi_is_in_heap_region(ptr)) {
        mi_free(ptr);
        return;
    }
    __try {
        if (g_origFreeHeap) {
            g_origFreeHeap(ptr);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        // Suppress any SEH violation on corrupt/unrecognized block
    }
}

static void* __stdcall Hook_MemHeap_ReAlloc(void* ptr, unsigned int size) {
    if (!ptr) return g_pMemHeap ? mi_heap_malloc(g_pMemHeap, size) : mi_malloc(size);
    if (mi_is_in_heap_region(ptr)) {
        return g_pMemHeap ? mi_heap_realloc(g_pMemHeap, ptr, size) : mi_realloc(ptr, size);
    }
    __try {
        if (g_origReAllocHeap) return g_origReAllocHeap(ptr, size);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return g_pMemHeap ? mi_heap_malloc(g_pMemHeap, size) : mi_malloc(size);
    }
    return nullptr;
}

static void* __stdcall Hook_MemHeap_AllocUnsynced(unsigned int size) {
    void* ptr = g_pMemHeap ? mi_heap_malloc(g_pMemHeap, size) : mi_malloc(size);
    if (!ptr && g_origAllocUnsynced) return g_origAllocUnsynced(size);
    return ptr;
}

static void __stdcall Hook_MemHeap_FreeUnsynced(void* ptr) {
    if (!ptr) return;
    if (mi_is_in_heap_region(ptr)) {
        mi_free(ptr);
        return;
    }
    __try {
        if (g_origFreeUnsynced) {
            g_origFreeUnsynced(ptr);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        // Suppress any SEH violation on corrupt/unrecognized block
    }
}

static void* __stdcall Hook_MemHeap_ReAllocUnsynced(void* ptr, unsigned int size) {
    if (!ptr) return g_pMemHeap ? mi_heap_malloc(g_pMemHeap, size) : mi_malloc(size);
    if (mi_is_in_heap_region(ptr)) {
        return g_pMemHeap ? mi_heap_realloc(g_pMemHeap, ptr, size) : mi_realloc(ptr, size);
    }
    __try {
        if (g_origReAllocUnsynced) return g_origReAllocUnsynced(ptr, size);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return g_pMemHeap ? mi_heap_malloc(g_pMemHeap, size) : mi_malloc(size);
    }
    return nullptr;
}

static void* __stdcall Hook_MemHeap_AllocShortUsage(unsigned int size) {
    void* ptr = g_pShortUsageHeap ? mi_heap_malloc(g_pShortUsageHeap, size) : mi_malloc(size);
    if (!ptr && g_origAllocShortUsage) return g_origAllocShortUsage(size);
    return ptr;
}

static void __stdcall Hook_MemHeap_FreeShortUsage(void* ptr) {
    if (!ptr) return;
    if (mi_is_in_heap_region(ptr)) {
        mi_free(ptr);
        return;
    }
    __try {
        if (g_origFreeShortUsage) {
            g_origFreeShortUsage(ptr);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        // Suppress any SEH violation
    }
}

static void* __stdcall Hook_MemHeap_AllocShortUsageUnsynced(unsigned int size) {
    void* ptr = g_pShortUsageHeap ? mi_heap_malloc(g_pShortUsageHeap, size) : mi_malloc(size);
    if (!ptr && g_origAllocShortUsageUnsynced) return g_origAllocShortUsageUnsynced(size);
    return ptr;
}

static void __stdcall Hook_MemHeap_FreeShortUsageUnsynced(void* ptr) {
    if (!ptr) return;
    if (mi_is_in_heap_region(ptr)) {
        mi_free(ptr);
        return;
    }
    __try {
        if (g_origFreeShortUsageUnsynced) {
            g_origFreeShortUsageUnsynced(ptr);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        // Suppress any SEH violation
    }
}

static void __stdcall Hook_MemHeap_GetUsage(void* pUsage) {
    if (!pUsage) return;
    DWORD* pWords = reinterpret_cast<DWORD*>(pUsage);
    size_t current_rss = 0, current_commit = 0;
    mi_process_info(nullptr, nullptr, nullptr, &current_rss, nullptr, &current_commit, nullptr, nullptr);

    // Bypass slow HeapCompact call and populate usage metrics directly
    pWords[0x30 / 4] = 0x20000000;              // largest free block: 512 MB
    pWords[0x34 / 4] = 100000;                  // active allocations count
    pWords[0x38 / 4] = 120000;                  // max count
    pWords[0x3c / 4] = static_cast<DWORD>(current_commit);   // active memory in bytes
    pWords[0x40 / 4] = static_cast<DWORD>(current_commit);   // max memory in bytes
    pWords[0x44 / 4] = 0;                       // areas
    pWords[0x48 / 4] = 0;                       // large blocks
    pWords[0x4c / 4] = 0;
    pWords[0x50 / 4] = 0;
    pWords[0x54 / 4] = 0;
    pWords[0x58 / 4] = 0x6000000;               // 100 MB max compact block
}

// -----------------------------------------------------------------------------
// Game Module IAT HeapAlloc Routing
// Directs CRT HeapAlloc in game DLLs straight into g_pEngineHeap
// -----------------------------------------------------------------------------
static LPVOID WINAPI Hook_Game_HeapAlloc(HANDLE hHeap, DWORD dwFlags, SIZE_T dwBytes) {
    void* ptr = (dwFlags & HEAP_ZERO_MEMORY)
        ? (g_pEngineHeap ? mi_heap_zalloc(g_pEngineHeap, dwBytes) : mi_zalloc(dwBytes))
        : (g_pEngineHeap ? mi_heap_malloc(g_pEngineHeap, dwBytes) : mi_malloc(dwBytes));
    if (!ptr && g_origHeapAlloc) {
        return g_origHeapAlloc(hHeap, dwFlags, dwBytes);
    }
    return ptr;
}

static bool PatchModuleIAT(HMODULE hModule, const char* targetDll, const char* targetFunc, void* newFunc, void** origFunc) {
    if (!hModule) return false;
    PIMAGE_DOS_HEADER dosHeader = reinterpret_cast<PIMAGE_DOS_HEADER>(hModule);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) return false;
    PIMAGE_NT_HEADERS ntHeaders = reinterpret_cast<PIMAGE_NT_HEADERS>(reinterpret_cast<BYTE*>(hModule) + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) return false;

    PIMAGE_DATA_DIRECTORY importDataDir = &ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (importDataDir->Size == 0 || importDataDir->VirtualAddress == 0) return false;

    PIMAGE_IMPORT_DESCRIPTOR importDesc = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(reinterpret_cast<BYTE*>(hModule) + importDataDir->VirtualAddress);
    bool patched = false;
    while (importDesc->Name) {
        const char* modName = reinterpret_cast<const char*>(reinterpret_cast<BYTE*>(hModule) + importDesc->Name);
        if (_stricmp(modName, targetDll) == 0) {
            PIMAGE_THUNK_DATA origFirstThunk = importDesc->OriginalFirstThunk 
                ? reinterpret_cast<PIMAGE_THUNK_DATA>(reinterpret_cast<BYTE*>(hModule) + importDesc->OriginalFirstThunk)
                : reinterpret_cast<PIMAGE_THUNK_DATA>(reinterpret_cast<BYTE*>(hModule) + importDesc->FirstThunk);
            PIMAGE_THUNK_DATA firstThunk = reinterpret_cast<PIMAGE_THUNK_DATA>(reinterpret_cast<BYTE*>(hModule) + importDesc->FirstThunk);

            while (origFirstThunk->u1.AddressOfData && firstThunk->u1.Function) {
                if (!(origFirstThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG)) {
                    PIMAGE_IMPORT_BY_NAME importByName = reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(reinterpret_cast<BYTE*>(hModule) + origFirstThunk->u1.AddressOfData);
                    if (strcmp(reinterpret_cast<const char*>(importByName->Name), targetFunc) == 0) {
                        DWORD oldProtect = 0;
                        if (VirtualProtect(&firstThunk->u1.Function, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
                            if (origFunc && !*origFunc) {
                                *origFunc = reinterpret_cast<void*>(firstThunk->u1.Function);
                            }
                            firstThunk->u1.Function = reinterpret_cast<DWORD>(newFunc);
                            VirtualProtect(&firstThunk->u1.Function, sizeof(void*), oldProtect, &oldProtect);
                            patched = true;
                            break;
                        }
                    }
                }
                origFirstThunk++;
                firstThunk++;
            }
        }
        importDesc++;
    }
    return patched;
}

static void PatchAllGameModules() {
    static const char* targetModules[] = {
        nullptr, // Gothic3.exe
        "Engine.dll",
        "SharedBase.dll",
        "Game.dll",
        "FileSystem.dll",
        "GUI.dll",
        "GFC.dll",
        "Importer.dll",
        "SCM.dll",
        "Script.dll",
        "Dialogs.dll"
    };

    for (const char* modName : targetModules) {
        HMODULE hMod = modName ? GetModuleHandleA(modName) : GetModuleHandleA(nullptr);
        if (hMod) {
            PatchModuleIAT(hMod, "kernel32.dll", "HeapAlloc", reinterpret_cast<void*>(&Hook_Game_HeapAlloc), reinterpret_cast<void**>(&g_origHeapAlloc));
        }
    }
}

// -----------------------------------------------------------------------------
// Initialization & Maintenance
// -----------------------------------------------------------------------------
namespace MemoryOptimizer {

    bool Initialize() {
        // 0. Allocate isolated mimalloc heaps for different object lifetimes
        g_pEngineHeap = mi_heap_new();
        g_pMemHeap = mi_heap_new();
        g_pShortUsageHeap = mi_heap_new();

        // 1. Hook Kernel32 Heap functions for engine-wide heap safety
        HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
        if (hKernel32) {
            g_origHeapAlloc = reinterpret_cast<PFN_HeapAlloc>(GetProcAddress(hKernel32, "HeapAlloc"));

            FARPROC pHeapSize = GetProcAddress(hKernel32, "HeapSize");
            if (pHeapSize) {
                MH_CreateHook(pHeapSize, reinterpret_cast<LPVOID>(&Hook_HeapSize), reinterpret_cast<LPVOID*>(&g_origHeapSize));
                MH_EnableHook(pHeapSize);
            }
            FARPROC pHeapFree = GetProcAddress(hKernel32, "HeapFree");
            if (pHeapFree) {
                MH_CreateHook(pHeapFree, reinterpret_cast<LPVOID>(&Hook_HeapFree), reinterpret_cast<LPVOID*>(&g_origHeapFree));
                MH_EnableHook(pHeapFree);
            }
            FARPROC pHeapReAlloc = GetProcAddress(hKernel32, "HeapReAlloc");
            if (pHeapReAlloc) {
                MH_CreateHook(pHeapReAlloc, reinterpret_cast<LPVOID>(&Hook_HeapReAlloc), reinterpret_cast<LPVOID*>(&g_origHeapReAlloc));
                MH_EnableHook(pHeapReAlloc);
            }
        }

        // 2. Hook SharedBase.dll
        HMODULE hSharedBase = GetModuleHandleA("SharedBase.dll");
        if (!hSharedBase) {
            hSharedBase = LoadLibraryA("SharedBase.dll");
            if (!hSharedBase) {
                OutputDebugStringA("[Gothic3Optima] SharedBase.dll not found!\n");
                return false;
            }
        }

        // Hook _new@8 (C++ operator new across all game modules)
        FARPROC pNew = GetProcAddress(hSharedBase, "_new@8");
        if (pNew) {
            MH_CreateHook(pNew, reinterpret_cast<LPVOID>(&Hook_SharedBase_New), reinterpret_cast<LPVOID*>(&g_origNew));
            MH_EnableHook(pNew);
        }

        // Hook bCMemoryAdmin methods
        FARPROC pMalloc = GetProcAddress(hSharedBase, "?Malloc@bCMemoryAdmin@@QAEPAXK@Z");
        FARPROC pFree = GetProcAddress(hSharedBase, "?Free@bCMemoryAdmin@@QAEXPAX@Z");
        FARPROC pRealloc = GetProcAddress(hSharedBase, "?Realloc@bCMemoryAdmin@@QAEPAXPAXK@Z");
        FARPROC pNewObj = GetProcAddress(hSharedBase, "?NewObject@bCMemoryAdmin@@QAEPAXK@Z");
        FARPROC pDelObj = GetProcAddress(hSharedBase, "?DeleteObject@bCMemoryAdmin@@QAEXPAX@Z");

        if (pMalloc) {
            MH_CreateHook(pMalloc, reinterpret_cast<LPVOID>(&Hook_bCMemoryAdmin_Malloc), reinterpret_cast<LPVOID*>(&g_origMalloc));
            MH_EnableHook(pMalloc);
        }
        if (pFree) {
            MH_CreateHook(pFree, reinterpret_cast<LPVOID>(&Hook_bCMemoryAdmin_Free), reinterpret_cast<LPVOID*>(&g_origFree));
            MH_EnableHook(pFree);
        }
        if (pRealloc) {
            MH_CreateHook(pRealloc, reinterpret_cast<LPVOID>(&Hook_bCMemoryAdmin_Realloc), reinterpret_cast<LPVOID*>(&g_origRealloc));
            MH_EnableHook(pRealloc);
        }
        if (pNewObj) {
            MH_CreateHook(pNewObj, reinterpret_cast<LPVOID>(&Hook_bCMemoryAdmin_NewObject), reinterpret_cast<LPVOID*>(&g_origNewObject));
            MH_EnableHook(pNewObj);
        }
        if (pDelObj && pDelObj != pFree) {
            MH_CreateHook(pDelObj, reinterpret_cast<LPVOID>(&Hook_bCMemoryAdmin_DeleteObject), reinterpret_cast<LPVOID*>(&g_origDeleteObject));
            MH_EnableHook(pDelObj);
        }

        // Hook MemHeap standard methods
        FARPROC pAlloc = GetProcAddress(hSharedBase, "?Alloc@MemHeap@@SGPAXI@Z");
        FARPROC pFreeHeap = GetProcAddress(hSharedBase, "?Free@MemHeap@@SGXPAX@Z");
        FARPROC pReAllocHeap = GetProcAddress(hSharedBase, "?ReAlloc@MemHeap@@SGPAXPAXI@Z");
        FARPROC pGetUsage = GetProcAddress(hSharedBase, "?GetUsage@MemHeap@@SGXPAUMemUsage@@@Z");

        if (pAlloc) {
            MH_CreateHook(pAlloc, reinterpret_cast<LPVOID>(&Hook_MemHeap_Alloc), reinterpret_cast<LPVOID*>(&g_origAlloc));
            MH_EnableHook(pAlloc);
        }
        if (pFreeHeap) {
            MH_CreateHook(pFreeHeap, reinterpret_cast<LPVOID>(&Hook_MemHeap_Free), reinterpret_cast<LPVOID*>(&g_origFreeHeap));
            MH_EnableHook(pFreeHeap);
        }
        if (pReAllocHeap) {
            MH_CreateHook(pReAllocHeap, reinterpret_cast<LPVOID>(&Hook_MemHeap_ReAlloc), reinterpret_cast<LPVOID*>(&g_origReAllocHeap));
            MH_EnableHook(pReAllocHeap);
        }
        if (pGetUsage) {
            MH_CreateHook(pGetUsage, reinterpret_cast<LPVOID>(&Hook_MemHeap_GetUsage), reinterpret_cast<LPVOID*>(&g_origGetUsage));
            MH_EnableHook(pGetUsage);
        }

        // Hook MemHeap Unsynced methods
        FARPROC pAllocUnsync = GetProcAddress(hSharedBase, "?AllocUnsynced@MemHeap@@SGPAXI@Z");
        FARPROC pFreeUnsync = GetProcAddress(hSharedBase, "?FreeUnsynced@MemHeap@@SGXPAX@Z");
        FARPROC pReAllocUnsync = GetProcAddress(hSharedBase, "?ReAllocUnsynced@MemHeap@@SGPAXPAXI@Z");

        if (pAllocUnsync) {
            MH_CreateHook(pAllocUnsync, reinterpret_cast<LPVOID>(&Hook_MemHeap_AllocUnsynced), reinterpret_cast<LPVOID*>(&g_origAllocUnsynced));
            MH_EnableHook(pAllocUnsync);
        }
        if (pFreeUnsync) {
            MH_CreateHook(pFreeUnsync, reinterpret_cast<LPVOID>(&Hook_MemHeap_FreeUnsynced), reinterpret_cast<LPVOID*>(&g_origFreeUnsynced));
            MH_EnableHook(pFreeUnsync);
        }
        if (pReAllocUnsync) {
            MH_CreateHook(pReAllocUnsync, reinterpret_cast<LPVOID>(&Hook_MemHeap_ReAllocUnsynced), reinterpret_cast<LPVOID*>(&g_origReAllocUnsynced));
            MH_EnableHook(pReAllocUnsync);
        }

        // Hook MemHeap ShortUsage methods
        FARPROC pAllocShort = GetProcAddress(hSharedBase, "?AllocShortUsage@MemHeap@@SGPAXI@Z");
        FARPROC pFreeShort = GetProcAddress(hSharedBase, "?FreeShortUsage@MemHeap@@SGXPAX@Z");
        FARPROC pAllocShortUnsync = GetProcAddress(hSharedBase, "?AllocShortUsageUnsynced@MemHeap@@SGPAXI@Z");
        FARPROC pFreeShortUnsync = GetProcAddress(hSharedBase, "?FreeShortUsageUnsynced@MemHeap@@SGXPAX@Z");

        if (pAllocShort) {
            MH_CreateHook(pAllocShort, reinterpret_cast<LPVOID>(&Hook_MemHeap_AllocShortUsage), reinterpret_cast<LPVOID*>(&g_origAllocShortUsage));
            MH_EnableHook(pAllocShort);
        }
        if (pFreeShort) {
            MH_CreateHook(pFreeShort, reinterpret_cast<LPVOID>(&Hook_MemHeap_FreeShortUsage), reinterpret_cast<LPVOID*>(&g_origFreeShortUsage));
            MH_EnableHook(pFreeShort);
        }
        if (pAllocShortUnsync) {
            MH_CreateHook(pAllocShortUnsync, reinterpret_cast<LPVOID>(&Hook_MemHeap_AllocShortUsageUnsynced), reinterpret_cast<LPVOID*>(&g_origAllocShortUsageUnsynced));
            MH_EnableHook(pAllocShortUnsync);
        }
        if (pFreeShortUnsync) {
            MH_CreateHook(pFreeShortUnsync, reinterpret_cast<LPVOID>(&Hook_MemHeap_FreeShortUsageUnsynced), reinterpret_cast<LPVOID*>(&g_origFreeShortUsageUnsynced));
            MH_EnableHook(pFreeShortUnsync);
        }

        // 3. Patch IAT for all loaded game modules
        PatchAllGameModules();

        OutputDebugStringA("[Gothic3Optima] Phase 13.1 mimalloc engine-wide integration: bCMemoryAdmin, _new@8, MemHeap, and Game IAT HeapAlloc routed to isolated mimalloc pools!\n");
        return true;
    }

    void Collect() {
        if (g_pEngineHeap) mi_heap_collect(g_pEngineHeap, true);
        if (g_pMemHeap) mi_heap_collect(g_pMemHeap, true);
        if (g_pShortUsageHeap) mi_heap_collect(g_pShortUsageHeap, true);
        mi_collect(true);
    }

    void Shutdown() {
        // Individual hooks are safely revoked by MinHook MH_DisableHook(MH_ALL_HOOKS)
    }

} // namespace MemoryOptimizer
