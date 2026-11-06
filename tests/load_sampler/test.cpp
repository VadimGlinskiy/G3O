#include "load_sampler.h"
#include <windows.h>
#include <cstdio>
#include <cstring>

static volatile LONG stopWorker = 0;
static DWORD WINAPI BusyWorker(void*) {
    volatile unsigned value = 0;
    while (InterlockedCompareExchange(&stopWorker, 0, 0) == 0) ++value;
    return value;
}

int main() {
    DWORD tid = 0;
    HANDLE worker = CreateThread(nullptr, 0, BusyWorker, nullptr, 0, &tid);
    if (!worker) return 1;
    FILE* report = tmpfile();
    if (!report) return 2;
    if (G3O::StartLoadSampling(0)) return 3;
    for (int i = 0; i < 2; ++i) {
        if (!G3O::StartLoadSampling(tid)) return 4;
        Sleep(150);
        if (G3O::StopLoadSampling(report) == 0) return 5;
        if (G3O::StopLoadSampling(report) != 0) return 6;
    }
    if (!G3O::StartLoadSampling(GetCurrentThreadId())) return 9;
    LARGE_INTEGER frequency, start, now;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    do { QueryPerformanceCounter(&now); } while (now.QuadPart - start.QuadPart < frequency.QuadPart / 8);
    if (G3O::StopLoadSampling(report) == 0) return 10;
    InterlockedExchange(&stopWorker, 1);
    if (WaitForSingleObject(worker, 3000) != WAIT_OBJECT_0) return 7;
    CloseHandle(worker);
    rewind(report);
    char text[8192] = {};
    fread(text, 1, sizeof(text) - 1, report);
    fclose(report);
    if (!strstr(text, "module+RVA") || !strstr(text, "load_sampler_test.exe+0x")) return 8;
    puts("PASS: two capture cycles, correct module+RVA, worker resumed");
    return 0;
}
