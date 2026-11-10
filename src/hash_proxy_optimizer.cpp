#include "hash_proxy_optimizer.h"
#include "fast_djb2.h"
#include "template_cache_policy.h"
#include "config_manager.h"
#include "diagnostic_logger.h"
#include "savegame_profiler.h"
#include <minhook.h>
#include <windows.h>
#include <atomic>

namespace G3O {

using GetHashValue_bCString_t = unsigned long (__stdcall*)(const void* pStr);
using GetHashValue_PAD_t = unsigned long (__stdcall*)(const char* const* ppStr);
using TemplateProxyGetEntity_t = void* (__thiscall*)(void* pThis);

static GetHashValue_bCString_t Real_GetHashValue_bCString = nullptr;
static GetHashValue_PAD_t Real_GetHashValue_PAD = nullptr;
static TemplateProxyGetEntity_t Real_TemplateProxyGetEntity = nullptr;

static void* s_targetHash_bCString = nullptr;
static void* s_targetHash_PAD = nullptr;
static void* s_targetProxyGetEntity = nullptr;

static std::atomic<uint32_t> s_hashCalls{0};
static std::atomic<uint32_t> s_proxyHits{0};
static std::atomic<uint32_t> s_proxyNegHits{0};
static std::atomic<uint32_t> s_proxyMisses{0};

static unsigned long __stdcall Hooked_GetHashValue_bCString(const void* pStr) {
    if (!pStr) return 0;
    const char* pData = *reinterpret_cast<const char* const*>(pStr);
    if (!pData || *pData == '\0') return 0;

    s_hashCalls.fetch_add(1, std::memory_order_relaxed);

    int len = *reinterpret_cast<const int*>(pData - 8);
    if (len > 0 && len <= 4096) {
        return FastDJB2_Length(pData, len);
    }
    return FastDJB2_NullTerminated(pData);
}

static unsigned long __stdcall Hooked_GetHashValue_PAD(const char* const* ppStr) {
    if (!ppStr) return 0;
    const char* str = *ppStr;
    if (!str) return 0;

    s_hashCalls.fetch_add(1, std::memory_order_relaxed);
    return FastDJB2_NullTerminated(str);
}

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
    return s_hashCalls.load(std::memory_order_relaxed);
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

    HMODULE hSharedBase = GetModuleHandleA("SharedBase.dll");
    if (hSharedBase) {
        // 1. Hook g_GetHashValue<bCString>
        void* pHashBCString = reinterpret_cast<void*>(GetProcAddress(
            hSharedBase, "??$g_GetHashValue@VbCString@@@@YGKABVbCString@@@Z"));
        if (pHashBCString) {
            if (MH_CreateHook(pHashBCString, reinterpret_cast<LPVOID>(Hooked_GetHashValue_bCString),
                              reinterpret_cast<LPVOID*>(&Real_GetHashValue_bCString)) == MH_OK) {
                if (MH_EnableHook(pHashBCString) == MH_OK) {
                    s_targetHash_bCString = pHashBCString;
                    Log("G3O", "Hash Optimizer: Hooked g_GetHashValue<bCString> (0x%p)", pHashBCString);
                }
            }
        }

        // 2. Hook g_GetHashValue<PAD>
        void* pHashPAD = reinterpret_cast<void*>(GetProcAddress(
            hSharedBase, "??$g_GetHashValue@PAD@@YGKABQAD@Z"));
        if (pHashPAD) {
            if (MH_CreateHook(pHashPAD, reinterpret_cast<LPVOID>(Hooked_GetHashValue_PAD),
                              reinterpret_cast<LPVOID*>(&Real_GetHashValue_PAD)) == MH_OK) {
                if (MH_EnableHook(pHashPAD) == MH_OK) {
                    s_targetHash_PAD = pHashPAD;
                    Log("G3O", "Hash Optimizer: Hooked g_GetHashValue<PAD> (0x%p)", pHashPAD);
                }
            }
        }
    }

    HMODULE hEngine = GetModuleHandleA("Engine.dll");
    if (hEngine) {
        // 3. Hook eCTemplateEntityProxy::GetEntity
        void* pProxyGet = reinterpret_cast<void*>(GetProcAddress(
            hEngine, "?GetEntity@eCTemplateEntityProxy@@QAEPAVeCTemplateEntity@@XZ"));
        if (!pProxyGet) {
            pProxyGet = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(hEngine) + 0x002302D0);
        }

        if (pProxyGet) {
            if (MH_CreateHook(pProxyGet, reinterpret_cast<LPVOID>(Hooked_TemplateProxyGetEntity),
                              reinterpret_cast<LPVOID*>(&Real_TemplateProxyGetEntity)) == MH_OK) {
                if (MH_EnableHook(pProxyGet) == MH_OK) {
                    s_targetProxyGetEntity = pProxyGet;
                    Log("G3O", "Proxy Optimizer: Hooked eCTemplateEntityProxy::GetEntity (0x%p)", pProxyGet);
                }
            }
        }
    }
}

void ShutdownHashProxyOptimizer() {
    if (s_targetHash_bCString) {
        MH_DisableHook(s_targetHash_bCString);
        MH_RemoveHook(s_targetHash_bCString);
        s_targetHash_bCString = nullptr;
    }
    if (s_targetHash_PAD) {
        MH_DisableHook(s_targetHash_PAD);
        MH_RemoveHook(s_targetHash_PAD);
        s_targetHash_PAD = nullptr;
    }
    if (s_targetProxyGetEntity) {
        MH_DisableHook(s_targetProxyGetEntity);
        MH_RemoveHook(s_targetProxyGetEntity);
        s_targetProxyGetEntity = nullptr;
    }
    FastTemplateCache::ClearAll();
}

} // namespace G3O
