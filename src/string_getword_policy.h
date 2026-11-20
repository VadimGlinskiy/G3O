#pragma once

#include <cstdint>
#include <cstring>

namespace G3O {

struct WordSpanResult {
    int offset = 0;
    const char* ptr = nullptr;
    int length = 0;
};

// Trims ASCII whitespace in-place matching Gothic 3 bCString::TrimLeft / TrimRight.
// Avoids secondary heap allocations, memmove, and virtual calls.
inline void TrimWordSpan(WordSpanResult& span, bool trimLeft, bool trimRight) noexcept {
    const char* start = span.ptr;
    int len = span.length;
    if (trimLeft) {
        while (len > 0) {
            const unsigned char c = static_cast<unsigned char>(*start);
            if (c <= 127 && (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f')) {
                ++start;
                --len;
            } else {
                break;
            }
        }
    }
    if (trimRight) {
        while (len > 0) {
            const unsigned char c = static_cast<unsigned char>(start[len - 1]);
            if (c <= 127 && (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f')) {
                --len;
            } else {
                break;
            }
        }
    }
    span.ptr = start;
    span.length = len;
}

// Exact single-pass equivalent of Gothic 3 SharedBase.dll bCString::GetWord
// (0x10004C40) without intermediate Left/Right heap allocations or caching.
inline WordSpanResult ExtractWordSpan(
    const char* text, int textLen, int wordNumber, const char* delimiters) noexcept {
    WordSpanResult result{};
    if (wordNumber < 0) {
        result.offset = 0;
        result.ptr = "";
        result.length = 0;
        return result;
    }

    if (!text || textLen <= 0) {
        result.offset = (wordNumber > 0) ? -1 : 0;
        result.ptr = "";
        result.length = 0;
        return result;
    }

    const int delimLen = delimiters ? static_cast<int>(std::strlen(delimiters)) : 0;
    if (delimLen == 0) {
        result.offset = (wordNumber > 0) ? -1 : 0;
        result.ptr = text;
        result.length = textLen;
        return result;
    }

    // Fast path for single-character delimiter (e.g. ';' in gCInfo_PS::Read).
    if (delimLen == 1) {
        const char delim = delimiters[0];
        const char* cur = text;
        const char* const end = text + textLen;
        int remaining = wordNumber;
        while (true) {
            const int curOffset = static_cast<int>(cur - text);
            const char* found = (cur < end)
                ? static_cast<const char*>(std::memchr(cur, static_cast<unsigned char>(delim), static_cast<std::size_t>(end - cur)))
                : nullptr;
            if (!found) {
                result.offset = (remaining > 0) ? -1 : curOffset;
                result.ptr = cur;
                result.length = static_cast<int>(end - cur);
                return result;
            }
            if (remaining == 0) {
                result.offset = curOffset;
                result.ptr = cur;
                result.length = static_cast<int>(found - cur);
                return result;
            }
            cur = found + 1;
            --remaining;
        }
    }

    // General path for multi-character delimiters using a 256-bit table.
    std::uint64_t mask[4] = {};
    for (int i = 0; i < delimLen; ++i) {
        const unsigned char c = static_cast<unsigned char>(delimiters[i]);
        mask[c >> 6] |= (1ULL << (c & 63));
    }

    const char* cur = text;
    const char* const end = text + textLen;
    int remaining = wordNumber;
    while (true) {
        const int curOffset = static_cast<int>(cur - text);
        const char* scan = cur;
        while (scan < end) {
            const unsigned char c = static_cast<unsigned char>(*scan);
            if ((mask[c >> 6] & (1ULL << (c & 63))) != 0) {
                break;
            }
            ++scan;
        }
        if (scan >= end) {
            result.offset = (remaining > 0) ? -1 : curOffset;
            result.ptr = cur;
            result.length = static_cast<int>(end - cur);
            return result;
        }
        if (remaining == 0) {
            result.offset = curOffset;
            result.ptr = cur;
            result.length = static_cast<int>(scan - cur);
            return result;
        }
        cur = scan + 1;
        --remaining;
    }
}

} // namespace G3O
