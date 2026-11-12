#include "../../src/pvs_prefetch_policy.h"
#include <iostream>
#include <cmath>
#include <cassert>

using namespace G3O;

static bool Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        return false;
    }
    return true;
}

int main() {
    bool ok = true;
    std::cout << "[Test] PVS Prefetch Optimizer Policy Suite starting...\n";

    // 1. Layout & constant checks
    ok &= Expect(sizeof(GothicSphere) == 16, "GothicSphere size must be exactly 16 bytes");
    ok &= Expect(PvsPrefetchPolicy::kDefaultBootRadius == 3800.0f, "kDefaultBootRadius must be 3800.0f");
    ok &= Expect(PvsPrefetchPolicy::kDefaultBootRoiRadius == 3600.0f, "kDefaultBootRoiRadius must be 3600.0f");

    GothicSphere sphere{};
    sphere.radius = 14000.0f;
    sphere.x = 1000.0f;
    sphere.y = 500.0f;
    sphere.z = 2000.0f;

    ok &= Expect(sphere.radius == 14000.0f, "Radius field at offset 0");
    ok &= Expect(sphere.x == 1000.0f, "X field at offset 4");
    ok &= Expect(sphere.y == 500.0f, "Y field at offset 8");
    ok &= Expect(sphere.z == 2000.0f, "Z field at offset 12");

    // 2. Policy state and sync evaluation
    PvsPolicyState state{};
    PvsPrefetchPolicy::Reset(state);

    PvsPolicyState unknownTargetState{};
    ok &= Expect(unknownTargetState.targetRoiRadius == 0.0f,
                 "unknown target ROI starts unset");
    PvsPrefetchPolicy::Reset(unknownTargetState);
    GothicSphere unknownTargetOutput{};
    const auto unknownTargetAction = PvsPrefetchPolicy::EvaluateSync(
        unknownTargetState, sphere, 3800.0f, 200.0f * 200.0f, 24,
        unknownTargetOutput, 3600.0f, 0.0f);
    ok &= Expect(unknownTargetAction == PvsSyncAction::ExecuteClamped,
                 "PVS sphere clamping should work without a cached Entity.ROI target");
    ok &= Expect(unknownTargetState.targetRoiRadius == 0.0f &&
                 unknownTargetState.effectiveRoiRadius == 0.0f,
                 "unknown Entity.ROI must not acquire a guessed target or boot value");
    const bool unknownTargetRamped = PvsPrefetchPolicy::EvaluateAsync(
        unknownTargetState, sphere, 3800.0f, 24, unknownTargetOutput, 3600.0f);
    ok &= Expect(unknownTargetRamped && unknownTargetState.effectiveRoiRadius == 0.0f,
                 "PVS sphere can ramp while Entity.ROI remains untouched when its target is unknown");

    GothicSphere outSphere{};
    const float bootRadius = 3800.0f;
    const float bootRoiRadius = 3600.0f;
    const float targetRoiRadius = 4000.0f;
    const float redundantDeltaSq = 200.0f * 200.0f;
    const uint32_t rampCalls = 24;

    // First sync call (e.g. GotoLoadedPosition call #1)
    auto action = PvsPrefetchPolicy::EvaluateSync(state, sphere, bootRadius, redundantDeltaSq, rampCalls, outSphere, bootRoiRadius, targetRoiRadius);
    ok &= Expect(action == PvsSyncAction::ExecuteClamped, "First sync call must be clamped");
    ok &= Expect(outSphere.radius == 3800.0f, "Clamped radius must equal bootRadius (3800.0f)");
    ok &= Expect(state.effectiveRoiRadius == 3600.0f, "Clamped ROI must equal bootRoiRadius (3600.0f)");
    ok &= Expect(state.targetRoiRadius == 4000.0f, "Target ROI must equal 4000.0f");
    ok &= Expect(outSphere.x == 1000.0f && outSphere.y == 500.0f && outSphere.z == 2000.0f, "Coordinates preserved");

    // Second sync call with exact same coordinates (GotoLoadedPosition call #2)
    action = PvsPrefetchPolicy::EvaluateSync(state, sphere, bootRadius, redundantDeltaSq, rampCalls, outSphere, bootRoiRadius, targetRoiRadius);
    ok &= Expect(action == PvsSyncAction::SkipRedundant, "Second sync call at same position must be skipped");
    ok &= Expect(state.effectiveRoiRadius == 3600.0f, "ROI remains clamped during redundant call");

    PvsPolicyState radiusChangedState{};
    GothicSphere wideSphere = sphere;
    action = PvsPrefetchPolicy::EvaluateSync(
        radiusChangedState, wideSphere, bootRadius, redundantDeltaSq, rampCalls,
        outSphere, bootRoiRadius, targetRoiRadius);
    GothicSphere narrowSphere = wideSphere;
    narrowSphere.radius = 1000.0f;
    action = PvsPrefetchPolicy::EvaluateSync(
        radiusChangedState, narrowSphere, bootRadius, redundantDeltaSq, rampCalls,
        outSphere, bootRoiRadius, targetRoiRadius);
    ok &= Expect(action == PvsSyncAction::ExecuteNormal,
                 "same center with a materially changed radius must be processed");

    // Third sync call with small camera jitter (50 units away)
    GothicSphere jitterSphere = sphere;
    jitterSphere.x += 30.0f;
    jitterSphere.z += 40.0f; // 30^2 + 40^2 = 2500 <= 40000
    action = PvsPrefetchPolicy::EvaluateSync(state, jitterSphere, bootRadius, redundantDeltaSq, rampCalls, outSphere, bootRoiRadius, targetRoiRadius);
    ok &= Expect(action == PvsSyncAction::SkipRedundant, "Third sync call within 50 units must be skipped");

    // Fourth sync call moved 300 units away (e.g. teleporting to another spot)
    GothicSphere movedSphere = sphere;
    movedSphere.x += 300.0f; // 300^2 = 90000 > 40000
    action = PvsPrefetchPolicy::EvaluateSync(state, movedSphere, bootRadius, redundantDeltaSq, rampCalls, outSphere, bootRoiRadius, targetRoiRadius);
    ok &= Expect(action == PvsSyncAction::ExecuteClamped, "Sync call moved > 200 units must execute clamped");
    ok &= Expect(outSphere.radius == 3800.0f, "Moved sync call clamped to 3800");
    ok &= Expect(state.effectiveRoiRadius == 3600.0f, "ROI clamped to 3600 on moved sync call");

    // 3. Async progressive ramp-up in gameplay
    GothicSphere gameSphere{};
    gameSphere.radius = 14000.0f;
    gameSphere.x = movedSphere.x;
    gameSphere.y = movedSphere.y;
    gameSphere.z = movedSphere.z;

    float prevRadius = bootRadius;
    float prevRoi = bootRoiRadius;
    for (uint32_t i = 0; i < rampCalls; ++i) {
        GothicSphere ramped{};
        bool isRamped = PvsPrefetchPolicy::EvaluateAsync(state, gameSphere, bootRadius, rampCalls, ramped, bootRoiRadius);
        ok &= Expect(isRamped, "Async call during ramp-up period must return ramped=true");
        ok &= Expect(ramped.radius >= prevRadius, "PVS radius must monotonically increase during ramp-up");
        ok &= Expect(ramped.radius <= 14000.0f, "PVS radius must not exceed original");
        ok &= Expect(state.effectiveRoiRadius >= prevRoi, "ROI radius must monotonically increase during ramp-up");
        ok &= Expect(state.effectiveRoiRadius <= targetRoiRadius, "ROI radius must not exceed target ROI");
        prevRadius = ramped.radius;
        prevRoi = state.effectiveRoiRadius;
    }

    // 4. After ramp-up finishes
    GothicSphere finalSphere{};
    bool isRampedAfter = PvsPrefetchPolicy::EvaluateAsync(state, gameSphere, bootRadius, rampCalls, finalSphere, bootRoiRadius);
    ok &= Expect(!isRampedAfter, "Async call after ramp-up completes must return ramped=false");
    ok &= Expect(finalSphere.radius == 14000.0f, "Radius must be fully restored to 14000.0f");
    ok &= Expect(state.effectiveRoiRadius == 4000.0f, "ROI radius must be fully restored to 4000.0f");

    // 5. Standalone CalculateEffectiveRoi unit test
    ok &= Expect(PvsPrefetchPolicy::CalculateEffectiveRoi(24, 24, 2400.0f, 4000.0f) == 2400.0f, "0% progress ROI is 2400");
    ok &= Expect(std::fabs(PvsPrefetchPolicy::CalculateEffectiveRoi(12, 24, 2400.0f, 4000.0f) - 3200.0f) < 0.01f, "50% progress ROI is 3200");
    ok &= Expect(PvsPrefetchPolicy::CalculateEffectiveRoi(0, 24, 2400.0f, 4000.0f) == 4000.0f, "100% progress ROI is 4000");

    if (ok) {
        std::cout << "[Test] PVS Prefetch Optimizer Policy Suite PASSED!\n";
        return 0;
    } else {
        std::cerr << "[Test] PVS Prefetch Optimizer Policy Suite FAILED!\n";
        return 1;
    }
}
