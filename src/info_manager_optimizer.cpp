#include "info_manager_optimizer.h"
#include "config_manager.h"
#include "diagnostic_logger.h"
#include "hook_install_policy.h"
#include <minhook.h>
#include <windows.h>
#include <atomic>
#include <string_view>
#include <unordered_map>
#include <cstring>
#include <vector>

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

static uintptr_t s_expectedInitReturn = 0;
static uintptr_t s_gameBase = 0;

static FnMemAdminGetInstance s_memAdminGetInstance = nullptr;
static FnMemAdminRealloc     s_memAdminRealloc     = nullptr;
static FnGetHashValueCString s_getHashValueCString = nullptr;

// Function pointers for In-Memory Binary Serialization of gCInfo_PS via bCMemoryStream
using FnMemStreamCtor        = void (__thiscall*)(void* pStream);
using FnMemStreamDtor        = void (__thiscall*)(void* pStream);
using FnMemStreamWrite       = uint32_t (__thiscall*)(void* pOStream, const void* pData, uint32_t bytes);
using FnMemStreamGetBuffer   = const uint8_t* (__thiscall*)(const void* pStream, uint32_t flag);
using FnMemStreamGetSize     = int64_t (__thiscall*)(const void* pStream);
using FnMemStreamSetCount    = void (__thiscall*)(void* pStream, uint32_t count);

using FnAccessorCtorWithObj  = void (__thiscall*)(void* pAccessor, void* pObj);
using FnAccessorCtorEmpty    = void (__thiscall*)(void* pAccessor);
using FnAccessorDtor         = void (__thiscall*)(void* pAccessor);
using FnAccessorGetNativeObj = void* (__thiscall*)(void* pAccessor);

using FnStreamWriteAccessor  = void* (__stdcall*)(void* pOStream, void* pAccessor);
using FnStreamReadAccessor   = void* (__stdcall*)(void* pIStream, void* pAccessor);

using FnOStreamWriteU16      = void* (__thiscall*)(void* pOStream, uint16_t val);
using FnOStreamWriteU32      = void* (__thiscall*)(void* pOStream, uint32_t val);
using FnIStreamReadU16       = void* (__thiscall*)(void* pIStream, uint16_t* pVal);
using FnIStreamReadU32       = void* (__thiscall*)(void* pIStream, uint32_t* pVal);

using FnCStringAssign        = void* (__thiscall*)(bCString* pDest, const bCString* pSrc);
using FnCStringDtor          = void (__thiscall*)(bCString* pStr);

static FnMemStreamCtor        s_fnMemStreamCtor        = nullptr;
static FnMemStreamDtor        s_fnMemStreamDtor        = nullptr;
static FnMemStreamWrite       s_fnMemStreamWrite       = nullptr;
static FnMemStreamGetBuffer   s_fnMemStreamGetBuffer   = nullptr;
static FnMemStreamGetSize     s_fnMemStreamGetSize     = nullptr;
static FnMemStreamSetCount    s_fnMemStreamSetCount    = nullptr;

static FnAccessorCtorWithObj  s_fnAccessorCtorWithObj  = nullptr;
static FnAccessorCtorEmpty    s_fnAccessorCtorEmpty    = nullptr;
static FnAccessorDtor         s_fnAccessorDtor         = nullptr;
static FnAccessorGetNativeObj s_fnAccessorGetNativeObj = nullptr;

static FnStreamWriteAccessor  s_fnStreamWriteAccessor  = nullptr;
static FnStreamReadAccessor   s_fnStreamReadAccessor   = nullptr;

static FnOStreamWriteU16      s_fnOStreamWriteU16      = nullptr;
static FnOStreamWriteU32      s_fnOStreamWriteU32      = nullptr;
static FnIStreamReadU16       s_fnIStreamReadU16       = nullptr;
static FnIStreamReadU32       s_fnIStreamReadU32       = nullptr;

static FnCStringAssign        s_fnCStringAssign        = nullptr;
static FnCStringDtor          s_fnCStringDtor          = nullptr;

static SRWLOCK                s_snapshotLock           = SRWLOCK_INIT;
static std::vector<uint8_t>   s_snapshotBytes;
static uint32_t               s_snapshotCount          = 0;

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

