#include "cpu_topology.h"
#include "config_manager.h"
#include "diagnostic_logger.h"
#include <windows.h>
#include <stdio.h>

namespace G3O {

static DWORD_PTR g_pCoreMask = 0;
static DWORD_PTR g_allCoresMask = 0;
static bool g_isHybrid = false;

DWORD_PTR GetPCoreAffinityMask() {
    return g_pCoreMask;
}

void InitializeCPUTopology() {
    const G3O::Config& cfg = G3O::GetConfig();

    // 1. Manual Affinity Mask override (if specified in G3O.ini, e.g. CPUAffinityMask=0xFFFFF)
    if (cfg.cpuAffinityMask != 0) {
        g_pCoreMask = cfg.cpuAffinityMask;
        g_allCoresMask = cfg.cpuAffinityMask;
        SetProcessAffinityMask(GetCurrentProcess(), g_pCoreMask);
        Log("G3O", "CPU Topology: Manual affinity mask override from G3O.ini: 0x%IX", g_pCoreMask);
        return;
    }

    // 2. Manual Thread Count override (if specified in G3O.ini, e.g. CPUThreads=20)
    int targetThreads = cfg.cpuThreads > 0 ? cfg.cpuThreads : (cfg.cpuCores > 0 ? cfg.cpuCores : 0);
    if (targetThreads > 0) {
        DWORD_PTR mask = 0;
        int count = (targetThreads > 64) ? 64 : targetThreads;
        for (int i = 0; i < count; ++i) {
            mask |= ((DWORD_PTR)1 << i);
        }
        g_pCoreMask = mask;
        g_allCoresMask = mask;
        SetProcessAffinityMask(GetCurrentProcess(), g_pCoreMask);
        Log("G3O", "CPU Topology: Config override applied: %d threads (%d cores requested). Affinity mask: 0x%IX",
            targetThreads, cfg.cpuCores > 0 ? cfg.cpuCores : targetThreads, g_pCoreMask);
        return;
    }

    // 3. Auto-detection (RelationProcessorCore)
    DWORD length = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &length);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || length == 0) {
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

    if (isHybrid) {
        Log("G3O", "Hybrid CPU detected! P-cores: %d, E-cores: %d. Locking process to P-core mask: 0x%IX",
            pCoreCount, eCoreCount, g_pCoreMask);
        SetProcessAffinityMask(GetCurrentProcess(), g_pCoreMask);
    } else {
        Log("G3O", "Uniform CPU topology detected (%d cores). Affinity mask: 0x%IX",
            pCoreCount, g_pCoreMask);
    }
}

} // namespace G3O
