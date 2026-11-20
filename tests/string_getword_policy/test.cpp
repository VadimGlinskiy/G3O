#include "string_getword_policy.h"
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

// Verbatim reference of Gothic 3 SharedBase.dll bCString::GetWord (0x10004C40)
static std::pair<int, std::string> ReferenceGetWord(
    const std::string& input, int wordNumber, const std::string& delims,
    bool trimLeft = false, bool trimRight = false) {
    std::string result;
    std::string cur = input;
    int offset = 0;
    int lastPos = -1;
    int remaining = wordNumber;

    while (remaining >= 0) {
        --remaining;
        offset += lastPos + 1;
        const char* found = (!cur.empty() && !delims.empty())
            ? std::strpbrk(cur.c_str(), delims.c_str())
            : nullptr;
        if (!found) {
            result = cur;
            if (remaining > -1) {
                offset = -1;
            }
            break;
        }
        const int pos = static_cast<int>(found - cur.c_str());
        lastPos = pos;
        result = cur.substr(0, static_cast<std::size_t>(pos));
        cur = cur.substr(static_cast<std::size_t>(pos + 1));
    }

    if (trimLeft) {
        std::size_t s = 0;
        while (s < result.size() && static_cast<unsigned char>(result[s]) <= 127 &&
               std::isspace(static_cast<unsigned char>(result[s]))) {
            ++s;
        }
        result.erase(0, s);
    }
    if (trimRight) {
        while (!result.empty() && static_cast<unsigned char>(result.back()) <= 127 &&
               std::isspace(static_cast<unsigned char>(result.back()))) {
            result.pop_back();
        }
    }

    return { offset, result };
}

static bool CheckCase(
    const std::string& input, int wordNum, const std::string& delims,
    bool trimLeft = false, bool trimRight = false) {
    const auto ref = ReferenceGetWord(input, wordNum, delims, trimLeft, trimRight);
    auto fast = G3O::ExtractWordSpan(
        input.c_str(), static_cast<int>(input.size()), wordNum, delims.c_str());
    if (trimLeft || trimRight) {
        G3O::TrimWordSpan(fast, trimLeft, trimRight);
    }
    const std::string fastStr(fast.ptr ? fast.ptr : "", static_cast<std::size_t>(fast.length));
    if (fast.offset != ref.first || fastStr != ref.second) {
        std::fprintf(stderr,
            "FAIL: input='%s' word=%d delims='%s' trimL=%d trimR=%d | ref=(%d, '%s') vs fast=(%d, '%s')\n",
            input.c_str(), wordNum, delims.c_str(), (int)trimLeft, (int)trimRight,
            ref.first, ref.second.c_str(), fast.offset, fastStr.c_str());
        return false;
    }
    return true;
}

int main() {
    bool ok = true;
    const std::string inputs[] = {
        "",
        "single",
        "  leading_spaces  ",
        "\t\r\n surrounded \r\n\t ",
        "a;b;c",
        ";leading;trailing;",
        ";;;",
        "alpha; beta ; gamma ; delta",
        "  alpha  ;  beta  ;  gamma  ;  delta  ",
        "one,two;three:four"
    };
    const std::string delimSets[] = { "", ";", ",;:", " \t" };
    const bool boolVals[] = { false, true };

    for (const auto& s : inputs) {
        for (const auto& d : delimSets) {
            for (int w = -2; w <= 6; ++w) {
                for (bool tl : boolVals) {
                    for (bool tr : boolVals) {
                        ok &= CheckCase(s, w, d, tl, tr);
                    }
                }
            }
        }
    }

    // Stress test: 200 semicolon-separated tokens (typical gCInfo_PS::Read list)
    std::string big;
    for (int i = 0; i < 200; ++i) {
        if (i > 0) big += ';';
        big += "  InfoToken_" + std::to_string(i) + " \t";
    }
    for (int w = -1; w <= 202; ++w) {
        for (bool tl : boolVals) {
            for (bool tr : boolVals) {
                ok &= CheckCase(big, w, ";", tl, tr);
            }
        }
    }

    if (!ok) return 1;
    std::puts("PASS: ExtractWordSpan and TrimWordSpan match Gothic 3 bCString::GetWord across all edge cases");
    return 0;
}
