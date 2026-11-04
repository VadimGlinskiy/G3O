#include "borderless_window.h"
#include "diagnostic_logger.h"
#include "camera_controller.h"
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
    switch (uMsg) {
    case WM_ACTIVATEAPP:
        if (s_isBorderless && wParam == FALSE) {
            // Prevent Gothic 3 from freezing or minimizing on Alt+Tab focus loss
            return 0;
        }
        break;

    case WM_SYSCOMMAND:
        if (s_isBorderless && (wParam & 0xFFF0) == SC_MINIMIZE) {
            // Ignore minimize command in borderless mode
            return 0;
        }
        break;

    case WM_KEYDOWN:
        // Hotkey: F11 -> Toggle Borderless Fullscreen
        if (wParam == VK_F11) {
            ToggleBorderlessFullscreen();
            return 0;
        }
        // Hotkey: Ctrl + '[' or Ctrl + NumPad '-' -> Decrease FOV
        if ((GetKeyState(VK_CONTROL) & 0x8000) && (wParam == VK_OEM_4 || wParam == VK_SUBTRACT)) {
            AdjustDynamicFOV(-5.0f);
            return 0;
        }
        // Hotkey: Ctrl + ']' or Ctrl + NumPad '+' -> Increase FOV
        if ((GetKeyState(VK_CONTROL) & 0x8000) && (wParam == VK_OEM_6 || wParam == VK_ADD)) {
            AdjustDynamicFOV(+5.0f);
            return 0;
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

    LONG_PTR newStyle = s_origStyle;
    newStyle &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
    newStyle |= WS_POPUP;

    SetWindowLongPtrA(hWnd, GWL_STYLE, newStyle);
    SetWindowPos(hWnd, HWND_TOP, screenX, screenY, screenW, screenH, SWP_FRAMECHANGED | SWP_SHOWWINDOW);

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
    int attempts = 100;
    while (attempts-- > 0 && !s_gameHWnd) {
        HWND found = nullptr;
        EnumWindows(EnumWindowsCallback, (LPARAM)&found);
        if (found) {
            s_gameHWnd = found;
            break;
        }
        Sleep(50);
    }

    if (s_gameHWnd) {
        // Subclass window procedure
        s_origWndProc = (WNDPROC)SetWindowLongPtrA(s_gameHWnd, GWLP_WNDPROC, (LONG_PTR)BorderlessWndProc);
        Log("G3O", "Game window captured (HWND 0x%p). Subclassed WndProc.", s_gameHWnd);

        // Apply Borderless Fullscreen automatically
        ApplyBorderless(s_gameHWnd);
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
