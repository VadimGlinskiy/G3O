#include "vfs_cache_optimizer.h"
#include "diagnostic_logger.h"
#include <minhook.h>
#include <windows.h>
#include <stdint.h>
#include <string.h>

namespace G3O {

// -----------------------------------------------------------------------------
// Telemetry Counters
// -----------------------------------------------------------------------------
static volatile LONG s_vfsCacheHits = 0;
static volatile LONG s_vfsCacheMisses = 0;

LONG GetVFSCacheHits() { return s_vfsCacheHits; }
LONG GetVFSCacheMisses() { return s_vfsCacheMisses; }
void ResetVFSCacheTelemetry() {
    InterlockedExchange(&s_vfsCacheHits, 0);
    InterlockedExchange(&s_vfsCacheMisses, 0);
}

// -----------------------------------------------------------------------------
// EASTL String Extraction & Path Hashing
// -----------------------------------------------------------------------------
// In EASTL basic_string<char>, byte 11 indicates small-string-optimization (SSO) vs heap.
// If bit 7 of byte 11 is set: Heap allocated string:
//   offset 0: pointer to char buffer (const char*)
//   offset 4: string length (size_t)
// If bit 7 of byte 11 is NOT set: SSO inline string:
//   offset 0: char data starts directly at buffer
//   offset 11: remaining capacity, length = 0x0B - (byte 11 & 0x7F)
static inline const char* ExtractEastlString(const void* pStr, size_t& outLen) {
    if (!pStr) {
        outLen = 0;
        return "";
    }
    const BYTE* b = (const BYTE*)pStr;
    if ((b[11] >> 7) & 1) {
        outLen = (size_t)*(const DWORD*)(b + 4);
        const char* ptr = *(const char* const*)b;
        return ptr ? ptr : "";
    } else {
        outLen = (size_t)(0x0B - (b[11] & 0x7F));
        return (const char*)b;
    }
}

static inline uint64_t HashNormalizedPath(const char* str, size_t len) {
    // 64-bit FNV-1a hash with inline case-folding and slash-normalization
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)str[i];
        if (c >= 'A' && c <= 'Z') {
            c += ('a' - 'A');
        } else if (c == '/') {
            c = '\\';
        }
        h ^= (uint64_t)c;
        h *= 1099511628211ULL;
    }
    return h;
}

// -----------------------------------------------------------------------------
// 4-Way Associative Flat Hash Cache
// -----------------------------------------------------------------------------
// 16,384 buckets * 4 slots = 65,536 cache entries (~1 MB total footprint)
static const size_t VFS_BUCKET_COUNT = 16384;
static const size_t VFS_BUCKET_MASK = VFS_BUCKET_COUNT - 1;

struct VFSSlot {
    uint64_t hash;
    const void* result;
    uint8_t flags; // 0 = empty, 1 = valid result, 2 = negative cached (null)
};

struct VFSBucket {
    VFSSlot slots[4];
    uint8_t nextVictim;
};

static VFSBucket s_buckets[VFS_BUCKET_COUNT];
static SRWLOCK s_cacheLock = SRWLOCK_INIT;

void ClearVFSCache() {
    AcquireSRWLockExclusive(&s_cacheLock);
    memset(s_buckets, 0, sizeof(s_buckets));
    ReleaseSRWLockExclusive(&s_cacheLock);
}

// -----------------------------------------------------------------------------
// Hooks
// -----------------------------------------------------------------------------
typedef const void* (__thiscall* GetDirCacheEntry_t)(void* pThis, const void* pEastlStr);
static GetDirCacheEntry_t Real_GetDirCacheEntry = nullptr;
static void* s_targetGetDirCacheEntry = nullptr;

typedef void (__thiscall* RemoveFromDirCache_t)(void* pThis, const void* pEastlStr, bool a, bool b);
static RemoveFromDirCache_t Real_RemoveFromDirCache = nullptr;
static void* s_targetRemoveFromDirCache = nullptr;

typedef bool (__thiscall* UnmountPackFiles_t)(void* pThis, const void* pStr);
static UnmountPackFiles_t Real_UnmountPackFiles = nullptr;
static void* s_targetUnmountPackFiles = nullptr;

typedef bool (__thiscall* UnmountDirectory_t)(void* pThis, const void* pStr);
static UnmountDirectory_t Real_UnmountDirectory = nullptr;
static void* s_targetUnmountDirectory = nullptr;

