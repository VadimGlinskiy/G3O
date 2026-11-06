#include "engine_fatal_report.h"
#include <cstdlib>
#include <iostream>
#include <string>

static void requireContains(const std::string& report, const char* expected) {
    if (report.find(expected) == std::string::npos) {
        std::cerr << "Missing expected report text: " << expected << '\n';
        std::exit(1);
    }
}

int main() {
    const std::string report = G3O::BuildEngineFatalReport(
        "2026-11-06 13:14:15.123", "GenomeWorld", "Missing entity: RebNovice_01",
        "gCWorld::LoadGame", "E:\\Gothic3\\world.cpp", 1394,
        "#00 Engine.dll+0x1234 gCWorld::LoadGame\n", "[EngineFatal] captured\n", 17, 23);

    requireContains(report, "Модуль: GenomeWorld");
    requireContains(report, "Сообщение: Missing entity: RebNovice_01");
    requireContains(report, "Функция (по стеку; API её не передаёт): gCWorld::LoadGame");
    requireContains(report, "Файл и строка: E:\\Gothic3\\world.cpp:1394");
    requireContains(report, "#00 Engine.dll+0x1234 gCWorld::LoadGame");
    requireContains(report, "[EngineFatal] captured");
    requireContains(report, "PID: 17");
    requireContains(report, "TID: 23");

    const std::string noDescription = G3O::BuildEngineFatalReport(
        "2026-11-06 13:14:15.123", "GenomeWorld", nullptr,
        nullptr, nullptr, 42, nullptr, nullptr, 0, 0);
    requireContains(noDescription, "Сообщение: <не передано этой перегрузкой>");
    requireContains(noDescription, "Функция (по стеку; API её не передаёт): <не удалось определить>");
    requireContains(noDescription, "Файл и строка: <неизвестен>:42");

    std::cout << "PASS: fatal report preserves engine fields and handles absent context\n";
    return 0;
}
