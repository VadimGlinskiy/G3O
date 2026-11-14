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

inline bool IsMediaKey(unsigned long long vkCode) noexcept {
    // VK_VOLUME_MUTE (0xAD) through VK_MEDIA_PLAY_PAUSE (0xB3)
    // VK_LAUNCH_MAIL (0xB4) through VK_LAUNCH_APP2 (0xB7)
    return (vkCode >= 0xAD && vkCode <= 0xB7);
}

inline bool ShouldClipCursor(bool isEnabled, bool isWindowActive, bool isForeground) noexcept {
    return isEnabled && isWindowActive && isForeground;
}

} // namespace G3O
