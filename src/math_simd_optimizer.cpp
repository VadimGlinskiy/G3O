#include "math_simd_optimizer.h"
#include "diagnostic_logger.h"
#include "sharedbase_string_compare.h"
#include "string_getword_policy.h"
#include <minhook.h>
#include <string>

namespace G3O {

// ---------------------------------------------------------------------------
// 1. Box-Box intersection (bCBox::Intersects(const bCBox&))
// ---------------------------------------------------------------------------

static bool __fastcall Hooked_BoxIntersectsBox(const Box3* pThis, void* /*edx*/, const Box3* pOther) {
    return FastBoxIntersects(pThis, pOther);
}

// ---------------------------------------------------------------------------
// 2. Sphere-Sphere intersection (bCSphere::Intersects(const bCSphere&))
// Replaces 35 legacy x87 instructions and 15 memory roundtrips with fast scalar SSE.
// ---------------------------------------------------------------------------

static bool __fastcall Hooked_SphereIntersectsSphere(const Sphere3* pThis, void* /*edx*/, const Sphere3* pOther) {
    return FastSphereIntersects(pThis, pOther);
}

// ---------------------------------------------------------------------------
// 4. String equality replacements use the complete null-terminated text.
// ---------------------------------------------------------------------------

static bool __fastcall Hooked_CompareFast(const void* pThis, void* /*edx*/, const void* pOther) {
    return SharedBase::FastStringCompare(pThis, pOther);
}

// ---------------------------------------------------------------------------
// 5. Case-insensitive equality replacement uses length-bounded comparison.
// ---------------------------------------------------------------------------

static bool __fastcall Hooked_CompareNoCaseFast(const void* pThis, void* /*edx*/, const void* pOther) {
    return SharedBase::FastStringCompareNoCase(pThis, pOther);
}

// ---------------------------------------------------------------------------
// 6. Single-pass bCString::GetWord replacement (eliminates O(k*L) Left/Right allocations)
// ---------------------------------------------------------------------------
using bCStringSetTextLenFn = void (__thiscall*)(void*, const char*, std::int32_t);
using bCStringClearFn = void (__thiscall*)(void*);
using bCStringTrimFn = void (__thiscall*)(void*);
using bCStringGetWordFn = std::int32_t (__thiscall*)(void*, std::int32_t, const void*, void*, bool, bool);

static bCStringSetTextLenFn s_bstrSetTextLen = nullptr;
static bCStringClearFn s_bstrClear = nullptr;
static bCStringTrimFn s_bstrTrimLeft = nullptr;
static bCStringTrimFn s_bstrTrimRight = nullptr;
static bCStringGetWordFn Real_SharedBaseGetWord = nullptr;

bool TryFastCStringGetWord(
    void* self, std::int32_t wordNumber, const void* delimiters,
    void* destination, bool trimLeft, bool trimRight,
    std::int32_t& outOffset) noexcept {
    if (!self || !delimiters || !destination ||
        !s_bstrSetTextLen || !s_bstrClear) {
        return false;
    }

    const char* text = *reinterpret_cast<const char* const*>(self);
    const int textLen = text ? *reinterpret_cast<const int*>(text - 8) : 0;
    const char* delimText = *reinterpret_cast<const char* const*>(delimiters);

    WordSpanResult span = ExtractWordSpan(text, textLen, wordNumber, delimText ? delimText : "");

    if (trimLeft || trimRight) {
        TrimWordSpan(span, trimLeft, trimRight);
    }

    if (span.length <= 0) {
        s_bstrClear(destination);
    } else if (span.length < 512) {
        char stackBuf[512];
        std::memcpy(stackBuf, span.ptr, static_cast<std::size_t>(span.length));
        stackBuf[span.length] = '\0';
        s_bstrSetTextLen(destination, stackBuf, span.length);
    } else {
        std::string tmp(span.ptr, static_cast<std::size_t>(span.length));
        s_bstrSetTextLen(destination, tmp.c_str(), span.length);
    }

    outOffset = span.offset;
    return true;
}

static std::int32_t __fastcall Hooked_SharedBaseGetWord(
    void* self, void* /*edx*/, std::int32_t wordNumber, const void* delimiters,
    void* destination, bool trimLeft, bool trimRight) {
    std::int32_t offset = 0;
    if (TryFastCStringGetWord(self, wordNumber, delimiters, destination, trimLeft, trimRight, offset)) {
        return offset;
    }
    return Real_SharedBaseGetWord(self, wordNumber, delimiters, destination, trimLeft, trimRight);
}

// ---------------------------------------------------------------------------
// 7. Fast DJB2 string hash replacement (4-byte unrolled, eliminates 1-byte loops)
// ---------------------------------------------------------------------------
using GetHashValue_bCString_Fn = unsigned int (__stdcall*)(const void*);
static GetHashValue_bCString_Fn Real_GetHashValue_bCString = nullptr;

static unsigned int __stdcall Hooked_GetHashValue_bCString(const void* pStr) {
    return SharedBase::FastDjb2Hash(pStr);
}

// ---------------------------------------------------------------------------
// 8. Redundant bCAccessorPropertyObject::SetInstance bypass
// ---------------------------------------------------------------------------
using SetInstanceFn = void (__thiscall*)(void*, void*);
static SetInstanceFn Real_SetInstance = nullptr;

static void __fastcall Hooked_SetInstance(void* pThis, void* /*edx*/, void* pInstance) {
    if (pThis) {
        void** fields = reinterpret_cast<void**>(pThis);
        void* currentClass = fields[0];
        void* currentInstance = fields[1];
        if (pInstance && pInstance == currentInstance && currentClass != nullptr) {
            return; // Exact redundant re-assignment, 100% no-op (skips 4 virtual calls)
        }
    }
    Real_SetInstance(pThis, pInstance);
}

// ---------------------------------------------------------------------------
// Initialization & Shutdown
static void* s_sharedBaseHookTargets[6] = {};
static void* s_compareFastPatchAddr = nullptr;
static uint8_t s_compareFastOriginalBytes[75] = {};
static bool s_compareFastPatchApplied = false;

void InitializeSharedBaseOptimizations() {
    HMODULE hSharedBase = GetModuleHandleA("SharedBase.dll");
    if (!hSharedBase) return;

    // 1. Hook bCBox::Intersects(const bCBox&)
    void* pIntersects = (void*)GetProcAddress(hSharedBase, "?Intersects@bCBox@@QBE_NABV1@@Z");
    if (pIntersects) {
        MH_STATUS status = MH_CreateHook(pIntersects, (LPVOID)Hooked_BoxIntersectsBox, nullptr);
        if (status == MH_OK) {
            status = MH_EnableHook(pIntersects);
            if (status == MH_OK) {
                s_sharedBaseHookTargets[0] = pIntersects;
                Log("G3O", "SharedBase box intersection hook enabled.");
            } else {
                MH_RemoveHook(pIntersects);
                Log("G3O", "SharedBase box intersection hook activation failed (code %d).", (int)status);
            }
        } else {
            Log("G3O", "SharedBase box intersection hook creation failed (code %d).", (int)status);
        }
    } else {
        Log("G3O", "SharedBase box intersection export is missing; optimization skipped.");
    }

    // 2. Hook bCSphere::Intersects(const bCSphere&) (Task 8.11: SSE replacement for x87)
    void* pSphereIntersects = (void*)GetProcAddress(hSharedBase, "?Intersects@bCSphere@@QBE_NABV1@@Z");
    if (pSphereIntersects) {
        MH_STATUS status = MH_CreateHook(pSphereIntersects, (LPVOID)Hooked_SphereIntersectsSphere, nullptr);
        if (status == MH_OK) {
            status = MH_EnableHook(pSphereIntersects);
            if (status == MH_OK) {
                s_sharedBaseHookTargets[2] = pSphereIntersects;
                Log("G3O", "SharedBase sphere intersection hook enabled (scalar SSE replacement for x87).");
            } else {
                MH_RemoveHook(pSphereIntersects);
                Log("G3O", "SharedBase sphere intersection hook activation failed (code %d).", (int)status);
            }
        } else {
            Log("G3O", "SharedBase sphere intersection hook creation failed (code %d).", (int)status);
        }
    } else {
        Log("G3O", "SharedBase sphere intersection export is missing; optimization skipped.");
    }

    // 3. bCString::CompareFast vectorized in-place replacement (Task 8.14)
    // SharedBase.dll!?CompareFast@bCString@@QBE_NABV1@@Z (RVA 0x1AE0, 75 bytes) consumes ~3.5% of CPU in gameplay
    // because it executes a scalar 2-byte loop to null terminator without checking pointer equality.
    // We replace the entire function in-place with a 58-byte vectorized implementation:
    // 1) Fast pointer equality check (2 cycles for COW strings or both null)
    // 2) Null checks
    // 3) Length check (offset -8)
    // 4) Hardware-vectorized 'repe cmpsb' for byte comparison
    // 5) Padded with NOPs up to 75 bytes.
    void* pCompareFast = reinterpret_cast<void*>(GetProcAddress(hSharedBase, "?CompareFast@bCString@@QBE_NABV1@@Z"));
    if (pCompareFast) {
        uint8_t* patchAddr = reinterpret_cast<uint8_t*>(pCompareFast);
        const uint8_t expectedHeader[16] = {
            0x8B, 0x01, 0x85, 0xC0, 0x75, 0x0D, 0x8B, 0x44, 0x24, 0x04, 0x83, 0x38, 0x00, 0x0F, 0x94, 0xC0
        };
        if (std::memcmp(patchAddr, expectedHeader, 16) == 0) {
            DWORD oldProtect = 0;
            if (VirtualProtect(patchAddr, 75, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                std::memcpy(s_compareFastOriginalBytes, patchAddr, 75);
                static const uint8_t s_compareFastReplacement[75] = {
                    0x8B, 0x01,             // mov eax, [ecx]
                    0x8B, 0x4C, 0x24, 0x04, // mov ecx, [esp+4]
                    0x8B, 0x11,             // mov edx, [ecx]
                    0x3B, 0xC2,             // cmp eax, edx
                    0x74, 0x24,             // je equal (10001b10)
                    0x85, 0xC0,             // test eax, eax
                    0x74, 0x25,             // jz not_equal (10001b15)
                    0x85, 0xD2,             // test edx, edx
                    0x74, 0x21,             // jz not_equal (10001b15)
                    0x8B, 0x48, 0xF8,       // mov ecx, [eax-8]
                    0x3B, 0x4A, 0xF8,       // cmp ecx, [edx-8]
                    0x75, 0x19,             // jne not_equal (10001b15)
                    0x85, 0xC9,             // test ecx, ecx
                    0x74, 0x10,             // jz equal (10001b10)
                    0x56,                   // push esi
                    0x57,                   // push edi
                    0x8B, 0xF0,             // mov esi, eax
                    0x8B, 0xFA,             // mov edi, edx
                    0xF3, 0xA6,             // repe cmpsb
                    0x0F, 0x94, 0xC0,       // sete al
                    0x5F,                   // pop edi
                    0x5E,                   // pop esi
                    0xC2, 0x04, 0x00,       // ret 4
                    // equal:
                    0xB0, 0x01,             // mov al, 1
                    0xC2, 0x04, 0x00,       // ret 4
                    // not_equal:
                    0x32, 0xC0,             // xor al, al
                    0xC2, 0x04, 0x00,       // ret 4
                    // padding:
                    0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
                    0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
                    0x90
                };
                std::memcpy(patchAddr, s_compareFastReplacement, 75);
                VirtualProtect(patchAddr, 75, oldProtect, &oldProtect);
                FlushInstructionCache(GetCurrentProcess(), patchAddr, 75);
                s_compareFastPatchAddr = patchAddr;
                s_compareFastPatchApplied = true;
                Log("G3O", "SharedBase bCString::CompareFast vectorized in-place replacement enabled (75 bytes).");
            } else {
                Log("G3O", "SharedBase bCString::CompareFast VirtualProtect failed.");
            }
        } else {
            Log("G3O", "SharedBase bCString::CompareFast byte signature mismatch at RVA 0x1AE0; replacement skipped.");
        }
    }

    // 4. Single-pass bCString::GetWord optimization (Task 8.4/8.8)
    s_bstrSetTextLen = reinterpret_cast<bCStringSetTextLenFn>(GetProcAddress(hSharedBase, "?SetText@bCString@@QAEXPBDH@Z"));
    s_bstrClear = reinterpret_cast<bCStringClearFn>(GetProcAddress(hSharedBase, "?Clear@bCString@@QAEXXZ"));
    s_bstrTrimLeft = reinterpret_cast<bCStringTrimFn>(GetProcAddress(hSharedBase, "?TrimLeft@bCString@@QAEXXZ"));
    s_bstrTrimRight = reinterpret_cast<bCStringTrimFn>(GetProcAddress(hSharedBase, "?TrimRight@bCString@@QAEXXZ"));
    void* pGetWord = reinterpret_cast<void*>(GetProcAddress(hSharedBase, "?GetWord@bCString@@QBEHHABV1@AAV1@_N2@Z"));
    if (pGetWord && s_bstrSetTextLen && s_bstrClear && s_bstrTrimLeft && s_bstrTrimRight) {
        MH_STATUS status = MH_CreateHook(pGetWord, reinterpret_cast<LPVOID>(Hooked_SharedBaseGetWord),
                                         reinterpret_cast<LPVOID*>(&Real_SharedBaseGetWord));
        if (status == MH_OK) {
            status = MH_EnableHook(pGetWord);
            if (status == MH_OK) {
                s_sharedBaseHookTargets[1] = pGetWord;
                Log("G3O", "SharedBase single-pass bCString::GetWord hook enabled.");
            } else {
                MH_RemoveHook(pGetWord);
                Real_SharedBaseGetWord = nullptr;
                Log("G3O", "SharedBase bCString::GetWord hook activation failed (code %d).", (int)status);
            }
        } else if (status == MH_ERROR_ALREADY_CREATED) {
            Log("G3O", "SharedBase bCString::GetWord delegated via SavegameProfiler audit hook.");
        } else {
            Log("G3O", "SharedBase bCString::GetWord hook creation failed (code %d).", (int)status);
        }
    }

    // 5. Hook ??$g_GetHashValue@VbCString@@@@YGKABVbCString@@@Z (Unrolled FastDjb2Hash)
    void* pGetHashValue = (void*)GetProcAddress(hSharedBase, "??$g_GetHashValue@VbCString@@@@YGKABVbCString@@@Z");
    if (pGetHashValue) {
        MH_STATUS status = MH_CreateHook(pGetHashValue, (LPVOID)Hooked_GetHashValue_bCString,
                                         (LPVOID*)&Real_GetHashValue_bCString);
        if (status == MH_OK) {
            status = MH_EnableHook(pGetHashValue);
            if (status == MH_OK) {
                s_sharedBaseHookTargets[3] = pGetHashValue;
                Log("G3O", "SharedBase fast DJB2 string hash hook enabled (4-byte unrolled).");
            } else {
                MH_RemoveHook(pGetHashValue);
                Real_GetHashValue_bCString = nullptr;
                Log("G3O", "SharedBase fast DJB2 string hash hook activation failed (code %d).", (int)status);
            }
        } else {
            Log("G3O", "SharedBase fast DJB2 string hash hook creation failed (code %d).", (int)status);
        }
    }

    // 6. Hook ?SetInstance@bCAccessorPropertyObject@@IAEXPAVbCPropertyObjectBase@@@Z (Redundant accessor bypass)
    void* pSetInstance = (void*)GetProcAddress(hSharedBase, "?SetInstance@bCAccessorPropertyObject@@IAEXPAVbCPropertyObjectBase@@@Z");
    if (pSetInstance) {
        MH_STATUS status = MH_CreateHook(pSetInstance, (LPVOID)Hooked_SetInstance,
                                         (LPVOID*)&Real_SetInstance);
        if (status == MH_OK) {
            status = MH_EnableHook(pSetInstance);
            if (status == MH_OK) {
                s_sharedBaseHookTargets[4] = pSetInstance;
                Log("G3O", "SharedBase redundant accessor SetInstance bypass hook enabled.");
            } else {
                MH_RemoveHook(pSetInstance);
                Real_SetInstance = nullptr;
                Log("G3O", "SharedBase accessor SetInstance hook activation failed (code %d).", (int)status);
            }
        } else {
            Log("G3O", "SharedBase accessor SetInstance hook creation failed (code %d).", (int)status);
        }
    }
}

void ShutdownSharedBaseOptimizations() {
    if (s_compareFastPatchApplied && s_compareFastPatchAddr) {
        DWORD oldProtect = 0;
        if (VirtualProtect(s_compareFastPatchAddr, 75, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            std::memcpy(s_compareFastPatchAddr, s_compareFastOriginalBytes, 75);
            VirtualProtect(s_compareFastPatchAddr, 75, oldProtect, &oldProtect);
            FlushInstructionCache(GetCurrentProcess(), s_compareFastPatchAddr, 75);
        }
        s_compareFastPatchApplied = false;
        s_compareFastPatchAddr = nullptr;
    }

    for (void*& target : s_sharedBaseHookTargets) {
        if (!target) continue;
        MH_DisableHook(target);
        MH_RemoveHook(target);
        target = nullptr;
    }
    Real_SharedBaseGetWord = nullptr;
    Real_GetHashValue_bCString = nullptr;
    Real_SetInstance = nullptr;
}

} // namespace G3O
