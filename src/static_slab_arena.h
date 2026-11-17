#pragma once

#include <windows.h>
#include <cstdint>
#include <atomic>

namespace G3O {

enum class SlabTier : uint8_t {
    Small    = 0, // 128 slots x   64 KB =  8 MB (meshes .xshm, collision .xcom)
    Medium   = 1, //  64 slots x  512 KB = 32 MB (animations .xact, standard geometry)
    Large    = 2, //   8 slots x 4096 KB = 32 MB (high-res textures .ximg mipmaps)
    Oversize = 3  // > 4 MB (direct main-thread libdeflate, 0 bytes extra RAM)
};

struct SlabHandle {
    SlabTier tier      = SlabTier::Oversize;
    uint8_t  slotIndex = 0xFF;
    uint8_t* ptr       = nullptr;
};

class StaticBoundedSlabArena {
public:
    static constexpr size_t kSmallSlotSize   = 64 * 1024;        // 64 KB
    static constexpr size_t kMediumSlotSize  = 512 * 1024;       // 512 KB
    static constexpr size_t kLargeSlotSize   = 4 * 1024 * 1024;  // 4 MB

    static constexpr size_t kSmallSlotCount  = 128; // 8 MB (2 words x 64 bits)
    static constexpr size_t kMediumSlotCount = 64;  // 32 MB (1 word x 64 bits)
    static constexpr size_t kLargeSlotCount  = 8;   // 32 MB (1 word x 8 bits)

    static constexpr size_t kTotalArenaBytes =
        (kSmallSlotSize * kSmallSlotCount) +
        (kMediumSlotSize * kMediumSlotCount) +
        (kLargeSlotSize * kLargeSlotCount); // Exactly 72 MB

    StaticBoundedSlabArena() = default;
    ~StaticBoundedSlabArena() { Shutdown(); }

    bool Initialize() noexcept;
    void Shutdown() noexcept;
    void ResetAll() noexcept;

    [[nodiscard]] static constexpr SlabTier ClassifySize(uint32_t uncompressedSize) noexcept {
        if (uncompressedSize <= kSmallSlotSize)  return SlabTier::Small;
        if (uncompressedSize <= kMediumSlotSize) return SlabTier::Medium;
        if (uncompressedSize <= kLargeSlotSize)  return SlabTier::Large;
        return SlabTier::Oversize;
    }

    [[nodiscard]] SlabHandle TryAcquire(uint32_t uncompressedSize) noexcept;
    void Release(const SlabHandle& handle) noexcept;

    [[nodiscard]] bool IsInitialized() const noexcept { return m_basePtr != nullptr; }
    [[nodiscard]] size_t GetTotalArenaBytes() const noexcept { return kTotalArenaBytes; }

private:
    uint8_t* m_basePtr    = nullptr;
    uint8_t* m_smallBase  = nullptr;
    uint8_t* m_mediumBase = nullptr;
    uint8_t* m_largeBase  = nullptr;

    std::atomic<uint64_t> m_smallFreeMask[2]{ ~0ULL, ~0ULL };
    std::atomic<uint64_t> m_mediumFreeMask{ ~0ULL };
    std::atomic<uint64_t> m_largeFreeMask{ 0xFFULL };
};

} // namespace G3O
