#include "load_sampler.h"
#include <algorithm>
#include <stdint.h>
#include <cstring>

namespace G3O {
namespace {
constexpr unsigned kCapacity = 32768;
DWORD samples[kCapacity];
unsigned count = 0;
unsigned dropped = 0;
HANDLE target = nullptr;
HANDLE sampler = nullptr;
HANDLE stopEvent = nullptr;

DWORD WINAPI SampleThread(void*) {
    while (WaitForSingleObject(stopEvent, 2) == WAIT_TIMEOUT) {
        if (SuspendThread(target) == DWORD(-1)) continue;
        CONTEXT context = {};
        context.ContextFlags = CONTEXT_CONTROL;
        BOOL ok = GetThreadContext(target, &context);
        ResumeThread(target);
        if (ok && context.Eip) {
            if (count < kCapacity) samples[count++] = context.Eip;
            else ++dropped;
        }
    }
    return 0;
}
}

bool StartLoadSampling(DWORD threadId) {
    if (!threadId || sampler) return false;
    target = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, threadId);
    if (!target) return false;
    stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent) { CloseHandle(target); target = nullptr; return false; }
    count = 0;
    dropped = 0;
    sampler = CreateThread(nullptr, 0, SampleThread, nullptr, 0, nullptr);
    if (!sampler) {
        CloseHandle(stopEvent); CloseHandle(target);
        stopEvent = target = nullptr;
        return false;
    }
    return true;
}

unsigned StopLoadSampling(FILE* report) {
    if (!sampler) return 0;
    SetEvent(stopEvent);
    WaitForSingleObject(sampler, INFINITE);
    CloseHandle(sampler); CloseHandle(stopEvent); CloseHandle(target);
    sampler = stopEvent = target = nullptr;
    unsigned total = count;
    if (report) {
        fprintf(report, "Sampled loading thread, module+RVA (instruction buckets, not function names): %u samples, %u dropped\n", total, dropped);
        std::sort(samples, samples + total);
        struct Entry { DWORD address; unsigned hits; } top[30] = {};
        for (unsigned i = 0; i < total;) {
            DWORD bucket = samples[i] & ~DWORD(15);
            unsigned j = i + 1;
            while (j < total && (samples[j] & ~DWORD(15)) == bucket) ++j;
            Entry e = {bucket, j - i};
            for (unsigned k = 0; k < 30; ++k) if (e.hits > top[k].hits) {
                for (unsigned m = 29; m > k; --m) top[m] = top[m - 1];
                top[k] = e; break;
            }
            i = j;
        }
        for (const auto& e : top) {
            if (!e.hits) break;
            HMODULE module = nullptr;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(e.address)), &module)) {
                char path[MAX_PATH] = {};
                GetModuleFileNameA(module, path, MAX_PATH);
                const char* name = strrchr(path, '\\');
                fprintf(report, "%5u %6.2f%% %s+0x%X\n", e.hits, 100.0 * e.hits / total, name ? name + 1 : path, e.address - (DWORD)(uintptr_t)module);
            } else {
                fprintf(report, "%5u %6.2f%% unknown:0x%08X\n", e.hits, 100.0 * e.hits / total, e.address);
            }
        }
        fflush(report);
    }
    count = 0;
    return total;
}
}
