#include "crash_guards_policy.h"

#include <cstdio>
#include <cstdint>

static bool Expect(bool condition, const char* message) {
    if (condition) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

int main() {
    bool ok = true;

    // 1. FocusNameGuardPolicy
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

    // 2. BoneMatrixGuardPolicy
    using G3O::BoneMatrixGuardPolicy::IsPointerSafe;
    using G3O::BoneMatrixGuardPolicy::IsBoneMatrixSafe;

    ok &= Expect(!IsPointerSafe(nullptr), "null pointer must not be safe");
    ok &= Expect(!IsPointerSafe(reinterpret_cast<const void*>(0x1000)), "pointer below 64KB must not be safe");
    ok &= Expect(!IsPointerSafe(reinterpret_cast<const void*>(0xFFFF)), "pointer at 0xFFFF must not be safe");
    ok &= Expect(IsPointerSafe(reinterpret_cast<const void*>(0x10000)), "pointer at 0x10000 must be safe");
    ok &= Expect(IsPointerSafe(reinterpret_cast<const void*>(0x00500000)), "normal heap/module pointer must be safe");

    const void* validSelf = reinterpret_cast<const void*>(0x02000000);
    const void* validActor = reinterpret_cast<const void*>(0x03000000);
    const void* validSkeleton = reinterpret_cast<const void*>(0x04000000);

    ok &= Expect(IsBoneMatrixSafe(validSelf, validActor, validSkeleton), "valid self, actor, skeleton must be safe");
    ok &= Expect(!IsBoneMatrixSafe(validSelf, nullptr, validSkeleton), "null actor must NOT be safe");
    ok &= Expect(!IsBoneMatrixSafe(validSelf, validActor, nullptr), "null skeleton must NOT be safe");
    ok &= Expect(!IsBoneMatrixSafe(nullptr, validActor, validSkeleton), "null self must NOT be safe");
    ok &= Expect(!IsBoneMatrixSafe(validSelf, reinterpret_cast<const void*>(0x120), validSkeleton), "low actor must NOT be safe");

    // 3. InventoryGuardPolicy
    using G3O::InventoryGuardPolicy::IsInventorySafe;
    using G3O::InventoryGuardPolicy::IsTemplateEntitySafe;
    using G3O::InventoryGuardPolicy::IsFindStackCallSafe;

    const void* validInventory = reinterpret_cast<const void*>(0x05000000);
    const void* validTemplate = reinterpret_cast<const void*>(0x06000000);

    ok &= Expect(IsFindStackCallSafe(validInventory, validTemplate), "valid inventory and template must be safe");
    ok &= Expect(!IsFindStackCallSafe(validInventory, nullptr), "null template must NOT be safe (would call FatalError)");
    ok &= Expect(!IsFindStackCallSafe(nullptr, validTemplate), "null inventory must NOT be safe");
    ok &= Expect(!IsFindStackCallSafe(nullptr, nullptr), "null inventory and null template must NOT be safe");
    ok &= Expect(!IsFindStackCallSafe(validInventory, reinterpret_cast<const void*>(0x50)), "low template pointer must NOT be safe");

    if (!ok) return 1;
    std::puts("PASS: crash guards policy (FocusName, BoneMatrix, Inventory)");
    return 0;
}
