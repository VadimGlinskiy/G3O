#include "cpu_topology.h"
#include <minhook.h>
#include <stdio.h>
#include <vector>

static DWORD_PTR g_pCoreMask = 0;
static DWORD_PTR g_allCoresMask = 0;
static bool g_isHybrid = false;

typedef HANDLE (WINAPI* CreateThread_t)(
    LPSECURITY_ATTRIBUTES lpThreadAttributes,
    SIZE_T dwStackSize,
    LPTHREAD_START_ROUTINE lpStartAddress,
    LPVOID lpParameter,
    DWORD dwCreationFlags,
    LPDWORD lpThreadId
);
static CreateThread_t Real_CreateThread = NULL;

static HANDLE WINAPI Hooked_CreateThread(
    LPSECURITY_ATTRIBUTES lpThreadAttributes,
    SIZE_T dwStackSize,
    LPTHREAD_START_ROUTINE lpStartAddress,
    LPVOID lpParameter,
    DWORD dwCreationFlags,
    LPDWORD lpThreadId
) {
    HANDLE hThread = Real_CreateThread(lpThreadAttributes, dwStackSize, lpStartAddress, lpParameter, dwCreationFlags, lpThreadId);
    if (hThread && g_isHybrid && g_pCoreMask != 0) {
        // Ensure new thread is explicitly locked to P-cores
        SetThreadAffinityMask(hThread, g_pCoreMask);
    }
    return hThread;
}

DWORD_PTR GetPCoreAffinityMask() {
    return g_pCoreMask;
}

void InitializeCPUTopology() {
    DWORD length = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &length);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || length == 0) {
        OutputDebugStringA("[Gothic3Optima] GetLogicalProcessorInformationEx not supported or failed.\n");
        return;
    }

    std::vector<BYTE> buffer(length);
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX info = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buffer.data();

    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, info, &length)) {
        OutputDebugStringA("[Gothic3Optima] Failed to read processor topology.\n");
        return;
    }

    BYTE maxEfficiencyClass = 0;
    BYTE minEfficiencyClass = 0xFF;

    // First pass: find min/max EfficiencyClass
    DWORD offset = 0;
    while (offset < length) {
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX item = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buffer.data() + offset);
        if (item->Relationship == RelationProcessorCore) {
            BYTE eff = item->Processor.EfficiencyClass;
            if (eff > maxEfficiencyClass) maxEfficiencyClass = eff;
            if (eff < minEfficiencyClass) minEfficiencyClass = eff;
        }
        offset += item->Size;
    }

    bool isHybrid = (maxEfficiencyClass > minEfficiencyClass);
    g_isHybrid = isHybrid;

    DWORD_PTR pCoreMask = 0;
    DWORD_PTR allMask = 0;
    int pCoreCount = 0;
    int eCoreCount = 0;

    // Second pass: construct mask
    offset = 0;
    while (offset < length) {
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX item = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buffer.data() + offset);
        if (item->Relationship == RelationProcessorCore) {
            for (WORD i = 0; i < item->Processor.GroupCount; ++i) {
                KAFFINITY mask = item->Processor.GroupMask[i].Mask;
                allMask |= (DWORD_PTR)mask;
                if (!isHybrid || item->Processor.EfficiencyClass == maxEfficiencyClass) {
                    pCoreMask |= (DWORD_PTR)mask;
                    pCoreCount++;
                } else {
                    eCoreCount++;
                }
            }
        }
        offset += item->Size;
    }

    g_allCoresMask = allMask;
    g_pCoreMask = (pCoreMask != 0) ? pCoreMask : allMask;

    char logBuf[256];
    if (isHybrid) {
        sprintf_s(logBuf, "[Gothic3Optima] Hybrid CPU detected! P-cores: %d, E-cores: %d. Locking process to P-core mask: 0x%IX\n",
            pCoreCount, eCoreCount, g_pCoreMask);
        OutputDebugStringA(logBuf);
        SetProcessAffinityMask(GetCurrentProcess(), g_pCoreMask);
    } else {
        sprintf_s(logBuf, "[Gothic3Optima] Uniform CPU topology detected (%d cores). Affinity mask: 0x%IX\n",
            pCoreCount, g_pCoreMask);
        OutputDebugStringA(logBuf);
    }

    // Hook CreateThread to enforce affinity on dynamically created threads
    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    if (hKernel32) {
        void* pCreateThread = (void*)GetProcAddress(hKernel32, "CreateThread");
        if (pCreateThread) {
            if (MH_CreateHook(pCreateThread, (LPVOID)Hooked_CreateThread, (LPVOID*)&Real_CreateThread) == MH_OK) {
                MH_EnableHook(pCreateThread);
                OutputDebugStringA("[Gothic3Optima] Hooked CreateThread for thread affinity enforcement.\n");
            }
        }
    }
}
