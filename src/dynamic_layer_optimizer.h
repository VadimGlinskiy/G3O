#pragma once

#include <windows.h>
#include <cstdint>

namespace G3O {

typedef void (__thiscall* WorldOnActivate_t)(void* pWorld);

void InitializeDynamicLayerOptimizer();
void ShutdownDynamicLayerOptimizer();

// Lifecycle triggers for savegame/world loading
void DynamicLayerOptimizer_OnLoadStart();
void DynamicLayerOptimizer_OnLoadEnd();

// Decompression cache lookup called inside Hooked_ZlibDecompress
bool DynamicLayerOptimizer_TryGet(
    const void* inBuffer,
    DWORD inSize,
    void* outBuffer,
    DWORD outCapacity,
    DWORD* pOutWritten
);

// Safe wrapper for gCWorld::OnActivate (executes sequentially on main thread, records timings)
void DynamicLayerOptimizer_ProcessWorldActivate(void* pWorld, WorldOnActivate_t originalWorldOnActivate);

struct DynamicLayerStats {
    uint32_t hits = 0;
    uint32_t misses = 0;
    uint64_t bytesServed = 0;
};
DynamicLayerStats DynamicLayerOptimizer_GetStats();

} // namespace G3O
