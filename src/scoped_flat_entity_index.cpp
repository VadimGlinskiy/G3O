#include "scoped_flat_entity_index.h"
#include "config_manager.h"
#include "diagnostic_logger.h"
#include <minhook.h>
#include <atomic>
#include <cstring>
#include <cstdio>

namespace G3O {

enum SlotState : uint8_t {
    State_Empty = 0,
    State_Occupied = 1,
    State_Tombstone = 2
};

struct FlatSlot {
    uint32_t key[5]; // 20 bytes (bCPropertyID)
    void* pNode;     // 4 bytes (pointer to Node in bTValMap)
    void* pMap;      // 4 bytes (pointer to bTValMap)
    uint8_t state;   // 1 byte (State_Empty, State_Occupied, State_Tombstone)
    uint8_t pad[3];  // 3 bytes padding (total 32 bytes)
};

static constexpr size_t kTableSize = 131072; // 1 << 17 slots (4 MB total VAS)
static constexpr size_t kTableMask = kTableSize - 1;
static constexpr size_t kMaxManagedMaps = 8;

struct G3_ValMap {
    void**   m_ppBuckets;    // +0x00 (array of Node*)
    uint32_t m_u32TableSize; // +0x04 (number of buckets)
    uint32_t m_u32Count;     // +0x08 (number of elements)
};

struct G3_ValNode {
    uint32_t    m_key[5];    // +0x00 (20 bytes: bCPropertyID)
    void*       m_pValue;    // +0x14 (4 bytes: eCEntity*)
    G3_ValNode* m_pNext;     // +0x18 (4 bytes: next Node*)
};

static inline uint32_t HashPropertyID(const void* pMap, const void* pKey) {
    const uint32_t* k = reinterpret_cast<const uint32_t*>(pKey);
    uint32_t h = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pMap)) ^ 0x9e3779b9;
    h = (h ^ k[0]) * 0x01000193;
    h = (h ^ k[1]) * 0x01000193;
    h = (h ^ k[2]) * 0x01000193;
    h = (h ^ k[3]) * 0x01000193;
    h = (h ^ k[4]) * 0x01000193;
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    return h;
}

class FlatEntityIndexManager {
public:
    FlatSlot* m_slots = nullptr;
    void* m_managedMaps[kMaxManagedMaps] = { nullptr };
    size_t m_managedMapCount = 0;

    std::atomic<bool> m_isActive{false};
    bool m_shadowVerifyMode = false;
    FlatEntityIndexStats m_stats = {};

    bool IsManagingMap(const void* pMap) const {
        for (size_t i = 0; i < m_managedMapCount; ++i) {
            if (m_managedMaps[i] == pMap) return true;
        }
        return false;
    }

    void Upsert(void* pMap, const void* pKey, void* pNode) {
        if (!m_slots || !pMap || !pKey) return;

        const uint32_t* k = reinterpret_cast<const uint32_t*>(pKey);
        uint32_t idx = HashPropertyID(pMap, pKey) & kTableMask;
        uint32_t firstTombstone = static_cast<uint32_t>(-1);

        while (true) {
            auto& slot = m_slots[idx];
            if (slot.state == State_Empty) {
                uint32_t targetIdx = (firstTombstone != static_cast<uint32_t>(-1)) ? firstTombstone : idx;
                auto& targetSlot = m_slots[targetIdx];
                targetSlot.key[0] = k[0];
                targetSlot.key[1] = k[1];
                targetSlot.key[2] = k[2];
                targetSlot.key[3] = k[3];
                targetSlot.key[4] = k[4];
                targetSlot.pNode = pNode;
                targetSlot.pMap = pMap;
                targetSlot.state = State_Occupied;
                m_stats.itemCount++;
                m_stats.upserts++;
                return;
            }
            if (slot.state == State_Tombstone && firstTombstone == static_cast<uint32_t>(-1)) {
                firstTombstone = idx;
            } else if (slot.state == State_Occupied && slot.pMap == pMap) {
                if (slot.key[0] == k[0] && slot.key[1] == k[1] && slot.key[2] == k[2] &&
                    slot.key[3] == k[3] && slot.key[4] == k[4]) {
                    slot.pNode = pNode; // update existing node pointer
                    return;
                }
            }
            idx = (idx + 1) & kTableMask;
        }
    }

