#include "focus_name_guard_policy.h"

#include <cstdio>

static bool Expect(bool condition, const char* message) {
    if (condition) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

int main() {
    bool ok = true;
    using G3O::FocusNameGuardPolicy::ShouldSkipFocusName;
    using G3O::FocusNameGuardPolicy::kHumanSpecies;
    using G3O::FocusNameGuardPolicy::kOrcSpecies;

    ok &= Expect(ShouldSkipFocusName(true, true, kHumanSpecies, false),
                 "skip a human focus name when its assembled actor is missing");
    ok &= Expect(ShouldSkipFocusName(true, true, kOrcSpecies, false),
                 "skip an orc focus name when its assembled actor is missing");
    ok &= Expect(!ShouldSkipFocusName(true, true, kHumanSpecies, true),
                 "preserve focus-name rendering when the actor exists");
    ok &= Expect(!ShouldSkipFocusName(true, true, 6, false),
                 "do not apply the human/orc guard to other species");
    ok &= Expect(!ShouldSkipFocusName(false, true, kHumanSpecies, false),
                 "leave a missing animation property set to the original path");
    ok &= Expect(!ShouldSkipFocusName(true, false, kHumanSpecies, false),
                 "leave a missing NPC property set to the original path");

    if (!ok) return 1;
    std::puts("PASS: focus-name guard policy");
    return 0;
}
