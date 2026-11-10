#pragma once
#include <cstdint>
#include <cstring>

namespace G3O {

// ----------------------------------------------------------------------------
// ShadowIndexMap: Open-Addressing Hash Table in .bss (Zero Heap Allocation)
// Maps (pLight, pCaster) -> uint32_t index in eCShadowMapDirectionalLightBase's
// internal array [pLight + 0x68].
// 65536 slots * 16 bytes = 1 MB static memory.
// ----------------------------------------------------------------------------
class ShadowIndexMap {
public:
    static constexpr uint32_t kCapacity   = 65536;
    static constexpr uint32_t kMask       = kCapacity - 1;
    static constexpr uintptr_t kEmpty     = 0;
    static constexpr uintptr_t kTombstone = ~static_cast<uintptr_t>(0);

    struct alignas(16) Entry {
        uintptr_t light;
        uintptr_t caster;
        uint32_t  arrayIndex;
        uint32_t  pad;
    };

    static inline uint32_t HashPair(uintptr_t light, uintptr_t caster) noexcept {
        uint32_t h = static_cast<uint32_t>(light) ^ (static_cast<uint32_t>(caster) * 0x9E3779B9u);
        h ^= h >> 16;
        h *= 0x85ebca6b;
        h ^= h >> 13;
        return h & kMask;
    }

    static inline bool FindIndex(const Entry* table, void* pLight, void* pCaster, uint32_t& outIdx) noexcept {
        if (!table || !pLight || !pCaster) return false;
        const uintptr_t l = reinterpret_cast<uintptr_t>(pLight);
        const uintptr_t c = reinterpret_cast<uintptr_t>(pCaster);
        uint32_t slot = HashPair(l, c);

        for (uint32_t probe = 0; probe < 128; ++probe) {
            const auto& e = table[slot];
            if (e.caster == c && e.light == l) {
                outIdx = e.arrayIndex;
                return true;
            }
            if (e.caster == kEmpty) {
                return false;
            }
            slot = (slot + 1) & kMask;
        }
        return false;
    }

    static inline void Upsert(Entry* table, void* pLight, void* pCaster, uint32_t arrayIdx) noexcept {
        if (!table || !pLight || !pCaster) return;
        const uintptr_t l = reinterpret_cast<uintptr_t>(pLight);
        const uintptr_t c = reinterpret_cast<uintptr_t>(pCaster);
        uint32_t slot = HashPair(l, c);
        uint32_t firstTombstone = 0xFFFFFFFFu;

        for (uint32_t probe = 0; probe < 128; ++probe) {
            auto& e = table[slot];
            if (e.caster == c && e.light == l) {
                e.arrayIndex = arrayIdx;
                return;
            }
            if (e.caster == kEmpty) {
                const uint32_t target = (firstTombstone != 0xFFFFFFFFu) ? firstTombstone : slot;
                table[target].light      = l;
                table[target].caster     = c;
                table[target].arrayIndex = arrayIdx;
                table[target].pad        = 0;
                return;
            }
            if (e.caster == kTombstone && firstTombstone == 0xFFFFFFFFu) {
                firstTombstone = slot;
            }
            slot = (slot + 1) & kMask;
        }

        // If probe limit reached and a tombstone was encountered, reuse it
        if (firstTombstone != 0xFFFFFFFFu) {
            table[firstTombstone].light      = l;
            table[firstTombstone].caster     = c;
            table[firstTombstone].arrayIndex = arrayIdx;
            table[firstTombstone].pad        = 0;
        }
    }

    static inline void Erase(Entry* table, void* pLight, void* pCaster) noexcept {
        if (!table || !pLight || !pCaster) return;
        const uintptr_t l = reinterpret_cast<uintptr_t>(pLight);
        const uintptr_t c = reinterpret_cast<uintptr_t>(pCaster);
        uint32_t slot = HashPair(l, c);

        for (uint32_t probe = 0; probe < 128; ++probe) {
            auto& e = table[slot];
            if (e.caster == c && e.light == l) {
                e.light      = kEmpty;
                e.caster     = kTombstone;
                e.arrayIndex = 0;
                e.pad        = 0;
                return;
            }
            if (e.caster == kEmpty) {
                return;
            }
            slot = (slot + 1) & kMask;
        }
    }

    static inline void Clear(Entry* table) noexcept {
        if (table) {
            std::memset(table, 0, sizeof(Entry) * kCapacity);
        }
    }
};

// Layout of bTValArray<void*> inside eCShadowMapDirectionalLightBase:
//   +0x68: void**   m_pArray
//   +0x6C: uint32_t m_u32Count
//   +0x70: uint32_t m_u32Capacity
static constexpr uintptr_t OFFSET_CASTER_ARRAY_PTR = 0x68;
static constexpr uintptr_t OFFSET_CASTER_COUNT     = 0x6C;
static constexpr uintptr_t OFFSET_CASTER_CAPACITY  = 0x70;

} // namespace G3O
