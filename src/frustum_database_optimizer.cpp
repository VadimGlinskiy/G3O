#include "frustum_database_optimizer.h"
#include "frustum_database_policy.h"
#include "config_manager.h"
#include "diagnostic_logger.h"
#include "hook_install_policy.h"
#include <minhook.h>
#include <windows.h>
#include <atomic>

namespace G3O {

using ExtractFromEntityFn = bool (__thiscall*)(void* pThis, void* pEntity, int status);
using EntityDestructorFn = void (__thiscall*)(void* pThis);
using EntityDestroyFn = void (__thiscall*)(void* pThis);

static ExtractFromEntityFn Real_ExtractFromEntity = nullptr;
static EntityDestructorFn Real_EntityDestructor = nullptr;
static EntityDestroyFn Real_EntityDestroy = nullptr;

static void* s_targetExtract = nullptr;
static void* s_targetDestructor = nullptr;
static void* s_targetDestroy = nullptr;

static std::atomic<bool> s_deferActive{false};
static DWORD s_mainThreadId = 0;
static FrustumBatcher s_batcher;
static CRITICAL_SECTION s_batcherLock;

static bool __fastcall Hooked_ExtractFromEntity(void* pThis, void* /*edx*/, void* pEntity, int status) {
    if (s_deferActive.load(std::memory_order_relaxed) && GetCurrentThreadId() == s_mainThreadId) {
        if (!pEntity || !pThis) return false;
        EnterCriticalSection(&s_batcherLock);
        s_batcher.AddOrUpdate(pThis, pEntity, status);
        LeaveCriticalSection(&s_batcherLock);
        return true;
    }
    return Real_ExtractFromEntity(pThis, pEntity, status);
}

static void __fastcall Hooked_EntityDestructor(void* pThis, void* /*edx*/) {
    if (s_deferActive.load(std::memory_order_relaxed)) {
        EnterCriticalSection(&s_batcherLock);
        s_batcher.RemoveEntity(pThis);
        LeaveCriticalSection(&s_batcherLock);
    }
    Real_EntityDestructor(pThis);
}

static void __fastcall Hooked_EntityDestroy(void* pThis, void* /*edx*/) {
    if (s_deferActive.load(std::memory_order_relaxed)) {
        EnterCriticalSection(&s_batcherLock);
        s_batcher.RemoveEntity(pThis);
        LeaveCriticalSection(&s_batcherLock);
    }
    Real_EntityDestroy(pThis);
}

void FrustumOptimizer_BeginDefer() {
    if (!GetConfig().enableFrustumDatabaseOptimizer) return;
    s_mainThreadId = GetCurrentThreadId();
    EnterCriticalSection(&s_batcherLock);
    s_batcher.ResetStats();
    s_batcher.Clear();
    LeaveCriticalSection(&s_batcherLock);
    s_deferActive.store(true, std::memory_order_release);
}

void FrustumOptimizer_EndDeferAndFlush() {
    if (!s_deferActive.load(std::memory_order_relaxed)) return;
    s_deferActive.store(false, std::memory_order_release);

    std::unordered_map<void*, std::vector<FrustumBatcher::DatabaseEntry>> snapshot;
    uint32_t totalCalls = 0;
    uint32_t uniqueEntities = 0;
    uint32_t deletions = 0;

    EnterCriticalSection(&s_batcherLock);
    totalCalls = s_batcher.TotalDeferredCalls();
    uniqueEntities = static_cast<uint32_t>(s_batcher.UniqueEntityCount());
    deletions = s_batcher.DeletionsHandled();
    s_batcher.SnapshotAndClear(snapshot);
    LeaveCriticalSection(&s_batcherLock);

    // Call Real_ExtractFromEntity completely outside s_batcherLock to guarantee 100% lock safety
    uint32_t flushed = 0;
    for (const auto& kv : snapshot) {
        void* pEntity = kv.first;
        for (const auto& dbEntry : kv.second) {
            if (Real_ExtractFromEntity) {
                Real_ExtractFromEntity(dbEntry.pFrustumDB, pEntity, dbEntry.cullStatus);
            }
            ++flushed;
        }
    }

    EnterCriticalSection(&s_batcherLock);
    s_batcher.RecordFlushedCount(flushed);
    LeaveCriticalSection(&s_batcherLock);

    Log("G3O", "[FrustumOptimizer] GotoLoadedPosition: batched %u spatial calls down to %u updates across %u unique entities (skipped %u redundant rebuilds, %u deletions guarded).",
        totalCalls, flushed, uniqueEntities, (totalCalls > flushed ? (totalCalls - flushed) : 0), deletions);
}

std::uint32_t FrustumOptimizer_GetDeferredCalls() noexcept {
    return s_batcher.TotalDeferredCalls();
}

std::uint32_t FrustumOptimizer_GetFlushedUpdates() noexcept {
    return s_batcher.TotalFlushedUpdates();
}

std::uint32_t FrustumOptimizer_GetDeletionsHandled() noexcept {
    return s_batcher.DeletionsHandled();
}

void InitializeFrustumDatabaseOptimizer() {
    InitializeCriticalSection(&s_batcherLock);

    if (!GetConfig().enableFrustumDatabaseOptimizer) {
        Log("G3O", "Frustum Database Optimizer is disabled by configuration.");
        return;
    }

    HMODULE hEngine = GetModuleHandleA("Engine.dll");
    if (!hEngine) {
        Log("G3O", "Frustum Database Optimizer: Engine.dll not loaded; hook skipped.");
        return;
    }

    // 1. Hook eCFrustumDatabase::ExtractFromEntity
    void* pExtract = reinterpret_cast<void*>(GetProcAddress(
        hEngine, "?ExtractFromEntity@eCFrustumDatabase@@IAE_NPAVeCEntity@@W4eECullStatus@eCFrustumFilter@@@Z"));
    if (pExtract) {
        MH_STATUS createStatus = MH_OK, enableStatus = MH_OK, removeStatus = MH_OK;
        const HookInstallResult result = InstallHookSafely(
            [&] { createStatus = MH_CreateHook(pExtract, reinterpret_cast<LPVOID>(Hooked_ExtractFromEntity),
                                               reinterpret_cast<LPVOID*>(&Real_ExtractFromEntity)); return createStatus == MH_OK; },
            [&] { enableStatus = MH_EnableHook(pExtract); return enableStatus == MH_OK; },
            [&] { removeStatus = MH_RemoveHook(pExtract); },
            [&] { Real_ExtractFromEntity = nullptr; });
        if (result == HookInstallResult::Installed) {
            s_targetExtract = pExtract;
            Log("G3O", "Frustum Database Optimizer: Hooked eCFrustumDatabase::ExtractFromEntity (0x%p)", pExtract);
        } else {
            Log("G3O", "Frustum Database Optimizer: ExtractFromEntity install failed (create=%d, enable=%d, remove=%d).",
                createStatus, enableStatus, removeStatus);
        }
    } else {
        Log("G3O", "Frustum Database Optimizer: eCFrustumDatabase::ExtractFromEntity export missing.");
    }

    // 2. Hook eCEntity::~eCEntity (Destructor)
    void* pDestructor = reinterpret_cast<void*>(GetProcAddress(hEngine, "??1eCEntity@@MAE@XZ"));
    if (pDestructor) {
        MH_STATUS createStatus = MH_OK, enableStatus = MH_OK, removeStatus = MH_OK;
        const HookInstallResult result = InstallHookSafely(
            [&] { createStatus = MH_CreateHook(pDestructor, reinterpret_cast<LPVOID>(Hooked_EntityDestructor),
                                               reinterpret_cast<LPVOID*>(&Real_EntityDestructor)); return createStatus == MH_OK; },
            [&] { enableStatus = MH_EnableHook(pDestructor); return enableStatus == MH_OK; },
            [&] { removeStatus = MH_RemoveHook(pDestructor); },
            [&] { Real_EntityDestructor = nullptr; });
        if (result == HookInstallResult::Installed) {
            s_targetDestructor = pDestructor;
            Log("G3O", "Frustum Database Optimizer: Hooked eCEntity::~eCEntity (0x%p)", pDestructor);
        } else {
            Log("G3O", "Frustum Database Optimizer: entity destructor install failed (create=%d, enable=%d, remove=%d).",
                createStatus, enableStatus, removeStatus);
        }
    }

    // 3. Hook eCEntity::Destroy
    void* pDestroy = reinterpret_cast<void*>(GetProcAddress(hEngine, "?Destroy@eCEntity@@UAEXXZ"));
    if (pDestroy) {
        MH_STATUS createStatus = MH_OK, enableStatus = MH_OK, removeStatus = MH_OK;
        const HookInstallResult result = InstallHookSafely(
            [&] { createStatus = MH_CreateHook(pDestroy, reinterpret_cast<LPVOID>(Hooked_EntityDestroy),
                                               reinterpret_cast<LPVOID*>(&Real_EntityDestroy)); return createStatus == MH_OK; },
            [&] { enableStatus = MH_EnableHook(pDestroy); return enableStatus == MH_OK; },
            [&] { removeStatus = MH_RemoveHook(pDestroy); },
            [&] { Real_EntityDestroy = nullptr; });
        if (result == HookInstallResult::Installed) {
            s_targetDestroy = pDestroy;
            Log("G3O", "Frustum Database Optimizer: Hooked eCEntity::Destroy (0x%p)", pDestroy);
        } else {
            Log("G3O", "Frustum Database Optimizer: entity Destroy install failed (create=%d, enable=%d, remove=%d).",
                createStatus, enableStatus, removeStatus);
        }
    }
}

void ShutdownFrustumDatabaseOptimizer() {
    s_deferActive.store(false, std::memory_order_release);
    if (s_targetExtract) {
        MH_DisableHook(s_targetExtract);
        MH_RemoveHook(s_targetExtract);
        s_targetExtract = nullptr;
    }
    if (s_targetDestructor) {
        MH_DisableHook(s_targetDestructor);
        MH_RemoveHook(s_targetDestructor);
        s_targetDestructor = nullptr;
    }
    if (s_targetDestroy) {
        MH_DisableHook(s_targetDestroy);
        MH_RemoveHook(s_targetDestroy);
        s_targetDestroy = nullptr;
    }
    DeleteCriticalSection(&s_batcherLock);
}

} // namespace G3O
