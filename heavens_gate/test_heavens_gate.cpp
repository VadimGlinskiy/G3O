#include "heavens_gate.h"
#include <iostream>
#include <vector>
#include <iomanip>

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "GOTHIC 3 OPTIMA - HEAVEN'S GATE TEST" << std::endl;
    std::cout << "========================================" << std::endl;

    std::cout << "[*] Checking WOW64 environment..." << std::endl;
    if (!HeavensGate::IsSupported()) {
        std::cerr << "[-] Error: Not running under WOW64 (64-bit Windows required)!" << std::endl;
        return 1;
    }
    std::cout << "[+] Process is running in WOW64 on 64-bit Windows!" << std::endl;

    std::cout << "[*] Initializing Heaven's Gate..." << std::endl;
    if (!HeavensGate::Initialize()) {
        std::cerr << "[-] Error: Failed to initialize Heaven's Gate!" << std::endl;
        return 1;
    }
    std::cout << "[+] Heaven's Gate initialized successfully!" << std::endl;

    uint64_t fnAlloc = HeavensGate::GetNtdll64ProcAddress("NtAllocateVirtualMemory");
    uint64_t fnFree = HeavensGate::GetNtdll64ProcAddress("NtFreeVirtualMemory");

    std::cout << "[+] 64-bit ntdll!NtAllocateVirtualMemory: 0x" << std::hex << fnAlloc << std::endl;
    std::cout << "[+] 64-bit ntdll!NtFreeVirtualMemory:     0x" << std::hex << fnFree << std::endl;

    // Test allocating 100 MB of memory above 4 GB
    const size_t allocSize = 100 * 1024 * 1024; // 100 MB
    std::cout << "[*] Requesting " << std::dec << (allocSize / (1024 * 1024)) << " MB in 64-bit address space..." << std::endl;

    uint64_t highMem = HeavensGate::AllocateHighMemory(allocSize);
    if (highMem == 0) {
        std::cerr << "[-] Error: Failed to allocate high memory!" << std::endl;
        return 1;
    }

    std::cout << "[+] Allocated 64-bit buffer at address: 0x" << std::hex << highMem << std::endl;
    if (highMem > 0xFFFFFFFFULL) {
        std::cout << "[+++] PROOF: Address is ABOVE 4 GB (32-bit limit bypassed)! 0x" 
                  << std::hex << highMem << " > 0xFFFFFFFF" << std::endl;
    } else {
        std::cout << "[!] Address is inside 4 GB range: 0x" << std::hex << highMem << std::endl;
    }

    // Prepare a test pattern in 32-bit space
    const size_t testBlockSize = 64 * 1024; // 64 KB
    std::vector<uint8_t> writeBuffer(testBlockSize);
    for (size_t i = 0; i < testBlockSize; ++i) {
        writeBuffer[i] = static_cast<uint8_t>((i * 7 + 13) & 0xFF);
    }

    std::cout << "[*] Writing test pattern (64 KB) to 64-bit high memory..." << std::endl;
    if (!HeavensGate::WriteHighMemory(highMem, writeBuffer.data(), testBlockSize)) {
        std::cerr << "[-] Error: WriteHighMemory failed!" << std::endl;
        return 1;
    }
    std::cout << "[+] WriteHighMemory succeeded!" << std::endl;

    std::vector<uint8_t> readBuffer(testBlockSize, 0);
    std::cout << "[*] Reading test pattern back from 64-bit high memory..." << std::endl;
    if (!HeavensGate::ReadHighMemory(readBuffer.data(), highMem, testBlockSize)) {
        std::cerr << "[-] Error: ReadHighMemory failed!" << std::endl;
        return 1;
    }
    std::cout << "[+] ReadHighMemory succeeded!" << std::endl;

    // Verify pattern
    bool matched = true;
    for (size_t i = 0; i < testBlockSize; ++i) {
        if (readBuffer[i] != writeBuffer[i]) {
            std::cerr << "[-] Pattern mismatch at index " << i << ": expected " 
                      << (int)writeBuffer[i] << ", got " << (int)readBuffer[i] << std::endl;
            matched = false;
            break;
        }
    }

    if (matched) {
        std::cout << "[+++] VERIFICATION PASSED: Data integrity 100% confirmed across 32-bit <-> 64-bit boundary!" << std::endl;
    } else {
        std::cerr << "[-] VERIFICATION FAILED: Data corruption detected!" << std::endl;
        return 1;
    }

    std::cout << "[*] Freeing 64-bit high memory..." << std::endl;
    if (!HeavensGate::FreeHighMemory(highMem)) {
        std::cerr << "[-] Error: FreeHighMemory failed!" << std::endl;
        return 1;
    }
    std::cout << "[+] FreeHighMemory succeeded! Memory released cleanly." << std::endl;

    std::cout << "========================================" << std::endl;
    std::cout << "ALL HEAVEN'S GATE TESTS PASSED 100%!" << std::endl;
    std::cout << "========================================" << std::endl;
    return 0;
}
