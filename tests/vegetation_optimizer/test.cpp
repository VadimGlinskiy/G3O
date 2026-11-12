#include "vegetation_shadow_policy.h"
#include <iostream>
#include <cmath>

static bool Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        return false;
    }
    return true;
}

int main() {
    bool ok = true;
    std::cout << "[Test] Vegetation Optimizer Adaptive Shadow Suite starting...\n";

    // 1. Threshold computation for base 6500.0f
    {
        const float baseDist = 6500.0f;
        auto t = G3O::ComputeVegetationShadowThresholds(baseDist);

        float bushDist = std::sqrt(t.bushDistSq);
        float mediumDist = std::sqrt(t.mediumDistSq);
        float giantDist = std::sqrt(t.giantDistSq);

        ok &= Expect(std::abs(bushDist - 4000.0f) < 1.0f, "bush distance should be ~4000 units (40m)");
        ok &= Expect(std::abs(mediumDist - 6500.0f) < 1.0f, "medium distance should be 6500 units (65m)");
        ok &= Expect(std::abs(giantDist - 8500.0f) < 1.0f, "giant distance should be ~8500 units (85m)");
    }

    // 2. Threshold computation for 0.0f (disabled / max FPS)
    {
        auto t = G3O::ComputeVegetationShadowThresholds(0.0f);
        ok &= Expect(t.bushDistSq == 0.0f && t.mediumDistSq == 0.0f && t.giantDistSq == 0.0f,
                     "0 base distance should result in zero thresholds");

        ok &= Expect(G3O::ShouldCullTreeShadow(100.0f, 500.0f, true, t),
                     "zero threshold should cull all tree shadows");
    }

    // 3. Tree classification by height
    {
        ok &= Expect(G3O::ClassifyTreeByHeight(-10.0f) == G3O::TreeShadowClass::Unknown,
                     "negative height should be Unknown");
        ok &= Expect(G3O::ClassifyTreeByHeight(0.0f) == G3O::TreeShadowClass::Unknown,
                     "zero height should be Unknown");
        ok &= Expect(G3O::ClassifyTreeByHeight(250.0f) == G3O::TreeShadowClass::Bush,
                     "height 250 cm should be classified as Bush (<400)");
        ok &= Expect(G3O::ClassifyTreeByHeight(800.0f) == G3O::TreeShadowClass::Medium,
                     "height 800 cm should be classified as Medium (400..1200)");
        ok &= Expect(G3O::ClassifyTreeByHeight(1500.0f) == G3O::TreeShadowClass::Giant,
                     "height 1500 cm should be classified as Giant (>1200)");
    }

    // 4. Adaptive culling decisions
    {
        auto t = G3O::ComputeVegetationShadowThresholds(6500.0f);

        // Bush (height = 300 cm)
        // 35m (3500) -> inside 40m, should render
        ok &= Expect(!G3O::ShouldCullTreeShadow(3500.0f * 3500.0f, 300.0f, true, t),
                     "bush at 35m should NOT be culled");
        // 45m (4500) -> outside 40m, should be culled
        ok &= Expect(G3O::ShouldCullTreeShadow(4500.0f * 4500.0f, 300.0f, true, t),
                     "bush at 45m SHOULD be culled");

        // Medium tree (height = 800 cm)
        // 45m (4500) -> inside 65m, should render
        ok &= Expect(!G3O::ShouldCullTreeShadow(4500.0f * 4500.0f, 800.0f, true, t),
                     "medium tree at 45m should NOT be culled");
        // 70m (7000) -> outside 65m, should be culled
        ok &= Expect(G3O::ShouldCullTreeShadow(7000.0f * 7000.0f, 800.0f, true, t),
                     "medium tree at 70m SHOULD be culled");

        // Giant tree (height = 1600 cm)
        // 70m (7000) -> inside 85m, should render!
        ok &= Expect(!G3O::ShouldCullTreeShadow(7000.0f * 7000.0f, 1600.0f, true, t),
                     "giant tree at 70m should NOT be culled (extended to 85m)");
        // 90m (9000) -> outside 85m, should be culled
        ok &= Expect(G3O::ShouldCullTreeShadow(9000.0f * 9000.0f, 1600.0f, true, t),
                     "giant tree at 90m SHOULD be culled");

        // Fallback with invalid/zero height: uses medium threshold (65m)
        ok &= Expect(!G3O::ShouldCullTreeShadow(5000.0f * 5000.0f, 0.0f, true, t),
                     "zero height at 50m should use fallback medium threshold and NOT cull");
        ok &= Expect(G3O::ShouldCullTreeShadow(7000.0f * 7000.0f, 0.0f, true, t),
                     "zero height at 70m should use fallback medium threshold and cull");

        // Disabled adaptive: bush at 50m should NOT be culled (uses flat 65m)
        ok &= Expect(!G3O::ShouldCullTreeShadow(5000.0f * 5000.0f, 300.0f, false, t),
                     "when adaptive is disabled, bush at 50m should use flat 65m threshold");
    }

    if (!ok) {
        std::cerr << "FAIL: vegetation_optimizer tests failed!\n";
        return 1;
    }

    std::cout << "PASS: vegetation_optimizer adaptive shadow policy passed.\n";
    return 0;
}
