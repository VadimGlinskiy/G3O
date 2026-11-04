#pragma once
#include <windows.h>

namespace G3O {

void InitializeCPUTopology();
DWORD_PTR GetPCoreAffinityMask();

} // namespace G3O