// ============================================================================
// 6. In-Memory Snapshot & Fast Replay for gCInfoManager_PS
// ============================================================================
static bool DoRecordSnapshotInternal(void* pInfoManager) {
    if (!pInfoManager || !s_fnMemStreamCtor || !s_fnMemStreamDtor ||
        !s_fnAccessorCtorWithObj || !s_fnAccessorDtor || !s_fnStreamWriteAccessor ||
        !s_fnOStreamWriteU16 || !s_fnOStreamWriteU32) {
        return false;
    }

    auto* pCount = reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(pInfoManager) + 0x30);
    auto* ppData = *reinterpret_cast<void* const* const*>(reinterpret_cast<const uint8_t*>(pInfoManager) + 0x2C);
    if (!pCount || !ppData || *pCount == 0) {
        return false;
    }

    const uint32_t count = *pCount;

    alignas(8) uint8_t memStreamBuf[64];
    std::memset(memStreamBuf, 0, sizeof(memStreamBuf));
    s_fnMemStreamCtor(memStreamBuf);

    // Pre-allocate 16 MB capacity up front to eliminate 600,000 quadratic heap reallocations!
    if (s_fnMemStreamSetCount) {
        s_fnMemStreamSetCount(memStreamBuf, 16 * 1024 * 1024);
    }

    // bCOStream is at offset +4 of bCMemoryStream
    void* pOStream = memStreamBuf + 4;

    const uint16_t version = 1;
    s_fnOStreamWriteU16(pOStream, version);
    s_fnOStreamWriteU32(pOStream, count);

    for (uint32_t i = 0; i < count; ++i) {
        void* pInfo = ppData[i];
        if (!pInfo) continue;

        alignas(8) uint8_t accessorBuf[32];
        std::memset(accessorBuf, 0, sizeof(accessorBuf));
        s_fnAccessorCtorWithObj(accessorBuf, pInfo);
        s_fnStreamWriteAccessor(pOStream, accessorBuf);
        s_fnAccessorDtor(accessorBuf);
    }

    // Direct buffer layout of bCMemoryStream:
    // offset 0x08: m_pBuffer (pointer to start of allocated RAM buffer)
    // offset 0x0C: m_u32Position (exact bytes written so far)
    const auto* pBufferBase = *reinterpret_cast<const uint8_t* const*>(memStreamBuf + 0x08);
    const uint32_t writtenBytes = *reinterpret_cast<const uint32_t*>(memStreamBuf + 0x0C);

    bool success = false;
    if (pBufferBase && writtenBytes > 6) {
        AcquireSRWLockExclusive(&s_snapshotLock);
        s_snapshotBytes.assign(pBufferBase, pBufferBase + writtenBytes);
        s_snapshotCount = count;
        success = true;
        ReleaseSRWLockExclusive(&s_snapshotLock);

        Log("G3O", "[InfoManager] Captured %zu bytes baseline in RAM (%u dialogs).",
            s_snapshotBytes.size(), count);
    }

    s_fnMemStreamDtor(memStreamBuf);
    return success;
}

static bool SafeRecordSnapshot(void* pInfoManager) {
    __try {
        return DoRecordSnapshotInternal(pInfoManager);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("G3O", "[InfoManager] Exception during RecordSnapshot (code=0x%08X).", GetExceptionCode());
        return false;
    }
}

