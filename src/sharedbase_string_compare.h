#pragma once

#include <cstring>
#include <string.h>
#include <cstdint>

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

} // namespace G3O::SharedBase
