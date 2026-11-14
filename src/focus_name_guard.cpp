#include "focus_name_guard.h"

#include "diagnostic_logger.h"
#include "focus_name_guard_policy.h"

#include <windows.h>
#include <MinHook.h>
#include <cstdint>
#include <cstring>

namespace G3O {
namespace {

static_assert(sizeof(void*) == 4, "Gothic 3 focus-name guard requires Win32.");

using GetPropertySetFn = void* (__thiscall*)(void*, int);
using DrawFocusNameFn = bool (__thiscall*)(void*, const void*, float);

constexpr int kNpcPropertySetType = 30;
constexpr int kAnimationPropertySetType = 100;
constexpr std::uintptr_t kDrawFocusNameRva = 0x79CB0;
constexpr std::uintptr_t kGetPropertySetRva = 0x226FD0;

// Verified for the Gothic 3 1.75.20108.14 exports before installing this hook.
constexpr std::uint8_t kDrawFocusNamePrologue[] = {
    0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x81, 0xEC, 0x94, 0x00, 0x00, 0x00
};

// SDK layout: gCNPC_PS species value at +0x34; animation factory actor at +0x98.
constexpr std::uintptr_t kNpcSpeciesValueOffset = 0x34;
constexpr std::uintptr_t kAssembledActorOffset = 0x98;

static GetPropertySetFn s_getPropertySet = nullptr;
static DrawFocusNameFn s_originalDrawFocusName = nullptr;
static void* s_drawFocusNameTarget = nullptr;
static volatile LONG s_missingActorLogged = 0;
static bool s_hookCreated = false;
static bool s_hookEnabled = false;

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
        if (InterlockedCompareExchange(&s_missingActorLogged, 1, 0) == 0) {
            Log("G3O", "Focus-name guard skipped a Human/Orc label with no assembled animation actor.");
        }
        return false;
    }

    return s_originalDrawFocusName ? s_originalDrawFocusName(self, entity, alpha) : false;
}

} // namespace

void InitializeFocusNameGuard() {
    HMODULE game = GetModuleHandleA("Game.dll");
    HMODULE engine = GetModuleHandleA("Engine.dll");
    if (!game || !engine) {
        Log("G3O", "Focus-name guard not installed: Game.dll or Engine.dll is not loaded.");
        return;
    }

    void* drawFocusName = reinterpret_cast<void*>(GetProcAddress(
        game, "?DrawFocusName@gCGUIEntityRenderer@@IAE_NPBVeCEntity@@M@Z"));
    s_getPropertySet = reinterpret_cast<GetPropertySetFn>(GetProcAddress(
        engine, "?GetPropertySet@eCEntity@@UAEPAVeCEntityPropertySet@@W4eEPropertySetType@@@Z"));
    if (!drawFocusName || !s_getPropertySet) {
        s_getPropertySet = nullptr;
        Log("G3O", "Focus-name guard not installed: required Game.dll/Engine.dll exports were not found.");
        return;
    }

    const auto gameBase = reinterpret_cast<std::uintptr_t>(game);
    const auto engineBase = reinterpret_cast<std::uintptr_t>(engine);
    const auto drawRva = reinterpret_cast<std::uintptr_t>(drawFocusName) - gameBase;
    const auto propertySetRva = reinterpret_cast<std::uintptr_t>(s_getPropertySet) - engineBase;
    if (drawRva != kDrawFocusNameRva || propertySetRva != kGetPropertySetRva ||
        std::memcmp(drawFocusName, kDrawFocusNamePrologue, sizeof(kDrawFocusNamePrologue)) != 0) {
        s_getPropertySet = nullptr;
        Log("G3O", "Focus-name guard not installed: engine exports do not match the verified 1.75.20108.14 layout.");
        return;
    }

    s_drawFocusNameTarget = drawFocusName;
    MH_STATUS status = MH_CreateHook(
        s_drawFocusNameTarget,
        reinterpret_cast<LPVOID>(&HookedDrawFocusName),
        reinterpret_cast<LPVOID*>(&s_originalDrawFocusName));
    if (status != MH_OK) {
        s_drawFocusNameTarget = nullptr;
        s_getPropertySet = nullptr;
        s_originalDrawFocusName = nullptr;
        Log("G3O", "Focus-name guard hook creation failed (MinHook code %d).", static_cast<int>(status));
        return;
    }
    s_hookCreated = true;

    status = MH_EnableHook(s_drawFocusNameTarget);
    if (status != MH_OK) {
        const MH_STATUS removeStatus = MH_RemoveHook(s_drawFocusNameTarget);
        Log("G3O", "Focus-name guard enable failed (MinHook code %d; remove code %d).",
            static_cast<int>(status), static_cast<int>(removeStatus));
        if (removeStatus == MH_OK) {
            s_hookCreated = false;
            s_drawFocusNameTarget = nullptr;
            s_getPropertySet = nullptr;
            s_originalDrawFocusName = nullptr;
        }
        return;
    }

    s_hookEnabled = true;
    Log("G3O", "Focus-name null-actor guard installed for Gothic 3 1.75.20108.14.");
}

void ShutdownFocusNameGuard() {
    if (s_drawFocusNameTarget && s_hookEnabled) {
        const MH_STATUS status = MH_DisableHook(s_drawFocusNameTarget);
        if (status != MH_OK && status != MH_ERROR_DISABLED) {
            Log("G3O", "Focus-name guard disable failed (MinHook code %d).", static_cast<int>(status));
        }
    }
    if (s_drawFocusNameTarget && s_hookCreated) {
        const MH_STATUS status = MH_RemoveHook(s_drawFocusNameTarget);
        if (status != MH_OK) {
            Log("G3O", "Focus-name guard removal failed (MinHook code %d).", static_cast<int>(status));
        }
    }

    s_hookEnabled = false;
    s_hookCreated = false;
    s_drawFocusNameTarget = nullptr;
    s_getPropertySet = nullptr;
    s_originalDrawFocusName = nullptr;
}

} // namespace G3O
