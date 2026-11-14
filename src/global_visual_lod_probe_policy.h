#pragma once

#include <cmath>

namespace G3O {

inline bool TryScaleGlobalVisualLoD(float factor, float scale, float& scaledFactor) noexcept {
    if (!std::isfinite(factor) || factor <= 0.0f ||
        !std::isfinite(scale) || scale < 0.25f || scale > 4.0f) {
        return false;
    }

    const float candidate = factor * scale;
    if (!std::isfinite(candidate) || candidate <= 0.0f) return false;

    scaledFactor = candidate;
    return true;
}

} // namespace G3O
