#pragma once
#include <windows.h>
#include <d3d9.h>

#ifdef __cplusplus
extern "C" {
#endif

void InstallD3D9Hooks();
void GetD3D9FilterStatistics(DWORD* pFilteredRenderStates, DWORD* pFilteredStageStates, DWORD* pFilteredSamplerStates);
void EvictD3D9Resources();

#ifdef __cplusplus
}
#endif
