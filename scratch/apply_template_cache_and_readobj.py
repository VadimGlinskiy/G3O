cpp_path = r'E:\Projects\G3O\src\savegame_profiler.cpp'
with open(cpp_path, 'r', encoding='utf-8') as f:
    content = f.read()

# 1. Add readObject to ContextAuditMetrics
old_ctx_metrics = '''struct ContextAuditMetrics {
    LayerAuditMetric cpoReadFile;          // bCPOStream::ReadFile (SharedBase.dll)
    LayerAuditMetric createSpecificEntity; // eCEntityDynamicContext::CreateSpecificEntity (Engine.dll)
    LayerAuditMetric dynEntityRead;        // eCDynamicEntity::Read (Engine.dll RVA 0x2295C0)
    LayerAuditMetric entityRead;           // eCEntity::Read (Engine.dll RVA 0x2257F0)
    LayerAuditMetric patchWithTemplate;    // eCEntity::PatchWithTemplate (Engine.dll RVA 0x224350)
    LayerAuditMetric readDependency;       // eCContextBase::ReadDependency (Engine.dll RVA 0x21A150)
    LayerAuditMetric childrenAvailable;    // eCEntity::ChildrenAvailable (Engine.dll)
};'''

new_ctx_metrics = '''struct ContextAuditMetrics {
    LayerAuditMetric cpoReadFile;          // bCPOStream::ReadFile (SharedBase.dll)
    LayerAuditMetric createSpecificEntity; // eCEntityDynamicContext::CreateSpecificEntity (Engine.dll)
    LayerAuditMetric dynEntityRead;        // eCDynamicEntity::Read (Engine.dll RVA 0x2295C0)
    LayerAuditMetric entityRead;           // eCEntity::Read (Engine.dll RVA 0x2257F0)
    LayerAuditMetric readObject;           // bCPropertyObjectSingleton::ReadObject (SharedBase.dll)
    LayerAuditMetric patchWithTemplate;    // eCEntity::PatchWithTemplate (Engine.dll RVA 0x224350)
    LayerAuditMetric readDependency;       // eCContextBase::ReadDependency (Engine.dll RVA 0x21A150)
    LayerAuditMetric childrenAvailable;    // eCEntity::ChildrenAvailable (Engine.dll)
};'''

assert old_ctx_metrics in content, "old_ctx_metrics not found"
content = content.replace(old_ctx_metrics, new_ctx_metrics, 1)

# 2. Add ReadObjectFn and Template Cache structures
old_prototypes = 'using PatchWithTemplateFn = bool (__thiscall*)(void*, const void*, bool);'
new_prototypes = '''using PatchWithTemplateFn = bool (__thiscall*)(void*, const void*, bool);
using PropertyObjectReadObjectFn = bool (__thiscall*)(void*, void*, void*);
static PropertyObjectReadObjectFn Real_PropertyObjectReadObject = nullptr;

// -----------------------------------------------------------------------------
// O(1) Template Entity Cache (Phase 11.8)
// Eliminates repetitive hash table traversals & divisions for ~1500 unique templates
// -----------------------------------------------------------------------------
struct TemplateCacheSlot {
    uint32_t guid[4];
    void*    pTemplateEntity;
    bool     valid;
};

static const uint32_t TEMPLATE_CACHE_SIZE = 4096;
static const uint32_t TEMPLATE_CACHE_MASK = TEMPLATE_CACHE_SIZE - 1;

static TemplateCacheSlot s_templateCache[TEMPLATE_CACHE_SIZE] = {};
static uint32_t s_templateHits = 0;
static uint32_t s_templateMisses = 0;
static uintptr_t s_cachedEngineBase = 0;

static void* LookupTemplateEntity(uintptr_t engineBase, const void* pGuid) {
    if (!engineBase) return nullptr;
    typedef void* (__cdecl* GetTemplateContextFn)();
    GetTemplateContextFn getCtx = reinterpret_cast<GetTemplateContextFn>(engineBase + 0x17B20);
    void* ctx = getCtx();
    if (!ctx) return nullptr;

    void* pMap = reinterpret_cast<char*>(ctx) + 0x34;
    void* pFindNode = reinterpret_cast<void*>(engineBase + 0x1F0F20);
    uint32_t dummy = 0;
    void* node = nullptr;

    __asm {
        mov eax, pMap
        lea ecx, dummy
        push ecx
        push pGuid
        call pFindNode
        mov node, eax
    }

    if (!node) return nullptr;
    return *reinterpret_cast<void**>(reinterpret_cast<char*>(node) + 0x14);
}'''

