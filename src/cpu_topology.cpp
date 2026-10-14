#include "cpu_topology.h"
#include <windows.h>
#include <stdio.h>

static DWORD_PTR g_pCoreMask = 0;
static DWORD_PTR g_allCoresMask = 0;
static bool g_isHybrid = false;

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

    BYTE stackBuf[2048];
    BYTE* pBuf = stackBuf;
    if (length > sizeof(stackBuf)) {
        pBuf = (BYTE*)LocalAlloc(LPTR, length);
        if (!pBuf) return;
    }

    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX info = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)pBuf;

    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, info, &length)) {
        OutputDebugStringA("[Gothic3Optima] Failed to read processor topology.\n");
        if (pBuf != stackBuf) LocalFree(pBuf);
        return;
    }

    BYTE maxEfficiencyClass = 0;
    BYTE minEfficiencyClass = 0xFF;

    // First pass: find min/max EfficiencyClass
    DWORD offset = 0;
    while (offset < length) {
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX item = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(pBuf + offset);
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
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX item = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(pBuf + offset);
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

    if (pBuf != stackBuf) {
        LocalFree(pBuf);
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
}
