#include "camera_controller.h"
#include "diagnostic_logger.h"
#include "config_manager.h"
#include <minhook.h>
#include <cmath>

namespace G3O {

typedef void (__thiscall *SetFieldOfView_t)(void* pThis, const float* pFov);
static SetFieldOfView_t s_origSetFieldOfView = nullptr;
static float s_customFOV = 85.0f;
static void* s_activeCamera = nullptr;
static void* s_targetSetFieldOfView = nullptr;

static void __fastcall HookedSetFieldOfView(void* pThis, void* /*edx*/, const float* pFov) {
    s_activeCamera = pThis;
    if (pFov) {
        // If the game sets standard gameplay FOV (50-75 degrees), override with our widescreen FOV
        if (*pFov >= 50.0f && *pFov <= 75.0f) {
            s_origSetFieldOfView(pThis, &s_customFOV);
            return;
        }
    }
    s_origSetFieldOfView(pThis, pFov);
}

void SetCustomFOV(float degrees) {
    if (degrees < 50.0f) degrees = 50.0f;
    if (degrees > 115.0f) degrees = 115.0f;
    s_customFOV = degrees;
    Log("G3O", "Camera FOV set to %.1f degrees.", s_customFOV);
    if (s_activeCamera && s_origSetFieldOfView) {
        s_origSetFieldOfView(s_activeCamera, &s_customFOV);
    }
}

void AdjustDynamicFOV(float delta) {
    SetCustomFOV(s_customFOV + delta);
}

float GetCurrentFOV() {
    return s_customFOV;
}

void InitializeCameraController() {
    HMODULE hEngine = GetModuleHandleA("Engine.dll");
    if (!hEngine) {
        Log("G3O", "FOV hook not installed: Engine.dll is not loaded.");
        return;
    }

    s_customFOV = GetConfig().defaultFOV;
    if (s_customFOV < 50.0f) s_customFOV = 50.0f;
    if (s_customFOV > 115.0f) s_customFOV = 115.0f;

    void* target = (void*)GetProcAddress(hEngine, "?SetFieldOfView@eCCameraBase@@QAEXABM@Z");
    if (!target) {
        Log("G3O", "FOV hook not installed: Engine.dll export eCCameraBase::SetFieldOfView was not found.");
        return;
    }

    MH_STATUS status = MH_CreateHook(target, (void*)&HookedSetFieldOfView, (void**)&s_origSetFieldOfView);
    if (status != MH_OK) {
        s_origSetFieldOfView = nullptr;
        Log("G3O", "FOV hook creation failed (MinHook code %d).", (int)status);
        return;
    }

    status = MH_EnableHook(target);
    if (status != MH_OK) {
        MH_RemoveHook(target);
        s_origSetFieldOfView = nullptr;
        Log("G3O", "FOV hook activation failed (MinHook code %d).", (int)status);
        return;
    }

    s_targetSetFieldOfView = target;
    Log("G3O", "FOV hook enabled for engine values from 50 to 75 degrees. DefaultFOV: %.1f degrees.", s_customFOV);
}

void ShutdownCameraController() {
    if (!s_targetSetFieldOfView) return;
    MH_DisableHook(s_targetSetFieldOfView);
    MH_RemoveHook(s_targetSetFieldOfView);
    s_targetSetFieldOfView = nullptr;
    s_origSetFieldOfView = nullptr;
}

} // namespace G3O
