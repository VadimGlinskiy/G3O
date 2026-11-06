#pragma once

#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace G3O::IniValue {

inline const char* SkipWhitespace(const char* text) noexcept {
    while (text && *text && std::isspace(static_cast<unsigned char>(*text))) ++text;
    return text;
}

inline bool IsTokenEnd(const char* text) noexcept {
    while (text && *text) {
        if (!std::isspace(static_cast<unsigned char>(*text))) return false;
        ++text;
    }
    return true;
}

inline bool ParseFloat(const char* text, float minimum, float maximum, float& output) noexcept {
    if (!text || minimum > maximum) return false;
    const char* start = SkipWhitespace(text);
    if (!start || !*start) return false;

    errno = 0;
    char* end = nullptr;
    const float value = std::strtof(start, &end);
    if (end == start || errno == ERANGE || !IsTokenEnd(end) || !std::isfinite(value) ||
        value < minimum || value > maximum) {
        return false;
    }
    output = value;
    return true;
}

inline bool ParseInt(const char* text, int minimum, int maximum, int& output) noexcept {
    if (!text || minimum > maximum) return false;
    const char* start = SkipWhitespace(text);
    if (!start || !*start) return false;

    errno = 0;
    char* end = nullptr;
    const long long value = std::strtoll(start, &end, 10);
    if (end == start || errno == ERANGE || !IsTokenEnd(end) ||
        value < minimum || value > maximum) {
        return false;
    }
    output = static_cast<int>(value);
    return true;
}

inline bool ParseUnsigned(const char* text, std::uintptr_t maximum,
                          std::uintptr_t& output) noexcept {
    if (!text) return false;
    const char* start = SkipWhitespace(text);
    if (!start || !*start || *start == '-' || *start == '+') return false;

    int base = 10;
    if (start[0] == '0' && (start[1] == 'x' || start[1] == 'X')) base = 16;

    errno = 0;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(start, &end, base);
    if (end == start || errno == ERANGE || !IsTokenEnd(end) || value > maximum) {
        return false;
    }
    output = static_cast<std::uintptr_t>(value);
    return true;
}

} // namespace G3O::IniValue
