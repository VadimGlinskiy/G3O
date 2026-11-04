#pragma once
#include <windows.h>

namespace G3O {

// Initializes O(1) hash acceleration for FileSystem.dll!GetDirCacheEntry
bool InitializeVFSCacheOptimizer();
void ShutdownVFSCacheOptimizer();
void ClearVFSCache();

// Telemetry counters
LONG GetVFSCacheHits();
LONG GetVFSCacheMisses();
void ResetVFSCacheTelemetry();

} // namespace G3O
