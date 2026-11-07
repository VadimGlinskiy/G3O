#pragma once

#include <cstdint>
#include <string>

namespace G3O {

constexpr bool IsGameplayCaptureActive(bool gameSessionStarted, bool loadActive) noexcept {
    return gameSessionStarted && !loadActive;
}

constexpr bool ShouldRecordGameplayFrameDelta(std::int32_t frameTimeUs) noexcept {
    return frameTimeUs > 100;
}

inline std::string NormalizeHotspotKey(const std::string& label) {
    // Only a named export provides a defensible function boundary. Preserve raw
    // module-relative EIP values for internal, non-exported code.
    const std::size_t exportStart = label.find('!');
    if (exportStart == std::string::npos) return label;

    const std::size_t offset = label.rfind("+0x");
    if (offset != std::string::npos && offset > exportStart) {
        return label.substr(0, offset);
    }
    return label;
}

} // namespace G3O
