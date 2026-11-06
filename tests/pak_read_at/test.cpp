#include "pak_read_at.h"
#include <cstdio>
#include <cstring>
#include <limits>

static bool Expect(bool condition, const char* message) {
    if (condition) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

int main() {
    bool ok = true;
    ok &= Expect(!G3O::IsInternalPakRead(), "internal-read marker should start clear");
    {
        G3O::ScopedInternalPakRead outer;
        ok &= Expect(G3O::IsInternalPakRead(), "internal-read scope should set the marker");
        {
            G3O::ScopedInternalPakRead inner;
            ok &= Expect(G3O::IsInternalPakRead(), "nested internal-read scope should keep the marker set");
        }
        ok &= Expect(G3O::IsInternalPakRead(), "nested scope should restore the prior marker");
    }
    ok &= Expect(!G3O::IsInternalPakRead(), "internal-read scope should restore the clear marker");

    char tempFile[MAX_PATH] = {};
    const DWORD pathLength = GetModuleFileNameA(nullptr, tempFile, MAX_PATH);
    if (!pathLength || pathLength >= MAX_PATH) {
        std::fprintf(stderr, "FAIL: could not resolve the test executable path\n");
        return 2;
    }
    char* slash = std::strrchr(tempFile, static_cast<char>(92));
    if (!slash) {
        std::fprintf(stderr, "FAIL: test executable path has no directory\n");
        return 2;
    }
    slash[1] = 0;
    std::strcat(tempFile, "pak_read_at_fixture.tmp");

    HANDLE file = CreateFileA(tempFile, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        DeleteFileA(tempFile);
        std::fprintf(stderr, "FAIL: could not create temporary test file\n");
        return 2;
    }

    const char payload[] = "abcdef";
    DWORD written = 0;
    const bool wrote = WriteFile(file, payload, sizeof(payload) - 1, &written, nullptr) &&
                       written == sizeof(payload) - 1;
    char slice[4] = {};
    const bool readAtOffset = wrote && G3O::ReadExactAt(file, 2, slice, 3);
    const bool correctSlice = std::memcmp(slice, "cde", 3) == 0;
    const bool rejectsShortRead = !G3O::ReadExactAt(file, 4, slice, 3);
    const bool rejectsInvalidOffset = !G3O::ReadExactAt(
        file, (std::numeric_limits<uint64_t>::max)(), slice, 1);

    CloseHandle(file);
    DeleteFileA(tempFile);

    ok &= Expect(readAtOffset && correctSlice,
                 "synchronous archive reads must seek to and read the requested 64-bit offset");
    ok &= Expect(rejectsShortRead, "partial archive reads must be rejected");
    ok &= Expect(rejectsInvalidOffset, "offsets outside the Win32 file-position range must be rejected");
    if (!ok) return 1;
    std::puts("PASS: exact synchronous reads honor file offsets and reject short reads");
    return 0;
}
