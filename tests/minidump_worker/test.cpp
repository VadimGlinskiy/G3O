#include "../../src/crash_handler.cpp"

#include <cstdio>
#include <cstring>

namespace G3O {

const Config& GetConfig() {
    static Config config;
    return config;
}

void FlushDiagnosticLog() {}
void Log(const char*, const char*, ...) {}

std::size_t CopyRecentLogs(char* output, std::size_t capacity) {
    if (output && capacity > 0) output[0] = '\0';
    return 0;
}

void OutputCrashReportToDebugger(const char*) {}

} // namespace G3O

extern "C" MH_STATUS WINAPI MH_CreateHook(LPVOID, LPVOID, LPVOID*) {
    return MH_OK;
}

extern "C" MH_STATUS WINAPI MH_EnableHook(LPVOID) {
    return MH_OK;
}

extern "C" MH_STATUS WINAPI MH_DisableHook(LPVOID) {
    return MH_OK;
}

extern "C" MH_STATUS WINAPI MH_RemoveHook(LPVOID) {
    return MH_OK;
}

static bool ReadDumpAt(HANDLE file, DWORD offset, void* buffer, DWORD size) {
    LARGE_INTEGER position = {};
    position.QuadPart = offset;
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) return false;
    DWORD bytesRead = 0;
    return ReadFile(file, buffer, size, &bytesRead, nullptr) && bytesRead == size;
}

