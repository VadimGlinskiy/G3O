#pragma once
#include <windows.h>

namespace G3O {

void InitializeBorderlessWindow();
void ShutdownBorderlessWindow();

// Toggle borderless fullscreen on / off dynamically
void ToggleBorderlessFullscreen();

// Check if borderless fullscreen is currently active
bool IsBorderlessActive();

// Return the main game HWND
HWND GetGameWindowHandle();

} // namespace G3O
