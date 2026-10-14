#pragma once
#include <windows.h>
#include <cstdint>

// Phase 20: Multicore Threading Engine (eCThreadPool Expander, Priority Scheduler & Task Offloader)
// Scales Genome Engine asset streaming pools across physical CPU cores and balances thread priorities.

void InitializeMulticoreManager();
void ShutdownMulticoreManager();

uint32_t GetMulticoreWorkerCount();
uint32_t GetMulticoreActiveJobs();
uint32_t GetMulticoreWorkingThreads();
