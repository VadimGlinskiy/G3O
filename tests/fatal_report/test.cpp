#include "engine_fatal_report.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

static unsigned long s_allocationCount = 0;

void* operator new(std::size_t size) {
    ++s_allocationCount;
    if (void* memory = std::malloc(size)) return memory;
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    ++s_allocationCount;
    if (void* memory = std::malloc(size)) return memory;
    throw std::bad_alloc();
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

static void requireContains(const char* report, const char* expected) {
    if (!report || std::strstr(report, expected) == nullptr) {
        std::fprintf(stderr, "Missing expected report text: %s\n", expected);
        std::exit(1);
    }
}

int main() {
    const char* runtimeContext =
        "[Версия G3O] 1.6.1\n"
        "[Путь отчёта] E:\\Gothic3\\G3O\\G3O_CrashReport_20261106.log\n"
        "[Статус дампа] не создавался: движковая ошибка без EXCEPTION_POINTERS\n"
        "[DLL] Engine.dll 1.75.14.0\n"
        "[Config] EnableFastPakDecompressor=true\n";
    char reportStorage[8192] = {};
    G3O::FixedTextBuffer report(reportStorage, sizeof(reportStorage));
    const unsigned long beforeFatalReport = s_allocationCount;
    G3O::BuildEngineFatalReport(
        report, "2026-11-06 13:14:15.123", "GenomeWorld", "Missing entity: RebNovice_01",
        "gCWorld::LoadGame", "E:\\Gothic3\\world.cpp", 1394,
        "#00 Engine.dll+0x1234 gCWorld::LoadGame\n", "[EngineFatal] captured\n", 17, 23,
        runtimeContext);
    if (s_allocationCount != beforeFatalReport) {
        std::fprintf(stderr, "FAIL: fatal report formatter allocated heap memory\n");
        return 1;
    }

    requireContains(report.CStr(), "Модуль: GenomeWorld");
    requireContains(report.CStr(), "Сообщение: Missing entity: RebNovice_01");
    requireContains(report.CStr(), "Функция (по стеку; API её не передаёт): gCWorld::LoadGame");
    requireContains(report.CStr(), "Файл и строка: E:\\Gothic3\\world.cpp:1394");
    requireContains(report.CStr(), "#00 Engine.dll+0x1234 gCWorld::LoadGame");
    requireContains(report.CStr(), "[EngineFatal] captured");
    requireContains(report.CStr(), "PID: 17");
    requireContains(report.CStr(), "TID: 23");
    requireContains(report.CStr(), "[Версия G3O] 1.6.1");
    requireContains(report.CStr(), "[Путь отчёта] E:\\Gothic3\\G3O\\G3O_CrashReport_20261106.log");
    requireContains(report.CStr(), "[Статус дампа] не создавался: движковая ошибка без EXCEPTION_POINTERS");
    requireContains(report.CStr(), "[DLL] Engine.dll 1.75.14.0");
    requireContains(report.CStr(), "[Config] EnableFastPakDecompressor=true");

    char noDescriptionStorage[4096] = {};
    G3O::FixedTextBuffer noDescription(noDescriptionStorage, sizeof(noDescriptionStorage));
    const unsigned long beforeEmptyReport = s_allocationCount;
    G3O::BuildEngineFatalReport(
        noDescription, "2026-11-06 13:14:15.123", "GenomeWorld", nullptr,
        nullptr, nullptr, 42, nullptr, nullptr, 0, 0, nullptr);
    if (s_allocationCount != beforeEmptyReport) {
        std::fprintf(stderr, "FAIL: empty-context fatal report allocated heap memory\n");
        return 1;
    }
    requireContains(noDescription.CStr(), "Сообщение: <не передано этой перегрузкой>");
    requireContains(noDescription.CStr(), "Функция (по стеку; API её не передаёт): <не удалось определить>");
    requireContains(noDescription.CStr(), "Файл и строка: <неизвестен>:42");

    std::puts("PASS: fatal report preserves engine fields and handles absent context without std::string");
    return 0;
}
