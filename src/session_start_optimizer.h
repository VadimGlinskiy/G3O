#pragma once
#include <cstdint>

namespace G3O {

bool InitializeSessionStartOptimizer();
void ShutdownSessionStartOptimizer();

uint32_t GetSessionStartSleepHookCount() noexcept;

} // namespace G3O
