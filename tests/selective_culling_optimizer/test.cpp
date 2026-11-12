#include "selective_culling_policy.h"
#include <iostream>
#include <cmath>
#include <vector>
#include <cstring>

static bool Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        return false;
    }
    return true;
}

int main() {
    bool ok = true;
    std::cout << "[Test] Selective Culling Optimizer Policy Suite starting...\n";

    // 1. Classification Tests
    {
        // 1.1 Nullptr entity
        ok &= Expect(G3O::ClassifyEntity(nullptr) == G3O::EntityCullCategory::Clutter,
                     "nullptr entity should classify as Clutter");

        // 1.2 Dummy entity buffer with 0x1B4 bytes (mimicking eCEntity)
        std::vector<std::uint8_t> dummyEntity(0x1B4, 0);
        auto* pBitmasks = reinterpret_cast<std::uint32_t*>(dummyEntity.data() + 0x194);

        // All masks 0 -> Clutter
        ok &= Expect(G3O::ClassifyEntity(dummyEntity.data()) == G3O::EntityCullCategory::Clutter,
                     "Empty entity with 0 property sets should classify as Clutter");

        // 1.3 Word 0: Lights and NPCs
        pBitmasks[0] = 1u << 15; // eCDynamicLight_PS
        ok &= Expect(G3O::ClassifyEntity(dummyEntity.data()) == G3O::EntityCullCategory::Important,
                     "eCDynamicLight_PS should classify as Important");

        pBitmasks[0] = 1u << 16; // eCDirectionalLight_PS
        ok &= Expect(G3O::ClassifyEntity(dummyEntity.data()) == G3O::EntityCullCategory::Important,
                     "eCDirectionalLight_PS should classify as Important");

        pBitmasks[0] = 1u << 17; // eCPointLight_PS
        ok &= Expect(G3O::ClassifyEntity(dummyEntity.data()) == G3O::EntityCullCategory::Important,
                     "eCPointLight_PS should classify as Important");

        pBitmasks[0] = 1u << 30; // gCNPC_PS
        ok &= Expect(G3O::ClassifyEntity(dummyEntity.data()) == G3O::EntityCullCategory::Important,
                     "gCNPC_PS should classify as Important");
        pBitmasks[0] = 0;

        // 1.4 Word 1: Items, interactions, trees
        pBitmasks[1] = 1u << 14; // eCSpeedTree_PS
        ok &= Expect(G3O::ClassifyEntity(dummyEntity.data()) == G3O::EntityCullCategory::Important,
                     "eCSpeedTree_PS should classify as Important");

        pBitmasks[1] = 1u << 16; // gCItem_PS (herbs, weapons, loot)
        ok &= Expect(G3O::ClassifyEntity(dummyEntity.data()) == G3O::EntityCullCategory::Important,
                     "gCItem_PS (herbs/loot) should classify as Important");

        pBitmasks[1] = 1u << 17; // gCInteraction_PS (chests, shrines, doors)
        ok &= Expect(G3O::ClassifyEntity(dummyEntity.data()) == G3O::EntityCullCategory::Important,
                     "gCInteraction_PS should classify as Important");

        pBitmasks[1] = 1u << 27; // gCFocus_PS
        ok &= Expect(G3O::ClassifyEntity(dummyEntity.data()) == G3O::EntityCullCategory::Important,
                     "gCFocus_PS should classify as Important");
        pBitmasks[1] = 0;

        // 1.5 Word 2: Static lights, locks, doors, particles
        pBitmasks[2] = 1u << 4; // gCDialog_PS
        ok &= Expect(G3O::ClassifyEntity(dummyEntity.data()) == G3O::EntityCullCategory::Important,
                     "gCDialog_PS should classify as Important");

        pBitmasks[2] = 1u << 5; // gCLock_PS
        ok &= Expect(G3O::ClassifyEntity(dummyEntity.data()) == G3O::EntityCullCategory::Important,
                     "gCLock_PS should classify as Important");

        pBitmasks[2] = 1u << 6; // gCDoor_PS
        ok &= Expect(G3O::ClassifyEntity(dummyEntity.data()) == G3O::EntityCullCategory::Important,
                     "gCDoor_PS should classify as Important");

        pBitmasks[2] = 1u << 7; // eCStaticLight_PS
        ok &= Expect(G3O::ClassifyEntity(dummyEntity.data()) == G3O::EntityCullCategory::Important,
                     "eCStaticLight_PS should classify as Important");

        pBitmasks[2] = 1u << 23; // eCParticle_PS (torches, flames)
        ok &= Expect(G3O::ClassifyEntity(dummyEntity.data()) == G3O::EntityCullCategory::Important,
                     "eCParticle_PS should classify as Important");
        pBitmasks[2] = 0;

        // 1.6 Word 3: Movers, teleporters, treasure sets
        pBitmasks[3] = 1u << 4; // eCVisualAnimation_PS
        ok &= Expect(G3O::ClassifyEntity(dummyEntity.data()) == G3O::EntityCullCategory::Important,
                     "eCVisualAnimation_PS should classify as Important");

        pBitmasks[3] = 1u << 7; // gCMover_PS (levers, gates)
        ok &= Expect(G3O::ClassifyEntity(dummyEntity.data()) == G3O::EntityCullCategory::Important,
                     "gCMover_PS should classify as Important");

        pBitmasks[3] = 1u << 8; // gCTeleporter_PS
        ok &= Expect(G3O::ClassifyEntity(dummyEntity.data()) == G3O::EntityCullCategory::Important,
                     "gCTeleporter_PS should classify as Important");

        pBitmasks[3] = 1u << 12; // gCTreasureSet_PS
        ok &= Expect(G3O::ClassifyEntity(dummyEntity.data()) == G3O::EntityCullCategory::Important,
                     "gCTreasureSet_PS should classify as Important");
        pBitmasks[3] = 0;

        // 1.7 Non-important property sets (e.g. gCArena_PS = 0x3F = 63, which is bit 31 in Word 1)
        pBitmasks[1] = 1u << 31; // gCArena_PS
        ok &= Expect(G3O::ClassifyEntity(dummyEntity.data()) == G3O::EntityCullCategory::Clutter,
                     "gCArena_PS without items/interaction should classify as Clutter");
        pBitmasks[1] = 0;
    }

    // 2. Area Factor Adjustment & Mathematical Invariant Tests
    {
        const float baseThreshold = 0.002f;
        const float importantCutoff = 0.002f;
        const float clutterCutoff = 0.008f;

        // 2.1 Important item with base 0.002 and important 0.002 -> scale = 1.0
        {
            float area = 0.003f;
            float adjusted = G3O::CalculateAdjustedAreaFactor(
                area, G3O::EntityCullCategory::Important, baseThreshold, importantCutoff, clutterCutoff, true
            );
            ok &= Expect(std::abs(adjusted - 0.003f) < 1e-6f,
                         "Important item with equal threshold should have scale 1.0");
        }

        // 2.2 Clutter item with base 0.002 and clutter 0.008 -> scale = 0.002 / 0.008 = 0.25
        {
            float area = 0.004f;
            float adjusted = G3O::CalculateAdjustedAreaFactor(
                area, G3O::EntityCullCategory::Clutter, baseThreshold, importantCutoff, clutterCutoff, true
            );
            ok &= Expect(std::abs(adjusted - 0.001f) < 1e-6f,
                         "Clutter item should be scaled down by 0.25 (0.004 * 0.25 = 0.001)");
        }

        // 2.3 Base threshold = 0.004 (e.g. ge3.ini preset)
        {
            const float base004 = 0.004f;
            // Important: scale = 0.004 / 0.002 = 2.0 (boosted area factor keeps it visible)
            float adjustedImp = G3O::CalculateAdjustedAreaFactor(
                0.002f, G3O::EntityCullCategory::Important, base004, importantCutoff, clutterCutoff, true
            );
            ok &= Expect(std::abs(adjustedImp - 0.004f) < 1e-6f,
                         "Important item with base 0.004 should have scale 2.0");

            // Clutter: scale = 0.004 / 0.008 = 0.5 (culled earlier)
            float adjustedClutter = G3O::CalculateAdjustedAreaFactor(
                0.004f, G3O::EntityCullCategory::Clutter, base004, importantCutoff, clutterCutoff, true
            );
            ok &= Expect(std::abs(adjustedClutter - 0.002f) < 1e-6f,
                         "Clutter item with base 0.004 should have scale 0.5");
        }

        // 2.4 Disabled selective culling -> area factor unchanged
        {
            float area = 0.005f;
            float adjusted = G3O::CalculateAdjustedAreaFactor(
                area, G3O::EntityCullCategory::Clutter, baseThreshold, importantCutoff, clutterCutoff, false
            );
            ok &= Expect(std::abs(adjusted - area) < 1e-6f,
                         "Disabled selective culling should return area unmodified");
        }

        // 2.5 Zero/negative area factor -> unchanged
        {
            float adjusted = G3O::CalculateAdjustedAreaFactor(
                0.0f, G3O::EntityCullCategory::Clutter, baseThreshold, importantCutoff, clutterCutoff, true
            );
            ok &= Expect(adjusted == 0.0f, "0 area should return 0");

            adjusted = G3O::CalculateAdjustedAreaFactor(
                -0.5f, G3O::EntityCullCategory::Clutter, baseThreshold, importantCutoff, clutterCutoff, true
            );
            ok &= Expect(adjusted == -0.5f, "negative area should return negative unmodified");
        }

        // 2.6 Zero thresholds boundary defense
        {
            float adjusted = G3O::CalculateAdjustedAreaFactor(
                0.005f, G3O::EntityCullCategory::Clutter, 0.0f, importantCutoff, clutterCutoff, true
            );
            ok &= Expect(std::abs(adjusted - 0.005f) < 1e-6f,
                         "0 base threshold should return area unmodified");

            adjusted = G3O::CalculateAdjustedAreaFactor(
                0.005f, G3O::EntityCullCategory::Clutter, baseThreshold, importantCutoff, 0.0f, true
            );
            ok &= Expect(std::abs(adjusted - 0.005f) < 1e-6f,
                         "0 target threshold should return area unmodified");
        }
    }

    if (ok) {
        std::cout << "[Test] Selective Culling Optimizer Policy Suite PASSED!\n";
        return 0;
    } else {
        std::cerr << "[Test] Selective Culling Optimizer Policy Suite FAILED!\n";
        return 1;
    }
}
