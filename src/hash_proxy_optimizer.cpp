#include "hash_proxy_optimizer.h"
#include "fast_djb2.h"
#include "template_cache_policy.h"
#include "config_manager.h"
#include "diagnostic_logger.h"
#include "savegame_profiler.h"
#include "hook_install_policy.h"
#include <minhook.h>
#include <windows.h>
#include <atomic>

namespace G3O {

using TemplateProxyGetEntity_t = void* (__thiscall*)(void* pThis);

static TemplateProxyGetEntity_t Real_TemplateProxyGetEntity = nullptr;
static void* s_targetProxyGetEntity = nullptr;

static std::atomic<uint32_t> s_proxyHits{0};
static std::atomic<uint32_t> s_proxyNegHits{0};
static std::atomic<uint32_t> s_proxyMisses{0};

// In eCTemplateEntityProxy (12 bytes):
//   +0x04: eCTemplateEntity* m_pEntityCached (if resolved)
//   +0x08: const bCPropertyID* m_pPropertyID (pointer to 20-byte GUID)
//
// In vanilla Engine.dll (0x002302D0), eCSceneAdmin::m_templateMap only has 43 buckets (0x2B),
// causing ~116-node collision walks for 56,669 dynamic entity queries that return nullptr.
// We use a 32K direct-mapped L1 cache with Negative Caching (kNotFound with negEpoch).
static void* __fastcall Hooked_TemplateProxyGetEntity(void* pThis, void* /*edx*/) {
    if (!pThis) return nullptr;

    const uintptr_t proxyAddr = reinterpret_cast<uintptr_t>(pThis);

    // 1. Check if the entity is already resolved on this proxy instance
    void** ppCachedEntity = reinterpret_cast<void**>(proxyAddr + 4);
    if (*ppCachedEntity) {
        return *ppCachedEntity;
    }

    // In 0x002302D0, [edi + 8] is a pointer to bCPropertyID
    const uint32_t* pPropId = *reinterpret_cast<const uint32_t* const*>(proxyAddr + 8);
    if (!pPropId) {
        return Real_TemplateProxyGetEntity(pThis);
    }

    // Guard against all-zero GUID
    if ((pPropId[0] | pPropId[1] | pPropId[2] | pPropId[3]) == 0) {
        return nullptr;
    }

    // 2. Direct-Mapped L1 Cache lookup
    const uint32_t slotIdx = FastTemplateCache::ComputeSlotIndex(pPropId);
    auto& slot = FastTemplateCache::s_slots[slotIdx];
    const uintptr_t cachedVal = slot.entityOrState;

    if (cachedVal != FastTemplateCache::kEmptySlot && FastTemplateCache::GuidEquals(pPropId, slot.guid)) {
        if (cachedVal != FastTemplateCache::kNotFound) {
            void* pResolved = reinterpret_cast<void*>(cachedVal);
            *ppCachedEntity = pResolved;
            s_proxyHits.fetch_add(1, std::memory_order_relaxed);
            return pResolved;
        }

        // Negative Cache Hit (kNotFound): valid during savegame loading within current epoch
        const uint32_t curEpoch = FastTemplateCache::s_currentNegEpoch.load(std::memory_order_relaxed);
        if (IsSavegameLoadActive() && slot.negEpoch == curEpoch) {
            s_proxyNegHits.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
    }

    // 3. Cache Miss - call vanilla resolution (walks 43 buckets in m_templateMap)
    s_proxyMisses.fetch_add(1, std::memory_order_relaxed);
    void* pResolvedEntity = Real_TemplateProxyGetEntity(pThis);

    // 4. Update slot
    slot.guid[0] = pPropId[0];
    slot.guid[1] = pPropId[1];
    slot.guid[2] = pPropId[2];
    slot.guid[3] = pPropId[3];

    if (pResolvedEntity != nullptr) {
        slot.entityOrState = reinterpret_cast<uintptr_t>(pResolvedEntity);
    } else {
        slot.entityOrState = FastTemplateCache::kNotFound;
        slot.negEpoch      = FastTemplateCache::s_currentNegEpoch.load(std::memory_order_relaxed);
    }

    return pResolvedEntity;
}

std::uint32_t HashProxy_GetHashHits() noexcept {
    return 0;
}

std::uint32_t HashProxy_GetProxyHits() noexcept {
    return s_proxyHits.load(std::memory_order_relaxed);
}

std::uint32_t HashProxy_GetProxyNegHits() noexcept {
    return s_proxyNegHits.load(std::memory_order_relaxed);
}

std::uint32_t HashProxy_GetProxyMisses() noexcept {
    return s_proxyMisses.load(std::memory_order_relaxed);
}

void HashProxy_InvalidateNegativeCache() noexcept {
    FastTemplateCache::InvalidateNegativeCache();
}

void HashProxy_ClearAll() noexcept {
    FastTemplateCache::ClearAll();
}

void InitializeHashProxyOptimizer() {
    if (!GetConfig().enableHashProxyOptimizer) {
        Log("G3O", "Hash & Proxy Optimizer is disabled by configuration.");
        return;
    }

    HMODULE hEngine = GetModuleHandleA("Engine.dll");
    if (hEngine) {
        // Hook eCTemplateEntityProxy::GetEntity
        void* pProxyGet = reinterpret_cast<void*>(GetProcAddress(
            hEngine, "?GetEntity@eCTemplateEntityProxy@@QAEPAVeCTemplateEntity@@XZ"));
        if (!pProxyGet) {
            pProxyGet = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(hEngine) + 0x002302D0);
        }

        if (pProxyGet) {
            MH_STATUS createStatus = MH_OK, enableStatus = MH_OK, removeStatus = MH_OK;
            const HookInstallResult result = InstallHookSafely(
                [&] { createStatus = MH_CreateHook(pProxyGet, reinterpret_cast<LPVOID>(Hooked_TemplateProxyGetEntity),
                                                   reinterpret_cast<LPVOID*>(&Real_TemplateProxyGetEntity)); return createStatus == MH_OK; },
                [&] { enableStatus = MH_EnableHook(pProxyGet); return enableStatus == MH_OK; },
                [&] { removeStatus = MH_RemoveHook(pProxyGet); },
                [&] { Real_TemplateProxyGetEntity = nullptr; });
            if (result == HookInstallResult::Installed) {
                s_targetProxyGetEntity = pProxyGet;
                Log("G3O", "Proxy Optimizer: Hooked eCTemplateEntityProxy::GetEntity (0x%p)", pProxyGet);
            } else {
                Log("G3O", "Proxy Optimizer: GetEntity hook install failed (create=%d, enable=%d, remove=%d).",
                    createStatus, enableStatus, removeStatus);
            }
        }
    }
}

void ShutdownHashProxyOptimizer() {
    if (s_targetProxyGetEntity) {
        MH_DisableHook(s_targetProxyGetEntity);
        MH_RemoveHook(s_targetProxyGetEntity);
        s_targetProxyGetEntity = nullptr;
    }
    FastTemplateCache::ClearAll();
}

} // namespace G3O
