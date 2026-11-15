#pragma once

namespace G3O {

// Telemetry-only sampling profiler for the calling thread. Samples EIP (and a
// short EBP-chain caller key) every ~1 ms until Stop(), then logs the hottest
// module+RVA locations. Intended to find hot spots inside engine load code.
void SamplingProfiler_Start(const char* label);
void SamplingProfiler_Stop();

} // namespace G3O
