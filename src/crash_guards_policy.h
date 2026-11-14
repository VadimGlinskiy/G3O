#pragma once

#include <cstdint>

namespace G3O {

namespace FocusNameGuardPolicy {

inline constexpr std::uint32_t kHumanSpecies = 0;
inline constexpr std::uint32_t kOrcSpecies = 5;

constexpr bool ShouldSkipFocusName(bool hasAnimationPropertySet,
                                   bool hasNpcPropertySet,
                                   std::uint32_t species,
                                   bool hasAssembledActor) noexcept {
    return hasAnimationPropertySet && hasNpcPropertySet && !hasAssembledActor &&
           (species == kHumanSpecies || species == kOrcSpecies);
}

} // namespace FocusNameGuardPolicy

namespace BoneMatrixGuardPolicy {

inline constexpr std::uintptr_t kLowPointerLimit = 0x10000;

constexpr bool IsPointerSafe(const void* ptr) noexcept {
    return ptr != nullptr && reinterpret_cast<std::uintptr_t>(ptr) >= kLowPointerLimit;
}

constexpr bool IsBoneMatrixSafe(const void* self,
                                const void* actor,
                                const void* skeleton) noexcept {
    return IsPointerSafe(self) && IsPointerSafe(actor) && IsPointerSafe(skeleton);
}

} // namespace BoneMatrixGuardPolicy

namespace InventoryGuardPolicy {

inline constexpr std::uintptr_t kLowPointerLimit = 0x10000;

constexpr bool IsPointerSafe(const void* ptr) noexcept {
    return ptr != nullptr && reinterpret_cast<std::uintptr_t>(ptr) >= kLowPointerLimit;
}

constexpr bool IsInventorySafe(const void* inventory) noexcept {
    return IsPointerSafe(inventory);
}

constexpr bool IsTemplateEntitySafe(const void* templateEntity) noexcept {
    return IsPointerSafe(templateEntity);
}

constexpr bool IsFindStackCallSafe(const void* inventory, const void* templateEntity) noexcept {
    return IsInventorySafe(inventory) && IsTemplateEntitySafe(templateEntity);
}

} // namespace InventoryGuardPolicy

} // namespace G3O
