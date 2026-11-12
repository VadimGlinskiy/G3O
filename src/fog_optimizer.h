#pragma once

#include <cstdint>

namespace G3O {

struct FogOptimizerStats {
    std::uint64_t totalSubmissions;
    std::uint64_t expandedSubmissions;
    std::uint64_t underwaterPreserved;
    float lastStart;
    float lastEnd;
};

void InitializeFogOptimizer();
void ShutdownFogOptimizer();
FogOptimizerStats GetFogOptimizerStats() noexcept;
void ResetFogOptimizerStats() noexcept;

} // namespace G3O
