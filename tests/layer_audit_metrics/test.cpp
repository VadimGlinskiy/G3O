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

    G3O::PvsPrefetchMetrics pvs{};
    G3O::RecordPvsPrefetchDuration(pvs, G3O::PvsPrefetchOperation::Process, 10, 40);
    G3O::RecordPvsPrefetchDuration(
        pvs, G3O::PvsPrefetchOperation::WaitForEmptyThreadQueue, 15, 35);
    G3O::RecordPvsPrefetchDuration(pvs, G3O::PvsPrefetchOperation::Process, 50, 70);
    ok &= Expect(pvs.process.calls == 2 && pvs.process.qpcTicks == 50,
                 "PVS Process calls should aggregate independently");
    ok &= Expect(pvs.waitForEmptyThreadQueue.calls == 1
                 && pvs.waitForEmptyThreadQueue.qpcTicks == 20,
                 "PVS queue-wait inclusive time should remain a separate metric");

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

    G3O::EntityReadVariantMetrics readVariants{};
    G3O::RecordEntityReadVariantDuration(readVariants, G3O::EntityReadVariant::V83, 10, 30);
    G3O::RecordEntityReadVariantDuration(readVariants, G3O::EntityReadVariant::V63, 30, 40);
    G3O::RecordEntityReadVariantDuration(readVariants, G3O::EntityReadVariant::V83, 40, 45);
    ok &= Expect(readVariants.v83.calls == 2 && readVariants.v83.qpcTicks == 25,
                 "V83 entity reads should aggregate separately");
    ok &= Expect(readVariants.v63.calls == 1 && readVariants.v63.qpcTicks == 10,
                 "V63 entity reads should aggregate separately");

    G3O::EntityReadSuboperationMetrics readSuboperations{};
    G3O::RecordEntityReadSuboperationDuration(
        readSuboperations, G3O::EntityReadSuboperation::NodeRead, 10, 30);
    G3O::RecordEntityReadSuboperationDuration(
        readSuboperations, G3O::EntityReadSuboperation::AccessorPropertyStreamRead, 30, 40);
    G3O::RecordEntityReadSuboperationDuration(
        readSuboperations, G3O::EntityReadSuboperation::NodeRead, 40, 45);
    ok &= Expect(readSuboperations.nodeRead.calls == 2 && readSuboperations.nodeRead.qpcTicks == 25,
                 "node reads should aggregate separately");
    ok &= Expect(readSuboperations.accessorPropertyStreamRead.calls == 1
                 && readSuboperations.accessorPropertyStreamRead.qpcTicks == 10,
                 "accessor-property stream reads should aggregate separately");

    G3O::EntityReadSuboperationMetrics nestedAccessorReads{};
    G3O::RecordAccessorPropertyStreamReadDuration(nestedAccessorReads, 10, 30, 15, 1);
    G3O::RecordAccessorPropertyStreamReadDuration(nestedAccessorReads, 15, 20, 5, 2);
    ok &= Expect(nestedAccessorReads.accessorPropertyStreamRead.calls == 2
                 && nestedAccessorReads.accessorPropertyStreamRead.qpcTicks == 25,
                 "nested accessor reads should retain inclusive durations");
    ok &= Expect(nestedAccessorReads.accessorPropertyStreamReadExclusive.calls == 2
                 && nestedAccessorReads.accessorPropertyStreamReadExclusive.qpcTicks == 20
                 && nestedAccessorReads.accessorNestedCalls == 1,
                 "exclusive accessor time should remove child time and count nested calls");
    G3O::RecordAccessorPropertyStreamReadDuration(nestedAccessorReads, 20, 25, 0, 3, false);
    ok &= Expect(nestedAccessorReads.accessorPropertyStreamRead.calls == 3
                 && nestedAccessorReads.accessorPropertyStreamRead.qpcTicks == 30
                 && nestedAccessorReads.accessorPropertyStreamReadExclusive.calls == 2
                 && nestedAccessorReads.accessorNestedCalls == 2
                 && nestedAccessorReads.accessorExclusiveUnavailableCalls == 1,
                 "unavailable exclusive samples should preserve inclusive time and be counted");

    G3O::PropertyReadDispatchMetrics<2> dispatch{};
    G3O::RecordPropertyReadDispatchDuration(dispatch, 0x1010, 10, 30);
    G3O::RecordPropertyReadDispatchDuration(dispatch, 0x2020, 40, 50);
    G3O::RecordPropertyReadDispatchDuration(dispatch, 0x1010, 30, 35);
    G3O::RecordPropertyReadDispatchDuration(dispatch, 0, 35, 40);
    ok &= Expect(dispatch.entries[0].target == 0x1010
                 && dispatch.entries[0].metric.calls == 2
                 && dispatch.entries[0].metric.qpcTicks == 25,
                 "calls to the same virtual Read target should aggregate their inclusive duration");
    ok &= Expect(dispatch.entries[1].target == 0x2020
                 && dispatch.entries[1].metric.calls == 1
                 && dispatch.entries[1].metric.qpcTicks == 10,
                 "different virtual Read targets should remain separate");
    ok &= Expect(dispatch.unresolvedCalls == 1,
                 "missing virtual Read targets should be counted separately");

    G3O::PropertyReadDispatchMetrics<1> cappedDispatch{};
    G3O::RecordPropertyReadDispatchDuration(cappedDispatch, 0x1010, 1, 2);
    G3O::RecordPropertyReadDispatchDuration(cappedDispatch, 0x2020, 2, 3);
    ok &= Expect(cappedDispatch.overflowCalls == 1,
                 "dispatch target overflow should be counted instead of overwriting a target");

    G3O::PropertyReadDispatchPaths<2> dispatchPaths{};
    G3O::RecordPropertyReadDispatchDuration(
        dispatchPaths, G3O::PropertyReadDispatchPath::ReadObject, 0x3030, 10, 20);
    G3O::RecordPropertyReadDispatchDuration(
        dispatchPaths, G3O::PropertyReadDispatchPath::ReadNewObject, 0x4040, 30, 45);
    ok &= Expect(dispatchPaths.readObject.entries[0].target == 0x3030
                 && dispatchPaths.readObject.entries[0].metric.calls == 1
                 && dispatchPaths.readObject.entries[0].metric.qpcTicks == 10,
                 "existing-object dispatch should retain its own target and duration");
    ok &= Expect(dispatchPaths.readNewObject.entries[0].target == 0x4040
                 && dispatchPaths.readNewObject.entries[0].metric.calls == 1
                 && dispatchPaths.readNewObject.entries[0].metric.qpcTicks == 15,
                 "new-object dispatch should retain its own target and duration");

    void* fakeVtable[6]{};
    ok &= Expect(G3O::kPropertyObjectReadVtableSlot == 5,
                 "factory callsite dispatches Read through vtable offset 0x14");
    fakeVtable[G3O::kPropertyObjectReadVtableSlot] = reinterpret_cast<void*>(std::uintptr_t{0x1234});
    void* fakeObject[] = { fakeVtable };
    ok &= Expect(G3O::ResolvePropertyObjectReadTarget(fakeObject) == 0x1234,
                 "property-object dispatch target should come from virtual Read slot 5");
    ok &= Expect(G3O::ResolvePropertyObjectReadTarget(nullptr) == 0,
                 "missing property-object should resolve to no virtual target");

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
