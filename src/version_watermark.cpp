#include "version_watermark.h"
#include <windows.h>
#include <cwchar>
#include <cstdio>
#include <cstring>
#include <minhook.h>

// -----------------------------------------------------------------------------
// Hook 1: GFC.dll CFFGFCWnd::SetWindowTextA (Catches UI widget updates)
// -----------------------------------------------------------------------------
typedef void (__fastcall* Real_SetWindowTextA_t)(void* pThis, void* dummy, void* pUnicodeStr);
static Real_SetWindowTextA_t Real_SetWindowTextA = nullptr;

typedef void* (__fastcall* Real_bCUnicodeString_Assign_t)(void* pThis, void* dummy, const wchar_t* wstr);
static Real_bCUnicodeString_Assign_t Real_bCUnicodeString_Assign = nullptr;

static void __fastcall Hooked_SetWindowTextA(void* pThis, void* /*edx*/, void* pUnicodeStr) {
    if (pUnicodeStr && Real_bCUnicodeString_Assign) {
        const wchar_t* const* ppStr = (const wchar_t* const*)pUnicodeStr;
        if (ppStr && *ppStr) {
            const wchar_t* wstr = *ppStr;
            if (wstr[0] != L'\0' && !wcsstr(wstr, L"G3O")) {
                if ((iswdigit(wstr[0]) && wcsstr(wstr, L"1.")) || wcsstr(wstr, L"CP ") || wcsstr(wstr, L"1.75")) {
                    wchar_t newBuf[256];
                    swprintf_s(newBuf, L"%s | G3O v1.5.0", wstr);
                    Real_bCUnicodeString_Assign(pUnicodeStr, nullptr, newBuf);
                    OutputDebugStringW(L"[Gothic3Optima] Main menu version watermark injected via SetWindowTextA: ");
                    OutputDebugStringW(newBuf);
                    OutputDebugStringW(L"\n");
                }
            }
        }
    }
    Real_SetWindowTextA(pThis, nullptr, pUnicodeStr);
}

void InitializeVersionHook() {
    HMODULE hSharedBase = GetModuleHandleA("SharedBase.dll");
    if (!hSharedBase) {
        hSharedBase = LoadLibraryA("SharedBase.dll");
    }
    if (hSharedBase) {
        Real_bCUnicodeString_Assign = (Real_bCUnicodeString_Assign_t)GetProcAddress(hSharedBase, "??4bCUnicodeString@@QAEAAV0@PB_W@Z");
    }

    HMODULE hGFC = GetModuleHandleA("GFC.dll");
    if (!hGFC) {
        hGFC = LoadLibraryA("GFC.dll");
    }
    if (hGFC) {
        void* pSetWindowText = (void*)GetProcAddress(hGFC, "?SetWindowTextA@CFFGFCWnd@@QAEXABVbCUnicodeString@@@Z");
        if (pSetWindowText) {
            if (MH_CreateHook(pSetWindowText, (LPVOID)Hooked_SetWindowTextA, (LPVOID*)&Real_SetWindowTextA) == MH_OK) {
                MH_EnableHook(pSetWindowText);
                OutputDebugStringA("[Gothic3Optima] Hooked CFFGFCWnd::SetWindowTextA (Main Menu Watermark).\n");
            }
        }
    }
}
