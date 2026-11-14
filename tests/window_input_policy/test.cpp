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

    ok &= Expect(G3O::IsMediaKey(0xAD), "VK_VOLUME_MUTE should be recognized as media key");
    ok &= Expect(G3O::IsMediaKey(0xAF), "VK_VOLUME_UP should be recognized as media key");
    ok &= Expect(G3O::IsMediaKey(0xB3), "VK_MEDIA_PLAY_PAUSE should be recognized as media key");
    ok &= Expect(!G3O::IsMediaKey('W'), "ordinary keys must not be recognized as media keys");
    ok &= Expect(!G3O::IsMediaKey(0x20), "Space key must not be recognized as media key");

    ok &= Expect(G3O::ShouldClipCursor(true, true, true), "clip cursor when enabled, active, and foreground");
    ok &= Expect(!G3O::ShouldClipCursor(false, true, true), "do not clip cursor when disabled");
    ok &= Expect(!G3O::ShouldClipCursor(true, false, true), "do not clip cursor when inactive");
    ok &= Expect(!G3O::ShouldClipCursor(true, true, false), "do not clip cursor when in background");

    if (!ok) return 1;
    std::puts("PASS: game window selection policy");
    return 0;
}
