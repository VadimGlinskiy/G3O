#pragma once
#include <windows.h>
#include <cstdint>

namespace G3O {

struct LayerRingArenaStats {
    uint32_t hits = 0;
    uint32_t misses = 0;
    uint64_t bytesServed = 0;
    uint64_t waitTimeUs = 0;
};

void InitializeLayerRingArena();
void ShutdownLayerRingArena();

// Called from Hooked_SectorOnActivate in savegame_profiler.cpp
void LayerRingArena_OnSectorStart(const char* sectorName);
void LayerRingArena_OnSectorEnd();

// Called from Hooked_ZlibDecompress in fast_pak_decompressor.cpp
bool LayerRingArena_TryGet(
    const void* inBuffer,
    DWORD inSize,
    void* outBuffer,
    DWORD outCapacity,
    DWORD* pOutWritten
);

bool LayerRingArena_IsActive();
LayerRingArenaStats LayerRingArena_GetStats();

} // namespace G3O
