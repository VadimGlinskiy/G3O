#include "profiler_sample_policy.h"
#include <cstdio>
#include <string>

static bool Expect(bool condition, const char* message) {
    if (condition) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

int main() {
    bool ok = true;
    ok &= Expect(!G3O::IsGameplayCaptureActive(false, false),
                 "sampling must remain off before a game session starts");
    ok &= Expect(!G3O::IsGameplayCaptureActive(true, true),
                 "sampling must pause during savegame loading");
    ok &= Expect(G3O::IsGameplayCaptureActive(true, false),
                 "sampling may run after a session starts and loading completes");

    ok &= Expect(!G3O::ShouldRecordGameplayFrameDelta(100),
                 "sub-100us frame intervals should remain filtered as noise");
    ok &= Expect(G3O::ShouldRecordGameplayFrameDelta(101),
                 "ordinary frame intervals should be recorded");
    ok &= Expect(G3O::ShouldRecordGameplayFrameDelta(500000),
                 "a 500ms stall must remain in gameplay statistics");
    ok &= Expect(G3O::ShouldRecordGameplayFrameDelta(7000000),
                 "multi-second gameplay stalls must remain in gameplay statistics");

    ok &= Expect(G3O::NormalizeHotspotKey("Engine.dll!gCWorld::Load+0x14")
                     == "Engine.dll!gCWorld::Load",
                 "named exports should aggregate their instruction offsets");
    ok &= Expect(G3O::NormalizeHotspotKey("Engine.dll+0x1234")
                     == "Engine.dll+0x1234",
                 "unexported EIP addresses must not collapse into a module label");
    return ok ? 0 : 1;
}
