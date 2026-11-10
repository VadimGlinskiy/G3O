#include "shadow_caster_optimizer.h"
#include "shadow_caster_policy.h"
#include "config_manager.h"
#include "diagnostic_logger.h"
#include <minhook.h>
#include <windows.h>
#include <atomic>

namespace G3O {

using AddShadowCaster_t    = void(__thiscall*)(void* pThisLight, void* pCaster);
using RemoveShadowCaster_t = void(__thiscall*)(void* pThisLight, void* pCaster);

static AddShadowCaster_t    fpAddShadowCasterOrig    = nullptr;
static RemoveShadowCaster_t fpRemoveShadowCasterOrig = nullptr;

static void* s_targetAddShadowCaster    = nullptr;
static void* s_targetRemoveShadowCaster = nullptr;

// 64K entries = 1 MB static memory in .bss (zero dynamic allocation)
static ShadowIndexMap::Entry s_shadowTable[ShadowIndexMap::kCapacity] = {};

static std::atomic<uint32_t> s_fastAdds{0};
static std::atomic<uint32_t> s_addDuplicatesAvoided{0};
static std::atomic<uint32_t> s_reallocAdds{0};
static std::atomic<uint32_t> s_fastRemoves{0};
static std::atomic<uint32_t> s_fallbackRemoves{0};

// Hooked eCShadowMapDirectionalLightBase::AddShadowCaster (Engine.dll RVA 0x00105200)
static void __fastcall Hooked_AddShadowCaster(void* pThisLight, void* /*edx*/, void* pCaster) noexcept {
    if (!pThisLight || !pCaster) return;

    const uintptr_t lightAddr = reinterpret_cast<uintptr_t>(pThisLight);
    void**   pArray   = *reinterpret_cast<void***>(lightAddr + OFFSET_CASTER_ARRAY_PTR);
    uint32_t count    = *reinterpret_cast<uint32_t*>(lightAddr + OFFSET_CASTER_COUNT);
    uint32_t capacity = *reinterpret_cast<uint32_t*>(lightAddr + OFFSET_CASTER_CAPACITY);

    // 1. O(1) duplicate check via ShadowIndexMap
    uint32_t existingIdx = 0;
    if (ShadowIndexMap::FindIndex(s_shadowTable, pThisLight, pCaster, existingIdx)) {
        if (existingIdx < count && pArray && pArray[existingIdx] == pCaster) {
            // Already present in caster array: immediate O(1) exit (bypasses linear scan of 10,000+ items)
            s_addDuplicatesAvoided.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }

    // 2. Fast direct append when buffer capacity is sufficient
    if (pArray != nullptr && count < capacity) {
        pArray[count] = pCaster;
        *reinterpret_cast<uint32_t*>(lightAddr + OFFSET_CASTER_COUNT) = count + 1;
        ShadowIndexMap::Upsert(s_shadowTable, pThisLight, pCaster, count);
        s_fastAdds.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // 3. Buffer full: let vanilla reserve and reallocate via bTValArray::GrowIfNeeded
    if (fpAddShadowCasterOrig) {
        fpAddShadowCasterOrig(pThisLight, pCaster);
        const uint32_t newCount = *reinterpret_cast<uint32_t*>(lightAddr + OFFSET_CASTER_COUNT);
        void** pNewArray        = *reinterpret_cast<void***>(lightAddr + OFFSET_CASTER_ARRAY_PTR);
        if (newCount > 0 && pNewArray && pNewArray[newCount - 1] == pCaster) {
            ShadowIndexMap::Upsert(s_shadowTable, pThisLight, pCaster, newCount - 1);
        }
        s_reallocAdds.fetch_add(1, std::memory_order_relaxed);
    }
}

// Hooked eCShadowMapDirectionalLightBase::RemoveShadowCaster (Engine.dll RVA 0x00105260)
// Replaces backwards linear search + memmove with O(1) hash lookup and Swap-and-Pop.
static void __fastcall Hooked_RemoveShadowCaster(void* pThisLight, void* /*edx*/, void* pCaster) noexcept {
    if (!pThisLight || !pCaster) return;

    const uintptr_t lightAddr = reinterpret_cast<uintptr_t>(pThisLight);
    void**   pArray = *reinterpret_cast<void***>(lightAddr + OFFSET_CASTER_ARRAY_PTR);
    uint32_t count  = *reinterpret_cast<uint32_t*>(lightAddr + OFFSET_CASTER_COUNT);

    if (!pArray || count == 0) return;

    // 1. O(1) index lookup via ShadowIndexMap
    uint32_t targetIdx = 0;
    if (ShadowIndexMap::FindIndex(s_shadowTable, pThisLight, pCaster, targetIdx) &&
        targetIdx < count && pArray[targetIdx] == pCaster) {
        const uint32_t lastIdx = count - 1;
        void* pLastCaster      = pArray[lastIdx];

        // O(1) Swap-and-Pop (eliminates memmove of the entire array tail)
        pArray[targetIdx] = pLastCaster;
        *reinterpret_cast<uint32_t*>(lightAddr + OFFSET_CASTER_COUNT) = lastIdx;

        ShadowIndexMap::Erase(s_shadowTable, pThisLight, pCaster);
        if (targetIdx != lastIdx) {
            ShadowIndexMap::Upsert(s_shadowTable, pThisLight, pLastCaster, targetIdx);
        }
        s_fastRemoves.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // 2. Fallback: backwards linear search if not in index map, but still use Swap-and-Pop
    for (int32_t i = static_cast<int32_t>(count) - 1; i >= 0; --i) {
        if (pArray[i] == pCaster) {
            const uint32_t lastIdx = count - 1;
            void* pLastCaster      = pArray[lastIdx];

            pArray[i] = pLastCaster;
            *reinterpret_cast<uint32_t*>(lightAddr + OFFSET_CASTER_COUNT) = lastIdx;

            ShadowIndexMap::Erase(s_shadowTable, pThisLight, pCaster);
            if (static_cast<uint32_t>(i) != lastIdx) {
                ShadowIndexMap::Upsert(s_shadowTable, pThisLight, pLastCaster, static_cast<uint32_t>(i));
            }
            s_fallbackRemoves.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}

void InitializeShadowCasterOptimizer() {
    HMODULE hEngine = GetModuleHandleA("Engine.dll");
    if (!hEngine) {
        Log("G3O", "ShadowCasterOptimizer: Engine.dll not loaded.");
        return;
    }

    // AddShadowCaster is exported by name
    void* pAdd = reinterpret_cast<void*>(
        GetProcAddress(hEngine, "?AddShadowCaster@eCShadowMapDirectionalLightBase@@UAEXPAVeCEntity@@@Z"));
    if (!pAdd) {
        Log("G3O", "ShadowCasterOptimizer: Could not resolve ?AddShadowCaster@eCShadowMapDirectionalLightBase@@UAEXPAVeCEntity@@@Z.");
        return;
    }

    // RemoveShadowCaster is located exactly 0x60 bytes after AddShadowCaster in Engine.dll
    void* pRemove = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(pAdd) + 0x60);

    // Verify RemoveShadowCaster function prologue bytes:
    // 56                push esi
    // 8B F1             mov  esi, ecx
    // 8B 46 6C          mov  eax, dword ptr [esi + 0x6C]
    const unsigned char expectedPrologue[] = { 0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x6C };
    if (std::memcmp(pRemove, expectedPrologue, sizeof(expectedPrologue)) != 0) {
        Log("G3O", "ShadowCasterOptimizer: Prologue mismatch at RemoveShadowCaster (0x%p). Hook aborted for safety.", pRemove);
        return;
    }

    ShadowIndexMap::Clear(s_shadowTable);

    if (MH_CreateHook(pAdd, reinterpret_cast<LPVOID>(Hooked_AddShadowCaster),
                      reinterpret_cast<LPVOID*>(&fpAddShadowCasterOrig)) == MH_OK &&
        MH_EnableHook(pAdd) == MH_OK) {
        s_targetAddShadowCaster = pAdd;
    } else {
        Log("G3O", "ShadowCasterOptimizer: Failed to hook AddShadowCaster.");
        return;
    }

    if (MH_CreateHook(pRemove, reinterpret_cast<LPVOID>(Hooked_RemoveShadowCaster),
                      reinterpret_cast<LPVOID*>(&fpRemoveShadowCasterOrig)) == MH_OK &&
        MH_EnableHook(pRemove) == MH_OK) {
        s_targetRemoveShadowCaster = pRemove;
        Log("G3O", "ShadowCasterOptimizer: Hooked AddShadowCaster (0x%p) & RemoveShadowCaster (0x%p) [O(1) Swap-and-Pop active].",
            pAdd, pRemove);
    } else {
        Log("G3O", "ShadowCasterOptimizer: Failed to hook RemoveShadowCaster.");
        if (s_targetAddShadowCaster) {
            MH_DisableHook(s_targetAddShadowCaster);
            s_targetAddShadowCaster = nullptr;
        }
    }
}

void ShutdownShadowCasterOptimizer() {
    if (s_targetAddShadowCaster) {
        MH_DisableHook(s_targetAddShadowCaster);
        s_targetAddShadowCaster = nullptr;
    }
    if (s_targetRemoveShadowCaster) {
        MH_DisableHook(s_targetRemoveShadowCaster);
        s_targetRemoveShadowCaster = nullptr;
    }
    ShadowIndexMap::Clear(s_shadowTable);
}

void ResetShadowCasterOptimizerSession() {
    ShadowIndexMap::Clear(s_shadowTable);
    s_fastAdds.store(0, std::memory_order_relaxed);
    s_addDuplicatesAvoided.store(0, std::memory_order_relaxed);
    s_reallocAdds.store(0, std::memory_order_relaxed);
    s_fastRemoves.store(0, std::memory_order_relaxed);
    s_fallbackRemoves.store(0, std::memory_order_relaxed);
}

uint32_t ShadowOptimizer_GetFastAdds() noexcept {
    return s_fastAdds.load(std::memory_order_relaxed);
}

uint32_t ShadowOptimizer_GetDuplicatesAvoided() noexcept {
    return s_addDuplicatesAvoided.load(std::memory_order_relaxed);
}

uint32_t ShadowOptimizer_GetFastRemoves() noexcept {
    return s_fastRemoves.load(std::memory_order_relaxed);
}

uint32_t ShadowOptimizer_GetFallbackRemoves() noexcept {
    return s_fallbackRemoves.load(std::memory_order_relaxed);
}

} // namespace G3O
