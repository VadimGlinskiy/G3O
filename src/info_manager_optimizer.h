#pragma once

#include <cstdint>

namespace G3O {

void InitializeInfoManagerOptimizer();
void ShutdownInfoManagerOptimizer();

uint64_t InfoManagerOptimizer_GetHits() noexcept;
uint64_t InfoManagerOptimizer_GetMisses() noexcept;
uint32_t InfoManagerOptimizer_GetRebuilds() noexcept;
uint32_t InfoManagerOptimizer_GetIndexedCount() noexcept;
uint32_t InfoManagerOptimizer_GetBucketExpansions() noexcept;
void InfoManagerOptimizer_LogStats() noexcept;

} // namespace G3O
