#include "property_cache_optimizer.h"
#include "config_manager.h"
#include "diagnostic_logger.h"
#include <minhook.h>
#include <windows.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

namespace G3O {

typedef bool (__thiscall* ResolvePropertyString_t)(void* pThis, const void* pName);
typedef bool (__thiscall* ResolvePropertyID_t)(void* pThis, const void* pId);

static ResolvePropertyString_t Real_ResolvePropertyString = nullptr;
static ResolvePropertyID_t Real_ResolvePropertyID = nullptr;

static void* s_targetString = nullptr;
static void* s_targetID = nullptr;

static volatile LONG s_stringHits = 0;
static volatile LONG s_stringMisses = 0;
static volatile LONG s_idHits = 0;
static volatile LONG s_idMisses = 0;

// Direct-mapped cache for String property lookups (16384 slots)
struct StringCacheEntry {
    const void* pClass;
    uint32_t    nameHash;
    char        name[64];
    void*       resolvedClass;
    uint32_t    resolvedIndex;
    bool        success;
    bool        valid;
};

// Direct-mapped cache for PropertyID lookups (8192 slots)
struct PropertyIdCacheEntry {
    const void* pClass;
    uint32_t    id[4];
    void*       resolvedClass;
    uint32_t    resolvedIndex;
    bool        success;
    bool        valid;
};

static const uint32_t STRING_CACHE_SIZE = 16384;
static const uint32_t STRING_CACHE_MASK = STRING_CACHE_SIZE - 1;

static const uint32_t ID_CACHE_SIZE = 8192;
static const uint32_t ID_CACHE_MASK = ID_CACHE_SIZE - 1;

// Thread-local caches to ensure 100% thread safety without locks
static thread_local StringCacheEntry* s_tlStringCache = nullptr;
static thread_local PropertyIdCacheEntry* s_tlIdCache = nullptr;

static inline StringCacheEntry* GetStringCache() {
    if (!s_tlStringCache) {
        s_tlStringCache = (StringCacheEntry*)calloc(STRING_CACHE_SIZE, sizeof(StringCacheEntry));
    }
    return s_tlStringCache;
}

static inline PropertyIdCacheEntry* GetIdCache() {
    if (!s_tlIdCache) {
        s_tlIdCache = (PropertyIdCacheEntry*)calloc(ID_CACHE_SIZE, sizeof(PropertyIdCacheEntry));
    }
    return s_tlIdCache;
}

static inline uint32_t HashClassAndString(const void* pClass, const char* str) {
    uint32_t hash = 2166136261u;
    hash ^= (uint32_t)((uintptr_t)pClass & 0xFFFFFFFF);
    hash *= 16777619u;
    while (*str) {
        hash ^= (uint8_t)(*str++);
        hash *= 16777619u;
    }
    return hash;
}

static inline uint32_t HashClassAndId(const void* pClass, const uint32_t* id) {
    uint32_t hash = 2166136261u;
    hash ^= (uint32_t)((uintptr_t)pClass & 0xFFFFFFFF);
    hash *= 16777619u;
    hash ^= id[0]; hash *= 16777619u;
    hash ^= id[1]; hash *= 16777619u;
    hash ^= id[2]; hash *= 16777619u;
    hash ^= id[3]; hash *= 16777619u;
    return hash;
}

static bool __fastcall Hooked_ResolvePropertyString(void* pThis, void* /*edx*/, const void* pName) {
    if (!pThis || !pName) {
        return Real_ResolvePropertyString(pThis, pName);
    }

    void* currentClass = *(void**)pThis;
    if (!currentClass) {
        return Real_ResolvePropertyString(pThis, pName);
    }

    const char* str = *(const char**)pName;
    if (!str) {
        str = "";
    }

    size_t len = strlen(str);
    if (len >= 63) {
        return Real_ResolvePropertyString(pThis, pName);
    }

    StringCacheEntry* cache = GetStringCache();
    if (!cache) {
        return Real_ResolvePropertyString(pThis, pName);
    }

    uint32_t hash = HashClassAndString(currentClass, str);
    uint32_t index = hash & STRING_CACHE_MASK;
    StringCacheEntry& entry = cache[index];

    if (entry.valid && entry.pClass == currentClass && entry.nameHash == hash && memcmp(entry.name, str, len + 1) == 0) {
        InterlockedIncrement(&s_stringHits);
        if (entry.success) {
            *(void**)pThis = entry.resolvedClass;
            *(uint32_t*)((BYTE*)pThis + 8) = entry.resolvedIndex;
            return true;
        } else {
            *(uint32_t*)((BYTE*)pThis + 8) = 0xFFFFFFFF;
            return false;
        }
    }

    InterlockedIncrement(&s_stringMisses);
    bool result = Real_ResolvePropertyString(pThis, pName);

    entry.pClass = currentClass;
    entry.nameHash = hash;
    memcpy(entry.name, str, len + 1);
    entry.success = result;
    if (result) {
        entry.resolvedClass = *(void**)pThis;
        entry.resolvedIndex = *(uint32_t*)((BYTE*)pThis + 8);
    } else {
        entry.resolvedClass = nullptr;
        entry.resolvedIndex = 0xFFFFFFFF;
    }
    entry.valid = true;

    return result;
}

static bool __fastcall Hooked_ResolvePropertyID(void* pThis, void* /*edx*/, const void* pId) {
    if (!pThis || !pId) {
        return Real_ResolvePropertyID(pThis, pId);
    }

    void* currentClass = *(void**)pThis;
    if (!currentClass) {
        return Real_ResolvePropertyID(pThis, pId);
    }

    PropertyIdCacheEntry* cache = GetIdCache();
    if (!cache) {
        return Real_ResolvePropertyID(pThis, pId);
    }

    const uint32_t* id = (const uint32_t*)pId;
    uint32_t hash = HashClassAndId(currentClass, id);
    uint32_t index = hash & ID_CACHE_MASK;
    PropertyIdCacheEntry& entry = cache[index];

    if (entry.valid && entry.pClass == currentClass &&
        entry.id[0] == id[0] && entry.id[1] == id[1] &&
        entry.id[2] == id[2] && entry.id[3] == id[3]) {
        InterlockedIncrement(&s_idHits);
        if (entry.success) {
            *(void**)pThis = entry.resolvedClass;
            *(uint32_t*)((BYTE*)pThis + 8) = entry.resolvedIndex;
            return true;
        } else {
            *(uint32_t*)((BYTE*)pThis + 8) = 0xFFFFFFFF;
            return false;
        }
    }

    InterlockedIncrement(&s_idMisses);
    bool result = Real_ResolvePropertyID(pThis, pId);

    entry.pClass = currentClass;
    entry.id[0] = id[0];
    entry.id[1] = id[1];
    entry.id[2] = id[2];
    entry.id[3] = id[3];
    entry.success = result;
    if (result) {
        entry.resolvedClass = *(void**)pThis;
        entry.resolvedIndex = *(uint32_t*)((BYTE*)pThis + 8);
    } else {
        entry.resolvedClass = nullptr;
        entry.resolvedIndex = 0xFFFFFFFF;
    }
    entry.valid = true;

    return result;
}

bool InitializePropertyCacheOptimizer() {
    if (!GetConfig().enablePropertyCache) {
        return false;
    }

    HMODULE hSharedBase = GetModuleHandleA("SharedBase.dll");
    if (!hSharedBase) {
        Log("G3O", "PropertyCacheOptimizer: SharedBase.dll not loaded.");
        return false;
    }

    void* fnString = (void*)GetProcAddress(hSharedBase, "?ResolveProperty@bCAccessorProperty@@IAE_NABVbCString@@@Z");
    void* fnID = (void*)GetProcAddress(hSharedBase, "?ResolveProperty@bCAccessorProperty@@IAE_NABVbCPropertyID@@@Z");

    if (!fnString || !fnID) {
        Log("G3O", "PropertyCacheOptimizer: ResolveProperty exports not found in SharedBase.dll.");
        return false;
    }

    s_targetString = fnString;
    s_targetID = fnID;

    MH_STATUS s1 = MH_CreateHook(fnString, (LPVOID)Hooked_ResolvePropertyString, (LPVOID*)&Real_ResolvePropertyString);
    MH_STATUS s2 = MH_CreateHook(fnID, (LPVOID)Hooked_ResolvePropertyID, (LPVOID*)&Real_ResolvePropertyID);

    if (s1 != MH_OK || s2 != MH_OK) {
        Log("G3O", "PropertyCacheOptimizer: Failed to create hooks (code %d, %d).", (int)s1, (int)s2);
        return false;
    }

    MH_EnableHook(fnString);
    MH_EnableHook(fnID);

    Log("G3O", "Property Cache Optimizer initialized (bCAccessorProperty::ResolveProperty hooked with O(1) hash cache).");
    return true;
}

void ShutdownPropertyCacheOptimizer() {
    if (s_targetString) {
        MH_DisableHook(s_targetString);
        s_targetString = nullptr;
    }
    if (s_targetID) {
        MH_DisableHook(s_targetID);
        s_targetID = nullptr;
    }
    Log("G3O", "Property Cache Optimizer stats: String hits=%ld, misses=%ld | ID hits=%ld, misses=%ld",
        s_stringHits, s_stringMisses, s_idHits, s_idMisses);
}

} // namespace G3O
