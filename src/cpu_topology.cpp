#include "cpu_topology.h"
#include "cpu_affinity_policy.h"
#include "config_manager.h"
#include "diagnostic_logger.h"
#include <windows.h>
#include <stddef.h>
#include <string.h>
#include <vector>

namespace G3O {

static DWORD_PTR g_pCoreMask = 0;

static bool IsGothic3Process() {
    char path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return false;
    const char* executable = strrchr(path, '\\');
    executable = executable ? executable + 1 : path;
    return _stricmp(executable, "Gothic3.exe") == 0;
}

DWORD_PTR GetPCoreAffinityMask() {
    return g_pCoreMask;
}

static bool ApplyAffinityMask(DWORD_PTR requested, DWORD_PTR available, const char* source) {
    if (!CpuAffinity::IsValidMask(static_cast<CpuAffinity::Mask>(requested),
                                  static_cast<CpuAffinity::Mask>(available))) {
        Log("G3O", "CPU Topology: %s mask 0x%IX is empty or includes unavailable processors; affinity unchanged.",
            source, requested);
        return false;
    }
    if (!SetProcessAffinityMask(GetCurrentProcess(), requested)) {
        const DWORD error = GetLastError();
        Log("G3O", "CPU Topology: applying %s mask 0x%IX failed (error %lu); affinity unchanged.",
            source, requested, error);
        return false;
    }
    g_pCoreMask = requested;
    Log("G3O", "CPU Topology: applied %s mask 0x%IX.", source, requested);
    return true;
}

void InitializeCPUTopology() {
    if (!IsGothic3Process()) {
        Log("G3O", "CPU Topology: affinity skipped because the host process is not Gothic3.exe.");
        return;
    }
    const DWORD activeProcessorCount = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    const unsigned affinityMaskBits = static_cast<unsigned>(sizeof(DWORD_PTR) * 8u);
    if (!CpuAffinity::CanRepresentProcessorCount(activeProcessorCount, affinityMaskBits)) {
        Log("G3O", "CPU Topology: %lu active processors cannot be represented by the %u-bit process affinity mask; affinity unchanged.",
            activeProcessorCount, affinityMaskBits);
        return;
    }
    const Config& cfg = GetConfig();
    g_pCoreMask = 0;
    DWORD_PTR processMask = 0;
    DWORD_PTR availableMask = 0;
    SetLastError(ERROR_SUCCESS);
    if (!GetProcessAffinityMask(GetCurrentProcess(), &processMask, &availableMask)) {
        Log("G3O", "CPU Topology: process/system affinity is unavailable; affinity unchanged (error %lu).",
            GetLastError());
        return;
    }
    if (processMask == 0 || availableMask == 0) {
        Log("G3O", "CPU Topology: process/system affinity is empty; affinity unchanged.");
        return;
    }

    if (cfg.cpuAffinityMask != 0) {
        ApplyAffinityMask(cfg.cpuAffinityMask, processMask, "manual");
        return;
    }

    GROUP_AFFINITY currentGroup = {};
    if (!GetThreadGroupAffinity(GetCurrentThread(), &currentGroup)) {
        Log("G3O", "CPU Topology: current processor group is unavailable; affinity unchanged (error %lu).",
            GetLastError());
        return;
    }
    DWORD length = 0;
    SetLastError(ERROR_SUCCESS);
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || length == 0) {
        Log("G3O", "CPU Topology: processor topology query failed (error %lu); affinity unchanged.", GetLastError());
        return;
    }

