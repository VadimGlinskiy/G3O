#pragma once

#include <cstdint>

namespace G3O::FocusNameGuardPolicy {

inline constexpr std::uint32_t kHumanSpecies = 0;
inline constexpr std::uint32_t kOrcSpecies = 5;

constexpr bool ShouldSkipFocusName(bool hasAnimationPropertySet,
                                   bool hasNpcPropertySet,
                                   std::uint32_t species,
                                   bool hasAssembledActor) noexcept {
    return hasAnimationPropertySet && hasNpcPropertySet && !hasAssembledActor &&
           (species == kHumanSpecies || species == kOrcSpecies);
}

} // namespace G3O::FocusNameGuardPolicy
