#pragma once
#include <windows.h>
#include <cstdint>

namespace G3O {

struct Vector3 {
    float x;
    float y;
    float z;
};

struct Box3 {
    Vector3 min;
    Vector3 max;
};
static_assert(sizeof(Box3) == 0x18, "Gothic 3 bCBox layout must be 24 bytes.");

struct Sphere3 {
    float radius;
    float x;
    float y;
    float z;
};
static_assert(sizeof(Sphere3) == 0x10, "Gothic 3 bCSphere layout must be 16 bytes.");

inline bool FastBoxIntersects(const Box3* pThis, const Box3* pOther) noexcept {
    if (!pThis || !pOther) return false;
    if (pThis->min.x > pOther->max.x || pThis->max.x < pOther->min.x) return false;
    if (pThis->min.y > pOther->max.y || pThis->max.y < pOther->min.y) return false;
    if (pThis->min.z > pOther->max.z || pThis->max.z < pOther->min.z) return false;
    return true;
}

inline bool FastSphereIntersects(const Sphere3* pThis, const Sphere3* pOther) noexcept {
    if (!pThis || !pOther) return false;
    const float dx = pOther->x - pThis->x;
    const float dy = pOther->y - pThis->y;
    const float dz = pOther->z - pThis->z;
    const float distSq = dx * dx + dy * dy + dz * dz;
    const float rSum = pThis->radius + pOther->radius;
    return (rSum * rSum) > distSq;
}

void InitializeSharedBaseOptimizations();
void ShutdownSharedBaseOptimizations();

bool TryFastCStringGetWord(
    void* self, std::int32_t wordNumber, const void* delimiters,
    void* destination, bool trimLeft, bool trimRight,
    std::int32_t& outOffset) noexcept;

} // namespace G3O
