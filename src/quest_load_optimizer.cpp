#include "quest_load_optimizer.h"
#include "config_manager.h"
#include "diagnostic_logger.h"
#include "savegame_profiler.h"
#include <minhook.h>
#include <windows.h>
#include <atomic>

namespace G3O {

using gCQuest_HaveNPCsAllRequiredItems_t = bool (__thiscall*)(void* pThisQuestPS, const void* pId1, const void* pId2);
using gCInfoManager_HasImportantInfo_t   = bool (__thiscall*)(void* pThisInfoMgrPS, void* pEntity);

static gCQuest_HaveNPCsAllRequiredItems_t Real_HaveNPCsAllRequiredItems = nullptr;
static gCInfoManager_HasImportantInfo_t   Real_HasImportantInfo         = nullptr;

static void* s_targetHaveItems     = nullptr;
static void* s_targetImportantInfo = nullptr;

static std::atomic<uint32_t> s_questChecksSuppressed{0};
static std::atomic<uint32_t> s_infoChecksSuppressed{0};

// In GotoLoadedPosition, entity spawning and inventory population trigger callbacks to
// HaveNPCsAllRequiredItems and HasImportantInfo. Both are pure read-only const queries.
// During loading screen, dialogue UI is not rendered and quest state is already deserialized from savegame.
// Suppressing them during savegame loading saves hundreds of milliseconds of redundant world and inventory traversal.
static bool __fastcall Hooked_HaveNPCsAllRequiredItems(void* pThisQuestPS, void* /*edx*/, const void* pId1, const void* pId2) noexcept {
    if (IsSavegameLoadActive()) {
        s_questChecksSuppressed.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return Real_HaveNPCsAllRequiredItems(pThisQuestPS, pId1, pId2);
}

static bool __fastcall Hooked_HasImportantInfo(void* pThisInfoMgrPS, void* /*edx*/, void* pEntity) noexcept {
    if (IsSavegameLoadActive()) {
        s_infoChecksSuppressed.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return Real_HasImportantInfo(pThisInfoMgrPS, pEntity);
}

uint32_t QuestLoadOptimizer_GetSuppressedQuestChecks() noexcept {
    return s_questChecksSuppressed.load(std::memory_order_relaxed);
}

uint32_t QuestLoadOptimizer_GetSuppressedInfoChecks() noexcept {
    return s_infoChecksSuppressed.load(std::memory_order_relaxed);
}

void InitializeQuestLoadOptimizer() {
    if (!GetConfig().enableQuestLoadOptimizer) {
        Log("G3O", "Quest Load Optimizer is disabled by configuration.");
        return;
    }

    HMODULE hGame = GetModuleHandleA("Game.dll");
    if (!hGame) {
        Log("G3O", "Quest Load Optimizer: Game.dll not loaded.");
        return;
    }

    const uintptr_t gameBase = reinterpret_cast<uintptr_t>(hGame);

    void* pTargetHaveItems = GetProcAddress(hGame, "?HaveNPCsAllRequiredItems@gCQuest_PS@@QAE_NABVbCPropertyID@@0@Z");
    if (!pTargetHaveItems) {
        pTargetHaveItems = reinterpret_cast<void*>(gameBase + 0x001576D0);
    }

    void* pTargetImportantInfo = GetProcAddress(hGame, "?HasImportantInfo@gCInfoManager_PS@@QAE_NPAVeCEntity@@@Z");
    if (!pTargetImportantInfo) {
        pTargetImportantInfo = reinterpret_cast<void*>(gameBase + 0x001CE8A0);
    }

    if (pTargetHaveItems) {
        if (MH_CreateHook(pTargetHaveItems, reinterpret_cast<LPVOID>(Hooked_HaveNPCsAllRequiredItems),
                          reinterpret_cast<LPVOID*>(&Real_HaveNPCsAllRequiredItems)) == MH_OK) {
            if (MH_EnableHook(pTargetHaveItems) == MH_OK) {
                s_targetHaveItems = pTargetHaveItems;
                Log("G3O", "Quest Load Optimizer: Hooked HaveNPCsAllRequiredItems (0x%p)", pTargetHaveItems);
            }
        }
    }

    if (pTargetImportantInfo) {
        if (MH_CreateHook(pTargetImportantInfo, reinterpret_cast<LPVOID>(Hooked_HasImportantInfo),
                          reinterpret_cast<LPVOID*>(&Real_HasImportantInfo)) == MH_OK) {
            if (MH_EnableHook(pTargetImportantInfo) == MH_OK) {
                s_targetImportantInfo = pTargetImportantInfo;
                Log("G3O", "Quest Load Optimizer: Hooked HasImportantInfo (0x%p)", pTargetImportantInfo);
            }
        }
    }
}

void ShutdownQuestLoadOptimizer() {
    if (s_targetHaveItems) {
        MH_DisableHook(s_targetHaveItems);
        s_targetHaveItems = nullptr;
    }
    if (s_targetImportantInfo) {
        MH_DisableHook(s_targetImportantInfo);
        s_targetImportantInfo = nullptr;
    }
}

} // namespace G3O
