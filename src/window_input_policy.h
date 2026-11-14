#pragma once

#include <cstring>

namespace G3O {

inline bool IsSplashWindowClass(const char* className) noexcept {
    return className && std::strcmp(className, "Splash") == 0;
}

inline bool IsPreferredGameWindowClass(const char* className) noexcept {
    return className &&
        (std::strcmp(className, "CFFGFCWnd") == 0 ||
         std::strcmp(className, "Gothic3") == 0 ||
         std::strcmp(className, "Gothic3 Engine") == 0);
}

} // namespace G3O
