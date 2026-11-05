#pragma once
#include <windows.h>
#include <cstdint>

namespace G3O {

void PakTracer_OnLoadStart(const char* saveName);
void PakTracer_OnLoadEnd();

void PakTracer_RecordRead(
    HANDLE hFile,
    const void* buffer,
    DWORD bytesRequested,
    DWORD bytesRead,
    LPOVERLAPPED ov
);

void PakTracer_RecordDecompress(
    const void* inBuffer,
    DWORD inSize,
    DWORD outCapacity,
    DWORD actualOut,
    uint64_t decompTimeUs,
    const void* outBuffer
);

bool PakTracer_TryGetPredecompressed(
    const void* inBuffer,
    DWORD inSize,
    void* outBuffer,
    DWORD outCapacity,
    DWORD* pActualOut
);

bool PakTracer_IsActive();

} // namespace G3O
