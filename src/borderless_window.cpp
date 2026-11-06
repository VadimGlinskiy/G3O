#include "borderless_window.h"
#include "diagnostic_logger.h"
#include "camera_controller.h"
#include "config_manager.h"
#include "window_input_policy.h"
#include <minhook.h>

namespace G3O {

static HWND s_gameHWnd = nullptr;
static WNDPROC s_origWndProc = nullptr;
static bool s_isBorderless = false;
static RECT s_origWindowRect = { 0 };
static LONG_PTR s_origStyle = 0;
static LONG_PTR s_origExStyle = 0;

HWND GetGameWindowHandle() {
    return s_gameHWnd;
}

bool IsBorderlessActive() {
    return s_isBorderless;
}

static LRESULT CALLBACK BorderlessWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    const Config& cfg = GetConfig();

    switch (uMsg) {
    case WM_ACTIVATEAPP:
        if (cfg.preventMinimizeOnFocusLoss && s_isBorderless && wParam == FALSE) {
            // Prevent Gothic 3 from freezing or minimizing on Alt+Tab focus loss
            return 0;
        }
        break;

    case WM_SYSCOMMAND:
        if (cfg.preventMinimizeOnFocusLoss && s_isBorderless && (wParam & 0xFFF0) == SC_MINIMIZE) {
            // Ignore minimize command in borderless mode
            return 0;
        }
        break;

    case WM_KEYDOWN:
        // Hotkey: Configured key -> Toggle Borderless Fullscreen
        if (cfg.enableBorderless && IsInitialKeyDown(lParam) &&
            cfg.toggleBorderlessKey != 0 && wParam == cfg.toggleBorderlessKey) {
            ToggleBorderlessFullscreen();
            return 0;
        }

        // Hotkeys: Dynamic FOV adjustment
        if (cfg.enableDynamicFOV && cfg.enableFOVHotkeys &&
            (GetKeyState(VK_CONTROL) & 0x8000)) {
            if ((cfg.decreaseFOVKey != 0 && wParam == cfg.decreaseFOVKey) || wParam == VK_SUBTRACT) {
                AdjustDynamicFOV(-cfg.fovStep);
                return 0;
            }
            if ((cfg.increaseFOVKey != 0 && wParam == cfg.increaseFOVKey) || wParam == VK_ADD) {
                AdjustDynamicFOV(+cfg.fovStep);
                return 0;
            }
        }
        break;
    }

    if (s_origWndProc) {
        return CallWindowProcA(s_origWndProc, hWnd, uMsg, wParam, lParam);
    }
    return DefWindowProcA(hWnd, uMsg, wParam, lParam);
}

static void ApplyBorderless(HWND hWnd) {
    if (!hWnd || !IsWindow(hWnd)) return;

    if (!s_isBorderless) {
        GetWindowRect(hWnd, &s_origWindowRect);
        s_origStyle = GetWindowLongPtrA(hWnd, GWL_STYLE);
        s_origExStyle = GetWindowLongPtrA(hWnd, GWL_EXSTYLE);
    }

    HMONITOR hMon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(MONITORINFO) };
    if (!GetMonitorInfoA(hMon, &mi)) return;

    int screenX = mi.rcMonitor.left;
    int screenY = mi.rcMonitor.top;
    int screenW = mi.rcMonitor.right - mi.rcMonitor.left;
    int screenH = mi.rcMonitor.bottom - mi.rcMonitor.top;

    if (screenW <= 0 || screenH <= 0) return;

    LONG_PTR newStyle = s_origStyle;
    newStyle &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
    newStyle |= WS_POPUP;

    SetWindowLongPtrA(hWnd, GWL_STYLE, newStyle);
    SetWindowPos(hWnd, HWND_TOP, screenX, screenY, screenW, screenH, SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);

    s_isBorderless = true;
    Log("G3O", "Borderless Fullscreen activated: %dx%d on display (%d, %d).", screenW, screenH, screenX, screenY);
}

