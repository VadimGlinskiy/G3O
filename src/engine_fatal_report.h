#pragma once

#include <string>

namespace G3O {

inline std::string BuildEngineFatalReport(const char* timestamp,
                                         const char* module,
                                         const char* message,
                                         const char* function,
                                         const char* file,
                                         int line,
                                         const char* stackTrace,
                                         const char* recentEvents,
                                         unsigned long processId,
                                         unsigned long threadId) {
    std::string report = "================================================================================\n";
    report += "Gothic 3 Optima — Engine Fatal Error Report\n";
    report += "Время: ";
    report += timestamp ? timestamp : "<неизвестно>";
    report += "\nPID: ";
    report += std::to_string(processId);
    report += "\nTID: ";
    report += std::to_string(threadId);
    report += "\nМодуль: ";
    report += module ? module : "<неизвестен>";
    report += "\nСообщение: ";
    report += message ? message : "<не передано этой перегрузкой>";
    report += "\nФункция (по стеку; API её не передаёт): ";
    report += function ? function : "<не удалось определить>";
    report += "\nФайл и строка: ";
    report += file ? file : "<неизвестен>";
    report += ":";
    report += std::to_string(line);
    report += "\n\n[Стек вызовов]\n";
    report += stackTrace ? stackTrace : "<стек недоступен>\n";
    report += "\n[Последние события G3O]\n";
    report += recentEvents ? recentEvents : "<событий нет>\n";
    report += "================================================================================\n";
    return report;
}

} // namespace G3O