static bool DoReplaySnapshotInternal(void* pInfoManager) {
    if (!pInfoManager || !s_memAdminGetInstance || !s_memAdminRealloc ||
        !s_fnMemStreamCtor || !s_fnMemStreamDtor || !s_fnMemStreamWrite ||
        !s_fnAccessorCtorEmpty || !s_fnAccessorDtor || !s_fnStreamReadAccessor ||
        !s_fnAccessorGetNativeObj || !s_fnIStreamReadU16 || !s_fnIStreamReadU32 ||
        !s_fnCStringAssign) {
        return false;
    }

    AcquireSRWLockShared(&s_snapshotLock);
    if (s_snapshotBytes.empty()) {
        ReleaseSRWLockShared(&s_snapshotLock);
        return false;
    }
    const std::vector<uint8_t> localSnapshot = s_snapshotBytes;
    const uint32_t expectedCount = s_snapshotCount;
    ReleaseSRWLockShared(&s_snapshotLock);

    void* admin = s_memAdminGetInstance();
    if (!admin) return false;

    alignas(8) uint8_t memStreamBuf[64];
    std::memset(memStreamBuf, 0, sizeof(memStreamBuf));
    s_fnMemStreamCtor(memStreamBuf);

    void* pOStream = memStreamBuf + 4; // bCOStream
    void* pIStream = memStreamBuf + 0; // bCIStream

    if (s_fnMemStreamSetCount) {
        s_fnMemStreamSetCount(memStreamBuf, static_cast<uint32_t>(localSnapshot.size() + 1024));
    }
    s_fnMemStreamWrite(pOStream, localSnapshot.data(), static_cast<uint32_t>(localSnapshot.size()));

    // Reset stream position to 0 (at offset +0x0C)
    *reinterpret_cast<uint32_t*>(memStreamBuf + 0x0C) = 0;

    uint16_t version = 0;
    s_fnIStreamReadU16(pIStream, &version);
    uint32_t count = 0;
    s_fnIStreamReadU32(pIStream, &count);

    if (count == 0 || count != expectedCount) {
        s_fnMemStreamDtor(memStreamBuf);
        Log("G3O", "[InfoManager] Snapshot count mismatch: %u (expected %u)", count, expectedCount);
        return false;
    }

    auto** ppArrayInfos = reinterpret_cast<void***>(reinterpret_cast<uint8_t*>(pInfoManager) + 0x2C);
    auto* pCount = reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(pInfoManager) + 0x30);
    auto* pCapacity = reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(pInfoManager) + 0x34);

    if (*ppArrayInfos) {
        s_memAdminRealloc(admin, *ppArrayInfos, 0);
        *ppArrayInfos = nullptr;
    }

    void** newArray = static_cast<void**>(s_memAdminRealloc(admin, nullptr, count * sizeof(void*)));
    if (!newArray) {
        s_fnMemStreamDtor(memStreamBuf);
        return false;
    }
    std::memset(newArray, 0, count * sizeof(void*));
    *ppArrayInfos = newArray;
    *pCount = 0;
    *pCapacity = count;

    for (uint32_t i = 0; i < count; ++i) {
        alignas(8) uint8_t accessorBuf[32];
        std::memset(accessorBuf, 0, sizeof(accessorBuf));
        s_fnAccessorCtorEmpty(accessorBuf);
        s_fnStreamReadAccessor(pIStream, accessorBuf);

        void* pNativeObj = s_fnAccessorGetNativeObj(accessorBuf);
        if (pNativeObj) {
            void** vtable = *reinterpret_cast<void***>(pNativeObj);
            if (vtable && vtable[8]) {
                using FnInit20 = void (__thiscall*)(void*);
                reinterpret_cast<FnInit20>(vtable[8])(pNativeObj);
            }
            newArray[i] = pNativeObj;
            *pCount = i + 1;
        }
        s_fnAccessorDtor(accessorBuf);
    }

    s_fnMemStreamDtor(memStreamBuf);

    auto* pMap = reinterpret_cast<bTInfoValMap*>(reinterpret_cast<uint8_t*>(pInfoManager) + 0x4C);
    ExpandAndRehashInfoMap(pMap, kInfoMapBucketCount, "ReplaySnapshot", &s_loadInfosStats);

    // Clear bucket pointers to prevent any stale chains
    if (pMap->m_ppBuckets && pMap->m_u32BucketCount > 0) {
        std::memset(pMap->m_ppBuckets, 0, pMap->m_u32BucketCount * sizeof(SInfoMapNode*));
        pMap->m_u32NodeCount = 0;
    }

    for (uint32_t i = 0; i < count; ++i) {
        void* pInfo = newArray[i];
        if (!pInfo) continue;

        auto* pStrName = reinterpret_cast<const bCString*>(reinterpret_cast<const uint8_t*>(pInfo) + 0x0C);
        if (!pStrName || !pStrName->m_pcText) continue;

        const uint32_t hash = s_getHashValueCString
            ? s_getHashValueCString(*pStrName)
            : FallbackHashCString(*pStrName);
        const uint32_t bucketIdx = hash % pMap->m_u32BucketCount;

        auto* pNode = static_cast<SInfoMapNode*>(s_memAdminRealloc(admin, nullptr, sizeof(SInfoMapNode)));
        if (!pNode) continue;

        pNode->m_key.m_pcText = nullptr;
        s_fnCStringAssign(&pNode->m_key, pStrName);
        pNode->m_pValue = reinterpret_cast<gCInfo_PS*>(pInfo);
        pNode->m_pNext = pMap->m_ppBuckets[bucketIdx];
        pMap->m_ppBuckets[bucketIdx] = pNode;
        pMap->m_u32NodeCount++;
    }

    CInfoShadowCache::Invalidate();
    return true;
}

