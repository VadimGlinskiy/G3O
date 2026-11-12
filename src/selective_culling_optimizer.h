#pragma once

#include <cstdint>

namespace G3O {

struct SelectiveCullingStats {
    std::uint64_t totalEvaluations;
    std::uint64_t importantEvaluations;
    std::uint64_t clutterEvaluations;
    std::uint64_t culledCount;
};

void InitializeSelectiveCullingOptimizer();
void ShutdownSelectiveCullingOptimizer();
SelectiveCullingStats GetSelectiveCullingStats() noexcept;
void ResetSelectiveCullingStats() noexcept;

} // namespace G3O
