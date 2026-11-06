#pragma once

#include <windows.h>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace G3O {

inline bool BuildG3OOutputDirectoryPath(char* output, std::size_t capacity,
                                        const char* executablePath) noexcept {
    if (!output || capacity == 0 || !executablePath || !executablePath[0]) return false;
    output[0] = '\0';

    const char* separator = std::strrchr(executablePath, '\\');
    const char* forwardSeparator = std::strrchr(executablePath, '/');
    if (!separator || (forwardSeparator && forwardSeparator > separator)) separator = forwardSeparator;
    if (!separator) return false;

    const std::size_t directoryLength = static_cast<std::size_t>(separator - executablePath);
    const int written = std::snprintf(output, capacity, "%.*s\\G3O",
                                      static_cast<int>(directoryLength), executablePath);
    if (written < 0 || static_cast<std::size_t>(written) >= capacity) {
        output[0] = '\0';
        return false;
    }
    return true;
}

inline bool EnsureG3OOutputDirectory(char* output, std::size_t capacity) noexcept {
    if (!output || capacity == 0) return false;
    char executablePath[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameA(nullptr, executablePath, MAX_PATH);
    if (length == 0 || length >= MAX_PATH ||
        !BuildG3OOutputDirectoryPath(output, capacity, executablePath)) {
        output[0] = '\0';
        return false;
    }

    if (!CreateDirectoryA(output, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        output[0] = '\0';
        return false;
    }
    const DWORD attributes = GetFileAttributesA(output);
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        output[0] = '\0';
        return false;
    }
    return true;
}

inline bool BuildG3OOutputFilePath(char* output, std::size_t capacity,
                                   const char* directory, const char* fileName) noexcept {
    if (!output || capacity == 0 || !directory || !directory[0] ||
        !fileName || !fileName[0] || std::strchr(fileName, '\\') || std::strchr(fileName, '/')) {
        return false;
    }
    output[0] = '\0';
    const std::size_t directoryLength = std::strlen(directory);
    const bool hasSeparator = directoryLength > 0 &&
        (directory[directoryLength - 1] == '\\' || directory[directoryLength - 1] == '/');
    const int written = std::snprintf(output, capacity, "%s%s%s",
                                      directory, hasSeparator ? "" : "\\", fileName);
    if (written < 0 || static_cast<std::size_t>(written) >= capacity) {
        output[0] = '\0';
        return false;
    }
    return true;
}

} // namespace G3O
