#include "borderless_window.h"
#include "diagnostic_logger.h"
#include "config_manager.h"
#include "window_input_policy.h"
#include <minhook.h>

namespace G3O {

static HWND s_gameHWnd = nullptr;
static WNDPROC s_origWndProc = nullptr;
static bool s_isBorderless = false;
static LONG_PTR s_origStyle = 0;

HWND GetGameWindowHandle() {
    return s_gameHWnd;
}

bool IsBorderlessActive() {
    return s_isBorderless;
}

static void UpdateCursorClip(HWND hWnd, bool bActive) {
    const Config& cfg = GetConfig();
    const bool isForeground = (GetForegroundWindow() == hWnd);
    if (!ShouldClipCursor(cfg.windowCursorLock, bActive, isForeground)) {
        ClipCursor(nullptr);
        return;
    }

    if (hWnd && IsWindow(hWnd)) {
        RECT rcClient = {};
        if (GetClientRect(hWnd, &rcClient)) {
            POINT pt1 = { rcClient.left, rcClient.top };
            POINT pt2 = { rcClient.right, rcClient.bottom };
            ClientToScreen(hWnd, &pt1);
            ClientToScreen(hWnd, &pt2);
            RECT rcClip = { pt1.x, pt1.y, pt2.x, pt2.y };
            ClipCursor(&rcClip);
            return;
        }
    }
    ClipCursor(nullptr);
}

static LRESULT CALLBACK BorderlessWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    const Config& cfg = GetConfig();

    switch (uMsg) {
    case WM_ACTIVATE:
        UpdateCursorClip(hWnd, LOWORD(wParam) != WA_INACTIVE);
        break;

    case WM_SETFOCUS:
        UpdateCursorClip(hWnd, true);
        break;

    case WM_KILLFOCUS:
        UpdateCursorClip(hWnd, false);
        break;

    case WM_SIZE:
    case WM_WINDOWPOSCHANGED:
        if (GetForegroundWindow() == hWnd) {
            UpdateCursorClip(hWnd, true);
        }
        break;

    case WM_ACTIVATEAPP:
        UpdateCursorClip(hWnd, wParam != FALSE);
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

    case WM_APPCOMMAND:
        if (cfg.inputSystemKeyPassthrough) {
            // Forward multimedia/app commands directly to DefWindowProc so the OS handles them
            return DefWindowProcA(hWnd, uMsg, wParam, lParam);
        }
        break;

    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
        if (cfg.inputSystemKeyPassthrough && IsMediaKey(wParam)) {
            return DefWindowProcA(hWnd, uMsg, wParam, lParam);
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
        s_origStyle = GetWindowLongPtrA(hWnd, GWL_STYLE);
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
    UpdateCursorClip(hWnd, true);
    Log("G3O", "Borderless Fullscreen activated: %dx%d on display (%d, %d).", screenW, screenH, screenX, screenY);
}

struct WindowSearchContext {
    DWORD processId;
    HWND preferredWindow;
    HWND fallbackWindow;
    long long fallbackArea;
};

static BOOL CALLBACK EnumWindowsCallback(HWND hWnd, LPARAM lParam) {
    auto* search = reinterpret_cast<WindowSearchContext*>(lParam);
    DWORD pid = 0;
    GetWindowThreadProcessId(hWnd, &pid);
    if (pid != search->processId || !IsWindowVisible(hWnd) || GetWindow(hWnd, GW_OWNER)) {
        return TRUE;
    }

    char className[256] = { 0 };
    GetClassNameA(hWnd, className, sizeof(className));
    if (IsPreferredGameWindowClass(className)) {
        search->preferredWindow = hWnd;
        return FALSE;
    }
    if (IsSplashWindowClass(className)) return TRUE;

    RECT rect = {};
    if (GetWindowRect(hWnd, &rect)) {
        const long long width = static_cast<long long>(rect.right) - rect.left;
        const long long height = static_cast<long long>(rect.bottom) - rect.top;
        const long long area = width > 0 && height > 0 ? width * height : 0;
        if (area > search->fallbackArea) {
            search->fallbackArea = area;
            search->fallbackWindow = hWnd;
        }
    }
    return TRUE;
}

static DWORD WINAPI WindowWatcherThread(LPVOID) {
    // Gothic 3 takes 15-30 seconds to load into main menu
    for (int i = 0; i < 180 && !s_gameHWnd; ++i) {
        WindowSearchContext search = { GetCurrentProcessId(), nullptr, nullptr, 0 };
        EnumWindows(EnumWindowsCallback, reinterpret_cast<LPARAM>(&search));
        HWND found = search.preferredWindow ? search.preferredWindow : search.fallbackWindow;
        if (found && IsWindow(found) && IsWindowVisible(found)) {
            s_gameHWnd = found;
            break;
        }
        Sleep(250);
    }

    if (!s_gameHWnd) {
        Log("G3O", "Game window not found; borderless is inactive.");
        return 0;
    }

    if (s_gameHWnd) {
        // Record the original window style
        s_origStyle = GetWindowLongPtrA(s_gameHWnd, GWL_STYLE);

        // Subclass window procedure
        SetLastError(ERROR_SUCCESS);
        s_origWndProc = (WNDPROC)SetWindowLongPtrA(s_gameHWnd, GWLP_WNDPROC, (LONG_PTR)BorderlessWndProc);
        if (!s_origWndProc) {
            Log("G3O", "Game window WndProc hook failed (error %lu).", GetLastError());
            s_gameHWnd = nullptr;
            return 0;
        }
        char className[256] = { 0 };
        GetClassNameA(s_gameHWnd, className, sizeof(className));
        Log("G3O", "Game window captured (HWND 0x%p, class '%s', style 0x%08X). Subclassed WndProc.",
            s_gameHWnd, className, (unsigned int)s_origStyle);

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
            Log("G3O", "Window hook active (borderless=%s).", s_isBorderless ? "yes" : "no");
        } else {
            s_isBorderless = false;
            Log("G3O", "Window hook active; borderless mode is disabled.");
        }
    }
    return 0;
}

void InitializeBorderlessWindow() {
    HANDLE hThread = CreateThread(nullptr, 0, WindowWatcherThread, nullptr, 0, nullptr);
    if (hThread) CloseHandle(hThread);
    else Log("G3O", "Game window watcher thread could not be started (error %lu).", GetLastError());
}

void ShutdownBorderlessWindow() {
    ClipCursor(nullptr);
    if (s_gameHWnd && s_origWndProc) {
        SetWindowLongPtrA(s_gameHWnd, GWLP_WNDPROC, (LONG_PTR)s_origWndProc);
        s_origWndProc = nullptr;
    }
}

} // namespace G3O
