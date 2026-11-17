#include "savegame_profiler.h"
#include "diagnostic_logger.h"
#include "config_manager.h"
#include "savegame_profiler_policy.h"
#include "hook_install_policy.h"
#include "layer_audit_metrics.h"
#include "timestamped_path.h"
#include "g3o_output_path.h"
#include "fast_pak_decompressor.h"
#include "property_cache_optimizer.h"
#include "vfs_cache_optimizer.h"
#include "frustum_database_optimizer.h"
#include "scene_node_optimizer.h"
#include "shadow_caster_optimizer.h"
#include "class_factory_optimizer.h"

#include "dynamic_layer_optimizer.h"
#include "pak_tracer.h"
#include "pak_read_at.h"
#include "profiler.h"
#include "layer_ring_arena.h"
#include "sampling_profiler.h"
#include <minhook.h>
#include <windows.h>
#include <stdio.h>
#include <string>
#include <map>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <intrin.h>
#include <atomic>

namespace G3O {

static std::string s_lastSaveName = "QuickSave";
static volatile LONG s_loadProfilerPathSequence = 0;

static LARGE_INTEGER s_freq = {};
static bool s_qpcFrequencyInitialized = false;

// -----------------------------------------------------------------------------
// ReadFile I/O Tracker
// -----------------------------------------------------------------------------
typedef BOOL (WINAPI* ReadFile_t)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
static ReadFile_t Real_ReadFile = nullptr;
static volatile LONG   s_readCalls = 0;
static volatile LONG64 s_readBytes = 0;
static volatile LONG64 s_readTimeUs = 0;
static std::atomic<bool> s_isLoadActive{ false };

bool IsSavegameLoadActive() noexcept {
    return s_isLoadActive.load(std::memory_order_relaxed);
}

static volatile bool   s_inPreLoad = false;
static volatile bool   s_skippedPreLoadActivate = false;

// ABI is intentionally restricted to the 32-bit Gothic 3 binaries.
static_assert(sizeof(void*) == 4, "Savegame profiler hooks require the x86 game ABI.");

#ifdef G3O_ENABLE_TELEMETRY
static inline bool IsTelemetryActive() { return true; }
#else
static inline bool IsTelemetryActive() { return GetConfig().enableSavegameAudit || GetConfig().enableProfiler; }
#endif

struct ContextAuditMetrics {
    LayerAuditMetric dynamicContextRead;    // eCEntityDynamicContext::Read (Engine.dll)
    LayerAuditMetric cpoReadFile;          // bCPOStream::ReadFile (SharedBase.dll)
    LayerAuditMetric createSpecificEntity; // eCEntityDynamicContext::CreateSpecificEntity (Engine.dll)
    LayerAuditMetric dynEntityRead;        // eCDynamicEntity::Read (Engine.dll RVA 0x2295C0)
    LayerAuditMetric entityRead;           // eCEntity::Read (Engine.dll RVA 0x2257F0)
    EntityReadVariantMetrics entityReadVariants;
    EntityReadSuboperationMetrics entityReadSuboperations;
    LayerAuditMetric readObject;           // bCPropertyObjectSingleton::ReadObject (SharedBase.dll)
    LayerAuditMetric readNewObject;         // bCPropertyObjectSingleton::ReadNewObject (SharedBase.dll)
    LayerAuditMetric propertyFactoryReadObject; // bCPropertyObjectFactory::ReadObject (SharedBase.dll)
    LayerAuditMetric propertyFactoryReadNewObject; // bCPropertyObjectFactory::ReadNewObject (SharedBase.dll)
    LayerAuditMetric cloneObjectInstance;  // bCPropertyObjectFactory::CloneObjectInstance (SharedBase.dll)
    LayerAuditMetric patchWithTemplate;    // eCEntity::PatchWithTemplate (Engine.dll RVA 0x224350)
    LayerAuditMetric readDependency;       // eCContextBase::ReadDependency (Engine.dll RVA 0x21A150)
    LayerAuditMetric childrenAvailable;    // eCEntity::ChildrenAvailable (Engine.dll)
};

static thread_local LayerAuditMetrics s_layerAuditMetrics{};
static thread_local ContextAuditMetrics s_contextAuditMetrics{};
// shortcut: tracks at most 256 targets; increase if overflow is reported.
static constexpr std::size_t kSysDynPropertyReadDispatchCapacity = 256;
static thread_local PropertyReadDispatchPaths<kSysDynPropertyReadDispatchCapacity> s_sysDynReadDispatchMetrics{};
struct PropertyCacheTelemetryCounts {
    LONG stringHits = 0;
    LONG stringMisses = 0;
    LONG idHits = 0;
    LONG idMisses = 0;
};
static thread_local PropertyCacheTelemetryCounts s_sysDynPropertyCacheTelemetry{};
static thread_local bool s_isSysDynSectorActive = false;
static thread_local bool s_collectSysDynReadDispatch = false;
static thread_local PvsPrefetchMetrics s_pvsPrefetchMetrics{};
static thread_local bool s_capturePvsPrefetchMetrics = false;

bool IsGotoLoadedPositionActive() noexcept {
    return s_capturePvsPrefetchMetrics;
}
static void* s_targetPvsProcess = nullptr;
static void* s_targetPvsWaitForEmptyThreadQueue = nullptr;

// Original function prototypes
using LayerCreateDataFn = std::int32_t (__thiscall*)(void*);
using DynamicLayerDoLoadDataFn = std::int32_t (__thiscall*)(void*, void*);
using DynamicLayerCreateEntityFn = void* (__thiscall*)(void*);
using EntityCreateFromTemplateFn = bool (__thiscall*)(void*, const void*);

using OnLoadDynamicContextFn = bool (__thiscall*)(void*, void*, void*, bool);
using DynamicContextReadFn = int (__thiscall*)(void*, void*);
using CPOStreamReadFileFn = bool (__thiscall*)(void*, void*);
using CreateSpecificEntityFn = void* (__thiscall*)(void*);
using DynamicEntityReadFn = int (__thiscall*)(void*, void*);
using EntityReadFn = int (__thiscall*)(void*, void*);
using EntityReadVersionedFn = int (__thiscall*)(void*, void*, std::uint16_t);
using EntityNodeReadFn = int (__thiscall*)(void*, void*);
using AccessorPropertyStreamReadFn = void* (__stdcall*)(void*, void*);
using PatchWithTemplateFn = bool (__thiscall*)(void*, const void*, bool);
using PropertyObjectReadObjectFn = bool (__thiscall*)(void*, void*, void*);
using PropertyObjectReadNewObjectFn = void* (__thiscall*)(void*, void*);
using PropertyFactoryReadObjectFn = bool (__thiscall*)(void*, void*, void*);
using PropertyFactoryReadNewObjectFn = void* (__thiscall*)(void*, void*);
using PropertyFactoryCloneObjectInstanceFn = void* (__thiscall*)(void*);
static PropertyObjectReadObjectFn Real_PropertyObjectReadObject = nullptr;
static PropertyObjectReadNewObjectFn Real_PropertyObjectReadNewObject = nullptr;
static PropertyFactoryReadObjectFn Real_PropertyFactoryReadObject = nullptr;
static PropertyFactoryReadNewObjectFn Real_PropertyFactoryReadNewObject = nullptr;
static PropertyFactoryCloneObjectInstanceFn Real_PropertyFactoryCloneObjectInstance = nullptr;

using ReadDependencyFn = int (__thiscall*)(void*, void*, void*);
using ChildrenAvailableFn = void (__thiscall*)(void*, bool);

static LayerCreateDataFn Real_LayerCreateData = nullptr;
static DynamicLayerDoLoadDataFn Real_DynamicLayerDoLoadData = nullptr;
static DynamicLayerCreateEntityFn Real_DynamicLayerCreateEntity = nullptr;
static EntityCreateFromTemplateFn Real_EntityCreateFromTemplate = nullptr;

static OnLoadDynamicContextFn Real_OnLoadDynamicContext = nullptr;
static DynamicContextReadFn Real_DynamicContextRead = nullptr;
static CPOStreamReadFileFn Real_CPOStreamReadFile = nullptr;
static CreateSpecificEntityFn Real_CreateSpecificEntity = nullptr;
static DynamicEntityReadFn Real_DynamicEntityRead = nullptr;
static EntityReadFn Real_EntityRead = nullptr;
static EntityReadVersionedFn Real_EntityReadV83 = nullptr;
static EntityReadVersionedFn Real_EntityReadV63 = nullptr;
static EntityNodeReadFn Real_EntityNodeRead = nullptr;
static AccessorPropertyStreamReadFn Real_AccessorPropertyStreamRead = nullptr;
static PatchWithTemplateFn Real_PatchWithTemplate = nullptr;
static ReadDependencyFn Real_ReadDependency = nullptr;
static ChildrenAvailableFn Real_ChildrenAvailable = nullptr;

static LPVOID s_metricsHookTargets[32] = {};
static int s_numMetricsHooks = 0;
static bool s_metricsHooksInitialized = false;

struct AccessorReadTimingFrame {
    std::uint64_t childTicks = 0;
};
static constexpr std::size_t kMaxAccessorReadTimingDepth = 128;
static thread_local AccessorReadTimingFrame s_accessorReadFrames[kMaxAccessorReadTimingDepth]{};
static thread_local std::uint64_t s_accessorReadDepth = 0;
static thread_local std::uint64_t s_accessorMaxDepth = 0;

static std::uint64_t BeginLayerMetric() {
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return static_cast<std::uint64_t>(t.QuadPart);
}

static void EndLayerMetric(LayerAuditMetric& metric, std::uint64_t start) {
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    RecordLayerAuditDuration(metric, start, static_cast<std::uint64_t>(t.QuadPart));
}

static double LayerMetricMs(std::uint64_t ticks) {
    return s_freq.QuadPart > 0
        ? static_cast<double>(ticks) * 1000.0 / static_cast<double>(s_freq.QuadPart)
        : 0.0;
}

template <std::size_t Capacity>
static void LogPropertyReadDispatchPath(
    const char* path, const PropertyReadDispatchMetrics<Capacity>& metrics) {
    std::uint64_t targetCount = 0;
    std::uint64_t recordedCalls = 0;
    std::uint64_t recordedTicks = 0;
    for (const auto& entry : metrics.entries) {
        if (!entry.target) continue;
        ++targetCount;
        recordedCalls += entry.metric.calls;
        recordedTicks += entry.metric.qpcTicks;
    }
    Log("G3O", "[PropertyReadDispatch] SysDyn path=%s virtual Read slot=%u targets=%llu recordedCalls=%llu recordedInclusive=%.3f ms overflowCalls=%llu unresolvedCalls=%llu; factory timings are inclusive",
        path, static_cast<unsigned>(kPropertyObjectReadVtableSlot),
        static_cast<unsigned long long>(targetCount),
        static_cast<unsigned long long>(recordedCalls), LayerMetricMs(recordedTicks),
        static_cast<unsigned long long>(metrics.overflowCalls),
        static_cast<unsigned long long>(metrics.unresolvedCalls));
    for (const auto& entry : metrics.entries) {
        if (!entry.target) continue;
        Log("G3O", "[PropertyReadDispatch] SysDyn path=%s target=%p factoryInclusive=%.3f ms calls=%llu",
            path, reinterpret_cast<void*>(entry.target), LayerMetricMs(entry.metric.qpcTicks),
            static_cast<unsigned long long>(entry.metric.calls));
    }
}

static LayerAuditMetrics SubtractLayerMetrics(
    const LayerAuditMetrics& after, const LayerAuditMetrics& before) {
    return DifferenceLayerAuditMetrics(after, before);
}

static std::int32_t __fastcall Hooked_LayerCreateData(void* self, void* /*edx*/) {
    if (!s_isLoadActive) return Real_LayerCreateData(self);
    const std::uint64_t start = BeginLayerMetric();
    const std::int32_t result = Real_LayerCreateData(self);
    EndLayerMetric(s_layerAuditMetrics.createData, start);
    return result;
}

static std::int32_t __fastcall Hooked_DynamicLayerDoLoadData(
    void* self, void* /*edx*/, void* streamReferenceSlot) {
    if (!s_isLoadActive) return Real_DynamicLayerDoLoadData(self, streamReferenceSlot);
#ifdef G3O_ENABLE_TELEMETRY
    // Sample only the heaviest layer (SysDyn) to avoid inflating the other 344 layers.
    const bool sampleThis = s_isSysDynSectorActive;
    if (sampleThis) SamplingProfiler_Start("SysDyn DoLoadData");
#endif
    const std::uint64_t start = BeginLayerMetric();
    const std::int32_t result = Real_DynamicLayerDoLoadData(self, streamReferenceSlot);
    EndLayerMetric(s_layerAuditMetrics.doLoadData, start);
#ifdef G3O_ENABLE_TELEMETRY
    if (sampleThis) SamplingProfiler_Stop();
#endif
    return result;
}

static void* __fastcall Hooked_DynamicLayerCreateEntity(void* self, void* /*edx*/) {
    if (!s_isLoadActive) return Real_DynamicLayerCreateEntity(self);
    const std::uint64_t start = BeginLayerMetric();
    void* result = Real_DynamicLayerCreateEntity(self);
    EndLayerMetric(s_layerAuditMetrics.createDynamicEntity, start);
    return result;
}

static bool __fastcall Hooked_EntityCreateFromTemplate(
    void* self, void* /*edx*/, const void* templateReferenceSlot) {
    if (!s_isLoadActive) return Real_EntityCreateFromTemplate(self, templateReferenceSlot);
    const std::uint64_t start = BeginLayerMetric();
    const bool result = Real_EntityCreateFromTemplate(self, templateReferenceSlot);
    EndLayerMetric(s_layerAuditMetrics.createFromTemplate, start);
    return result;
}

// -----------------------------------------------------------------------------
// Detailed Context Sub-phase Hooks
// -----------------------------------------------------------------------------
static bool __fastcall Hooked_OnLoadDynamicContext(void* self, void* /*edx*/, void* pStream, void* pLayer, bool directRead) {
    if (!s_isLoadActive) return Real_OnLoadDynamicContext(self, pStream, pLayer, directRead);

    const bool previousSysDynDispatch = s_collectSysDynReadDispatch;
    const bool collectSysDynMetrics = s_isSysDynSectorActive && !previousSysDynDispatch;
    const LONG stringHitsBefore = collectSysDynMetrics ? GetPropertyCacheStringHits() : 0;
    const LONG stringMissesBefore = collectSysDynMetrics ? GetPropertyCacheStringMisses() : 0;
    const LONG idHitsBefore = collectSysDynMetrics ? GetPropertyCacheIdHits() : 0;
    const LONG idMissesBefore = collectSysDynMetrics ? GetPropertyCacheIdMisses() : 0;
    s_collectSysDynReadDispatch = previousSysDynDispatch || collectSysDynMetrics;

    const ContextAuditMetrics before = s_contextAuditMetrics;
    const std::uint64_t previousAccessorMaxDepth = s_accessorMaxDepth;
    s_accessorMaxDepth = 0;
    const std::uint64_t start = BeginLayerMetric();

    const bool result = Real_OnLoadDynamicContext(self, pStream, pLayer, directRead);
    s_collectSysDynReadDispatch = previousSysDynDispatch;
    if (collectSysDynMetrics) {
        s_sysDynPropertyCacheTelemetry.stringHits += GetPropertyCacheStringHits() - stringHitsBefore;
        s_sysDynPropertyCacheTelemetry.stringMisses += GetPropertyCacheStringMisses() - stringMissesBefore;
        s_sysDynPropertyCacheTelemetry.idHits += GetPropertyCacheIdHits() - idHitsBefore;
        s_sysDynPropertyCacheTelemetry.idMisses += GetPropertyCacheIdMisses() - idMissesBefore;
    }

    const std::uint64_t elapsedTicks = BeginLayerMetric() - start;
    const double totalMs = LayerMetricMs(elapsedTicks);
    const std::uint64_t contextAccessorMaxDepth = s_accessorMaxDepth;
    s_accessorMaxDepth = previousAccessorMaxDepth > contextAccessorMaxDepth
        ? previousAccessorMaxDepth
        : contextAccessorMaxDepth;

    if (IsTelemetryActive() && totalMs >= 20.0) { // Log any layer taking >= 20 ms
        const char* filePath = "<unknown>";
        void* pContext = pLayer ? *reinterpret_cast<void**>(reinterpret_cast<char*>(pLayer) + 0x40) : nullptr;
        if (pContext) {
            const char** ppStr = reinterpret_cast<const char**>(reinterpret_cast<char*>(pContext) + 0x50);
            if (ppStr && *ppStr) filePath = *ppStr;
        }

        const double contextReadMs = LayerMetricMs(s_contextAuditMetrics.dynamicContextRead.qpcTicks - before.dynamicContextRead.qpcTicks);
        const uint64_t contextReadCalls = s_contextAuditMetrics.dynamicContextRead.calls - before.dynamicContextRead.calls;
        const double dynReadMs = LayerMetricMs(s_contextAuditMetrics.dynEntityRead.qpcTicks - before.dynEntityRead.qpcTicks);
        const uint64_t dynReadCalls = s_contextAuditMetrics.dynEntityRead.calls - before.dynEntityRead.calls;
        const double entReadMs = LayerMetricMs(s_contextAuditMetrics.entityRead.qpcTicks - before.entityRead.qpcTicks);
        const uint64_t entReadCalls = s_contextAuditMetrics.entityRead.calls - before.entityRead.calls;
        const EntityReadVariantMetrics readVariantDelta{
            DifferenceLayerAuditMetric(s_contextAuditMetrics.entityReadVariants.v63, before.entityReadVariants.v63),
            DifferenceLayerAuditMetric(s_contextAuditMetrics.entityReadVariants.v83, before.entityReadVariants.v83),
        };
        const double readV63Ms = LayerMetricMs(readVariantDelta.v63.qpcTicks);
        const double readV83Ms = LayerMetricMs(readVariantDelta.v83.qpcTicks);
        const EntityReadSuboperationMetrics readSuboperationDelta{
            DifferenceLayerAuditMetric(s_contextAuditMetrics.entityReadSuboperations.nodeRead, before.entityReadSuboperations.nodeRead),
            DifferenceLayerAuditMetric(s_contextAuditMetrics.entityReadSuboperations.accessorPropertyStreamRead, before.entityReadSuboperations.accessorPropertyStreamRead),
            DifferenceLayerAuditMetric(s_contextAuditMetrics.entityReadSuboperations.accessorPropertyStreamReadExclusive, before.entityReadSuboperations.accessorPropertyStreamReadExclusive),
            s_contextAuditMetrics.entityReadSuboperations.accessorNestedCalls - before.entityReadSuboperations.accessorNestedCalls,
            s_contextAuditMetrics.entityReadSuboperations.accessorExclusiveUnavailableCalls - before.entityReadSuboperations.accessorExclusiveUnavailableCalls,
        };
        const double entityNodeReadMs = LayerMetricMs(readSuboperationDelta.nodeRead.qpcTicks);
        const double accessorPropertyStreamReadMs = LayerMetricMs(readSuboperationDelta.accessorPropertyStreamRead.qpcTicks);
        const double accessorPropertyStreamReadExclusiveMs = LayerMetricMs(readSuboperationDelta.accessorPropertyStreamReadExclusive.qpcTicks);
        const std::uint64_t accessorNestedCalls = readSuboperationDelta.accessorNestedCalls;
        const std::uint64_t accessorExclusiveUnavailableCalls = readSuboperationDelta.accessorExclusiveUnavailableCalls;
        const std::uint64_t accessorMaxDepth = contextAccessorMaxDepth;
        const double readObjectMs = LayerMetricMs(s_contextAuditMetrics.readObject.qpcTicks - before.readObject.qpcTicks);
        const uint64_t readObjectCalls = s_contextAuditMetrics.readObject.calls - before.readObject.calls;
        const double propertyFactoryReadObjectMs = LayerMetricMs(s_contextAuditMetrics.propertyFactoryReadObject.qpcTicks - before.propertyFactoryReadObject.qpcTicks);
        const uint64_t propertyFactoryReadObjectCalls = s_contextAuditMetrics.propertyFactoryReadObject.calls - before.propertyFactoryReadObject.calls;
        const double readNewObjectMs = LayerMetricMs(s_contextAuditMetrics.readNewObject.qpcTicks - before.readNewObject.qpcTicks);
        const uint64_t readNewObjectCalls = s_contextAuditMetrics.readNewObject.calls - before.readNewObject.calls;
        const double propertyFactoryReadNewObjectMs = LayerMetricMs(s_contextAuditMetrics.propertyFactoryReadNewObject.qpcTicks - before.propertyFactoryReadNewObject.qpcTicks);
        const uint64_t propertyFactoryReadNewObjectCalls = s_contextAuditMetrics.propertyFactoryReadNewObject.calls - before.propertyFactoryReadNewObject.calls;
        const double cloneObjectInstanceMs = LayerMetricMs(s_contextAuditMetrics.cloneObjectInstance.qpcTicks - before.cloneObjectInstance.qpcTicks);
        const uint64_t cloneObjectInstanceCalls = s_contextAuditMetrics.cloneObjectInstance.calls - before.cloneObjectInstance.calls;
        const double readDependencyMs = LayerMetricMs(s_contextAuditMetrics.readDependency.qpcTicks - before.readDependency.qpcTicks);
        const uint64_t readDependencyCalls = s_contextAuditMetrics.readDependency.calls - before.readDependency.calls;
        const double patchWithTemplateMs = LayerMetricMs(s_contextAuditMetrics.patchWithTemplate.qpcTicks - before.patchWithTemplate.qpcTicks);
        const uint64_t patchWithTemplateCalls = s_contextAuditMetrics.patchWithTemplate.calls - before.patchWithTemplate.calls;
        const double childrenAvailableMs = LayerMetricMs(s_contextAuditMetrics.childrenAvailable.qpcTicks - before.childrenAvailable.qpcTicks);
        const uint64_t childrenAvailableCalls = s_contextAuditMetrics.childrenAvailable.calls - before.childrenAvailable.calls;
        const double createSpecificEntityMs = LayerMetricMs(s_contextAuditMetrics.createSpecificEntity.qpcTicks - before.createSpecificEntity.qpcTicks);
        const uint64_t createSpecificEntityCalls = s_contextAuditMetrics.createSpecificEntity.calls - before.createSpecificEntity.calls;

        Log("G3O",
            "[DynamicContextAudit] Layer %p ('%s') directRead=%d | OnLoad=%.2f ms"
            " | ContextRead=%.2f ms (%llu calls, inclusive)"
            " | DynamicEntityRead=%.2f ms (%llu calls, inclusive)"
            " | EntityRead=%.2f ms (%llu calls, inclusive)"
            " | ReadV83=%.2f ms (%llu calls, inclusive)"
            " | ReadV63=%.2f ms (%llu calls, inclusive)"
            " | eCNode::Read=%.2f ms (%llu calls, inclusive)"
            " | AccessorPropertyStreamRead=%.2f ms (%llu calls, inclusive)"
            " | AccessorPropertyStreamReadExclusive=%.2f ms (%llu calls)"
            " | AccessorNestedCalls=%llu | AccessorMaxDepth=%llu | AccessorExclusiveUnavailableCalls=%llu"
            " | PropertyObjectRead=%.2f ms (%llu calls, inclusive)"
            " | PropertyObjectReadNew=%.2f ms (%llu calls, inclusive)"
            " | PropertyFactoryReadObject=%.2f ms (%llu calls, inclusive)"
            " | PropertyFactoryReadNewObject=%.2f ms (%llu calls, inclusive)"
            " | CloneObjectInstance=%.2f ms (%llu calls, inclusive)"
            " | ReadDependency=%.2f ms (%llu calls, inclusive)"
            " | PatchWithTemplate(PropertyID)=%.2f ms (%llu calls, inclusive)"
            " | ChildrenAvailable=%.2f ms (%llu calls, inclusive)"
            " | CreateSpecificEntity=%.2f ms (%llu calls, inclusive)",
            pLayer, filePath, (int)directRead, totalMs,
            contextReadMs, contextReadCalls, dynReadMs, dynReadCalls, entReadMs, entReadCalls,
            readV83Ms, static_cast<unsigned long long>(readVariantDelta.v83.calls),
            readV63Ms, static_cast<unsigned long long>(readVariantDelta.v63.calls),
            entityNodeReadMs, static_cast<unsigned long long>(readSuboperationDelta.nodeRead.calls),
            accessorPropertyStreamReadMs, static_cast<unsigned long long>(readSuboperationDelta.accessorPropertyStreamRead.calls),
            accessorPropertyStreamReadExclusiveMs, static_cast<unsigned long long>(readSuboperationDelta.accessorPropertyStreamReadExclusive.calls),
            static_cast<unsigned long long>(accessorNestedCalls),
            static_cast<unsigned long long>(accessorMaxDepth),
            static_cast<unsigned long long>(accessorExclusiveUnavailableCalls),
            readObjectMs, readObjectCalls,
            readNewObjectMs, readNewObjectCalls,
            propertyFactoryReadObjectMs, propertyFactoryReadObjectCalls,
            propertyFactoryReadNewObjectMs, propertyFactoryReadNewObjectCalls,
            cloneObjectInstanceMs, cloneObjectInstanceCalls,
            readDependencyMs, readDependencyCalls,
            patchWithTemplateMs, patchWithTemplateCalls,
            childrenAvailableMs, childrenAvailableCalls,
            createSpecificEntityMs, createSpecificEntityCalls);
    }

    return result;
}

static int __fastcall Hooked_DynamicContextRead(void* self, void* /*edx*/, void* pStream) {
    if (!s_isLoadActive) return Real_DynamicContextRead(self, pStream);
    const std::uint64_t start = BeginLayerMetric();
    const int result = Real_DynamicContextRead(self, pStream);
    EndLayerMetric(s_contextAuditMetrics.dynamicContextRead, start);
    return result;
}

static bool __fastcall Hooked_CPOStreamReadFile(void* self, void* /*edx*/, void* pStream) {
    if (!s_isLoadActive) return Real_CPOStreamReadFile(self, pStream);
    const std::uint64_t start = BeginLayerMetric();
    const bool result = Real_CPOStreamReadFile(self, pStream);
    EndLayerMetric(s_contextAuditMetrics.cpoReadFile, start);
    return result;
}

static void* __fastcall Hooked_CreateSpecificEntity(void* self, void* /*edx*/) {
    if (!s_isLoadActive) return Real_CreateSpecificEntity(self);
    const std::uint64_t start = BeginLayerMetric();
    void* result = Real_CreateSpecificEntity(self);
    EndLayerMetric(s_contextAuditMetrics.createSpecificEntity, start);
    return result;
}

static int __fastcall Hooked_DynamicEntityRead(void* self, void* /*edx*/, void* pStream) {
    if (!s_isLoadActive) return Real_DynamicEntityRead(self, pStream);
    const std::uint64_t start = BeginLayerMetric();
    const int result = Real_DynamicEntityRead(self, pStream);
    EndLayerMetric(s_contextAuditMetrics.dynEntityRead, start);
    return result;
}

static int __fastcall Hooked_EntityRead(void* self, void* /*edx*/, void* pStream) {
    if (!s_isLoadActive) return Real_EntityRead(self, pStream);
    const std::uint64_t start = BeginLayerMetric();
    const int result = Real_EntityRead(self, pStream);
    EndLayerMetric(s_contextAuditMetrics.entityRead, start);
    return result;
}

static int __fastcall Hooked_EntityReadV83(
    void* self, void* /*edx*/, void* pStream, std::uint16_t version) {
    if (!s_isLoadActive) return Real_EntityReadV83(self, pStream, version);
    const std::uint64_t start = BeginLayerMetric();
    const int result = Real_EntityReadV83(self, pStream, version);
    RecordEntityReadVariantDuration(
        s_contextAuditMetrics.entityReadVariants, EntityReadVariant::V83,
        start, BeginLayerMetric());
    return result;
}

static int __fastcall Hooked_EntityReadV63(
    void* self, void* /*edx*/, void* pStream, std::uint16_t version) {
    if (!s_isLoadActive) return Real_EntityReadV63(self, pStream, version);
    const std::uint64_t start = BeginLayerMetric();
    const int result = Real_EntityReadV63(self, pStream, version);
    RecordEntityReadVariantDuration(
        s_contextAuditMetrics.entityReadVariants, EntityReadVariant::V63,
        start, BeginLayerMetric());
    return result;
}

static int __fastcall Hooked_EntityNodeRead(void* self, void* /*edx*/, void* pStream) {
    if (!s_isLoadActive) return Real_EntityNodeRead(self, pStream);
    const std::uint64_t start = BeginLayerMetric();
    const int result = Real_EntityNodeRead(self, pStream);
    RecordEntityReadSuboperationDuration(
        s_contextAuditMetrics.entityReadSuboperations, EntityReadSuboperation::NodeRead,
        start, BeginLayerMetric());
    return result;
}

static void* __stdcall Hooked_AccessorPropertyStreamRead(void* pStream, void* pAccessor) {
    if (!s_isLoadActive) return Real_AccessorPropertyStreamRead(pStream, pAccessor);

    const std::uint64_t depth = ++s_accessorReadDepth;
    if (depth > s_accessorMaxDepth) s_accessorMaxDepth = depth;
    const bool depthTracked = depth <= kMaxAccessorReadTimingDepth;
    if (depthTracked) s_accessorReadFrames[depth - 1].childTicks = 0;

    const std::uint64_t start = BeginLayerMetric();
    void* const result = Real_AccessorPropertyStreamRead(pStream, pAccessor);
    const std::uint64_t end = BeginLayerMetric();
    const std::uint64_t inclusiveTicks = end >= start ? end - start : 0;
    const std::uint64_t childTicks = depthTracked
        ? s_accessorReadFrames[depth - 1].childTicks
        : 0;
    const bool exclusiveAvailable = depthTracked && childTicks <= inclusiveTicks;
    const std::uint64_t exclusiveTicks = exclusiveAvailable ? inclusiveTicks - childTicks : 0;

    RecordAccessorPropertyStreamReadDuration(
        s_contextAuditMetrics.entityReadSuboperations,
        start, end, exclusiveTicks, depth, exclusiveAvailable);
    if (depth > 1 && depth - 1 <= kMaxAccessorReadTimingDepth) {
        s_accessorReadFrames[depth - 2].childTicks += inclusiveTicks;
    }
    s_accessorReadDepth = depth - 1;
    return result;
}

static bool __fastcall Hooked_PropertyObjectReadObject(void* self, void* /*edx*/, void* pStream, void* pObject) {
    if (!s_isLoadActive) return Real_PropertyObjectReadObject(self, pStream, pObject);
    const std::uint64_t start = BeginLayerMetric();
    const bool res = Real_PropertyObjectReadObject(self, pStream, pObject);
    EndLayerMetric(s_contextAuditMetrics.readObject, start);
    return res;
}

static bool __fastcall Hooked_PropertyFactoryReadObject(void* self, void* /*edx*/, void* pStream, void* pObject) {
    if (!s_isLoadActive) return Real_PropertyFactoryReadObject(self, pStream, pObject);
    const std::uintptr_t readTarget = s_collectSysDynReadDispatch
        ? ResolvePropertyObjectReadTarget(pObject)
        : 0;
    const std::uint64_t start = BeginLayerMetric();
    const bool result = Real_PropertyFactoryReadObject(self, pStream, pObject);
    if (s_collectSysDynReadDispatch) {
        const std::uint64_t end = BeginLayerMetric();
        RecordLayerAuditDuration(s_contextAuditMetrics.propertyFactoryReadObject, start, end);
        RecordPropertyReadDispatchDuration(
            s_sysDynReadDispatchMetrics, PropertyReadDispatchPath::ReadObject, readTarget, start, end);
    } else {
        EndLayerMetric(s_contextAuditMetrics.propertyFactoryReadObject, start);
    }
    return result;
}

static void* __fastcall Hooked_PropertyObjectReadNewObject(
    void* self, void* /*edx*/, void* pStream) {
    if (!s_isLoadActive) return Real_PropertyObjectReadNewObject(self, pStream);
    const std::uint64_t start = BeginLayerMetric();
    void* const result = Real_PropertyObjectReadNewObject(self, pStream);
    EndLayerMetric(s_contextAuditMetrics.readNewObject, start);
    return result;
}

static void* __fastcall Hooked_PropertyFactoryReadNewObject(
    void* self, void* /*edx*/, void* pStream) {
    if (!s_isLoadActive) return Real_PropertyFactoryReadNewObject(self, pStream);
    const std::uint64_t start = BeginLayerMetric();
    void* const result = Real_PropertyFactoryReadNewObject(self, pStream);
    if (s_collectSysDynReadDispatch) {
        const std::uint64_t end = BeginLayerMetric();
        RecordLayerAuditDuration(s_contextAuditMetrics.propertyFactoryReadNewObject, start, end);
        RecordPropertyReadDispatchDuration(
            s_sysDynReadDispatchMetrics, PropertyReadDispatchPath::ReadNewObject,
            ResolvePropertyObjectReadTarget(result), start, end);
    } else {
        EndLayerMetric(s_contextAuditMetrics.propertyFactoryReadNewObject, start);
    }
    return result;
}

static void* __fastcall Hooked_PropertyFactoryCloneObjectInstance(void* self, void* /*edx*/) {
    if (!s_isLoadActive) return Real_PropertyFactoryCloneObjectInstance(self);
    const std::uint64_t start = BeginLayerMetric();
    void* const result = Real_PropertyFactoryCloneObjectInstance(self);
    EndLayerMetric(s_contextAuditMetrics.cloneObjectInstance, start);
    return result;
}

static bool __fastcall Hooked_PatchWithTemplate(void* self, void* /*edx*/, const void* pGuid, bool flag) {
    if (!s_isLoadActive) return Real_PatchWithTemplate ? Real_PatchWithTemplate(self, pGuid, flag) : false;
    const std::uint64_t start = BeginLayerMetric();
    const bool result = Real_PatchWithTemplate ? Real_PatchWithTemplate(self, pGuid, flag) : false;
    EndLayerMetric(s_contextAuditMetrics.patchWithTemplate, start);
    return result;
}

static int __fastcall Hooked_ReadDependency(void* self, void* /*edx*/, void* pEntities, void* pStream) {
    if (!s_isLoadActive) return Real_ReadDependency(self, pEntities, pStream);
    const std::uint64_t start = BeginLayerMetric();
    const int result = Real_ReadDependency(self, pEntities, pStream);
    EndLayerMetric(s_contextAuditMetrics.readDependency, start);
    return result;
}

static void __fastcall Hooked_ChildrenAvailable(void* self, void* /*edx*/, bool bAvailable) {
    if (!s_isLoadActive) {
        Real_ChildrenAvailable(self, bAvailable);
        return;
    }
    const std::uint64_t start = BeginLayerMetric();
    Real_ChildrenAvailable(self, bAvailable);
    EndLayerMetric(s_contextAuditMetrics.childrenAvailable, start);
}

static bool InstallHookTarget(LPVOID target, LPVOID detour, LPVOID* original, const char* name) {
    if (!target) {
        Log("G3O", "[LayerAudit] Target address is null for: %s", name);
        return false;
    }
    if (s_numMetricsHooks >= 19) {
        Log("G3O", "[LayerAudit] Hook slots exhausted for: %s", name);
        return false;
    }
    const MH_STATUS status = MH_CreateHook(target, detour, original);
    if (status != MH_OK) {
        Log("G3O", "[LayerAudit] MH_CreateHook failed for %s (status=%d)", name, (int)status);
        return false;
    }
    s_metricsHookTargets[s_numMetricsHooks++] = target;
    Log("G3O", "[LayerAudit] Hook registered: %s at %p", name, target);
    return true;
}

static void RemoveCreatedMetricsHooks() {
    for (int i = 0; i < s_numMetricsHooks; ++i) {
        if (s_metricsHookTargets[i]) {
            MH_DisableHook(s_metricsHookTargets[i]);
            MH_RemoveHook(s_metricsHookTargets[i]);
            s_metricsHookTargets[i] = nullptr;
        }
    }
    s_numMetricsHooks = 0;

    Real_LayerCreateData = nullptr;
    Real_DynamicLayerDoLoadData = nullptr;
    Real_DynamicLayerCreateEntity = nullptr;
    Real_EntityCreateFromTemplate = nullptr;
    Real_OnLoadDynamicContext = nullptr;
    Real_DynamicContextRead = nullptr;
    Real_CPOStreamReadFile = nullptr;
    Real_CreateSpecificEntity = nullptr;
    Real_DynamicEntityRead = nullptr;
    Real_EntityRead = nullptr;
    Real_EntityReadV83 = nullptr;
    Real_EntityReadV63 = nullptr;
    Real_EntityNodeRead = nullptr;
    Real_AccessorPropertyStreamRead = nullptr;
    Real_PatchWithTemplate = nullptr;
    Real_PropertyObjectReadObject = nullptr;
    Real_PropertyObjectReadNewObject = nullptr;
    Real_PropertyFactoryReadObject = nullptr;
    Real_PropertyFactoryReadNewObject = nullptr;
    Real_PropertyFactoryCloneObjectInstance = nullptr;
    Real_ReadDependency = nullptr;
    Real_ChildrenAvailable = nullptr;

    s_metricsHooksInitialized = false;
}

static bool InitMetricsHooks() {
    if (s_metricsHooksInitialized) return true;
    if (!IsTelemetryActive()) return true;

    if (!s_qpcFrequencyInitialized) {
        if (!QueryPerformanceFrequency(&s_freq) || s_freq.QuadPart <= 0) {
            Log("G3O", "[LayerAudit] QueryPerformanceFrequency failed; layer timing hooks not installed.");
            return false;
        }
        s_qpcFrequencyInitialized = true;
    }

    HMODULE hGame = GetModuleHandleA("Game.dll");
    if (!hGame) {
        Log("G3O", "[LayerAudit] Game.dll unavailable; layer timing hooks not installed.");
        return false;
    }
    HMODULE hEngine = GetModuleHandleA("Engine.dll");
    if (!hEngine) {
        Log("G3O", "[LayerAudit] Engine.dll unavailable; context timing hooks not installed.");
        return false;
    }
    HMODULE hSharedBase = GetModuleHandleA("SharedBase.dll");
    if (!hSharedBase) {
        Log("G3O", "[LayerAudit] SharedBase.dll unavailable; property object read timing hooks not installed.");
    }

    bool created = true;
    created = InstallHookTarget(
        reinterpret_cast<LPVOID>(GetProcAddress(hGame, "?CreateData@gCLayerBase@@MAE?AW4eEResult@eCProcessibleElement@@XZ")),
        reinterpret_cast<LPVOID>(Hooked_LayerCreateData),
        reinterpret_cast<LPVOID*>(&Real_LayerCreateData),
        "gCLayerBase::CreateData") && created;
    created = InstallHookTarget(
        reinterpret_cast<LPVOID>(GetProcAddress(hGame, "?DoLoadData@gCDynamicLayer@@MAE?AW4eEResult@eCProcessibleElement@@AAVbCIStream@@@Z")),
        reinterpret_cast<LPVOID>(Hooked_DynamicLayerDoLoadData),
        reinterpret_cast<LPVOID*>(&Real_DynamicLayerDoLoadData),
        "gCDynamicLayer::DoLoadData") && created;
    created = InstallHookTarget(
        reinterpret_cast<LPVOID>(GetProcAddress(hGame, "?OnLoadDynamicContext@gCWorld@@IAE_NAAVbCIStream@@AAVgCDynamicLayer@@_N@Z")),
        reinterpret_cast<LPVOID>(Hooked_OnLoadDynamicContext),
        reinterpret_cast<LPVOID*>(&Real_OnLoadDynamicContext),
        "gCWorld::OnLoadDynamicContext") && created;
    created = InstallHookTarget(
        reinterpret_cast<LPVOID>(GetProcAddress(hEngine, "?Read@eCEntityDynamicContext@@UAE?AW4bEResult@@AAVbCIStream@@@Z")),
        reinterpret_cast<LPVOID>(Hooked_DynamicContextRead),
        reinterpret_cast<LPVOID*>(&Real_DynamicContextRead),
        "eCEntityDynamicContext::Read") && created;
    created = InstallHookTarget(
        reinterpret_cast<LPVOID>(GetProcAddress(hEngine, "?Read@eCDynamicEntity@@UAE?AW4bEResult@@AAVbCIStream@@@Z")),
        reinterpret_cast<LPVOID>(Hooked_DynamicEntityRead),
        reinterpret_cast<LPVOID*>(&Real_DynamicEntityRead),
        "eCDynamicEntity::Read") && created;
    created = InstallHookTarget(
        reinterpret_cast<LPVOID>(GetProcAddress(hEngine, "?Read@eCEntity@@UAE?AW4bEResult@@AAVbCIStream@@@Z")),
        reinterpret_cast<LPVOID>(Hooked_EntityRead),
        reinterpret_cast<LPVOID*>(&Real_EntityRead),
        "eCEntity::Read") && created;
    created = InstallHookTarget(
        reinterpret_cast<LPVOID>(GetProcAddress(hEngine, "?ReadV83@eCEntity@@MAE?AW4bEResult@@AAVbCIStream@@G@Z")),
        reinterpret_cast<LPVOID>(Hooked_EntityReadV83),
        reinterpret_cast<LPVOID*>(&Real_EntityReadV83),
        "eCEntity::ReadV83") && created;
    created = InstallHookTarget(
        reinterpret_cast<LPVOID>(GetProcAddress(hEngine, "?ReadV63@eCEntity@@MAE?AW4bEResult@@AAVbCIStream@@G@Z")),
        reinterpret_cast<LPVOID>(Hooked_EntityReadV63),
        reinterpret_cast<LPVOID*>(&Real_EntityReadV63),
        "eCEntity::ReadV63") && created;
    created = InstallHookTarget(
        reinterpret_cast<LPVOID>(GetProcAddress(hEngine, "?Read@eCNode@@UAE?AW4bEResult@@AAVbCIStream@@@Z")),
        reinterpret_cast<LPVOID>(Hooked_EntityNodeRead),
        reinterpret_cast<LPVOID*>(&Real_EntityNodeRead),
        "eCNode::Read") && created;

    if (hSharedBase) {
        (void)InstallHookTarget(
            reinterpret_cast<LPVOID>(GetProcAddress(hSharedBase, "?ReadObject@bCPropertyObjectSingleton@@QAE_NAAVbCIStream@@AAVbCPropertyObjectBase@@@Z")),
            reinterpret_cast<LPVOID>(Hooked_PropertyObjectReadObject),
            reinterpret_cast<LPVOID*>(&Real_PropertyObjectReadObject),
            "bCPropertyObjectSingleton::ReadObject");
        (void)InstallHookTarget(
            reinterpret_cast<LPVOID>(GetProcAddress(hSharedBase, "?ReadObject@bCPropertyObjectFactory@@IAE_NAAVbCIStream@@AAVbCPropertyObjectBase@@@Z")),
            reinterpret_cast<LPVOID>(Hooked_PropertyFactoryReadObject),
            reinterpret_cast<LPVOID*>(&Real_PropertyFactoryReadObject),
            "bCPropertyObjectFactory::ReadObject");
        (void)InstallHookTarget(
            reinterpret_cast<LPVOID>(GetProcAddress(hSharedBase, "?ReadNewObject@bCPropertyObjectSingleton@@QAEPAVbCPropertyObjectBase@@AAVbCIStream@@@Z")),
            reinterpret_cast<LPVOID>(Hooked_PropertyObjectReadNewObject),
            reinterpret_cast<LPVOID*>(&Real_PropertyObjectReadNewObject),
            "bCPropertyObjectSingleton::ReadNewObject");
        (void)InstallHookTarget(
            reinterpret_cast<LPVOID>(GetProcAddress(hSharedBase, "?ReadNewObject@bCPropertyObjectFactory@@IAEPAVbCPropertyObjectBase@@AAVbCIStream@@@Z")),
            reinterpret_cast<LPVOID>(Hooked_PropertyFactoryReadNewObject),
            reinterpret_cast<LPVOID*>(&Real_PropertyFactoryReadNewObject),
            "bCPropertyObjectFactory::ReadNewObject");
        (void)InstallHookTarget(
            reinterpret_cast<LPVOID>(GetProcAddress(hSharedBase, "?CloneObjectInstance@bCPropertyObjectFactory@@IBEPAVbCPropertyObjectBase@@XZ")),
            reinterpret_cast<LPVOID>(Hooked_PropertyFactoryCloneObjectInstance),
            reinterpret_cast<LPVOID*>(&Real_PropertyFactoryCloneObjectInstance),
            "bCPropertyObjectFactory::CloneObjectInstance");
        (void)InstallHookTarget(
            reinterpret_cast<LPVOID>(GetProcAddress(hSharedBase, "??5@YGAAVbCIStream@@AAV0@AAVbCAccessorPropertyObject@@@Z")),
            reinterpret_cast<LPVOID>(Hooked_AccessorPropertyStreamRead),
            reinterpret_cast<LPVOID*>(&Real_AccessorPropertyStreamRead),
            "operator>>(bCIStream&, bCAccessorPropertyObject&)");
    }
    (void)InstallHookTarget(
        reinterpret_cast<LPVOID>(GetProcAddress(hEngine, "?ReadDependency@eCContextBase@@IAE?AW4bEResult@@AAVbCIStream@@AAV?$bTValArray@PAVeCNode@@@@@Z")),
        reinterpret_cast<LPVOID>(Hooked_ReadDependency),
        reinterpret_cast<LPVOID*>(&Real_ReadDependency),
        "eCContextBase::ReadDependency");
    (void)InstallHookTarget(
        reinterpret_cast<LPVOID>(GetProcAddress(hEngine, "?PatchWithTemplate@eCEntity@@QAE_NABVbCPropertyID@@_N@Z")),
        reinterpret_cast<LPVOID>(Hooked_PatchWithTemplate),
        reinterpret_cast<LPVOID*>(&Real_PatchWithTemplate),
        "eCEntity::PatchWithTemplate(PropertyID)");
    (void)InstallHookTarget(
        reinterpret_cast<LPVOID>(GetProcAddress(hEngine, "?ChildrenAvailable@eCEntity@@UAEX_N@Z")),
        reinterpret_cast<LPVOID>(Hooked_ChildrenAvailable),
        reinterpret_cast<LPVOID*>(&Real_ChildrenAvailable),
        "eCEntity::ChildrenAvailable");
    (void)InstallHookTarget(
        reinterpret_cast<LPVOID>(GetProcAddress(hEngine, "?CreateSpecificEntity@eCEntityDynamicContext@@UAEPAVeCDynamicEntity@@XZ")),
        reinterpret_cast<LPVOID>(Hooked_CreateSpecificEntity),
        reinterpret_cast<LPVOID*>(&Real_CreateSpecificEntity),
        "eCEntityDynamicContext::CreateSpecificEntity");

    if (!created) {
        Log("G3O", "[LayerAudit] Layer timing hook installation failed; removing all registered hooks.");
        RemoveCreatedMetricsHooks();
        return false;
    }

    for (int i = 0; i < s_numMetricsHooks; ++i) {
        const MH_STATUS status = MH_EnableHook(s_metricsHookTargets[i]);
        if (status != MH_OK) {
            Log("G3O", "[LayerAudit] MH_EnableHook failed for hook index %d (status=%d)", i, (int)status);
            RemoveCreatedMetricsHooks();
            return false;
        }
    }

    s_metricsHooksInitialized = true;
    Log("G3O", "[LayerAudit] %d layer/context QPC hooks enabled; call times are inclusive except separately labeled accessor exclusive time (frequency=%lld Hz).",
        s_numMetricsHooks, s_freq.QuadPart);
    return true;
}

static void ShutdownMetricsHooks() {
    bool hadHooks = (s_numMetricsHooks > 0);
    bool cleanupSucceeded = true;
    for (int i = 0; i < s_numMetricsHooks; ++i) {
        LPVOID target = s_metricsHookTargets[i];
        if (!target) continue;

        const MH_STATUS disableStatus = MH_DisableHook(target);
        if (disableStatus != MH_OK && disableStatus != MH_ERROR_DISABLED) {
            Log("G3O", "[LayerAudit] MH_DisableHook failed for index %d (status=%d)", i, (int)disableStatus);
            cleanupSucceeded = false;
        }

        const MH_STATUS removeStatus = MH_RemoveHook(target);
        if (removeStatus != MH_OK && removeStatus != MH_ERROR_NOT_CREATED) {
            Log("G3O", "[LayerAudit] MH_RemoveHook failed for index %d (status=%d)", i, (int)removeStatus);
            cleanupSucceeded = false;
        }
        s_metricsHookTargets[i] = nullptr;
    }

    if (cleanupSucceeded) {
        s_numMetricsHooks = 0;
        Real_LayerCreateData = nullptr;
        Real_DynamicLayerDoLoadData = nullptr;
        Real_DynamicLayerCreateEntity = nullptr;
        Real_EntityCreateFromTemplate = nullptr;
        Real_OnLoadDynamicContext = nullptr;
        Real_DynamicContextRead = nullptr;
        Real_CPOStreamReadFile = nullptr;
        Real_CreateSpecificEntity = nullptr;
        Real_DynamicEntityRead = nullptr;
        Real_EntityRead = nullptr;
        Real_EntityReadV83 = nullptr;
        Real_EntityReadV63 = nullptr;
        Real_EntityNodeRead = nullptr;
        Real_AccessorPropertyStreamRead = nullptr;
        Real_PatchWithTemplate = nullptr;
        Real_PropertyObjectReadObject = nullptr;
        Real_PropertyObjectReadNewObject = nullptr;
        Real_PropertyFactoryReadObject = nullptr;
        Real_PropertyFactoryReadNewObject = nullptr;
        Real_PropertyFactoryCloneObjectInstance = nullptr;
        Real_ReadDependency = nullptr;
        Real_ChildrenAvailable = nullptr;
        s_metricsHooksInitialized = false;
    }
    if (hadHooks) Log("G3O", "[LayerAudit] Metrics hooks shutdown completed.");
}



static BOOL WINAPI Hooked_ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD pRead, LPOVERLAPPED ov) {
    if (IsInternalPakRead()) {
        return Real_ReadFile(h, buf, n, pRead, ov);
    }
    const Config& cfg = GetConfig();
    if (!s_isLoadActive) {
        return Real_ReadFile(h, buf, n, pRead, ov);
    }
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    BOOL r = Real_ReadFile(h, buf, n, pRead, ov);
    QueryPerformanceCounter(&t1);
    if (cfg.enableSavegameAudit) {
        if (pRead && r) {
            InterlockedExchangeAdd64(&s_readBytes, (LONG64)*pRead);
        }
        InterlockedIncrement(&s_readCalls);
        const LONG64 us = (s_freq.QuadPart > 0) ? ((t1.QuadPart - t0.QuadPart) * 1000000LL / s_freq.QuadPart) : 0;
        InterlockedExchangeAdd64(&s_readTimeUs, us);
    }
    if (ShouldTracePakPrefetch(cfg)) {
        PakTracer_RecordRead(h, buf, n, (pRead && r) ? *pRead : 0, ov);
    }
    return r;
}

