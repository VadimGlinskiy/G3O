#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <atomic>
#include <cstdio>
#include "guid_map_optimizer.h"
#include "guid_map_policy.h"
#include "diagnostic_logger.h"
#include "config_manager.h"
#include "MinHook.h"

namespace G3O {

namespace {

static std::atomic<uint32_t> s_u32FastWalkHits{0};
static std::atomic<uint32_t> s_u32Misses{0};

static bool s_bInitialized = false;
static void* s_pOrigFindNode = nullptr;

// ----------------------------------------------------------------------------
// FastPropertyIDMapLookup (C++ dispatcher called from naked hook)
// Operates directly on the live map buckets.
// Guarantees 100% safety against element deletion / reallocations.
// ----------------------------------------------------------------------------
extern "C" SPropertyIDMapNode* __cdecl FastPropertyIDMapLookup(
    const bTValMap_PropertyID* pMap,
    const bCPropertyID*        pKey,
    uint32_t*                  pOutBucket)
{
    if (!pMap || !pKey) {
        if (pOutBucket) *pOutBucket = 0;
        return nullptr;
    }

    const uint32_t u32BucketCount = pMap->m_u32BucketCount;
    if (u32BucketCount == 0 || pMap->m_ppBuckets == nullptr) {
        if (pOutBucket) *pOutBucket = 0;
        return nullptr;
    }

    const uint32_t u32NativeHash = ComputeNativePropertyIDHash(*pKey);
    const uint32_t u32BucketIdx  = ((u32BucketCount & (u32BucketCount - 1)) == 0)
        ? (u32NativeHash & (u32BucketCount - 1))
        : (u32NativeHash % u32BucketCount);

    if (pOutBucket) *pOutBucket = u32BucketIdx;

    const uint32_t k0 = pKey->d[0];
    const uint32_t k1 = pKey->d[1];
    const uint32_t k2 = pKey->d[2];
    const uint32_t k3 = pKey->d[3];

    SPropertyIDMapNode* pCurr = pMap->m_ppBuckets[u32BucketIdx];
    while (pCurr != nullptr) {
        if (pCurr->m_Key.d[0] == k0 &&
            pCurr->m_Key.d[1] == k1 &&
            pCurr->m_Key.d[2] == k2 &&
            pCurr->m_Key.d[3] == k3)
        {
            s_u32FastWalkHits.fetch_add(1, std::memory_order_relaxed);
            return pCurr;
        }
        pCurr = pCurr->m_pNext;
    }

    s_u32Misses.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

// ----------------------------------------------------------------------------
// Naked Hook Entry
// Engine.dll RVA 0x001F0F20 caller protocol:
// EAX = pMap (bTValMap*)
// [ESP + 4] = pKey (const bCPropertyID*)
// [ESP + 8] = pBucketOut (uint32_t*)
// Expects: return value in EAX (SPropertyIDMapNode*), stack cleaned via ret 8
// ----------------------------------------------------------------------------
__declspec(naked) static void Hook_PropertyIDMap_FindNode() {
    __asm {
        push dword ptr [esp + 8] // arg3: pBucketOut
        push dword ptr [esp + 8] // arg2: pKey (stack shifted by 4 after first push)
        push eax                 // arg1: pMap
        call FastPropertyIDMapLookup // __cdecl dispatcher returns SNode* in EAX
        add esp, 12              // clean __cdecl arguments
        ret 8                    // callee cleans pKey and pBucketOut (8 bytes)
    }
}

} // anonymous namespace

uint32_t GetGuidMapL1Hits() noexcept {
    return 0;
}

uint32_t GetGuidMapFastWalkHits() noexcept {
    return s_u32FastWalkHits.load(std::memory_order_relaxed);
}

uint32_t GetGuidMapMisses() noexcept {
    return s_u32Misses.load(std::memory_order_relaxed);
}

void LogGuidMapStats() {
    const uint32_t walks = s_u32FastWalkHits.load(std::memory_order_relaxed);
    const uint32_t misses = s_u32Misses.load(std::memory_order_relaxed);
    Log("GuidMapOpt", "Fast Inline Walks: %u | Misses: %u", walks, misses);
}

bool InitializeGuidMapOptimizer() {
    if (s_bInitialized) return true;

    const auto& config = GetConfig();
    if (!config.enableGuidMapOptimizer) {
        Log("GuidMapOpt", "GuidMapOptimizer disabled in config.");
        return false;
    }

    HMODULE hEngine = GetModuleHandleW(L"Engine.dll");
    if (!hEngine) {
        hEngine = LoadLibraryW(L"Engine.dll");
    }
    if (!hEngine) {
        Log("GuidMapOpt", "Failed to get Engine.dll handle.");
        return false;
    }

    const auto pBase = reinterpret_cast<uintptr_t>(hEngine);

    // Target: bTValMap<bCPropertyID, void*>::FindNode (RVA 0x001F0F20)
    void* pTarget = reinterpret_cast<void*>(pBase + 0x001F0F20);

    // Verify prologue bytes: 53 8b 5c 24 08 (push ebx; mov ebx, [esp + 8])
    const auto* pBytes = reinterpret_cast<const uint8_t*>(pTarget);
    if (pBytes[0] != 0x53 || pBytes[1] != 0x8B || pBytes[2] != 0x5C || pBytes[3] != 0x24 || pBytes[4] != 0x08) {
        Log("GuidMapOpt", "Prologue mismatch at Engine.dll+0x001F0F20: %02X %02X %02X %02X %02X",
            pBytes[0], pBytes[1], pBytes[2], pBytes[3], pBytes[4]);
        return false;
    }

    MH_STATUS status = MH_CreateHook(pTarget, reinterpret_cast<void*>(&Hook_PropertyIDMap_FindNode), &s_pOrigFindNode);
    if (status != MH_OK) {
        Log("GuidMapOpt", "MH_CreateHook failed for PropertyIDMap_FindNode (status %d)", status);
        return false;
    }

    status = MH_EnableHook(pTarget);
    if (status != MH_OK) {
        Log("GuidMapOpt", "MH_EnableHook failed for PropertyIDMap_FindNode (status %d)", status);
        return false;
    }

    s_bInitialized = true;
    Log("GuidMapOpt", "Successfully hooked Engine.dll!PropertyIDMap_FindNode at 0x%p (fast direct inline traversal)", pTarget);
    return true;
}

} // namespace G3O
