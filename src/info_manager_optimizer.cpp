#include "info_manager_optimizer.h"
#include "config_manager.h"
#include "diagnostic_logger.h"
#include <minhook.h>
#include <windows.h>
#include <atomic>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cctype>
#include <cstring>

namespace G3O {

// ============================================================================
// 1. Data Structures matching Genome Engine (Game.dll / SharedBase.dll, x86)
// ============================================================================
#pragma pack(push, 1)

struct bCString {
    const char* m_pcText; // Pointer to null-terminated ANSI buffer
};

struct gCInfo_PS;
struct gCInfoManager_PS;

struct SInfoMapNode {
    bCString       m_key;     // +0x00
    gCInfo_PS*     m_pValue;  // +0x04
    SInfoMapNode*  m_pNext;   // +0x08
};

// Layout of bTValMap<bCString, gCInfo_PS*> at gCInfoManager_PS + 0x4C:
// Verified via disassembly of Game.dll!0x10163590 and 0x1005E270:
// +0x00: SInfoMapNode** m_ppBuckets
// +0x04: uint32_t       m_u32BucketCount (initialized to 43 = 0x2B)
// +0x08: uint32_t       m_u32Capacity
// +0x0C: uint32_t       m_u32NodeCount (total count of nodes in map)
struct bTInfoValMap {
    SInfoMapNode** m_ppBuckets;      // +0x00
    uint32_t       m_u32BucketCount; // +0x04
    uint32_t       m_u32Capacity;    // +0x08
    uint32_t       m_u32NodeCount;   // +0x0C
};

#pragma pack(pop)

static_assert(sizeof(bCString) == 4, "bCString size mismatch");
static_assert(sizeof(SInfoMapNode) == 12, "SInfoMapNode size mismatch");
static_assert(sizeof(bTInfoValMap) == 16, "bTInfoValMap size mismatch");

// ============================================================================
// 2. Fast String Utilities & Transparent Hash
// ============================================================================
static inline std::string_view ToStringView(const bCString& str) noexcept {
    if (!str.m_pcText || !str.m_pcText[0]) {
        return {};
    }
    return std::string_view(str.m_pcText);
}

struct FastStringHash {
    using is_transparent = void;

