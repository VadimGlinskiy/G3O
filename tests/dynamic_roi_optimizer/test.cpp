#include "dynamic_roi_policy.h"
#include <iostream>
#include <cmath>

static bool Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        return false;
    }
    return true;
}

int main() {
    std::cout << "[Test] Dynamic ROI policy suite starting..." << std::endl;
    bool ok = true;

    using namespace G3O;

    // 1. Policy constants
    ok &= Expect(DynamicRoiPolicy::kDefaultBaseRoi == 4000.0f, "kDefaultBaseRoi must be 4000.0f");
    ok &= Expect(DynamicRoiPolicy::kDefaultAimingRoi == 7500.0f, "kDefaultAimingRoi must be 7500.0f");

    // 2. Aim Ramp-Up & Ramp-Down
    DynamicRoiState state{};
    state.currentRoi = 4000.0f;

    // Crosshair appears: ramp up over 15 frames from 4000 to 7500
    float prevRoi = state.currentRoi;
    for (int i = 0; i < 15; ++i) {
        float roi = DynamicRoiPolicy::UpdateRoi(state, true, 4000.0f, 7500.0f, 15, 30);
        ok &= Expect(roi >= prevRoi, "ROI must monotonically increase during aim ramp-up");
        ok &= Expect(roi <= 7500.0f, "ROI must not exceed aiming ROI");
        prevRoi = roi;
    }
    ok &= Expect(std::fabs(state.currentRoi - 7500.0f) < 0.1f, "ROI must reach 7500.0f after 15 frames");

    // Stays at 7500 while the crosshair remains visible
    float stillAiming = DynamicRoiPolicy::UpdateRoi(state, true, 4000.0f, 7500.0f, 15, 30);
    ok &= Expect(stillAiming == 7500.0f, "ROI must remain 7500.0f while aiming");

    // Crosshair disappears: ramp down over 30 frames from 7500 to 4000
    for (int i = 0; i < 30; ++i) {
        float roi = DynamicRoiPolicy::UpdateRoi(state, false, 4000.0f, 7500.0f, 15, 30);
        ok &= Expect(roi <= prevRoi, "ROI must monotonically decrease during aim ramp-down");
        ok &= Expect(roi >= 4000.0f, "ROI must not fall below base ROI");
        prevRoi = roi;
    }
    ok &= Expect(std::fabs(state.currentRoi - 4000.0f) < 0.1f, "ROI must return to 4000.0f after 30 frames");

    // 3. A lower crosshair target must ramp down without reversing direction
    DynamicRoiState lowerAimState{};
    lowerAimState.currentRoi = 8000.0f;
    prevRoi = lowerAimState.currentRoi;
    for (int i = 0; i < 30; ++i) {
        float roi = DynamicRoiPolicy::UpdateRoi(lowerAimState, true, 8000.0f, 7500.0f, 15, 30);
        ok &= Expect(roi <= prevRoi, "ROI must decrease monotonically when aiming target is below base");
        ok &= Expect(roi >= 7500.0f, "ROI must not fall below a lower aiming target");
        prevRoi = roi;
    }
    ok &= Expect(std::fabs(lowerAimState.currentRoi - 7500.0f) < 0.1f,
                 "ROI must reach a lower aiming target after the ramp-down frames");

    if (ok) {
        std::cout << "[Test] Dynamic ROI policy suite passed." << std::endl;
        return 0;
    }

    std::cerr << "[Test] Dynamic ROI policy suite failed." << std::endl;
    return 1;
}
