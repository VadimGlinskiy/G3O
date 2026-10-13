#include "heavens_gate.h"
#include <vector>
#include <string>
#include <cstring>
#include <iostream>

namespace HeavensGate {

    static uint64_t g_ntdll64Base = 0;
    static uint64_t g_peb64 = 0;
    static uint64_t g_fnNtAllocateVirtualMemory = 0;
    static uint64_t g_fnNtFreeVirtualMemory = 0;
    static bool g_initialized = false;

    bool IsSupported() {
        BOOL isWow64 = FALSE;
        typedef BOOL(WINAPI* LPFN_ISWOW64PROCESS)(HANDLE, PBOOL);
        LPFN_ISWOW64PROCESS fnIsWow64Process = (LPFN_ISWOW64PROCESS)GetProcAddress(
            GetModuleHandleA("kernel32.dll"), "IsWow64Process");
        if (fnIsWow64Process) {
            fnIsWow64Process(GetCurrentProcess(), &isWow64);
        }
        return isWow64 == TRUE;
    }

    // Naked query of 64-bit PEB from GS:[0x60]
    extern "C" __declspec(naked) uint64_t __cdecl RawQueryPeb64() {
        __asm {
            push ebx
            push esi
            push edi
            push ebp

            mov ebp, esp
            sub esp, 0x20

            push 0x33
            call to_64_peb
        to_64_peb:
            add dword ptr [esp], 5
            retf

            // --- 64-BIT MODE ---
            // mov ebp, ebp (zero-extend ebp to rbp): 89 ed
            _emit 0x89
            _emit 0xED

            // mov rax, gs:[0x60]: 65 48 8b 04 25 60 00 00 00
            _emit 0x65
            _emit 0x48
            _emit 0x8B
            _emit 0x04
            _emit 0x25
            _emit 0x60
            _emit 0x00
            _emit 0x00
            _emit 0x00

            // mov [rbp - 0x10], rax: 48 89 45 f0
            _emit 0x48
            _emit 0x89
            _emit 0x45
            _emit 0xF0

            // sub rsp, 8: 48 83 ec 08
            _emit 0x48
            _emit 0x83
            _emit 0xEC
            _emit 0x08

            // mov dword ptr [rsp + 4], 0x23: c7 44 24 04 23 00 00 00
            _emit 0xC7
            _emit 0x44
            _emit 0x24
            _emit 0x04
            _emit 0x23
            _emit 0x00
            _emit 0x00
            _emit 0x00

            // lea edx, [rip + 4]: 8d 15 04 00 00 00
            _emit 0x8D
            _emit 0x15
            _emit 0x04
            _emit 0x00
            _emit 0x00
            _emit 0x00

            // mov [rsp], edx: 89 14 24
            _emit 0x89
            _emit 0x14
            _emit 0x24

            // retf: cb
            _emit 0xCB

            // --- 32-BIT MODE ---
            mov eax, dword ptr [ebp - 0x10]
            mov edx, dword ptr [ebp - 0x0C]
            mov esp, ebp

            pop ebp
            pop edi
            pop esi
            pop ebx
            ret
        }
    }

