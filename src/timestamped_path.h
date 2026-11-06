#pragma once

#include <windows.h>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace G3O {

inline bool BuildTimestampedFilePath(char* output, std::size_t capacity,
                                     const char* directory, const char* prefix,
                                     const char* extension, const SYSTEMTIME& time,
                                     DWORD processId, DWORD threadId,
                                     unsigned long sequence) noexcept {
    if (!output || capacity == 0 || !directory || !prefix || !extension) return false;
    output[0] = '\0';
    const std::size_t directoryLength = std::strlen(directory);
    const bool hasSeparator = directoryLength > 0 &&
        (directory[directoryLength - 1] == '\\' || directory[directoryLength - 1] == '/');
    const int written = std::snprintf(output, capacity,
        "%s%s%s_%04u%02u%02u_%02u%02u%02u_%03u_%lu_%lu_%lu.%s",
        directory, hasSeparator ? "" : "\\", prefix,
        static_cast<unsigned>(time.wYear), static_cast<unsigned>(time.wMonth),
        static_cast<unsigned>(time.wDay), static_cast<unsigned>(time.wHour),
        static_cast<unsigned>(time.wMinute), static_cast<unsigned>(time.wSecond),
        static_cast<unsigned>(time.wMilliseconds), static_cast<unsigned long>(processId),
        static_cast<unsigned long>(threadId), sequence, extension);
    if (written < 0 || static_cast<std::size_t>(written) >= capacity) {
        output[0] = '\0';
        return false;
    }
    return true;
}

} // namespace G3O
