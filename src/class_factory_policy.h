#pragma once
#include <cstdint>
#include <cstring>

namespace G3O {

// ----------------------------------------------------------------------------
// SClassNodeCacheEntry: 32-byte cache slot (2 per 64-byte L1 cache line)
// Caches bTValMap<bCString, T>::FindNode results in O(1)
// ----------------------------------------------------------------------------
struct alignas(32) SClassNodeCacheEntry {
    uint32_t    u32Hash;        // Fast O(1) hash (0 = empty slot)
    uint32_t    u32Length;      // String length
    void*       pMap;           // Map pointer (ensures isolation across multiple maps)
    void*       pNode;          // Found bTMapNode*
    const char* pszKey;         // Saved string copy
    uint32_t    u32Bucket;      // Bucket index
    uint64_t    u64Prefix;      // First 8 bytes of string
};
static_assert(sizeof(SClassNodeCacheEntry) == 32, "SClassNodeCacheEntry must be exactly 32 bytes");

static constexpr uint32_t kClassCacheCapacity = 1024; // Power of two
static constexpr uint32_t kClassCacheMask     = kClassCacheCapacity - 1;

// Computes fast hash without byte-by-byte looping on strings >= 4 chars
__forceinline uint32_t ComputeFastClassHash(const char* pText, uint32_t u32Len) noexcept {
    if (!pText || u32Len == 0) return 0;
    uint32_t h = u32Len ^ 0x811C9DC5u;
    if (u32Len >= 4) {
        uint32_t uFirst = 0, uMid = 0, uLast = 0;
        std::memcpy(&uFirst, pText, 4);
        std::memcpy(&uMid,   pText + (u32Len >> 1) - 2, 4);
        std::memcpy(&uLast,  pText + u32Len - 4, 4);

        h = (h ^ uFirst) * 0x01000193u;
        h = (h ^ uMid)   * 0x01000193u;
        h = (h ^ uLast)  * 0x01000193u;
    } else {
        for (uint32_t i = 0; i < u32Len; ++i) {
            h = (h ^ static_cast<uint8_t>(pText[i])) * 0x01000193u;
        }
    }
    h ^= (h >> 16);
    return (h == 0) ? 1u : h;
}

__forceinline uint64_t ReadPrefix8(const char* pText, uint32_t u32Len) noexcept {
    uint64_t u64 = 0;
    if (!pText || u32Len == 0) return 0;
    if (u32Len >= 8) {
        std::memcpy(&u64, pText, 8);
    } else {
        std::memcpy(&u64, pText, u32Len);
    }
    return u64;
}

__forceinline const SClassNodeCacheEntry* FindInNodeCache(
    const SClassNodeCacheEntry* table, void* pMap, const char* pText, uint32_t u32Len, uint32_t u32Hash, uint64_t u64Prefix) noexcept {
    if (!table || !pMap || !pText || u32Len == 0 || u32Hash == 0) return nullptr;
    uint32_t idx = (u32Hash ^ (reinterpret_cast<uintptr_t>(pMap) >> 4)) & kClassCacheMask;
    for (uint32_t probe = 0; probe < 32; ++probe) {
        const SClassNodeCacheEntry& entry = table[idx];
        if (entry.u32Hash == 0) return nullptr; // Empty slot -> not found

        if (entry.pMap == pMap &&
            entry.u32Hash == u32Hash &&
            entry.u32Length == u32Len &&
            entry.u64Prefix == u64Prefix) {
            if (u32Len <= 8 || std::memcmp(entry.pszKey + 8, pText + 8, u32Len - 8) == 0) {
                return &entry;
            }
        }
        idx = (idx + 1) & kClassCacheMask;
    }
    return nullptr;
}

static inline const SClassNodeCacheEntry* InsertIntoNodeCache(
    SClassNodeCacheEntry* table, char* stringArena, uint32_t& arenaOffset, uint32_t arenaCapacity,
    void* pMap, const char* pText, uint32_t u32Len, uint32_t u32Hash, uint64_t u64Prefix,
    void* pNode, uint32_t u32Bucket) noexcept {
    if (!table || !pMap || !pText || u32Len == 0 || u32Hash == 0 || !pNode) return nullptr;

    uint32_t idx = (u32Hash ^ (reinterpret_cast<uintptr_t>(pMap) >> 4)) & kClassCacheMask;
    uint32_t targetIdx = UINT32_MAX;

    for (uint32_t probe = 0; probe < 32; ++probe) {
        if (table[idx].u32Hash == 0) {
            if (targetIdx == UINT32_MAX) targetIdx = idx;
            break;
        }
        if (table[idx].pMap == pMap &&
            table[idx].u32Hash == u32Hash &&
            table[idx].u32Length == u32Len &&
            table[idx].u64Prefix == u64Prefix) {
            if (u32Len <= 8 || std::memcmp(table[idx].pszKey + 8, pText + 8, u32Len - 8) == 0) {
                return &table[idx]; // Already present
            }
        }
        idx = (idx + 1) & kClassCacheMask;
    }

    if (targetIdx == UINT32_MAX) return nullptr;

    char* pszSaved = nullptr;
    if (stringArena && (arenaOffset + u32Len + 1 <= arenaCapacity)) {
        pszSaved = &stringArena[arenaOffset];
        std::memcpy(pszSaved, pText, u32Len);
        pszSaved[u32Len] = '\0';
        arenaOffset += (u32Len + 1);
    }

    SClassNodeCacheEntry& entry = table[targetIdx];
    entry.u32Hash   = u32Hash;
    entry.u32Length = u32Len;
    entry.pMap      = pMap;
    entry.pNode     = pNode;
    entry.pszKey    = pszSaved ? pszSaved : pText;
    entry.u32Bucket = u32Bucket;
    entry.u64Prefix = u64Prefix;
    return &entry;
}

} // namespace G3O