    // Naked memory copy from 64-bit space to 32-bit buffer
    extern "C" __declspec(naked) bool __cdecl RawRead64(void* dst32, uint64_t src64, uint32_t bytes) {
        __asm {
            push ebx
            push esi
            push edi
            push ebp

            mov ebp, esp

            push 0x33
            call to_64_read
        to_64_read:
            add dword ptr [esp], 5
            retf

            // --- 64-BIT MODE ---
            // mov ebp, ebp (zero-extend ebp to rbp): 89 ed
            _emit 0x89
            _emit 0xED

            // mov edi, [rbp + 20] (dst32, zero-extended): 8b 7d 14
            _emit 0x8B
            _emit 0x7D
            _emit 0x14

            // mov rsi, [rbp + 24] (src64): 48 8b 75 18
            _emit 0x48
            _emit 0x8B
            _emit 0x75
            _emit 0x18

            // mov ecx, [rbp + 32] (bytes, zero-extended): 8b 4d 20
            _emit 0x8B
            _emit 0x4D
            _emit 0x20

            // rep movsb: f3 a4
            _emit 0xF3
            _emit 0xA4

            // sub rsp, 8: 48 83 ec 08
            _emit 0x48
            _emit 0x83
            _emit 0xEC
            _emit 0x08

            // mov dword ptr [rsp + 4], 0x23: c7 44 24 04 23 00 00 00
            _emit 0xC7
            _emit 0x44
            _emit 0x24
            _emit 0x04
            _emit 0x23
            _emit 0x00
            _emit 0x00
            _emit 0x00

            // lea edx, [rip + 4]: 8d 15 04 00 00 00
            _emit 0x8D
            _emit 0x15
            _emit 0x04
            _emit 0x00
            _emit 0x00
            _emit 0x00

            // mov [rsp], edx: 89 14 24
            _emit 0x89
            _emit 0x14
            _emit 0x24

            // retf: cb
            _emit 0xCB

            // --- 32-BIT MODE ---
            mov esp, ebp
            mov eax, 1

            pop ebp
            pop edi
            pop esi
            pop ebx
            ret
        }
    }

    // Naked memory copy from 32-bit buffer to 64-bit space
    extern "C" __declspec(naked) bool __cdecl RawWrite64(uint64_t dst64, const void* src32, uint32_t bytes) {
        __asm {
            push ebx
            push esi
            push edi
            push ebp

            mov ebp, esp

            push 0x33
            call to_64_write
        to_64_write:
            add dword ptr [esp], 5
            retf

            // --- 64-BIT MODE ---
            // mov ebp, ebp (zero-extend ebp to rbp): 89 ed
            _emit 0x89
            _emit 0xED

            // mov rdi, [rbp + 20] (dst64): 48 8b 7d 14
            _emit 0x48
            _emit 0x8B
            _emit 0x7D
            _emit 0x14

            // mov esi, [rbp + 28] (src32, zero-extended): 8b 75 1c
            _emit 0x8B
            _emit 0x75
            _emit 0x1C

            // mov ecx, [rbp + 32] (bytes, zero-extended): 8b 4d 20
            _emit 0x8B
            _emit 0x4D
            _emit 0x20

            // rep movsb: f3 a4
            _emit 0xF3
            _emit 0xA4

            // sub rsp, 8: 48 83 ec 08
            _emit 0x48
            _emit 0x83
            _emit 0xEC
            _emit 0x08

            // mov dword ptr [rsp + 4], 0x23: c7 44 24 04 23 00 00 00
            _emit 0xC7
            _emit 0x44
            _emit 0x24
            _emit 0x04
            _emit 0x23
            _emit 0x00
            _emit 0x00
            _emit 0x00

            // lea edx, [rip + 4]: 8d 15 04 00 00 00
            _emit 0x8D
            _emit 0x15
            _emit 0x04
            _emit 0x00
            _emit 0x00
            _emit 0x00

            // mov [rsp], edx: 89 14 24
            _emit 0x89
            _emit 0x14
            _emit 0x24

            // retf: cb
            _emit 0xCB

            // --- 32-BIT MODE ---
            mov esp, ebp
            mov eax, 1

            pop ebp
            pop edi
            pop esi
            pop ebx
            ret
        }
    }