    void* Find(void* pMap, const void* pKey) {
        if (!m_slots || !pMap || !pKey) return nullptr;

        const uint32_t* k = reinterpret_cast<const uint32_t*>(pKey);
        uint32_t idx = HashPropertyID(pMap, pKey) & kTableMask;

        while (true) {
            const auto& slot = m_slots[idx];
            if (slot.state == State_Empty) {
                return nullptr; // not found
            }
            if (slot.state == State_Occupied && slot.pMap == pMap) {
                if (slot.key[0] == k[0] && slot.key[1] == k[1] && slot.key[2] == k[2] &&
                    slot.key[3] == k[3] && slot.key[4] == k[4]) {
                    return slot.pNode;
                }
            }
            idx = (idx + 1) & kTableMask;
        }
    }

    bool Remove(void* pMap, const void* pKey) {
        if (!m_slots || !pMap || !pKey) return false;

        const uint32_t* k = reinterpret_cast<const uint32_t*>(pKey);
        uint32_t idx = HashPropertyID(pMap, pKey) & kTableMask;

        while (true) {
            auto& slot = m_slots[idx];
            if (slot.state == State_Empty) {
                return false;
            }
            if (slot.state == State_Occupied && slot.pMap == pMap) {
                if (slot.key[0] == k[0] && slot.key[1] == k[1] && slot.key[2] == k[2] &&
                    slot.key[3] == k[3] && slot.key[4] == k[4]) {
                    slot.state = State_Tombstone;
                    m_stats.removes++;
                    if (m_stats.itemCount > 0) m_stats.itemCount--;
                    return true;
                }
            }
            idx = (idx + 1) & kTableMask;
        }
    }

    bool SeedMap(void* pMap) {
        if (!pMap || !m_slots) return false;
        G3_ValMap* map = reinterpret_cast<G3_ValMap*>(pMap);
        if (!map->m_ppBuckets || map->m_u32TableSize == 0 || map->m_u32Count == 0) return false;

        if (IsManagingMap(pMap)) return true;
        if (m_managedMapCount >= kMaxManagedMaps) return false;

        m_managedMaps[m_managedMapCount++] = pMap;
        m_stats.managedMapCount = static_cast<uint32_t>(m_managedMapCount);

        size_t seededCount = 0;
        for (uint32_t b = 0; b < map->m_u32TableSize; ++b) {
            G3_ValNode* node = reinterpret_cast<G3_ValNode*>(map->m_ppBuckets[b]);
            while (node) {
                Upsert(pMap, node->m_key, node);
                seededCount++;
                node = node->m_pNext;
            }
        }

        Log("G3O", "[FlatEntityIndex] Auto-seeded map 0x%p: %zu entities (table size %u).",
            pMap, seededCount, map->m_u32TableSize);
        return true;
    }
};

static FlatEntityIndexManager s_indexManager;

// Real function pointers
static void* s_pRealFindNode = nullptr;

using RegisterEntity_t = bool(__thiscall*)(void* pSceneAdmin, void* pEntity);
using UnregisterEntity_t = bool(__thiscall*)(void* pSceneAdmin, void* pEntity);
static RegisterEntity_t Real_RegisterEntity = nullptr;
static UnregisterEntity_t Real_UnregisterEntity = nullptr;

static void* s_targetFindNode = nullptr;
static void* s_targetRegisterEntity = nullptr;
static void* s_targetUnregisterEntity = nullptr;

static void* CallRealFindNode(void* pMap, const void* pKey, uint32_t* pOutHashIndex) {
    void* result = nullptr;
    __asm {
        mov eax, pMap
        push pOutHashIndex
        push pKey
        call s_pRealFindNode
        mov result, eax
    }
    return result;
}

extern "C" void* __cdecl ScopedFlatEntityIndex_FindNodeHandler(void* pMap, const void* pKey, uint32_t* pOutHashIndex) {
    if (!s_indexManager.m_isActive.load(std::memory_order_relaxed) || !pMap || !pKey) {
        return CallRealFindNode(pMap, pKey, pOutHashIndex);
    }

    // Auto-discover and seed heavy entity maps on first touch
    if (!s_indexManager.IsManagingMap(pMap)) {
        G3_ValMap* map = reinterpret_cast<G3_ValMap*>(pMap);
        // Only seed maps with substantial entity counts (>= 32 items)
        if (map && map->m_u32Count >= 32 && map->m_u32TableSize > 0 && map->m_ppBuckets) {
            s_indexManager.SeedMap(pMap);
        } else {
            return CallRealFindNode(pMap, pKey, pOutHashIndex);
        }
    }

    s_indexManager.m_stats.totalLookups++;

    // Shadow Verification Mode
    if (s_indexManager.m_shadowVerifyMode) {
        void* pFast = s_indexManager.Find(pMap, pKey);
        void* pReal = CallRealFindNode(pMap, pKey, pOutHashIndex);
        if (pFast != pReal) {
            s_indexManager.m_stats.mismatches++;
            if (s_indexManager.m_stats.mismatches <= 10) {
                const uint32_t* k = reinterpret_cast<const uint32_t*>(pKey);
                Log("G3O", "[FlatEntityIndex] Shadow MISMATCH: key=[%08X %08X %08X %08X %08X] fast=%p real=%p",
                    k[0], k[1], k[2], k[3], k[4], pFast, pReal);
            }
        }
        return pReal;
    }

    // Active Acceleration Mode
    void* pFast = s_indexManager.Find(pMap, pKey);
    if (pFast) {
        s_indexManager.m_stats.fastHits++;
        if (pOutHashIndex) *pOutHashIndex = 0;
        return pFast;
    }

    // Fail-open fallback: query live engine map
    void* pReal = CallRealFindNode(pMap, pKey, pOutHashIndex);
    if (pReal) {
        // Learn dynamic entity into flat index
        s_indexManager.Upsert(pMap, pKey, pReal);
    }
    s_indexManager.m_stats.fallbacks++;
    return pReal;
}

