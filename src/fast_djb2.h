#pragma once

#include <cstdint>
#include <cstddef>

namespace G3O {

// Exact equivalence to Gothic 3 SharedBase.dll djb2 string hashing:
// hash = hash * 33 + (signed char)c
// With 4-byte chunk unrolling:
// ((h*33 + c0)*33 + c1)*33 + c2)*33 + c3 = h * 1185921 + (c0*35937 + c1*1089 + c2*33 + c3)
inline uint32_t FastDJB2_Length(const char* pData, int len) noexcept {
    if (!pData || len <= 0) return 0;
    uint32_t h = 0;
    int i = 0;
    while (i + 4 <= len) {
        const int8_t* p = reinterpret_cast<const int8_t*>(pData + i);
        int32_t term = static_cast<int32_t>(p[0]) * 35937 +
                       static_cast<int32_t>(p[1]) * 1089 +
                       static_cast<int32_t>(p[2]) * 33 +
                       static_cast<int32_t>(p[3]);
        h = h * 1185921u + static_cast<uint32_t>(term);
        i += 4;
    }
    while (i < len) {
        int8_t c = static_cast<int8_t>(pData[i]);
        h = h * 33u + static_cast<uint32_t>(static_cast<int32_t>(c));
        i++;
    }
    return h;
}

inline uint32_t FastDJB2_NullTerminated(const char* str) noexcept {
    if (!str) return 0;
    uint32_t h = 0;
    while (*str) {
        int8_t c = static_cast<int8_t>(*str++);
        h = h * 33u + static_cast<uint32_t>(static_cast<int32_t>(c));
    }
    return h;
}

} // namespace G3O
