#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

namespace G3O {

// Bounded per-thread correlation between completed PAK reads and decompressions.
// Exact buffer/length matching avoids pointer arithmetic on untrusted buffers.
template <typename Entry, std::size_t Capacity>
class RecentReadHistory {
    static_assert(Capacity > 0, "read history must have at least one slot");

public:
    void Push(Entry&& entry) {
        if (entry.buffer) {
            // Reusing a destination buffer overwrites its prior contents; older
            // entries for that address must never be used as a fallback match.
            for (Entry& existing : entries_) {
                if (existing.valid && existing.buffer == entry.buffer) {
                    existing = Entry{};
                }
            }
        }

        entries_[head_] = std::move(entry);
        head_ = (head_ + 1) % Capacity;
        if (count_ < Capacity) ++count_;
    }

    Entry* FindMostRecentExact(
        const void* buffer,
        std::uint32_t bytesRead,
        std::uint64_t loadGeneration) noexcept {
        if (!buffer || bytesRead == 0) return nullptr;

        for (std::size_t step = 0; step < count_; ++step) {
            const std::size_t index = (head_ + Capacity - 1 - step) % Capacity;
            Entry& entry = entries_[index];
            if (entry.valid && entry.buffer == buffer &&
                entry.bytesRead == bytesRead &&
                entry.loadGeneration == loadGeneration) {
                return &entry;
            }
        }
        return nullptr;
    }

    // Matches if the requested range [buffer, buffer + bytesNeeded) is contained within
    // the read buffer [entry.buffer, entry.buffer + entry.bytesRead).
    // Calculates the exact file offset in the archive for offset/sub-buffer reads.
    Entry* FindMostRecentContaining(
        const void* buffer,
        std::uint32_t bytesNeeded,
        std::uint64_t loadGeneration,
        std::uint64_t* outOffsetInArchive = nullptr) noexcept {
        if (!buffer || bytesNeeded == 0) return nullptr;

        const uintptr_t targetStart = reinterpret_cast<uintptr_t>(buffer);
        const uintptr_t targetEnd = targetStart + bytesNeeded;

        for (std::size_t step = 0; step < count_; ++step) {
            const std::size_t index = (head_ + Capacity - 1 - step) % Capacity;
            Entry& entry = entries_[index];
            if (entry.valid && entry.loadGeneration == loadGeneration && entry.buffer) {
                const uintptr_t entryStart = reinterpret_cast<uintptr_t>(entry.buffer);
                const uintptr_t entryEnd = entryStart + entry.bytesRead;

                if (targetStart >= entryStart && targetEnd <= entryEnd) {
                    if (outOffsetInArchive) {
                        *outOffsetInArchive = entry.offset + static_cast<std::uint64_t>(targetStart - entryStart);
                    }
                    return &entry;
                }
            }
        }
        return nullptr;
    }

    bool Consume(Entry* entry) noexcept {
        if (!entry) return false;
        for (Entry& slot : entries_) {
            if (&slot == entry && slot.valid) {
                slot = Entry{};
                return true;
            }
        }
        return false;
    }

    void Clear() {
        for (Entry& entry : entries_) entry = Entry{};
        head_ = 0;
        count_ = 0;
    }

private:
    Entry entries_[Capacity]{};
    std::size_t head_ = 0;
    std::size_t count_ = 0;
};

inline bool SamePakArchiveName(const char* left, const char* right) noexcept {
    if (!left || !right) return false;
    while (*left && *right) {
        unsigned char a = static_cast<unsigned char>(*left++);
        unsigned char b = static_cast<unsigned char>(*right++);
        if (a >= 'A' && a <= 'Z') a = static_cast<unsigned char>(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = static_cast<unsigned char>(b + ('a' - 'A'));
        if (a != b) return false;
    }
    return *left == *right;
}

inline bool PakReadIdentityMatches(
    const char* cachedArchive,
    std::uint64_t cachedOffset,
    std::uint32_t cachedInputBytes,
    const char* readArchive,
    std::uint64_t readOffset,
    std::uint32_t readInputBytes) noexcept {
    return cachedOffset == readOffset &&
        cachedInputBytes == readInputBytes &&
        SamePakArchiveName(cachedArchive, readArchive);
}

} // namespace G3O
