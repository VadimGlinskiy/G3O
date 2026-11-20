#pragma once
#include <cstdint>

namespace G3O {

inline uint32_t FastGetPropertySetType(void* pPropertySet) {
    if (!pPropertySet) return 0xFFFFFFFF;
    uintptr_t vptr = *reinterpret_cast<const uintptr_t*>(pPropertySet);

    // Call vtable[24] (offset 0x60 in x86): uint32_t __thiscall GetType()
    using GetType_t = uint32_t(__thiscall*)(void*);
    auto vtable = reinterpret_cast<const uintptr_t*>(vptr);
    auto pfnGetType = reinterpret_cast<GetType_t>(vtable[24]);
    return pfnGetType(pPropertySet);
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