assert old_prototypes in content, "old_prototypes not found"
content = content.replace(old_prototypes, new_prototypes, 1)

# 3. Replace Hooked_PatchWithTemplate with O(1) Cache implementation
old_hook_patch = '''static bool __fastcall Hooked_PatchWithTemplate(void* self, void* /*edx*/, const void* pGuid, bool flag) {
    if (!s_isLoadActive) return Real_PatchWithTemplate(self, pGuid, flag);
    const std::uint64_t start = BeginLayerMetric();
    const bool result = Real_PatchWithTemplate(self, pGuid, flag);
    EndLayerMetric(s_contextAuditMetrics.patchWithTemplate, start);
    return result;
}'''

new_hook_patch = '''static bool __fastcall Hooked_PropertyObjectReadObject(void* self, void* /*edx*/, void* pStream, void* pObject) {
    if (!s_isLoadActive) return Real_PropertyObjectReadObject(self, pStream, pObject);
    const std::uint64_t start = BeginLayerMetric();
    const bool res = Real_PropertyObjectReadObject(self, pStream, pObject);
    EndLayerMetric(s_contextAuditMetrics.readObject, start);
    return res;
}

static bool __fastcall Hooked_PatchWithTemplate(void* self, void* /*edx*/, const void* pGuid, bool flag) {
    if (!s_isLoadActive || !pGuid) {
        return Real_PatchWithTemplate(self, pGuid, flag);
    }

    const std::uint64_t start = BeginLayerMetric();

    const uint32_t* g = reinterpret_cast<const uint32_t*>(pGuid);
    uint32_t hash = (g[0] ^ (g[1] * 31) ^ (g[2] * 127) ^ g[3]) & TEMPLATE_CACHE_MASK;
    TemplateCacheSlot& slot = s_templateCache[hash];

    void* pTemplate = nullptr;
    if (slot.valid && slot.guid[0] == g[0] && slot.guid[1] == g[1] && slot.guid[2] == g[2] && slot.guid[3] == g[3]) {
        pTemplate = slot.pTemplateEntity;
        ++s_templateHits;
    } else {
        pTemplate = LookupTemplateEntity(s_cachedEngineBase, pGuid);
        slot.guid[0] = g[0];
        slot.guid[1] = g[1];
        slot.guid[2] = g[2];
        slot.guid[3] = g[3];
        slot.pTemplateEntity = pTemplate;
        slot.valid = true;
        ++s_templateMisses;
    }

    bool result = true;
    if (pTemplate) {
        void* enginePtr = *reinterpret_cast<void**>(s_cachedEngineBase + 0x602A88);
        if (enginePtr && *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(enginePtr) + 0x188) != 0) {
            void** vtbl = *reinterpret_cast<void***>(self);
            using CopyFromTemplateFn = bool (__thiscall*)(void*, void*, int, bool);
            CopyFromTemplateFn copyFn = reinterpret_cast<CopyFromTemplateFn>(vtbl[0x104 / 4]);
            result = copyFn(self, pTemplate, 0, flag);
        }
    }

    EndLayerMetric(s_contextAuditMetrics.patchWithTemplate, start);
    return result;
}'''

assert old_hook_patch in content, "old_hook_patch not found"
content = content.replace(old_hook_patch, new_hook_patch, 1)

# 4. In InitMetricsHooks: save s_cachedEngineBase and install PropertyObject::ReadObject hook
old_init_engine = '    uintptr_t engineBase = reinterpret_cast<uintptr_t>(hEngine);'
new_init_engine = '''    uintptr_t engineBase = reinterpret_cast<uintptr_t>(hEngine);
    s_cachedEngineBase = engineBase;'''

assert old_init_engine in content, "old_init_engine not found"
content = content.replace(old_init_engine, new_init_engine, 1)

old_install_patch = '        "eCEntity::PatchWithTemplate") && created;'
new_install_patch = '''        "eCEntity::PatchWithTemplate") && created;

    created = InstallHookTarget(
        reinterpret_cast<LPVOID>(GetProcAddress(hSharedBase, "?ReadObject@bCPropertyObjectSingleton@@QAE_NAAVbCIStream@@AAVbCPropertyObjectBase@@@Z")),
        reinterpret_cast<LPVOID>(Hooked_PropertyObjectReadObject),
        reinterpret_cast<LPVOID*>(&Real_PropertyObjectReadObject),
        "bCPropertyObjectSingleton::ReadObject") && created;'''

assert old_install_patch in content, "old_install_patch not found"
content = content.replace(old_install_patch, new_install_patch, 1)