    // Naked Call64 dispatcher supporting up to 6 arguments
    extern "C" __declspec(naked) uint64_t __cdecl RawCall64(uint64_t funcAddr, uint32_t numArgs, const uint64_t* args) {
        __asm {
            push ebx
            push esi
            push edi
            push ebp

            mov ebp, esp

            push 0x33
            call to_64_call
        to_64_call:
            add dword ptr [esp], 5
            retf

            // --- 64-BIT MODE ---
            // mov ebp, ebp (zero-extend ebp to rbp): 89 ed
            _emit 0x89
            _emit 0xED

            // mov r12, rsp: 49 89 e4
            _emit 0x49
            _emit 0x89
            _emit 0xE4

            // and rsp, -16: 48 83 e4 f0
            _emit 0x48
            _emit 0x83
            _emit 0xE4
            _emit 0xF0

            // sub rsp, 0x40 (shadow space + stack args): 48 83 ec 40
            _emit 0x48
            _emit 0x83
            _emit 0xEC
            _emit 0x40

            // ebx = args (32-bit pointer zero-extended into rbx): mov ebx, [rbp + 32] -> 8b 5d 20
            _emit 0x8B
            _emit 0x5D
            _emit 0x20

            // Load RCX = args[0]: 48 8b 0b
            _emit 0x48
            _emit 0x8B
            _emit 0x0B

            // Load RDX = args[1]: 48 8b 53 08
            _emit 0x48
            _emit 0x8B
            _emit 0x53
            _emit 0x08

            // Load R8 = args[2]: 4c 8b 43 10
            _emit 0x4C
            _emit 0x8B
            _emit 0x43
            _emit 0x10

            // Load R9 = args[3]: 4c 8b 4b 18
            _emit 0x4C
            _emit 0x8B
            _emit 0x4B
            _emit 0x18

            // Copy 5th arg to [rsp + 0x20]:
            // mov rax, [rbx + 32] -> 48 8b 43 20
            _emit 0x48
            _emit 0x8B
            _emit 0x43
            _emit 0x20
            // mov [rsp + 0x20], rax -> 48 89 44 24 20
            _emit 0x48
            _emit 0x89
            _emit 0x44
            _emit 0x24
            _emit 0x20

            // Copy 6th arg to [rsp + 0x28]:
            // mov rax, [rbx + 40] -> 48 8b 43 28
            _emit 0x48
            _emit 0x8B
            _emit 0x43
            _emit 0x28
            // mov [rsp + 0x28], rax -> 48 89 44 24 28
            _emit 0x48
            _emit 0x89
            _emit 0x44
            _emit 0x24
            _emit 0x28

            // Load funcAddr into rax: mov rax, [rbp + 20] -> 48 8b 45 14
            _emit 0x48
            _emit 0x8B
            _emit 0x45
            _emit 0x14

            // call rax: ff d0
            _emit 0xFF
            _emit 0xD0

            // Store return value rax into [rbp + 20]: mov [rbp + 20], rax -> 48 89 45 14
            _emit 0x48
            _emit 0x89
            _emit 0x45
            _emit 0x14

            // Restore rsp: mov rsp, r12 -> 49 89 e4
            _emit 0x49
            _emit 0x89
            _emit 0xE4

            // sub rsp, 8: 48 83 ec 08
            _emit 0x48
            _emit 0x83
            _emit 0xEC
            _emit 0x08

            // mov dword ptr [rsp + 4], 0x23: c7 44 24 04 23 00 00 00
            _emit 0xC7
            _emit 0x44
            _emit 0x24
            _emit 0x04
            _emit 0x23
            _emit 0x00
            _emit 0x00
            _emit 0x00

            // lea edx, [rip + 4]: 8d 15 04 00 00 00
            _emit 0x8D
            _emit 0x15
            _emit 0x04
            _emit 0x00
            _emit 0x00
            _emit 0x00

            // mov [rsp], edx: 89 14 24
            _emit 0x89
            _emit 0x14
            _emit 0x24

            // retf: cb
            _emit 0xCB

            // --- 32-BIT MODE ---
            mov esp, ebp
            mov eax, dword ptr [ebp + 20]
            mov edx, dword ptr [ebp + 24]

            pop ebp
            pop edi
            pop esi
            pop ebx
            ret
        }
    }

    bool ReadHighMemory(void* dst32, uint64_t src64, size_t bytes) {
        if (!dst32 || !src64 || bytes == 0) return false;
        return RawRead64(dst32, src64, static_cast<uint32_t>(bytes));
    }

