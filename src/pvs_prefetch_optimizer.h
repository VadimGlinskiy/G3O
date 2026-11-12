#pragma once
#include <cstdint>

namespace G3O {

void InitializePVSPrefetchOptimizer();
void ShutdownPVSPrefetchOptimizer();
void ResetPVSPrefetchOptimizerSession();

uint32_t PVSOptimizer_GetClampedCalls() noexcept;
uint32_t PVSOptimizer_GetSkippedCalls() noexcept;
uint32_t PVSOptimizer_GetRampedCalls() noexcept;
float    PVSOptimizer_GetEffectiveROI() noexcept;
float    PVSOptimizer_GetOriginalTargetROI() noexcept;
bool     PVSOptimizer_IsRampingUp() noexcept;
void     PVSOptimizer_ApplyEffectiveROI(float roi) noexcept;

} // namespace G3O
