#pragma once

#include <cstdint>

namespace G3O {

struct LayerAuditMetric {
    std::uint64_t calls = 0;
    std::uint64_t qpcTicks = 0;
};

struct LayerAuditMetrics {
    LayerAuditMetric createData;
    LayerAuditMetric doLoadData;
    LayerAuditMetric createDynamicEntity;
    LayerAuditMetric createFromTemplate;
};

enum class LayerAuditHookState {
    Disabled,
    Enabled,
    Unavailable,
};

inline constexpr LayerAuditHookState ResolveLayerAuditHookState(
    bool auditRequested, bool hooksInstalled) noexcept {
    if (!auditRequested) return LayerAuditHookState::Disabled;
    return hooksInstalled ? LayerAuditHookState::Enabled : LayerAuditHookState::Unavailable;
}

inline constexpr const char* LayerAuditHookStateName(LayerAuditHookState state) noexcept {
    switch (state) {
    case LayerAuditHookState::Disabled: return "disabled";
    case LayerAuditHookState::Enabled: return "enabled";
    case LayerAuditHookState::Unavailable: return "unavailable";
    }
    return "unavailable";
}

inline void RecordLayerAuditDuration(
    LayerAuditMetric& metric, std::uint64_t start, std::uint64_t end) noexcept {
    if (end < start) return;
    metric.qpcTicks += end - start;
    ++metric.calls;
}

inline LayerAuditMetric DifferenceLayerAuditMetric(
    const LayerAuditMetric& after, const LayerAuditMetric& before) noexcept {
    if (after.calls < before.calls || after.qpcTicks < before.qpcTicks) return {};
    return { after.calls - before.calls, after.qpcTicks - before.qpcTicks };
}

inline LayerAuditMetrics DifferenceLayerAuditMetrics(
    const LayerAuditMetrics& after, const LayerAuditMetrics& before) noexcept {
    return {
        DifferenceLayerAuditMetric(after.createData, before.createData),
        DifferenceLayerAuditMetric(after.doLoadData, before.doLoadData),
        DifferenceLayerAuditMetric(after.createDynamicEntity, before.createDynamicEntity),
        DifferenceLayerAuditMetric(after.createFromTemplate, before.createFromTemplate),
    };
}

} // namespace G3O
