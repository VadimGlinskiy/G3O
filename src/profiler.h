#pragma once
#include <windows.h>

namespace G3O {

// Lightweight passive profiler. Writes one line per second to G3O_profile.log:
// FPS, worst frame time, ReadFile volume/time, culling call count, CPU usage
// of the process and of its busiest thread. Does not alter game behaviour.
void InitializeProfiler();
void ShutdownProfiler();

extern volatile LONG g_cullCalls;

// Installs the frame-time hook on vtbl[17] (IDirect3DDevice9::Present).
void ProfilerHookPresent(void** deviceVtbl);

} // namespace G3O