// -----------------------------------------------------------------------------
// Sub-Phase Measurements
// -----------------------------------------------------------------------------
struct PhaseStats {
    double durationSec = 0.0;
    LONG   pakBlocks = 0;
    double pakOutMb = 0.0;
    double pakTimeSec = 0.0;
    LONG   readCalls = 0;
    double readMb = 0.0;
    double readTimeSec = 0.0;
};

static LARGE_INTEGER s_tHUDStart = {};
static LARGE_INTEGER s_tWorldStart = {};

static PhaseStats s_statHeader = {};
static PhaseStats s_statSessionStop = {};
static PhaseStats s_statClearEntities = {};
static PhaseStats s_statUnloadCaches = {};
static PhaseStats s_statLoadGameWorld = {};
static PhaseStats s_statGotoPos = {};
static PhaseStats s_statSessionStart = {};
static PhaseStats s_statWorldActivate = {};

static volatile LONG   s_deactivateCount = 0;
static volatile LONG64 s_deactivateTimeUs = 0;
static volatile LONG64 s_worldActivateElemTimeUs = 0;
static volatile LONG64 s_forceROIUpdateTimeUs = 0;
static volatile LONG   s_sectorActivateCount = 0;
static volatile LONG64 s_sectorActivateTotalUs = 0;

