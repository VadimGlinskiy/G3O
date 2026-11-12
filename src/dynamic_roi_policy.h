#pragma once
#include <cstdint>
#include <algorithm>

namespace G3O {

struct DynamicRoiState {
    float currentRoi = 4000.0f;
    bool crosshairVisible = false;
    uint32_t frameCount = 0;
};

class DynamicRoiPolicy {
public:
    static constexpr float kDefaultBaseRoi = 4000.0f;
    static constexpr float kDefaultAimingRoi = 7500.0f;
    static constexpr uint32_t kDefaultCrosshairRampUpFrames = 15;
    static constexpr uint32_t kDefaultCrosshairRampDownFrames = 30;

    // Moves currentRoi toward the configured target while the crosshair is visible.
    // Returns the new currentRoi.
    static float UpdateRoi(
        DynamicRoiState& state,
        bool crosshairVisible,
        float baseRoi = kDefaultBaseRoi,
        float aimingRoi = kDefaultAimingRoi,
        uint32_t rampUpFrames = kDefaultCrosshairRampUpFrames,
        uint32_t rampDownFrames = kDefaultCrosshairRampDownFrames) noexcept
    {
        state.crosshairVisible = crosshairVisible;
        state.frameCount++;

        const float targetRoi = crosshairVisible ? aimingRoi : baseRoi;
        if (state.currentRoi == targetRoi) {
            return state.currentRoi;
        }

        if (state.currentRoi < targetRoi) {
            const uint32_t frames = (rampUpFrames > 0) ? rampUpFrames : 1;
            const float span = (aimingRoi >= baseRoi) ? (aimingRoi - baseRoi) : (baseRoi - aimingRoi);
            const float step = span / static_cast<float>(frames);
            state.currentRoi = (std::min)(state.currentRoi + step, targetRoi);
        } else {
            const uint32_t frames = (rampDownFrames > 0) ? rampDownFrames : 1;
            const float span = (aimingRoi >= baseRoi) ? (aimingRoi - baseRoi) : (baseRoi - aimingRoi);
            const float step = span / static_cast<float>(frames);
            state.currentRoi = (std::max)(state.currentRoi - step, targetRoi);
        }

        return state.currentRoi;
    }
};

} // namespace G3O
