#pragma once

#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace G3O {

template <std::size_t BucketCount>
class VfsLookupCache {
    static_assert(BucketCount > 0, "VFS cache must have at least one bucket");

    struct Slot {
        std::uint64_t hash = 0;
        const void* result = nullptr;
        char path[96] = {};
    };

    struct Bucket {
        Slot slots[4];
        unsigned char nextVictim = 0;
    };

public:
    VfsLookupCache() noexcept { InitializeSRWLock(&lock_); }
    VfsLookupCache(const VfsLookupCache&) = delete;
    VfsLookupCache& operator=(const VfsLookupCache&) = delete;

    const void* Find(std::uint64_t hash, const char* path, std::size_t length) const noexcept {
        if (!path || length == 0 || length >= sizeof(Slot::path)) return nullptr;
        const Bucket& bucket = buckets_[hash % BucketCount];
        AcquireSRWLockShared(&lock_);
        const void* result = nullptr;
        for (const Slot& slot : bucket.slots) {
            if (slot.result && slot.hash == hash && _stricmp(slot.path, path) == 0) {
                result = slot.result;
                break;
            }
        }
        ReleaseSRWLockShared(&lock_);
        return result;
    }

    void Store(std::uint64_t hash, const char* path, std::size_t length,
               const void* result) noexcept {
        if (!result || !path || length == 0 || length >= sizeof(Slot::path)) return;
        Bucket& bucket = buckets_[hash % BucketCount];
        AcquireSRWLockExclusive(&lock_);
        const unsigned char slotIndex = bucket.nextVictim & 3;
        bucket.nextVictim = static_cast<unsigned char>((slotIndex + 1) & 3);
        Slot& slot = bucket.slots[slotIndex];
        slot.hash = hash;
        slot.result = nullptr;
        std::memcpy(slot.path, path, length);
        slot.path[length] = '\0';
        slot.result = result;
        ReleaseSRWLockExclusive(&lock_);
    }

    void Clear() noexcept {
        AcquireSRWLockExclusive(&lock_);
        std::memset(buckets_, 0, sizeof(buckets_));
        ReleaseSRWLockExclusive(&lock_);
    }

private:
    mutable SRWLOCK lock_;
    Bucket buckets_[BucketCount] = {};
};

} // namespace G3O
