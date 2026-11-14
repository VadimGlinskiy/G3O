#include "window_input_policy.h"
#include <cstdio>

static bool Expect(bool condition, const char* message) {
    if (condition) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

int main() {
    bool ok = true;
    ok &= Expect(G3O::IsSplashWindowClass("Splash"),
                 "the splash window must not be selected as the game input target");
    ok &= Expect(!G3O::IsPreferredGameWindowClass("Splash"),
                 "the splash window must not be preferred over the game window");
    ok &= Expect(G3O::IsPreferredGameWindowClass("Gothic3 Engine"),
                 "the actual Gothic 3 engine window class should be preferred");
    ok &= Expect(G3O::IsPreferredGameWindowClass("Gothic3"),
                 "the legacy Gothic 3 window class should remain supported");

    if (!ok) return 1;
    std::puts("PASS: game window selection policy");
    return 0;
}
