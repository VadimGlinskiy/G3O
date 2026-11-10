#pragma once

#include <cstdint>

namespace G3O {

void InitializeHashProxyOptimizer();
void ShutdownHashProxyOptimizer();

// Telemetry counters
std::uint32_t HashProxy_GetHashHits() noexcept;
std::uint32_t HashProxy_GetProxyHits() noexcept;
std::uint32_t HashProxy_GetProxyNegHits() noexcept;
std::uint32_t HashProxy_GetProxyMisses() noexcept;

void HashProxy_InvalidateNegativeCache() noexcept;
void HashProxy_ClearAll() noexcept;

} // namespace G3O