static void RestoreWindowed(HWND hWnd) {
    if (!hWnd || !IsWindow(hWnd) || !s_isBorderless) return;

    SetWindowLongPtrA(hWnd, GWL_STYLE, s_origStyle);
    SetWindowLongPtrA(hWnd, GWL_EXSTYLE, s_origExStyle);

    int w = s_origWindowRect.right - s_origWindowRect.left;
    int h = s_origWindowRect.bottom - s_origWindowRect.top;
    SetWindowPos(hWnd, HWND_NOTOPMOST, s_origWindowRect.left, s_origWindowRect.top, w, h, SWP_FRAMECHANGED | SWP_SHOWWINDOW);

    s_isBorderless = false;
    Log("G3O", "Restored standard windowed mode.");
}

void ToggleBorderlessFullscreen() {
    if (!s_gameHWnd) return;
    if (s_isBorderless) {
        RestoreWindowed(s_gameHWnd);
    } else {
        ApplyBorderless(s_gameHWnd);
    }
}

static BOOL CALLBACK EnumWindowsCallback(HWND hWnd, LPARAM lParam) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hWnd, &pid);
    if (pid == GetCurrentProcessId()) {
        char className[256] = { 0 };
        GetClassNameA(hWnd, className, sizeof(className));
        if (strcmp(className, "CFFGFCWnd") == 0 || strcmp(className, "Gothic3") == 0) {
            *(HWND*)lParam = hWnd;
            return FALSE;
        }
    }
    return TRUE;
}

static DWORD WINAPI WindowWatcherThread(LPVOID) {
    // Gothic 3 takes 15-30 seconds to load into main menu
    for (int i = 0; i < 180 && !s_gameHWnd; ++i) {
        HWND found = nullptr;
        EnumWindows(EnumWindowsCallback, (LPARAM)&found);
        if (found && IsWindow(found) && IsWindowVisible(found)) {
            s_gameHWnd = found;
            break;
        }
        Sleep(250);
    }

    if (s_gameHWnd) {
        // Record original window dimensions and styles
        GetWindowRect(s_gameHWnd, &s_origWindowRect);
        s_origStyle = GetWindowLongPtrA(s_gameHWnd, GWL_STYLE);
        s_origExStyle = GetWindowLongPtrA(s_gameHWnd, GWL_EXSTYLE);

        // Subclass window procedure
        s_origWndProc = (WNDPROC)SetWindowLongPtrA(s_gameHWnd, GWLP_WNDPROC, (LONG_PTR)BorderlessWndProc);
        Log("G3O", "Game window captured (HWND 0x%p, style 0x%08X). Subclassed WndProc.", s_gameHWnd, (unsigned int)s_origStyle);

        // Only convert frame if window is running in traditional windowed mode (has caption/borders).
        // If the game is already in fullscreen mode (WS_POPUP without WS_CAPTION), DXVK and the engine
        // already control presentation; calling SetWindowPos/SetWindowLongPtr triggers an abrupt
        // D3D9 device reset which causes Vulkan driver crashes (amdvlk32.dll on AMD GPUs).
        const Config& cfg = GetConfig();
        if (cfg.enableBorderless && (s_origStyle & WS_CAPTION) && cfg.autoBorderless) {
            Log("G3O", "Detected windowed mode with borders. Applying Borderless Fullscreen.");
            Sleep(500);
            ApplyBorderless(s_gameHWnd);
        } else if (cfg.enableBorderless) {
            s_isBorderless = (s_origStyle & WS_CAPTION) == 0;
            Log("G3O", "Window hook active (borderless=%s, ToggleKey=%s).",
                s_isBorderless ? "yes" : "no", KeyCodeToString(cfg.toggleBorderlessKey).c_str());
        } else {
            s_isBorderless = false;
            Log("G3O", "Window input hook active for dynamic FOV hotkeys; borderless mode is disabled.");
        }
    }
    return 0;
}

void InitializeBorderlessWindow() {
    HANDLE hThread = CreateThread(nullptr, 0, WindowWatcherThread, nullptr, 0, nullptr);
    if (hThread) CloseHandle(hThread);
}

void ShutdownBorderlessWindow() {
    if (s_gameHWnd && s_origWndProc) {
        SetWindowLongPtrA(s_gameHWnd, GWLP_WNDPROC, (LONG_PTR)s_origWndProc);
        s_origWndProc = nullptr;
    }
}

} // namespace G3O
