#pragma once

#include <cstdint>
#include <cstring>

namespace G3O {

#pragma pack(push, 4)
struct bCPropertyID {
    uint32_t d[5]; // 20 bytes: 16 bytes UUID (m_Core) + 4 bytes suffix/count (m_u32Count)

    // SharedBase.dll!bCPropertyID::operator== compares the 16-byte UUID core (d[0]..d[3])
    __forceinline bool Equals(const bCPropertyID& other) const noexcept {
        return (d[0] == other.d[0]) &&
               (d[1] == other.d[1]) &&
               (d[2] == other.d[2]) &&
               (d[3] == other.d[3]);
    }

    __forceinline bool operator==(const bCPropertyID& other) const noexcept {
        return Equals(other);
    }

    __forceinline bool operator!=(const bCPropertyID& other) const noexcept {
        return !Equals(other);
    }
};
static_assert(sizeof(bCPropertyID) == 20, "bCPropertyID must be exactly 20 bytes");

// Node layout in Genome Engine's bTValMap<bCPropertyID, void*> (0x1C = 28 bytes)
struct SPropertyIDMapNode {
    bCPropertyID        m_Key;    // +0x00 .. +0x13 (20 bytes)
    void*               m_pValue; // +0x14 (4 bytes, e.g. eCTemplateEntity* or eCEntity*)
    SPropertyIDMapNode* m_pNext;  // +0x18 (4 bytes)
};
static_assert(sizeof(SPropertyIDMapNode) == 0x1C, "SPropertyIDMapNode must be 0x1C bytes");

// Header layout of bTValMap<bCPropertyID, void*>
struct bTValMap_PropertyID {
    SPropertyIDMapNode** m_ppBuckets;      // +0x00 (array of bucket head pointers)
    uint32_t             m_u32BucketCount; // +0x04 (number of buckets)
    uint32_t             m_u32Count;       // +0x08 (number of elements)
};
#pragma pack(pop)

// Native hash calculation for bCPropertyID in SharedBase.dll:
// (pKey->d[0] >> 3) + pKey->d[4]
__forceinline uint32_t ComputeNativePropertyIDHash(const bCPropertyID& key) noexcept {
    return (key.d[0] >> 3) + key.d[4];
}

// Telemetry counters
struct SGuidMapTelemetry {
    uint32_t u32FastWalkHits = 0;
    uint32_t u32Misses       = 0;
};

// Pure lookup function operating directly on live map buckets.
// Guarantees 100% memory safety (no caching of dangling pointers, safe across deletes).
__forceinline SPropertyIDMapNode* LookupPropertyIDNode(
    const bTValMap_PropertyID* pMap,
    const bCPropertyID*        pKey,
    uint32_t*                  pOutBucket,
    SGuidMapTelemetry*         pTelemetry = nullptr) noexcept
{
    if (!pMap || !pKey) {
        if (pOutBucket) *pOutBucket = 0;
        return nullptr;
    }

    const uint32_t u32BucketCount = pMap->m_u32BucketCount;
    if (u32BucketCount == 0 || pMap->m_ppBuckets == nullptr) {
        if (pOutBucket) *pOutBucket = 0;
        return nullptr;
    }

    const uint32_t u32NativeHash = ComputeNativePropertyIDHash(*pKey);
    const uint32_t u32BucketIdx  = ((u32BucketCount & (u32BucketCount - 1)) == 0)
        ? (u32NativeHash & (u32BucketCount - 1))
        : (u32NativeHash % u32BucketCount);

    if (pOutBucket) *pOutBucket = u32BucketIdx;

    const uint32_t k0 = pKey->d[0];
    const uint32_t k1 = pKey->d[1];
    const uint32_t k2 = pKey->d[2];
    const uint32_t k3 = pKey->d[3];

    SPropertyIDMapNode* pCurr = pMap->m_ppBuckets[u32BucketIdx];
    while (pCurr != nullptr) {
        if (pCurr->m_Key.d[0] == k0 &&
            pCurr->m_Key.d[1] == k1 &&
            pCurr->m_Key.d[2] == k2 &&
            pCurr->m_Key.d[3] == k3)
        {
            if (pTelemetry) ++pTelemetry->u32FastWalkHits;
            return pCurr;
        }
        pCurr = pCurr->m_pNext;
    }

    if (pTelemetry) ++pTelemetry->u32Misses;
    return nullptr;
}

} // namespace G3O
