#pragma once

#include <cstdint>
#include <cmath>
#include <algorithm>

namespace G3O {

#pragma pack(push, 1)
struct GfxFogData {
    std::uint8_t  bEnable;      // +0x00: D3DRS_FOGENABLE (0 or 1)
    std::uint8_t  subMode;      // +0x01
    std::uint16_t pad;          // +0x02
    std::uint32_t color;        // +0x04: D3DRS_FOGCOLOR (ARGB)
    std::uint32_t type;         // +0x08
    std::uint32_t mode;         // +0x0C: D3DFOGMODE (3 = D3DFOG_LINEAR)
    float         fStart;       // +0x10: D3DRS_FOGSTART
    float         fEnd;         // +0x14: D3DRS_FOGEND
    float         fDensity;     // +0x18: D3DRS_FOGDENSITY
};
#pragma pack(pop)

static_assert(sizeof(GfxFogData) == 28, "GfxFogData must be exactly 28 bytes");

inline bool IsUnderwaterOrCloseRangeFog(const GfxFogData& data) noexcept {
    // Underwater fog or close-range poison/cave gas has very short fEnd (< 30 meters = 3000 units)
    return data.fEnd > 0.0f && data.fEnd < 3000.0f;
}

inline void ExpandAtmosphericFog(
    GfxFogData& data,
    float distanceMultiplier,
    float fogEndMinDistance,
    bool preserveUnderwater
) noexcept {
    // If fog is disabled, or non-linear/invalid range, do nothing
    if (!data.bEnable || data.fEnd <= 0.0f) {
        return;
    }

    // Preserve special close-range underwater and cave fog if requested
    if (preserveUnderwater && IsUnderwaterOrCloseRangeFog(data)) {
        return;
    }

    const float mult = (distanceMultiplier >= 0.1f) ? distanceMultiplier : 1.0f;
    float newEnd = data.fEnd * mult;

    // Apply minimum horizon distance only to clear/moderate weather (fEnd >= 35000 = 350m)
    // to preserve the atmospheric density of heavy storms, dense blizzards, and sandstorms
    if (fogEndMinDistance > 0.0f && data.fEnd >= 35000.0f && newEnd < fogEndMinDistance) {
        newEnd = fogEndMinDistance;
    }

    // Scale fStart proportionally to maintain the atmospheric gradient transition ratio
    float ratio = (data.fEnd > 1e-3f) ? (data.fStart / data.fEnd) : 0.2f;
    if (ratio < 0.0f) ratio = 0.0f;
    if (ratio > 0.9f) ratio = 0.9f;

    data.fStart = newEnd * ratio;
    data.fEnd = newEnd;
}

} // namespace G3O
