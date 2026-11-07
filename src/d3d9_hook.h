#pragma once
#include <windows.h>

namespace G3O {

void InvalidateD3D9StateCaches();
void AttachD3D9DeviceIfReady(void* pDevice);
void InitializeD3D9Optimizer();
void ShutdownD3D9Optimizer();

} // namespace G3O
