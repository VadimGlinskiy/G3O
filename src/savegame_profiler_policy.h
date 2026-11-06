#pragma once

#include "config_manager.h"

namespace G3O {

inline bool ShouldTracePakPrefetch(const Config& config) noexcept {
    return config.enablePakPrefetch && config.enableFastPakDecompressor;
}

inline bool ShouldInstallSavegameProfilerHooks(const Config& config) noexcept {
    return config.enableSavegameAudit || config.enableProfiler || ShouldTracePakPrefetch(config);
}

inline bool ShouldCollectSavegameAudit(const Config& config) noexcept {
    return config.enableSavegameAudit;
}

} // namespace G3O