# 5. In RemoveCreatedMetricsHooks / ShutdownMetricsHooks: clear Real_PropertyObjectReadObject
old_shutdown = '    Real_PatchWithTemplate = nullptr;'
new_shutdown = '''    Real_PatchWithTemplate = nullptr;
    Real_PropertyObjectReadObject = nullptr;'''

assert old_shutdown in content, "old_shutdown not found"
content = content.replace(old_shutdown, new_shutdown, 2)

# 6. In Hooked_OnLoadDynamicContext: update log with readObject and template hits/misses
old_ctx_log = '''        const double patchMs = LayerMetricMs(s_contextAuditMetrics.patchWithTemplate.qpcTicks - before.patchWithTemplate.qpcTicks);
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
            childMs, childCalls);'''

new_ctx_log = '''        const double patchMs = LayerMetricMs(s_contextAuditMetrics.patchWithTemplate.qpcTicks - before.patchWithTemplate.qpcTicks);
        const uint64_t patchCalls = s_contextAuditMetrics.patchWithTemplate.calls - before.patchWithTemplate.calls;

        const double readObjMs = LayerMetricMs(s_contextAuditMetrics.readObject.qpcTicks - before.readObject.qpcTicks);
        const uint64_t readObjCalls = s_contextAuditMetrics.readObject.calls - before.readObject.calls;

        const double depMs = LayerMetricMs(s_contextAuditMetrics.readDependency.qpcTicks - before.readDependency.qpcTicks);
        const uint64_t depCalls = s_contextAuditMetrics.readDependency.calls - before.readDependency.calls;

        const double childMs = LayerMetricMs(s_contextAuditMetrics.childrenAvailable.qpcTicks - before.childrenAvailable.qpcTicks);
        const uint64_t childCalls = s_contextAuditMetrics.childrenAvailable.calls - before.childrenAvailable.calls;

        Log("G3O", "[DynamicContextAudit] Layer %p ('%s') directRead=%d | Total=%.2f ms | CPOReadFile=%.2f ms (%llu calls) | Entities=%llu (Create=%.2f ms, DynRead=%.2f ms [EntRead=%.2f ms (ReadObj=%.2f ms in %llu calls), Patch=%.2f ms (Cache: %u hits, %u miss)]) | ReadDep=%.2f ms (%llu calls)",
            pLayer, filePath, (int)directRead, totalMs,
            cpoMs, cpoCalls,
            createCalls, createMs, dynReadMs,
            entReadMs, readObjMs, readObjCalls,
            patchMs, s_templateHits, s_templateMisses,
            depMs, depCalls);'''

assert old_ctx_log in content, "old_ctx_log not found"
content = content.replace(old_ctx_log, new_ctx_log, 1)

# 7. In Hooked_HUDLoadGame: reset template cache stats
old_hud_reset = '    ResetPropertyCacheTelemetry();'
new_hud_reset = '''    ResetPropertyCacheTelemetry();
    s_templateHits = 0;
    s_templateMisses = 0;
    memset(s_templateCache, 0, sizeof(s_templateCache));'''

assert old_hud_reset in content, "old_hud_reset not found"
content = content.replace(old_hud_reset, new_hud_reset, 1)

# 8. In LogLoadReport: add Template Resolution Cache line
old_report_line = '''    Log("G3O", "  - Property Resolution Cache (load-thread counters): %ld resolves (%ld hits, %ld misses, %.1f%% hit rate)",
        totalProps, strHits + idHits, strMiss + idMiss, propHitRate);'''

new_report_line = '''    Log("G3O", "  - Property Resolution Cache (load-thread counters): %ld resolves (%ld hits, %ld misses, %.1f%% hit rate)",
        totalProps, strHits + idHits, strMiss + idMiss, propHitRate);
    uint32_t tmplTotal = s_templateHits + s_templateMisses;
    double tmplHitRate = (tmplTotal > 0) ? (static_cast<double>(s_templateHits) * 100.0 / static_cast<double>(tmplTotal)) : 0.0;
    Log("G3O", "  - Template Resolution Cache:        %ld resolves (%ld hits, %ld misses, %.1f%% hit rate)",
        tmplTotal, s_templateHits, s_templateMisses, tmplHitRate);'''

assert old_report_line in content, "old_report_line not found"
content = content.replace(old_report_line, new_report_line, 1)

with open(cpp_path, 'w', encoding='utf-8') as f:
    f.write(content)

print("SUCCESS: savegame_profiler.cpp updated with Template Cache & ReadObject audit!")
