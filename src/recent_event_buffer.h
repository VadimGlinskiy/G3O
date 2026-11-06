#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace G3O {

// Append is serialized by the normal logger. CopyTo is bounded, allocation-free,
// and never waits for a writer; an event being changed is simply skipped.
template <std::size_t EventCount, std::size_t EventBytes>
class RecentEventBuffer {
    static_assert(EventCount > 0, "RecentEventBuffer needs at least one slot");
    static_assert(EventBytes >= 3, "Each slot needs room for text, a cut marker, and NUL");
    static_assert(std::atomic<char>::is_always_lock_free,
                  "Crash-path event reads require lock-free byte atomics");
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
                  "Crash-path event reads require lock-free 32-bit atomics");

    struct Slot {
        std::atomic<std::uint32_t> revision;
        std::atomic<std::uint32_t> sequence;
        std::atomic<char> text[EventBytes];

        Slot() noexcept : revision(0), sequence(0), text{} {
            for (std::size_t i = 0; i < EventBytes; ++i) {
                text[i].store('\0', std::memory_order_relaxed);
            }
        }
    };

public:
    static constexpr std::size_t SlotCount = EventCount;
    static constexpr std::size_t SlotBytes = EventBytes;
    static constexpr std::size_t SnapshotCapacity = EventCount * EventBytes + 1;

    // The producer must be serialized by its caller; this function takes no lock
    // and stores no dynamically allocated data.
    void Append(const char* event, std::size_t length) noexcept {
        if (!event) {
            return;
        }

        const std::uint32_t sequence = m_latestSequence.load(std::memory_order_relaxed) + 1u;
        Slot& slot = m_slots[(sequence - 1u) % EventCount];

        std::uint32_t revision = slot.revision.load(std::memory_order_relaxed);
        if (revision & 1u) {
            ++revision;
        }
        const std::uint32_t writingRevision = revision + 1u;
        slot.revision.store(writingRevision, std::memory_order_seq_cst);

        const std::size_t maxLength = EventBytes - 1;
        const bool truncated = length > maxLength;
        const std::size_t copiedLength = truncated ? maxLength - 2 : length;
        for (std::size_t i = 0; i < copiedLength; ++i) {
            slot.text[i].store(event[i], std::memory_order_relaxed);
        }

        std::size_t storedLength = copiedLength;
        if (truncated) {
            slot.text[storedLength++].store('~', std::memory_order_relaxed);
            slot.text[storedLength++].store('\n', std::memory_order_relaxed);
        }
        slot.text[storedLength].store('\0', std::memory_order_relaxed);
        slot.sequence.store(sequence, std::memory_order_relaxed);
        slot.revision.store(writingRevision + 1u, std::memory_order_seq_cst);

        const std::uint32_t count = m_count.load(std::memory_order_relaxed);
        if (count < EventCount) {
            m_count.store(count + 1u, std::memory_order_relaxed);
        }
        m_latestSequence.store(sequence, std::memory_order_release);
    }

    // Copies complete events in chronological order. The destination is always
    // NUL-terminated when capacity is nonzero; no partial event is exposed.
    std::size_t CopyTo(char* destination, std::size_t capacity) const noexcept {
        if (!destination || capacity == 0) {
            return 0;
        }
        destination[0] = '\0';

        const std::uint32_t latest = m_latestSequence.load(std::memory_order_acquire);
        const std::uint32_t count = m_count.load(std::memory_order_relaxed);
        const std::size_t available = count < EventCount ? count : EventCount;
        std::size_t outputLength = 0;

        for (std::size_t age = available; age > 0; --age) {
            const std::uint32_t expectedSequence = latest - static_cast<std::uint32_t>(age - 1u);
            const std::size_t index = (expectedSequence - 1u) % EventCount;
            const Slot& slot = m_slots[index];

            const std::uint32_t before = slot.revision.load(std::memory_order_seq_cst);
            if (before & 1u) {
                continue;
            }
            if (slot.sequence.load(std::memory_order_relaxed) != expectedSequence) {
                continue;
            }

            std::size_t lineLength = 0;
            bool terminated = false;
            bool fits = true;
            for (; lineLength < EventBytes; ++lineLength) {
                const char value = slot.text[lineLength].load(std::memory_order_relaxed);
                if (value == '\0') {
                    terminated = true;
                    break;
                }
                if (outputLength + lineLength + 1u >= capacity) {
                    fits = false;
                    break;
                }
                destination[outputLength + lineLength] = value;
            }

            const std::uint32_t after = slot.revision.load(std::memory_order_seq_cst);
            if (before != after || (after & 1u)) {
                destination[outputLength] = '\0';
                continue;
            }
            if (!fits) {
                destination[outputLength] = '\0';
                break;
            }
            if (!terminated) {
                destination[outputLength] = '\0';
                continue;
            }

            outputLength += lineLength;
            destination[outputLength] = '\0';
        }

        return outputLength;
    }

private:
    Slot m_slots[EventCount];
    std::atomic<std::uint32_t> m_latestSequence{ 0 };
    std::atomic<std::uint32_t> m_count{ 0 };
};

} // namespace G3O
