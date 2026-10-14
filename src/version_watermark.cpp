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
                    swprintf_s(newBuf, L"%s | G3O v1.1.0", wstr);
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

// -----------------------------------------------------------------------------
// Hook 2: Engine.dll eCApplication::GetApplicationVersionString
// -----------------------------------------------------------------------------
typedef void* (__fastcall* Real_GetAppVer_t)(void* pThis, void* dummy, void* pOutString);
static Real_GetAppVer_t Real_GetAppVer = nullptr;

typedef void* (__fastcall* Real_bCString_Assign_t)(void* pThis, void* dummy, const char* str);
static Real_bCString_Assign_t Real_bCString_Assign = nullptr;

static void* __fastcall Hooked_GetApplicationVersionString(void* pThis, void* /*edx*/, void* pOutString) {
    void* result = Real_GetAppVer(pThis, nullptr, pOutString);
    if (result && pOutString && Real_bCString_Assign) {
        const char* const* ppStr = (const char* const*)pOutString;
        if (ppStr && *ppStr) {
            const char* orig = *ppStr;
            if (orig[0] != '\0' && !strstr(orig, "G3O")) {
                char buf[256];
                sprintf_s(buf, "%s | G3O v1.1.0", orig);
                Real_bCString_Assign(pOutString, nullptr, buf);
                OutputDebugStringA("[Gothic3Optima] Version string injected via GetApplicationVersionString: ");
                OutputDebugStringA(buf);
                OutputDebugStringA("\n");
            }
        }
    }
    return result;
}

void InitializeVersionHook() {
    HMODULE hSharedBase = GetModuleHandleA("SharedBase.dll");
    if (!hSharedBase) {
        hSharedBase = LoadLibraryA("SharedBase.dll");
    }
    if (hSharedBase) {
        Real_bCUnicodeString_Assign = (Real_bCUnicodeString_Assign_t)GetProcAddress(hSharedBase, "??4bCUnicodeString@@QAEAAV0@PB_W@Z");
        Real_bCString_Assign = (Real_bCString_Assign_t)GetProcAddress(hSharedBase, "??4bCString@@QAEAAV0@PBD@Z");
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
                OutputDebugStringA("[Gothic3Optima] Hooked CFFGFCWnd::SetWindowTextA.\n");
            }
        }
    }

    HMODULE hEngine = GetModuleHandleA("Engine.dll");
    if (!hEngine) {
        hEngine = LoadLibraryA("Engine.dll");
    }
    if (hEngine) {
        void* pGetAppVer = (void*)GetProcAddress(hEngine, "?GetApplicationVersionString@eCApplication@@QAE?AVbCString@@XZ");
        if (pGetAppVer) {
            if (MH_CreateHook(pGetAppVer, (LPVOID)Hooked_GetApplicationVersionString, (LPVOID*)&Real_GetAppVer) == MH_OK) {
                MH_EnableHook(pGetAppVer);
                OutputDebugStringA("[Gothic3Optima] Hooked eCApplication::GetApplicationVersionString.\n");
            }
        }
    }
}
