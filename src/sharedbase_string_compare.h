#pragma once

#include <cstring>
#include <string.h>

namespace G3O::SharedBase {

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
