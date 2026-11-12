#pragma once
#include <cstdint>

namespace G3O {

void InitializeDynamicROIOptimizer();
void ShutdownDynamicROIOptimizer();

// Statistics and diagnostics
uint32_t DynamicROI_GetCrosshairVisibleFrames() noexcept;
float    DynamicROI_GetCurrentROI() noexcept;

} // namespace G3O
