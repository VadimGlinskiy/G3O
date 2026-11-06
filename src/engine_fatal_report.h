#pragma once

#include "fixed_text_buffer.h"

namespace G3O {

inline void BuildEngineFatalReport(FixedTextBuffer& report,
                                   const char* timestamp,
                                   const char* module,
                                   const char* message,
                                   const char* function,
                                   const char* file,
                                   int line,
                                   const char* stackTrace,
                                   const char* recentEvents,
                                   unsigned long processId,
                                   unsigned long threadId,
                                   const char* runtimeContext) noexcept {
    report.Reset();
    report.Append("================================================================================\n");
    report.Append("Gothic 3 Optima — Engine Fatal Error Report\n");
    report.Append("Время: ");
    report.Append(timestamp ? timestamp : "<неизвестно>");
    report.AppendFormat("\nPID: %lu\nTID: %lu\n", processId, threadId);
    if (runtimeContext && runtimeContext[0] != '\0') {
        report.Append(runtimeContext);
        report.Append("\n");
    }
    report.Append("Модуль: ");
    report.Append(module ? module : "<неизвестен>");
    report.Append("\nСообщение: ");
    report.Append(message ? message : "<не передано этой перегрузкой>");
    report.Append("\nФункция (по стеку; API её не передаёт): ");
    report.Append(function ? function : "<не удалось определить>");
    report.Append("\nФайл и строка: ");
    report.Append(file ? file : "<неизвестен>");
    report.AppendFormat(":%d\n\n[Стек вызовов]\n", line);
    report.Append(stackTrace ? stackTrace : "<стек недоступен>\n");
    report.Append("\n[Последние события загрузки / Guru / G3O]\n");
    report.Append(recentEvents ? recentEvents : "<событий нет>\n");
    report.Append("================================================================================\n");
    report.Finish();
}

} // namespace G3O
