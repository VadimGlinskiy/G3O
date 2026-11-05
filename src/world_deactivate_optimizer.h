#pragma once
#include <windows.h>

namespace G3O {

bool InitializeWorldDeactivateOptimizer();
void ShutdownWorldDeactivateOptimizer();

void SetWorldDeactivateLoggingSuppressed(bool suppress);
bool IsWorldDeactivateLoggingSuppressed();

double GetLastWorldDeactivateDurationSec();

} // namespace G3O
