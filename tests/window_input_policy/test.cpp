#include "window_input_policy.h"
#include <cstdio>

static bool Expect(bool condition, const char* message) {
    if (condition) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

int main() {
    bool ok = true;
    G3O::Config config{};
    config.enableBorderless = false;
    config.enableDynamicFOV = false;
    config.enableFOVHotkeys = true;
    ok &= Expect(!G3O::ShouldInstallWindowInputHook(config),
                 "window hook should not install when borderless and dynamic FOV are disabled");
    config.enableDynamicFOV = true;
    ok &= Expect(G3O::ShouldInstallWindowInputHook(config),
                 "FOV hotkeys should install the input hook without borderless mode");
    config.enableFOVHotkeys = false;
    ok &= Expect(!G3O::ShouldInstallWindowInputHook(config),
                 "dynamic FOV alone should not install the hotkey window hook");
    config.enableBorderless = true;
    ok &= Expect(G3O::ShouldInstallWindowInputHook(config),
                 "borderless mode should install the window hook");

    ok &= Expect(G3O::IsInitialKeyDown(0), "first keydown should be accepted");
    ok &= Expect(!G3O::IsInitialKeyDown(static_cast<LPARAM>(1u << 30)),
                 "autorepeated keydown should not retrigger the borderless toggle");

    if (!ok) return 1;
    std::puts("PASS: window input policy and key-repeat detection");
    return 0;
}
