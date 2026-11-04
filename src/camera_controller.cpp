#include "camera_controller.h"
#include "diagnostic_logger.h"
#include <minhook.h>
#include <cmath>

namespace G3O {

typedef void (__thiscall *SetFieldOfView_t)(void* pThis, const float* pFov);
static SetFieldOfView_t s_origSetFieldOfView = nullptr;
static float s_customFOV = 85.0f; // Default modern widescreen FOV
static void* s_activeCamera = nullptr;

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
    Log("G3O", "Dynamic FOV set to %.1f degrees.", s_customFOV);
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
    if (!hEngine) return;

    void* pSetFOV = (void*)GetProcAddress(hEngine, "?SetFieldOfView@eCCameraBase@@QAEXABM@Z");
    if (pSetFOV) {
        MH_CreateHook(pSetFOV, (void*)&HookedSetFieldOfView, (void**)&s_origSetFieldOfView);
        MH_EnableHook(pSetFOV);
        Log("G3O", "Dynamic Camera FOV controller hooked (?SetFieldOfView@eCCameraBase). Default FOV: %.1f deg.", s_customFOV);
    }
}

void ShutdownCameraController() {
    HMODULE hEngine = GetModuleHandleA("Engine.dll");
    if (hEngine) {
        void* pSetFOV = (void*)GetProcAddress(hEngine, "?SetFieldOfView@eCCameraBase@@QAEXABM@Z");
        if (pSetFOV) {
            MH_DisableHook(pSetFOV);
        }
    }
}

} // namespace G3O
