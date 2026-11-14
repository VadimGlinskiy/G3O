#include "global_visual_lod_probe.h"

#include "config_manager.h"
#include "diagnostic_logger.h"
#include "global_visual_lod_probe_policy.h"
#include "hook_install_policy.h"

#include <windows.h>
#include <minhook.h>

namespace G3O {

using SetGlobalVisualLodFactor_t = void(__thiscall*)(void*, const float*);

static SetGlobalVisualLodFactor_t s_originalSetGlobalVisualLodFactor = nullptr;
static void* s_targetSetGlobalVisualLodFactor = nullptr;

static void __fastcall HookedSetGlobalVisualLodFactor(void* self, void*, const float* factor) {
    const Config& config = GetConfig();
    const SetGlobalVisualLodFactor_t original = s_originalSetGlobalVisualLodFactor;
    if (!original) return;

    float scaledFactor = 0.0f;
    if (factor && TryScaleGlobalVisualLoD(*factor, config.globalVisualLodProbeScale, scaledFactor)) {
        original(self, &scaledFactor);
        return;
    }

    original(self, factor);
}

void InitializeGlobalVisualLodProbe() {
    const Config& config = GetConfig();
    if (!config.enableGlobalVisualLodProbe || config.globalVisualLodProbeScale == 1.0f) return;

    HMODULE engine = GetModuleHandleA("Engine.dll");
    if (!engine) {
        Log("GlobalVisualLODProbe", "Hook not installed: Engine.dll is not loaded.");
        return;
    }

    void* target = reinterpret_cast<void*>(GetProcAddress(
        engine, "?SetGlobalVisualLoDFactor@eCEngineCaps_PS@@QAEXABM@Z"));
    if (!target) {
        Log("GlobalVisualLODProbe", "Hook not installed: Engine.dll export SetGlobalVisualLoDFactor was not found.");
        return;
    }

    MH_STATUS createStatus = MH_OK;
    MH_STATUS enableStatus = MH_OK;
    MH_STATUS removeStatus = MH_OK;
    const HookInstallResult result = InstallHookSafely(
        [&] {
            createStatus = MH_CreateHook(target,
                reinterpret_cast<LPVOID>(&HookedSetGlobalVisualLodFactor),
                reinterpret_cast<LPVOID*>(&s_originalSetGlobalVisualLodFactor));
            return createStatus == MH_OK;
        },
        [&] {
            enableStatus = MH_EnableHook(target);
            return enableStatus == MH_OK;
        },
        [&] { removeStatus = MH_RemoveHook(target); },
        [&] { s_originalSetGlobalVisualLodFactor = nullptr; });

    if (result != HookInstallResult::Installed) {
        Log("GlobalVisualLODProbe", "Hook install failed (create=%d, enable=%d, remove=%d); engine behavior is unchanged.",
            static_cast<int>(createStatus), static_cast<int>(enableStatus), static_cast<int>(removeStatus));
        return;
    }

    s_targetSetGlobalVisualLodFactor = target;
    Log("GlobalVisualLODProbe", "Experimental global LOD probe installed with scale %.3g; all visual meshes may change.",
        config.globalVisualLodProbeScale);
}

void ShutdownGlobalVisualLodProbe() {
    void* target = s_targetSetGlobalVisualLodFactor;
    if (!target) return;

    const MH_STATUS disableStatus = MH_DisableHook(target);
    const MH_STATUS removeStatus = MH_RemoveHook(target);
    if (disableStatus == MH_OK && removeStatus == MH_OK) {
        s_targetSetGlobalVisualLodFactor = nullptr;
        s_originalSetGlobalVisualLodFactor = nullptr;
    } else {
        Log("GlobalVisualLODProbe", "Probe removal failed (disable=%d, remove=%d).",
            static_cast<int>(disableStatus), static_cast<int>(removeStatus));
    }
}

} // namespace G3O
