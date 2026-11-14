#include "global_visual_lod_probe_policy.h"
#include <cmath>
#include <cstdio>
#include <limits>

static bool Expect(bool condition, const char* message) {
    if (condition) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

int main() {
    bool ok = true;
    float scaled = -1.0f;
    ok &= Expect(G3O::TryScaleGlobalVisualLoD(1.0f, 0.75f, scaled) && scaled == 0.75f,
                 "probe should scale a valid engine LOD factor");
    ok &= Expect(G3O::TryScaleGlobalVisualLoD(2.0f, 1.25f, scaled) && scaled == 2.5f,
                 "probe should preserve the incoming factor while applying its scale");

    scaled = 123.0f;
    ok &= Expect(!G3O::TryScaleGlobalVisualLoD(std::numeric_limits<float>::quiet_NaN(), 1.25f, scaled) && scaled == 123.0f,
                 "non-finite engine values should pass through untouched");
    ok &= Expect(!G3O::TryScaleGlobalVisualLoD(1.0f, 0.0f, scaled) && scaled == 123.0f,
                 "out-of-range probe scales should be rejected without changing output");
    ok &= Expect(!G3O::TryScaleGlobalVisualLoD(std::numeric_limits<float>::max(), 4.0f, scaled) && scaled == 123.0f,
                 "overflowing scaled factors should be rejected without changing output");

    if (!ok) return 1;
    std::puts("PASS: global visual LOD probe policy");
    return 0;
}
