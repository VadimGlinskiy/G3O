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

// Initializes the arena from an explicit archive path (used by focused tests
// or custom archive verification without requiring global Config mutation).
bool InitializeLayerRingArenaFromArchive(const char* archivePath, uint32_t waitTimeoutMs = 10);

void LayerRingArena_SetWorkerPausedForTest(bool paused);

// Called by the gCSector::OnActivate hook to start or stop the RB_Mod pipeline.
void LayerRingArena_OnSectorStart(const char* sectorName);
void LayerRingArena_OnSectorEnd();

// Called by the FileSystem.dll zlib hook; returns false for non-matching or unready blocks.
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
