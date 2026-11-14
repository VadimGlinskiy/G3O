#include "crash_guards.h"

#include "diagnostic_logger.h"
#include "crash_guards_policy.h"

#include <windows.h>
#include <MinHook.h>
#include <cstdint>
#include <cstring>
#include <atomic>

namespace G3O {
namespace {

static_assert(sizeof(void*) == 4, "Gothic 3 crash guards require Win32.");

// =============================================================================
// Diagnostic counters
// =============================================================================
static std::atomic<std::uint64_t> s_focusNameSkips{0};
static std::atomic<std::uint64_t> s_boneMatrixSkips{0};
static std::atomic<std::uint64_t> s_inventorySkips{0};

static volatile LONG s_missingActorLogged = 0;
static volatile LONG s_boneMatrixLogged = 0;
static volatile LONG s_inventoryLogged = 0;

// =============================================================================
// 1. Focus Name Guard (Game.dll / Engine.dll)
// =============================================================================
using GetPropertySetFn = void* (__thiscall*)(void*, int);
using DrawFocusNameFn = bool (__thiscall*)(void*, const void*, float);

constexpr int kNpcPropertySetType = 30;
constexpr int kAnimationPropertySetType = 100;
constexpr std::uintptr_t kDrawFocusNameRva = 0x79CB0;
constexpr std::uintptr_t kGetPropertySetRva = 0x226FD0;

constexpr std::uint8_t kDrawFocusNamePrologue[] = {
    0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x81, 0xEC, 0x94, 0x00, 0x00, 0x00
};

constexpr std::uintptr_t kNpcSpeciesValueOffset = 0x34;
constexpr std::uintptr_t kAssembledActorOffset = 0x98;

static GetPropertySetFn s_getPropertySet = nullptr;
static DrawFocusNameFn s_originalDrawFocusName = nullptr;
static void* s_drawFocusNameTarget = nullptr;
static bool s_focusNameHookCreated = false;
static bool s_focusNameHookEnabled = false;

static bool ShouldSkipMissingActor(const void* entity) noexcept {
    if (!entity || !s_getPropertySet) return false;

    __try {
        void* animationPs = s_getPropertySet(const_cast<void*>(entity), kAnimationPropertySetType);
        if (!animationPs) return false;

        void* npcPs = s_getPropertySet(const_cast<void*>(entity), kNpcPropertySetType);
        if (!npcPs) return false;

        const auto* npcBytes = static_cast<const std::uint8_t*>(npcPs);
        const auto* animationBytes = static_cast<const std::uint8_t*>(animationPs);
        const std::uint32_t species = *reinterpret_cast<const std::uint32_t*>(npcBytes + kNpcSpeciesValueOffset);
        const bool hasAssembledActor =
            *reinterpret_cast<void* const*>(animationBytes + kAssembledActorOffset) != nullptr;

        return FocusNameGuardPolicy::ShouldSkipFocusName(
            true, true, species, hasAssembledActor);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool __fastcall HookedDrawFocusName(void* self, void* /*edx*/, const void* entity, float alpha) {
    if (ShouldSkipMissingActor(entity)) {
        s_focusNameSkips.fetch_add(1, std::memory_order_relaxed);
        if (InterlockedCompareExchange(&s_missingActorLogged, 1, 0) == 0) {
            Log("G3O", "CrashGuards: Focus-name guard skipped a Human/Orc label with no assembled animation actor.");
        }
        return false;
    }

    return s_originalDrawFocusName ? s_originalDrawFocusName(self, entity, alpha) : false;
}

// =============================================================================
// 2. Bone Matrix Guard (Engine.dll)
// =============================================================================
using GetBoneMatrixIndexFn = bool (__thiscall*)(void*, int, void*);
using GetBoneMatrixNameFn = bool (__thiscall*)(void*, const void*, void*);

constexpr std::uintptr_t kGetBoneMatrixIndexRva = 0x2C6A0;
constexpr std::uintptr_t kGetBoneMatrixNameRva = 0x2C5C0;

constexpr std::uint8_t kGetBoneMatrixIndexPrologue[] = {
    0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x83, 0xEC, 0x40, 0x56, 0x57, 0x8B
};
constexpr std::uint8_t kGetBoneMatrixNamePrologue[] = {
    0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x83, 0xEC, 0x4C, 0x53, 0x56, 0x8B
};

static GetBoneMatrixIndexFn s_originalGetBoneMatrixIndex = nullptr;
static GetBoneMatrixNameFn s_originalGetBoneMatrixName = nullptr;
static void* s_getBoneMatrixIndexTarget = nullptr;
static void* s_getBoneMatrixNameTarget = nullptr;
static bool s_boneMatrixHooksCreated = false;
static bool s_boneMatrixHooksEnabled = false;

static bool CheckBoneMatrixSafety(void* self) noexcept {
    if (!BoneMatrixGuardPolicy::IsPointerSafe(self)) return false;
    __try {
        const auto* selfBytes = static_cast<const std::uint8_t*>(self);
        void* actor = *reinterpret_cast<void* const*>(selfBytes + 0x98);
        if (!BoneMatrixGuardPolicy::IsPointerSafe(actor)) return false;

        const auto* actorBytes = static_cast<const std::uint8_t*>(actor);
        void* skeleton = *reinterpret_cast<void* const*>(actorBytes + 0x0C);
        if (!BoneMatrixGuardPolicy::IsPointerSafe(skeleton)) return false;

        return BoneMatrixGuardPolicy::IsBoneMatrixSafe(self, actor, skeleton);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool __fastcall HookedGetBoneMatrixIndex(void* self, void* /*edx*/, int boneIndex, void* pMatrix) {
    if (!CheckBoneMatrixSafety(self)) {
        s_boneMatrixSkips.fetch_add(1, std::memory_order_relaxed);
        if (InterlockedCompareExchange(&s_boneMatrixLogged, 1, 0) == 0) {
            Log("G3O", "CrashGuards: BoneMatrix guard prevented Access Violation on invalid actor/skeleton.");
        }
        return false;
    }

    return s_originalGetBoneMatrixIndex ? s_originalGetBoneMatrixIndex(self, boneIndex, pMatrix) : false;
}

static bool __fastcall HookedGetBoneMatrixName(void* self, void* /*edx*/, const void* pBoneName, void* pMatrix) {
    if (!CheckBoneMatrixSafety(self)) {
        s_boneMatrixSkips.fetch_add(1, std::memory_order_relaxed);
        if (InterlockedCompareExchange(&s_boneMatrixLogged, 1, 0) == 0) {
            Log("G3O", "CrashGuards: BoneMatrix guard prevented Access Violation on invalid actor/skeleton.");
        }
        return false;
    }

    return s_originalGetBoneMatrixName ? s_originalGetBoneMatrixName(self, pBoneName, pMatrix) : false;
}

// =============================================================================
// 3. Inventory Guard (Game.dll)
// =============================================================================
using FindStackFn = const void* (__thiscall*)(void*, const void*, int, unsigned long, int);
using FindStackIndexFn = int (__thiscall*)(void*, const void*, int, unsigned long, int);

constexpr std::uintptr_t kFindStackRva = 0xB59B0;
constexpr std::uintptr_t kFindStackIndexRva = 0xB5850;

constexpr std::uint8_t kFindStackPrologue[] = {
    0x8B, 0x44, 0x24, 0x10, 0x8B, 0x54, 0x24, 0x08, 0x56, 0x50, 0x8B, 0x44
};
constexpr std::uint8_t kFindStackIndexPrologue[] = {
    0x8B, 0x44, 0x24, 0x10, 0x8B, 0x54, 0x24, 0x0C, 0x50, 0x8B, 0x44, 0x24
};

static FindStackFn s_originalFindStack = nullptr;
static FindStackIndexFn s_originalFindStackIndex = nullptr;
static void* s_findStackTarget = nullptr;
static void* s_findStackIndexTarget = nullptr;
static bool s_inventoryHooksCreated = false;
static bool s_inventoryHooksEnabled = false;

static const void* __fastcall HookedFindStack(void* self, void* /*edx*/, const void* pTemplate, int quality, unsigned long p3, int p4) {
    if (!InventoryGuardPolicy::IsFindStackCallSafe(self, pTemplate)) {
        s_inventorySkips.fetch_add(1, std::memory_order_relaxed);
        if (InterlockedCompareExchange(&s_inventoryLogged, 1, 0) == 0) {
            Log("G3O", "CrashGuards: Inventory FindStack intercepted null template entity, preventing CallFatalError ('Where is the guru?').");
        }
        return nullptr;
    }

    return s_originalFindStack ? s_originalFindStack(self, pTemplate, quality, p3, p4) : nullptr;
}

static int __fastcall HookedFindStackIndex(void* self, void* /*edx*/, const void* pTemplate, int quality, unsigned long p3, int p4) {
    if (!InventoryGuardPolicy::IsFindStackCallSafe(self, pTemplate)) {
        s_inventorySkips.fetch_add(1, std::memory_order_relaxed);
        if (InterlockedCompareExchange(&s_inventoryLogged, 1, 0) == 0) {
            Log("G3O", "CrashGuards: Inventory FindStackIndex intercepted null template entity, preventing CallFatalError ('Where is the guru?').");
        }
        return -1;
    }

    return s_originalFindStackIndex ? s_originalFindStackIndex(self, pTemplate, quality, p3, p4) : -1;
}

} // namespace

std::uint64_t GetFocusNameGuardSkips() {
    return s_focusNameSkips.load(std::memory_order_relaxed);
}

std::uint64_t GetBoneMatrixGuardSkips() {
    return s_boneMatrixSkips.load(std::memory_order_relaxed);
}

std::uint64_t GetInventoryGuardSkips() {
    return s_inventorySkips.load(std::memory_order_relaxed);
}

void InitializeCrashGuards() {
    HMODULE game = GetModuleHandleA("Game.dll");
    HMODULE engine = GetModuleHandleA("Engine.dll");
    if (!game || !engine) {
        Log("G3O", "CrashGuards not installed: Game.dll or Engine.dll is not loaded.");
        return;
    }

    const auto gameBase = reinterpret_cast<std::uintptr_t>(game);
    const auto engineBase = reinterpret_cast<std::uintptr_t>(engine);

    // 1. Install FocusNameGuard
    void* drawFocusName = reinterpret_cast<void*>(GetProcAddress(
        game, "?DrawFocusName@gCGUIEntityRenderer@@IAE_NPBVeCEntity@@M@Z"));
    s_getPropertySet = reinterpret_cast<GetPropertySetFn>(GetProcAddress(
        engine, "?GetPropertySet@eCEntity@@UAEPAVeCEntityPropertySet@@W4eEPropertySetType@@@Z"));

    if (drawFocusName && s_getPropertySet) {
        const auto drawRva = reinterpret_cast<std::uintptr_t>(drawFocusName) - gameBase;
        const auto propertySetRva = reinterpret_cast<std::uintptr_t>(s_getPropertySet) - engineBase;
        if (drawRva == kDrawFocusNameRva && propertySetRva == kGetPropertySetRva &&
            std::memcmp(drawFocusName, kDrawFocusNamePrologue, sizeof(kDrawFocusNamePrologue)) == 0) {
            if (MH_CreateHook(drawFocusName, reinterpret_cast<void*>(&HookedDrawFocusName),
                              reinterpret_cast<void**>(&s_originalDrawFocusName)) == MH_OK) {
                s_drawFocusNameTarget = drawFocusName;
                s_focusNameHookCreated = true;
                if (MH_EnableHook(drawFocusName) == MH_OK) {
                    s_focusNameHookEnabled = true;
                    Log("G3O", "CrashGuards: Focus-name guard installed successfully.");
                }
            }
        } else {
            Log("G3O", "CrashGuards: Focus-name guard skipped: binary signature mismatch.");
        }
    }

    // 2. Install BoneMatrixGuard
    void* getBoneMatrixIndex = reinterpret_cast<void*>(GetProcAddress(
        engine, "?GetBoneMatrix@eCVisualAnimation_PS@@QAE_NHAAVbCMatrix@@@Z"));
    void* getBoneMatrixName = reinterpret_cast<void*>(GetProcAddress(
        engine, "?GetBoneMatrix@eCVisualAnimation_PS@@QAE_NABVbCString@@AAVbCMatrix@@@Z"));

    if (getBoneMatrixIndex && getBoneMatrixName) {
        const auto indexRva = reinterpret_cast<std::uintptr_t>(getBoneMatrixIndex) - engineBase;
        const auto nameRva = reinterpret_cast<std::uintptr_t>(getBoneMatrixName) - engineBase;
        if (indexRva == kGetBoneMatrixIndexRva && nameRva == kGetBoneMatrixNameRva &&
            std::memcmp(getBoneMatrixIndex, kGetBoneMatrixIndexPrologue, sizeof(kGetBoneMatrixIndexPrologue)) == 0 &&
            std::memcmp(getBoneMatrixName, kGetBoneMatrixNamePrologue, sizeof(kGetBoneMatrixNamePrologue)) == 0) {

            bool ok1 = MH_CreateHook(getBoneMatrixIndex, reinterpret_cast<void*>(&HookedGetBoneMatrixIndex),
                                     reinterpret_cast<void**>(&s_originalGetBoneMatrixIndex)) == MH_OK;
            bool ok2 = MH_CreateHook(getBoneMatrixName, reinterpret_cast<void*>(&HookedGetBoneMatrixName),
                                     reinterpret_cast<void**>(&s_originalGetBoneMatrixName)) == MH_OK;
            if (ok1 && ok2) {
                s_getBoneMatrixIndexTarget = getBoneMatrixIndex;
                s_getBoneMatrixNameTarget = getBoneMatrixName;
                s_boneMatrixHooksCreated = true;
                if (MH_EnableHook(getBoneMatrixIndex) == MH_OK &&
                    MH_EnableHook(getBoneMatrixName) == MH_OK) {
                    s_boneMatrixHooksEnabled = true;
                    Log("G3O", "CrashGuards: BoneMatrix guard installed successfully.");
                }
            }
        } else {
            Log("G3O", "CrashGuards: BoneMatrix guard skipped: binary signature mismatch.");
        }
    }

    // 3. Install InventoryGuard
    void* findStack = reinterpret_cast<void*>(GetProcAddress(
        game, "?FindStack@gCInventory_PS@@QBEPBVgCInventoryStack@@PBVeCTemplateEntity@@W4gEInventoryFindQuality@@KH@Z"));
    void* findStackIndex = reinterpret_cast<void*>(GetProcAddress(
        game, "?FindStackIndex@gCInventory_PS@@QBEHPBVeCTemplateEntity@@W4gEInventoryFindQuality@@KH@Z"));

    if (findStack && findStackIndex) {
        const auto fsRva = reinterpret_cast<std::uintptr_t>(findStack) - gameBase;
        const auto fsiRva = reinterpret_cast<std::uintptr_t>(findStackIndex) - gameBase;
        if (fsRva == kFindStackRva && fsiRva == kFindStackIndexRva &&
            std::memcmp(findStack, kFindStackPrologue, sizeof(kFindStackPrologue)) == 0 &&
            std::memcmp(findStackIndex, kFindStackIndexPrologue, sizeof(kFindStackIndexPrologue)) == 0) {

            bool ok1 = MH_CreateHook(findStack, reinterpret_cast<void*>(&HookedFindStack),
                                     reinterpret_cast<void**>(&s_originalFindStack)) == MH_OK;
            bool ok2 = MH_CreateHook(findStackIndex, reinterpret_cast<void*>(&HookedFindStackIndex),
                                     reinterpret_cast<void**>(&s_originalFindStackIndex)) == MH_OK;
            if (ok1 && ok2) {
                s_findStackTarget = findStack;
                s_findStackIndexTarget = findStackIndex;
                s_inventoryHooksCreated = true;
                if (MH_EnableHook(findStack) == MH_OK &&
                    MH_EnableHook(findStackIndex) == MH_OK) {
                    s_inventoryHooksEnabled = true;
                    Log("G3O", "CrashGuards: Inventory null-template guard installed successfully.");
                }
            }
        } else {
            Log("G3O", "CrashGuards: Inventory null-template guard skipped: binary signature mismatch.");
        }
    }
}

void ShutdownCrashGuards() {
    if (s_focusNameHookEnabled && s_drawFocusNameTarget) {
        MH_DisableHook(s_drawFocusNameTarget);
        s_focusNameHookEnabled = false;
    }
    if (s_focusNameHookCreated && s_drawFocusNameTarget) {
        MH_RemoveHook(s_drawFocusNameTarget);
        s_focusNameHookCreated = false;
        s_drawFocusNameTarget = nullptr;
    }
    s_originalDrawFocusName = nullptr;
    s_getPropertySet = nullptr;

    if (s_boneMatrixHooksEnabled) {
        if (s_getBoneMatrixIndexTarget) MH_DisableHook(s_getBoneMatrixIndexTarget);
        if (s_getBoneMatrixNameTarget) MH_DisableHook(s_getBoneMatrixNameTarget);
        s_boneMatrixHooksEnabled = false;
    }
    if (s_boneMatrixHooksCreated) {
        if (s_getBoneMatrixIndexTarget) {
            MH_RemoveHook(s_getBoneMatrixIndexTarget);
            s_getBoneMatrixIndexTarget = nullptr;
        }
        if (s_getBoneMatrixNameTarget) {
            MH_RemoveHook(s_getBoneMatrixNameTarget);
            s_getBoneMatrixNameTarget = nullptr;
        }
        s_boneMatrixHooksCreated = false;
    }
    s_originalGetBoneMatrixIndex = nullptr;
    s_originalGetBoneMatrixName = nullptr;

    if (s_inventoryHooksEnabled) {
        if (s_findStackTarget) MH_DisableHook(s_findStackTarget);
        if (s_findStackIndexTarget) MH_DisableHook(s_findStackIndexTarget);
        s_inventoryHooksEnabled = false;
    }
    if (s_inventoryHooksCreated) {
        if (s_findStackTarget) {
            MH_RemoveHook(s_findStackTarget);
            s_findStackTarget = nullptr;
        }
        if (s_findStackIndexTarget) {
            MH_RemoveHook(s_findStackIndexTarget);
            s_findStackIndexTarget = nullptr;
        }
        s_inventoryHooksCreated = false;
    }
    s_originalFindStack = nullptr;
    s_originalFindStackIndex = nullptr;
}

} // namespace G3O