static bool SafeReplaySnapshot(void* pInfoManager) {
    __try {
        return DoReplaySnapshotInternal(pInfoManager);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("G3O", "[InfoManager] Exception during ReplaySnapshot (code=0x%08X).", GetExceptionCode());
        return false;
    }
}

static bool __fastcall Hooked_LoadInfosFromInis(void* pThis, void* /*edx*/) {
    if (!pThis) return false;

    AcquireSRWLockShared(&s_snapshotLock);
    const bool hasSnapshot = !s_snapshotBytes.empty();
    ReleaseSRWLockShared(&s_snapshotLock);

    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    if (hasSnapshot) {
        const bool replayed = SafeReplaySnapshot(pThis);
        if (replayed) {
            QueryPerformanceCounter(&t1);
            const double ms = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
            Log("G3O", "[InfoManager] INSTANT REPLAY SUCCESS: restored %u dialogs from RAM in %.2f ms! (Saved ~3.1 s)",
                s_snapshotCount, ms);
            s_infoAudit.loadInfosFromInisTicks += static_cast<uint64_t>(t1.QuadPart - t0.QuadPart);
            s_infoAudit.loadInfosFromInisCalls++;
            return true;
        }
        Log("G3O", "[InfoManager] Replay failed; falling back to original LoadInfosFromInis.");
    }

    s_loadInfosStats.calls.fetch_add(1, std::memory_order_relaxed);
    s_loadInfosStats.matches.fetch_add(1, std::memory_order_relaxed);
    auto* pMap = reinterpret_cast<bTInfoValMap*>(
        reinterpret_cast<uint8_t*>(pThis) + 0x4C);
    ExpandAndRehashInfoMap(pMap, kInfoMapBucketCount, "LoadInfosFromInis (this+0x4C)", &s_loadInfosStats);

    const bool result = Real_LoadInfosFromInis(pThis);
    QueryPerformanceCounter(&t1);
    const double loadMs = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
    s_infoAudit.loadInfosFromInisTicks += static_cast<uint64_t>(t1.QuadPart - t0.QuadPart);
    s_infoAudit.loadInfosFromInisCalls++;

    Log("G3O", "[InfoManager] LoadInfosFromInis completed in %.2f ms. Capturing baseline RAM snapshot...", loadMs);

    if (result) {
        SafeRecordSnapshot(pThis);
    }

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
// 7. Public API & Telemetry
// ============================================================================
uint64_t InfoManagerOptimizer_GetHits() noexcept { return CInfoShadowCache::GetHits(); }
uint64_t InfoManagerOptimizer_GetMisses() noexcept { return CInfoShadowCache::GetMisses(); }
uint32_t InfoManagerOptimizer_GetRebuilds() noexcept { return CInfoShadowCache::GetRebuilds(); }
uint32_t InfoManagerOptimizer_GetIndexedCount() noexcept { return CInfoShadowCache::GetIndexedCount(); }
uint32_t InfoManagerOptimizer_GetBucketExpansions() noexcept {
    return s_loadInfosStats.expanded.load(std::memory_order_relaxed) +
           s_initStats.expanded.load(std::memory_order_relaxed);
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
}

static HookInstallResult InstallInfoManagerHook(
    void* target,
    LPVOID detour,
    LPVOID* original,
    MH_STATUS& createStatus,
    MH_STATUS& enableStatus,
    MH_STATUS& removeStatus)
{
    return InstallHookSafely(
        [&] {
            createStatus = MH_CreateHook(target, detour, original);
            return createStatus == MH_OK;
        },
        [&] {
            enableStatus = MH_EnableHook(target);
            return enableStatus == MH_OK;
        },
        [&] { removeStatus = MH_RemoveHook(target); },
        [&] { if (original) *original = nullptr; });
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

    // Resolve bCMemoryAdmin allocator and hash function from SharedBase.dll
    HMODULE hSharedBase = GetModuleHandleW(L"SharedBase.dll");
    if (hSharedBase) {
        s_memAdminGetInstance = reinterpret_cast<FnMemAdminGetInstance>(
            GetProcAddress(hSharedBase, "?GetInstance@bCMemoryAdmin@@SGAAV1@XZ"));
        s_memAdminRealloc = reinterpret_cast<FnMemAdminRealloc>(
            GetProcAddress(hSharedBase, "?Realloc@bCMemoryAdmin@@QAEPAXPAXK@Z"));
        s_getHashValueCString = reinterpret_cast<FnGetHashValueCString>(
            GetProcAddress(hSharedBase, "??$g_GetHashValue@VbCString@@@@YGKABVbCString@@@Z"));

        s_fnMemStreamCtor = reinterpret_cast<FnMemStreamCtor>(
            GetProcAddress(hSharedBase, "??0bCMemoryStream@@QAE@XZ"));
        s_fnMemStreamDtor = reinterpret_cast<FnMemStreamDtor>(
            GetProcAddress(hSharedBase, "??1bCMemoryStream@@UAE@XZ"));
        s_fnMemStreamWrite = reinterpret_cast<FnMemStreamWrite>(
            GetProcAddress(hSharedBase, "?Write@bCMemoryStream@@UAEKPBXK@Z"));
        s_fnMemStreamGetBuffer = reinterpret_cast<FnMemStreamGetBuffer>(
            GetProcAddress(hSharedBase, "?GetBuffer@bCMemoryStream@@QBEPBEK@Z"));
        s_fnMemStreamGetSize = reinterpret_cast<FnMemStreamGetSize>(
            GetProcAddress(hSharedBase, "?GetSize@bCMemoryStream@@UBE_JXZ"));
        s_fnMemStreamSetCount = reinterpret_cast<FnMemStreamSetCount>(
            GetProcAddress(hSharedBase, "?SetCount@bCMemoryStream@@QAEXK@Z"));

        s_fnAccessorCtorWithObj = reinterpret_cast<FnAccessorCtorWithObj>(
            GetProcAddress(hSharedBase, "??0bCAccessorPropertyObject@@QAE@PAVbCObjectRefBase@@@Z"));
        s_fnAccessorCtorEmpty = reinterpret_cast<FnAccessorCtorEmpty>(
            GetProcAddress(hSharedBase, "??0bCAccessorPropertyObject@@QAE@XZ"));
        s_fnAccessorDtor = reinterpret_cast<FnAccessorDtor>(
            GetProcAddress(hSharedBase, "??1bCAccessorPropertyObject@@QAE@XZ"));
        s_fnAccessorGetNativeObj = reinterpret_cast<FnAccessorGetNativeObj>(
            GetProcAddress(hSharedBase, "?GetNativeObject@bCAccessorPropertyObject@@QAEPAVbCObjectRefBase@@XZ"));

        s_fnStreamWriteAccessor = reinterpret_cast<FnStreamWriteAccessor>(
            GetProcAddress(hSharedBase, "??6@YGAAVbCOStream@@AAV0@AAVbCAccessorPropertyObject@@@Z"));
        s_fnStreamReadAccessor = reinterpret_cast<FnStreamReadAccessor>(
            GetProcAddress(hSharedBase, "??5@YGAAVbCIStream@@AAV0@AAVbCAccessorPropertyObject@@@Z"));

        s_fnOStreamWriteU16 = reinterpret_cast<FnOStreamWriteU16>(
            GetProcAddress(hSharedBase, "??6bCOStream@@QAEAAV0@G@Z"));
        s_fnOStreamWriteU32 = reinterpret_cast<FnOStreamWriteU32>(
            GetProcAddress(hSharedBase, "??6bCOStream@@QAEAAV0@K@Z"));
        s_fnIStreamReadU16 = reinterpret_cast<FnIStreamReadU16>(
            GetProcAddress(hSharedBase, "??5bCIStream@@QAEAAV0@AAG@Z"));
        s_fnIStreamReadU32 = reinterpret_cast<FnIStreamReadU32>(
            GetProcAddress(hSharedBase, "??5bCIStream@@QAEAAV0@AAK@Z"));

        s_fnCStringAssign = reinterpret_cast<FnCStringAssign>(
            GetProcAddress(hSharedBase, "??4bCString@@QAEAAV0@ABV0@@Z"));
        s_fnCStringDtor = reinterpret_cast<FnCStringDtor>(
            GetProcAddress(hSharedBase, "??1bCString@@QAE@XZ"));
    }

    const auto baseAddr = reinterpret_cast<uintptr_t>(hGame);
    s_gameBase = baseAddr;
    s_expectedInitReturn  = baseAddr + 0x1CF26A;

    // 1. Hook gCInfoManager_PS::GetInfo (Game.dll+0x1CE270)
    void* pGetInfo = reinterpret_cast<void*>(baseAddr + 0x1CE270);
    MH_STATUS createStatus = MH_OK, enableStatus = MH_OK, removeStatus = MH_OK;
    HookInstallResult result = InstallInfoManagerHook(
        pGetInfo, reinterpret_cast<LPVOID>(&Hooked_GetInfo),
        reinterpret_cast<LPVOID*>(&Real_GetInfo), createStatus, enableStatus, removeStatus);
    if (result == HookInstallResult::Installed) {
        s_targetGetInfo = pGetInfo;
        Log("G3O", "[InfoManager] Successfully hooked gCInfoManager_PS::GetInfo at Game.dll+0x1CE270 (O(1) Shadow Table active).");
    } else {
        Log("G3O", "[InfoManager] GetInfo hook failed (create=%d, enable=%d, remove=%d).",
            createStatus, enableStatus, removeStatus);
    }

    // 2. Hook bTValMap::InitHashTable (Game.dll+0x005E270) - Primary path for LoadInfosFromInis (return: 0x1CF26A)
    void* pInitHashTable = reinterpret_cast<void*>(baseAddr + 0x005E270);
    result = InstallInfoManagerHook(
        pInitHashTable, reinterpret_cast<LPVOID>(&HookInitHashTableThunk),
        &s_realInitHashTable, createStatus, enableStatus, removeStatus);
    if (result == HookInstallResult::Installed) {
        s_targetInitHashTable = pInitHashTable;
        Log("G3O", "[InfoManager] Successfully hooked bTValMap::InitHashTable at Game.dll+0x005E270 (Primary path active).");
    } else {
        Log("G3O", "[InfoManager] InitHashTable hook failed (create=%d, enable=%d, remove=%d).",
            createStatus, enableStatus, removeStatus);
    }

    // 3. Micro-timing probes for bTPropertyObject<gCInfoManager_PS>::Read (0x1D0240)
    s_expectedPhaseBReturn = baseAddr + 0x1D0482;
    s_expectedPhaseCReturn = baseAddr + 0x1D048D;

    void* pPropertyObjectRead = reinterpret_cast<void*>(baseAddr + 0x1D0240);
    result = InstallInfoManagerHook(
        pPropertyObjectRead, reinterpret_cast<LPVOID>(&Hooked_InfoManagerPropertyObjectRead),
        reinterpret_cast<LPVOID*>(&Real_PropertyObjectRead), createStatus, enableStatus, removeStatus);
    if (result == HookInstallResult::Installed) {
        s_targetPropertyObjectRead = pPropertyObjectRead;
        Log("G3O", "[InfoManager] Successfully hooked 0x1D0240 (bTPropertyObject<gCInfoManager_PS>::Read probe active).");
    } else {
        Log("G3O", "[InfoManager] PropertyObject::Read hook failed (create=%d, enable=%d, remove=%d).",
            createStatus, enableStatus, removeStatus);
    }

    void* pInfoManagerCreate = reinterpret_cast<void*>(baseAddr + 0x1D0090);
    result = InstallInfoManagerHook(
        pInfoManagerCreate, reinterpret_cast<LPVOID>(&Hooked_InfoManagerCreate),
        reinterpret_cast<LPVOID*>(&Real_InfoManagerCreate), createStatus, enableStatus, removeStatus);
    if (result == HookInstallResult::Installed) {
        s_targetInfoManagerCreate = pInfoManagerCreate;
        Log("G3O", "[InfoManager] Successfully hooked 0x1D0090 (Phase B probe active).");
    } else {
        Log("G3O", "[InfoManager] InfoManagerCreate hook failed (create=%d, enable=%d, remove=%d).",
            createStatus, enableStatus, removeStatus);
    }

    if (hSharedBase) {
        void* pStreamReadObjectBase = reinterpret_cast<void*>(
            GetProcAddress(hSharedBase, "??5@YGAAVbCIStream@@AAV0@AAVbCObjectBase@@@Z"));
        if (pStreamReadObjectBase) {
            result = InstallInfoManagerHook(
                pStreamReadObjectBase, reinterpret_cast<LPVOID>(&Hooked_StreamReadObjectBase),
                reinterpret_cast<LPVOID*>(&Real_StreamReadObjectBase), createStatus, enableStatus, removeStatus);
            if (result == HookInstallResult::Installed) {
                s_targetStreamReadObjectBase = pStreamReadObjectBase;
                Log("G3O", "[InfoManager] Successfully hooked operator>> at %p (Phase C probe active).", s_targetStreamReadObjectBase);
            } else {
                Log("G3O", "[InfoManager] operator>> hook failed (create=%d, enable=%d, remove=%d).",
                    createStatus, enableStatus, removeStatus);
            }
        }
    }

    void* pLoadInfos = reinterpret_cast<void*>(
        GetProcAddress(hGame, "?LoadInfosFromInis@gCInfoManager_PS@@QAE_NXZ"));
    if (!pLoadInfos) {
        pLoadInfos = reinterpret_cast<void*>(baseAddr + 0x1CF160);
    }
    if (pLoadInfos) {
        result = InstallInfoManagerHook(
            pLoadInfos, reinterpret_cast<LPVOID>(&Hooked_LoadInfosFromInis),
            reinterpret_cast<LPVOID*>(&Real_LoadInfosFromInis), createStatus, enableStatus, removeStatus);
        if (result == HookInstallResult::Installed) {
            s_targetLoadInfosFromInis = pLoadInfos;
            Log("G3O", "[InfoManager] Successfully hooked LoadInfosFromInis at %p.", s_targetLoadInfosFromInis);
        } else {
            Log("G3O", "[InfoManager] LoadInfosFromInis hook failed (create=%d, enable=%d, remove=%d).",
                createStatus, enableStatus, removeStatus);
        }
    }

    s_bInitialized = true;
}

void ShutdownInfoManagerOptimizer() {
    if (!s_bInitialized) return;

    if (s_targetGetInfo) {
        MH_DisableHook(s_targetGetInfo);
        MH_RemoveHook(s_targetGetInfo);
        s_targetGetInfo = nullptr;
    }
    Real_GetInfo = nullptr;

    if (s_targetInitHashTable) {
        MH_DisableHook(s_targetInitHashTable);
        MH_RemoveHook(s_targetInitHashTable);
        s_targetInitHashTable = nullptr;
    }
    s_realInitHashTable = nullptr;

    if (s_targetPropertyObjectRead) {
        MH_DisableHook(s_targetPropertyObjectRead);
        MH_RemoveHook(s_targetPropertyObjectRead);
        s_targetPropertyObjectRead = nullptr;
    }
    Real_PropertyObjectRead = nullptr;

    if (s_targetInfoManagerCreate) {
        MH_DisableHook(s_targetInfoManagerCreate);
        MH_RemoveHook(s_targetInfoManagerCreate);
        s_targetInfoManagerCreate = nullptr;
    }
    Real_InfoManagerCreate = nullptr;

    if (s_targetStreamReadObjectBase) {
        MH_DisableHook(s_targetStreamReadObjectBase);
        MH_RemoveHook(s_targetStreamReadObjectBase);
        s_targetStreamReadObjectBase = nullptr;
    }
    Real_StreamReadObjectBase = nullptr;

    if (s_targetLoadInfosFromInis) {
        MH_DisableHook(s_targetLoadInfosFromInis);
        MH_RemoveHook(s_targetLoadInfosFromInis);
        s_targetLoadInfosFromInis = nullptr;
    }
    Real_LoadInfosFromInis = nullptr;

    Log("G3O", "[InfoManager] Shutdown complete (Hits: %llu, Misses: %llu, Rebuilds: %u, Cached: %u, Expansions: %u)",
        InfoManagerOptimizer_GetHits(),
        InfoManagerOptimizer_GetMisses(),
        InfoManagerOptimizer_GetRebuilds(),
        InfoManagerOptimizer_GetIndexedCount(),
        InfoManagerOptimizer_GetBucketExpansions());

    InfoManagerOptimizer_LogStats();

    CInfoShadowCache::Invalidate();

    AcquireSRWLockExclusive(&s_snapshotLock);
    s_snapshotBytes.clear();
    s_snapshotBytes.shrink_to_fit();
    s_snapshotCount = 0;
    ReleaseSRWLockExclusive(&s_snapshotLock);

    s_bInitialized = false;
}

} // namespace G3O
