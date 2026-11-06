#include "layer_audit_metrics.h"
#include <cstdio>

static bool Expect(bool condition, const char* message) {
    if (condition) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

int main() {
    bool ok = true;

    G3O::LayerAuditMetric total{};
    G3O::RecordLayerAuditDuration(total, 10, 30);
    G3O::RecordLayerAuditDuration(total, 30, 35);
    G3O::RecordLayerAuditDuration(total, 40, 39);
    ok &= Expect(total.calls == 2 && total.qpcTicks == 25,
                 "valid per-layer call durations should aggregate; reversed samples should be ignored");

    G3O::LayerAuditMetrics before{};
    before.createData = {2, 40};
    before.doLoadData = {1, 25};
    G3O::LayerAuditMetrics after{};
    after.createData = {5, 100};
    after.doLoadData = {4, 125};
    const G3O::LayerAuditMetrics delta = G3O::DifferenceLayerAuditMetrics(after, before);
    ok &= Expect(delta.createData.calls == 3 && delta.createData.qpcTicks == 60,
                 "CreateData deltas should be measured independently");
    ok &= Expect(delta.doLoadData.calls == 3 && delta.doLoadData.qpcTicks == 100,
                 "DynamicLayer::DoLoadData deltas should be measured independently");

    ok &= Expect(G3O::ResolveLayerAuditHookState(false, false) == G3O::LayerAuditHookState::Disabled,
                 "audit hooks should be reported disabled when audit was not requested");
    ok &= Expect(G3O::ResolveLayerAuditHookState(true, true) == G3O::LayerAuditHookState::Enabled,
                 "audit hooks should be reported enabled only after successful installation");
    ok &= Expect(G3O::ResolveLayerAuditHookState(true, false) == G3O::LayerAuditHookState::Unavailable,
                 "requested but uninstalled audit hooks should be reported unavailable");

    if (!ok) return 1;
    std::puts("PASS: layer audit reports truthful hook state and independent call-time deltas");
    return 0;
}
