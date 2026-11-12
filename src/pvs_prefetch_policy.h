#pragma once
#include <cstdint>
#include <cmath>

namespace G3O {

#pragma pack(push, 4)
struct GothicSphere {
    float radius; // +0x00
    float x;      // +0x04
    float y;      // +0x08
    float z;      // +0x0C
};
#pragma pack(pop)
static_assert(sizeof(GothicSphere) == 16, "sizeof(GothicSphere) must be 16 bytes");

enum class PvsSyncAction {
    ExecuteNormal,
    ExecuteClamped,
    SkipRedundant
};

struct PvsPolicyState {
    bool         hadSyncCall = false;
    GothicSphere lastSyncSphere = {};
    uint32_t     rampUpRemaining = 0;
    float        effectiveRoiRadius = 0.0f;
    float        targetRoiRadius = 0.0f;
};

class PvsPrefetchPolicy {
public:
    static constexpr float kDefaultBootRadius       = 3800.0f;
    static constexpr float kDefaultRedundantDeltaSq = 200.0f * 200.0f; // 2 meters
    static constexpr uint32_t kDefaultRampUpCalls   = 24;
    static constexpr float kDefaultBootRoiRadius    = 3600.0f;

    static inline float DistanceSq(const GothicSphere& a, const GothicSphere& b) noexcept {
        const float dx = a.x - b.x;
        const float dy = a.y - b.y;
        const float dz = a.z - b.z;
        return dx * dx + dy * dy + dz * dz;
    }

    static inline float CalculateEffectiveRoi(
        uint32_t rampUpRemaining,
        uint32_t totalRampCalls,
        float bootRoiRadius,
        float targetRoiRadius) noexcept
    {
        if (targetRoiRadius <= 0.0f) {
            return 0.0f;
        }
        if (totalRampCalls == 0 || rampUpRemaining == 0) {
            return targetRoiRadius;
        }
        if (rampUpRemaining >= totalRampCalls) {
            return bootRoiRadius;
        }
        const float progress = 1.0f - (static_cast<float>(rampUpRemaining) / static_cast<float>(totalRampCalls));
        return bootRoiRadius + (targetRoiRadius - bootRoiRadius) * progress;
    }

    static inline PvsSyncAction EvaluateSync(
        PvsPolicyState& state,
        const GothicSphere& input,
        float bootRadius,
        float redundantDeltaSq,
        uint32_t rampUpCalls,
        GothicSphere& outSphere,
        float bootRoiRadius = kDefaultBootRoiRadius,
        float targetRoiRadius = 0.0f) noexcept
    {
        if (targetRoiRadius > 0.0f) {
            state.targetRoiRadius = targetRoiRadius;
        }
        state.effectiveRoiRadius = state.targetRoiRadius > 0.0f ? bootRoiRadius : 0.0f;

        if (state.hadSyncCall) {
            const float distSq = DistanceSq(input, state.lastSyncSphere);
            const float radiusDelta = input.radius - state.lastSyncSphere.radius;
            if (distSq <= redundantDeltaSq && radiusDelta * radiusDelta <= redundantDeltaSq) {
                return PvsSyncAction::SkipRedundant;
            }
        }

        state.hadSyncCall = true;
        state.lastSyncSphere = input;
        state.rampUpRemaining = rampUpCalls;

        outSphere = input;
        if (input.radius > bootRadius) {
            outSphere.radius = bootRadius;
            return PvsSyncAction::ExecuteClamped;
        }

        return PvsSyncAction::ExecuteNormal;
    }

    static inline bool EvaluateAsync(
        PvsPolicyState& state,
        const GothicSphere& input,
        float bootRadius,
        uint32_t totalRampCalls,
        GothicSphere& outSphere,
        float bootRoiRadius = kDefaultBootRoiRadius) noexcept
    {
        state.hadSyncCall = false;
        outSphere = input;

        if (state.rampUpRemaining > 0 && totalRampCalls > 0) {
            --state.rampUpRemaining;
            const float progress = 1.0f - (static_cast<float>(state.rampUpRemaining) / static_cast<float>(totalRampCalls));
            if (input.radius > bootRadius) {
                outSphere.radius = bootRadius + (input.radius - bootRadius) * progress;
            }
            state.effectiveRoiRadius = CalculateEffectiveRoi(
                state.rampUpRemaining, totalRampCalls, bootRoiRadius, state.targetRoiRadius);
            return true; // ramped
        }

        state.effectiveRoiRadius = state.targetRoiRadius > 0.0f ? state.targetRoiRadius : 0.0f;
        return false; // normal
    }

    static inline void Reset(PvsPolicyState& state) noexcept {
        state.hadSyncCall = false;
        state.lastSyncSphere = {};
        state.rampUpRemaining = 0;
        state.effectiveRoiRadius = state.targetRoiRadius;
    }
};

} // namespace G3O