struct SectorTimingRecord {
    std::string name;
    double durationMs;
};
static std::vector<SectorTimingRecord> s_sectorTimings;
static CRITICAL_SECTION s_sectorTimingCs;
static bool s_sectorTimingCsInit = false;

static const char* SafeGetSectorName(void* pSector) {
    if (!pSector) return "(null)";
    __try {
        const char* str = *(const char**)((const BYTE*)pSector + 0x14);
        if (!str || !*str) return "(unnamed)";
        return str;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "(invalid_ptr)";
    }
}

static void RecordSectorTiming(const char* name, double ms) {
    if (ms < 0.2) return;
    if (!s_sectorTimingCsInit) {
        InitializeCriticalSection(&s_sectorTimingCs);
        s_sectorTimingCsInit = true;
    }
    EnterCriticalSection(&s_sectorTimingCs);
    s_sectorTimings.push_back({ name ? name : "unknown", ms });
    LeaveCriticalSection(&s_sectorTimingCs);
}

static double s_preWorldTotalSec = 0.0;
static volatile LONG   s_cellsProcessed = 0;
static volatile LONG64 s_hierarchyTimeUs = 0;

static inline void SnapshotPhase(PhaseStats& st, LARGE_INTEGER t0, LARGE_INTEGER t1,
                                 LONG pakCount0, LONG64 pakOut0, LONG read0, LONG64 readBytes0,
                                 double pakTimeMs0 = 0.0, LONG64 readTimeUs0 = 0) {
    st.durationSec = (double)(t1.QuadPart - t0.QuadPart) / (double)s_freq.QuadPart;
    st.pakBlocks = GetPakDecompCount() - pakCount0;
    st.pakOutMb = (double)(GetPakDecompOutBytes() - pakOut0) / (1024.0 * 1024.0);
    st.pakTimeSec = (GetPakDecompTimeMs() - pakTimeMs0) / 1000.0;
    st.readCalls = s_readCalls - read0;
    st.readMb = (double)(s_readBytes - readBytes0) / (1024.0 * 1024.0);
    st.readTimeSec = (double)(s_readTimeUs - readTimeUs0) / 1000000.0;
}

