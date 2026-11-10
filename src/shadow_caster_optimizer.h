#pragma once
#include <cstdint>

namespace G3O {

void InitializeShadowCasterOptimizer();
void ShutdownShadowCasterOptimizer();
void ResetShadowCasterOptimizerSession();

uint32_t ShadowOptimizer_GetFastAdds() noexcept;
uint32_t ShadowOptimizer_GetDuplicatesAvoided() noexcept;
uint32_t ShadowOptimizer_GetFastRemoves() noexcept;
uint32_t ShadowOptimizer_GetFallbackRemoves() noexcept;

} // namespace G3O
