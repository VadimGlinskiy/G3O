#include "fixed_text_buffer.h"

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

int main() {
    char report[64] = {};
    G3O::FixedTextBuffer output(report, sizeof(report));
    const unsigned long before = s_allocationCount;
    output.Append("frame=");
    output.AppendFormat("%02u", 7u);
    output.Finish();
    if (s_allocationCount != before || std::strcmp(output.CStr(), "frame=07") != 0) {
        std::fprintf(stderr, "FAIL: append/format changed text or allocated memory\n");
        return 1;
    }

    char shortReport[48] = {};
    char longInput[128];
    std::memset(longInput, 'x', sizeof(longInput) - 1);
    longInput[sizeof(longInput) - 1] = '\0';
    G3O::FixedTextBuffer truncated(shortReport, sizeof(shortReport));
    truncated.Append(longInput);
    truncated.Finish();
    if (!truncated.IsTruncated() || shortReport[sizeof(shortReport) - 1] != '\0' ||
        std::strstr(shortReport, "[ОТЧЁТ УКОРОЧЕН]") == nullptr) {
        std::fprintf(stderr, "FAIL: truncation marker or NUL termination missing\n");
        return 1;
    }

    std::puts("PASS: fixed report buffer appends without allocation and marks truncation");
    return 0;
}
