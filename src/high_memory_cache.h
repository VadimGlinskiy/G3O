#pragma once
#include <windows.h>
#include <cstdint>
#include <cstddef>

namespace HighMemoryCache {

    // Initialize the High Memory Shadow Cache (> 4 GB) using Heaven's Gate x86->x64 bridge
    bool Initialize();
    void Shutdown();

    // Check if the 64-bit High Memory Cache is available and active
    bool IsActive();

    // Register and load an archive into 64-bit High Memory (> 4 GB)
    bool RegisterArchive(const char* filePath, HANDLE hFile, uint64_t fileSize);

    // Unregister archive handle when closed
    void UnregisterArchive(HANDLE hFile);

    // Check if handle is cached in High Memory
    bool IsArchiveCached(HANDLE hFile);

    // Read data directly from 64-bit High Memory into 32-bit engine buffer
    bool ReadArchive(HANDLE hFile, uint64_t fileOffset, void* dstBuffer, DWORD bytesToRead, DWORD* bytesRead);

    // Query cache metrics (bytes used, capacity, cache hits, bytes read)
    void GetStatistics(uint64_t* pUsedBytes, uint64_t* pCapacityBytes, DWORD* pHits, uint64_t* pBytesRead);

} // namespace HighMemoryCache