static void LogLoadReport(double totalSec) {
    LONG rc = s_readCalls;
    double rbMb = (double)s_readBytes / (1024.0 * 1024.0);
    double rSec = (double)s_readTimeUs / 1000000.0;
    const auto& config = GetConfig();

    LONG pakCount = GetPakDecompCount();
    double pakInMb = (double)GetPakDecompInBytes() / (1024.0 * 1024.0);
    double pakOutMb = (double)GetPakDecompOutBytes() / (1024.0 * 1024.0);
    double pakSec = GetPakDecompTimeMs() / 1000.0;

    LONG saveCount = GetSavegameDecompCount();
    double saveInMb = (double)GetSavegameDecompInBytes() / (1024.0 * 1024.0);
    double saveOutMb = (double)GetSavegameDecompOutBytes() / (1024.0 * 1024.0);
    double saveSec = GetSavegameDecompTimeMs() / 1000.0;

    LONG strHits = GetPropertyCacheStringHits();
    LONG strMiss = GetPropertyCacheStringMisses();
    LONG idHits = GetPropertyCacheIdHits();
    LONG idMiss = GetPropertyCacheIdMisses();
    LONG totalProps = strHits + strMiss + idHits + idMiss;
    double propHitRate = totalProps ? (100.0 * (strHits + idHits) / totalProps) : 0.0;

    double deactSec = (double)s_deactivateTimeUs / 1000000.0;
    double secTotalSec = (double)s_sectorActivateTotalUs / 1000000.0;
    double worldActElemSec = (double)s_worldActivateElemTimeUs / 1000000.0;
    double roiSec = (double)s_forceROIUpdateTimeUs / 1000000.0;

    double preKnown = s_statHeader.durationSec + s_statSessionStop.durationSec + 
                      s_statClearEntities.durationSec + s_statUnloadCaches.durationSec +
                      deactSec + worldActElemSec + roiSec;
    double preOther = s_preWorldTotalSec - preKnown;
    if (preOther < 0.0) preOther = 0.0;

    double postKnown = s_statGotoPos.durationSec + s_statSessionStart.durationSec;
    double postTotal = totalSec - (s_preWorldTotalSec + s_statLoadGameWorld.durationSec);
    if (postTotal < 0.0) postTotal = 0.0;
    double postOther = postTotal - postKnown;
    if (postOther < 0.0) postOther = 0.0;

    double gotoPakSec = (s_statGotoPos.pakTimeSec > 0.0) ? s_statGotoPos.pakTimeSec
                      : ((pakOutMb > 0.0) ? (s_statGotoPos.pakOutMb / pakOutMb) * pakSec : 0.0);
    double gotoReadSec = (s_statGotoPos.readTimeSec > 0.0) ? s_statGotoPos.readTimeSec
                       : ((rbMb > 0.0) ? (s_statGotoPos.readMb / rbMb) * rSec : 0.0);
    double pvsSec = (double)s_hierarchyTimeUs / 1000000.0;
    double gotoSetupSec = s_statGotoPos.durationSec - (gotoPakSec + gotoReadSec + pvsSec);
    if (gotoSetupSec < 0.0) gotoSetupSec = 0.0;

    Log("G3O", "================================================================================");
    Log("G3O", "[LoadProfile] === DETAILED SAVEGAME LOAD BENCHMARK (Total: %.3f s) ===", totalSec);
    Log("G3O", "--------------------------------------------------------------------------------");
    Log("G3O", "Detailed Phase Breakdown:");
    Log("G3O", "  1. Pre-Load Preparation:          %6.3f s (%5.1f%%)",
        s_preWorldTotalSec, totalSec > 0 ? (s_preWorldTotalSec * 100.0 / totalSec) : 0.0);
    Log("G3O", "       * GetSaveGameHeader:         %6.3f s [%ld pak blocks, %.2f MB read]",
        s_statHeader.durationSec, s_statHeader.pakBlocks, s_statHeader.readMb);
    Log("G3O", "       * gCSession::Stop:           %6.3f s", s_statSessionStop.durationSec);
    Log("G3O", "       * gCGUIManager::ClearEnt:    %6.3f s", s_statClearEntities.durationSec);
    Log("G3O", "       * Elements Deactivation:     %6.3f s [%ld calls]", deactSec, s_deactivateCount);
    Log("G3O", "       * gCWorld::Activate:         %6.3f s [%ld sectors, %.3f s, avg %.1f ms/sector]",
        worldActElemSec, s_sectorActivateCount, secTotalSec,
        s_sectorActivateCount > 0 ? (secTotalSec * 1000.0 / (double)s_sectorActivateCount) : 0.0);
    Log("G3O", "       * ForceFullROIUpdate:        %6.3f s", roiSec);
    Log("G3O", "       * gCWorld::UnloadCaches:     %6.3f s [%ld pak blocks, %.2f MB read]",
        s_statUnloadCaches.durationSec, s_statUnloadCaches.pakBlocks, s_statUnloadCaches.readMb);
    Log("G3O", "       * Other Pre-Load Overhead:   %6.3f s", preOther);
    if (s_sectorTimingCsInit) {
        EnterCriticalSection(&s_sectorTimingCs);
        std::vector<SectorTimingRecord> records = s_sectorTimings;
        LeaveCriticalSection(&s_sectorTimingCs);

        std::sort(records.begin(), records.end(), [](const SectorTimingRecord& a, const SectorTimingRecord& b) {
            return a.durationMs > b.durationMs;
        });

        size_t topN = (records.size() < 15) ? records.size() : 15;
        if (topN > 0) {
            Log("G3O", "       * Top %zu Slowest Sectors in Activation:", topN);
            for (size_t i = 0; i < topN; ++i) {
                Log("G3O", "           [%2zu] %-32s %7.2f ms", i + 1, records[i].name.c_str(), records[i].durationMs);
            }
        }
    }
    if (s_metricsHooksInitialized) {
        Log("G3O", "[LayerAudit] Load-thread inclusive totals: gCLayerBase::CreateData=%llu calls / %.2f ms; gCDynamicLayer::DoLoadData=%llu calls / %.2f ms",
            static_cast<unsigned long long>(s_layerAuditMetrics.createData.calls), LayerMetricMs(s_layerAuditMetrics.createData.qpcTicks),
            static_cast<unsigned long long>(s_layerAuditMetrics.doLoadData.calls), LayerMetricMs(s_layerAuditMetrics.doLoadData.qpcTicks));
    }
    Log("G3O", "  2. gCWorld::LoadGameWorld:        %6.3f s (%5.1f%%) [%ld pak blocks, %.2f MB decomp, %.2f MB read]",
        s_statLoadGameWorld.durationSec, totalSec > 0 ? (s_statLoadGameWorld.durationSec * 100.0 / totalSec) : 0.0,
        s_statLoadGameWorld.pakBlocks, s_statLoadGameWorld.pakOutMb, s_statLoadGameWorld.readMb);
    Log("G3O", "  3. Post-Load Processing:          %6.3f s (%5.1f%%)",
        postTotal, totalSec > 0 ? (postTotal * 100.0 / totalSec) : 0.0);
    Log("G3O", "       * gCSession::GotoLoadedPos:  %6.3f s [%ld pak blocks, %.2f MB decomp, %.2f MB read]",
        s_statGotoPos.durationSec, s_statGotoPos.pakBlocks, s_statGotoPos.pakOutMb, s_statGotoPos.readMb);
    Log("G3O", "           - Pak Decompress (libdeflate): %6.3f s", gotoPakSec);
    Log("G3O", "           - Disk ReadFile I/O:           %6.3f s", gotoReadSec);
    Log("G3O", "           - PVS Cell Hierarchies:        %6.3f s [%ld cells]", pvsSec, s_cellsProcessed);
    Log("G3O", "           - Scene & Entity Setup:        %6.3f s", gotoSetupSec);
    if (s_targetPvsProcess) {
        Log("G3O", "[PVSDirect] GotoLoadedPosition Process inclusive: %.3f s [%llu calls] (not additive)",
            LayerMetricMs(s_pvsPrefetchMetrics.process.qpcTicks) / 1000.0,
            static_cast<unsigned long long>(s_pvsPrefetchMetrics.process.calls));
    } else {
        Log("G3O", "[PVSDirect] eCPVSPrefetcher3::Process timing unavailable (hook not installed)");
    }
    if (s_targetPvsWaitForEmptyThreadQueue) {
        Log("G3O", "[PVSDirect] GotoLoadedPosition queue-wait inclusive: %.3f s [%llu calls] (overlaps Process)",
            LayerMetricMs(s_pvsPrefetchMetrics.waitForEmptyThreadQueue.qpcTicks) / 1000.0,
            static_cast<unsigned long long>(s_pvsPrefetchMetrics.waitForEmptyThreadQueue.calls));
    } else {
        Log("G3O", "[PVSDirect] eCPVSPrefetcher3::WaitForEmptyThreadQueue timing unavailable (hook not installed)");
    }
    Log("G3O", "       * gCSession::Start (World):  %6.3f s [%ld pak blocks, %.2f MB read]",
        s_statSessionStart.durationSec, s_statSessionStart.pakBlocks, s_statSessionStart.readMb);
    Log("G3O", "       * Other Post Overhead:       %6.3f s", postOther);
    Log("G3O", "--------------------------------------------------------------------------------");
    Log("G3O", "Subsystem Activity:");
    Log("G3O", "  - Disk ReadFile I/O:              %6.3f s (%5.1f%%) [%ld calls, %.2f MB read, avg speed %.1f MB/s]",
        rSec, totalSec > 0 ? (rSec * 100.0 / totalSec) : 0.0, rc, rbMb, rSec > 0 ? (rbMb / rSec) : 0.0);

    Log("G3O", "  - Pak Decompress (libdeflate):    %6.3f s (%5.1f%%) [%ld blocks, %.2f MB -> %.2f MB]",
        pakSec, totalSec > 0 ? (pakSec * 100.0 / totalSec) : 0.0, pakCount, pakInMb, pakOutMb);
    Log("G3O", "  - Save Decompress (libdeflate):   %6.3f s (%5.1f%%) [%ld chunks, %.2f MB -> %.2f MB]",
        saveSec, totalSec > 0 ? (saveSec * 100.0 / totalSec) : 0.0, saveCount, saveInMb, saveOutMb);
    Log("G3O", "  - Property Resolution Cache (load-thread counters): %ld resolves (%ld hits, %ld misses, %.1f%% hit rate)",
        totalProps, strHits + idHits, strMiss + idMiss, propHitRate);
    Log("G3O", "  - Frustum Spatial Updates:        %u calls batched -> %u flushed (%u deletions guarded)",
        FrustumOptimizer_GetDeferredCalls(), FrustumOptimizer_GetFlushedUpdates(), FrustumOptimizer_GetDeletionsHandled());
    Log("G3O", "  - Scene Node Preallocations:      %u sector root nodes expanded to 1024-32768",
        SceneNodeOptimizer_GetPreallocations());
    Log("G3O", "  - Directional Shadow Casters:     %u fast adds (%u duplicates avoided), %u fast removes (memmove bypassed)",
        ShadowOptimizer_GetFastAdds(), ShadowOptimizer_GetDuplicatesAvoided(), ShadowOptimizer_GetFastRemoves());
    Log("G3O", "  - Class Factory Reflection:       %u hits (0 alloc), %u misses (cached %u classes)",
        ClassFactory_GetHits(), ClassFactory_GetMisses(), ClassFactory_GetCachedClassesCount());

    Log("G3O", "================================================================================");

    FILE* fp = nullptr;
    if (IsTelemetryActive()) {
        char directory[MAX_PATH] = {};
        char path[MAX_PATH] = {};
        SYSTEMTIME fileTime;
        GetLocalTime(&fileTime);
        const unsigned long sequence = static_cast<unsigned long>(
            InterlockedIncrement(&s_loadProfilerPathSequence));
        if (EnsureG3OOutputDirectory(directory, sizeof(directory)) &&
            BuildTimestampedFilePath(path, sizeof(path), directory, "G3O_LoadProfiler", "log",
                                     fileTime, GetCurrentProcessId(), GetCurrentThreadId(), sequence)) {
            fp = fopen(path, "w");
        }
        if (fp) {
            Log("G3O", "Load profile output: %s", path);
        } else {
            Log("G3O", "Could not create load profile in G3O.");
        }
    }
    if (fp) {
        fprintf(fp, "================================================================================\n");
        fprintf(fp, "[LoadProfile] === DETAILED SAVEGAME LOAD BENCHMARK (Total: %.3f s) ===\n", totalSec);
        fprintf(fp, "--------------------------------------------------------------------------------\n");
        fprintf(fp, "Detailed Phase Breakdown:\n");
        fprintf(fp, "  1. Pre-Load Preparation:          %6.3f s (%5.1f%%)\n",
            s_preWorldTotalSec, totalSec > 0 ? (s_preWorldTotalSec * 100.0 / totalSec) : 0.0);
        fprintf(fp, "       * GetSaveGameHeader:         %6.3f s [%ld pak blocks, %.2f MB read]\n",
            s_statHeader.durationSec, s_statHeader.pakBlocks, s_statHeader.readMb);
        fprintf(fp, "       * gCSession::Stop:           %6.3f s\n", s_statSessionStop.durationSec);
        fprintf(fp, "       * gCGUIManager::ClearEnt:    %6.3f s\n", s_statClearEntities.durationSec);
        fprintf(fp, "       * Elements Deactivation:     %6.3f s [%ld calls]\n", deactSec, s_deactivateCount);
        fprintf(fp, "       * gCWorld::Activate:         %6.3f s [%ld sectors, %.3f s, avg %.1f ms/sector]\n",
            worldActElemSec, s_sectorActivateCount, secTotalSec,
            s_sectorActivateCount > 0 ? (secTotalSec * 1000.0 / (double)s_sectorActivateCount) : 0.0);
        fprintf(fp, "       * ForceFullROIUpdate:        %6.3f s\n", roiSec);
        fprintf(fp, "       * gCWorld::UnloadCaches:     %6.3f s [%ld pak blocks, %.2f MB read]\n",
            s_statUnloadCaches.durationSec, s_statUnloadCaches.pakBlocks, s_statUnloadCaches.readMb);
        fprintf(fp, "       * Other Pre-Load Overhead:   %6.3f s\n", preOther);
        if (s_sectorTimingCsInit) {
            EnterCriticalSection(&s_sectorTimingCs);
            std::vector<SectorTimingRecord> records = s_sectorTimings;
            LeaveCriticalSection(&s_sectorTimingCs);

            std::sort(records.begin(), records.end(), [](const SectorTimingRecord& a, const SectorTimingRecord& b) {
                return a.durationMs > b.durationMs;
            });

            size_t topN = (records.size() < 15) ? records.size() : 15;
            if (topN > 0) {
                fprintf(fp, "       * Top %zu Slowest Sectors in Activation:\n", topN);
                for (size_t i = 0; i < topN; ++i) {
                    fprintf(fp, "           [%2zu] %-32s %7.2f ms\n", i + 1, records[i].name.c_str(), records[i].durationMs);
                }
            }
        }
        if (s_metricsHooksInitialized) {
            fprintf(fp, "[LayerAudit] Load-thread inclusive totals: gCLayerBase::CreateData=%llu calls / %.2f ms; gCDynamicLayer::DoLoadData=%llu calls / %.2f ms\n",
                static_cast<unsigned long long>(s_layerAuditMetrics.createData.calls), LayerMetricMs(s_layerAuditMetrics.createData.qpcTicks),
                static_cast<unsigned long long>(s_layerAuditMetrics.doLoadData.calls), LayerMetricMs(s_layerAuditMetrics.doLoadData.qpcTicks));
        }
        fprintf(fp, "  2. gCWorld::LoadGameWorld:        %6.3f s (%5.1f%%) [%ld pak blocks, %.2f MB decomp, %.2f MB read]\n",
            s_statLoadGameWorld.durationSec, totalSec > 0 ? (s_statLoadGameWorld.durationSec * 100.0 / totalSec) : 0.0,
            s_statLoadGameWorld.pakBlocks, s_statLoadGameWorld.pakOutMb, s_statLoadGameWorld.readMb);
        fprintf(fp, "  3. Post-Load Processing:          %6.3f s (%5.1f%%)\n",
            postTotal, totalSec > 0 ? (postTotal * 100.0 / totalSec) : 0.0);
        fprintf(fp, "       * gCSession::GotoLoadedPos:  %6.3f s [%ld pak blocks, %.2f MB decomp, %.2f MB read]\n",
            s_statGotoPos.durationSec, s_statGotoPos.pakBlocks, s_statGotoPos.pakOutMb, s_statGotoPos.readMb);
        fprintf(fp, "           - Pak Decompress (libdeflate): %6.3f s\n", gotoPakSec);
        fprintf(fp, "           - Disk ReadFile I/O:           %6.3f s\n", gotoReadSec);
        fprintf(fp, "           - PVS Cell Hierarchies:        %6.3f s [%ld cells]\n", pvsSec, s_cellsProcessed);
        fprintf(fp, "           - Scene & Entity Setup:        %6.3f s\n", gotoSetupSec);
        if (s_targetPvsProcess) {
            fprintf(fp, "[PVSDirect] GotoLoadedPosition Process inclusive: %.3f s [%llu calls] (not additive)\n",
                LayerMetricMs(s_pvsPrefetchMetrics.process.qpcTicks) / 1000.0,
                static_cast<unsigned long long>(s_pvsPrefetchMetrics.process.calls));
        } else {
            fprintf(fp, "[PVSDirect] eCPVSPrefetcher3::Process timing unavailable (hook not installed)\n");
        }
        if (s_targetPvsWaitForEmptyThreadQueue) {
            fprintf(fp, "[PVSDirect] GotoLoadedPosition queue-wait inclusive: %.3f s [%llu calls] (overlaps Process)\n",
                LayerMetricMs(s_pvsPrefetchMetrics.waitForEmptyThreadQueue.qpcTicks) / 1000.0,
                static_cast<unsigned long long>(s_pvsPrefetchMetrics.waitForEmptyThreadQueue.calls));
        } else {
            fprintf(fp, "[PVSDirect] eCPVSPrefetcher3::WaitForEmptyThreadQueue timing unavailable (hook not installed)\n");
        }
        fprintf(fp, "       * gCSession::Start (World):  %6.3f s [%ld pak blocks, %.2f MB read]\n",
            s_statSessionStart.durationSec, s_statSessionStart.pakBlocks, s_statSessionStart.readMb);
        fprintf(fp, "       * Other Post Overhead:       %6.3f s\n", postOther);
        fprintf(fp, "--------------------------------------------------------------------------------\n");
        fprintf(fp, "Subsystem Activity:\n");
        fprintf(fp, "  - Disk ReadFile I/O:              %6.3f s (%5.1f%%) [%ld calls, %.2f MB read, speed %.1f MB/s]\n",
            rSec, totalSec > 0 ? (rSec * 100.0 / totalSec) : 0.0, rc, rbMb, rSec > 0 ? (rbMb / rSec) : 0.0);

        fprintf(fp, "  - Pak Decompress (libdeflate):    %6.3f s (%5.1f%%) [%ld blocks, %.2f MB -> %.2f MB]\n",
            pakSec, totalSec > 0 ? (pakSec * 100.0 / totalSec) : 0.0, pakCount, pakInMb, pakOutMb);
        fprintf(fp, "  - Save Decompress (libdeflate):   %6.3f s (%5.1f%%) [%ld chunks, %.2f MB -> %.2f MB]\n",
            saveSec, totalSec > 0 ? (saveSec * 100.0 / totalSec) : 0.0, saveCount, saveInMb, saveOutMb);
        fprintf(fp, "  - Decomp Duplicate Cache:         %ld hits (%.2f MB saved from redundant decompression)\n",
            GetDecompCacheHits(), (double)GetDecompCacheBytesSaved() / (1024.0 * 1024.0));
        fprintf(fp, "  - Property Resolution Cache (load-thread counters): %ld resolves (%ld hits, %ld misses, %.1f%% hit rate)\n",
            totalProps, strHits + idHits, strMiss + idMiss, propHitRate);
        fprintf(fp, "  - Frustum Spatial Updates:        %u calls batched -> %u flushed (%u deletions guarded)\n",
            FrustumOptimizer_GetDeferredCalls(), FrustumOptimizer_GetFlushedUpdates(), FrustumOptimizer_GetDeletionsHandled());
        fprintf(fp, "  - Scene Node Preallocations:      %u sector root nodes expanded to 1024-32768\n",
            SceneNodeOptimizer_GetPreallocations());
        fprintf(fp, "  - Directional Shadow Casters:     %u fast adds (%u duplicates avoided), %u fast removes (memmove bypassed)\n",
            ShadowOptimizer_GetFastAdds(), ShadowOptimizer_GetDuplicatesAvoided(), ShadowOptimizer_GetFastRemoves());
        fprintf(fp, "  - Class Factory Reflection:       %u hits (0 alloc), %u misses (cached %u classes)\n",
            ClassFactory_GetHits(), ClassFactory_GetMisses(), ClassFactory_GetCachedClassesCount());

        fprintf(fp, "================================================================================\n");
        fclose(fp);
    }
}

