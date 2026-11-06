cpp_path = r'E:\Projects\G3O\src\savegame_profiler.cpp'
with open(cpp_path, 'r', encoding='utf-8') as f:
    lines = f.readlines()

start_idx = -1
end_idx = -1

for i, line in enumerate(lines):
    if line.startswith('struct LayerAuditMetric {'):
        start_idx = i
        break

for i in range(start_idx, len(lines)):
    if lines[i].startswith('static BOOL WINAPI Hooked_ReadFile'):
        end_idx = i - 1
        # Trim blank lines backwards
        while end_idx > start_idx and lines[end_idx].strip() == '':
            end_idx -= 1
        end_idx += 1  # end_idx is exclusive
        break

print(f"Replacing lines {start_idx + 1} to {end_idx}")

new_code_lines = '''struct LayerAuditMetric {
    std::uint64_t calls = 0;
    std::uint64_t qpcTicks = 0;
};

struct LayerAuditMetrics {
    LayerAuditMetric createData;
    LayerAuditMetric doLoadData;
    LayerAuditMetric createDynamicEntity;
    LayerAuditMetric createFromTemplate;
};

struct ContextAuditMetrics {
    LayerAuditMetric cpoReadFile;          // bCPOStream::ReadFile (SharedBase.dll)
    LayerAuditMetric createSpecificEntity; // eCEntityDynamicContext::CreateSpecificEntity (Engine.dll)
    LayerAuditMetric dynEntityRead;        // eCDynamicEntity::Read (Engine.dll RVA 0x2295C0)
    LayerAuditMetric entityRead;           // eCEntity::Read (Engine.dll RVA 0x2257F0)
    LayerAuditMetric patchWithTemplate;    // eCEntity::PatchWithTemplate (Engine.dll RVA 0x224350)
    LayerAuditMetric readDependency;       // eCContextBase::ReadDependency (Engine.dll RVA 0x21A150)
    LayerAuditMetric childrenAvailable;    // eCEntity::ChildrenAvailable (Engine.dll)
};

static thread_local LayerAuditMetrics s_layerAuditMetrics{};
static thread_local ContextAuditMetrics s_contextAuditMetrics{};

// Original function prototypes
using LayerCreateDataFn = std::int32_t (__thiscall*)(void*);
using DynamicLayerDoLoadDataFn = std::int32_t (__thiscall*)(void*, void*);
using DynamicLayerCreateEntityFn = void* (__thiscall*)(void*);
using EntityCreateFromTemplateFn = bool (__thiscall*)(void*, const void*);

using OnLoadDynamicContextFn = bool (__thiscall*)(void*, void*, void*, bool);
using CPOStreamReadFileFn = bool (__thiscall*)(void*, void*);
using CreateSpecificEntityFn = void* (__thiscall*)(void*);
using DynamicEntityReadFn = int (__thiscall*)(void*, void*);
using EntityReadFn = int (__thiscall*)(void*, void*);
using PatchWithTemplateFn = bool (__thiscall*)(void*, const void*, bool);
using ReadDependencyFn = int (__thiscall*)(void*, void*, void*);
using ChildrenAvailableFn = void (__thiscall*)(void*, bool);

static LayerCreateDataFn Real_LayerCreateData = nullptr;
static DynamicLayerDoLoadDataFn Real_DynamicLayerDoLoadData = nullptr;
static DynamicLayerCreateEntityFn Real_DynamicLayerCreateEntity = nullptr;
static EntityCreateFromTemplateFn Real_EntityCreateFromTemplate = nullptr;

static OnLoadDynamicContextFn Real_OnLoadDynamicContext = nullptr;
static CPOStreamReadFileFn Real_CPOStreamReadFile = nullptr;
static CreateSpecificEntityFn Real_CreateSpecificEntity = nullptr;
static DynamicEntityReadFn Real_DynamicEntityRead = nullptr;
static EntityReadFn Real_EntityRead = nullptr;
static PatchWithTemplateFn Real_PatchWithTemplate = nullptr;
static ReadDependencyFn Real_ReadDependency = nullptr;
static ChildrenAvailableFn Real_ChildrenAvailable = nullptr;

static LPVOID s_metricsHookTargets[16] = {};
static int s_numMetricsHooks = 0;
static bool s_metricsHooksInitialized = false;

static std::uint64_t BeginLayerMetric() {
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return static_cast<std::uint64_t>(t.QuadPart);
}

static void EndLayerMetric(LayerAuditMetric& metric, std::uint64_t start) {
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    metric.qpcTicks += static_cast<std::uint64_t>(t.QuadPart) - start;
    ++metric.calls;
}

static double LayerMetricMs(std::uint64_t ticks) {
    return s_freq.QuadPart > 0
        ? static_cast<double>(ticks) * 1000.0 / static_cast<double>(s_freq.QuadPart)
        : 0.0;
}

static LayerAuditMetrics SubtractLayerMetrics(
    const LayerAuditMetrics& after, const LayerAuditMetrics& before) {
    LayerAuditMetrics delta{};
    delta.createData.calls = after.createData.calls - before.createData.calls;
    delta.createData.qpcTicks = after.createData.qpcTicks - before.createData.qpcTicks;
    delta.doLoadData.calls = after.doLoadData.calls - before.doLoadData.calls;
    delta.doLoadData.qpcTicks = after.doLoadData.qpcTicks - before.doLoadData.qpcTicks;
    delta.createDynamicEntity.calls = after.createDynamicEntity.calls - before.createDynamicEntity.calls;
    delta.createDynamicEntity.qpcTicks = after.createDynamicEntity.qpcTicks - before.createDynamicEntity.qpcTicks;
    delta.createFromTemplate.calls = after.createFromTemplate.calls - before.createFromTemplate.calls;
    delta.createFromTemplate.qpcTicks = after.createFromTemplate.qpcTicks - before.createFromTemplate.qpcTicks;
    return delta;
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
    const std::uint64_t start = BeginLayerMetric();
    const std::int32_t result = Real_DynamicLayerDoLoadData(self, streamReferenceSlot);
    EndLayerMetric(s_layerAuditMetrics.doLoadData, start);
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

    const ContextAuditMetrics before = s_contextAuditMetrics;
    const std::uint64_t start = BeginLayerMetric();

    const bool result = Real_OnLoadDynamicContext(self, pStream, pLayer, directRead);

    const std::uint64_t elapsedTicks = BeginLayerMetric() - start;
    const double totalMs = LayerMetricMs(elapsedTicks);

    if (totalMs >= 20.0) { // Log any layer taking >= 20 ms
        const char* filePath = "<unknown>";
        void* pContext = pLayer ? *reinterpret_cast<void**>(reinterpret_cast<char*>(pLayer) + 0x40) : nullptr;
        if (pContext) {
            const char** ppStr = reinterpret_cast<const char**>(reinterpret_cast<char*>(pContext) + 0x50);
            if (ppStr && *ppStr) filePath = *ppStr;
        }

        const double cpoMs = LayerMetricMs(s_contextAuditMetrics.cpoReadFile.qpcTicks - before.cpoReadFile.qpcTicks);
        const uint64_t cpoCalls = s_contextAuditMetrics.cpoReadFile.calls - before.cpoReadFile.calls;

        const double createMs = LayerMetricMs(s_contextAuditMetrics.createSpecificEntity.qpcTicks - before.createSpecificEntity.qpcTicks);
        const uint64_t createCalls = s_contextAuditMetrics.createSpecificEntity.calls - before.createSpecificEntity.calls;

        const double dynReadMs = LayerMetricMs(s_contextAuditMetrics.dynEntityRead.qpcTicks - before.dynEntityRead.qpcTicks);
        const uint64_t dynReadCalls = s_contextAuditMetrics.dynEntityRead.calls - before.dynEntityRead.calls;

        const double entReadMs = LayerMetricMs(s_contextAuditMetrics.entityRead.qpcTicks - before.entityRead.qpcTicks);
        const uint64_t entReadCalls = s_contextAuditMetrics.entityRead.calls - before.entityRead.calls;

        const double patchMs = LayerMetricMs(s_contextAuditMetrics.patchWithTemplate.qpcTicks - before.patchWithTemplate.qpcTicks);
        const uint64_t patchCalls = s_contextAuditMetrics.patchWithTemplate.calls - before.patchWithTemplate.calls;

        const double depMs = LayerMetricMs(s_contextAuditMetrics.readDependency.qpcTicks - before.readDependency.qpcTicks);
        const uint64_t depCalls = s_contextAuditMetrics.readDependency.calls - before.readDependency.calls;

        const double childMs = LayerMetricMs(s_contextAuditMetrics.childrenAvailable.qpcTicks - before.childrenAvailable.qpcTicks);
        const uint64_t childCalls = s_contextAuditMetrics.childrenAvailable.calls - before.childrenAvailable.calls;

        Log("G3O", "[DynamicContextAudit] Layer %p ('%s') directRead=%d | Total=%.2f ms | CPOReadFile=%.2f ms (%llu calls) | Entities=%llu (Create=%.2f ms, DynRead=%.2f ms [EntRead=%.2f ms, Patch=%.2f ms]) | ReadDep=%.2f ms (%llu calls) | ChildrenAvail=%.2f ms (%llu calls)",
            pLayer, filePath, (int)directRead, totalMs,
            cpoMs, cpoCalls,
            createCalls, createMs, dynReadMs, entReadMs, patchMs,
            depMs, depCalls,
            childMs, childCalls);
    }

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

static bool __fastcall Hooked_PatchWithTemplate(void* self, void* /*edx*/, const void* pGuid, bool flag) {
    if (!s_isLoadActive) return Real_PatchWithTemplate(self, pGuid, flag);
    const std::uint64_t start = BeginLayerMetric();
    const bool result = Real_PatchWithTemplate(self, pGuid, flag);
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
    if (s_numMetricsHooks >= 16) {
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
    Real_CPOStreamReadFile = nullptr;
    Real_CreateSpecificEntity = nullptr;
    Real_DynamicEntityRead = nullptr;
    Real_EntityRead = nullptr;
    Real_PatchWithTemplate = nullptr;
    Real_ReadDependency = nullptr;
    Real_ChildrenAvailable = nullptr;

    s_metricsHooksInitialized = false;
}

static bool InitMetricsHooks() {
    if (s_metricsHooksInitialized) return true;

    if (!s_qpcFrequencyInitialized) {
        if (!QueryPerformanceFrequency(&s_freq) || s_freq.QuadPart <= 0) {
            Log("G3O", "[LayerAudit] QueryPerformanceFrequency failed; metrics hooks not installed.");
            return false;
        }
        s_qpcFrequencyInitialized = true;
    }

    HMODULE hGame = GetModuleHandleA("Game.dll");
    HMODULE hEngine = GetModuleHandleA("Engine.dll");
    HMODULE hSharedBase = GetModuleHandleA("SharedBase.dll");
    if (!hGame || !hEngine || !hSharedBase) {
        Log("G3O", "[LayerAudit] Required game DLLs unavailable; metrics hooks not installed.");
        return false;
    }

    uintptr_t engineBase = reinterpret_cast<uintptr_t>(hEngine);
    bool created = true;

    // 1. Layer base / Sector hooks
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
        reinterpret_cast<LPVOID>(GetProcAddress(hGame, "?CreateDynamicEntity@gCDynamicLayer@@QAEPAVeCDynamicEntity@@XZ")),
        reinterpret_cast<LPVOID>(Hooked_DynamicLayerCreateEntity),
        reinterpret_cast<LPVOID*>(&Real_DynamicLayerCreateEntity),
        "gCDynamicLayer::CreateDynamicEntity") && created;

    created = InstallHookTarget(
        reinterpret_cast<LPVOID>(GetProcAddress(hEngine, "?CreateFromTemplate@eCEntity@@QAE_NABVeCTemplateEntity@@@Z")),
        reinterpret_cast<LPVOID>(Hooked_EntityCreateFromTemplate),
        reinterpret_cast<LPVOID*>(&Real_EntityCreateFromTemplate),
        "eCEntity::CreateFromTemplate") && created;

    // 2. Dynamic context sub-phase hooks
    created = InstallHookTarget(
        reinterpret_cast<LPVOID>(GetProcAddress(hGame, "?OnLoadDynamicContext@gCWorld@@IAE_NAAVbCIStream@@AAVgCDynamicLayer@@_N@Z")),
        reinterpret_cast<LPVOID>(Hooked_OnLoadDynamicContext),
        reinterpret_cast<LPVOID*>(&Real_OnLoadDynamicContext),
        "gCWorld::OnLoadDynamicContext") && created;

    created = InstallHookTarget(
        reinterpret_cast<LPVOID>(GetProcAddress(hSharedBase, "?ReadFile@bCPOStream@@QAE_NAAVbCIStream@@@Z")),
        reinterpret_cast<LPVOID>(Hooked_CPOStreamReadFile),
        reinterpret_cast<LPVOID*>(&Real_CPOStreamReadFile),
        "bCPOStream::ReadFile") && created;

    created = InstallHookTarget(
        reinterpret_cast<LPVOID>(GetProcAddress(hEngine, "?CreateSpecificEntity@eCEntityDynamicContext@@UAEPAVeCDynamicEntity@@XZ")),
        reinterpret_cast<LPVOID>(Hooked_CreateSpecificEntity),
        reinterpret_cast<LPVOID*>(&Real_CreateSpecificEntity),
        "eCEntityDynamicContext::CreateSpecificEntity") && created;

    created = InstallHookTarget(
        reinterpret_cast<LPVOID>(engineBase + 0x2295C0),
        reinterpret_cast<LPVOID>(Hooked_DynamicEntityRead),
        reinterpret_cast<LPVOID*>(&Real_DynamicEntityRead),
        "eCDynamicEntity::Read") && created;

    created = InstallHookTarget(
        reinterpret_cast<LPVOID>(engineBase + 0x2257F0),
        reinterpret_cast<LPVOID>(Hooked_EntityRead),
        reinterpret_cast<LPVOID*>(&Real_EntityRead),
        "eCEntity::Read") && created;

    created = InstallHookTarget(
        reinterpret_cast<LPVOID>(engineBase + 0x224350),
        reinterpret_cast<LPVOID>(Hooked_PatchWithTemplate),
        reinterpret_cast<LPVOID*>(&Real_PatchWithTemplate),
        "eCEntity::PatchWithTemplate") && created;

    created = InstallHookTarget(
        reinterpret_cast<LPVOID>(engineBase + 0x21A150),
        reinterpret_cast<LPVOID>(Hooked_ReadDependency),
        reinterpret_cast<LPVOID*>(&Real_ReadDependency),
        "eCContextBase::ReadDependency") && created;

    created = InstallHookTarget(
        reinterpret_cast<LPVOID>(GetProcAddress(hEngine, "?ChildrenAvailable@eCEntity@@UAEX_N@Z")),
        reinterpret_cast<LPVOID>(Hooked_ChildrenAvailable),
        reinterpret_cast<LPVOID*>(&Real_ChildrenAvailable),
        "eCEntity::ChildrenAvailable") && created;

    if (!created) {
        Log("G3O", "[LayerAudit] One or more metrics hooks failed to install; removing all.");
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
    Log("G3O", "[LayerAudit] %d QPC metrics hooks successfully enabled (frequency=%lld Hz).",
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
        Real_CPOStreamReadFile = nullptr;
        Real_CreateSpecificEntity = nullptr;
        Real_DynamicEntityRead = nullptr;
        Real_EntityRead = nullptr;
        Real_PatchWithTemplate = nullptr;
        Real_ReadDependency = nullptr;
        Real_ChildrenAvailable = nullptr;
        s_metricsHooksInitialized = false;
    }
    if (hadHooks) Log("G3O", "[LayerAudit] Metrics hooks shutdown completed.");
}
'''

new_lines = lines[:start_idx] + [new_code_lines + '\n\n'] + lines[end_idx:]
with open(cpp_path, 'w', encoding='utf-8') as f:
    f.writelines(new_lines)

print("SUCCESS: Exactly replaced lines in savegame_profiler.cpp")
