#pragma once

#include <cstdint>

namespace G3O {

void InitializeCrashGuards();
void ShutdownCrashGuards();

// Diagnostic counters
std::uint64_t GetFocusNameGuardSkips();
std::uint64_t GetBoneMatrixGuardSkips();
std::uint64_t GetInventoryGuardSkips();

} // namespace G3O