// -----------------------------------------------------------------------------
// Hooks
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// Stack sampler: finds where the main thread spends time during a load.
// Samples every ~20 ms; counts EIP samples and heuristic return-address candidates per phase.
// -----------------------------------------------------------------------------
static constexpr int kSamplePhaseCount = G3O::kSysDynSamplePhase + 1;
static volatile int s_samplePhase = 0; // 0=PreLoad 1=LoadGameWorld 2=GotoLoadedPosition 3=Session::Start 4=SysDyn sector
static const char* const kPhaseNames[kSamplePhaseCount] = { "Pre-Load", "LoadGameWorld", "GotoLoadedPosition", "Session::Start", "SysDyn sector" };
static HANDLE s_sampleTarget = nullptr;
static HANDLE s_sampleThread = nullptr;
static volatile bool s_sampleStop = false;
static std::map<DWORD, int> s_sampleInclusive[kSamplePhaseCount];
static std::map<DWORD, int> s_sampleLeaf[kSamplePhaseCount];
static int s_sampleCount[kSamplePhaseCount] = { 0, 0, 0, 0, 0 };

static bool LooksLikeReturnAddr(DWORD a) {
    // Raw stack scan yields candidates only; it does not unwind the call stack.
    const BYTE* p = (const BYTE*)a;
    if (a < 0x10000) return false;
    HMODULE hm = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &hm) || !hm) return false;
    bool ok = false;
    __try {
        if (p[-5] == 0xE8) ok = true;
        else if (p[-6] == 0xFF && ((p[-5] >> 3) & 7) == 2) ok = true;
        else if (p[-2] == 0xFF && ((p[-1] >> 3) & 7) == 2) ok = true;
        else if (p[-3] == 0xFF && ((p[-2] >> 3) & 7) == 2) ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    return ok;
}

// Runs while target thread is suspended: no allocations, no locks, no logging.
static int CaptureStackRaw(HANDLE hThread, DWORD* outEip, DWORD* outFrames, int maxFrames) {
    CONTEXT ctx = {};
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (!GetThreadContext(hThread, &ctx)) return -1;
    *outEip = ctx.Eip;
    int n = 0;
    DWORD sp = ctx.Esp;
    __try {
        for (int i = 0; i < 4096 / 4 && n < maxFrames; ++i) {
            DWORD v = ((DWORD*)sp)[i];
            outFrames[n++] = v; // raw candidates; filtered after resume
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return n;
}

static DWORD WINAPI SamplerThreadProc(LPVOID) {
    static DWORD raw[1024];
    while (!s_sampleStop) {
        Sleep(20);
        if (s_sampleStop) break;
        DWORD eip = 0;
        int n = 0;
        if (SuspendThread(s_sampleTarget) == (DWORD)-1) continue;
        int ph = s_samplePhase;
        n = CaptureStackRaw(s_sampleTarget, &eip, raw, 1024);
        ResumeThread(s_sampleTarget);
        if (n < 0) continue;

        if (ph < 0 || ph >= kSamplePhaseCount) ph = 0;
        s_sampleCount[ph]++;
        s_sampleLeaf[ph][eip]++;
        std::map<DWORD, int>& inc = s_sampleInclusive[ph];
        DWORD seen[64]; int seenN = 0;
        for (int i = 0; i < n && seenN < 64; ++i) {
            DWORD v = raw[i];
            if (!LooksLikeReturnAddr(v)) continue;
            bool dup = false;
            for (int k = 0; k < seenN; ++k) if (seen[k] == v) { dup = true; break; }
            if (dup) continue;
            seen[seenN++] = v;
            inc[v]++;
        }
    }
    return 0;
}

static std::string DescribeAddr(DWORD a) {
    char buf[320] = { 0 };
    HMODULE hm = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &hm) && hm) {
        char path[MAX_PATH] = { 0 };
        GetModuleFileNameA(hm, path, MAX_PATH);
        const char* base = strrchr(path, '\\');
        base = base ? base + 1 : path;
        snprintf(buf, sizeof(buf), "%s+0x%X", base, (unsigned)(a - (DWORD)hm));
    } else {
        snprintf(buf, sizeof(buf), "0x%08X", (unsigned)a);
    }
    return buf;
}

static void StartSampler() {
    for (int i = 0; i < kSamplePhaseCount; ++i) { s_sampleInclusive[i].clear(); s_sampleLeaf[i].clear(); s_sampleCount[i] = 0; }
    s_samplePhase = 0;
    s_sampleStop = false;
    if (!s_sampleTarget) {
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &s_sampleTarget,
                        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    }
    if (s_sampleTarget) s_sampleThread = CreateThread(nullptr, 0, SamplerThreadProc, nullptr, 0, nullptr);
}

static void StopSamplerAndReport() {
    if (!s_sampleThread) return;
    s_sampleStop = true;
    WaitForSingleObject(s_sampleThread, 2000);
    CloseHandle(s_sampleThread);
    s_sampleThread = nullptr;

    for (int ph = 0; ph < kSamplePhaseCount; ++ph) {
        int total = s_sampleCount[ph];
        if (total < 5) continue;
        Log("G3O", "[Sampler] ===== %s: %d samples (~%.2f s) =====", kPhaseNames[ph], total, total * 0.02);
        std::vector<std::pair<int, DWORD>> v;
        for (auto& kv : s_sampleInclusive[ph]) v.push_back({ kv.second, kv.first });
        std::sort(v.begin(), v.end(), [](const std::pair<int, DWORD>& a, const std::pair<int, DWORD>& b) { return a.first > b.first; });
        Log("G3O", "[Sampler]  -- heuristic stack candidates (not unwound) --");
        for (size_t i = 0; i < v.size() && i < 25; ++i)
            Log("G3O", "[Sampler]   %5.1f%%  %s", 100.0 * v[i].first / total, DescribeAddr(v[i].second).c_str());
        std::vector<std::pair<int, DWORD>> l;
        for (auto& kv : s_sampleLeaf[ph]) l.push_back({ kv.second, kv.first });
        std::sort(l.begin(), l.end(), [](const std::pair<int, DWORD>& a, const std::pair<int, DWORD>& b) { return a.first > b.first; });
        Log("G3O", "[Sampler]  -- leaf (EIP) (top 12) --");
        for (size_t i = 0; i < l.size() && i < 12; ++i)
            Log("G3O", "[Sampler]   %5.1f%%  %s", 100.0 * l[i].first / total, DescribeAddr(l[i].second).c_str());
    }
}

typedef void (__stdcall* EnableSectorActivation_t)(bool);
static EnableSectorActivation_t Real_EnableSectorActivation = nullptr;

typedef bool (__thiscall* HUDLoadGame_t)(void* pThis, void* a, void* b);
static HUDLoadGame_t Real_HUDLoadGame = nullptr;

static bool __fastcall Hooked_HUDLoadGame(void* pThis, void* /*edx*/, void* a, void* b) {
    const Config& cfg = GetConfig();
    ProfilerSetGameplayReady(false);
    SetEnginePhase(Phase_PreLoad_SysDyn);
    s_isLoadActive = true;
    s_layerAuditMetrics = {};
    s_pvsPrefetchMetrics = {};
    s_capturePvsPrefetchMetrics = false;
    s_inPreLoad = true;
    s_skippedPreLoadActivate = false;
    QueryPerformanceCounter(&s_tHUDStart);
    if (ShouldCollectSavegameAudit(cfg) || cfg.enableProfiler) {
        Log("G3O", "================================================================================");
        Log("G3O", "[LoadProfile] >>> SAVEGAME LOAD INITIATED (gCHUDFileManager::LoadGame) >>>");
    }

    DynamicLayerOptimizer_OnLoadStart();

    ResetDecompTelemetry();
    ResetPropertyCacheTelemetry();
    ResetShadowCasterOptimizerSession();
    ResetClassFactoryOptimizerSession();

    ResetVFSCacheTelemetry();
    InterlockedExchange(&s_readCalls, 0);
    InterlockedExchange64(&s_readBytes, 0);
    InterlockedExchange64(&s_readTimeUs, 0);
    InterlockedExchange(&s_cellsProcessed, 0);
    InterlockedExchange64(&s_hierarchyTimeUs, 0);

    InterlockedExchange(&s_deactivateCount, 0);
    InterlockedExchange(&s_sectorActivateCount, 0);
    InterlockedExchange64(&s_sectorActivateTotalUs, 0);
    InterlockedExchange64(&s_deactivateTimeUs, 0);
    InterlockedExchange64(&s_worldActivateElemTimeUs, 0);
    InterlockedExchange64(&s_forceROIUpdateTimeUs, 0);

    s_statHeader = {};
    s_statSessionStop = {};
    s_statClearEntities = {};
    s_statUnloadCaches = {};
    s_statLoadGameWorld = {};
    s_statGotoPos = {};
    s_statSessionStart = {};
    s_statWorldActivate = {};
    s_preWorldTotalSec = 0.0;
    if (s_sectorTimingCsInit) {
        EnterCriticalSection(&s_sectorTimingCs);
        s_sectorTimings.clear();
        LeaveCriticalSection(&s_sectorTimingCs);
    }

    if (ShouldTracePakPrefetch(cfg)) {
        PakTracer_OnLoadStart(s_lastSaveName.c_str());
    }
    bool r = Real_HUDLoadGame(pThis, a, b);
    if (ShouldTracePakPrefetch(cfg)) {
        PakTracer_OnLoadEnd();
    }

    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);
    FrustumOptimizer_EndDeferAndFlush(); // Backstop flush in case Session::Start was bypassed
    s_isLoadActive = false;
    SetEnginePhase(r ? Phase_Gameplay : Phase_Init);
    ProfilerSetGameplayReady(r);
    if (Real_EnableSectorActivation) {
        Real_EnableSectorActivation(true);
    }
    double totalSec = (double)(t1.QuadPart - s_tHUDStart.QuadPart) / (double)s_freq.QuadPart;

    if (ShouldCollectSavegameAudit(cfg)) {
        LogLoadReport(totalSec);
    }
    return r;
}