int main() {
    G3O::ReportFileTargets reportFiles;
    std::snprintf(reportFiles.reportPath, sizeof(reportFiles.reportPath),
                  "E:\\Gothic3\\G3O\\G3O_CrashReport_test.log");
    char runtimeStorage[128 * 1024] = {};
    G3O::FixedTextBuffer runtimeContext(runtimeStorage, sizeof(runtimeStorage));
    G3O::BuildRuntimeContext(runtimeContext, reportFiles, true,
                             "test dump was written", "E:\\Gothic3\\G3O\\G3O_Crash_test.dmp", 17, 23);
    if (runtimeContext.IsTruncated() ||
        std::strstr(runtimeContext.CStr(), "[Версия G3O] 1.7.0") == nullptr ||
        std::strstr(runtimeContext.CStr(), "[Сборка G3O] ") == nullptr ||
        std::strstr(runtimeContext.CStr(), G3O_BUILD_TIMESTAMP) == nullptr ||
        std::strstr(runtimeContext.CStr(), "PID: 17") == nullptr ||
        std::strstr(runtimeContext.CStr(), "TID: 23") == nullptr ||
        std::strstr(runtimeContext.CStr(), "[Путь отчёта] E:\\Gothic3\\G3O\\G3O_CrashReport_test.log") == nullptr ||
        std::strstr(runtimeContext.CStr(), "[Статус дампа] test dump was written") == nullptr ||
        std::strstr(runtimeContext.CStr(), "[Путь дампа] E:\\Gothic3\\G3O\\G3O_Crash_test.dmp") == nullptr ||
        std::strstr(runtimeContext.CStr(), "EnableFastPakDecompressor=") == nullptr ||
        std::strstr(runtimeContext.CStr(), "[Версии загруженных модулей]") == nullptr ||
        std::strstr(runtimeContext.CStr(), "minidump_worker_test.exe |") == nullptr) {
        std::fprintf(stderr, "FAIL: runtime report context is incomplete or truncated\n");
        return 1;
    }

    char tempPath[MAX_PATH] = {};
    const DWORD pathLength = GetTempPathA(MAX_PATH, tempPath);
    if (pathLength == 0 || pathLength >= MAX_PATH) {
        std::fprintf(stderr, "FAIL: GetTempPathA (%lu)\n", GetLastError());
        return 1;
    }

    const int logPathLength = std::snprintf(
        G3O::s_crashLogPath, sizeof(G3O::s_crashLogPath),
        "%sG3O_CrashReport.log", tempPath);
    if (logPathLength < 0 || static_cast<size_t>(logPathLength) >= sizeof(G3O::s_crashLogPath)) {
        std::fprintf(stderr, "FAIL: crash path too long\n");
        return 1;
    }

    const DWORD initError = G3O::InitializeMiniDumpWorker();
    if (initError != ERROR_SUCCESS) {
        std::fprintf(stderr, "FAIL: worker initialization (%lu)\n", initError);
        return 1;
    }

    MINIDUMP_CALLBACK_INPUT callbackInput = {};
    callbackInput.CallbackType = ThreadCallback;
    callbackInput.Thread.ThreadId = G3O::s_dumpWorkerThreadId;
    MINIDUMP_CALLBACK_OUTPUT callbackOutput = {};
    callbackOutput.ThreadWriteFlags = 0xFFFFFFFFu;
    const PVOID callbackParam = reinterpret_cast<PVOID>(
        static_cast<ULONG_PTR>(G3O::s_dumpWorkerThreadId));
    if (!G3O::ExcludeDumpWorkerThread(callbackParam, &callbackInput, &callbackOutput) ||
        callbackOutput.ThreadWriteFlags != 0) {
        std::fprintf(stderr, "FAIL: worker thread is not filtered from the dump\n");
        return 1;
    }

    CONTEXT context = {};
    RtlCaptureContext(&context);
    EXCEPTION_RECORD exceptionRecord = {};
    exceptionRecord.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    exceptionRecord.ExceptionAddress = reinterpret_cast<PVOID>(
        static_cast<ULONG_PTR>(context.Eip));
    EXCEPTION_POINTERS exceptionPointers = { &exceptionRecord, &context };

    const G3O::MiniDumpResult result = G3O::WriteMiniDumpOnWorker(&exceptionPointers);
    const bool fileExists = GetFileAttributesA(G3O::s_dumpPath) != INVALID_FILE_ATTRIBUTES;
    HANDLE dumpFile = fileExists
        ? CreateFileA(G3O::s_dumpPath, GENERIC_READ, FILE_SHARE_READ, nullptr,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)
        : INVALID_HANDLE_VALUE;
    MINIDUMP_HEADER header = {};
    LARGE_INTEGER dumpFileSize = {};
    bool exceptionPreserved = dumpFile != INVALID_HANDLE_VALUE &&
        GetFileSizeEx(dumpFile, &dumpFileSize) &&
        ReadDumpAt(dumpFile, 0, &header, sizeof(header)) &&
        header.Signature == MINIDUMP_SIGNATURE;
    if (exceptionPreserved) {
        const ULONGLONG directoryEnd = static_cast<ULONGLONG>(header.StreamDirectoryRva) +
            static_cast<ULONGLONG>(header.NumberOfStreams) * sizeof(MINIDUMP_DIRECTORY);
        exceptionPreserved = directoryEnd <= static_cast<ULONGLONG>(dumpFileSize.QuadPart);
        bool exceptionStreamFound = false;
        for (ULONG index = 0; exceptionPreserved && index < header.NumberOfStreams; ++index) {
            const DWORD directoryOffset = header.StreamDirectoryRva +
                index * static_cast<DWORD>(sizeof(MINIDUMP_DIRECTORY));
            MINIDUMP_DIRECTORY directory = {};
            if (!ReadDumpAt(dumpFile, directoryOffset, &directory, sizeof(directory))) {
                exceptionPreserved = false;
                break;
            }
            if (directory.StreamType != ExceptionStream) continue;

            exceptionStreamFound = true;
            MINIDUMP_EXCEPTION_STREAM stream = {};
            const ULONGLONG streamEnd = static_cast<ULONGLONG>(directory.Location.Rva) +
                sizeof(stream);
            exceptionPreserved = directory.Location.DataSize >= sizeof(stream) &&
                streamEnd <= static_cast<ULONGLONG>(dumpFileSize.QuadPart) &&
                ReadDumpAt(dumpFile, directory.Location.Rva, &stream, sizeof(stream)) &&
                stream.ThreadId == GetCurrentThreadId() &&
                stream.ExceptionRecord.ExceptionCode == EXCEPTION_ACCESS_VIOLATION;
            break;
        }
        exceptionPreserved = exceptionPreserved && exceptionStreamFound;
    }
    if (dumpFile != INVALID_HANDLE_VALUE) CloseHandle(dumpFile);

    const BOOL fileDeleted = fileExists ? DeleteFileA(G3O::s_dumpPath) : FALSE;
    if (!result.written || !result.headerVerified || result.sizeBytes == 0 ||
        !fileExists || !exceptionPreserved || !fileDeleted) {
        std::fprintf(stderr,
            "FAIL: written=%d header=%d size=%llu error=%lu exists=%d exception=%d deleted=%d\n",
            result.written, result.headerVerified, result.sizeBytes, result.error,
            fileExists, exceptionPreserved, fileDeleted);
        return 1;
    }

    std::puts("PASS: worker wrote a verified dump with the original thread and exception data");
    return 0;
}
