#include "high_memory_cache.h"
#include "heavens_gate.h"
#include <windows.h>
#include <vector>
#include <string>
#include <unordered_map>
#include <stdio.h>
#include <algorithm>

namespace HighMemoryCache {

    struct HighCacheEntry {
        uint64_t    bufferOffset;
        uint64_t    fileSize;
        std::string fileName;
    };

    static CRITICAL_SECTION s_cacheLock;
    static bool s_lockInitialized = false;

    static uint64_t s_highMemoryBase = 0;
    static uint64_t s_capacityBytes = 0;
    static uint64_t s_usedBytes = 0;
    static volatile DWORD s_cacheHits = 0;
    static uint64_t s_totalBytesRead = 0;

    static std::unordered_map<HANDLE, HighCacheEntry> s_handleToEntry;
    static std::unordered_map<std::string, HighCacheEntry> s_nameToEntry;

    static std::string NormalizePath(const char* path) {
        if (!path) return "";
        std::string s(path);
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        std::replace(s.begin(), s.end(), '/', '\\');
        return s;
    }

    bool Initialize() {
        if (!s_lockInitialized) {
            InitializeCriticalSection(&s_cacheLock);
            s_lockInitialized = true;
        }

        if (s_highMemoryBase != 0) {
            return true;
        }

        if (!HeavensGate::IsSupported()) {
            OutputDebugStringA("[Gothic3Optima] High Memory Cache: WOW64 64-bit environment not supported.\n");
            return false;
        }

        if (!HeavensGate::Initialize()) {
            OutputDebugStringA("[Gothic3Optima] High Memory Cache: Failed to initialize Heaven's Gate.\n");
            return false;
        }

        MEMORYSTATUSEX memStatus = { sizeof(MEMORYSTATUSEX) };
        GlobalMemoryStatusEx(&memStatus);

        uint64_t targetCapacity = 2048ULL * 1024ULL * 1024ULL;
        s_highMemoryBase = HeavensGate::AllocateHighMemory(static_cast<size_t>(targetCapacity));

        if (s_highMemoryBase == 0 || s_highMemoryBase <= 0xFFFFFFFFULL) {
            if (s_highMemoryBase != 0) {
                HeavensGate::FreeHighMemory(s_highMemoryBase);
                s_highMemoryBase = 0;
            }
            OutputDebugStringA("[Gothic3Optima] High Memory Cache: Failed to allocate High Memory buffer above 4 GB!\n");
            return false;
        }

        s_capacityBytes = targetCapacity;
        s_usedBytes = 0;

        char buf[256];
        sprintf_s(buf, "[Gothic3Optima] Phase 14.1: High Memory Shadow Cache allocated %llu MB at 0x%llX (ABOVE 4 GB limit)!\n",
            s_capacityBytes / (1024ULL * 1024ULL), s_highMemoryBase);
        OutputDebugStringA(buf);

        FILE* fLog = fopen("G3O_Streaming.log", "a");
        if (fLog) {
            fprintf(fLog, "High Memory Shadow Cache: Allocated %llu MB at 0x%llX (ABOVE 4 GB boundary)\n",
                s_capacityBytes / (1024ULL * 1024ULL), s_highMemoryBase);
            fclose(fLog);
        }

        return true;
    }

    bool IsActive() {
        return (s_highMemoryBase != 0);
    }

    bool RegisterArchive(const char* filePath, HANDLE hFile, uint64_t fileSize) {
        if (!s_highMemoryBase || !filePath || hFile == INVALID_HANDLE_VALUE || fileSize == 0) {
            return false;
        }

        std::string normPath = NormalizePath(filePath);

        EnterCriticalSection(&s_cacheLock);

        // Check if this archive is already cached under another handle
        auto itName = s_nameToEntry.find(normPath);
        if (itName != s_nameToEntry.end()) {
            s_handleToEntry[hFile] = itName->second;
            LeaveCriticalSection(&s_cacheLock);
            return true;
        }

        // Check capacity limit
        if (s_usedBytes + fileSize > s_capacityBytes) {
            LeaveCriticalSection(&s_cacheLock);
            return false;
        }

        uint64_t currentOffset = s_usedBytes;
        LeaveCriticalSection(&s_cacheLock);

        // Stream archive content from disk into 64-bit high memory via 4 MB staging chunks
        const DWORD chunkSize = 4 * 1024 * 1024;
        std::vector<uint8_t> chunkBuf(chunkSize);

        LARGE_INTEGER zero = { 0 };
        LARGE_INTEGER origPos = { 0 };
        SetFilePointerEx(hFile, zero, &origPos, FILE_CURRENT);
        SetFilePointer(hFile, 0, NULL, FILE_BEGIN);

        uint64_t bytesLoaded = 0;
        bool readSuccess = true;

        while (bytesLoaded < fileSize) {
            DWORD toRead = (DWORD)std::min<uint64_t>(chunkSize, fileSize - bytesLoaded);
            DWORD actualRead = 0;
            if (!ReadFile(hFile, chunkBuf.data(), toRead, &actualRead, NULL) || actualRead == 0) {
                readSuccess = false;
                break;
            }

            if (!HeavensGate::WriteHighMemory(s_highMemoryBase + currentOffset + bytesLoaded, chunkBuf.data(), actualRead)) {
                readSuccess = false;
                break;
            }

            bytesLoaded += actualRead;
        }

        // Restore original file handle position
        SetFilePointerEx(hFile, origPos, NULL, FILE_BEGIN);

        if (!readSuccess || bytesLoaded != fileSize) {
            return false;
        }

        EnterCriticalSection(&s_cacheLock);
        HighCacheEntry entry;
        entry.bufferOffset = currentOffset;
        entry.fileSize = fileSize;
        entry.fileName = normPath;

        s_usedBytes += fileSize;
        s_handleToEntry[hFile] = entry;
        s_nameToEntry[normPath] = entry;

        char logBuf[256];
        sprintf_s(logBuf, "[Gothic3Optima] High Memory Cache: Cached '%s' (%.2f MB) into 64-bit RAM! Total used: %.2f / %.2f MB\n",
            filePath, (double)fileSize / (1024.0 * 1024.0),
            (double)s_usedBytes / (1024.0 * 1024.0),
            (double)s_capacityBytes / (1024.0 * 1024.0));
        OutputDebugStringA(logBuf);

        FILE* fArchLog = fopen("G3O_Streaming.log", "a");
        if (fArchLog) {
            fprintf(fArchLog, "High Memory Cache: Cached '%s' (%.2f MB, total High RAM: %.2f / %.2f MB)\n",
                filePath, (double)fileSize / (1024.0 * 1024.0),
                (double)s_usedBytes / (1024.0 * 1024.0),
                (double)s_capacityBytes / (1024.0 * 1024.0));
            fclose(fArchLog);
        }

        LeaveCriticalSection(&s_cacheLock);
        return true;
    }

