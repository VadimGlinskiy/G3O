#include "scene_node_optimizer.h"
#include "scene_node_policy.h"
#include "config_manager.h"
#include "diagnostic_logger.h"
#include "hook_install_policy.h"
#include <minhook.h>
#include <windows.h>
#include <atomic>

namespace G3O {

using bCMemoryAdmin_GetInstance_t = void* (__stdcall*)();
using bCMemoryAdmin_Realloc_t     = void* (__thiscall*)(void* pThis, void* pMem, uint32_t u32Size);
using eCNode_AttachChild_t        = int32_t (__thiscall*)(void* pThisNode, void* pChildNode);

static bCMemoryAdmin_GetInstance_t fpMemAdminGetInstance = nullptr;
static bCMemoryAdmin_Realloc_t     fpMemAdminRealloc     = nullptr;
static eCNode_AttachChild_t        Real_AttachChild      = nullptr;
static void*                       s_targetAttachChild   = nullptr;

static std::atomic<uint32_t> s_nodePreallocations{0};

// Hooked eCNode::AttachChild (Engine.dll RVA 0x0020DDF0)
// Layout of eCNode:
//   +0x0C: eCNode*  m_pParent
//   +0x10: uint16_t m_u16Capacity
//   +0x12: uint16_t m_u16Count
//   +0x14: void**   m_ppChildren
//
// In vanilla Engine.dll (0x000AC340), bTValArray expands by only 25% (shr eax, 2) once Capacity > 64.
// For large sector roots (SysDyn, RB_Mod, etc. holding 20,000+ entities), this produces ~26-30 reallocations.
// We intercept when count == capacity for large nodes (>= 64) and preallocate exponentially: 1024 -> 8192 -> 32768.
// When vanilla GrowIfNeeded executes right after, it sees count < capacity and exits in 1 clock cycle.
static int32_t __fastcall Hooked_AttachChild(void* pThisNode, void* /*edx*/, void* pChildNode) noexcept {
    if (pThisNode && pChildNode && fpMemAdminGetInstance && fpMemAdminRealloc) {
        const uintptr_t childAddr = reinterpret_cast<uintptr_t>(pChildNode);
        void* pChildParent = *reinterpret_cast<void**>(childAddr + 0x0C);

        if (pChildParent != pThisNode) {
            const uintptr_t nodeAddr = reinterpret_cast<uintptr_t>(pThisNode);
            uint16_t& u16Cap = *reinterpret_cast<uint16_t*>(nodeAddr + 0x10);
            const uint16_t u16Count = *reinterpret_cast<const uint16_t*>(nodeAddr + 0x12);

            if (SceneNodePolicy::ShouldPreallocate(u16Count, u16Cap)) {
                const uint16_t u16NewCap = SceneNodePolicy::NextCapacity(u16Cap);
                void* pMemAdmin = fpMemAdminGetInstance();
                if (pMemAdmin) {
                    void**& ppChildren = *reinterpret_cast<void***>(nodeAddr + 0x14);
                    if (SceneNodePolicy::TryPreallocate(
                            pMemAdmin, ppChildren, u16Cap, u16NewCap, fpMemAdminRealloc)) {
                        s_nodePreallocations.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        }
    }

    return Real_AttachChild(pThisNode, pChildNode);
}

uint32_t SceneNodeOptimizer_GetPreallocations() noexcept {
    return s_nodePreallocations.load(std::memory_order_relaxed);
}

void InitializeSceneNodeOptimizer() {
    if (!GetConfig().enableSceneNodeOptimizer) {
        Log("G3O", "Scene Node Optimizer is disabled by configuration.");
        return;
    }

    HMODULE hSharedBase = GetModuleHandleA("SharedBase.dll");
    HMODULE hEngine     = GetModuleHandleA("Engine.dll");
    if (!hSharedBase || !hEngine) {
        Log("G3O", "Scene Node Optimizer: Required modules not found.");
        return;
    }

    fpMemAdminGetInstance = reinterpret_cast<bCMemoryAdmin_GetInstance_t>(
        GetProcAddress(hSharedBase, "?GetInstance@bCMemoryAdmin@@SGAAV1@XZ"));
    fpMemAdminRealloc = reinterpret_cast<bCMemoryAdmin_Realloc_t>(
        GetProcAddress(hSharedBase, "?Realloc@bCMemoryAdmin@@QAEPAXPAXK@Z"));

    if (!fpMemAdminGetInstance || !fpMemAdminRealloc) {
        Log("G3O", "Scene Node Optimizer: bCMemoryAdmin exports not resolved in SharedBase.dll.");
        return;
    }

    void* pTargetAttachChild = GetProcAddress(hEngine, "?AttachChild@eCNode@@UAEHPAV1@@Z");
    if (!pTargetAttachChild) {
        pTargetAttachChild = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(hEngine) + 0x0020DDF0);
    }

    if (pTargetAttachChild) {
        MH_STATUS createStatus = MH_OK, enableStatus = MH_OK, removeStatus = MH_OK;
        const HookInstallResult result = InstallHookSafely(
            [&] { createStatus = MH_CreateHook(pTargetAttachChild, reinterpret_cast<LPVOID>(Hooked_AttachChild),
                                               reinterpret_cast<LPVOID*>(&Real_AttachChild)); return createStatus == MH_OK; },
            [&] { enableStatus = MH_EnableHook(pTargetAttachChild); return enableStatus == MH_OK; },
            [&] { removeStatus = MH_RemoveHook(pTargetAttachChild); },
            [&] { Real_AttachChild = nullptr; });
        if (result == HookInstallResult::Installed) {
            s_targetAttachChild = pTargetAttachChild;
            Log("G3O", "Scene Node Optimizer: Hooked eCNode::AttachChild (0x%p)", pTargetAttachChild);
        } else {
            Log("G3O", "Scene Node Optimizer: AttachChild hook install failed (create=%d, enable=%d, remove=%d).",
                createStatus, enableStatus, removeStatus);
        }
    }
}

void ShutdownSceneNodeOptimizer() {
    if (s_targetAttachChild) {
        MH_DisableHook(s_targetAttachChild);
        MH_RemoveHook(s_targetAttachChild);
        s_targetAttachChild = nullptr;
        Real_AttachChild = nullptr;
    }
}

} // namespace G3O
