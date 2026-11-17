#include "static_slab_arena.h"
#include "diagnostic_logger.h"
#include <intrin.h>

namespace G3O {

namespace {

inline int CountTrailingZeros64(uint64_t mask) noexcept {
    unsigned long index = 0;
    if (_BitScanForward(&index, static_cast<uint32_t>(mask))) {
        return static_cast<int>(index);
    }
    if (_BitScanForward(&index, static_cast<uint32_t>(mask >> 32))) {
        return static_cast<int>(index + 32);
    }
    return 64;
}

} // anonymous namespace

bool StaticBoundedSlabArena::Initialize() noexcept {
    if (m_basePtr) return true;

    // Allocate single contiguous 72 MB virtual address reservation + commit
    m_basePtr = static_cast<uint8_t*>(
        ::VirtualAlloc(nullptr, kTotalArenaBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)
    );
    if (!m_basePtr) {
        Log("G3O", "[StaticSlabArena] Failed to allocate 72 MB slab arena (error %lu).", ::GetLastError());
        return false;
    }

    m_smallBase  = m_basePtr;
    m_mediumBase = m_smallBase + (kSmallSlotSize * kSmallSlotCount);
    m_largeBase  = m_mediumBase + (kMediumSlotSize * kMediumSlotCount);

    ResetAll();
    Log("G3O", "[StaticSlabArena] Initialized 72 MB static slab arena (Small=128x64KB, Medium=64x512KB, Large=8x4MB).");
    return true;
}

void StaticBoundedSlabArena::Shutdown() noexcept {
    if (m_basePtr) {
        ::VirtualFree(m_basePtr, 0, MEM_RELEASE);
        m_basePtr = nullptr;
        m_smallBase = nullptr;
        m_mediumBase = nullptr;
        m_largeBase = nullptr;
    }
}

void StaticBoundedSlabArena::ResetAll() noexcept {
    m_smallFreeMask[0].store(~0ULL, std::memory_order_relaxed);
    m_smallFreeMask[1].store(~0ULL, std::memory_order_relaxed);
    m_mediumFreeMask.store(~0ULL, std::memory_order_relaxed);
    m_largeFreeMask.store(0xFFULL, std::memory_order_relaxed); // 8 slots = lowest 8 bits
}

SlabHandle StaticBoundedSlabArena::TryAcquire(uint32_t uncompressedSize) noexcept {
    if (!m_basePtr) return {};

    const SlabTier tier = ClassifySize(uncompressedSize);
    switch (tier) {
        case SlabTier::Small: {
            for (int word = 0; word < 2; ++word) {
                uint64_t mask = m_smallFreeMask[word].load(std::memory_order_relaxed);
                while (mask != 0) {
                    const int bit = CountTrailingZeros64(mask);
                    const uint64_t bitMask = (1ULL << bit);
                    if (m_smallFreeMask[word].compare_exchange_weak(
                            mask, mask & ~bitMask,
                            std::memory_order_acquire, std::memory_order_relaxed)) {
                        const uint8_t idx = static_cast<uint8_t>(word * 64 + bit);
                        return { SlabTier::Small, idx, m_smallBase + (idx * kSmallSlotSize) };
                    }
                }
            }
            break;
        }
        case SlabTier::Medium: {
            uint64_t mask = m_mediumFreeMask.load(std::memory_order_relaxed);
            while (mask != 0) {
                const int bit = CountTrailingZeros64(mask);
                const uint64_t bitMask = (1ULL << bit);
                if (m_mediumFreeMask.compare_exchange_weak(
                        mask, mask & ~bitMask,
                        std::memory_order_acquire, std::memory_order_relaxed)) {
                    const uint8_t idx = static_cast<uint8_t>(bit);
                    return { SlabTier::Medium, idx, m_mediumBase + (idx * kMediumSlotSize) };
                }
            }
            break;
        }
        case SlabTier::Large: {
            uint64_t mask = m_largeFreeMask.load(std::memory_order_relaxed);
            while (mask != 0) {
                const int bit = CountTrailingZeros64(mask);
                const uint64_t bitMask = (1ULL << bit);
                if (m_largeFreeMask.compare_exchange_weak(
                        mask, mask & ~bitMask,
                        std::memory_order_acquire, std::memory_order_relaxed)) {
                    const uint8_t idx = static_cast<uint8_t>(bit);
                    return { SlabTier::Large, idx, m_largeBase + (idx * kLargeSlotSize) };
                }
            }
            break;
        }
        default:
            break;
    }
    return {};
}

void StaticBoundedSlabArena::Release(const SlabHandle& handle) noexcept {
    if (!handle.ptr || handle.slotIndex == 0xFF) return;

    switch (handle.tier) {
        case SlabTier::Small: {
            const int word = handle.slotIndex / 64;
            const int bit  = handle.slotIndex % 64;
            m_smallFreeMask[word].fetch_or(1ULL << bit, std::memory_order_release);
            break;
        }
        case SlabTier::Medium: {
            m_mediumFreeMask.fetch_or(1ULL << handle.slotIndex, std::memory_order_release);
            break;
        }
        case SlabTier::Large: {
            m_largeFreeMask.fetch_or(1ULL << handle.slotIndex, std::memory_order_release);
            break;
        }
        default:
            break;
    }
}

} // namespace G3O
