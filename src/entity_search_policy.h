#pragma once
#include <cstdint>

namespace G3O {

struct VTableTypeCacheEntry {
    uintptr_t vptr;
    uint32_t  type;
};

static constexpr uint32_t VTABLE_CACHE_SIZE = 256;
static constexpr uint32_t VTABLE_CACHE_MASK = VTABLE_CACHE_SIZE - 1;

// Thread-local 256-slot direct-mapped L1 cache (2 KB, fits entirely in L1D cache)
alignas(64) static thread_local VTableTypeCacheEntry s_vtableTypeCache[VTABLE_CACHE_SIZE] = {};

inline uint32_t FastGetPropertySetType(void* pPropertySet) {
    if (!pPropertySet) return 0xFFFFFFFF;
    uintptr_t vptr = *reinterpret_cast<const uintptr_t*>(pPropertySet);
    uint32_t idx = ((vptr >> 4) ^ (vptr >> 12)) & VTABLE_CACHE_MASK;

    if (s_vtableTypeCache[idx].vptr == vptr) {
        return s_vtableTypeCache[idx].type;
    }

    // Call vtable[24] (offset 0x60 in x86): uint32_t __thiscall GetType()
    using GetType_t = uint32_t(__thiscall*)(void*);
    auto vtable = reinterpret_cast<const uintptr_t*>(vptr);
    auto pfnGetType = reinterpret_cast<GetType_t>(vtable[24]);
    uint32_t actualType = pfnGetType(pPropertySet);

    s_vtableTypeCache[idx].vptr = vptr;
    s_vtableTypeCache[idx].type = actualType;
    return actualType;
}

inline void ResetVTableTypeCache() {
    for (uint32_t i = 0; i < VTABLE_CACHE_SIZE; ++i) {
        s_vtableTypeCache[i].vptr = 0;
        s_vtableTypeCache[i].type = 0;
    }
}

// Pure search algorithm operating on sorted array of PropertySets
inline uint16_t FastSearchPropertySet(void* const* pElements, uint16_t count, uint32_t a_Type) {
    if (!pElements || count == 0) return 0xFFFF;

    // Linear scan with Early Exit for small arrays (<= 8 components, >95% of entities)
    if (count <= 8) {
        for (uint16_t i = 0; i < count; ++i) {
            uint32_t t = FastGetPropertySetType(pElements[i]);
            if (t == a_Type) return i;
            if (t > a_Type) return 0xFFFF; // Array is sorted ascending
        }
        return 0xFFFF;
    }

    // Binary search for complex entities (NPCs with > 8 components)
    int32_t low = 0;
    int32_t high = static_cast<int32_t>(count) - 1;
    while (low <= high) {
        int32_t mid = (low + high) >> 1;
        uint32_t midType = FastGetPropertySetType(pElements[mid]);
        if (midType == a_Type) return static_cast<uint16_t>(mid);
        if (midType < a_Type) {
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }
    return 0xFFFF;
}

} // namespace G3O
