#pragma once

#include <cstddef>
#include <cstdint>

namespace G3O {

struct LayerAuditMetric {
    std::uint64_t calls = 0;
    std::uint64_t qpcTicks = 0;
};

enum class PvsPrefetchOperation {
    Process,
    WaitForEmptyThreadQueue,
};

struct PvsPrefetchMetrics {
    LayerAuditMetric process;
    LayerAuditMetric waitForEmptyThreadQueue;
};

inline constexpr std::size_t kPropertyObjectReadVtableSlot = 5;

inline std::uintptr_t ResolvePropertyObjectReadTarget(const void* object) noexcept {
    if (!object) return 0;
    void* const* vtable = *reinterpret_cast<void* const* const*>(object);
    return vtable
        ? reinterpret_cast<std::uintptr_t>(vtable[kPropertyObjectReadVtableSlot])
        : 0;
}

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

inline void RecordPvsPrefetchDuration(
    PvsPrefetchMetrics& metrics, PvsPrefetchOperation operation,
    std::uint64_t start, std::uint64_t end) noexcept {
    RecordLayerAuditDuration(
        operation == PvsPrefetchOperation::Process
            ? metrics.process
            : metrics.waitForEmptyThreadQueue,
        start, end);
}

enum class EntityReadVariant {
    V63,
    V83,
};

struct EntityReadVariantMetrics {
    LayerAuditMetric v63;
    LayerAuditMetric v83;
};

inline void RecordEntityReadVariantDuration(
    EntityReadVariantMetrics& metrics, EntityReadVariant variant,
    std::uint64_t start, std::uint64_t end) noexcept {
    RecordLayerAuditDuration(
        variant == EntityReadVariant::V83 ? metrics.v83 : metrics.v63, start, end);
}

enum class EntityReadSuboperation {
    NodeRead,
    AccessorPropertyStreamRead,
};

struct EntityReadSuboperationMetrics {
    LayerAuditMetric nodeRead;
    LayerAuditMetric accessorPropertyStreamRead;
    LayerAuditMetric accessorPropertyStreamReadExclusive;
    std::uint64_t accessorNestedCalls = 0;
    std::uint64_t accessorExclusiveUnavailableCalls = 0;
};

inline void RecordEntityReadSuboperationDuration(
    EntityReadSuboperationMetrics& metrics, EntityReadSuboperation suboperation,
    std::uint64_t start, std::uint64_t end) noexcept {
    RecordLayerAuditDuration(
        suboperation == EntityReadSuboperation::NodeRead
            ? metrics.nodeRead
            : metrics.accessorPropertyStreamRead,
        start, end);
}

inline void RecordAccessorPropertyStreamReadDuration(
    EntityReadSuboperationMetrics& metrics,
    std::uint64_t start,
    std::uint64_t end,
    std::uint64_t exclusiveTicks,
    std::uint64_t depth,
    bool exclusiveAvailable = true) noexcept {
    if (end < start) return;
    const std::uint64_t inclusiveTicks = end - start;
    RecordLayerAuditDuration(metrics.accessorPropertyStreamRead, start, end);
    if (exclusiveAvailable && exclusiveTicks <= inclusiveTicks) {
        RecordLayerAuditDuration(
            metrics.accessorPropertyStreamReadExclusive, start, start + exclusiveTicks);
    } else {
        ++metrics.accessorExclusiveUnavailableCalls;
    }
    if (depth > 1) ++metrics.accessorNestedCalls;
}

struct PropertyReadDispatchEntry {
    std::uintptr_t target = 0;
    LayerAuditMetric metric{};
};

template <std::size_t Capacity>
struct PropertyReadDispatchMetrics {
    PropertyReadDispatchEntry entries[Capacity]{};
    std::uint64_t overflowCalls = 0;
    std::uint64_t unresolvedCalls = 0;
};

template <std::size_t Capacity>
inline bool RecordPropertyReadDispatchDuration(
    PropertyReadDispatchMetrics<Capacity>& metrics, std::uintptr_t target,
    std::uint64_t start, std::uint64_t end) noexcept {
    if (end < start) return false;
    if (!target) {
        ++metrics.unresolvedCalls;
        return false;
    }
    for (auto& entry : metrics.entries) {
        if (entry.target == target || entry.target == 0) {
            entry.target = target;
            RecordLayerAuditDuration(entry.metric, start, end);
            return true;
        }
    }
    ++metrics.overflowCalls;
    return false;
}

enum class PropertyReadDispatchPath {
    ReadObject,
    ReadNewObject,
};

template <std::size_t Capacity>
struct PropertyReadDispatchPaths {
    PropertyReadDispatchMetrics<Capacity> readObject;
    PropertyReadDispatchMetrics<Capacity> readNewObject;
};

template <std::size_t Capacity>
inline bool RecordPropertyReadDispatchDuration(
    PropertyReadDispatchPaths<Capacity>& metrics, PropertyReadDispatchPath path,
    std::uintptr_t target, std::uint64_t start, std::uint64_t end) noexcept {
    return RecordPropertyReadDispatchDuration(
        path == PropertyReadDispatchPath::ReadObject ? metrics.readObject : metrics.readNewObject,
        target, start, end);
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
