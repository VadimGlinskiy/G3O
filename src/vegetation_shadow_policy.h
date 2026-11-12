#pragma once
#include <cstdint>

namespace G3O {

struct VegetationShadowConfig {
    bool enableVegetationOptimizer = true;
    bool enableAdaptiveTreeShadows = true;
    float treeShadowDistance = 6500.0f;
};

struct VegetationShadowThresholds {
    float bushDistSq = 0.0f;
    float mediumDistSq = 0.0f;
    float giantDistSq = 0.0f;
};

inline VegetationShadowThresholds ComputeVegetationShadowThresholds(float baseDist) {
    VegetationShadowThresholds t{};
    if (baseDist <= 0.0f) {
        return t;
    }

    constexpr float kBushRatio  = 4000.0f / 6500.0f;  // ~0.6153846f
    constexpr float kGiantRatio = 8500.0f / 6500.0f;  // ~1.3076923f

    float bushDist  = baseDist * kBushRatio;
    float giantDist = baseDist * kGiantRatio;

    t.bushDistSq   = bushDist * bushDist;
    t.mediumDistSq = baseDist * baseDist;
    t.giantDistSq  = giantDist * giantDist;
    return t;
}

enum class TreeShadowClass {
    Bush,
    Medium,
    Giant,
    Unknown
};

inline TreeShadowClass ClassifyTreeByHeight(float height) {
    if (height <= 0.0f) {
        return TreeShadowClass::Unknown;
    }
    if (height < 400.0f) {
        return TreeShadowClass::Bush;
    }
    if (height > 1200.0f) {
        return TreeShadowClass::Giant;
    }
    return TreeShadowClass::Medium;
}

inline bool ShouldCullTreeShadow(
    float distSq,
    float height,
    bool enableAdaptive,
    const VegetationShadowThresholds& thresholds)
{
    if (thresholds.mediumDistSq <= 0.0f) {
        return true; // Base distance <= 0 disables all tree shadows
    }

    float maxDistSq = thresholds.mediumDistSq;

    if (enableAdaptive && height > 0.0f) {
        if (height < 400.0f) {
            maxDistSq = thresholds.bushDistSq;
        } else if (height > 1200.0f) {
            maxDistSq = thresholds.giantDistSq;
        }
    }

    return distSq > maxDistSq;
}

} // namespace G3O
