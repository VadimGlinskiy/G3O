#pragma once

#include <cstdint>

namespace G3O {

bool InitializeGuidMapOptimizer();
void LogGuidMapStats();
uint32_t GetGuidMapL1Hits() noexcept;
uint32_t GetGuidMapFastWalkHits() noexcept;
uint32_t GetGuidMapMisses() noexcept;

} // namespace G3O
