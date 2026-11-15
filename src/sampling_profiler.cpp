#include "sampling_profiler.h"
#include "diagnostic_logger.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace G3O {

namespace {

struct Session {
    HANDLE target = nullptr;
    HANDLE thread = nullptr;
    std::atomic<bool> stop{ false };
    std::string label;
    std::unordered_map<std::uint32_t, std::uint32_t> eipHits;
    std::unordered_map<std::uint64_t, std::uint32_t> callerHits; // eip + first return address
    std::uint32_t total = 0;
};

Session* g_session = nullptr;

bool ReadableRange(const void* p, SIZE_T n) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    const auto end = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return reinterpret_cast<std::uintptr_t>(p) + n <= end;
}

std::string Describe(std::uint32_t addr) {
    HMODULE mod = nullptr;
    char name[MAX_PATH] = "?";
    std::uint32_t rva = addr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(static_cast<std::uintptr_t>(addr)), &mod) && mod) {
        char full[MAX_PATH] = {};
        GetModuleFileNameA(mod, full, MAX_PATH);
        const char* base = strrchr(full, '\\');
        std::snprintf(name, sizeof(name), "%s", base ? base + 1 : full);
        rva = addr - static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(mod));
    }
    char out[MAX_PATH + 32];
    std::snprintf(out, sizeof(out), "%s+0x%X", name, rva);
    return out;
}

DWORD WINAPI SamplerThread(LPVOID param) {
    auto* s = static_cast<Session*>(param);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    while (!s->stop.load(std::memory_order_acquire)) {
        Sleep(1);
        if (SuspendThread(s->target) == static_cast<DWORD>(-1)) continue;
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (GetThreadContext(s->target, &ctx)) {
            const std::uint32_t eip = static_cast<std::uint32_t>(ctx.Eip);
            std::uint32_t ret = 0;
            const auto* ebp = reinterpret_cast<const std::uint32_t*>(static_cast<std::uintptr_t>(ctx.Ebp));
            if (ReadableRange(ebp, 8)) ret = ebp[1];
            ++s->total;
            ++s->eipHits[eip];
            ++s->callerHits[(static_cast<std::uint64_t>(eip) << 32) | ret];
        }
        ResumeThread(s->target);
    }
    return 0;
}

} // namespace

void SamplingProfiler_Start(const char* label) {
    if (g_session) return;
    auto* s = new Session();
    s->label = label ? label : "";
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                         &s->target, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                         FALSE, 0)) {
        delete s;
        return;
    }
    s->thread = CreateThread(nullptr, 0, SamplerThread, s, 0, nullptr);
    if (!s->thread) {
        CloseHandle(s->target);
        delete s;
        return;
    }
    g_session = s;
}

void SamplingProfiler_Stop() {
    Session* s = g_session;
    if (!s) return;
    g_session = nullptr;
    s->stop.store(true, std::memory_order_release);
    WaitForSingleObject(s->thread, 5000);
    CloseHandle(s->thread);
    CloseHandle(s->target);

    // Aggregate by module+function-ish bucket (RVA rounded to 64 bytes) to merge nearby EIPs.
    std::unordered_map<std::string, std::uint32_t> buckets;
    for (const auto& kv : s->eipHits) {
        std::string d = Describe(kv.first & ~63u);
        buckets[d] += kv.second;
    }
    std::vector<std::pair<std::string, std::uint32_t>> v(buckets.begin(), buckets.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });

    Log("G3O", "[Sampler] '%s': %u samples (~1 ms each). Top hot spots (64-byte buckets):",
        s->label.c_str(), s->total);
    for (size_t i = 0; i < v.size() && i < 40; ++i) {
        Log("G3O", "[Sampler]   %5.1f%% %6u  %s", 100.0 * v[i].second / (s->total ? s->total : 1),
            v[i].second, v[i].first.c_str());
    }

    // Module totals
    std::unordered_map<std::string, std::uint32_t> mods;
    for (const auto& kv : s->eipHits) {
        std::string d = Describe(kv.first);
        mods[d.substr(0, d.find('+'))] += kv.second;
    }
    std::vector<std::pair<std::string, std::uint32_t>> mv(mods.begin(), mods.end());
    std::sort(mv.begin(), mv.end(), [](auto& a, auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < mv.size() && i < 12; ++i) {
        Log("G3O", "[Sampler]   module %-24s %5.1f%% (%u)", mv[i].first.c_str(),
            100.0 * mv[i].second / (s->total ? s->total : 1), mv[i].second);
    }

    // Top callers (EIP bucket <- return address), to show who drives the hot code.
    std::unordered_map<std::string, std::uint32_t> callers;
    for (const auto& kv : s->callerHits) {
        const std::uint32_t eip = static_cast<std::uint32_t>(kv.first >> 32);
        const std::uint32_t ret = static_cast<std::uint32_t>(kv.first & 0xFFFFFFFFu);
        callers[Describe(eip & ~63u) + " <- " + Describe(ret & ~63u)] += kv.second;
    }
    std::vector<std::pair<std::string, std::uint32_t>> cv(callers.begin(), callers.end());
    std::sort(cv.begin(), cv.end(), [](auto& a, auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < cv.size() && i < 25; ++i) {
        Log("G3O", "[Sampler]   caller %5.1f%% %6u  %s", 100.0 * cv[i].second / (s->total ? s->total : 1),
            cv[i].second, cv[i].first.c_str());
    }
    delete s;
}

} // namespace G3O
