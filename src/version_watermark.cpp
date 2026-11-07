#include "g3o_build_info.h"
#include "version_watermark.h"

void InitializeVersionHook() {
    // Intentionally a no-op: hooking CFFGFCWnd::SetWindowTextA globally interferes
    // with game HUD pages and text layout widgets.
}
