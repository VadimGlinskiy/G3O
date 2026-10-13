#include "memory_allocator.h"
#include <windows.h>
#include <mimalloc.h>
#include <minhook.h>
#include <iostream>

// -----------------------------------------------------------------------------
// Function pointer typedefs
// -----------------------------------------------------------------------------
typedef void* (__thiscall* PFN_bCMemoryAdmin_Malloc)(void* pThis, unsigned long size);
typedef void  (__thiscall* PFN_bCMemoryAdmin_Free)(void* pThis, void* ptr);
typedef void* (__thiscall* PFN_bCMemoryAdmin_Realloc)(void* pThis, void* ptr, unsigned long size);

typedef void* (__stdcall* PFN_MemHeap_Alloc)(unsigned int size);
typedef void  (__stdcall* PFN_MemHeap_Free)(void* ptr);
typedef void* (__stdcall* PFN_MemHeap_ReAlloc)(void* ptr, unsigned int size);

typedef SIZE_T (WINAPI* PFN_HeapSize)(HANDLE hHeap, DWORD dwFlags, LPCVOID lpMem);
typedef BOOL   (WINAPI* PFN_HeapFree)(HANDLE hHeap, DWORD dwFlags, LPVOID lpMem);
typedef LPVOID (WINAPI* PFN_HeapReAlloc)(HANDLE hHeap, DWORD dwFlags, LPVOID lpMem, SIZE_T dwBytes);

// -----------------------------------------------------------------------------
// Trampolines
// -----------------------------------------------------------------------------
static PFN_bCMemoryAdmin_Malloc  g_origMalloc = nullptr;
static PFN_bCMemoryAdmin_Free    g_origFree = nullptr;
static PFN_bCMemoryAdmin_Realloc g_origRealloc = nullptr;
static PFN_bCMemoryAdmin_Malloc  g_origNewObject = nullptr;
static PFN_bCMemoryAdmin_Free    g_origDeleteObject = nullptr;

static PFN_MemHeap_Alloc   g_origAlloc = nullptr;
static PFN_MemHeap_Free    g_origFreeHeap = nullptr;
static PFN_MemHeap_ReAlloc g_origReAllocHeap = nullptr;

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

// -----------------------------------------------------------------------------
// Kernel32 Safe Heap Hooks
// Prevents RtlSizeHeap / HeapFree crashes across all engine DLLs
// -----------------------------------------------------------------------------
static SIZE_T WINAPI Hook_HeapSize(HANDLE hHeap, DWORD dwFlags, LPCVOID lpMem) {
    if (!lpMem) return (SIZE_T)-1;
    if (mi_is_in_heap_region(lpMem)) {
        return mi_usable_size(lpMem);
    }
    __try {
        if (g_origHeapSize) {
            return g_origHeapSize(hHeap, dwFlags, lpMem);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return (SIZE_T)-1;
    }
    return (SIZE_T)-1;
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
        return FALSE;
    }
    return FALSE;
}

static LPVOID WINAPI Hook_HeapReAlloc(HANDLE hHeap, DWORD dwFlags, LPVOID lpMem, SIZE_T dwBytes) {
    if (!lpMem) {
        return mi_malloc(dwBytes);
    }
    if (mi_is_in_heap_region(lpMem)) {
        return mi_realloc(lpMem, dwBytes);
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
// bCMemoryAdmin Hooks
// -----------------------------------------------------------------------------
static void* __fastcall Hook_bCMemoryAdmin_Malloc(void* pThis, void* /*edx*/, unsigned long size) {
    void* ptr = mi_malloc(size);
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
        return mi_malloc(size);
    }
    if (mi_is_in_heap_region(ptr)) {
        return mi_realloc(ptr, size);
    }
    if (g_origRealloc) {
        return g_origRealloc(pThis, ptr, size);
    }
    return nullptr;
}

static void* __fastcall Hook_bCMemoryAdmin_NewObject(void* pThis, void* /*edx*/, unsigned long size) {
    void* ptr = mi_malloc(size);
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
    void* ptr = mi_malloc(size);
    if (!ptr && g_origAlloc) return g_origAlloc(size);
    return ptr;
}

static void __stdcall Hook_MemHeap_Free(void* ptr) {
    if (!ptr) return;
    if (mi_is_in_heap_region(ptr)) {
        mi_free(ptr);
    } else if (g_origFreeHeap) {
        g_origFreeHeap(ptr);
    }
}

static void* __stdcall Hook_MemHeap_ReAlloc(void* ptr, unsigned int size) {
    if (!ptr) return mi_malloc(size);
    if (mi_is_in_heap_region(ptr)) {
        return mi_realloc(ptr, size);
    }
    if (g_origReAllocHeap) return g_origReAllocHeap(ptr, size);
    return nullptr;
}

static void* __stdcall Hook_MemHeap_AllocUnsynced(unsigned int size) {
    void* ptr = mi_malloc(size);
    if (!ptr && g_origAllocUnsynced) return g_origAllocUnsynced(size);
    return ptr;
}

static void __stdcall Hook_MemHeap_FreeUnsynced(void* ptr) {
    if (!ptr) return;
    if (mi_is_in_heap_region(ptr)) {
        mi_free(ptr);
    } else if (g_origFreeUnsynced) {
        g_origFreeUnsynced(ptr);
    }
}

static void* __stdcall Hook_MemHeap_ReAllocUnsynced(void* ptr, unsigned int size) {
    if (!ptr) return mi_malloc(size);
    if (mi_is_in_heap_region(ptr)) {
        return mi_realloc(ptr, size);
    }
    if (g_origReAllocUnsynced) return g_origReAllocUnsynced(ptr, size);
    return nullptr;
}

static void* __stdcall Hook_MemHeap_AllocShortUsage(unsigned int size) {
    void* ptr = mi_malloc(size);
    if (!ptr && g_origAllocShortUsage) return g_origAllocShortUsage(size);
    return ptr;
}

static void __stdcall Hook_MemHeap_FreeShortUsage(void* ptr) {
    if (!ptr) return;
    if (mi_is_in_heap_region(ptr)) {
        mi_free(ptr);
    } else if (g_origFreeShortUsage) {
        g_origFreeShortUsage(ptr);
    }
}

static void* __stdcall Hook_MemHeap_AllocShortUsageUnsynced(unsigned int size) {
    void* ptr = mi_malloc(size);
    if (!ptr && g_origAllocShortUsageUnsynced) return g_origAllocShortUsageUnsynced(size);
    return ptr;
}

static void __stdcall Hook_MemHeap_FreeShortUsageUnsynced(void* ptr) {
    if (!ptr) return;
    if (mi_is_in_heap_region(ptr)) {
        mi_free(ptr);
    } else if (g_origFreeShortUsageUnsynced) {
        g_origFreeShortUsageUnsynced(ptr);
    }
}

// -----------------------------------------------------------------------------
// Initialization
// -----------------------------------------------------------------------------
namespace MemoryOptimizer {

    bool Initialize() {
        // 1. Hook Kernel32 Heap functions first for system-wide safety
        HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
        if (hKernel32) {
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

        OutputDebugStringA("[Gothic3Optima] Complete mimalloc integration: Kernel32, bCMemoryAdmin, and MemHeap hooked successfully!\n");
        return true;
    }

    void Shutdown() {
        // MinHook global disable will handle disabling individual hooks
    }

} // namespace MemoryOptimizer