static const void* __fastcall Hooked_GetDirCacheEntry(void* pThis, void* /*edx*/, const void* pEastlStr) {
    if (!pEastlStr) {
        return Real_GetDirCacheEntry(pThis, pEastlStr);
    }

    size_t len = 0;
    const char* path = ExtractEastlString(pEastlStr, len);
    if (len == 0 || !path) {
        return Real_GetDirCacheEntry(pThis, pEastlStr);
    }

    uint64_t hash = HashNormalizedPath(path, len);
    size_t bucketIdx = (size_t)(hash & VFS_BUCKET_MASK);
    VFSBucket& bucket = s_buckets[bucketIdx];

    // Read lock-free for fast path (slots are updated atomically or near-atomically)
    for (int i = 0; i < 4; ++i) {
        if (bucket.slots[i].flags != 0 && bucket.slots[i].hash == hash) {
            InterlockedIncrement(&s_vfsCacheHits);
            return (bucket.slots[i].flags == 2) ? nullptr : bucket.slots[i].result;
        }
    }

    // Cache miss: Query original engine function
    InterlockedIncrement(&s_vfsCacheMisses);
    const void* res = Real_GetDirCacheEntry(pThis, pEastlStr);

    // Insert into cache
    AcquireSRWLockExclusive(&s_cacheLock);
    uint8_t slotIdx = bucket.nextVictim & 3;
    bucket.nextVictim = (slotIdx + 1) & 3;

    bucket.slots[slotIdx].hash = hash;
    bucket.slots[slotIdx].result = res;
    bucket.slots[slotIdx].flags = (res != nullptr) ? 1 : 2; // 2 = negative cache
    ReleaseSRWLockExclusive(&s_cacheLock);

    return res;
}

static void __fastcall Hooked_RemoveFromDirCache(void* pThis, void* /*edx*/, const void* pEastlStr, bool a, bool b) {
    ClearVFSCache();
    Real_RemoveFromDirCache(pThis, pEastlStr, a, b);
}

static bool __fastcall Hooked_UnmountPackFiles(void* pThis, void* /*edx*/, const void* pStr) {
    ClearVFSCache();
    return Real_UnmountPackFiles(pThis, pStr);
}

static bool __fastcall Hooked_UnmountDirectory(void* pThis, void* /*edx*/, const void* pStr) {
    ClearVFSCache();
    return Real_UnmountDirectory(pThis, pStr);
}

bool InitializeVFSCacheOptimizer() {
    HMODULE hFS = GetModuleHandleA("FileSystem.dll");
    if (!hFS) return false;

    ClearVFSCache();

    void* pGetDir = (void*)GetProcAddress(hFS, "?GetDirCacheEntry@CFFFileSystemModule@@ABEPBUCFFDirCacheEntry@@ABV?$basic_string@DVallocator@eastl@@@eastl@@@Z");
    if (pGetDir) {
        s_targetGetDirCacheEntry = pGetDir;
        MH_CreateHook(pGetDir, (LPVOID)Hooked_GetDirCacheEntry, (LPVOID*)&Real_GetDirCacheEntry);
        MH_EnableHook(pGetDir);
        Log("G3O", "VFS Cache Optimizer: Hooked FileSystem.dll!GetDirCacheEntry (0x%p) with 64K O(1) hash cache.", pGetDir);
    }

    void* pRemove = (void*)GetProcAddress(hFS, "?RemoveFromDirCache@CFFFileSystemModule@@AAEXABV?$basic_string@DVallocator@eastl@@@eastl@@_N1@Z");
    if (pRemove) {
        s_targetRemoveFromDirCache = pRemove;
        MH_CreateHook(pRemove, (LPVOID)Hooked_RemoveFromDirCache, (LPVOID*)&Real_RemoveFromDirCache);
        MH_EnableHook(pRemove);
    }

    void* pUnmountPaks = (void*)GetProcAddress(hFS, "?UnmountPackFiles@CFFFileSystemModule@@UAE_NABVbCString@@@Z");
    if (pUnmountPaks) {
        s_targetUnmountPackFiles = pUnmountPaks;
        MH_CreateHook(pUnmountPaks, (LPVOID)Hooked_UnmountPackFiles, (LPVOID*)&Real_UnmountPackFiles);
        MH_EnableHook(pUnmountPaks);
    }

    void* pUnmountDir = (void*)GetProcAddress(hFS, "?UnmountDirectory@CFFFileSystemModule@@UAE_NABVbCString@@@Z");
    if (pUnmountDir) {
        s_targetUnmountDirectory = pUnmountDir;
        MH_CreateHook(pUnmountDir, (LPVOID)Hooked_UnmountDirectory, (LPVOID*)&Real_UnmountDirectory);
        MH_EnableHook(pUnmountDir);
    }

    Log("G3O", "VFS Cache Optimizer initialized successfully.");
    return true;
}

void ShutdownVFSCacheOptimizer() {
    if (s_targetGetDirCacheEntry) MH_DisableHook(s_targetGetDirCacheEntry);
    if (s_targetRemoveFromDirCache) MH_DisableHook(s_targetRemoveFromDirCache);
    if (s_targetUnmountPackFiles) MH_DisableHook(s_targetUnmountPackFiles);
    if (s_targetUnmountDirectory) MH_DisableHook(s_targetUnmountDirectory);
    ClearVFSCache();
}

} // namespace G3O