    BYTE* buffer = static_cast<BYTE*>(LocalAlloc(LPTR, length));
    if (!buffer) {
        Log("G3O", "CPU Topology: unable to allocate %lu bytes for processor topology.", length);
        return;
    }
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
            reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer), &length)) {
        const DWORD error = GetLastError();
        LocalFree(buffer);
        Log("G3O", "CPU Topology: processor topology query failed (error %lu); affinity unchanged.", error);
        return;
    }

    struct CoreInfo {
        DWORD_PTR mask;
        BYTE efficiencyClass;
    };
    std::vector<CoreInfo> cores;
    DWORD offset = 0;
    while (offset < length) {
        const auto* item = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer + offset);
        if (item->Size == 0 || item->Size > length - offset) break;
        if (item->Relationship == RelationProcessorCore) {
            const size_t masksOffset = offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Processor) +
                                       offsetof(PROCESSOR_RELATIONSHIP, GroupMask);
            const size_t requiredSize = masksOffset +
                static_cast<size_t>(item->Processor.GroupCount) * sizeof(GROUP_AFFINITY);
            if (requiredSize <= item->Size) {
                for (WORD i = 0; i < item->Processor.GroupCount; ++i) {
                    const GROUP_AFFINITY& group = item->Processor.GroupMask[i];
                    if (group.Group != currentGroup.Group) continue;
                    const DWORD_PTR mask = static_cast<DWORD_PTR>(group.Mask) & processMask;
                    if (mask != 0) cores.push_back({ mask, item->Processor.EfficiencyClass });
                }
            }
        }
        offset += item->Size;
    }
    LocalFree(buffer);

    if (cores.empty()) {
        Log("G3O", "CPU Topology: no cores found in processor group %u; affinity unchanged.", currentGroup.Group);
        return;
    }

    BYTE minEfficiencyClass = 0xFF;
    BYTE maxEfficiencyClass = 0;
    for (const CoreInfo& core : cores) {
        if (core.efficiencyClass < minEfficiencyClass) minEfficiencyClass = core.efficiencyClass;
        if (core.efficiencyClass > maxEfficiencyClass) maxEfficiencyClass = core.efficiencyClass;
    }
    const bool isHybrid = maxEfficiencyClass > minEfficiencyClass;

    std::vector<CpuAffinity::Mask> preferredCores;
    std::vector<CpuAffinity::Mask> otherCores;
    DWORD_PTR pCoreMask = 0;
    for (const CoreInfo& core : cores) {
        const auto mask = static_cast<CpuAffinity::Mask>(core.mask);
        if (!isHybrid || core.efficiencyClass == maxEfficiencyClass) {
            preferredCores.push_back(mask);
            pCoreMask |= core.mask;
        } else {
            otherCores.push_back(mask);
        }
    }

    if (cfg.cpuThreads > 0) {
        const DWORD_PTR mask = static_cast<DWORD_PTR>(CpuAffinity::SelectLogicalProcessors(
            static_cast<CpuAffinity::Mask>(processMask), static_cast<unsigned>(cfg.cpuThreads)));
        ApplyAffinityMask(mask, processMask, "CPUThreads");
        return;
    }

    if (cfg.cpuCores > 0) {
        preferredCores.insert(preferredCores.end(), otherCores.begin(), otherCores.end());
        const DWORD_PTR mask = static_cast<DWORD_PTR>(CpuAffinity::SelectPhysicalCores(
            preferredCores.data(), preferredCores.size(), static_cast<unsigned>(cfg.cpuCores),
            static_cast<CpuAffinity::Mask>(processMask)));
        ApplyAffinityMask(mask, processMask, "CPUCores");
        return;
    }

    if (isHybrid && pCoreMask != 0) {
        Log("G3O", "Hybrid CPU detected in group %u (%zu preferred cores, %zu other cores); selecting preferred-core mask 0x%IX.",
            currentGroup.Group, preferredCores.size(), otherCores.size(), pCoreMask);
        ApplyAffinityMask(pCoreMask, processMask, "automatic P-core");
    } else {
        g_pCoreMask = pCoreMask;
        Log("G3O", "Uniform CPU topology detected in group %u; automatic affinity left unchanged (mask 0x%IX).",
            currentGroup.Group, pCoreMask);
    }
}

} // namespace G3O
