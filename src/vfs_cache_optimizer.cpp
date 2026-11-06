#include "vfs_cache_optimizer.h"
#include "vfs_cache_core.h"
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
// 4-Way Associative Flat Hash Cache with Exact Path Verification
// -----------------------------------------------------------------------------
// 16,384 buckets * 4 slots = 65,536 cache entries (~7 MB total footprint)
static const size_t VFS_BUCKET_COUNT = 16384;
static VfsLookupCache<VFS_BUCKET_COUNT> s_cache;

void ClearVFSCache() {
    s_cache.Clear();
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

static const void* __fastcall Hooked_GetDirCacheEntry(void* pThis, void* /*edx*/, const void* pEastlStr) {
    if (!pEastlStr) {
        return Real_GetDirCacheEntry(pThis, pEastlStr);
    }

    size_t len = 0;
    const char* path = ExtractEastlString(pEastlStr, len);
    if (len == 0 || !path || len >= 96) {
        return Real_GetDirCacheEntry(pThis, pEastlStr);
    }

    uint64_t hash = HashNormalizedPath(path, len);
    if (const void* cached = s_cache.Find(hash, path, len)) {
        InterlockedIncrement(&s_vfsCacheHits);
        return cached;
    }

    // Cache miss: Query original engine function
    InterlockedIncrement(&s_vfsCacheMisses);
    const void* res = Real_GetDirCacheEntry(pThis, pEastlStr);

    // CRITICAL: Only cache POSITIVE findings. NEVER cache nullptr,
    // so scripts and dynamically generated files are never falsely hidden.
    if (res != nullptr) {
        s_cache.Store(hash, path, len, res);
    }

    return res;
}

static void __fastcall Hooked_RemoveFromDirCache(void* pThis, void* /*edx*/, const void* pEastlStr, bool a, bool b) {
    ClearVFSCache();
    Real_RemoveFromDirCache(pThis, pEastlStr, a, b);
}

bool InitializeVFSCacheOptimizer() {
    if (s_targetGetDirCacheEntry && s_targetRemoveFromDirCache &&
        Real_GetDirCacheEntry && Real_RemoveFromDirCache) {
        return true;
    }

    HMODULE hFS = GetModuleHandleA("FileSystem.dll");
    if (!hFS) {
        Log("G3O", "VFS Cache Optimizer: FileSystem.dll is not loaded; cache remains disabled.");
        return false;
    }

    void* pGetDir = (void*)GetProcAddress(hFS, "?GetDirCacheEntry@CFFFileSystemModule@@ABEPBUCFFDirCacheEntry@@ABV?$basic_string@DVallocator@eastl@@@eastl@@@Z");
    void* pRemove = (void*)GetProcAddress(hFS, "?RemoveFromDirCache@CFFFileSystemModule@@AAEXABV?$basic_string@DVallocator@eastl@@@eastl@@_N1@Z");
    if (!pGetDir || !pRemove) {
        Log("G3O", "VFS Cache Optimizer: required lookup/invalidation export missing; cache remains disabled.");
        return false;
    }

    MH_STATUS getStatus = MH_CreateHook(pGetDir, (LPVOID)Hooked_GetDirCacheEntry,
                                        (LPVOID*)&Real_GetDirCacheEntry);
    if (getStatus != MH_OK) {
        Log("G3O", "VFS Cache Optimizer: lookup hook creation failed (code %d).", (int)getStatus);
        return false;
    }

    MH_STATUS removeStatus = MH_CreateHook(pRemove, (LPVOID)Hooked_RemoveFromDirCache,
                                           (LPVOID*)&Real_RemoveFromDirCache);
    if (removeStatus != MH_OK) {
        MH_RemoveHook(pGetDir);
        Real_GetDirCacheEntry = nullptr;
        Log("G3O", "VFS Cache Optimizer: invalidation hook creation failed (code %d).", (int)removeStatus);
        return false;
    }

    getStatus = MH_EnableHook(pGetDir);
    removeStatus = getStatus == MH_OK ? MH_EnableHook(pRemove) : getStatus;
    if (getStatus != MH_OK || removeStatus != MH_OK) {
        MH_DisableHook(pGetDir);
        MH_DisableHook(pRemove);
        MH_RemoveHook(pGetDir);
        MH_RemoveHook(pRemove);
        Real_GetDirCacheEntry = nullptr;
        Real_RemoveFromDirCache = nullptr;
        Log("G3O", "VFS Cache Optimizer: hook activation failed (lookup=%d, invalidation=%d).",
            (int)getStatus, (int)removeStatus);
        return false;
    }

    s_targetGetDirCacheEntry = pGetDir;
    s_targetRemoveFromDirCache = pRemove;
    ClearVFSCache();
    Log("G3O", "VFS Cache Optimizer enabled with lookup and invalidation hooks.");
    return true;
}

void ShutdownVFSCacheOptimizer() {
    if (s_targetGetDirCacheEntry) {
        MH_DisableHook(s_targetGetDirCacheEntry);
        MH_RemoveHook(s_targetGetDirCacheEntry);
        s_targetGetDirCacheEntry = nullptr;
    }
    if (s_targetRemoveFromDirCache) {
        MH_DisableHook(s_targetRemoveFromDirCache);
        MH_RemoveHook(s_targetRemoveFromDirCache);
        s_targetRemoveFromDirCache = nullptr;
    }
    Real_GetDirCacheEntry = nullptr;
    Real_RemoveFromDirCache = nullptr;
    ClearVFSCache();
}

} // namespace G3O
