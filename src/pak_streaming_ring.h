#pragma once

#include <windows.h>
#include <cstdint>
#include <cstddef>

namespace G3O {

// Initialize the streaming ring buffer arena and background worker pool.
// Uses a strictly bounded 64 MB virtual arena (32 slots x 2 MB).
// workerCount: 0 = auto-detect based on CPU topology, >0 = explicit worker thread count.
bool InitializePakStreamingRing(uint32_t workerCount = 0);

// Shutdown the background worker pool and release the virtual arena.
void ShutdownPakStreamingRing();

// Called from Hooked_ReadFile when reading a PAK archive block.
// fileOffset: absolute byte offset in the archive
// compressedBytes: number of bytes read
void PakStreamingRing_OnBlockRead(HANDLE hFile, uint64_t fileOffset, uint32_t compressedBytes);

// Called from Hooked_ZlibDecompress on the main thread.
// contentKey: 64-bit hardware SSE4.2 CRC32C fingerprint
// If matching pre-decompressed block is ready: copies to outBuffer in ~0.04 ms,
// atomically marks the slot SLOT_EMPTY immediately, and returns true.
// Zero inter-request caching, zero disk caching.
bool PakStreamingRing_TryConsume(uint64_t contentKey, void* outBuffer, uint32_t outCapacity, DWORD* pOutWritten);

// Clear any pending in-flight requests and reset slots (e.g. at end of load).
void PakStreamingRing_ResetLoadState();

// Telemetry
LONG64 PakStreamingRing_GetHits();
LONG64 PakStreamingRing_GetMisses();
LONG64 PakStreamingRing_GetProducedBlocks();
LONG64 PakStreamingRing_GetConsumedBytes();

} // namespace G3O