__declspec(naked) static void Hooked_FindNodeGateway() {
    __asm {
        push dword ptr [esp + 8]  // arg3: pOutHashIndex
        push dword ptr [esp + 8]  // arg2: pKey
        push eax                  // arg1: pMap
        call ScopedFlatEntityIndex_FindNodeHandler
        add esp, 12
        ret 8
    }
}

static bool __fastcall Hooked_RegisterEntity(void* pSceneAdmin, void* /*edx*/, void* pEntity) {
    bool result = Real_RegisterEntity ? Real_RegisterEntity(pSceneAdmin, pEntity) : false;
    if (result && s_indexManager.m_isActive.load(std::memory_order_relaxed) && pEntity && pSceneAdmin) {
        // eCEntity::m_PropertyID is at offset +0x18 (20 bytes)
        const void* pKey = reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(pEntity) + 0x18);
        void* pMap = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(pSceneAdmin) + 0x14);
        uint32_t dummyHash = 0;
        void* pNode = CallRealFindNode(pMap, pKey, &dummyHash);
        if (pNode) {
            s_indexManager.Upsert(pMap, pKey, pNode);
        }
    }
    return result;
}

static bool __fastcall Hooked_UnregisterEntity(void* pSceneAdmin, void* /*edx*/, void* pEntity) {
    if (s_indexManager.m_isActive.load(std::memory_order_relaxed) && pEntity && pSceneAdmin) {
        const void* pKey = reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(pEntity) + 0x18);
        void* pMap = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(pSceneAdmin) + 0x14);
        s_indexManager.Remove(pMap, pKey);
    }
    return Real_UnregisterEntity ? Real_UnregisterEntity(pSceneAdmin, pEntity) : false;
}

bool InitializeScopedFlatEntityIndex() {
    if (!GetConfig().enableFlatEntityIndex) {
        Log("G3O", "ScopedFlatEntityIndex disabled by configuration.");
        return false;
    }

    HMODULE hEngine = GetModuleHandleA("Engine.dll");
    if (!hEngine) {
        hEngine = LoadLibraryA("Engine.dll");
    }
    if (!hEngine) {
        Log("G3O", "[FlatEntityIndex] Error: Engine.dll not found.");
        return false;
    }

    // 1. Hook bTValMap<bCPropertyID, void*>::FindNode at RVA 0x1F0F20
    BYTE* pBase = reinterpret_cast<BYTE*>(hEngine);
    void* pFindNode = reinterpret_cast<void*>(pBase + 0x1F0F20);

    // Verify prologue bytes: 53 8B 5C 24 08 56 57 (push ebx; mov ebx, [esp+8]; push esi; push edi)
    const BYTE expectedSig[7] = { 0x53, 0x8B, 0x5C, 0x24, 0x08, 0x56, 0x57 };
    if (memcmp(pFindNode, expectedSig, sizeof(expectedSig)) != 0) {
        Log("G3O", "[FlatEntityIndex] Warning: FindNode signature mismatch at 0x%p. Hook aborted.", pFindNode);
        return false;
    }

    s_targetFindNode = pFindNode;
    MH_STATUS st = MH_CreateHook(pFindNode, reinterpret_cast<LPVOID>(Hooked_FindNodeGateway), reinterpret_cast<LPVOID*>(&s_pRealFindNode));
    if (st != MH_OK) {
        Log("G3O", "[FlatEntityIndex] Error: Failed to hook FindNode (code %d).", static_cast<int>(st));
        return false;
    }
    MH_EnableHook(pFindNode);

    // 2. Hook eCSceneAdmin::RegisterEntity
    void* pRegister = reinterpret_cast<void*>(GetProcAddress(hEngine, "?RegisterEntity@eCSceneAdmin@@IAE_NPAVeCEntity@@@Z"));
    if (pRegister) {
        s_targetRegisterEntity = pRegister;
        st = MH_CreateHook(pRegister, reinterpret_cast<LPVOID>(Hooked_RegisterEntity), reinterpret_cast<LPVOID*>(&Real_RegisterEntity));
        if (st == MH_OK) MH_EnableHook(pRegister);
    }

    // 3. Hook eCSceneAdmin::UnregisterEntity
    void* pUnregister = reinterpret_cast<void*>(GetProcAddress(hEngine, "?UnregisterEntity@eCSceneAdmin@@IAE_NPAVeCEntity@@@Z"));
    if (pUnregister) {
        s_targetUnregisterEntity = pUnregister;
        st = MH_CreateHook(pUnregister, reinterpret_cast<LPVOID>(Hooked_UnregisterEntity), reinterpret_cast<LPVOID*>(&Real_UnregisterEntity));
        if (st == MH_OK) MH_EnableHook(pUnregister);
    }

    Log("G3O", "[FlatEntityIndex] Initialized: FindNode hooked at 0x%p, RegisterEntity=%p, UnregisterEntity=%p.",
        pFindNode, pRegister, pUnregister);
    return true;
}

