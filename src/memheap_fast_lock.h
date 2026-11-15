#pragma once

namespace G3O {

// Replaces the cost of MemHeap's global CRITICAL_SECTION (RtlEnter/LeaveCriticalSection in
// ntdll) with a recursive user-mode spin lock. Only SharedBase.dll's own IAT entries are
// patched and only calls on the MemHeap critical section are redirected, so every MemHeap
// entry point (including vanilla fallbacks) stays mutually exclusive.
bool InitializeMemHeapFastLock();
void ShutdownMemHeapFastLock();

} // namespace G3O
