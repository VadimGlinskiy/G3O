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
};

class PvsPrefetchPolicy {
public:
    static constexpr float kDefaultBootRadius       = 3800.0f;
    static constexpr float kDefaultRedundantDeltaSq = 200.0f * 200.0f; // 2 meters
    static constexpr uint32_t kDefaultRampUpCalls   = 24;

    static inline float DistanceSq(const GothicSphere& a, const GothicSphere& b) noexcept {
        const float dx = a.x - b.x;
        const float dy = a.y - b.y;
        const float dz = a.z - b.z;
        return dx * dx + dy * dy + dz * dz;
    }

    static inline PvsSyncAction EvaluateSync(
        PvsPolicyState& state,
        const GothicSphere& input,
        float bootRadius,
        float redundantDeltaSq,
        uint32_t rampUpCalls,
        GothicSphere& outSphere) noexcept
    {
        if (state.hadSyncCall) {
            const float distSq = DistanceSq(input, state.lastSyncSphere);
            if (distSq <= redundantDeltaSq) {
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
        GothicSphere& outSphere) noexcept
    {
        state.hadSyncCall = false;
        outSphere = input;

        if (state.rampUpRemaining > 0 && input.radius > bootRadius && totalRampCalls > 0) {
            --state.rampUpRemaining;
            const float progress = 1.0f - (static_cast<float>(state.rampUpRemaining) / static_cast<float>(totalRampCalls));
            outSphere.radius = bootRadius + (input.radius - bootRadius) * progress;
            return true; // ramped
        }

        return false; // normal
    }

    static inline void Reset(PvsPolicyState& state) noexcept {
        state.hadSyncCall = false;
        state.lastSyncSphere = {};
        state.rampUpRemaining = 0;
    }
};

} // namespace G3O
