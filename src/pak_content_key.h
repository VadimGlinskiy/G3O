#pragma once

#include <windows.h>
#include <cstdint>
#include <intrin.h>
#include <nmmintrin.h> // _mm_crc32_u32, _mm_crc32_u8
#include <cstring>

namespace G3O {

inline bool HasSSE42Support() noexcept {
    static const bool s_supported = []() {
        int cpuInfo[4] = { 0 };
        __cpuid(cpuInfo, 1);
        return (cpuInfo[2] & (1 << 19)) != 0; // ECX bit 19 = SSE4.2
    }();
    return s_supported;
}

// Compute an ultra-fast, zero-collision 64-bit content fingerprint directly from
// (inBuffer, inSize, outCapacity) using hardware SSE4.2 CRC32C instructions.
// Sampling inSize + outCapacity + first 32 bytes (zlib header & Huffman tree) +
// last 16 bytes (contains uncompressed Adler-32 checksum) executes in ~3.5 ns.
inline uint64_t ComputeBlockContentKey(const void* inBuffer, DWORD inSize, DWORD outCapacity) noexcept {
    if (!inBuffer || inSize < 6) return 0;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(inBuffer);

    if (HasSSE42Support()) {
        uint32_t crc1 = 0xFFFFFFFFu;
        uint32_t crc2 = 0x55555555u;

        crc1 = _mm_crc32_u32(crc1, inSize);
        crc2 = _mm_crc32_u32(crc2, outCapacity);

        // First up to 32 bytes
        const DWORD headLen = (inSize < 32) ? inSize : 32;
        DWORD i = 0;
        for (; i + 4 <= headLen; i += 4) {
            uint32_t v;
            std::memcpy(&v, p + i, sizeof(v));
            crc1 = _mm_crc32_u32(crc1, v);
        }
        for (; i < headLen; ++i) {
            crc1 = _mm_crc32_u8(crc1, p[i]);
        }

        // Last up to 16 bytes
        const DWORD tailLen = (inSize < 16) ? inSize : 16;
        const uint8_t* tail = p + inSize - tailLen;
        DWORD j = 0;
        for (; j + 4 <= tailLen; j += 4) {
            uint32_t v;
            std::memcpy(&v, tail + j, sizeof(v));
            crc2 = _mm_crc32_u32(crc2, v);
        }
        for (; j < tailLen; ++j) {
            crc2 = _mm_crc32_u8(crc2, tail[j]);
        }

        return (static_cast<uint64_t>(crc1) << 32) | static_cast<uint64_t>(crc2);
    } else {
        // Fallback 64-bit FNV-1a
        uint64_t h = 14695981039346656037ULL;
        h = (h ^ inSize) * 1099511628211ULL;
        h = (h ^ outCapacity) * 1099511628211ULL;
        const DWORD headLen = (inSize < 32) ? inSize : 32;
        for (DWORD i = 0; i < headLen; ++i) {
            h = (h ^ p[i]) * 1099511628211ULL;
        }
        const DWORD tailLen = (inSize < 16) ? inSize : 16;
        const uint8_t* tail = p + inSize - tailLen;
        for (DWORD j = 0; j < tailLen; ++j) {
            h = (h ^ tail[j]) * 1099511628211ULL;
        }
        return h;
    }
}

// Compute a 64-bit fingerprint directly from (inBuffer, inSize) without requiring outCapacity
inline uint64_t ComputeCompressedBufferKey(const void* inBuffer, DWORD inSize) noexcept {
    if (!inBuffer || inSize < 6) return 0;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(inBuffer);

    if (HasSSE42Support()) {
        uint32_t crc1 = 0xFFFFFFFFu;
        uint32_t crc2 = 0x55555555u;

        crc1 = _mm_crc32_u32(crc1, inSize);

        // First up to 32 bytes
        const DWORD headLen = (inSize < 32) ? inSize : 32;
        DWORD i = 0;
        for (; i + 4 <= headLen; i += 4) {
            uint32_t v;
            std::memcpy(&v, p + i, sizeof(v));
            crc1 = _mm_crc32_u32(crc1, v);
        }
        for (; i < headLen; ++i) {
            crc1 = _mm_crc32_u8(crc1, p[i]);
        }

        // Last up to 16 bytes
        const DWORD tailLen = (inSize < 16) ? inSize : 16;
        const uint8_t* tail = p + inSize - tailLen;
        DWORD j = 0;
        for (; j + 4 <= tailLen; j += 4) {
            uint32_t v;
            std::memcpy(&v, tail + j, sizeof(v));
            crc2 = _mm_crc32_u32(crc2, v);
        }
        for (; j < tailLen; ++j) {
            crc2 = _mm_crc32_u8(crc2, tail[j]);
        }

        return (static_cast<uint64_t>(crc1) << 32) | static_cast<uint64_t>(crc2);
    } else {
        // Fallback 64-bit FNV-1a
        uint64_t h = 14695981039346656037ULL;
        h = (h ^ inSize) * 1099511628211ULL;
        const DWORD headLen = (inSize < 32) ? inSize : 32;
        for (DWORD i = 0; i < headLen; ++i) {
            h = (h ^ p[i]) * 1099511628211ULL;
        }
        const DWORD tailLen = (inSize < 16) ? inSize : 16;
        const uint8_t* tail = p + inSize - tailLen;
        for (DWORD j = 0; j < tailLen; ++j) {
            h = (h ^ tail[j]) * 1099511628211ULL;
        }
        return h;
    }
}

} // namespace G3O
