#include "dynamic_roi_optimizer.h"
#include "dynamic_roi_policy.h"
#include "pvs_prefetch_optimizer.h"
#include "config_manager.h"
#include "diagnostic_logger.h"

#include <windows.h>
#include <atomic>
#include <MinHook.h>

namespace G3O {

namespace {

using EntityAdminProcessFn = void (__thiscall*)(void*);
using GetSessionInstanceFn = void* (__stdcall*)();
using GetPlayerFn = void* (__thiscall*)(void*);
using GetPropertySetFn = void* (__thiscall*)(void*, int);
using GetDrawCrossHairFn = const bool& (__thiscall*)(const void*);

static EntityAdminProcessFn Real_EntityAdminProcess = nullptr;
static GetSessionInstanceFn s_pfnGetSessionInstance = nullptr;
static GetPlayerFn s_pfnGetPlayer = nullptr;
static GetPropertySetFn s_pfnGetPropertySet = nullptr;
static GetDrawCrossHairFn s_pfnGetDrawCrossHair = nullptr;
static void* s_targetEntityAdminProcess = nullptr;
static bool s_hookCreated = false;
static bool s_hookEnabled = false;

static DynamicRoiState s_roiState{};

static std::atomic<uint32_t> s_crosshairVisibleFrames{0};
static std::atomic<float>    s_currentEffectiveRoi{0.0f};

static bool ResolveFunctionPointers() {
    HMODULE hGame = GetModuleHandleA("Game.dll");
    if (hGame) {
        s_pfnGetSessionInstance = reinterpret_cast<GetSessionInstanceFn>(
            GetProcAddress(hGame, "?GetInstance@gCSession@@SGAAV1@XZ"));
        s_pfnGetPlayer = reinterpret_cast<GetPlayerFn>(
            GetProcAddress(hGame, "?GetPlayer@gCSession@@QAEPAVeCDynamicEntity@@XZ"));
        s_pfnGetDrawCrossHair = reinterpret_cast<GetDrawCrossHairFn>(
            GetProcAddress(hGame, "?GetDrawCrossHair@gCFocus_PS@@QBEAB_NXZ"));
    }

    HMODULE hEngine = GetModuleHandleA("Engine.dll");
    if (hEngine) {
        s_pfnGetPropertySet = reinterpret_cast<GetPropertySetFn>(
            GetProcAddress(hEngine, "?GetPropertySet@eCEntity@@UAEPAVeCEntityPropertySet@@W4eEPropertySetType@@@Z"));
    }

    return s_pfnGetSessionInstance && s_pfnGetPlayer && s_pfnGetPropertySet && s_pfnGetDrawCrossHair;
}

static bool IsCrosshairVisible() noexcept {
    if (!s_pfnGetSessionInstance || !s_pfnGetPlayer || !s_pfnGetPropertySet || !s_pfnGetDrawCrossHair) {
        return false;
    }
    __try {
        void* pSession = s_pfnGetSessionInstance();
        if (!pSession) return false;

        void* pPlayer = s_pfnGetPlayer(pSession);
        if (!pPlayer) return false;

        void* pFocus = s_pfnGetPropertySet(pPlayer, 59); // eEPropertySetType_Focus
        if (!pFocus) return false;

        return s_pfnGetDrawCrossHair(pFocus);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void UpdateFrameState() noexcept {
    const bool crosshairVisible = IsCrosshairVisible();
    if (crosshairVisible) {
        s_crosshairVisibleFrames.fetch_add(1, std::memory_order_relaxed);
    }

    const auto& cfg = GetConfig();
    if (cfg.enableDynamicROI && !PVSOptimizer_IsRampingUp()) {
        const float baseRoi = (cfg.baseROI > 0.0f ? cfg.baseROI : PVSOptimizer_GetOriginalTargetROI());
        const float crosshairRoi = (cfg.aimingROI > 0.0f ? cfg.aimingROI : baseRoi);

        if (baseRoi > 0.0f) {
            const float newRoi = DynamicRoiPolicy::UpdateRoi(
                s_roiState,
                crosshairVisible,
                baseRoi,
                crosshairRoi,
                DynamicRoiPolicy::kDefaultCrosshairRampUpFrames,
                DynamicRoiPolicy::kDefaultCrosshairRampDownFrames);

            s_currentEffectiveRoi.store(newRoi, std::memory_order_relaxed);
            PVSOptimizer_ApplyEffectiveROI(newRoi);
        }
    }
}

static void __fastcall Hooked_EntityAdminProcess(void* pThis, void* /*edx*/) {
    UpdateFrameState();

    if (Real_EntityAdminProcess) {
        Real_EntityAdminProcess(pThis);
    }
}

} // anonymous namespace

void InitializeDynamicROIOptimizer() {
    const auto& cfg = GetConfig();
    if (!cfg.enableDynamicROI) {
        Log("G3O", "Dynamic ROI Optimizer disabled in configuration.");
        return;
    }

    HMODULE hEngine = GetModuleHandleA("Engine.dll");
    if (!hEngine) {
        Log("G3O", "Dynamic ROI Optimizer: Engine.dll not loaded.");
        return;
    }

    if (!ResolveFunctionPointers()) {
        Log("G3O", "Dynamic ROI: required Game.dll or Engine.dll exports were not found.");
        return;
    }

    s_targetEntityAdminProcess = reinterpret_cast<void*>(
        GetProcAddress(hEngine, "?Process@eCEntityAdmin@@QAEXXZ"));
    if (!s_targetEntityAdminProcess) {
        Log("G3O", "Dynamic ROI: Failed to locate ?Process@eCEntityAdmin@@QAEXXZ");
        return;
    }

    MH_STATUS status = MH_CreateHook(
        s_targetEntityAdminProcess,
        reinterpret_cast<LPVOID>(&Hooked_EntityAdminProcess),
        reinterpret_cast<LPVOID*>(&Real_EntityAdminProcess));
    if (status != MH_OK) {
        Log("G3O", "Dynamic ROI: MH_CreateHook failed (%d).", static_cast<int>(status));
        s_targetEntityAdminProcess = nullptr;
        Real_EntityAdminProcess = nullptr;
        return;
    }
    s_hookCreated = true;

    status = MH_EnableHook(s_targetEntityAdminProcess);
    if (status != MH_OK) {
        const MH_STATUS removeStatus = MH_RemoveHook(s_targetEntityAdminProcess);
        Log("G3O", "Dynamic ROI: MH_EnableHook failed (%d); removing hook returned %d.",
            static_cast<int>(status), static_cast<int>(removeStatus));
        if (removeStatus == MH_OK) {
            s_hookCreated = false;
            s_targetEntityAdminProcess = nullptr;
            Real_EntityAdminProcess = nullptr;
        }
        return;
    }
    s_hookEnabled = true;

    const float initBaseRoi = (cfg.baseROI > 0.0f ? cfg.baseROI : PVSOptimizer_GetOriginalTargetROI());
    s_roiState.currentRoi = initBaseRoi;
    s_currentEffectiveRoi.store(initBaseRoi, std::memory_order_relaxed);

    Log("G3O", "Dynamic ROI initialized (base Entity.ROI=%.0f%s, crosshair Entity.ROI=%.0f).",
        initBaseRoi, cfg.baseROI <= 0.0f ? " (current game value)" : "", cfg.aimingROI);
}

void ShutdownDynamicROIOptimizer() {
    Log("G3O", "Dynamic ROI shutdown (crosshair-visible frames=%u).",
        s_crosshairVisibleFrames.load(std::memory_order_relaxed));

    if (s_targetEntityAdminProcess && s_hookEnabled) {
        const MH_STATUS status = MH_DisableHook(s_targetEntityAdminProcess);
        if (status != MH_OK && status != MH_ERROR_DISABLED) {
            Log("G3O", "Dynamic ROI: MH_DisableHook failed (%d).", static_cast<int>(status));
        }
    }
    if (s_targetEntityAdminProcess && s_hookCreated) {
        const MH_STATUS status = MH_RemoveHook(s_targetEntityAdminProcess);
        if (status != MH_OK) {
            Log("G3O", "Dynamic ROI: MH_RemoveHook failed (%d).", static_cast<int>(status));
        }
    }
    s_hookEnabled = false;
    s_hookCreated = false;
    s_targetEntityAdminProcess = nullptr;
    Real_EntityAdminProcess = nullptr;
}

uint32_t DynamicROI_GetCrosshairVisibleFrames() noexcept {
    return s_crosshairVisibleFrames.load(std::memory_order_relaxed);
}

float DynamicROI_GetCurrentROI() noexcept {
    return s_currentEffectiveRoi.load(std::memory_order_relaxed);
}

} // namespace G3O
