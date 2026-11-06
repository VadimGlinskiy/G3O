#pragma once

#include <cstddef>
#include <cstdint>

namespace G3O::CpuAffinity {

using Mask = std::uintptr_t;

inline bool CanRepresentProcessorCount(unsigned processorCount, unsigned maskBits) noexcept {
    return processorCount != 0 && processorCount <= maskBits;
}

inline bool IsValidMask(Mask requested, Mask available) noexcept {
    return requested != 0 && (requested & ~available) == 0;
}

inline Mask SelectLogicalProcessors(Mask available, unsigned count) noexcept {
    Mask selected = 0;
    while (available != 0 && count != 0) {
        const Mask bit = available & (~available + Mask{ 1 });
        selected |= bit;
        available &= ~bit;
        --count;
    }
    return selected;
}

inline Mask SelectPhysicalCores(const Mask* coreMasks, std::size_t coreCount,
                                unsigned requestedCores, Mask allowed) noexcept {
    Mask selected = 0;
    for (std::size_t i = 0; i < coreCount && requestedCores != 0; ++i) {
        const Mask availableOnCore = coreMasks[i] & allowed;
        if (availableOnCore == 0) continue;
        if ((selected & availableOnCore) != 0) continue;
        selected |= availableOnCore & (~availableOnCore + Mask{ 1 });
        --requestedCores;
    }
    return selected;
}

} // namespace G3O::CpuAffinity
