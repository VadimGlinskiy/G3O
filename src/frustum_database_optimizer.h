#pragma once

#include <cstdint>

namespace G3O {

void InitializeFrustumDatabaseOptimizer();
void ShutdownFrustumDatabaseOptimizer();

// Scoped lifecycle controls (called from Hooked_GotoLoadedPosition)
void FrustumOptimizer_BeginDefer();
void FrustumOptimizer_EndDeferAndFlush();

// Telemetry counters
std::uint32_t FrustumOptimizer_GetDeferredCalls() noexcept;
std::uint32_t FrustumOptimizer_GetFlushedUpdates() noexcept;
std::uint32_t FrustumOptimizer_GetDeletionsHandled() noexcept;

} // namespace G3O
