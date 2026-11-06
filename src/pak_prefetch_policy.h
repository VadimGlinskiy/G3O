#pragma once

namespace G3O {

inline constexpr bool ShouldTryPredecompressedLookup(
    bool tracerActive, bool prefetchWorkerStarted) noexcept {
    return tracerActive && prefetchWorkerStarted;
}

} // namespace G3O
