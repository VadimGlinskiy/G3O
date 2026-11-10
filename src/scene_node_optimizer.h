#pragma once
#include <cstdint>

namespace G3O {

void InitializeSceneNodeOptimizer();
void ShutdownSceneNodeOptimizer();

uint32_t SceneNodeOptimizer_GetPreallocations() noexcept;

} // namespace G3O