// 1. GetSaveGameHeader hook
typedef int (__thiscall* GetSaveGameHeader_t)(void* pThis, const void* pStr, void* pHeader);
static GetSaveGameHeader_t Real_GetSaveGameHeader = nullptr;

static int __fastcall Hooked_GetSaveGameHeader(void* pThis, void* /*edx*/, const void* pStr, void* pHeader) {
    if (pStr) {
        const char* s = *(const char**)pStr;
        if (s && *s) s_lastSaveName = s;
    }
    if (!s_isLoadActive) return Real_GetSaveGameHeader(pThis, pStr, pHeader);
    LARGE_INTEGER t0, t1;
    LONG pak0 = GetPakDecompCount(); LONG64 pakOut0 = GetPakDecompOutBytes();
    LONG read0 = s_readCalls; LONG64 readBytes0 = s_readBytes;
    QueryPerformanceCounter(&t0);

    int r = Real_GetSaveGameHeader(pThis, pStr, pHeader);

    QueryPerformanceCounter(&t1);
    SnapshotPhase(s_statHeader, t0, t1, pak0, pakOut0, read0, readBytes0);
    Log("G3O", "[LoadProfile] gCWorld::GetSaveGameHeader took %.3f s", s_statHeader.durationSec);
    return r;
}

// 2. SessionStop hook
typedef void (__thiscall* SessionStop_t)(void* pThis);
static SessionStop_t Real_SessionStop = nullptr;

static void __fastcall Hooked_SessionStop(void* pThis, void* /*edx*/) {
    ProfilerSetGameplayReady(false);
    if (!s_isLoadActive) { Real_SessionStop(pThis); return; }
    LARGE_INTEGER t0, t1;
    LONG pak0 = GetPakDecompCount(); LONG64 pakOut0 = GetPakDecompOutBytes();
    LONG read0 = s_readCalls; LONG64 readBytes0 = s_readBytes;
    QueryPerformanceCounter(&t0);

    Real_SessionStop(pThis);

    QueryPerformanceCounter(&t1);
    SnapshotPhase(s_statSessionStop, t0, t1, pak0, pakOut0, read0, readBytes0);
    Log("G3O", "[LoadProfile] gCSession::Stop took %.3f s", s_statSessionStop.durationSec);
}

// 3. ClearAllEntities hook
typedef bool (__thiscall* ClearAllEntities_t)(void* pThis);
static ClearAllEntities_t Real_ClearAllEntities = nullptr;

static bool __fastcall Hooked_ClearAllEntities(void* pThis, void* /*edx*/) {
    if (!s_isLoadActive) return Real_ClearAllEntities(pThis);
    LARGE_INTEGER t0, t1;
    LONG pak0 = GetPakDecompCount(); LONG64 pakOut0 = GetPakDecompOutBytes();
    LONG read0 = s_readCalls; LONG64 readBytes0 = s_readBytes;
    QueryPerformanceCounter(&t0);

    bool r = Real_ClearAllEntities(pThis);

    QueryPerformanceCounter(&t1);
    SnapshotPhase(s_statClearEntities, t0, t1, pak0, pakOut0, read0, readBytes0);
    Log("G3O", "[LoadProfile] gCGUIManager::ClearAllEntities took %.3f s", s_statClearEntities.durationSec);
    return r;
}

// 4. Deactivate hook (Engine.dll: ?Deactivate@eCProcessibleElement@@QAE?AW4eEResult@1@XZ)
typedef int (__thiscall* Deactivate_t)(void* pThis);
static Deactivate_t Real_Deactivate = nullptr;
static void* s_targetDeactivate = nullptr;

static int __fastcall Hooked_Deactivate(void* pThis, void* /*edx*/) {
    if (!s_isLoadActive) return Real_Deactivate(pThis);

    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);

    int res = Real_Deactivate(pThis);

    QueryPerformanceCounter(&t1);
    LONG64 us = (s_freq.QuadPart > 0) ? ((t1.QuadPart - t0.QuadPart) * 1000000LL / s_freq.QuadPart) : 0;
    InterlockedExchangeAdd64(&s_deactivateTimeUs, us);
    InterlockedIncrement(&s_deactivateCount);

    double dt = (double)us / 1000000.0;
    if (dt >= 0.010) {
        Log("G3O", "[LoadProfile] eCProcessibleElement::Deactivate (0x%p) took %.3f s", pThis, dt);
    }
    return res;
}

typedef int (__thiscall* GetDynamicLayerCount_t)(void* pSector);
static GetDynamicLayerCount_t Real_GetDynamicLayerCount = nullptr;

typedef int (__thiscall* GetSectorCount_t)(void* pWorld);
static GetSectorCount_t Real_GetSectorCount = nullptr;

// 5. WorldActivateElem hook (Game.dll: ?Activate@gCWorld@@UAE?AW4eEResult@eCProcessibleElement@@XZ)
typedef int (__thiscall* WorldActivateElem_t)(void* pThis);
static WorldActivateElem_t Real_WorldActivateElem = nullptr;
static void* s_targetWorldActivateElem = nullptr;

static int __fastcall Hooked_WorldActivateElem(void* pThis, void* /*edx*/) {
    if (!s_isLoadActive) return Real_WorldActivateElem(pThis);

    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);

    int res = Real_WorldActivateElem(pThis);

    QueryPerformanceCounter(&t1);
    LONG64 us = (s_freq.QuadPart > 0) ? ((t1.QuadPart - t0.QuadPart) * 1000000LL / s_freq.QuadPart) : 0;
    InterlockedExchangeAdd64(&s_worldActivateElemTimeUs, us);
    Log("G3O", "[LoadProfile] gCWorld::Activate took %.3f s", (double)us / 1000000.0);
    return res;
}

static void DumpSectorCallStack(const char* secName, void* pSector, DWORD* stackPtr) {
    Log("G3O", "[SectorTrace] Stack dump at sector 1 ('%s', pSector=0x%p):", secName, pSector);
    for (int i = 0; i < 24; ++i) {
        DWORD val = stackPtr[i];
        if (LooksLikeReturnAddr(val)) {
            Log("G3O", "  esp[%2d]: 0x%08X -> %s", i, val, DescribeAddr(val).c_str());
        }
    }
}

// 5.3. SectorOnActivate hook (Game.dll: ?OnActivate@gCSector@@MAEXXZ)
typedef void (__thiscall* SectorOnActivate_t)(void* pSector);
static SectorOnActivate_t Real_SectorOnActivate = nullptr;
static void* s_targetSectorOnActivate = nullptr;

typedef int (__thiscall* SectorDoLoadData_t)(void* pSector, void* pStream);
static SectorDoLoadData_t Real_SectorDoLoadData = nullptr;
static void* s_targetSectorDoLoadData = nullptr;
static thread_local LONG64 s_sectorDoLoadDataUs = 0;
static thread_local LONG s_sectorDoLoadDataCalls = 0;

// Sub-activation probes inside gCSector::OnActivate
typedef int (__thiscall* ElementActivate_t)(void* pThis);
static ElementActivate_t Real_ElementActivate = nullptr;
static void* s_targetElementActivate = nullptr;
static uintptr_t s_retDynLayerActivate = 0;
static uintptr_t s_retStaticLayerActivate = 0;

typedef int (__thiscall* GeometryLayerActivate_t)(void* pThis);
static GeometryLayerActivate_t Real_GeometryLayerActivate = nullptr;
static void* s_targetGeometryLayerActivate = nullptr;

typedef void (__thiscall* ElementOnActivate_t)(void* pThis);
static ElementOnActivate_t Real_ElementOnActivate = nullptr;
static void* s_targetElementOnActivate = nullptr;
static uintptr_t s_retSectorBaseOnActivate = 0;

struct SectorActivationAudit {
    uint64_t baseOnActivateTicks = 0;
    uint32_t baseOnActivateCalls = 0;
    uint64_t dynLayersTicks = 0;
    uint32_t dynLayersCalls = 0;
    uint64_t staticLayersTicks = 0;
    uint32_t staticLayersCalls = 0;
};
static thread_local SectorActivationAudit s_sectorActivationAudit{};

static int __fastcall Hooked_ElementActivate(void* pThis, void* /*edx*/) {
    if (!s_isLoadActive) return Real_ElementActivate(pThis);
    const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    if (caller == s_retDynLayerActivate) {
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        int res = Real_ElementActivate(pThis);
        QueryPerformanceCounter(&t1);
        s_sectorActivationAudit.dynLayersTicks += static_cast<uint64_t>(t1.QuadPart - t0.QuadPart);
        s_sectorActivationAudit.dynLayersCalls++;
        return res;
    }
    if (caller == s_retStaticLayerActivate) {
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        int res = Real_ElementActivate(pThis);
        QueryPerformanceCounter(&t1);
        s_sectorActivationAudit.staticLayersTicks += static_cast<uint64_t>(t1.QuadPart - t0.QuadPart);
        s_sectorActivationAudit.staticLayersCalls++;
        return res;
    }
    return Real_ElementActivate(pThis);
}

static int __fastcall Hooked_GeometryLayerActivate(void* pThis, void* /*edx*/) {
    if (!s_isLoadActive) return Real_GeometryLayerActivate(pThis);
    const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    if (caller == s_retStaticLayerActivate) {
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        int res = Real_GeometryLayerActivate(pThis);
        QueryPerformanceCounter(&t1);
        s_sectorActivationAudit.staticLayersTicks += static_cast<uint64_t>(t1.QuadPart - t0.QuadPart);
        s_sectorActivationAudit.staticLayersCalls++;
        return res;
    }
    return Real_GeometryLayerActivate(pThis);
}

static void __fastcall Hooked_ElementOnActivate(void* pThis, void* /*edx*/) {
    if (!s_isLoadActive) {
        Real_ElementOnActivate(pThis);
        return;
    }
    const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    if (caller == s_retSectorBaseOnActivate) {
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        Real_ElementOnActivate(pThis);
        QueryPerformanceCounter(&t1);
        s_sectorActivationAudit.baseOnActivateTicks += static_cast<uint64_t>(t1.QuadPart - t0.QuadPart);
        s_sectorActivationAudit.baseOnActivateCalls++;
        return;
    }
    Real_ElementOnActivate(pThis);
}

static int __fastcall Hooked_SectorDoLoadData(void* pSector, void* /*edx*/, void* pStream) {
    if (!s_isLoadActive) return Real_SectorDoLoadData(pSector, pStream);
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    int res = Real_SectorDoLoadData(pSector, pStream);
    QueryPerformanceCounter(&t1);
    LONG64 us = (s_freq.QuadPart > 0) ? ((t1.QuadPart - t0.QuadPart) * 1000000LL / s_freq.QuadPart) : 0;
    s_sectorDoLoadDataUs += us;
    ++s_sectorDoLoadDataCalls;
    return res;
}

static void __fastcall Hooked_SectorOnActivate(void* pSector, void* /*edx*/) {
    const Config& config = GetConfig();
    const bool layerArenaEnabled = config.enableLayerRingArena && config.enableFastPakDecompressor;
    const char* sectorNameForCapture = nullptr;
    if (layerArenaEnabled) {
        sectorNameForCapture = SafeGetSectorName(pSector);
        if (sectorNameForCapture && std::strstr(sectorNameForCapture, "RB_Mod")) {
            LayerRingArena_OnSectorStart(sectorNameForCapture);
        } else {
            LayerRingArena_OnSectorEnd();
        }
    }
    if (!s_isLoadActive) {
        Real_SectorOnActivate(pSector);
        return;
    }

    if (!sectorNameForCapture) sectorNameForCapture = SafeGetSectorName(pSector);
    const bool isSysDynSector = IsTelemetryActive()
        && sectorNameForCapture
        && std::strncmp(sectorNameForCapture, "SysDyn_", sizeof("SysDyn_") - 1) == 0;
    const bool previousSysDynSectorActive = s_isSysDynSectorActive;
    const bool previousSysDynDispatch = s_collectSysDynReadDispatch;
    const bool ownsSysDynCapture = isSysDynSector && !previousSysDynSectorActive;
    if (ownsSysDynCapture) {
        s_sysDynReadDispatchMetrics = {};
        s_sysDynPropertyCacheTelemetry = {};
    }
    s_isSysDynSectorActive = isSysDynSector;
    s_collectSysDynReadDispatch = false;

    s_sectorDoLoadDataUs = 0;
    s_sectorDoLoadDataCalls = 0;
    s_sectorActivationAudit = {};
    int dynBefore = Real_GetDynamicLayerCount ? Real_GetDynamicLayerCount(pSector) : -1;

    const LayerAuditMetrics metricsBefore = s_layerAuditMetrics;
    const int previousSamplePhase = s_samplePhase;
    if (s_sampleThread) {
        s_samplePhase = G3O::ResolveSamplePhaseForSector(previousSamplePhase, sectorNameForCapture);
    }
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    Real_SectorOnActivate(pSector);
    s_collectSysDynReadDispatch = previousSysDynDispatch;
    s_isSysDynSectorActive = previousSysDynSectorActive;
    if (s_sampleThread) s_samplePhase = previousSamplePhase;
    QueryPerformanceCounter(&t1);
    const LayerAuditMetrics metricsDelta = SubtractLayerMetrics(s_layerAuditMetrics, metricsBefore);
    LONG64 us = (s_freq.QuadPart > 0) ? ((t1.QuadPart - t0.QuadPart) * 1000000LL / s_freq.QuadPart) : 0;
    InterlockedIncrement(&s_sectorActivateCount);
    InterlockedExchangeAdd64(&s_sectorActivateTotalUs, us);

    double totalMs = (double)us / 1000.0;
    const LONG64 sectorDoLoadDataUs = s_sectorDoLoadDataUs;
    const LONG sectorDoLoadDataCalls = s_sectorDoLoadDataCalls;
    double sectorDoLoadDataMs = (double)sectorDoLoadDataUs / 1000.0;
    double baseMs = LayerMetricMs(s_sectorActivationAudit.baseOnActivateTicks);
    double dynLayersMs = LayerMetricMs(s_sectorActivationAudit.dynLayersTicks);
    double staticLayersMs = LayerMetricMs(s_sectorActivationAudit.staticLayersTicks);
    double otherOnActivateMs = totalMs - baseMs - dynLayersMs - staticLayersMs;
    if (otherOnActivateMs < 0.0) otherOnActivateMs = 0.0;
    int dynAfter = Real_GetDynamicLayerCount ? Real_GetDynamicLayerCount(pSector) : -1;
    const char* secName = SafeGetSectorName(pSector);
    RecordSectorTiming(secName, totalMs);

    if (IsTelemetryActive() && totalMs >= 50.0) {
        Log("G3O", "[SectorAudit] Sector '%s': OnActivate=%.2f ms | Base=%.2f ms (calls=%u, Sector::DoLoadData=%.2f ms) | DynLayers=%.2f ms (calls=%u) | StaticLayers=%.2f ms (calls=%u) | Other=%.2f ms | dynLayers: %d -> %d",
            secName ? secName : "unknown",
            totalMs,
            baseMs, s_sectorActivationAudit.baseOnActivateCalls, sectorDoLoadDataMs,
            dynLayersMs, s_sectorActivationAudit.dynLayersCalls,
            staticLayersMs, s_sectorActivationAudit.staticLayersCalls,
            otherOnActivateMs,
            dynBefore, dynAfter);
        Log("G3O", "[LayerAudit] Sector '%s': gCLayerBase::CreateData=%llu calls / %.2f ms | gCDynamicLayer::DoLoadData=%llu calls / %.2f ms",
            secName ? secName : "unknown",
            static_cast<unsigned long long>(metricsDelta.createData.calls), LayerMetricMs(metricsDelta.createData.qpcTicks),
            static_cast<unsigned long long>(metricsDelta.doLoadData.calls), LayerMetricMs(metricsDelta.doLoadData.qpcTicks));
    }

    if (ownsSysDynCapture) {
        Log("G3O", "[PropertyCache] SysDyn OnLoadDynamicContext load-thread stringHits=%ld stringMisses=%ld idHits=%ld idMisses=%ld",
            s_sysDynPropertyCacheTelemetry.stringHits,
            s_sysDynPropertyCacheTelemetry.stringMisses,
            s_sysDynPropertyCacheTelemetry.idHits,
            s_sysDynPropertyCacheTelemetry.idMisses);
        LogPropertyReadDispatchPath("ReadObject", s_sysDynReadDispatchMetrics.readObject);
        LogPropertyReadDispatchPath("ReadNewObject", s_sysDynReadDispatchMetrics.readNewObject);
    }
}

// 5.4. WorldOnActivate hook (Game.dll: ?OnActivate@gCWorld@@MAEXXZ)
typedef void (__thiscall* WorldOnActivate_t)(void* pWorld);
static WorldOnActivate_t Real_WorldOnActivate = nullptr;
static void* s_targetWorldOnActivate = nullptr;

typedef int (__thiscall* WorldDoLoadData_t)(void* pWorld, void* pStream);
static WorldDoLoadData_t Real_WorldDoLoadData = nullptr;
static void* s_targetWorldDoLoadData = nullptr;

