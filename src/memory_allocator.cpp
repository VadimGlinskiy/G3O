#include "memory_allocator.h"
#include <windows.h>
#include <mimalloc.h>
#include <minhook.h>
#include <iostream>

// Function pointer typedefs
typedef void* (__thiscall* PFN_bCMemoryAdmin_Malloc)(void* pThis, unsigned long size);
typedef void  (__thiscall* PFN_bCMemoryAdmin_Free)(void* pThis, void* ptr);
typedef void* (__thiscall* PFN_bCMemoryAdmin_Realloc)(void* pThis, void* ptr, unsigned long size);

typedef void* (__stdcall* PFN_MemHeap_AllocShortUsage)(unsigned int size);
typedef void  (__stdcall* PFN_MemHeap_FreeShortUsage)(void* ptr);

// Trampolines
static PFN_bCMemoryAdmin_Malloc  g_origMalloc = nullptr;
static PFN_bCMemoryAdmin_Free    g_origFree = nullptr;
static PFN_bCMemoryAdmin_Realloc g_origRealloc = nullptr;

static PFN_MemHeap_AllocShortUsage g_origAllocShortUsage = nullptr;
static PFN_MemHeap_FreeShortUsage  g_origFreeShortUsage = nullptr;
static PFN_MemHeap_AllocShortUsage g_origAllocShortUsageUnsynced = nullptr;
static PFN_MemHeap_FreeShortUsage  g_origFreeShortUsageUnsynced = nullptr;

// Fastcall wrapper for __thiscall in x86 MSVC:
// In __fastcall: ECX is 1st arg (this), EDX is 2nd arg (unused dummy), stack has remaining args
static void* __fastcall Hook_bCMemoryAdmin_Malloc(void* pThis, void* /*edx*/, unsigned long size) {
    void* ptr = mi_malloc(size);
    if (!ptr) {
        // Fallback to original allocator if mimalloc failed
        return g_origMalloc(pThis, size);
    }
    return ptr;
}

static void __fastcall Hook_bCMemoryAdmin_Free(void* pThis, void* /*edx*/, void* ptr) {
    if (!ptr) return;
    if (mi_is_in_heap_region(ptr)) {
        mi_free(ptr);
    } else {
        g_origFree(pThis, ptr);
    }
}

static void* __fastcall Hook_bCMemoryAdmin_Realloc(void* pThis, void* /*edx*/, void* ptr, unsigned long size) {
    if (!ptr) {
        return mi_malloc(size);
    }
    if (mi_is_in_heap_region(ptr)) {
        return mi_realloc(ptr, size);
    } else {
        // Pointer was allocated before hook: allocate with mimalloc, copy, and free original
        void* newPtr = mi_malloc(size);
        if (newPtr) {
            std::memcpy(newPtr, ptr, size);
            g_origFree(pThis, ptr);
            return newPtr;
        }
        return g_origRealloc(pThis, ptr, size);
    }
}

static void* __stdcall Hook_MemHeap_AllocShortUsage(unsigned int size) {
    void* ptr = mi_malloc(size);
    if (!ptr) return g_origAllocShortUsage(size);
    return ptr;
}

static void __stdcall Hook_MemHeap_FreeShortUsage(void* ptr) {
    if (!ptr) return;
    if (mi_is_in_heap_region(ptr)) {
        mi_free(ptr);
    } else {
        g_origFreeShortUsage(ptr);
    }
}

static void* __stdcall Hook_MemHeap_AllocShortUsageUnsynced(unsigned int size) {
    void* ptr = mi_malloc(size);
    if (!ptr) return g_origAllocShortUsageUnsynced(size);
    return ptr;
}

static void __stdcall Hook_MemHeap_FreeShortUsageUnsynced(void* ptr) {
    if (!ptr) return;
    if (mi_is_in_heap_region(ptr)) {
        mi_free(ptr);
    } else {
        g_origFreeShortUsageUnsynced(ptr);
    }
}

namespace MemoryOptimizer {

    bool Initialize() {
        HMODULE hSharedBase = GetModuleHandleA("SharedBase.dll");
        if (!hSharedBase) {
            // Might not be loaded yet, try loading or return false
            hSharedBase = LoadLibraryA("SharedBase.dll");
            if (!hSharedBase) {
                OutputDebugStringA("[Gothic3Optima] SharedBase.dll not found!\n");
                return false;
            }
        }

        // 1. Hook bCMemoryAdmin methods
        FARPROC pMalloc = GetProcAddress(hSharedBase, "?Malloc@bCMemoryAdmin@@QAEPAXK@Z");
        FARPROC pFree = GetProcAddress(hSharedBase, "?Free@bCMemoryAdmin@@QAEXPAX@Z");
        FARPROC pRealloc = GetProcAddress(hSharedBase, "?Realloc@bCMemoryAdmin@@QAEPAXPAXK@Z");

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

        // 2. Hook MemHeap methods
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

        OutputDebugStringA("[Gothic3Optima] mimalloc memory hooks installed on bCMemoryAdmin and MemHeap!\n");
        return true;
    }

    void Shutdown() {
        // MinHook global disable will handle disabling individual hooks
    }

} // namespace MemoryOptimizer
