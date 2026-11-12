#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace G3O::SceneNodePolicy {

inline bool ShouldPreallocate(std::uint16_t count, std::uint16_t capacity) noexcept {
    return count >= 64 && count == capacity && capacity < 32768;
}

inline std::uint16_t NextCapacity(std::uint16_t capacity) noexcept {
    if (capacity >= 8192) return 32768;
    if (capacity >= 1024) return 8192;
    return 1024;
}

template <typename Realloc>
inline bool TryPreallocate(
    void* memoryAdmin,
    void**& children,
    std::uint16_t& capacity,
    std::uint16_t newCapacity,
    Realloc realloc) {
    if (newCapacity <= capacity || (capacity > 0 && !children)) return false;

    const std::uint16_t oldCapacity = capacity;
    const std::uint32_t newSize = static_cast<std::uint32_t>(newCapacity) * sizeof(void*);
    void* const resized = realloc(memoryAdmin, children, newSize);
    if (!resized) return false;

    auto** const newChildren = static_cast<void**>(resized);
    std::memset(
        newChildren + oldCapacity,
        0,
        static_cast<std::size_t>(newCapacity - oldCapacity) * sizeof(void*));
    children = newChildren;
    capacity = newCapacity;
    return true;
}

} // namespace G3O::SceneNodePolicy