    bool WriteHighMemory(uint64_t dst64, const void* src32, size_t bytes) {
        if (!dst64 || !src32 || bytes == 0) return false;
        return RawWrite64(dst64, src32, static_cast<uint32_t>(bytes));
    }

    uint64_t Call64(uint64_t funcAddr, size_t numArgs, ...) {
        std::vector<uint64_t> args(numArgs > 0 ? numArgs : 1, 0);
        va_list vl;
        va_start(vl, numArgs);
        for (size_t i = 0; i < numArgs; ++i) {
            args[i] = va_arg(vl, uint64_t);
        }
        va_end(vl);
        return RawCall64(funcAddr, static_cast<uint32_t>(numArgs), args.data());
    }

    static uint64_t FindNtdll64Base() {
        if (g_ntdll64Base != 0) return g_ntdll64Base;

        uint64_t peb64 = RawQueryPeb64();
        if (peb64 == 0) return 0;
        g_peb64 = peb64;

        // In PEB64: Ldr pointer is at PEB64 + 0x18
        uint64_t ldr64 = 0;
        if (!ReadHighMemory(&ldr64, peb64 + 0x18, sizeof(ldr64)) || ldr64 == 0) {
            return 0;
        }

        // In PEB_LDR_DATA64: InLoadOrderModuleList is at Ldr + 0x10
        uint64_t head = ldr64 + 0x10;
        uint64_t flink = 0;
        if (!ReadHighMemory(&flink, head, sizeof(flink)) || flink == 0) {
            return 0;
        }

        // Iterate through loaded modules
        uint64_t current = flink;
        int maxModules = 100;
        while (current != 0 && current != head && maxModules-- > 0) {
            uint64_t dllBase = 0;
            ReadHighMemory(&dllBase, current + 0x30, sizeof(dllBase));

            uint16_t nameLen = 0;
            ReadHighMemory(&nameLen, current + 0x58, sizeof(nameLen));

            uint64_t nameBuffer = 0;
            ReadHighMemory(&nameBuffer, current + 0x60, sizeof(nameBuffer));

            if (nameLen > 0 && nameBuffer != 0 && nameLen < 256) {
                std::vector<wchar_t> wname((nameLen / sizeof(wchar_t)) + 1, 0);
                ReadHighMemory(wname.data(), nameBuffer, nameLen);

                if (_wcsicmp(wname.data(), L"ntdll.dll") == 0) {
                    g_ntdll64Base = dllBase;
                    return dllBase;
                }
            }

            if (!ReadHighMemory(&current, current, sizeof(current))) {
                break;
            }
        }

        return 0;
    }

    uint64_t GetNtdll64ProcAddress(const char* procName) {
        if (!procName) return 0;
        uint64_t ntdll = FindNtdll64Base();
        if (ntdll == 0) return 0;

        IMAGE_DOS_HEADER dosHeader{};
        if (!ReadHighMemory(&dosHeader, ntdll, sizeof(dosHeader))) return 0;
        if (dosHeader.e_magic != IMAGE_DOS_SIGNATURE) return 0;

        uint64_t ntHeadersAddr = ntdll + dosHeader.e_lfanew;
        IMAGE_NT_HEADERS64 ntHeaders{};
        if (!ReadHighMemory(&ntHeaders, ntHeadersAddr, sizeof(ntHeaders))) return 0;
        if (ntHeaders.Signature != IMAGE_NT_SIGNATURE) return 0;

        IMAGE_DATA_DIRECTORY exportDirEntry = ntHeaders.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (exportDirEntry.VirtualAddress == 0 || exportDirEntry.Size == 0) return 0;

        uint64_t exportDirAddr = ntdll + exportDirEntry.VirtualAddress;
        IMAGE_EXPORT_DIRECTORY exportDir{};
        if (!ReadHighMemory(&exportDir, exportDirAddr, sizeof(exportDir))) return 0;

        uint64_t namesAddr = ntdll + exportDir.AddressOfNames;
        uint64_t ordinalsAddr = ntdll + exportDir.AddressOfNameOrdinals;
        uint64_t functionsAddr = ntdll + exportDir.AddressOfFunctions;

        std::vector<uint32_t> nameRvas(exportDir.NumberOfNames);
        std::vector<uint16_t> ordinals(exportDir.NumberOfNames);
        ReadHighMemory(nameRvas.data(), namesAddr, exportDir.NumberOfNames * sizeof(uint32_t));
        ReadHighMemory(ordinals.data(), ordinalsAddr, exportDir.NumberOfNames * sizeof(uint16_t));

        char nameBuffer[128];
        for (DWORD i = 0; i < exportDir.NumberOfNames; ++i) {
            uint64_t currentNameAddr = ntdll + nameRvas[i];
            std::memset(nameBuffer, 0, sizeof(nameBuffer));
            ReadHighMemory(nameBuffer, currentNameAddr, sizeof(nameBuffer) - 1);

            if (std::strcmp(nameBuffer, procName) == 0) {
                uint16_t ordinal = ordinals[i];
                uint32_t funcRva = 0;
                ReadHighMemory(&funcRva, functionsAddr + (ordinal * sizeof(uint32_t)), sizeof(funcRva));
                return ntdll + funcRva;
            }
        }

        return 0;
    }

