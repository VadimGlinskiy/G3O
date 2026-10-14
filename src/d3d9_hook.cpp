#include "d3d9_hook.h"
#include <MinHook.h>
#include <stdio.h>
#include <shlobj.h>

// Statistics (kept for interface compatibility)
void GetD3D9FilterStatistics(DWORD* pFilteredRenderStates, DWORD* pFilteredStageStates, DWORD* pFilteredSamplerStates) {
    if (pFilteredRenderStates) *pFilteredRenderStates = 0;
    if (pFilteredStageStates) *pFilteredStageStates = 0;
    if (pFilteredSamplerStates) *pFilteredSamplerStates = 0;
}

void EvictD3D9Resources() {
    // No-op: Modern GPUs have abundant VRAM (4GB-16GB+).
}

// Direct3D9 hooks (IDirect3D9::CreateDevice vtable hooking removed to ensure 100%
// compatibility with Steam Overlay (gameoverlayrenderer.dll), DXVK, and GPU drivers).
void InstallD3D9Hooks() {
    // Bypass restrictive Engine.dll Shader Model 3.0 assertions for hybrid/virtual environments
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
    }
}
