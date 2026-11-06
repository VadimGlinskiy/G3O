#include "cpu_affinity_policy.h"
#include <cstdio>

static bool Expect(bool condition, const char* message) {
    if (condition) return true;
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

int main() {
    using G3O::CpuAffinity::Mask;
    bool ok = true;
    ok &= Expect(G3O::CpuAffinity::CanRepresentProcessorCount(32, 32),
                 "a 32-bit affinity mask should represent exactly 32 processors");
    ok &= Expect(!G3O::CpuAffinity::CanRepresentProcessorCount(33, 32),
                 "a 32-bit WOW64 process must not apply an ambiguous mask above 32 processors");
    ok &= Expect(!G3O::CpuAffinity::CanRepresentProcessorCount(0, 32),
                 "an unavailable processor count must disable affinity changes");
    const Mask available = 0b101101;
    ok &= Expect(G3O::CpuAffinity::SelectLogicalProcessors(available, 3) == 0b001101,
                 "logical thread limit should choose only available processors");
    ok &= Expect(G3O::CpuAffinity::SelectLogicalProcessors(available, 0) == 0,
                 "zero requested threads should select none");
    ok &= Expect(G3O::CpuAffinity::IsValidMask(0b001101, available),
                 "manual mask contained in the system mask should be valid");
    ok &= Expect(!G3O::CpuAffinity::IsValidMask(0b111101, available),
                 "manual mask containing unavailable processors should be rejected");

    const Mask physicalCores[] = { 0b00011, 0b01100, 0b10000 };
    ok &= Expect(G3O::CpuAffinity::SelectPhysicalCores(physicalCores, 3, 2, 0b11111) == 0b00101,
                 "physical core limit should select one logical processor per core");
    ok &= Expect(G3O::CpuAffinity::SelectPhysicalCores(physicalCores, 3, 8, 0b01101) == 0b00101,
                 "physical core selection should honor the allowed mask and available core count");
    const Mask overlappingCores[] = { 0b00011, 0b00011, 0b01100 };
    ok &= Expect(G3O::CpuAffinity::SelectPhysicalCores(overlappingCores, 3, 2, 0b01111) == 0b00101,
                 "overlapping topology records should not count the same core twice");

    if (!ok) return 1;
    std::puts("PASS: affinity masks are validated and core/thread limits select valid processors");
    return 0;
}
