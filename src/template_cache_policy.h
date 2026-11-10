#pragma once

#include <cstdint>
#include <cstring>
#include <atomic>
#include <unordered_map>
#include <emmintrin.h> // SSE2
#include <nmmintrin.h> // SSE4.2 (_mm_crc32_u32)

namespace G3O {

struct PropertyId128 {
    uint32_t d[4];
    bool operator==(const PropertyId128& o) const noexcept {
        return d[0] == o.d[0] && d[1] == o.d[1] && d[2] == o.d[2] && d[3] == o.d[3];
    }
};

struct PropertyId128Hash {
    std::size_t operator()(const PropertyId128& id) const noexcept {
        uint32_t hash = 2166136261u;
        for (int i = 0; i < 4; ++i) {
            hash ^= id.d[i];
            hash *= 16777619u;
        }
        return static_cast<std::size_t>(hash);
    }
};

class TemplateEntityCache {
public:
    void* Lookup(const void* pPropertyId) const noexcept {
        if (!pPropertyId) return nullptr;
        PropertyId128 key;
        std::memcpy(&key, pPropertyId, sizeof(key));
        auto it = m_map.find(key);
        if (it != m_map.end()) {
            return it->second;
        }
        return nullptr;
    }

    void Insert(const void* pPropertyId, void* pEntity) {
        if (!pPropertyId || !pEntity) return;
        PropertyId128 key;
        std::memcpy(&key, pPropertyId, sizeof(key));
        m_map[key] = pEntity;
    }

    std::size_t Size() const noexcept { return m_map.size(); }
    void Clear() { m_map.clear(); }

private:
    std::unordered_map<PropertyId128, void*, PropertyId128Hash> m_map;
};

// ============================================================================
// FastTemplateCache: Direct-Mapped L1/L2 Cache with Negative Caching
//
// Solves the bottleneck where eCSceneAdmin::m_templateMap only has 43 buckets (0x2B),
// causing ~116-node collision walks for 56,669 dynamic entity queries that return nullptr.
//
// Positive entries (valid eCTemplateEntity*) are cached indefinitely.
// Negative entries (kNotFound) are tracked with an epoch counter, allowing O(1)
// negative cache invalidation across loading phases without evicting positive hits.
// ============================================================================
class FastTemplateCache {
public:
    static constexpr uint32_t kSlotCount = 32768;
    static constexpr uint32_t kSlotMask  = kSlotCount - 1;

    static constexpr uintptr_t kEmptySlot = 0;
    static constexpr uintptr_t kNotFound  = static_cast<uintptr_t>(~0ULL); // 0xFFFFFFFF in 32-bit

    struct alignas(32) CacheSlot {
        uint32_t  guid[4];       // 16 bytes GUID
        uintptr_t entityOrState; // kEmptySlot, kNotFound, or valid eCTemplateEntity*
        uint32_t  negEpoch;      // Negative cache epoch
        uint32_t  reserved[2];   // Padding to 32 bytes
    };
    static_assert(sizeof(CacheSlot) == 32, "CacheSlot must be exactly 32 bytes");

    static void InvalidateNegativeCache() noexcept {
        s_currentNegEpoch.fetch_add(1, std::memory_order_relaxed);
    }

    static void ClearAll() noexcept {
        std::memset(s_slots, 0, sizeof(s_slots));
        s_currentNegEpoch.fetch_add(1, std::memory_order_relaxed);
    }

    __forceinline static uint32_t ComputeSlotIndex(const uint32_t* pGuid) noexcept {
        uint32_t crc = _mm_crc32_u32(0x811C9DC5u, pGuid[0]);
        crc = _mm_crc32_u32(crc, pGuid[1]);
        crc = _mm_crc32_u32(crc, pGuid[2]);
        crc = _mm_crc32_u32(crc, pGuid[3]);
        return crc & kSlotMask;
    }

    __forceinline static bool GuidEquals(const uint32_t* a, const uint32_t* b) noexcept {
        const __m128i va  = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a));
        const __m128i vb  = _mm_load_si128(reinterpret_cast<const __m128i*>(b));
        const __m128i cmp = _mm_cmpeq_epi32(va, vb);
        return _mm_movemask_epi8(cmp) == 0xFFFF;
    }

    static inline CacheSlot             s_slots[kSlotCount]{};
    static inline std::atomic<uint32_t> s_currentNegEpoch{1};
};

} // namespace G3O
