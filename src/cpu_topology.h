#pragma once
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

void InitializeCPUTopology();
DWORD_PTR GetPCoreAffinityMask();

#ifdef __cplusplus
}
#endif
