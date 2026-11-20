#pragma once

#include "config_manager.h"

#include <cstring>

namespace G3O {

inline constexpr int kSysDynSamplePhase = 4;

inline int ResolveSamplePhaseForSector(int currentPhase, const char* sectorName) noexcept {
    constexpr char prefix[] = "SysDyn_";
    return sectorName && std::strncmp(sectorName, prefix, sizeof(prefix) - 1) == 0
        ? kSysDynSamplePhase
        : currentPhase;
}

inline bool ShouldInstallSavegameProfilerHooks(const Config& config) noexcept {
    return config.enableSavegameAudit || config.enableProfiler || config.autoTestEnabled;
}

inline bool ShouldCollectSavegameAudit(const Config& config) noexcept {
    return config.enableSavegameAudit || config.autoTestEnabled;
}

} // namespace G3O