static int __fastcall Hooked_WorldDoLoadData(void* pWorld, void* /*edx*/, void* pStream) {
    int res = Real_WorldDoLoadData(pWorld, pStream);
    int numSectors = Real_GetSectorCount ? Real_GetSectorCount(pWorld) : 0;
    Log("G3O", "[WorldAudit] World DoLoadData returned res=%d, totalSectors=%d, isLoadActive=%d",
        res, numSectors, (int)s_isLoadActive);
    return res;
}

static void __fastcall Hooked_WorldOnActivate(void* pWorld, void* /*edx*/) {
    if (s_isLoadActive) {
        PakTracer_OnWorldActivateStart();
    }
    const Config& cfg = GetConfig();
    if (cfg.enableParallelDynamicLayers) {
        DynamicLayerOptimizer_ProcessWorldActivate(pWorld, Real_WorldOnActivate);
        DynamicLayerOptimizer_OnLoadEnd();
    } else {
        int numSectors = Real_GetSectorCount ? Real_GetSectorCount(pWorld) : 0;
        Log("G3O", "[WorldAudit] WorldOnActivate entered: pWorld=0x%p, isLoadActive=%d, totalSectors=%d",
            pWorld, (int)s_isLoadActive, numSectors);
        Real_WorldOnActivate(pWorld);
    }
}

// 5.5. DoAfterWorldActivate hook (Game.dll: ?DoAfterWorldActivate@gCWorld@@IAEXXZ)
typedef void (__thiscall* DoAfterWorldActivate_t)(void* pWorld);
static DoAfterWorldActivate_t Real_DoAfterWorldActivate = nullptr;
static void* s_targetDoAfterWorldActivate = nullptr;

static void __fastcall Hooked_DoAfterWorldActivate(void* pWorld, void* /*edx*/) {
    Real_DoAfterWorldActivate(pWorld);
}

// 6. ForceROIUpdate hook (Engine.dll: ?FourceFullROIUpdate@eCEntityAdmin@@QAEXXZ)
typedef void (__thiscall* ForceROIUpdate_t)(void* pThis);
static ForceROIUpdate_t Real_ForceROIUpdate = nullptr;
static void* s_targetForceROIUpdate = nullptr;

static void __fastcall Hooked_ForceROIUpdate(void* pThis, void* /*edx*/) {
    if (!s_isLoadActive) { Real_ForceROIUpdate(pThis); return; }

    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);

    Real_ForceROIUpdate(pThis);

    QueryPerformanceCounter(&t1);
    LONG64 us = (s_freq.QuadPart > 0) ? ((t1.QuadPart - t0.QuadPart) * 1000000LL / s_freq.QuadPart) : 0;
    InterlockedExchangeAdd64(&s_forceROIUpdateTimeUs, us);
    Log("G3O", "[LoadProfile] eCEntityAdmin::ForceFullROIUpdate took %.3f s", (double)us / 1000000.0);
}

// 7. UnloadCaches hook
typedef void (__thiscall* UnloadCaches_t)(void* pThis);
static UnloadCaches_t Real_UnloadCaches = nullptr;

static void __fastcall Hooked_UnloadCaches(void* pThis, void* /*edx*/) {
    if (!s_isLoadActive) { Real_UnloadCaches(pThis); return; }
    LARGE_INTEGER t0, t1;
    LONG pak0 = GetPakDecompCount(); LONG64 pakOut0 = GetPakDecompOutBytes();
    LONG read0 = s_readCalls; LONG64 readBytes0 = s_readBytes;
    QueryPerformanceCounter(&t0);

    Real_UnloadCaches(pThis);

    QueryPerformanceCounter(&t1);
    SnapshotPhase(s_statUnloadCaches, t0, t1, pak0, pakOut0, read0, readBytes0);
    Log("G3O", "[LoadProfile] gCWorld::UnloadCaches took %.3f s", s_statUnloadCaches.durationSec);
}

// 8. LoadGameWorld hook
typedef bool (__thiscall* LoadGameWorld_t)(void* pThis, const void* pSaveName, bool isNewGame);
static LoadGameWorld_t Real_LoadGameWorld = nullptr;

static bool __fastcall Hooked_LoadGameWorld(void* pThis, void* /*edx*/, const void* pSaveName, bool isNewGame) {
    SetEnginePhase(Phase_LoadGameWorld);
    s_inPreLoad = false;
    DynamicLayerOptimizer_OnLoadStart();
    if (Real_EnableSectorActivation) {
        Real_EnableSectorActivation(true);
    }
    s_samplePhase = 1;
    PakTracer_OnLoadGameWorldStart(s_sectorActivateCount == 0);
    QueryPerformanceCounter(&s_tWorldStart);
    if (s_tHUDStart.QuadPart > 0) {
        s_preWorldTotalSec = (double)(s_tWorldStart.QuadPart - s_tHUDStart.QuadPart) / (double)s_freq.QuadPart;
    }
    Log("G3O", "[LoadProfile] -> gCWorld::LoadGameWorld START (pre-load total: %.3f s)", s_preWorldTotalSec);

    LARGE_INTEGER t0, t1;
    LONG pak0 = GetPakDecompCount(); LONG64 pakOut0 = GetPakDecompOutBytes();
    LONG read0 = s_readCalls; LONG64 readBytes0 = s_readBytes;
    QueryPerformanceCounter(&t0);

    bool r = Real_LoadGameWorld(pThis, pSaveName, isNewGame);

    QueryPerformanceCounter(&t1);
    SnapshotPhase(s_statLoadGameWorld, t0, t1, pak0, pakOut0, read0, readBytes0);
    Log("G3O", "[LoadProfile] <- gCWorld::LoadGameWorld FINISHED in %.3f s (result=%d, pak blocks=%ld, read=%.2f MB)",
        s_statLoadGameWorld.durationSec, (int)r, s_statLoadGameWorld.pakBlocks, s_statLoadGameWorld.readMb);
    return r;
}

// 9. GotoLoadedPosition hook
typedef void (__thiscall* GotoLoadedPosition_t)(void* pThis, void* pEntity, bool a);
static GotoLoadedPosition_t Real_GotoLoadedPosition = nullptr;

static void __fastcall Hooked_GotoLoadedPosition(void* pThis, void* /*edx*/, void* pEntity, bool a) {
    if (!s_isLoadActive) { Real_GotoLoadedPosition(pThis, pEntity, a); return; }
    SetEnginePhase(Phase_GotoLoadedPosition);
    LARGE_INTEGER t0, t1;
    LONG pak0 = GetPakDecompCount(); LONG64 pakOut0 = GetPakDecompOutBytes();
    double pakTimeMs0 = GetPakDecompTimeMs();
    LONG read0 = s_readCalls; LONG64 readBytes0 = s_readBytes;
    LONG64 readTimeUs0 = s_readTimeUs;
    QueryPerformanceCounter(&t0);
    Log("G3O", "[LoadProfile] -> gCSession::GotoLoadedPosition START (Resource Streaming / Scene Init)");

    s_samplePhase = 2;
    PakTracer_OnGotoLoadedPositionStart();
    const bool wasCapturingPvsMetrics = s_capturePvsPrefetchMetrics;
    s_capturePvsPrefetchMetrics = true;

    // Batch frustum updates across both GotoLoadedPosition and Session::Start.
    // Flushed at the end of Session::Start before gameplay begins.
    FrustumOptimizer_BeginDefer();
    Real_GotoLoadedPosition(pThis, pEntity, a);

    s_capturePvsPrefetchMetrics = wasCapturingPvsMetrics;

    QueryPerformanceCounter(&t1);
    SnapshotPhase(s_statGotoPos, t0, t1, pak0, pakOut0, read0, readBytes0, pakTimeMs0, readTimeUs0);
    Log("G3O", "[LoadProfile] <- gCSession::GotoLoadedPosition FINISHED in %.3f s (pak blocks=%ld, decomp=%.2f MB, read=%.2f MB)",
        s_statGotoPos.durationSec, s_statGotoPos.pakBlocks, s_statGotoPos.pakOutMb, s_statGotoPos.readMb);
}

// PVS prefetcher call times are inclusive and are scoped to GotoLoadedPosition.
using PvsPrefetchProcessFn = bool (__thiscall*)(void*, void*);
using PvsPrefetchWaitFn = void (__thiscall*)(void*);
static PvsPrefetchProcessFn Real_PvsPrefetchProcess = nullptr;
static PvsPrefetchWaitFn Real_PvsPrefetchWait = nullptr;

static bool __fastcall Hooked_PvsPrefetchProcess(void* pThis, void* /*edx*/, void* pDatabase) {
    if (!s_capturePvsPrefetchMetrics) return Real_PvsPrefetchProcess(pThis, pDatabase);
    const std::uint64_t start = BeginLayerMetric();
    const bool result = Real_PvsPrefetchProcess(pThis, pDatabase);
    const std::uint64_t end = BeginLayerMetric();
    RecordPvsPrefetchDuration(
        s_pvsPrefetchMetrics, PvsPrefetchOperation::Process, start, end);
    return result;
}

static void __fastcall Hooked_PvsPrefetchWait(void* pThis, void* /*edx*/) {
    if (!s_capturePvsPrefetchMetrics) {
        Real_PvsPrefetchWait(pThis);
        return;
    }
    const std::uint64_t start = BeginLayerMetric();
    Real_PvsPrefetchWait(pThis);
    const std::uint64_t end = BeginLayerMetric();
    RecordPvsPrefetchDuration(
        s_pvsPrefetchMetrics, PvsPrefetchOperation::WaitForEmptyThreadQueue, start, end);
}

// 10. SessionStart hook
typedef void (__thiscall* SessionStart_t)(void* pThis, int mode);
static SessionStart_t Real_SessionStart = nullptr;

static void __fastcall Hooked_SessionStart(void* pThis, void* /*edx*/, int mode) {
    if (!s_isLoadActive) {
        Real_SessionStart(pThis, mode);
        ProfilerSetGameplayReady(true);
        return;
    }
    LARGE_INTEGER t0, t1;
    LONG pak0 = GetPakDecompCount(); LONG64 pakOut0 = GetPakDecompOutBytes();
    LONG read0 = s_readCalls; LONG64 readBytes0 = s_readBytes;
    QueryPerformanceCounter(&t0);
    Log("G3O", "[LoadProfile] -> gCSession::Start (mode=%d) START", mode);

    s_samplePhase = 3;
    Real_SessionStart(pThis, mode);

    // Finalize and flush all deferred spatial and frustum updates before gameplay starts
    FrustumOptimizer_EndDeferAndFlush();

    ProfilerSetGameplayReady(true);

    QueryPerformanceCounter(&t1);
    SnapshotPhase(s_statSessionStart, t0, t1, pak0, pakOut0, read0, readBytes0);
    Log("G3O", "[LoadProfile] <- gCSession::Start FINISHED in %.3f s (pak blocks=%ld, read=%.2f MB)",
        s_statSessionStart.durationSec, s_statSessionStart.pakBlocks, s_statSessionStart.readMb);
}

// 11. WorldActivate hook
typedef int (__thiscall* WorldActivate_t)(void* pThis);
static WorldActivate_t Real_WorldActivate = nullptr;

static int __fastcall Hooked_WorldActivate(void* pThis, void* /*edx*/) {
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);

    int r = Real_WorldActivate(pThis);

    QueryPerformanceCounter(&t1);
    s_statWorldActivate.durationSec = (double)(t1.QuadPart - t0.QuadPart) / (double)s_freq.QuadPart;
    return r;
}

// 12. BuildStaticHierachy hook
typedef void (__thiscall* BuildStaticHierachy_t)(void* pThis);
static BuildStaticHierachy_t Real_BuildStaticHierachy = nullptr;

static void __fastcall Hooked_BuildStaticHierachy(void* pThis, void* /*edx*/) {
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);

    Real_BuildStaticHierachy(pThis);

    QueryPerformanceCounter(&t1);
    InterlockedIncrement(&s_cellsProcessed);
    LONG64 us = (s_freq.QuadPart > 0) ? ((t1.QuadPart - t0.QuadPart) * 1000000LL / s_freq.QuadPart) : 0;
    InterlockedExchangeAdd64(&s_hierarchyTimeUs, us);
}

static void* FindHUDLoadGameTarget(HMODULE hGame) {
    if (!hGame) return nullptr;

    const BYTE sig[] = {
        0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x8B, 0x45, 0x08,
        0x83, 0xEC, 0x5C, 0x83, 0x78, 0x04, 0x00, 0x53, 0x56, 0x57
    };
    const size_t sigLen = sizeof(sig);

    BYTE* targetRva = (BYTE*)hGame + 0xA9A10;
    if (memcmp(targetRva, sig, sigLen) == 0) {
        return targetRva;
    }

    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)hGame;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((BYTE*)hGame + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

    IMAGE_SECTION_HEADER* sect = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (memcmp(sect[i].Name, ".text", 5) == 0) {
            BYTE* start = (BYTE*)hGame + sect[i].VirtualAddress;
            DWORD size = sect[i].Misc.VirtualSize;
            if (size > sigLen) {
                for (DWORD offset = 0; offset <= size - sigLen; ++offset) {
                    if (memcmp(start + offset, sig, sigLen) == 0) {
                        return start + offset;
                    }
                }
            }
        }
    }

    return nullptr;
}

static void* s_targetReadFile = nullptr;
static void* s_targetHUDLoadGame = nullptr;
static void* s_targetHeader = nullptr;
static void* s_targetSessionStop = nullptr;
static void* s_targetClearEntities = nullptr;
static void* s_targetUnloadCaches = nullptr;
static void* s_targetLoadGameWorld = nullptr;
static void* s_targetGotoPos = nullptr;
static void* s_targetSessionStart = nullptr;
static void* s_targetWorldActivate = nullptr;
static void* s_targetBuildStatic = nullptr;

static bool InstallSavegameHook(
    void* target, LPVOID detour, LPVOID* original, void** targetSlot, const char* methodName) {
    if (!original || !targetSlot) return false;
    if (!target) {
        *original = nullptr;
        Log("G3O", "Savegame Profiler: %s target not found; hook unavailable.", methodName);
        return false;
    }
    if (*targetSlot) {
        if (*targetSlot == target) return true;
        Log("G3O", "Savegame Profiler: %s hook already has a different target; installation skipped.", methodName);
        return false;
    }

    MH_STATUS createStatus = MH_OK;
    MH_STATUS enableStatus = MH_OK;
    MH_STATUS removeStatus = MH_OK;
    const HookInstallResult result = InstallHookSafely(
        [&] {
            createStatus = MH_CreateHook(target, detour, original);
            return createStatus == MH_OK;
        },
        [&] {
            enableStatus = MH_EnableHook(target);
            return enableStatus == MH_OK;
        },
        [&] { removeStatus = MH_RemoveHook(target); },
        [&] { *original = nullptr; });

    if (result == HookInstallResult::CreateFailed) {
        Log("G3O", "Savegame Profiler: %s hook creation failed (status=%d).",
            methodName, static_cast<int>(createStatus));
        return false;
    }
    if (result == HookInstallResult::EnableFailed) {
        Log("G3O", "Savegame Profiler: %s hook enable failed (status=%d).",
            methodName, static_cast<int>(enableStatus));
        if (removeStatus != MH_OK) {
            Log("G3O", "Savegame Profiler: %s failed hook cleanup (status=%d).",
                methodName, static_cast<int>(removeStatus));
        }
        return false;
    }

    *targetSlot = target;
    Log("G3O", "Savegame Profiler: Hooked %s (0x%p)", methodName, target);
    return true;
}

static bool InstallPvsMetricHook(
    HMODULE module, const char* exportedName, LPVOID detour, LPVOID* original,
    void** targetSlot, const char* methodName) {
    void* target = module ? reinterpret_cast<void*>(GetProcAddress(module, exportedName)) : nullptr;
    return InstallSavegameHook(target, detour, original, targetSlot, methodName);
}