    [[nodiscard]] size_t operator()(std::string_view sv) const noexcept {
        uint32_t hash = 2166136261u;
        for (unsigned char c : sv) {
            hash ^= c;
            hash *= 16777619u;
        }
        return hash;
    }
};

// ============================================================================
// 3. Shadow Hash Table Cache
// ============================================================================
class CInfoShadowCache {
public:
    static gCInfo_PS* Lookup(
        gCInfoManager_PS* pManager,
        const bTInfoValMap* pMap,
        const bCString& key,
        gCInfo_PS* (__thiscall* fnOrigGetInfo)(gCInfoManager_PS*, const bCString*))
    {
        const std::string_view svKey = ToStringView(key);
        if (svKey.empty()) {
            return nullptr;
        }

        // Fast path: shared read lock
        AcquireSRWLockShared(&s_lock);
        const bool needsRebuild = (m_pLastManager != pManager ||
                                   m_pLastBuckets != pMap->m_ppBuckets ||
                                   pMap->m_u32NodeCount != m_uLastIndexedCount ||
                                   pMap->m_u32NodeCount == 0);
        if (!needsRebuild) {
            auto it = m_shadowMap.find(svKey);
            if (it != m_shadowMap.end()) {
                ReleaseSRWLockShared(&s_lock);
                s_u64Hits.fetch_add(1, std::memory_order_relaxed);
                return it->second;
            }
            // If shadow map has all nodes from engine map, key definitely does not exist
            ReleaseSRWLockShared(&s_lock);
            s_u64Misses.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        ReleaseSRWLockShared(&s_lock);

        // Slow path: exclusive write lock to rebuild index
        AcquireSRWLockExclusive(&s_lock);
        // Double-check condition under exclusive lock
        if (m_pLastManager != pManager ||
            m_pLastBuckets != pMap->m_ppBuckets ||
            pMap->m_u32NodeCount != m_uLastIndexedCount ||
            pMap->m_u32NodeCount == 0)
        {
            RebuildFull(pManager, pMap);
        }

        auto it = m_shadowMap.find(svKey);
        if (it != m_shadowMap.end()) {
            gCInfo_PS* result = it->second;
            ReleaseSRWLockExclusive(&s_lock);
            s_u64Hits.fetch_add(1, std::memory_order_relaxed);
            return result;
        }
        ReleaseSRWLockExclusive(&s_lock);

        // Fallback: call original GetInfo if somehow not indexed
        s_u64Misses.fetch_add(1, std::memory_order_relaxed);
        return fnOrigGetInfo ? fnOrigGetInfo(pManager, &key) : nullptr;
    }

    static void Invalidate() noexcept {
        AcquireSRWLockExclusive(&s_lock);
        m_shadowMap.clear();
        m_pLastManager = nullptr;
        m_pLastBuckets = nullptr;
        m_uLastIndexedCount = 0;
        ReleaseSRWLockExclusive(&s_lock);
    }

    static uint64_t GetHits() noexcept { return s_u64Hits.load(std::memory_order_relaxed); }
    static uint64_t GetMisses() noexcept { return s_u64Misses.load(std::memory_order_relaxed); }
    static uint32_t GetRebuilds() noexcept { return s_u32Rebuilds.load(std::memory_order_relaxed); }
    static uint32_t GetIndexedCount() noexcept { return m_uLastIndexedCount; }

private:
    static void RebuildFull(gCInfoManager_PS* pManager, const bTInfoValMap* pMap) {
        m_shadowMap.clear();
        m_pLastManager = pManager;
        m_pLastBuckets = pMap->m_ppBuckets;
        m_uLastIndexedCount = 0;
        s_u32Rebuilds.fetch_add(1, std::memory_order_relaxed);

        if (!pMap->m_ppBuckets || pMap->m_u32BucketCount == 0 || pMap->m_u32NodeCount == 0) {
            return;
        }

        if (m_shadowMap.bucket_count() < 32768) {
            m_shadowMap.reserve(32768);
        }

        for (uint32_t i = 0; i < pMap->m_u32BucketCount; ++i) {
            for (SInfoMapNode* pNode = pMap->m_ppBuckets[i]; pNode != nullptr; pNode = pNode->m_pNext) {
                const std::string_view sv = ToStringView(pNode->m_key);
                if (!sv.empty() && pNode->m_pValue != nullptr) {
                    m_shadowMap.emplace(sv, pNode->m_pValue);
                }
            }
        }

        m_uLastIndexedCount = pMap->m_u32NodeCount;
    }

    static inline SRWLOCK s_lock = SRWLOCK_INIT;
    static inline std::unordered_map<std::string_view, gCInfo_PS*, FastStringHash, std::equal_to<>> m_shadowMap;
    static inline gCInfoManager_PS* m_pLastManager = nullptr;
    static inline SInfoMapNode**    m_pLastBuckets = nullptr;
    static inline uint32_t          m_uLastIndexedCount = 0;

    static inline std::atomic<uint64_t> s_u64Hits{0};
    static inline std::atomic<uint64_t> s_u64Misses{0};
    static inline std::atomic<uint32_t> s_u32Rebuilds{0};
};

// ============================================================================
// 4. Dynamic Bucket Expansion & Map Resize Telemetry
// ============================================================================
using FnMemAdminGetInstance = void* (__stdcall*)();
using FnMemAdminRealloc = void* (__thiscall*)(void* admin, void* memory, uint32_t bytes);
using FnGetHashValueCString = uint32_t (__stdcall*)(const bCString& str);

static void* s_realInitHashTable = nullptr;
static void* s_realInfoMapClear = nullptr;

static uintptr_t s_expectedInitReturn = 0;
static uintptr_t s_expectedClearReturn = 0;
static uintptr_t s_gameBase = 0;

static FnMemAdminGetInstance s_memAdminGetInstance = nullptr;
static FnMemAdminRealloc     s_memAdminRealloc     = nullptr;
static FnGetHashValueCString s_getHashValueCString = nullptr;

// Prime number bucket count to ensure optimal hash distribution across 24,555 dialogs
static constexpr uint32_t kInfoMapBucketCount = 32771;

struct MapResizeStats {
    std::atomic<uint32_t> calls{0};
    std::atomic<uint32_t> matches{0};
    std::atomic<uint32_t> reallocAttempts{0};
    std::atomic<uint32_t> expanded{0};
    std::atomic<uint32_t> reallocFailures{0};
    std::atomic<uint32_t> allocatorUnavailable{0};
    std::atomic<uint32_t> nullBuckets{0};
    std::atomic<uint32_t> alreadyLarge{0};
    std::atomic<uint32_t> bucketsBefore{0};
    std::atomic<uint32_t> bucketsAfter{0};
    std::atomic<uintptr_t> firstMismatchedCallerRva{0};
};

static MapResizeStats s_loadInfosStats;
static MapResizeStats s_initStats;
static MapResizeStats s_clearStats;

// Exact bit-for-bit duplicate of Genome Engine's djb2 hash implementation in SharedBase.dll!0x10055F40
static inline uint32_t FallbackHashCString(const bCString& str) noexcept {
    const char* s = str.m_pcText;
    if (!s) return 0;
    uint32_t hash = 0;
    while (*s) {
        // Genome Engine: hash = (hash << 5) + hash + (int32_t)(signed char)*s
        hash = (hash * 33) + static_cast<int32_t>(static_cast<signed char>(*s));
        ++s;
    }
    return hash;
}

// Call original method: in this binary, 'this' pointer is passed via EAX register.
static uint32_t CallOriginalWithEax(void* trampoline, bTInfoValMap* map)
{
    uint32_t result = 0;

    __asm {
        mov eax, map
        call dword ptr [trampoline]
        mov result, eax
    }

    return result;
}

static void ExpandAndRehashInfoMap(
    bTInfoValMap* map,
    uint32_t newBucketCount,
    const char* sourceName,
    MapResizeStats* pStats = nullptr)
{
    if (!map) {
        return;
    }

    const uint32_t before = map->m_u32BucketCount;
    if (pStats) {
        pStats->bucketsBefore.store(before, std::memory_order_relaxed);
    }

    if (before >= newBucketCount) {
        if (pStats) {
            pStats->alreadyLarge.fetch_add(1, std::memory_order_relaxed);
            pStats->bucketsAfter.store(before, std::memory_order_relaxed);
        }
        return;
    }

    if (!map->m_ppBuckets) {
        if (pStats) {
            pStats->nullBuckets.fetch_add(1, std::memory_order_relaxed);
            pStats->bucketsAfter.store(before, std::memory_order_relaxed);
        }
        return;
    }

    if (!s_memAdminGetInstance || !s_memAdminRealloc) {
        if (pStats) {
            pStats->allocatorUnavailable.fetch_add(1, std::memory_order_relaxed);
            pStats->bucketsAfter.store(before, std::memory_order_relaxed);
        }
        return;
    }

    void* admin = s_memAdminGetInstance();
    if (!admin) {
        if (pStats) {
            pStats->allocatorUnavailable.fetch_add(1, std::memory_order_relaxed);
            pStats->bucketsAfter.store(before, std::memory_order_relaxed);
        }
        return;
    }

    if (pStats) {
        pStats->reallocAttempts.fetch_add(1, std::memory_order_relaxed);
    }

    SInfoMapNode** oldBuckets = map->m_ppBuckets;
    const uint32_t oldBucketCount = map->m_u32BucketCount;
    const uint32_t nodeCount = map->m_u32NodeCount;

    const uint32_t bytes = newBucketCount * static_cast<uint32_t>(sizeof(SInfoMapNode*));
    auto* newBuckets = static_cast<SInfoMapNode**>(
        s_memAdminRealloc(admin, nullptr, bytes));

    if (!newBuckets) {
        if (pStats) {
            pStats->reallocFailures.fetch_add(1, std::memory_order_relaxed);
            pStats->bucketsAfter.store(map->m_u32BucketCount, std::memory_order_relaxed);
        }
        return;
    }

    std::memset(newBuckets, 0, bytes);

    // Rehash existing nodes without reallocation to guarantee 100% safety on QuickLoad
    if (oldBuckets && nodeCount > 0) {
        for (uint32_t i = 0; i < oldBucketCount; ++i) {
            SInfoMapNode* pNode = oldBuckets[i];
            while (pNode) {
                SInfoMapNode* pNext = pNode->m_pNext;
                const uint32_t hash = s_getHashValueCString
                    ? s_getHashValueCString(pNode->m_key)
                    : FallbackHashCString(pNode->m_key);
                const uint32_t newIdx = hash % newBucketCount;

                pNode->m_pNext = newBuckets[newIdx];
                newBuckets[newIdx] = pNode;

                pNode = pNext;
            }
        }
    }

    // Free the old bucket array
    if (oldBuckets) {
        s_memAdminRealloc(admin, oldBuckets, 0);
    }

    map->m_ppBuckets = newBuckets;
    map->m_u32BucketCount = newBucketCount;
    map->m_u32Capacity = newBucketCount;

    if (pStats) {
        pStats->expanded.fetch_add(1, std::memory_order_relaxed);
        pStats->bucketsAfter.store(newBucketCount, std::memory_order_relaxed);
    }

    Log("G3O", "[InfoManager] Successfully expanded InfoMap to %u buckets via bCMemoryAdmin (source: %s, preserved: %u nodes, buckets: %u -> %u)!",
        newBucketCount, sourceName, nodeCount, before, newBucketCount);
}

static uint32_t __stdcall HookInitHashTableImpl(
    bTInfoValMap* map,
    void* callerReturn)
{
    s_initStats.calls.fetch_add(1, std::memory_order_relaxed);

    const uint32_t originalResult =
        CallOriginalWithEax(s_realInitHashTable, map);

    const uintptr_t caller = reinterpret_cast<uintptr_t>(callerReturn);
    if (caller != s_expectedInitReturn) {
        uintptr_t empty = 0;
        s_initStats.firstMismatchedCallerRva.compare_exchange_strong(
            empty, caller - s_gameBase, std::memory_order_relaxed);
        return originalResult;
    }

    s_initStats.matches.fetch_add(1, std::memory_order_relaxed);
    ExpandAndRehashInfoMap(map, kInfoMapBucketCount, "InitHashTable", &s_initStats);
    return originalResult;
}

__declspec(naked) static void HookInitHashTableThunk()
{
    __asm {
        push dword ptr [esp]       // Caller return address
        push eax                   // this / map
        call HookInitHashTableImpl // __stdcall cleans up both arguments
        ret
    }
}

static uint32_t __stdcall HookInfoMapClearImpl(
    bTInfoValMap* map,
    void* callerReturn)
{
    s_clearStats.calls.fetch_add(1, std::memory_order_relaxed);

    const uint32_t originalResult =
        CallOriginalWithEax(s_realInfoMapClear, map);

    const uintptr_t caller = reinterpret_cast<uintptr_t>(callerReturn);
    if (caller != s_expectedClearReturn) {
        uintptr_t empty = 0;
        s_clearStats.firstMismatchedCallerRva.compare_exchange_strong(
            empty, caller - s_gameBase, std::memory_order_relaxed);
        return originalResult;
    }

    s_clearStats.matches.fetch_add(1, std::memory_order_relaxed);
    ExpandAndRehashInfoMap(map, kInfoMapBucketCount, "Clear", &s_clearStats);
    return originalResult;
}

__declspec(naked) static void HookInfoMapClearThunk()
{
    __asm {
        push dword ptr [esp]       // Caller return address
        push eax                   // this / map
        call HookInfoMapClearImpl  // __stdcall cleans up both arguments
        ret
    }
}

// ============================================================================
// 5. Hook for gCInfoManager_PS::GetInfo (Game.dll RVA +0x1CE270)
// ============================================================================
using FnGetInfo = gCInfo_PS* (__thiscall*)(gCInfoManager_PS* pThis, const bCString* pName);

static FnGetInfo Real_GetInfo = nullptr;
static void*     s_targetGetInfo = nullptr;
static void*     s_targetInitHashTable = nullptr;
static bool      s_bInitialized = false;

// ============================================================================
// 6. Micro-timing probes for bTPropertyObject<gCInfoManager_PS>::Read
// ============================================================================
using FnPropertyObjectRead = uint16_t (__thiscall*)(void* pThis, void* pStream, void* pArg2);
static FnPropertyObjectRead Real_PropertyObjectRead = nullptr;
static void* s_targetPropertyObjectRead = nullptr;

using FnInfoManagerCreate = void (__thiscall*)(void* pThis);
static FnInfoManagerCreate Real_InfoManagerCreate = nullptr;
static void* s_targetInfoManagerCreate = nullptr;
static uintptr_t s_expectedPhaseBReturn = 0;

using FnStreamReadObjectBase = void* (__stdcall*)(void* pStream, void* pObject);
static FnStreamReadObjectBase Real_StreamReadObjectBase = nullptr;
static void* s_targetStreamReadObjectBase = nullptr;
static uintptr_t s_expectedPhaseCReturn = 0;

using FnLoadInfosFromInis = bool (__thiscall*)(void* pThis);
static FnLoadInfosFromInis Real_LoadInfosFromInis = nullptr;
static void* s_targetLoadInfosFromInis = nullptr;

struct InfoManagerReadAudit {
    uint64_t phaseBTicks = 0;
    uint32_t phaseBCalls = 0;
    uint64_t phaseCTicks = 0;
    uint32_t phaseCCalls = 0;
    uint64_t loadInfosFromInisTicks = 0;
    uint32_t loadInfosFromInisCalls = 0;
};
static thread_local InfoManagerReadAudit s_infoAudit{};

static uint16_t __fastcall Hooked_InfoManagerPropertyObjectRead(
    void* pThis, void* /*edx*/, void* pStream, void* pArg2)
{
    s_infoAudit = {};
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    const uint16_t result = Real_PropertyObjectRead(pThis, pStream, pArg2);
    QueryPerformanceCounter(&t1);

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    if (freq.QuadPart > 0) {
        const double totalMs = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
        const double phaseBMs = static_cast<double>(s_infoAudit.phaseBTicks) * 1000.0 / static_cast<double>(freq.QuadPart);
        const double phaseCMs = static_cast<double>(s_infoAudit.phaseCTicks) * 1000.0 / static_cast<double>(freq.QuadPart);
        double phaseAMs = totalMs - phaseBMs - phaseCMs;
        if (phaseAMs < 0.0) phaseAMs = 0.0;
        const double loadInisMs = static_cast<double>(s_infoAudit.loadInfosFromInisTicks) * 1000.0 / static_cast<double>(freq.QuadPart);

        Log("G3O", "[InfoManager Audit] 0x1D0240 Total: %.2f ms | Phase A: %.2f ms | Phase B: %.2f ms (calls=%u) | Phase C: %.2f ms (calls=%u, LoadInfosFromInis: %.2f ms, calls=%u)",
            totalMs, phaseAMs,
            phaseBMs, s_infoAudit.phaseBCalls,
            phaseCMs, s_infoAudit.phaseCCalls,
            loadInisMs, s_infoAudit.loadInfosFromInisCalls);
    }
    return result;
}

static void __fastcall Hooked_InfoManagerCreate(void* pThis, void* /*edx*/) {
    const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    if (caller == s_expectedPhaseBReturn) {
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        Real_InfoManagerCreate(pThis);
        QueryPerformanceCounter(&t1);
        s_infoAudit.phaseBTicks += static_cast<uint64_t>(t1.QuadPart - t0.QuadPart);
        s_infoAudit.phaseBCalls++;
        return;
    }
    Real_InfoManagerCreate(pThis);
}

static void* __stdcall Hooked_StreamReadObjectBase(void* pStream, void* pObject) {
    const uintptr_t caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    if (caller == s_expectedPhaseCReturn) {
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        void* result = Real_StreamReadObjectBase(pStream, pObject);
        QueryPerformanceCounter(&t1);
        s_infoAudit.phaseCTicks += static_cast<uint64_t>(t1.QuadPart - t0.QuadPart);
        s_infoAudit.phaseCCalls++;
        return result;
    }
    return Real_StreamReadObjectBase(pStream, pObject);
}

static bool __fastcall Hooked_LoadInfosFromInis(void* pThis, void* /*edx*/) {
    if (pThis) {
        s_loadInfosStats.calls.fetch_add(1, std::memory_order_relaxed);
        s_loadInfosStats.matches.fetch_add(1, std::memory_order_relaxed);
        auto* pMap = reinterpret_cast<bTInfoValMap*>(
            reinterpret_cast<uint8_t*>(pThis) + 0x4C);
        ExpandAndRehashInfoMap(pMap, kInfoMapBucketCount, "LoadInfosFromInis (this+0x4C)", &s_loadInfosStats);
    }
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    const bool result = Real_LoadInfosFromInis(pThis);
    QueryPerformanceCounter(&t1);
    s_infoAudit.loadInfosFromInisTicks += static_cast<uint64_t>(t1.QuadPart - t0.QuadPart);
    s_infoAudit.loadInfosFromInisCalls++;
    return result;
}

// Calling convention: __thiscall (ECX = this, [ESP+4] = pName)
// In MSVC x86, __fastcall with dummy EDX maps directly to __thiscall with 1 stack argument.
static gCInfo_PS* __fastcall Hooked_GetInfo(
    gCInfoManager_PS* pThis,
    void* /*edx_unused*/,
    const bCString* pName)
{
    if (!pThis || !pName || !pName->m_pcText) {
        return nullptr;
    }

    // m_mapInfos is located at this + 0x4C
    const auto* pMap = reinterpret_cast<const bTInfoValMap*>(
        reinterpret_cast<const uint8_t*>(pThis) + 0x4C
    );

    // Fast check: if map buckets are not yet allocated, return null
    if (!pMap->m_ppBuckets || pMap->m_u32BucketCount == 0) {
        return nullptr;
    }

    return CInfoShadowCache::Lookup(pThis, pMap, *pName, Real_GetInfo);
}

// ============================================================================
// 6. Binary Cache Infrastructure & High-Performance Buffered Stream (FastFileStream)
// ============================================================================

namespace FastFileStream {

struct StreamBuffer {
    bool inUse = false;
    bool isWrite = false;
    std::string filePath;
    std::vector<uint8_t> data;
    size_t position = 0;
};

static constexpr uintptr_t kPseudoHandleBase = 0x7F000000;
static constexpr size_t kMaxBuffers = 4;
static StreamBuffer s_buffers[kMaxBuffers];
static SRWLOCK s_lock = SRWLOCK_INIT;

static inline bool IsPseudoHandle(HANDLE h) noexcept {
    const uintptr_t val = reinterpret_cast<uintptr_t>(h);
    return (val & 0xFFFFFF00) == kPseudoHandleBase;
}

static inline size_t HandleToIndex(HANDLE h) noexcept {
    return reinterpret_cast<uintptr_t>(h) & 0xFF;
}

static inline HANDLE IndexToHandle(size_t idx) noexcept {
    return reinterpret_cast<HANDLE>(kPseudoHandleBase | (idx & 0xFF));
}

using FnCreateFileA      = HANDLE (__stdcall*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using FnReadFile         = BOOL   (__stdcall*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
using FnWriteFile        = BOOL   (__stdcall*)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
using FnSetFilePointer   = DWORD  (__stdcall*)(HANDLE, LONG, PLONG, DWORD);
using FnSetEndOfFile     = BOOL   (__stdcall*)(HANDLE);
using FnGetFileSize      = DWORD  (__stdcall*)(HANDLE, LPDWORD);
using FnGetFileSizeEx    = BOOL   (__stdcall*)(HANDLE, PLARGE_INTEGER);
using FnGetFileType      = DWORD  (__stdcall*)(HANDLE);
using FnFlushFileBuffers = BOOL   (__stdcall*)(HANDLE);
using FnCloseHandle      = BOOL   (__stdcall*)(HANDLE);

static FnCreateFileA      Real_CreateFileA      = nullptr;
static FnReadFile         Real_ReadFile         = nullptr;
static FnWriteFile        Real_WriteFile        = nullptr;
static FnSetFilePointer   Real_SetFilePointer   = nullptr;
static FnSetEndOfFile     Real_SetEndOfFile     = nullptr;
static FnGetFileSize      Real_GetFileSize      = nullptr;
static FnGetFileSizeEx    Real_GetFileSizeEx    = nullptr;
static FnGetFileType      Real_GetFileType      = nullptr;
static FnFlushFileBuffers Real_FlushFileBuffers = nullptr;
static FnCloseHandle      Real_CloseHandle      = nullptr;

static HANDLE __stdcall Hooked_CreateFileA(
    LPCSTR lpFileName,
    DWORD dwDesiredAccess,
    DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes,
    HANDLE hTemplateFile)
{
    if (lpFileName && std::strstr(lpFileName, "compiledinfos")) {
        if ((dwDesiredAccess & GENERIC_WRITE) != 0) {
            AcquireSRWLockExclusive(&s_lock);
            for (size_t i = 0; i < kMaxBuffers; ++i) {
                if (!s_buffers[i].inUse) {
                    s_buffers[i].inUse = true;
                    s_buffers[i].isWrite = true;
                    s_buffers[i].filePath = lpFileName;
                    s_buffers[i].data.clear();
                    s_buffers[i].data.reserve(16 * 1024 * 1024);
                    s_buffers[i].position = 0;
                    ReleaseSRWLockExclusive(&s_lock);
                    return IndexToHandle(i);
                }
            }
            ReleaseSRWLockExclusive(&s_lock);
        } else if ((dwDesiredAccess & GENERIC_READ) != 0) {
            HANDLE hReal = Real_CreateFileA(
                lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
                dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
            if (hReal == INVALID_HANDLE_VALUE) {
                return INVALID_HANDLE_VALUE;
            }
            DWORD sizeHigh = 0;
            DWORD sizeLow = Real_GetFileSize(hReal, &sizeHigh);
            if (sizeHigh != 0 || sizeLow == INVALID_FILE_SIZE || sizeLow == 0) {
                return hReal;
            }

            std::vector<uint8_t> content(sizeLow);
            DWORD bytesRead = 0;
            const BOOL readOk = Real_ReadFile(hReal, content.data(), sizeLow, &bytesRead, nullptr);
            Real_CloseHandle(hReal);

            if (!readOk || bytesRead != sizeLow) {
                return INVALID_HANDLE_VALUE;
            }

            AcquireSRWLockExclusive(&s_lock);
            for (size_t i = 0; i < kMaxBuffers; ++i) {
                if (!s_buffers[i].inUse) {
                    s_buffers[i].inUse = true;
                    s_buffers[i].isWrite = false;
                    s_buffers[i].filePath = lpFileName;
                    s_buffers[i].data = std::move(content);
                    s_buffers[i].position = 0;
                    ReleaseSRWLockExclusive(&s_lock);
                    return IndexToHandle(i);
                }
            }
            ReleaseSRWLockExclusive(&s_lock);
        }
    }

    return Real_CreateFileA(
        lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
        dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
}

static BOOL __stdcall Hooked_ReadFile(
    HANDLE hFile,
    LPVOID lpBuffer,
    DWORD nNumberOfBytesToRead,
    LPDWORD lpNumberOfBytesRead,
    LPOVERLAPPED lpOverlapped)
{
    if (IsPseudoHandle(hFile)) {
        const size_t idx = HandleToIndex(hFile);
        if (idx < kMaxBuffers) {
            AcquireSRWLockShared(&s_lock);
            auto& buf = s_buffers[idx];
            if (buf.inUse && !buf.isWrite) {
                size_t avail = (buf.position < buf.data.size()) ? (buf.data.size() - buf.position) : 0;
                size_t toCopy = (nNumberOfBytesToRead < avail) ? nNumberOfBytesToRead : avail;
                if (toCopy > 0 && lpBuffer) {
                    std::memcpy(lpBuffer, buf.data.data() + buf.position, toCopy);
                    buf.position += toCopy;
                }
                if (lpNumberOfBytesRead) {
                    *lpNumberOfBytesRead = static_cast<DWORD>(toCopy);
                }
                ReleaseSRWLockShared(&s_lock);
                return TRUE;
            }
            ReleaseSRWLockShared(&s_lock);
        }
    }

    return Real_ReadFile(hFile, lpBuffer, nNumberOfBytesToRead, lpNumberOfBytesRead, lpOverlapped);
}

static BOOL __stdcall Hooked_WriteFile(
    HANDLE hFile,
    LPCVOID lpBuffer,
    DWORD nNumberOfBytesToWrite,
    LPDWORD lpNumberOfBytesWritten,
    LPOVERLAPPED lpOverlapped)
{
    if (IsPseudoHandle(hFile)) {
        const size_t idx = HandleToIndex(hFile);
        if (idx < kMaxBuffers) {
            AcquireSRWLockExclusive(&s_lock);
            auto& buf = s_buffers[idx];
            if (buf.inUse && buf.isWrite) {
                if (nNumberOfBytesToWrite > 0 && lpBuffer) {
                    if (buf.position + nNumberOfBytesToWrite > buf.data.size()) {
                        buf.data.resize(buf.position + nNumberOfBytesToWrite);
                    }
                    std::memcpy(buf.data.data() + buf.position, lpBuffer, nNumberOfBytesToWrite);
                    buf.position += nNumberOfBytesToWrite;
                }
                if (lpNumberOfBytesWritten) {
                    *lpNumberOfBytesWritten = nNumberOfBytesToWrite;
                }
                ReleaseSRWLockExclusive(&s_lock);
                return TRUE;
            }
            ReleaseSRWLockExclusive(&s_lock);
        }
    }

    return Real_WriteFile(hFile, lpBuffer, nNumberOfBytesToWrite, lpNumberOfBytesWritten, lpOverlapped);
}

static DWORD __stdcall Hooked_SetFilePointer(
    HANDLE hFile,
    LONG lDistanceToMove,
    PLONG lpDistanceToMoveHigh,
    DWORD dwMoveMethod)
{
    if (IsPseudoHandle(hFile)) {
        const size_t idx = HandleToIndex(hFile);
        if (idx < kMaxBuffers) {
            AcquireSRWLockExclusive(&s_lock);
            auto& buf = s_buffers[idx];
            if (buf.inUse) {
                int64_t target = 0;
                int64_t move = lDistanceToMove;
                if (lpDistanceToMoveHigh) {
                    move |= (static_cast<int64_t>(*lpDistanceToMoveHigh) << 32);
                }

                if (dwMoveMethod == FILE_BEGIN) {
                    target = move;
                } else if (dwMoveMethod == FILE_CURRENT) {
                    target = static_cast<int64_t>(buf.position) + move;
                } else if (dwMoveMethod == FILE_END) {
                    target = static_cast<int64_t>(buf.data.size()) + move;
                }

                if (target < 0) target = 0;
                buf.position = static_cast<size_t>(target);

                if (lpDistanceToMoveHigh) {
                    *lpDistanceToMoveHigh = static_cast<LONG>(target >> 32);
                }
                const DWORD res = static_cast<DWORD>(target & 0xFFFFFFFF);
                ReleaseSRWLockExclusive(&s_lock);
                return res;
            }
            ReleaseSRWLockExclusive(&s_lock);
        }
    }

    return Real_SetFilePointer(hFile, lDistanceToMove, lpDistanceToMoveHigh, dwMoveMethod);
}

static BOOL __stdcall Hooked_SetEndOfFile(HANDLE hFile) {
    if (IsPseudoHandle(hFile)) {
        const size_t idx = HandleToIndex(hFile);
        if (idx < kMaxBuffers) {
            AcquireSRWLockExclusive(&s_lock);
            auto& buf = s_buffers[idx];
            if (buf.inUse && buf.isWrite) {
                buf.data.resize(buf.position);
                ReleaseSRWLockExclusive(&s_lock);
                return TRUE;
            }
            ReleaseSRWLockExclusive(&s_lock);
        }
    }
    return Real_SetEndOfFile(hFile);
}

static DWORD __stdcall Hooked_GetFileSize(HANDLE hFile, LPDWORD lpFileSizeHigh) {
    if (IsPseudoHandle(hFile)) {
        const size_t idx = HandleToIndex(hFile);
        if (idx < kMaxBuffers) {
            AcquireSRWLockShared(&s_lock);
            auto& buf = s_buffers[idx];
            if (buf.inUse) {
                if (lpFileSizeHigh) *lpFileSizeHigh = 0;
                const DWORD sz = static_cast<DWORD>(buf.data.size());
                ReleaseSRWLockShared(&s_lock);
                return sz;
            }
            ReleaseSRWLockShared(&s_lock);
        }
    }
    return Real_GetFileSize(hFile, lpFileSizeHigh);
}

static BOOL __stdcall Hooked_GetFileSizeEx(HANDLE hFile, PLARGE_INTEGER lpFileSize) {
    if (IsPseudoHandle(hFile)) {
        const size_t idx = HandleToIndex(hFile);
        if (idx < kMaxBuffers) {
            AcquireSRWLockShared(&s_lock);
            auto& buf = s_buffers[idx];
            if (buf.inUse && lpFileSize) {
                lpFileSize->QuadPart = static_cast<LONGLONG>(buf.data.size());
                ReleaseSRWLockShared(&s_lock);
                return TRUE;
            }
            ReleaseSRWLockShared(&s_lock);
        }
    }
    return Real_GetFileSizeEx(hFile, lpFileSize);
}

static DWORD __stdcall Hooked_GetFileType(HANDLE hFile) {
    if (IsPseudoHandle(hFile)) {
        return FILE_TYPE_DISK;
    }
    return Real_GetFileType(hFile);
}

static BOOL __stdcall Hooked_FlushFileBuffers(HANDLE hFile) {
    if (IsPseudoHandle(hFile)) {
        return TRUE;
    }
    return Real_FlushFileBuffers(hFile);
}

static BOOL __stdcall Hooked_CloseHandle(HANDLE hObject) {
    if (IsPseudoHandle(hObject)) {
        const size_t idx = HandleToIndex(hObject);
        if (idx < kMaxBuffers) {
            AcquireSRWLockExclusive(&s_lock);
            auto& buf = s_buffers[idx];
            if (buf.inUse) {
                if (buf.isWrite && !buf.data.empty()) {
                    std::string path = buf.filePath;
                    const size_t lastSlash = path.find_last_of("/\\");
                    if (lastSlash != std::string::npos) {
                        const std::string dir = path.substr(0, lastSlash);
                        CreateDirectoryA(dir.c_str(), nullptr);
                    }

                    HANDLE hReal = Real_CreateFileA(
                        path.c_str(), GENERIC_WRITE, 0, nullptr,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (hReal != INVALID_HANDLE_VALUE) {
                        DWORD written = 0;
                        Real_WriteFile(hReal, buf.data.data(), static_cast<DWORD>(buf.data.size()), &written, nullptr);
                        Real_CloseHandle(hReal);
                    }
                }

                buf.inUse = false;
                buf.isWrite = false;
                buf.filePath.clear();
                buf.data.clear();
                buf.data.shrink_to_fit();
                buf.position = 0;
                ReleaseSRWLockExclusive(&s_lock);
                return TRUE;
            }
            ReleaseSRWLockExclusive(&s_lock);
        }
    }
    return Real_CloseHandle(hObject);
}

static bool s_fastStreamInitialized = false;

static void Initialize() {
    if (s_fastStreamInitialized) return;

    MH_CreateHook(&CreateFileA, reinterpret_cast<void*>(&Hooked_CreateFileA), reinterpret_cast<void**>(&Real_CreateFileA));
    MH_EnableHook(&CreateFileA);

    MH_CreateHook(&ReadFile, reinterpret_cast<void*>(&Hooked_ReadFile), reinterpret_cast<void**>(&Real_ReadFile));
    MH_EnableHook(&ReadFile);

    MH_CreateHook(&WriteFile, reinterpret_cast<void*>(&Hooked_WriteFile), reinterpret_cast<void**>(&Real_WriteFile));
    MH_EnableHook(&WriteFile);

    MH_CreateHook(&SetFilePointer, reinterpret_cast<void*>(&Hooked_SetFilePointer), reinterpret_cast<void**>(&Real_SetFilePointer));
    MH_EnableHook(&SetFilePointer);

    MH_CreateHook(&SetEndOfFile, reinterpret_cast<void*>(&Hooked_SetEndOfFile), reinterpret_cast<void**>(&Real_SetEndOfFile));
    MH_EnableHook(&SetEndOfFile);

    MH_CreateHook(&GetFileSize, reinterpret_cast<void*>(&Hooked_GetFileSize), reinterpret_cast<void**>(&Real_GetFileSize));
    MH_EnableHook(&GetFileSize);

    MH_CreateHook(&GetFileSizeEx, reinterpret_cast<void*>(&Hooked_GetFileSizeEx), reinterpret_cast<void**>(&Real_GetFileSizeEx));
    MH_EnableHook(&GetFileSizeEx);

    MH_CreateHook(&GetFileType, reinterpret_cast<void*>(&Hooked_GetFileType), reinterpret_cast<void**>(&Real_GetFileType));
    MH_EnableHook(&GetFileType);

    MH_CreateHook(&FlushFileBuffers, reinterpret_cast<void*>(&Hooked_FlushFileBuffers), reinterpret_cast<void**>(&Real_FlushFileBuffers));
    MH_EnableHook(&FlushFileBuffers);

    MH_CreateHook(&CloseHandle, reinterpret_cast<void*>(&Hooked_CloseHandle), reinterpret_cast<void**>(&Real_CloseHandle));
    MH_EnableHook(&CloseHandle);

    s_fastStreamInitialized = true;
    Log("G3O", "[InfoManager] Fast File Stream buffering active for compiled binary archives.");
}

static void Shutdown() {
    if (!s_fastStreamInitialized) return;

    MH_DisableHook(&CreateFileA);
    MH_DisableHook(&ReadFile);
    MH_DisableHook(&WriteFile);
    MH_DisableHook(&SetFilePointer);
    MH_DisableHook(&SetEndOfFile);
    MH_DisableHook(&GetFileSize);
    MH_DisableHook(&GetFileSizeEx);
    MH_DisableHook(&GetFileType);
    MH_DisableHook(&FlushFileBuffers);
    MH_DisableHook(&CloseHandle);

    s_fastStreamInitialized = false;
}

} // namespace FastFileStream

using FnVfsGetInstance        = void* (__cdecl*)();
using FnVfsAddDirToCache      = unsigned int (__thiscall*)(void* pVfs, const bCString* pDir, unsigned int* pOutCount);
using FnCStringCtorFromPChar  = bCString* (__thiscall*)(bCString* pThis, const char* pText);
using FnCStringDtor           = void (__thiscall*)(bCString* pThis);

static FnVfsGetInstance        s_vfsGetInstance        = nullptr;
static FnVfsAddDirToCache      s_vfsAddDirToCache      = nullptr;
static FnCStringCtorFromPChar  s_cStringCtor           = nullptr;
static FnCStringDtor           s_cStringDtor           = nullptr;

static uint8_t s_originalPatchBytes[2] = { 0x75, 0x2A };
static void*   s_patchAddress = nullptr;
static bool    s_bPatchApplied = false;

using FnReadCompiledInfoManager = bool (__thiscall*)(void* pThis, const bCString* pFileName);
static FnReadCompiledInfoManager Real_ReadCompiledInfoManager = nullptr;
static void* s_targetReadCompiledInfoManager = nullptr;

using FnWriteCompiledInfoManager = bool (__thiscall*)(void* pThis, const bCString* pFileName);
static FnWriteCompiledInfoManager Real_WriteCompiledInfoManager = nullptr;
static void* s_targetWriteCompiledInfoManager = nullptr;

static void* s_targetInfoMapClear = nullptr;

struct FileMetaRecord {
    std::string name;
    DWORD sizeLow = 0;
    DWORD sizeHigh = 0;
    DWORD timeLow = 0;
    DWORD timeHigh = 0;
};

// Computes 64-bit FNV-1a fingerprint over all Data\Infos.* archives and loose files + world name
static uint64_t ComputeInfosArchiveFingerprint(const char* worldName) {
    uint64_t hash = 14695981039346656037ull; // FNV-1a offset basis
    auto hashBytes = [&](const void* data, size_t len) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < len; ++i) {
            hash ^= p[i];
            hash *= 1099511628211ull; // FNV-1a prime
        }
    };

    if (worldName && worldName[0]) {
        hashBytes(worldName, std::strlen(worldName));
    }

    std::vector<FileMetaRecord> records;

    // Scan Data\Infos.* archives (e.g. Data\Infos.pak, Data\Infos.cpt)
    WIN32_FIND_DATAA wfd;
    HANDLE hFind = FindFirstFileA("Data\\Infos.*", &wfd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (!(wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                FileMetaRecord rec;
                rec.name = wfd.cFileName;
                for (char& c : rec.name) {
                    c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
                }
                rec.sizeLow = wfd.nFileSizeLow;
                rec.sizeHigh = wfd.nFileSizeHigh;
                rec.timeLow = wfd.ftLastWriteTime.dwLowDateTime;
                rec.timeHigh = wfd.ftLastWriteTime.dwHighDateTime;
                records.push_back(std::move(rec));
            }
        } while (FindNextFileA(hFind, &wfd));
        FindClose(hFind);
    }

    // Also scan loose files in Data\Infos\*.* (excluding compiled cache files starting with '_')
    hFind = FindFirstFileA("Data\\Infos\\*.*", &wfd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (!(wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                if (wfd.cFileName[0] != '_') { // ignore our cache and meta files
                    FileMetaRecord rec;
                    rec.name = wfd.cFileName;
                    for (char& c : rec.name) {
                        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
                    }
                    rec.sizeLow = wfd.nFileSizeLow;
                    rec.sizeHigh = wfd.nFileSizeHigh;
                    rec.timeLow = wfd.ftLastWriteTime.dwLowDateTime;
                    rec.timeHigh = wfd.ftLastWriteTime.dwHighDateTime;
                    records.push_back(std::move(rec));
                }
            }
        } while (FindNextFileA(hFind, &wfd));
        FindClose(hFind);
    }

    // Sort deterministically by filename
    std::sort(records.begin(), records.end(), [](const FileMetaRecord& a, const FileMetaRecord& b) {
        return a.name < b.name;
    });

    for (const auto& r : records) {
        hashBytes(r.name.data(), r.name.size());
        hashBytes(&r.sizeLow, sizeof(r.sizeLow));
        hashBytes(&r.sizeHigh, sizeof(r.sizeHigh));
        hashBytes(&r.timeLow, sizeof(r.timeLow));
        hashBytes(&r.timeHigh, sizeof(r.timeHigh));
    }

    return hash;
}

static void RefreshVfsInfosDirectory() {
    if (s_vfsGetInstance && s_vfsAddDirToCache && s_cStringCtor && s_cStringDtor) {
        void* pVfs = s_vfsGetInstance();
        if (pVfs) {
            bCString dir{};
            s_cStringCtor(&dir, "Data/Infos");
            unsigned int count = 0;
            s_vfsAddDirToCache(pVfs, &dir, &count);
            s_cStringDtor(&dir);
        }
    }
}

static void ApplyCompiledFolderPatch(uintptr_t gameBase) {
    uint8_t* pTarget = reinterpret_cast<uint8_t*>(gameBase + 0x1CF41C);
    s_patchAddress = pTarget;
    if (pTarget[0] == 0x75 && pTarget[1] == 0x2A) {
        DWORD oldProtect = 0;
        if (VirtualProtect(pTarget, 2, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            pTarget[0] = 0x90;
            pTarget[1] = 0x90;
            VirtualProtect(pTarget, 2, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), pTarget, 2);
            s_bPatchApplied = true;
            Log("G3O", "[InfoManager] Memory-patched Game.dll+0x1CF41C (75 2A -> 90 90, ReadCompiledInfoManager branch enabled).");
        } else {
            Log("G3O", "[InfoManager] Warning: VirtualProtect failed at Game.dll+0x1CF41C.");
        }
    } else if (pTarget[0] == 0x90 && pTarget[1] == 0x90) {
        s_bPatchApplied = true;
        Log("G3O", "[InfoManager] Game.dll+0x1CF41C is already patched with NOPs.");
    } else {
        Log("G3O", "[InfoManager] Warning: Unexpected bytes at Game.dll+0x1CF41C: %02X %02X (expected 75 2A).", pTarget[0], pTarget[1]);
    }
}

static void RevertCompiledFolderPatch() {
    if (s_bPatchApplied && s_patchAddress) {
        uint8_t* pTarget = reinterpret_cast<uint8_t*>(s_patchAddress);
        DWORD oldProtect = 0;
        if (VirtualProtect(pTarget, 2, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            pTarget[0] = s_originalPatchBytes[0];
            pTarget[1] = s_originalPatchBytes[1];
            VirtualProtect(pTarget, 2, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), pTarget, 2);
            s_bPatchApplied = false;
        }
    }
}

static bool __fastcall Hooked_ReadCompiledInfoManager(
    void* pThis, void* /*edx*/, const bCString* pFileName)
{
    if (!pFileName || !pFileName->m_pcText) {
        return false;
    }

    const std::string baseName = pFileName->m_pcText; // e.g. "compiledinfos_G3_World_01.bin"

    // Extract world name, e.g. "G3_World_01"
    std::string worldName = "Default";
    const size_t prefixPos = baseName.find("compiledinfos_");
    if (prefixPos != std::string::npos) {
        const size_t start = prefixPos + std::strlen("compiledinfos_");
        const size_t dotPos = baseName.rfind('.');
        if (dotPos != std::string::npos && dotPos > start) {
            worldName = baseName.substr(start, dotPos - start);
        }
    }

    std::string vfsName;
    if (baseName.rfind("compiledinfos_", 0) == 0) {
        vfsName = "_" + baseName; // "_compiledinfos_G3_World_01.bin"
    } else {
        vfsName = baseName;
    }

    const std::string diskPath = "Data\\Infos\\" + vfsName;
    const std::string metaPath = "Data\\Infos\\" + vfsName + ".meta";

    const uint64_t currentFingerprint = ComputeInfosArchiveFingerprint(worldName.c_str());

    bool needGenerate = false;
    const DWORD binAttr = GetFileAttributesA(diskPath.c_str());
    if (binAttr == INVALID_FILE_ATTRIBUTES) {
        Log("G3O", "[InfoManager] Binary cache '%s' does not exist on disk.", diskPath.c_str());
        needGenerate = true;
    } else {
        // Check meta file
        uint64_t savedFingerprint = 0;
        FILE* fMeta = nullptr;
        const errno_t err = fopen_s(&fMeta, metaPath.c_str(), "rb");
        if (err == 0 && fMeta) {
            fread(&savedFingerprint, sizeof(savedFingerprint), 1, fMeta);
            fclose(fMeta);
        }

        if (savedFingerprint != currentFingerprint || savedFingerprint == 0) {
            Log("G3O", "[InfoManager] Binary cache '%s' is stale or missing meta (saved=0x%llX, current=0x%llX). Deleting stale cache...",
                diskPath.c_str(), savedFingerprint, currentFingerprint);
            DeleteFileA(diskPath.c_str());
            DeleteFileA(metaPath.c_str());
            needGenerate = true;
        }
    }

    if (needGenerate) {
        Log("G3O", "[InfoManager] Cache miss for '%s'; falling back to engine INI loader to regenerate binary cache.",
            diskPath.c_str());
        return false;
    }

    // Ensure VFS has indexed Data/Infos
    RefreshVfsInfosDirectory();

    // Construct bCString with the VFS name "_compiledinfos_G3_World_01.bin"
    bCString vfsCString{};
    if (s_cStringCtor) {
        s_cStringCtor(&vfsCString, vfsName.c_str());
    } else {
        vfsCString.m_pcText = vfsName.c_str();
    }

    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    const bool success = Real_ReadCompiledInfoManager(pThis, &vfsCString);
    QueryPerformanceCounter(&t1);

    if (s_cStringDtor) {
        s_cStringDtor(&vfsCString);
    }

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    const double ms = (freq.QuadPart > 0)
        ? static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart)
        : 0.0;

    if (success) {
        Log("G3O", "[InfoManager] SUCCESS: ReadCompiledInfoManager loaded '%s' in %.2f ms! (LoadInfosFromInis skipped)",
            vfsName.c_str(), ms);
        return true;
    } else {
        Log("G3O", "[InfoManager] WARNING: ReadCompiledInfoManager failed to load '%s' (%.2f ms). Falling back to LoadInfosFromInis.",
            vfsName.c_str(), ms);
        return false;
    }
}

static bool __fastcall Hooked_WriteCompiledInfoManager(
    void* pThis, void* /*edx*/, const bCString* pFileName)
{
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    const bool result = Real_WriteCompiledInfoManager(pThis, pFileName);
    QueryPerformanceCounter(&t1);

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    const double ms = (freq.QuadPart > 0)
        ? static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart)
        : 0.0;

    if (result && pFileName && pFileName->m_pcText) {
        const std::string writtenPath = pFileName->m_pcText; // e.g. "Data/Infos/_compiledinfos_G3_World_01.bin"
        std::string metaPath = writtenPath + ".meta";
        for (char& c : metaPath) {
            if (c == '/') c = '\\';
        }

        std::string baseName;
        const size_t lastSlash = writtenPath.find_last_of("/\\");
        if (lastSlash != std::string::npos) {
            baseName = writtenPath.substr(lastSlash + 1);
        } else {
            baseName = writtenPath;
        }

        std::string worldName = "Default";
        const size_t prefixPos = baseName.find("compiledinfos_");
        if (prefixPos != std::string::npos) {
            const size_t start = prefixPos + std::strlen("compiledinfos_");
            const size_t dotPos = baseName.rfind('.');
            if (dotPos != std::string::npos && dotPos > start) {
                worldName = baseName.substr(start, dotPos - start);
            }
        }

        const uint64_t currentFingerprint = ComputeInfosArchiveFingerprint(worldName.c_str());
        FILE* fMeta = nullptr;
        const errno_t err = fopen_s(&fMeta, metaPath.c_str(), "wb");
        if (err == 0 && fMeta) {
            fwrite(&currentFingerprint, sizeof(currentFingerprint), 1, fMeta);
            fclose(fMeta);
        }

        // Re-index Data/Infos in VFS so that QuickLoad in the same session works immediately
        RefreshVfsInfosDirectory();

        Log("G3O", "[InfoManager] SUCCESS: WriteCompiledInfoManager generated '%s' in %.2f ms (fingerprint=0x%llX)!",
            writtenPath.c_str(), ms, currentFingerprint);
    } else {
        Log("G3O", "[InfoManager] WARNING: WriteCompiledInfoManager failed (%.2f ms).", ms);
    }

    return result;
}

// ============================================================================
// 7. Public API & Telemetry
// ============================================================================
uint64_t InfoManagerOptimizer_GetHits() noexcept { return CInfoShadowCache::GetHits(); }
uint64_t InfoManagerOptimizer_GetMisses() noexcept { return CInfoShadowCache::GetMisses(); }
uint32_t InfoManagerOptimizer_GetRebuilds() noexcept { return CInfoShadowCache::GetRebuilds(); }
uint32_t InfoManagerOptimizer_GetIndexedCount() noexcept { return CInfoShadowCache::GetIndexedCount(); }
uint32_t InfoManagerOptimizer_GetBucketExpansions() noexcept {
    return s_loadInfosStats.expanded.load(std::memory_order_relaxed) +
           s_initStats.expanded.load(std::memory_order_relaxed) +
           s_clearStats.expanded.load(std::memory_order_relaxed);
}

static void LogOneMapResizeStats(const char* name, const MapResizeStats& stats) {
    Log("G3O", "  - %-14s: %u calls, %u matches, %u expanded, %u reallocAttempts, %u failures, %u allocUnavail, %u alreadyLarge (buckets: %u -> %u, mismatchRVA=0x%X)",
        name,
        stats.calls.load(std::memory_order_relaxed),
        stats.matches.load(std::memory_order_relaxed),
        stats.expanded.load(std::memory_order_relaxed),
        stats.reallocAttempts.load(std::memory_order_relaxed),
        stats.reallocFailures.load(std::memory_order_relaxed),
        stats.allocatorUnavailable.load(std::memory_order_relaxed),
        stats.alreadyLarge.load(std::memory_order_relaxed),
        stats.bucketsBefore.load(std::memory_order_relaxed),
        stats.bucketsAfter.load(std::memory_order_relaxed),
        static_cast<unsigned int>(stats.firstMismatchedCallerRva.load(std::memory_order_relaxed)));
}

void InfoManagerOptimizer_LogStats() noexcept {
    Log("G3O", "[InfoManager] Dynamic Bucket Expansion Diagnostics:");
    LogOneMapResizeStats("LoadInfos", s_loadInfosStats);
    LogOneMapResizeStats("InitHashTable", s_initStats);
    LogOneMapResizeStats("Clear", s_clearStats);
}

void InitializeInfoManagerOptimizer() {
    if (s_bInitialized) return;

    if (!GetConfig().enableInfoManagerOptimizer) {
        Log("G3O", "Info Manager Optimizer is disabled by configuration.");
        return;
    }

    HMODULE hGame = GetModuleHandleW(L"Game.dll");
    if (!hGame) {
        Log("G3O", "Info Manager Optimizer: Game.dll not loaded.");
        return;
    }

    HMODULE hEngine = GetModuleHandleW(L"Engine.dll");
    HMODULE hSharedBase = GetModuleHandleW(L"SharedBase.dll");

    // Resolve bCMemoryAdmin allocator and hash function from SharedBase.dll
    if (hSharedBase) {
        s_memAdminGetInstance = reinterpret_cast<FnMemAdminGetInstance>(
            GetProcAddress(hSharedBase, "?GetInstance@bCMemoryAdmin@@SGAAV1@XZ"));
        s_memAdminRealloc = reinterpret_cast<FnMemAdminRealloc>(
            GetProcAddress(hSharedBase, "?Realloc@bCMemoryAdmin@@QAEPAXPAXK@Z"));
        s_getHashValueCString = reinterpret_cast<FnGetHashValueCString>(
            GetProcAddress(hSharedBase, "??$g_GetHashValue@VbCString@@@@YGKABVbCString@@@Z"));
        s_cStringCtor = reinterpret_cast<FnCStringCtorFromPChar>(
            GetProcAddress(hSharedBase, "??0bCString@@QAE@PBD@Z"));
        s_cStringDtor = reinterpret_cast<FnCStringDtor>(
            GetProcAddress(hSharedBase, "??1bCString@@QAE@XZ"));
    }

    if (hEngine) {
        s_vfsGetInstance = reinterpret_cast<FnVfsGetInstance>(
            GetProcAddress(hEngine, "?GetInstance@eCVirtualFileSystem@@SGAAV1@XZ"));
        s_vfsAddDirToCache = reinterpret_cast<FnVfsAddDirToCache>(
            GetProcAddress(hEngine, "?AddDirToCache@eCVirtualFileSystem@@QAEIABVbCString@@AAI@Z"));
    }

    const auto baseAddr = reinterpret_cast<uintptr_t>(hGame);
    s_gameBase = baseAddr;
    s_expectedInitReturn  = baseAddr + 0x1CF26A;
    s_expectedClearReturn = baseAddr + 0x1CFDB9;

    // Binary Cache optimization: fast buffered file stream, folder check patch, and Read/Write hooks
    if (GetConfig().enableInfoManagerBinaryCache) {
        FastFileStream::Initialize();
        ApplyCompiledFolderPatch(baseAddr);

        s_targetReadCompiledInfoManager = reinterpret_cast<void*>(baseAddr + 0x1CFC30);
        const MH_STATUS readCompStatus = MH_CreateHook(
            s_targetReadCompiledInfoManager,
            reinterpret_cast<void*>(&Hooked_ReadCompiledInfoManager),
            reinterpret_cast<void**>(&Real_ReadCompiledInfoManager));
        if (readCompStatus == MH_OK) {
            MH_EnableHook(s_targetReadCompiledInfoManager);
            Log("G3O", "[InfoManager] Successfully hooked ReadCompiledInfoManager at Game.dll+0x1CFC30.");
        } else {
            Log("G3O", "[InfoManager] Warning: Failed to hook ReadCompiledInfoManager (MH_STATUS=%d).", readCompStatus);
        }

        s_targetWriteCompiledInfoManager = reinterpret_cast<void*>(baseAddr + 0x1CFB60);
        const MH_STATUS writeCompStatus = MH_CreateHook(
            s_targetWriteCompiledInfoManager,
            reinterpret_cast<void*>(&Hooked_WriteCompiledInfoManager),
            reinterpret_cast<void**>(&Real_WriteCompiledInfoManager));
        if (writeCompStatus == MH_OK) {
            MH_EnableHook(s_targetWriteCompiledInfoManager);
            Log("G3O", "[InfoManager] Successfully hooked WriteCompiledInfoManager at Game.dll+0x1CFB60.");
        } else {
            Log("G3O", "[InfoManager] Warning: Failed to hook WriteCompiledInfoManager (MH_STATUS=%d).", writeCompStatus);
        }

        // Hook bTValMap::Clear (Game.dll+0x0061DB0) - Fallback path for ReadCompiledInfoManager (return: 0x1CFDB9)
        s_targetInfoMapClear = reinterpret_cast<void*>(baseAddr + 0x0061DB0);
        const MH_STATUS clearHookStatus = MH_CreateHook(
            s_targetInfoMapClear,
            reinterpret_cast<void*>(&HookInfoMapClearThunk),
            &s_realInfoMapClear);

        if (clearHookStatus == MH_OK) {
            MH_EnableHook(s_targetInfoMapClear);
            Log("G3O", "[InfoManager] Successfully hooked bTValMap::Clear at Game.dll+0x0061DB0 (Fallback path active).");
        } else {
            Log("G3O", "[InfoManager] Warning: Failed to hook bTValMap::Clear at Game.dll+0x0061DB0 (MH_STATUS=%d).", clearHookStatus);
        }
    }

    // 1. Hook gCInfoManager_PS::GetInfo (Game.dll+0x1CE270)
    s_targetGetInfo = reinterpret_cast<void*>(baseAddr + 0x1CE270);
    const MH_STATUS getInfoStatus = MH_CreateHook(
        s_targetGetInfo,
        reinterpret_cast<void*>(&Hooked_GetInfo),
        reinterpret_cast<void**>(&Real_GetInfo));

    if (getInfoStatus == MH_OK) {
        MH_EnableHook(s_targetGetInfo);
        Log("G3O", "[InfoManager] Successfully hooked gCInfoManager_PS::GetInfo at Game.dll+0x1CE270 (O(1) Shadow Table active).");
    } else {
        Log("G3O", "[InfoManager] Failed to create hook on Game.dll+0x1CE270 (MH_STATUS=%d).", getInfoStatus);
    }

    // 2. Hook bTValMap::InitHashTable (Game.dll+0x005E270) - Primary path for LoadInfosFromInis (return: 0x1CF26A)
    s_targetInitHashTable = reinterpret_cast<void*>(baseAddr + 0x005E270);
    const MH_STATUS initHookStatus = MH_CreateHook(
        s_targetInitHashTable,
        reinterpret_cast<void*>(&HookInitHashTableThunk),
        &s_realInitHashTable);

    if (initHookStatus == MH_OK) {
        MH_EnableHook(s_targetInitHashTable);
        Log("G3O", "[InfoManager] Successfully hooked bTValMap::InitHashTable at Game.dll+0x005E270 (Primary path active).");
    } else {
        Log("G3O", "[InfoManager] Warning: Failed to hook InitHashTable at Game.dll+0x005E270 (MH_STATUS=%d).", initHookStatus);
    }

    // 3. Micro-timing probes for bTPropertyObject<gCInfoManager_PS>::Read (0x1D0240)
    s_expectedPhaseBReturn = baseAddr + 0x1D0482;
    s_expectedPhaseCReturn = baseAddr + 0x1D048D;

    s_targetPropertyObjectRead = reinterpret_cast<void*>(baseAddr + 0x1D0240);
    const MH_STATUS propReadStatus = MH_CreateHook(
        s_targetPropertyObjectRead,
        reinterpret_cast<void*>(&Hooked_InfoManagerPropertyObjectRead),
        reinterpret_cast<void**>(&Real_PropertyObjectRead));
    if (propReadStatus == MH_OK) {
        MH_EnableHook(s_targetPropertyObjectRead);
        Log("G3O", "[InfoManager] Successfully hooked 0x1D0240 (bTPropertyObject<gCInfoManager_PS>::Read probe active).");
    } else {
        Log("G3O", "[InfoManager] Warning: Failed to hook 0x1D0240 (MH_STATUS=%d).", propReadStatus);
    }

    s_targetInfoManagerCreate = reinterpret_cast<void*>(baseAddr + 0x1D0090);
    const MH_STATUS createStatus = MH_CreateHook(
        s_targetInfoManagerCreate,
        reinterpret_cast<void*>(&Hooked_InfoManagerCreate),
        reinterpret_cast<void**>(&Real_InfoManagerCreate));
    if (createStatus == MH_OK) {
        MH_EnableHook(s_targetInfoManagerCreate);
        Log("G3O", "[InfoManager] Successfully hooked 0x1D0090 (Phase B probe active).");
    } else {
        Log("G3O", "[InfoManager] Warning: Failed to hook 0x1D0090 (MH_STATUS=%d).", createStatus);
    }

    if (hSharedBase) {
        s_targetStreamReadObjectBase = reinterpret_cast<void*>(
            GetProcAddress(hSharedBase, "??5@YGAAVbCIStream@@AAV0@AAVbCObjectBase@@@Z"));
        if (s_targetStreamReadObjectBase) {
            const MH_STATUS opStatus = MH_CreateHook(
                s_targetStreamReadObjectBase,
                reinterpret_cast<void*>(&Hooked_StreamReadObjectBase),
                reinterpret_cast<void**>(&Real_StreamReadObjectBase));
            if (opStatus == MH_OK) {
                MH_EnableHook(s_targetStreamReadObjectBase);
                Log("G3O", "[InfoManager] Successfully hooked operator>> at %p (Phase C probe active).", s_targetStreamReadObjectBase);
            } else {
                Log("G3O", "[InfoManager] Warning: Failed to hook operator>> (MH_STATUS=%d).", opStatus);
            }
        }
    }

    void* pLoadInfos = reinterpret_cast<void*>(
        GetProcAddress(hGame, "?LoadInfosFromInis@gCInfoManager_PS@@QAE_NXZ"));
    if (!pLoadInfos) {
        pLoadInfos = reinterpret_cast<void*>(baseAddr + 0x1CF160);
    }
    s_targetLoadInfosFromInis = pLoadInfos;
    if (s_targetLoadInfosFromInis) {
        const MH_STATUS loadStatus = MH_CreateHook(
            s_targetLoadInfosFromInis,
            reinterpret_cast<void*>(&Hooked_LoadInfosFromInis),
            reinterpret_cast<void**>(&Real_LoadInfosFromInis));
        if (loadStatus == MH_OK) {
            MH_EnableHook(s_targetLoadInfosFromInis);
            Log("G3O", "[InfoManager] Successfully hooked LoadInfosFromInis at %p.", s_targetLoadInfosFromInis);
        } else {
            Log("G3O", "[InfoManager] Warning: Failed to hook LoadInfosFromInis (MH_STATUS=%d).", loadStatus);
        }
    }

    s_bInitialized = true;
}

void ShutdownInfoManagerOptimizer() {
    if (!s_bInitialized) return;

    if (s_targetGetInfo) {
        MH_DisableHook(s_targetGetInfo);
        s_targetGetInfo = nullptr;
    }
    Real_GetInfo = nullptr;

    if (s_targetInitHashTable) {
        MH_DisableHook(s_targetInitHashTable);
        s_targetInitHashTable = nullptr;
    }
    s_realInitHashTable = nullptr;

    if (s_targetInfoMapClear) {
        MH_DisableHook(s_targetInfoMapClear);
        s_targetInfoMapClear = nullptr;
    }
    s_realInfoMapClear = nullptr;

    if (s_targetPropertyObjectRead) {
        MH_DisableHook(s_targetPropertyObjectRead);
        s_targetPropertyObjectRead = nullptr;
    }
    Real_PropertyObjectRead = nullptr;

    if (s_targetInfoManagerCreate) {
        MH_DisableHook(s_targetInfoManagerCreate);
        s_targetInfoManagerCreate = nullptr;
    }
    Real_InfoManagerCreate = nullptr;

    if (s_targetStreamReadObjectBase) {
        MH_DisableHook(s_targetStreamReadObjectBase);
        s_targetStreamReadObjectBase = nullptr;
    }
    Real_StreamReadObjectBase = nullptr;

    if (s_targetLoadInfosFromInis) {
        MH_DisableHook(s_targetLoadInfosFromInis);
        s_targetLoadInfosFromInis = nullptr;
    }
    Real_LoadInfosFromInis = nullptr;

    if (s_targetReadCompiledInfoManager) {
        MH_DisableHook(s_targetReadCompiledInfoManager);
        s_targetReadCompiledInfoManager = nullptr;
    }
    Real_ReadCompiledInfoManager = nullptr;

    if (s_targetWriteCompiledInfoManager) {
        MH_DisableHook(s_targetWriteCompiledInfoManager);
        s_targetWriteCompiledInfoManager = nullptr;
    }
    Real_WriteCompiledInfoManager = nullptr;

    RevertCompiledFolderPatch();
    FastFileStream::Shutdown();

    Log("G3O", "[InfoManager] Shutdown complete (Hits: %llu, Misses: %llu, Rebuilds: %u, Cached: %u, Expansions: %u)",
        InfoManagerOptimizer_GetHits(),
        InfoManagerOptimizer_GetMisses(),
        InfoManagerOptimizer_GetRebuilds(),
        InfoManagerOptimizer_GetIndexedCount(),
        InfoManagerOptimizer_GetBucketExpansions());

    InfoManagerOptimizer_LogStats();

    CInfoShadowCache::Invalidate();
    s_bInitialized = false;
}

} // namespace G3O
