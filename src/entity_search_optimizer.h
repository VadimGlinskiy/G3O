#pragma once
#include <cstdint>

namespace G3O {

bool InitializeEntitySearchOptimizer();
void ShutdownEntitySearchOptimizer();

uint64_t GetEntitySearchFastHits();
uint64_t GetEntitySearchFallbackSorts();

} // namespace G3O