bool InitializeSavegameProfiler() {
    const Config& cfg = GetConfig();
    if (cfg.enablePakPrefetch && !cfg.enableFastPakDecompressor) {
        Log("G3O", "[PakCache] Trace-based pre-decompression requires EnableFastPakDecompressor=true; it is disabled.");
    }
    if (!ShouldInstallSavegameProfilerHooks(cfg)) {
        Log("G3O", "Savegame profiler hooks disabled by configuration.");
        return true;
    }
    if (s_freq.QuadPart <= 0) {
        QueryPerformanceFrequency(&s_freq);
    }
    const bool metricsHooksReady = (cfg.enableSavegameAudit && cfg.enableLayerAudit) ? InitMetricsHooks() : false;
    if (s_freq.QuadPart <= 0) {
        Log("G3O", "Savegame Profiler initialization aborted: QPC frequency unavailable.");
        return false;
    }

    if (cfg.enableSavegameAudit || ShouldTracePakPrefetch(cfg) || cfg.enableParallelDynamicLayers) {
        if (cfg.enableSavegameAudit || ShouldTracePakPrefetch(cfg)) {
            HMODULE hK32 = GetModuleHandleA("kernel32.dll");
            if (hK32) {
                void* pRead = (void*)GetProcAddress(hK32, "ReadFile");
                if (pRead) {
                    InstallSavegameHook(pRead, (LPVOID)Hooked_ReadFile, (LPVOID*)&Real_ReadFile,
                        &s_targetReadFile, "kernel32!ReadFile");
                }
            }
        }
        HMODULE hGame = GetModuleHandleA("Game.dll");
        if (hGame) {
            void* pHeader = (void*)GetProcAddress(hGame, "?GetSaveGameHeader@gCWorld@@QAE?AW4bEResult@@ABVbCString@@AAVgCSaveGameHeader@1@@Z");
            if (pHeader && !s_targetHeader) {
                InstallSavegameHook(pHeader, (LPVOID)Hooked_GetSaveGameHeader, (LPVOID*)&Real_GetSaveGameHeader,
                    &s_targetHeader, "gCWorld::GetSaveGameHeader");
            }
            void* pWorld = (void*)GetProcAddress(hGame, "?LoadGameWorld@gCWorld@@QAE_NABVbCString@@_N@Z");
            if (pWorld && !s_targetLoadGameWorld) {
                InstallSavegameHook(pWorld, (LPVOID)Hooked_LoadGameWorld, (LPVOID*)&Real_LoadGameWorld,
                    &s_targetLoadGameWorld, "gCWorld::LoadGameWorld");
            }
            void* pGoto = (void*)GetProcAddress(hGame, "?GotoLoadedPosition@gCSession@@QAEXPAVeCEntity@@_N@Z");
            if (pGoto && !s_targetGotoPos) {
                InstallSavegameHook(pGoto, (LPVOID)Hooked_GotoLoadedPosition, (LPVOID*)&Real_GotoLoadedPosition,
                    &s_targetGotoPos, "gCSession::GotoLoadedPosition");
            }
            void* pOnAct = (void*)GetProcAddress(hGame, "?OnActivate@gCWorld@@MAEXXZ");
            if (pOnAct && !s_targetWorldOnActivate) {
                InstallSavegameHook(pOnAct, (LPVOID)Hooked_WorldOnActivate, (LPVOID*)&Real_WorldOnActivate,
                    &s_targetWorldOnActivate, "gCWorld::OnActivate");
            }
        }
    }

    HMODULE hGame = GetModuleHandleA("Game.dll");
    if (hGame) {
        if (cfg.enableSavegameAudit || cfg.enableProfiler || ShouldTracePakPrefetch(cfg)) {
            void* pHUD = FindHUDLoadGameTarget(hGame);
            if (pHUD) {
                InstallSavegameHook(pHUD, (LPVOID)Hooked_HUDLoadGame, (LPVOID*)&Real_HUDLoadGame,
                    &s_targetHUDLoadGame, "gCHUDFileManager::LoadGame");
            }
        }

        if (cfg.enableSavegameAudit) {
        void* pHeader = (void*)GetProcAddress(hGame, "?GetSaveGameHeader@gCWorld@@QAE?AW4bEResult@@ABVbCString@@AAVgCSaveGameHeader@1@@Z");
        if (pHeader && !s_targetHeader) {
            InstallSavegameHook(pHeader, (LPVOID)Hooked_GetSaveGameHeader, (LPVOID*)&Real_GetSaveGameHeader,
                &s_targetHeader, "gCWorld::GetSaveGameHeader");
        }

        void* pStop = (void*)GetProcAddress(hGame, "?Stop@gCSession@@QAEXXZ");
        if (pStop) {
            InstallSavegameHook(pStop, (LPVOID)Hooked_SessionStop, (LPVOID*)&Real_SessionStop,
                &s_targetSessionStop, "gCSession::Stop");
        }

        void* pClear = (void*)GetProcAddress(hGame, "?ClearAllEntities@gCGUIManager@@QAE_NXZ");
        if (pClear) {
            InstallSavegameHook(pClear, (LPVOID)Hooked_ClearAllEntities, (LPVOID*)&Real_ClearAllEntities,
                &s_targetClearEntities, "gCGUIManager::ClearAllEntities");
        }

        void* pUnload = (void*)GetProcAddress(hGame, "?UnloadCaches@gCWorld@@QAEXXZ");
        if (pUnload) {
            InstallSavegameHook(pUnload, (LPVOID)Hooked_UnloadCaches, (LPVOID*)&Real_UnloadCaches,
                &s_targetUnloadCaches, "gCWorld::UnloadCaches");
        }

        void* pWorld = (void*)GetProcAddress(hGame, "?LoadGameWorld@gCWorld@@QAE_NABVbCString@@_N@Z");
        if (pWorld && !s_targetLoadGameWorld) {
            InstallSavegameHook(pWorld, (LPVOID)Hooked_LoadGameWorld, (LPVOID*)&Real_LoadGameWorld,
                &s_targetLoadGameWorld, "gCWorld::LoadGameWorld");
        }

        void* pGoto = (void*)GetProcAddress(hGame, "?GotoLoadedPosition@gCSession@@QAEXPAVeCEntity@@_N@Z");
        if (pGoto && !s_targetGotoPos) {
            InstallSavegameHook(pGoto, (LPVOID)Hooked_GotoLoadedPosition, (LPVOID*)&Real_GotoLoadedPosition,
                &s_targetGotoPos, "gCSession::GotoLoadedPosition");
        }

        void* pStart = (void*)GetProcAddress(hGame, "?Start@gCSession@@QAEXW4gESession_StartMode@@@Z");
        if (pStart) {
            InstallSavegameHook(pStart, (LPVOID)Hooked_SessionStart, (LPVOID*)&Real_SessionStart,
                &s_targetSessionStart, "gCSession::Start");
        }

        void* pAct = (void*)GetProcAddress(hGame, "?WorldActivate@gCSession@@UAE?AW4bEResult@@XZ");
        if (pAct) {
            InstallSavegameHook(pAct, (LPVOID)Hooked_WorldActivate, (LPVOID*)&Real_WorldActivate,
                &s_targetWorldActivate, "gCSession::WorldActivate");
        }

        void* pWorldAct = (void*)GetProcAddress(hGame, "?Activate@gCWorld@@UAE?AW4eEResult@eCProcessibleElement@@XZ");
        if (pWorldAct) {
            InstallSavegameHook(pWorldAct, (LPVOID)Hooked_WorldActivateElem, (LPVOID*)&Real_WorldActivateElem,
                &s_targetWorldActivateElem, "gCWorld::Activate");
        }

        void* pEnableSec = (void*)GetProcAddress(hGame, "?EnableSectorActivation@gCSector@@SGX_N@Z");
        if (pEnableSec) {
            Real_EnableSectorActivation = (EnableSectorActivation_t)pEnableSec;
            Log("G3O", "Savegame Profiler: Found gCSector::EnableSectorActivation (0x%p)", pEnableSec);
        }

        void* pSecOnAct = (void*)GetProcAddress(hGame, "?OnActivate@gCSector@@MAEXXZ");
        if (pSecOnAct) {
            InstallSavegameHook(pSecOnAct, (LPVOID)Hooked_SectorOnActivate, (LPVOID*)&Real_SectorOnActivate,
                &s_targetSectorOnActivate, "gCSector::OnActivate");
        }

        void* pSecDoLoad = (void*)GetProcAddress(hGame, "?DoLoadData@gCSector@@MAE?AW4eEResult@eCProcessibleElement@@AAVbCIStream@@@Z");
        if (pSecDoLoad) {
            InstallSavegameHook(pSecDoLoad, (LPVOID)Hooked_SectorDoLoadData, (LPVOID*)&Real_SectorDoLoadData,
                &s_targetSectorDoLoadData, "gCSector::DoLoadData");
        }

        void* pOnAct = (void*)GetProcAddress(hGame, "?OnActivate@gCWorld@@MAEXXZ");
        if (pOnAct && !s_targetWorldOnActivate) {
            InstallSavegameHook(pOnAct, (LPVOID)Hooked_WorldOnActivate, (LPVOID*)&Real_WorldOnActivate,
                &s_targetWorldOnActivate, "gCWorld::OnActivate");
        }

        void* pWorldDoLoad = (void*)GetProcAddress(hGame, "?DoLoadData@gCWorld@@MAE?AW4eEResult@eCProcessibleElement@@AAVbCIStream@@@Z");
        if (pWorldDoLoad) {
            InstallSavegameHook(pWorldDoLoad, (LPVOID)Hooked_WorldDoLoadData, (LPVOID*)&Real_WorldDoLoadData,
                &s_targetWorldDoLoadData, "gCWorld::DoLoadData");
        }

        void* pDoAfter = (void*)GetProcAddress(hGame, "?DoAfterWorldActivate@gCWorld@@IAEXXZ");
        if (pDoAfter) {
            InstallSavegameHook(pDoAfter, (LPVOID)Hooked_DoAfterWorldActivate, (LPVOID*)&Real_DoAfterWorldActivate,
                &s_targetDoAfterWorldActivate, "gCWorld::DoAfterWorldActivate");
        }
        }

        void* pCount = (void*)GetProcAddress(hGame, "?GetSectorCount@gCWorld@@QBEHXZ");
        if (pCount) {
            Real_GetSectorCount = (GetSectorCount_t)pCount;
            Log("G3O", "Savegame Profiler: Found gCWorld::GetSectorCount (0x%p)", pCount);
        }

        void* pDynCount = (void*)GetProcAddress(hGame, "?GetDynamicLayerCount@gCSector@@QBEHXZ");
        if (pDynCount) {
            Real_GetDynamicLayerCount = (GetDynamicLayerCount_t)pDynCount;
            Log("G3O", "Savegame Profiler: Found gCSector::GetDynamicLayerCount (0x%p)", pDynCount);
        }
    }

    if (cfg.enableLayerRingArena && cfg.enableFastPakDecompressor && !cfg.enableSavegameAudit) {
        HMODULE hGameForLayerArena = GetModuleHandleA("Game.dll");
        void* pSecOnAct = hGameForLayerArena
            ? reinterpret_cast<void*>(GetProcAddress(hGameForLayerArena, "?OnActivate@gCSector@@MAEXXZ"))
            : nullptr;
        InstallSavegameHook(pSecOnAct, (LPVOID)Hooked_SectorOnActivate,
            (LPVOID*)&Real_SectorOnActivate, &s_targetSectorOnActivate, "gCSector::OnActivate (LayerRingArena)");
    }

    if (cfg.enableSavegameAudit) {
    HMODULE hEngine = GetModuleHandleA("Engine.dll");
    HMODULE hGame = GetModuleHandleA("Game.dll");
    if (hEngine && hGame) {
        const uintptr_t gameBase = reinterpret_cast<uintptr_t>(hGame);
        s_retSectorBaseOnActivate = gameBase + 0x19419B;
        s_retDynLayerActivate     = gameBase + 0x1941EC;
        s_retStaticLayerActivate  = gameBase + 0x19425C;

        void* pElemAct = (void*)GetProcAddress(hEngine, "?Activate@eCProcessibleElement@@UAE?AW4eEResult@1@XZ");
        if (pElemAct) {
            InstallSavegameHook(pElemAct, (LPVOID)Hooked_ElementActivate, (LPVOID*)&Real_ElementActivate,
                &s_targetElementActivate, "eCProcessibleElement::Activate");
        }

        void* pGeomAct = (void*)GetProcAddress(hGame, "?Activate@gCGeometryLayer@@UAE?AW4eEResult@eCProcessibleElement@@XZ");
        if (!pGeomAct) {
            pGeomAct = reinterpret_cast<void*>(gameBase + 0x0C7660);
        }
        if (pGeomAct) {
            InstallSavegameHook(pGeomAct, (LPVOID)Hooked_GeometryLayerActivate,
                (LPVOID*)&Real_GeometryLayerActivate, &s_targetGeometryLayerActivate,
                "gCGeometryLayer::Activate");
        }

        void* pElemOnAct = (void*)GetProcAddress(hEngine, "?OnActivate@eCProcessibleElement@@MAEXXZ");
        if (pElemOnAct) {
            InstallSavegameHook(pElemOnAct, (LPVOID)Hooked_ElementOnActivate,
                (LPVOID*)&Real_ElementOnActivate, &s_targetElementOnActivate,
                "eCProcessibleElement::OnActivate");
        }
    }
    if (hEngine) {
        void* pBuild = (void*)GetProcAddress(hEngine, "?BuildStaticHierachy@eCPVSCellItem@@IAEXXZ");
        if (pBuild) {
            InstallSavegameHook(pBuild, (LPVOID)Hooked_BuildStaticHierachy,
                (LPVOID*)&Real_BuildStaticHierachy, &s_targetBuildStatic,
                "eCPVSCellItem::BuildStaticHierarchy");
        }

        InstallPvsMetricHook(
            hEngine,
            "?Process@eCPVSPrefetcher3@@QAE_NAAVeCFrustumDatabase@@@Z",
            (LPVOID)Hooked_PvsPrefetchProcess,
            (LPVOID*)&Real_PvsPrefetchProcess,
            &s_targetPvsProcess,
            "eCPVSPrefetcher3::Process");
        InstallPvsMetricHook(
            hEngine,
            "?WaitForEmptyThreadQueue@eCPVSPrefetcher3@@QAEXXZ",
            (LPVOID)Hooked_PvsPrefetchWait,
            (LPVOID*)&Real_PvsPrefetchWait,
            &s_targetPvsWaitForEmptyThreadQueue,
            "eCPVSPrefetcher3::WaitForEmptyThreadQueue");

        void* pDeact = (void*)GetProcAddress(hEngine, "?Deactivate@eCProcessibleElement@@QAE?AW4eEResult@1@XZ");
        if (pDeact) {
            InstallSavegameHook(pDeact, (LPVOID)Hooked_Deactivate, (LPVOID*)&Real_Deactivate,
                &s_targetDeactivate, "eCProcessibleElement::Deactivate");
        }

        void* pROI = (void*)GetProcAddress(hEngine, "?FourceFullROIUpdate@eCEntityAdmin@@QAEXXZ");
        if (pROI) {
            InstallSavegameHook(pROI, (LPVOID)Hooked_ForceROIUpdate, (LPVOID*)&Real_ForceROIUpdate,
                &s_targetForceROIUpdate, "eCEntityAdmin::ForceFullROIUpdate");
        }

    }
    }

    const LayerAuditHookState auditHookState = ResolveLayerAuditHookState(
        cfg.enableSavegameAudit && cfg.enableLayerAudit, metricsHooksReady && s_metricsHooksInitialized);
    Log("G3O", "Savegame hooks initialized (audit=%s, profiler=%s, PAK prefetch=%s, LayerAudit hooks=%s).",
        cfg.enableSavegameAudit ? "enabled" : "disabled",
        cfg.enableProfiler ? "enabled" : "disabled",
        cfg.enablePakPrefetch ? "enabled" : "disabled",
        LayerAuditHookStateName(auditHookState));
    return true;
}

void ShutdownSavegameProfiler() {
    ShutdownMetricsHooks();
    if (s_targetReadFile) MH_DisableHook(s_targetReadFile);
    if (s_targetHUDLoadGame) MH_DisableHook(s_targetHUDLoadGame);
    if (s_targetHeader) MH_DisableHook(s_targetHeader);
    if (s_targetSessionStop) MH_DisableHook(s_targetSessionStop);
    if (s_targetClearEntities) MH_DisableHook(s_targetClearEntities);
    if (s_targetDeactivate) MH_DisableHook(s_targetDeactivate);
    if (s_targetWorldActivateElem) MH_DisableHook(s_targetWorldActivateElem);
    if (s_targetWorldOnActivate) MH_DisableHook(s_targetWorldOnActivate);
    if (s_targetDoAfterWorldActivate) MH_DisableHook(s_targetDoAfterWorldActivate);
    if (s_targetForceROIUpdate) MH_DisableHook(s_targetForceROIUpdate);
    if (s_targetUnloadCaches) MH_DisableHook(s_targetUnloadCaches);
    if (s_targetLoadGameWorld) MH_DisableHook(s_targetLoadGameWorld);
    if (s_targetGotoPos) MH_DisableHook(s_targetGotoPos);
    if (s_targetSessionStart) MH_DisableHook(s_targetSessionStart);
    if (s_targetSectorOnActivate) MH_DisableHook(s_targetSectorOnActivate);
    if (s_targetWorldActivate) MH_DisableHook(s_targetWorldActivate);
    if (s_targetBuildStatic) MH_DisableHook(s_targetBuildStatic);
    if (s_targetPvsProcess) MH_DisableHook(s_targetPvsProcess);
    if (s_targetPvsWaitForEmptyThreadQueue) MH_DisableHook(s_targetPvsWaitForEmptyThreadQueue);
    if (s_targetWorldDoLoadData) MH_DisableHook(s_targetWorldDoLoadData);
    if (s_targetSectorDoLoadData) MH_DisableHook(s_targetSectorDoLoadData);
    if (s_targetElementActivate) MH_DisableHook(s_targetElementActivate);
    if (s_targetGeometryLayerActivate) {
        MH_DisableHook(s_targetGeometryLayerActivate);
        s_targetGeometryLayerActivate = nullptr;
    }
    Real_GeometryLayerActivate = nullptr;
    if (s_targetElementOnActivate) MH_DisableHook(s_targetElementOnActivate);
}

} // namespace G3O
