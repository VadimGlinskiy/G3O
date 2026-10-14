#include "combat_guard.h"
#include <minhook.h>
#include <stdio.h>

// Function signatures from Engine.dll
typedef void* (__thiscall* GetPropertySet_t)(void* pEntity, int type);
static GetPropertySet_t s_pfnGetPropertySet = nullptr;

typedef void* (__thiscall* GetEntityFromProxy_t)(void* pProxy);
static GetEntityFromProxy_t s_pfnGetEntityFromProxy = nullptr;

// Pointer to Game.dll session singleton for player hero entity
static uintptr_t* s_ppSession = nullptr;

// Real hooked functions from Engine.dll
typedef int (__thiscall* CullTreeHC_t)(void* pThis, void* pFrustumDB);
static CullTreeHC_t Real_CullTreeHC = nullptr;

typedef bool (__thiscall* CullVPTHC_t)(void* pThis, void* pFrustumDB);
static CullVPTHC_t Real_CullVPTHC = nullptr;

typedef float (__thiscall* CullSmallObject_t)(void* pThis, void* pFrustumDB, bool bFlag);
static CullSmallObject_t Real_CullSmallObject = nullptr;

// Diagnostic statistics
static volatile DWORD g_combatantCullBypasses = 0;

static bool IsActiveCombatant(void* pEntity) {
    if (!pEntity) return false;

    __try {
        // 1. Instant bitmask test for PropertySet 0x1e (gCNPC_PS)
        // Bit 30 in DWORD at offset 0x194 indicates presence of gCNPC_PS
        DWORD mask = *(DWORD*)((char*)pEntity + 0x194);
        if (!(mask & 0x40000000)) {
            return false;
        }

        // 2. Query gCNPC_PS from entity
        if (!s_pfnGetPropertySet) return false;
        void* pNPC_PS = s_pfnGetPropertySet(pEntity, 0x1e);
        if (!pNPC_PS) return false;

        // 3. Resolve Hero (Player) entity
        void* pHero = nullptr;
        if (s_ppSession && *s_ppSession) {
            pHero = *(void**)(*s_ppSession + 0x1c);
        }

        // The player entity itself has its own visibility rules, not treated as an external combatant
        if (pHero && pEntity == pHero) {
            return false;
        }

        // 4. Distance check relative to player
        // Maximum engagement radius: 65.0 meters = 6500.0f units
        // 6500 * 6500 = 42,250,000.0f
        if (pHero) {
            float ex = *(float*)((char*)pEntity + 0xb0);
            float ey = *(float*)((char*)pEntity + 0xb4);
            float ez = *(float*)((char*)pEntity + 0xb8);

            float hx = *(float*)((char*)pHero + 0xb0);
            float hy = *(float*)((char*)pHero + 0xb4);
            float hz = *(float*)((char*)pHero + 0xb8);

            float dx = ex - hx;
            float dy = ey - hy;
            float dz = ez - hz;
            float distSq = dx * dx + dy * dy + dz * dz;

            if (distSq > 42250000.0f) {
                return false;
            }
        }

        // 5. Check combat state
        DWORD combatState = *(DWORD*)((char*)pNPC_PS + 0x118);
        if (combatState != 0) {
            return true;
        }

        // 6. Check targeted entity
        if (s_pfnGetEntityFromProxy) {
            void* pTarget = s_pfnGetEntityFromProxy((char*)pNPC_PS + 0xa8);
            if (pTarget != nullptr && pHero && pTarget == pHero) {
                return true;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    return false;
}

// Hook for CullTreeHC
static int __fastcall Hooked_CullTreeHC(void* pThis, void* /*edx*/, void* pFrustumDB) {
    if (!pThis || !Real_CullTreeHC) return 0xf;

    void* pEntity = *(void**)((char*)pThis + 0xc);
    if (pEntity && IsActiveCombatant(pEntity)) {
        InterlockedIncrement((volatile LONG*)&g_combatantCullBypasses);
        return 0; // 0 = eECullStatus_Inside
    }

    return Real_CullTreeHC(pThis, pFrustumDB);
}

// Hook for CullVPTHC
static bool __fastcall Hooked_CullVPTHC(void* pThis, void* /*edx*/, void* pFrustumDB) {
    if (!pThis || !Real_CullVPTHC) return false;

    void* pEntity = *(void**)((char*)pThis + 0xc);
    if (pEntity && IsActiveCombatant(pEntity)) {
        InterlockedIncrement((volatile LONG*)&g_combatantCullBypasses);
        return true; // Force visible, keep entity active regardless of camera angle
    }

    return Real_CullVPTHC(pThis, pFrustumDB);
}

// Hook for CullSmallObject
static float __fastcall Hooked_CullSmallObject(void* pThis, void* /*edx*/, void* pFrustumDB, bool bFlag) {
    if (!pThis || !Real_CullSmallObject) return 0.0f;

    void* pEntity = *(void**)((char*)pThis + 0xc);
    if (pEntity && IsActiveCombatant(pEntity)) {
        return 1.0f; // Force screen area to 1.0f so offscreen combatant is not dropped by LoD
    }

    return Real_CullSmallObject(pThis, pFrustumDB, bFlag);
}

void InitializeCombatGuard() {
    HMODULE hEngine = GetModuleHandleA("Engine.dll");
    HMODULE hGame = GetModuleHandleA("Game.dll");
    if (!hEngine || !hGame) return;

    // Resolve helper functions
    s_pfnGetPropertySet = (GetPropertySet_t)GetProcAddress(
        hEngine, "?GetPropertySet@eCEntity@@UAEPAVeCEntityPropertySet@@W4eEPropertySetType@@@Z"
    );
    s_pfnGetEntityFromProxy = (GetEntityFromProxy_t)GetProcAddress(
        hEngine, "?GetEntity@eCEntityProxy@@QAEPAVeCEntity@@XZ"
    );
    s_ppSession = (uintptr_t*)((char*)hGame + 0x3f0218);

    // 1. Hook CullTreeHC
    void* pCullTreeHC = (void*)GetProcAddress(
        hEngine, "?CullTreeHC@eCFrustumElementItem@@QAE?AW4eECullStatus@eCFrustumFilter@@AAVeCFrustumDatabase@@@Z"
    );
    if (pCullTreeHC) {
        if (MH_CreateHook(pCullTreeHC, (LPVOID)Hooked_CullTreeHC, (LPVOID*)&Real_CullTreeHC) == MH_OK) {
            MH_EnableHook(pCullTreeHC);
        }
    }

    // 2. Hook CullVPTHC
    void* pCullVPTHC = (void*)GetProcAddress(
        hEngine, "?CullVPTHC@eCFrustumElementItem@@QAE_NAAVeCFrustumDatabase@@@Z"
    );
    if (pCullVPTHC) {
        if (MH_CreateHook(pCullVPTHC, (LPVOID)Hooked_CullVPTHC, (LPVOID*)&Real_CullVPTHC) == MH_OK) {
            MH_EnableHook(pCullVPTHC);
        }
    }

    // 3. Hook CullSmallObject
    void* pCullSmallObject = (void*)GetProcAddress(
        hEngine, "?CullSmallObject@eCFrustumElementItem@@QAEMAAVeCFrustumDatabase@@_N@Z"
    );
    if (pCullSmallObject) {
        if (MH_CreateHook(pCullSmallObject, (LPVOID)Hooked_CullSmallObject, (LPVOID*)&Real_CullSmallObject) == MH_OK) {
            MH_EnableHook(pCullSmallObject);
        }
    }

    OutputDebugStringA("[Gothic3Optima] Phase 19.1: Combatant Frustum Culling Exclusion active (CullTreeHC, CullVPTHC, CullSmallObject).\n");
}

void ShutdownCombatGuard() {
    // Hooks disabled globally by MH_DisableHook in ShutdownOptima
}
