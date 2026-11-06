#pragma once

#include "config_manager.h"
#include <windows.h>

namespace G3O {

inline bool IsInitialKeyDown(LPARAM lParam) noexcept {
    constexpr ULONG_PTR previousKeyState = static_cast<ULONG_PTR>(1) << 30;
    return (static_cast<ULONG_PTR>(lParam) & previousKeyState) == 0;
}

inline bool ShouldInstallWindowInputHook(const Config& config) noexcept {
    return config.enableBorderless || (config.enableDynamicFOV && config.enableFOVHotkeys);
}

} // namespace G3O
