#include "pak_resolver.h"
#include <windows.h>
#include <cstring>
#include <cstdio>

namespace G3O {

static HANDLE OpenPakFile(const char* archiveName) {
    if (!archiveName || archiveName[0] == '\0') return INVALID_HANDLE_VALUE;

    // 1. Try path as specified
    HANDLE hFile = CreateFileA(archiveName, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) return hFile;

    // 2. Try under Data\ directory
    char candidate[MAX_PATH];
    snprintf(candidate, sizeof(candidate), "Data\\%s", archiveName);
    hFile = CreateFileA(candidate, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) return hFile;

    // 3. Try with executable folder
    char exePath[MAX_PATH] = {0};
    if (GetModuleFileNameA(NULL, exePath, MAX_PATH) > 0) {
        char* slash = strrchr(exePath, '\\');
        if (!slash) slash = strrchr(exePath, '/');
        if (slash) {
            *(slash + 1) = '\0';
            snprintf(candidate, sizeof(candidate), "%sData\\%s", exePath, archiveName);
            hFile = CreateFileA(candidate, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hFile != INVALID_HANDLE_VALUE) return hFile;

            snprintf(candidate, sizeof(candidate), "%s%s", exePath, archiveName);
            hFile = CreateFileA(candidate, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hFile != INVALID_HANDLE_VALUE) return hFile;
        }
    }

    return INVALID_HANDLE_VALUE;
}

bool ResolvePakVirtualResource(const char* archiveName, uint64_t targetOffset, PakResolvedResource& outResource) {
    memset(&outResource, 0, sizeof(outResource));
    if (!archiveName || archiveName[0] == '\0') return false;

    HANDLE hFile = OpenPakFile(archiveName);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    // Read 32-byte header
    BYTE hdr[32];
    DWORD bytesRead = 0;
    if (!ReadFile(hFile, hdr, 32, &bytesRead, NULL) || bytesRead != 32) {
        CloseHandle(hFile);
        return false;
    }

    // Verify 'G3V0' magic at offset 4
    if (memcmp(hdr + 4, "G3V0", 4) != 0) {
        CloseHandle(hFile);
        return false;
    }

    uint32_t fat_off = *reinterpret_cast<const uint32_t*>(hdr + 24);

    LARGE_INTEGER fsize = {};
    if (!GetFileSizeEx(hFile, &fsize) || fat_off >= static_cast<uint64_t>(fsize.QuadPart)) {
        CloseHandle(hFile);
        return false;
    }

    uint64_t fat_size = fsize.QuadPart - fat_off;
    if (fat_size == 0 || fat_size > 64 * 1024 * 1024) {
        CloseHandle(hFile);
        return false;
    }

    // Use VirtualAlloc to remain 100% independent of potentially corrupted C-runtime heap
    BYTE* pFat = reinterpret_cast<BYTE*>(VirtualAlloc(NULL, static_cast<SIZE_T>(fat_size),
                                                      MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!pFat) {
        CloseHandle(hFile);
        return false;
    }

    LARGE_INTEGER liFat;
    liFat.QuadPart = fat_off;
    if (!SetFilePointerEx(hFile, liFat, NULL, FILE_BEGIN)) {
        VirtualFree(pFat, 0, MEM_RELEASE);
        CloseHandle(hFile);
        return false;
    }

    if (!ReadFile(hFile, pFat, static_cast<DWORD>(fat_size), &bytesRead, NULL) || bytesRead == 0) {
        VirtualFree(pFat, 0, MEM_RELEASE);
        CloseHandle(hFile);
        return false;
    }
    CloseHandle(hFile);

    // Scan FAT entries for data block covering targetOffset
    const size_t totalBytes = static_cast<size_t>(bytesRead);
    size_t pos = 0;
    while (pos + 48 <= totalBytes) {
        uint64_t off = *reinterpret_cast<const uint64_t*>(pFat + pos);
        uint64_t c_sz = *reinterpret_cast<const uint64_t*>(pFat + pos + 8);
        uint64_t u_sz = *reinterpret_cast<const uint64_t*>(pFat + pos + 16);

        if (c_sz > 0 && c_sz < 0x20000000 && off <= targetOffset && targetOffset < off + c_sz) {
            if (pos + 36 <= totalBytes) {
                uint32_t l1 = *reinterpret_cast<const uint32_t*>(pFat + pos + 32);
                if (l1 > 0 && l1 < sizeof(outResource.virtualPath) && pos + 36 + l1 <= totalBytes) {
                    memcpy(outResource.virtualPath, pFat + pos + 36, l1);
                    outResource.virtualPath[l1 - 1] = '\0';

                    size_t p2_pos = pos + 36 + l1;
                    if (p2_pos + 4 <= totalBytes) {
                        uint32_t l2 = *reinterpret_cast<const uint32_t*>(pFat + p2_pos);
                        if (l2 > 0 && l2 < sizeof(outResource.authorPath) && p2_pos + 4 + l2 <= totalBytes) {
                            memcpy(outResource.authorPath, pFat + p2_pos + 4, l2);
                            outResource.authorPath[l2 - 1] = '\0';
                        }
                    }
                    outResource.fileOffset = off;
                    outResource.compressedSize = c_sz;
                    outResource.uncompressedSize = u_sz;
                    outResource.found = true;
                    VirtualFree(pFat, 0, MEM_RELEASE);
                    return true;
                }
            }
        }
        pos++;
    }

    VirtualFree(pFat, 0, MEM_RELEASE);
    return false;
}

} // namespace G3O
