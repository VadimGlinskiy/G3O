#pragma once

#include <cstdint>

namespace G3O {

// PropertySet Bitmask definitions for eCEntity (offsets 0x194 - 0x1A0)
// Word 0 (offset 0x194): PropertySet types 0 - 31
//   - Bit 15: eCDynamicLight_PS (0x0F)
//   - Bit 16: eCDirectionalLight_PS (0x10)
//   - Bit 17: eCPointLight_PS (0x11)
//   - Bit 30: gCNPC_PS (0x1E)
constexpr std::uint32_t IMPORTANT_WORD0_MASK = (1u << 15) | (1u << 16) | (1u << 17) | (1u << 30); // 0x40038000

// Word 1 (offset 0x198): PropertySet types 32 - 63
//   - Bit 14 (46 - 32): eCSpeedTree_PS (0x2E)
//   - Bit 16 (48 - 32): gCItem_PS (0x30) - All weapons, plants, herbs, potions, loot
//   - Bit 17 (49 - 32): gCInteraction_PS (0x31) - Chests, shrines, beds, doors, anvils
//   - Bit 27 (59 - 32): gCFocus_PS (0x3B) - Targetable interactive objects
constexpr std::uint32_t IMPORTANT_WORD1_MASK = (1u << 14) | (1u << 16) | (1u << 17) | (1u << 27); // 0x08034000

// Word 2 (offset 0x19C): PropertySet types 64 - 95
//   - Bit 4  (68 - 64): gCDialog_PS (0x44)
//   - Bit 5  (69 - 64): gCLock_PS (0x45) - Locked chests & doors
//   - Bit 6  (70 - 64): gCDoor_PS (0x46) - Doors & gates
//   - Bit 7  (71 - 64): eCStaticLight_PS (0x47)
//   - Bit 8  (72 - 64): eCStaticPointLight_PS (0x48)
//   - Bit 9  (73 - 64): eCStaticSpotLight_PS (0x49)
//   - Bit 23 (87 - 64): eCParticle_PS (0x57) - Torches, flames, campfires, magic glows
constexpr std::uint32_t IMPORTANT_WORD2_MASK = (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7) |
                                               (1u << 8) | (1u << 9) | (1u << 23); // 0x008003F0

// Word 3 (offset 0x1A0): PropertySet types 96 - 127
//   - Bit 4  (100 - 96): eCVisualAnimation_PS (0x64) - Windmills, banners, mechanisms
//   - Bit 7  (103 - 96): gCMover_PS (0x67) - Levers, winches, portcullises
//   - Bit 8  (104 - 96): gCTeleporter_PS (0x68) - Portals, stone circles
//   - Bit 12 (108 - 96): gCTreasureSet_PS (0x6C) - Treasure mounds & containers
constexpr std::uint32_t IMPORTANT_WORD3_MASK = (1u << 4) | (1u << 7) | (1u << 8) | (1u << 12); // 0x00001190

enum class EntityCullCategory {
    Important, // Category A: interactive items, herbs, chests, lights, actors, trees
    Clutter    // Category B: non-interactive static mesh clutter (plates, cups, small rocks, debris)
};

inline EntityCullCategory ClassifyEntityByBitmasks(const std::uint32_t* pBitmasks) noexcept {
    if (!pBitmasks) {
        return EntityCullCategory::Clutter;
    }

    // Word 1 checked first: in open world, items and interactions are most frequent among dynamic entities
    if (pBitmasks[1] & IMPORTANT_WORD1_MASK) return EntityCullCategory::Important;
    if (pBitmasks[0] & IMPORTANT_WORD0_MASK) return EntityCullCategory::Important;
    if (pBitmasks[2] & IMPORTANT_WORD2_MASK) return EntityCullCategory::Important;
    if (pBitmasks[3] & IMPORTANT_WORD3_MASK) return EntityCullCategory::Important;

    return EntityCullCategory::Clutter;
}

inline EntityCullCategory ClassifyEntity(const void* pEntity) noexcept {
    if (!pEntity) {
        return EntityCullCategory::Clutter;
    }
    const auto* pBitmasks = reinterpret_cast<const std::uint32_t*>(
        reinterpret_cast<const char*>(pEntity) + 0x194
    );
    return ClassifyEntityByBitmasks(pBitmasks);
}

inline float CalculateAdjustedAreaFactor(
    float fAreaFactor,
    EntityCullCategory category,
    float baseThreshold,
    float importantThreshold,
    float clutterThreshold,
    bool enableSelectiveCulling
) noexcept {
    if (!enableSelectiveCulling || fAreaFactor <= 0.0f || baseThreshold <= 1e-6f) {
        return fAreaFactor;
    }

    const float targetThreshold = (category == EntityCullCategory::Important)
        ? importantThreshold
        : clutterThreshold;

    if (targetThreshold <= 1e-6f) {
        return fAreaFactor;
    }

    // Scaling the area factor by (base / target) is mathematically identical to
    // evaluating the engine's culling formula against targetThreshold without modifying
    // engine state or memory at eCFrustumFilter + 0x34.
    const float scale = baseThreshold / targetThreshold;
    return fAreaFactor * scale;
}

} // namespace G3O
