#pragma once
#include <cstdint>

namespace G3O {

void InitializeQuestLoadOptimizer();
void ShutdownQuestLoadOptimizer();

uint32_t QuestLoadOptimizer_GetSuppressedQuestChecks() noexcept;
uint32_t QuestLoadOptimizer_GetSuppressedInfoChecks() noexcept;

} // namespace G3O
