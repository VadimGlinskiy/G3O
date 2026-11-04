#pragma once
#include <windows.h>

namespace G3O {

void InitializeCameraController();
void ShutdownCameraController();

// Adjust FOV dynamically by delta degrees (e.g. +5.0f or -5.0f)
void AdjustDynamicFOV(float delta);

// Get current FOV in degrees
float GetCurrentFOV();

// Force set FOV
void SetCustomFOV(float degrees);

} // namespace G3O
