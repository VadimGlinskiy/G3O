#include "entity_search_optimizer.h"
#include "entity_search_policy.h"
#include "config_manager.h"
#include "diagnostic_logger.h"
#include "hook_install_policy.h"
#include <minhook.h>
#include <windows.h>

namespace G3O {

using SearchForEntity_t = uint16_t(__thiscall*)(void* pThis, uint32_t a_Type);
static SearchForEntity_t Real_SearchForEntity = nullptr;
static void* s_targetSearchForEntity = nullptr;

static thread_local uint64_t s_searchFastHits = 0;
static thread_local uint64_t s_searchFallbackSorts = 0;

static uint16_t __fastcall Hooked_SearchForEntity(void* pThis, void* /*edx*/, uint32_t a_Type) {
    if (!pThis) return 0xFFFF;

    // 1. Check isSorted flag at offset 0x148
    // If array was marked dirty (e.g. newly attached PropertySet), delegate to
    // original method so Genome Engine can sort via CRT qsort and set flag = 1.
    const uint8_t isSorted = *reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(pThis) + 0x148);
    if (isSorted == 0) {
        ++s_searchFallbackSorts;
        return Real_SearchForEntity ? Real_SearchForEntity(pThis, a_Type) : 0xFFFF;
    }

    // 2. Read component count (uint16_t at 0x13E) and array pointer (void** at 0x140)
    const uint16_t count = *reinterpret_cast<const uint16_t*>(reinterpret_cast<uintptr_t>(pThis) + 0x13E);
    if (count == 0) return 0xFFFF;

    void* const* pElements = *reinterpret_cast<void* const* const*>(reinterpret_cast<uintptr_t>(pThis) + 0x140);
    if (!pElements) return 0xFFFF;

    ++s_searchFastHits;
    return FastSearchPropertySet(pElements, count, a_Type);
}

bool InitializeEntitySearchOptimizer() {
    HMODULE hEngine = GetModuleHandleA("Engine.dll");
    if (!hEngine) {
        hEngine = LoadLibraryA("Engine.dll");
    }
    if (!hEngine) {
        Log("G3O", "EntitySearchOptimizer: Engine.dll not loaded.");
        return false;
    }

    void* fnSearch = reinterpret_cast<void*>(GetProcAddress(hEngine, "?SearchForEntity@eCEntity@@IBEGW4eEPropertySetType@@@Z"));
    if (!fnSearch) {
        Log("G3O", "EntitySearchOptimizer: SearchForEntity export not found in Engine.dll.");
        return false;
    }

    MH_STATUS createStatus = MH_OK;
    MH_STATUS enableStatus = MH_OK;
    MH_STATUS removeStatus = MH_OK;
    const HookInstallResult result = InstallHookSafely(
        [&] {
            createStatus = MH_CreateHook(fnSearch, reinterpret_cast<LPVOID>(Hooked_SearchForEntity), reinterpret_cast<LPVOID*>(&Real_SearchForEntity));
            return createStatus == MH_OK;
        },
        [&] {
            enableStatus = MH_EnableHook(fnSearch);
            return enableStatus == MH_OK;
        },
        [&] { removeStatus = MH_RemoveHook(fnSearch); },
        [&] { Real_SearchForEntity = nullptr; });
    if (result == HookInstallResult::Installed) {
        s_targetSearchForEntity = fnSearch;
        Log("G3O", "Entity Search Optimizer initialized (sorted search with early exit).");
        return true;
    } else {
        s_targetSearchForEntity = nullptr;
        if (result == HookInstallResult::CreateFailed) {
            Log("G3O", "Entity Search Optimizer: Failed to create SearchForEntity hook (code %d).", static_cast<int>(createStatus));
        } else {
            Log("G3O", "Entity Search Optimizer: Failed to enable SearchForEntity hook (code %d; remove code %d).",
                static_cast<int>(enableStatus), static_cast<int>(removeStatus));
        }
        return false;
    }
}

void ShutdownEntitySearchOptimizer() {
    if (s_targetSearchForEntity) {
        MH_DisableHook(s_targetSearchForEntity);
        MH_RemoveHook(s_targetSearchForEntity);
        s_targetSearchForEntity = nullptr;
    }
    Real_SearchForEntity = nullptr;
    Log("G3O", "Entity Search Optimizer shutdown (fast lookups=%llu, fallback sorts=%llu).",
        static_cast<unsigned long long>(s_searchFastHits),
        static_cast<unsigned long long>(s_searchFallbackSorts));
}

uint64_t GetEntitySearchFastHits() {
    return s_searchFastHits;
}

uint64_t GetEntitySearchFallbackSorts() {
    return s_searchFallbackSorts;
}

} // namespace G3O
