#include "fog_policy.h"
#include <iostream>
#include <cmath>
#include <cstddef>

static bool Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        return false;
    }
    return true;
}

static bool ApproxEqual(float a, float b, float epsilon = 0.01f) {
    return std::fabs(a - b) <= epsilon;
}

int main() {
    bool ok = true;
    std::cout << "[Test] Fog Optimizer Policy Suite starting...\n";

    // 1. Binary Struct Layout Verification
    {
        ok &= Expect(sizeof(G3O::GfxFogData) == 28, "sizeof(GfxFogData) must be exactly 28 bytes");
        ok &= Expect(offsetof(G3O::GfxFogData, bEnable) == 0, "bEnable offset must be 0");
        ok &= Expect(offsetof(G3O::GfxFogData, subMode) == 1, "subMode offset must be 1");
        ok &= Expect(offsetof(G3O::GfxFogData, color) == 4, "color offset must be 4");
        ok &= Expect(offsetof(G3O::GfxFogData, type) == 8, "type offset must be 8");
        ok &= Expect(offsetof(G3O::GfxFogData, mode) == 12, "mode offset must be 12");
        ok &= Expect(offsetof(G3O::GfxFogData, fStart) == 16, "fStart offset must be 16");
        ok &= Expect(offsetof(G3O::GfxFogData, fEnd) == 20, "fEnd offset must be 20");
        ok &= Expect(offsetof(G3O::GfxFogData, fDensity) == 24, "fDensity offset must be 24");
    }

    // 2. IsUnderwaterOrCloseRangeFog Classification
    {
        G3O::GfxFogData fog{};
        fog.bEnable = 1;
        fog.fEnd = 1500.0f; // 15m (underwater / cave gas)
        ok &= Expect(G3O::IsUnderwaterOrCloseRangeFog(fog), "fEnd = 1500.0f should be classified as underwater/close-range");

        fog.fEnd = 2999.0f;
        ok &= Expect(G3O::IsUnderwaterOrCloseRangeFog(fog), "fEnd = 2999.0f should be classified as underwater/close-range");

        fog.fEnd = 3000.0f;
        ok &= Expect(!G3O::IsUnderwaterOrCloseRangeFog(fog), "fEnd = 3000.0f should not be classified as underwater/close-range");

        fog.fEnd = 50000.0f;
        ok &= Expect(!G3O::IsUnderwaterOrCloseRangeFog(fog), "fEnd = 50000.0f should not be classified as underwater/close-range");

        fog.fEnd = 0.0f;
        ok &= Expect(!G3O::IsUnderwaterOrCloseRangeFog(fog), "fEnd = 0.0f should not be classified as underwater/close-range");

        fog.fEnd = -100.0f;
        ok &= Expect(!G3O::IsUnderwaterOrCloseRangeFog(fog), "fEnd < 0.0f should not be classified as underwater/close-range");
    }

    // 3. Fog Expansion - Disabled / Invalid Handling
    {
        G3O::GfxFogData disabledFog{};
        disabledFog.bEnable = 0;
        disabledFog.fStart = 1000.0f;
        disabledFog.fEnd = 10000.0f;
        G3O::ExpandAtmosphericFog(disabledFog, 2.5f, 250000.0f, true);
        ok &= Expect(disabledFog.fStart == 1000.0f && disabledFog.fEnd == 10000.0f,
                     "Disabled fog (bEnable = 0) must remain completely untouched");

        G3O::GfxFogData invalidFog{};
        invalidFog.bEnable = 1;
        invalidFog.fStart = 0.0f;
        invalidFog.fEnd = 0.0f;
        G3O::ExpandAtmosphericFog(invalidFog, 2.5f, 250000.0f, true);
        ok &= Expect(invalidFog.fEnd == 0.0f, "Invalid fog range (fEnd <= 0) must remain untouched");
    }

    // 4. Fog Expansion - Underwater Preservation
    {
        G3O::GfxFogData underwater{};
        underwater.bEnable = 1;
        underwater.fStart = 100.0f;
        underwater.fEnd = 1200.0f; // 12 meters
        G3O::ExpandAtmosphericFog(underwater, 2.5f, 250000.0f, true);
        ok &= Expect(underwater.fStart == 100.0f && underwater.fEnd == 1200.0f,
                     "Underwater fog with preserveUnderwater=true must remain untouched");

        // When preservation is disabled, it should scale
        G3O::ExpandAtmosphericFog(underwater, 2.0f, 250000.0f, false);
        ok &= Expect(ApproxEqual(underwater.fEnd, 2400.0f),
                     "Underwater fog with preserveUnderwater=false should scale by multiplier");
    }

    // 5. Fog Expansion - Dense Storm / Blizzard Weather
    {
        // Heavy rain/storm in Gothic 3 has fEnd around 20,000 (200m)
        G3O::GfxFogData storm{};
        storm.bEnable = 1;
        storm.fStart = 4000.0f;  // 40m
        storm.fEnd = 20000.0f;   // 200m
        G3O::ExpandAtmosphericFog(storm, 2.5f, 250000.0f, true);

        // Since fEnd (20000) < 35000, minimum horizon clamp must NOT apply (preserving storm feel)
        ok &= Expect(ApproxEqual(storm.fEnd, 50000.0f),
                     "Storm fog should scale by multiplier (20000 * 2.5 = 50000) without horizon clamp");
        ok &= Expect(ApproxEqual(storm.fStart, 10000.0f),
                     "Storm fog gradient ratio (4000/20000 = 0.2) should be preserved (50000 * 0.2 = 10000)");
    }

    // 6. Fog Expansion - Clear Weather Horizon Vista
    {
        // Vanilla clear weather: fStart = 12000 (120m), fEnd = 60000 (600m)
        G3O::GfxFogData clearWeather{};
        clearWeather.bEnable = 1;
        clearWeather.fStart = 12000.0f;
        clearWeather.fEnd = 60000.0f;
        G3O::ExpandAtmosphericFog(clearWeather, 2.5f, 250000.0f, true);

        // 60000 * 2.5 = 150000, which is < 250000, so clamp to 250000.0f
        ok &= Expect(ApproxEqual(clearWeather.fEnd, 250000.0f),
                     "Clear weather fogEnd should clamp to minEnd 250000.0f");
        // Ratio = 12000 / 60000 = 0.2 -> 250000 * 0.2 = 50000.0f
        ok &= Expect(ApproxEqual(clearWeather.fStart, 50000.0f),
                     "Clear weather fogStart should scale proportionally to 50000.0f");
    }

    // 7. Fog Expansion - Ultra High Vanilla Vista
    {
        // Modded high weather: fStart = 30000 (300m), fEnd = 120000 (1.2 km)
        G3O::GfxFogData highWeather{};
        highWeather.bEnable = 1;
        highWeather.fStart = 30000.0f;
        highWeather.fEnd = 120000.0f;
        G3O::ExpandAtmosphericFog(highWeather, 2.5f, 250000.0f, true);

        // 120000 * 2.5 = 300000 >= 250000 -> 300000.0f (3.0 km)
        ok &= Expect(ApproxEqual(highWeather.fEnd, 300000.0f),
                     "High weather fogEnd should multiply to 300000.0f (3.0 km)");
        // Ratio = 30000 / 120000 = 0.25 -> 300000 * 0.25 = 75000.0f
        ok &= Expect(ApproxEqual(highWeather.fStart, 75000.0f),
                     "High weather fogStart should scale proportionally to 75000.0f");
    }

    // 8. Sanity Bounds and Edge Cases
    {
        // Negative fStart
        G3O::GfxFogData negStart{};
        negStart.bEnable = 1;
        negStart.fStart = -500.0f;
        negStart.fEnd = 50000.0f;
        G3O::ExpandAtmosphericFog(negStart, 2.0f, 250000.0f, true);
        ok &= Expect(ApproxEqual(negStart.fStart, 0.0f),
                     "Negative fStart should be clamped to 0.0f");

        // Inverted fStart > fEnd
        G3O::GfxFogData inverted{};
        inverted.bEnable = 1;
        inverted.fStart = 80000.0f;
        inverted.fEnd = 50000.0f;
        G3O::ExpandAtmosphericFog(inverted, 2.0f, 250000.0f, true);
        // Ratio > 0.9 should clamp to 0.9
        ok &= Expect(ApproxEqual(inverted.fStart, inverted.fEnd * 0.9f),
                     "Inverted fStart > fEnd ratio should clamp to 0.9 maximum");

        // Negative multiplier should fall back safely to 1.0
        G3O::GfxFogData fallbackMult{};
        fallbackMult.bEnable = 1;
        fallbackMult.fStart = 10000.0f;
        fallbackMult.fEnd = 40000.0f;
        G3O::ExpandAtmosphericFog(fallbackMult, -3.0f, 0.0f, true);
        ok &= Expect(ApproxEqual(fallbackMult.fEnd, 40000.0f),
                     "Negative distance multiplier should safely fall back to 1.0x");
    }

    if (ok) {
        std::cout << "[Test] All Fog Optimizer Policy Suite tests PASSED!\n";
        return 0;
    } else {
        std::cerr << "[Test] Some tests FAILED!\n";
        return 1;
    }
}
