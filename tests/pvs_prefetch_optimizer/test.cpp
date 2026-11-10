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

    // 1. Layout checks
    ok &= Expect(sizeof(GothicSphere) == 16, "GothicSphere size must be exactly 16 bytes");

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

    GothicSphere outSphere{};
    const float bootRadius = 3800.0f;
    const float redundantDeltaSq = 200.0f * 200.0f;
    const uint32_t rampCalls = 24;

    // First sync call (e.g. GotoLoadedPosition call #1)
    auto action = PvsPrefetchPolicy::EvaluateSync(state, sphere, bootRadius, redundantDeltaSq, rampCalls, outSphere);
    ok &= Expect(action == PvsSyncAction::ExecuteClamped, "First sync call must be clamped");
    ok &= Expect(outSphere.radius == 3800.0f, "Clamped radius must equal bootRadius (3800.0f)");
    ok &= Expect(outSphere.x == 1000.0f && outSphere.y == 500.0f && outSphere.z == 2000.0f, "Coordinates preserved");

    // Second sync call with exact same coordinates (GotoLoadedPosition call #2)
    action = PvsPrefetchPolicy::EvaluateSync(state, sphere, bootRadius, redundantDeltaSq, rampCalls, outSphere);
    ok &= Expect(action == PvsSyncAction::SkipRedundant, "Second sync call at same position must be skipped");

    // Third sync call with small camera jitter (50 units away)
    GothicSphere jitterSphere = sphere;
    jitterSphere.x += 30.0f;
    jitterSphere.z += 40.0f; // 30^2 + 40^2 = 2500 <= 40000
    action = PvsPrefetchPolicy::EvaluateSync(state, jitterSphere, bootRadius, redundantDeltaSq, rampCalls, outSphere);
    ok &= Expect(action == PvsSyncAction::SkipRedundant, "Third sync call within 50 units must be skipped");

    // Fourth sync call moved 300 units away (e.g. teleporting to another spot)
    GothicSphere movedSphere = sphere;
    movedSphere.x += 300.0f; // 300^2 = 90000 > 40000
    action = PvsPrefetchPolicy::EvaluateSync(state, movedSphere, bootRadius, redundantDeltaSq, rampCalls, outSphere);
    ok &= Expect(action == PvsSyncAction::ExecuteClamped, "Sync call moved > 200 units must execute clamped");
    ok &= Expect(outSphere.radius == 3800.0f, "Moved sync call clamped to 3800");

    // 3. Async progressive ramp-up in gameplay
    GothicSphere gameSphere{};
    gameSphere.radius = 14000.0f;
    gameSphere.x = movedSphere.x;
    gameSphere.y = movedSphere.y;
    gameSphere.z = movedSphere.z;

    float prevRadius = bootRadius;
    for (uint32_t i = 0; i < rampCalls; ++i) {
        GothicSphere ramped{};
        bool isRamped = PvsPrefetchPolicy::EvaluateAsync(state, gameSphere, bootRadius, rampCalls, ramped);
        ok &= Expect(isRamped, "Async call during ramp-up period must return ramped=true");
        ok &= Expect(ramped.radius >= prevRadius, "Radius must monotonically increase during ramp-up");
        ok &= Expect(ramped.radius <= 14000.0f, "Radius must not exceed original");
        prevRadius = ramped.radius;
    }

    // 4. After ramp-up finishes
    GothicSphere finalSphere{};
    bool isRampedAfter = PvsPrefetchPolicy::EvaluateAsync(state, gameSphere, bootRadius, rampCalls, finalSphere);
    ok &= Expect(!isRampedAfter, "Async call after ramp-up completes must return ramped=false");
    ok &= Expect(finalSphere.radius == 14000.0f, "Radius must be fully restored to 14000.0f");

    if (ok) {
        std::cout << "[Test] PVS Prefetch Optimizer Policy Suite PASSED!\n";
        return 0;
    } else {
        std::cerr << "[Test] PVS Prefetch Optimizer Policy Suite FAILED!\n";
        return 1;
    }
}