void ShutdownScopedFlatEntityIndex() {
    ScopedFlatEntityIndex_OnLoadEnd();

    if (s_targetFindNode) {
        MH_DisableHook(s_targetFindNode);
        MH_RemoveHook(s_targetFindNode);
        s_targetFindNode = nullptr;
    }
    if (s_targetRegisterEntity) {
        MH_DisableHook(s_targetRegisterEntity);
        MH_RemoveHook(s_targetRegisterEntity);
        s_targetRegisterEntity = nullptr;
    }
    if (s_targetUnregisterEntity) {
        MH_DisableHook(s_targetUnregisterEntity);
        MH_RemoveHook(s_targetUnregisterEntity);
        s_targetUnregisterEntity = nullptr;
    }

    s_pRealFindNode = nullptr;
    Real_RegisterEntity = nullptr;
    Real_UnregisterEntity = nullptr;
}

void ScopedFlatEntityIndex_OnLoadStart(void* /*pWorld*/) {
    if (!GetConfig().enableFlatEntityIndex) {
        return;
    }

    s_indexManager.m_shadowVerifyMode = GetConfig().enableFlatEntityShadowVerify;

    // Allocate 4 MB flat index memory
    if (!s_indexManager.m_slots) {
        s_indexManager.m_slots = reinterpret_cast<FlatSlot*>(
            VirtualAlloc(NULL, kTableSize * sizeof(FlatSlot), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    }

    s_indexManager.m_managedMapCount = 0;
    for (size_t i = 0; i < kMaxManagedMaps; ++i) s_indexManager.m_managedMaps[i] = nullptr;
    s_indexManager.m_stats = {};

    s_indexManager.m_isActive.store(true, std::memory_order_release);

    Log("G3O", "[FlatEntityIndex] Activated for gCWorld::LoadGameWorld (4 MB VirtualAlloc buffer, ShadowVerify=%s).",
        s_indexManager.m_shadowVerifyMode ? "true" : "false");
}

void ScopedFlatEntityIndex_OnLoadEnd() {
    if (!s_indexManager.m_isActive.load(std::memory_order_relaxed)) {
        return;
    }

    s_indexManager.m_isActive.store(false, std::memory_order_release);

    const auto& st = s_indexManager.m_stats;
    const double hitRate = st.totalLookups > 0 ? (static_cast<double>(st.fastHits) * 100.0 / static_cast<double>(st.totalLookups)) : 0.0;

    Log("G3O", "[FlatEntityIndex] Deactivated: lookups=%llu, fastHits=%llu (%.1f%%), fallbacks=%llu, upserts=%llu, removes=%llu, mismatches=%llu, maps=%u.",
        st.totalLookups, st.fastHits, hitRate, st.fallbacks, st.upserts, st.removes, st.mismatches, st.managedMapCount);

    if (s_indexManager.m_slots) {
        VirtualFree(s_indexManager.m_slots, 0, MEM_RELEASE);
        s_indexManager.m_slots = nullptr;
    }
    s_indexManager.m_managedMapCount = 0;
}

bool ScopedFlatEntityIndex_IsActive() {
    return s_indexManager.m_isActive.load(std::memory_order_relaxed);
}

FlatEntityIndexStats ScopedFlatEntityIndex_GetStats() {
    return s_indexManager.m_stats;
}

} // namespace G3O