    void UnregisterArchive(HANDLE hFile) {
        if (!s_lockInitialized) return;
        EnterCriticalSection(&s_cacheLock);
        s_handleToEntry.erase(hFile);
        LeaveCriticalSection(&s_cacheLock);
    }

    bool IsArchiveCached(HANDLE hFile) {
        if (!s_highMemoryBase || hFile == INVALID_HANDLE_VALUE || !s_lockInitialized) return false;
        EnterCriticalSection(&s_cacheLock);
        bool found = (s_handleToEntry.find(hFile) != s_handleToEntry.end());
        LeaveCriticalSection(&s_cacheLock);
        return found;
    }

    bool ReadArchive(HANDLE hFile, uint64_t fileOffset, void* dstBuffer, DWORD bytesToRead, DWORD* bytesRead) {
        if (!s_highMemoryBase || !dstBuffer || bytesToRead == 0 || !s_lockInitialized) {
            return false;
        }

        EnterCriticalSection(&s_cacheLock);
        auto it = s_handleToEntry.find(hFile);
        if (it == s_handleToEntry.end()) {
            LeaveCriticalSection(&s_cacheLock);
            return false;
        }

        HighCacheEntry entry = it->second;
        LeaveCriticalSection(&s_cacheLock);

        if (fileOffset >= entry.fileSize) {
            if (bytesRead) *bytesRead = 0;
            return true;
        }

        uint64_t remain = entry.fileSize - fileOffset;
        DWORD toCopy = (DWORD)std::min<uint64_t>(static_cast<uint64_t>(bytesToRead), remain);

        bool ok = HeavensGate::ReadHighMemory(dstBuffer, s_highMemoryBase + entry.bufferOffset + fileOffset, toCopy);
        if (ok) {
            if (bytesRead) *bytesRead = toCopy;
            DWORD hits = InterlockedIncrement(reinterpret_cast<volatile LONG*>(&s_cacheHits));

            EnterCriticalSection(&s_cacheLock);
            s_totalBytesRead += toCopy;
            uint64_t totalRead = s_totalBytesRead;
            LeaveCriticalSection(&s_cacheLock);

            if (hits % 2000 == 1) {
                FILE* fLog = fopen("G3O_Streaming.log", "a");
                if (fLog) {
                    fprintf(fLog, "High Memory Streaming stats: %u RAM reads, %.2f MB read from 64-bit RAM\n",
                        hits, (double)totalRead / (1024.0 * 1024.0));
                    fclose(fLog);
                }
            }
        }

        return ok;
    }

    void GetStatistics(uint64_t* pUsedBytes, uint64_t* pCapacityBytes, DWORD* pHits, uint64_t* pBytesRead) {
        if (pUsedBytes) *pUsedBytes = s_usedBytes;
        if (pCapacityBytes) *pCapacityBytes = s_capacityBytes;
        if (pHits) *pHits = s_cacheHits;
        if (pBytesRead) *pBytesRead = s_totalBytesRead;
    }

    void Shutdown() {
        if (s_highMemoryBase != 0) {
            HeavensGate::FreeHighMemory(s_highMemoryBase);
            s_highMemoryBase = 0;
        }

        if (s_lockInitialized) {
            EnterCriticalSection(&s_cacheLock);
            s_handleToEntry.clear();
            s_nameToEntry.clear();
            s_usedBytes = 0;
            s_capacityBytes = 0;
            LeaveCriticalSection(&s_cacheLock);

            DeleteCriticalSection(&s_cacheLock);
            s_lockInitialized = false;
        }
    }

} // namespace HighMemoryCache
