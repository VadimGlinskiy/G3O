#pragma once

#include <cstring>
#include <string.h>
#include <cstdint>

#ifdef CompareString
#undef CompareString
#endif

namespace G3O::SharedBase {

// Gothic 3 bCString stores its 32-bit character length at offset -8 from the string pointer.
// A null bCString instance has m_pString == nullptr.
inline bool FastStringCompare(const void* pThis, const void* pOther) noexcept {
    if (pThis == pOther) return true;
    if (!pThis || !pOther) return false;

    const char* s1 = *reinterpret_cast<const char* const*>(pThis);
    const char* s2 = *reinterpret_cast<const char* const*>(pOther);
    if (s1 == s2) return true;
    if (!s1 || !s2) return false;

    const int32_t len1 = *reinterpret_cast<const int32_t*>(s1 - 8);
    const int32_t len2 = *reinterpret_cast<const int32_t*>(s2 - 8);
    if (len1 != len2) return false;
    if (len1 <= 0) return true;

    // Vectorized memory comparison for matching lengths (replaces engine's 1-byte loop)
    return std::memcmp(s1, s2, static_cast<size_t>(len1)) == 0;
}

inline bool FastStringCompareNoCase(const void* pThis, const void* pOther) noexcept {
    if (pThis == pOther) return true;
    if (!pThis || !pOther) return false;

    const char* s1 = *reinterpret_cast<const char* const*>(pThis);
    const char* s2 = *reinterpret_cast<const char* const*>(pOther);
    if (s1 == s2) return true;
    if (!s1 || !s2) return false;

    const int32_t len1 = *reinterpret_cast<const int32_t*>(s1 - 8);
    const int32_t len2 = *reinterpret_cast<const int32_t*>(s2 - 8);
    if (len1 != len2) return false;
    if (len1 <= 0) return true;

    // Length-bounded case-insensitive comparison (avoids unbounded _stricmp null scans)
    return _memicmp(s1, s2, static_cast<size_t>(len1)) == 0;
}

// Legacy raw-pointer comparison helpers
inline bool CompareString(const char* left, const char* right) noexcept {
    if (left == right) return true;
    if (!left || !right) return false;
    return std::strcmp(left, right) == 0;
}

inline bool CompareStringNoCase(const char* left, const char* right) noexcept {
    if (left == right) return true;
    if (!left || !right) return false;
    return _stricmp(left, right) == 0;
}

// Unrolled fast DJB2 hash matching SharedBase.dll!?g_GetHashValue@VbCString@@@@YGKABVbCString@@@Z (RVA 0x55F40)
// Native computes: hash = hash * 33 + (int32_t)(signed char)*p
// We process 4 bytes at a time using unrolled arithmetic:
// h4 = h0 * 33^4 + c0 * 33^3 + c1 * 33^2 + c2 * 33 + c3
// 33^1 = 33, 33^2 = 1089, 33^3 = 35937, 33^4 = 1185921
inline uint32_t FastDjb2Hash(const void* pThis) noexcept {
    if (!pThis) return 0;
    const char* str = *reinterpret_cast<const char* const*>(pThis);
    if (!str) return 0;

    const int32_t len = *reinterpret_cast<const int32_t*>(str - 8);
    if (len <= 0) return 0;

    uint32_t hash = 0;
    const char* ptr = str;
    int32_t remaining = len;

    while (remaining >= 4) {
        uint32_t word;
        std::memcpy(&word, ptr, sizeof(uint32_t));
        // Check for null terminator within these 4 bytes
        if (((word - 0x01010101u) & ~word & 0x80808080u) != 0) {
            break;
        }

        const int32_t c0 = static_cast<signed char>(ptr[0]);
        const int32_t c1 = static_cast<signed char>(ptr[1]);
        const int32_t c2 = static_cast<signed char>(ptr[2]);
        const int32_t c3 = static_cast<signed char>(ptr[3]);

        hash = hash * 1185921u + static_cast<uint32_t>(c0 * 35937 + c1 * 1089 + c2 * 33 + c3);
        ptr += 4;
        remaining -= 4;
    }

    while (remaining > 0 && *ptr != '\0') {
        hash = hash * 33u + static_cast<uint32_t>(static_cast<int32_t>(static_cast<signed char>(*ptr++)));
        --remaining;
    }

    return hash;
}

} // namespace G3O::SharedBase
