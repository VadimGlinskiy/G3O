#include "lfh_enforcer.h"
#include <windows.h>
#include <minhook.h>
#include <vector>

typedef HANDLE (WINAPI* PFN_HeapCreate)(DWORD flOptions, SIZE_T dwInitialSize, SIZE_T dwMaximumSize);
static PFN_HeapCreate g_origHeapCreate = nullptr;

static void EnableLFH(HANDLE hHeap) {
    if (!hHeap) return;
    ULONG heapInfo = 2; // HeapCompatibilityInformation = 2 (LFH)
    HeapSetInformation(hHeap, HeapCompatibilityInformation, &heapInfo, sizeof(heapInfo));
}

static HANDLE WINAPI Hook_HeapCreate(DWORD flOptions, SIZE_T dwInitialSize, SIZE_T dwMaximumSize) {
    HANDLE hHeap = g_origHeapCreate(flOptions, dwInitialSize, dwMaximumSize);
    if (hHeap) {
        EnableLFH(hHeap);
    }
    return hHeap;
}

namespace LFHEnforcer {

    bool Initialize() {
        // 1. Enable LFH on the default process heap
        EnableLFH(GetProcessHeap());

        // 2. Hook HeapCreate so any newly created heap in Engine.dll / CRT automatically gets LFH
        HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
        if (hKernel32) {
            FARPROC pHeapCreate = GetProcAddress(hKernel32, "HeapCreate");
            if (pHeapCreate) {
                MH_CreateHook(pHeapCreate, reinterpret_cast<LPVOID>(&Hook_HeapCreate), reinterpret_cast<LPVOID*>(&g_origHeapCreate));
                MH_EnableHook(pHeapCreate);
            }
        }

        OutputDebugStringA("[Gothic3Optima] Low Fragmentation Heap (LFH) enforced on all heaps!\n");
        return true;
    }

    void Shutdown() {
        // Handled by MinHook
    }

} // namespace LFHEnforcer
