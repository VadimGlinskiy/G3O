#include "class_factory_optimizer.h"
#include "class_factory_policy.h"
#include "diagnostic_logger.h"
#include "hook_install_policy.h"
#include <windows.h>
#include <atomic>
#include "MinHook.h"

namespace G3O {

namespace {

// ----------------------------------------------------------------------------
// Globals & State
// ----------------------------------------------------------------------------
static HMODULE               s_hSharedBase                  = nullptr;
static void*                 s_pfnOriginalClassMapFindNode  = nullptr;
static SClassNodeCacheEntry  s_classCache[kClassCacheCapacity] = {};
static char                  s_stringArena[16384]           = {};
static uint32_t              s_arenaOffset                  = 0;
static SRWLOCK               s_cacheLock                    = SRWLOCK_INIT;

static std::atomic<uint32_t> s_u32Hits{0};
static std::atomic<uint32_t> s_u32Misses{0};
static std::atomic<uint32_t> s_u32CachedClasses{0};
static bool                  s_bInitialized = false;

// ----------------------------------------------------------------------------
// Low-level helper: Calls original trampoline with exact register conventions:
// EDI = pMap
// EAX = pKey (&bCString)
// [ESP + 4] = pBucketOut (callee pops via ret 4)
// ----------------------------------------------------------------------------
__forceinline void* CallOriginalClassMapFindNode(void* pMap, void* pKey, uint32_t* pBucketOut) {
    void* pResult = nullptr;
    void* pfnTarget = s_pfnOriginalClassMapFindNode;
    __asm {
        push ebx
        push esi
        push edi
        mov edi, pMap
        mov eax, pKey
        push pBucketOut
        call pfnTarget
        mov pResult, eax
        pop edi
        pop esi
        pop ebx
    }
    return pResult;
}

// ----------------------------------------------------------------------------
// FastClassMapLookup (C++ dispatcher)
// ----------------------------------------------------------------------------
void* __cdecl FastClassMapLookup(void* pMap, void* pKey, uint32_t* pBucketOut) {
    if (!pMap || !pKey) {
        return CallOriginalClassMapFindNode(pMap, pKey, pBucketOut);
    }

    const char* pText = *reinterpret_cast<const char**>(pKey);
    if (!pText) {
        return CallOriginalClassMapFindNode(pMap, pKey, pBucketOut);
    }

    uint32_t u32Len = *reinterpret_cast<const uint32_t*>(pText - 8);
    if (u32Len == 0) {
        return CallOriginalClassMapFindNode(pMap, pKey, pBucketOut);
    }

    uint32_t u32Hash   = ComputeFastClassHash(pText, u32Len);
    uint64_t u64Prefix = ReadPrefix8(pText, u32Len);

    // 1. Fast O(1) Cache Lookup
    const SClassNodeCacheEntry* pEntry = FindInNodeCache(s_classCache, pMap, pText, u32Len, u32Hash, u64Prefix);
    if (pEntry) {
        s_u32Hits.fetch_add(1, std::memory_order_relaxed);
        if (pBucketOut) *pBucketOut = pEntry->u32Bucket;
        return pEntry->pNode;
    }

    // 2. Cache Miss: Invoke original engine find
    s_u32Misses.fetch_add(1, std::memory_order_relaxed);
    void* pNode = CallOriginalClassMapFindNode(pMap, pKey, pBucketOut);
    if (pNode) {
        uint32_t u32Bucket = pBucketOut ? *pBucketOut : 0;
        AcquireSRWLockExclusive(&s_cacheLock);
        const SClassNodeCacheEntry* pInserted = InsertIntoNodeCache(
            s_classCache, s_stringArena, s_arenaOffset, sizeof(s_stringArena),
            pMap, pText, u32Len, u32Hash, u64Prefix, pNode, u32Bucket);
        if (pInserted) {
            s_u32CachedClasses.fetch_add(1, std::memory_order_relaxed);
        }
        ReleaseSRWLockExclusive(&s_cacheLock);
    }
    return pNode;
}

// ----------------------------------------------------------------------------
// Naked Hook Entry
// SharedBase.dll RVA 0x00057FB0 caller protocol:
// EDI = pMap
// EAX = pKey
// [ESP + 4] = pBucketOut
// Expects: return value in EAX, stack cleaned with ret 4
// ----------------------------------------------------------------------------
__declspec(naked) void Hook_ClassMapFindNode() {
    __asm {
        push dword ptr [esp + 4] // arg3: pBucketOut
        push eax                 // arg2: pKey
        push edi                 // arg1: pMap
        call FastClassMapLookup  // C++ dispatcher returns pNode in EAX
        add esp, 12              // clean __cdecl arguments
        ret 4                    // callee cleans pBucketOut (4 bytes)
    }
}

} // anonymous namespace

// ----------------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------------
bool InitializeClassFactoryOptimizer() {
    if (s_bInitialized) return true;

    s_hSharedBase = GetModuleHandleW(L"SharedBase.dll");
    if (!s_hSharedBase) {
        s_hSharedBase = LoadLibraryW(L"SharedBase.dll");
    }
    if (!s_hSharedBase) {
        Log("ClassFactory", "Failed to get SharedBase.dll handle");
        return false;
    }

    auto pBase = reinterpret_cast<uintptr_t>(s_hSharedBase);

    // Target: bTValMap<bCString, T>::FindNode (RVA 0x00057FB0)
    void* pTarget = reinterpret_cast<void*>(pBase + 0x00057FB0);

    // Verify prologue bytes: push ebx (53), push esi (56), mov esi, [edi + 4] (8b 77 04)
    const uint8_t* pBytes = reinterpret_cast<const uint8_t*>(pTarget);
    if (pBytes[0] != 0x53 || pBytes[1] != 0x56 || pBytes[2] != 0x8B || pBytes[3] != 0x77 || pBytes[4] != 0x04) {
        Log("ClassFactory", "Prologue signature mismatch at 0x%p (%02X %02X %02X %02X %02X)",
            pTarget, pBytes[0], pBytes[1], pBytes[2], pBytes[3], pBytes[4]);
        return false;
    }

    MH_STATUS createStatus = MH_OK;
    MH_STATUS enableStatus = MH_OK;
    MH_STATUS removeStatus = MH_OK;
    const HookInstallResult result = InstallHookSafely(
        [&] {
            createStatus = MH_CreateHook(pTarget, reinterpret_cast<void*>(&Hook_ClassMapFindNode),
                                         reinterpret_cast<void**>(&s_pfnOriginalClassMapFindNode));
            return createStatus == MH_OK;
        },
        [&] {
            enableStatus = MH_EnableHook(pTarget);
            return enableStatus == MH_OK;
        },
        [&] { removeStatus = MH_RemoveHook(pTarget); },
        [&] { s_pfnOriginalClassMapFindNode = nullptr; });
    if (result != HookInstallResult::Installed) {
        if (result == HookInstallResult::CreateFailed) {
            Log("ClassFactory", "MH_CreateHook failed with status %d", createStatus);
        } else {
            Log("ClassFactory", "MH_EnableHook failed with status %d; MH_RemoveHook returned %d",
                enableStatus, removeStatus);
        }
        return false;
    }

    s_bInitialized = true;
    Log("ClassFactory", "Successfully hooked bTValMap::FindNode at 0x%p (O(1) L1 cache active)", pTarget);
    return true;
}

void ShutdownClassFactoryOptimizer() {
    if (!s_bInitialized) return;

    if (s_hSharedBase) {
        auto pBase = reinterpret_cast<uintptr_t>(s_hSharedBase);
        void* pTarget = reinterpret_cast<void*>(pBase + 0x00057FB0);
        MH_DisableHook(pTarget);
        MH_RemoveHook(pTarget);
        s_pfnOriginalClassMapFindNode = nullptr;
    }

    s_bInitialized = false;
    Log("ClassFactory", "Shutdown complete (Hits: %u, Misses: %u, Cached: %u)",
        s_u32Hits.load(), s_u32Misses.load(), s_u32CachedClasses.load());
}

void ResetClassFactoryOptimizerSession() {
    s_u32Hits.store(0, std::memory_order_relaxed);
    s_u32Misses.store(0, std::memory_order_relaxed);
}

uint32_t ClassFactory_GetHits() {
    return s_u32Hits.load(std::memory_order_relaxed);
}

uint32_t ClassFactory_GetMisses() {
    return s_u32Misses.load(std::memory_order_relaxed);
}

uint32_t ClassFactory_GetCachedClassesCount() {
    return s_u32CachedClasses.load(std::memory_order_relaxed);
}

} // namespace G3O
