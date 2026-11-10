#pragma once
#include <cstdint>

namespace G3O {

void InitializeNavigationOptimizer();
void ShutdownNavigationOptimizer();

uint32_t NavigationOptimizer_GetSkippedTicks() noexcept;
uint32_t NavigationOptimizer_GetGameplayTicks() noexcept;

} // namespace G3O