    bool Initialize() {
        if (g_initialized) return true;
        if (!IsSupported()) return false;

        uint64_t ntdll = FindNtdll64Base();
        if (ntdll == 0) return false;

        g_fnNtAllocateVirtualMemory = GetNtdll64ProcAddress("NtAllocateVirtualMemory");
        g_fnNtFreeVirtualMemory = GetNtdll64ProcAddress("NtFreeVirtualMemory");

        if (g_fnNtAllocateVirtualMemory == 0 || g_fnNtFreeVirtualMemory == 0) {
            return false;
        }

        g_initialized = true;
        return true;
    }

    uint64_t AllocateHighMemory(size_t sizeInBytes) {
        if (!Initialize()) return 0;

        // Try allocating above 4 GB first (start at 4 GB boundary: 0x100000000)
        uint64_t baseAddressBuf = 0x0000000100000000ULL;
        uint64_t regionSizeBuf = static_cast<uint64_t>(sizeInBytes);

        uint64_t pBaseAddress = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&baseAddressBuf));
        uint64_t pRegionSize = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&regionSizeBuf));

        uint64_t processHandle = static_cast<uint64_t>(-1);
        uint64_t zeroBits = 0;
        uint64_t allocType = MEM_RESERVE | MEM_COMMIT;
        uint64_t protect = PAGE_READWRITE;

        uint64_t status = Call64(
            g_fnNtAllocateVirtualMemory,
            6,
            processHandle,
            pBaseAddress,
            zeroBits,
            pRegionSize,
            allocType,
            protect
        );

        if (status == 0) {
            return baseAddressBuf;
        }

        // If specific address failed, let OS allocate anywhere (base = 0)
        baseAddressBuf = 0;
        regionSizeBuf = static_cast<uint64_t>(sizeInBytes);
        status = Call64(
            g_fnNtAllocateVirtualMemory,
            6,
            processHandle,
            pBaseAddress,
            zeroBits,
            pRegionSize,
            allocType,
            protect
        );

        if (status == 0) {
            return baseAddressBuf;
        }

        return 0;
    }

    bool FreeHighMemory(uint64_t addr64) {
        if (!Initialize() || addr64 == 0) return false;

        uint64_t baseAddressBuf = addr64;
        uint64_t regionSizeBuf = 0;

        uint64_t pBaseAddress = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&baseAddressBuf));
        uint64_t pRegionSize = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&regionSizeBuf));

        uint64_t processHandle = static_cast<uint64_t>(-1);
        uint64_t freeType = MEM_RELEASE;

        uint64_t status = Call64(
            g_fnNtFreeVirtualMemory,
            4,
            processHandle,
            pBaseAddress,
            pRegionSize,
            freeType
        );

        return status == 0;
    }

} // namespace HeavensGate
