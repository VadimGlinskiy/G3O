#pragma once

namespace G3O {

bool InitializeAutoTestRunner();
void ShutdownAutoTestRunner();

void AutoTestNotifyLoadStarted();
void AutoTestNotifyLoadFinished(bool succeeded, double totalSec, const char* loadProfilerPath);

} // namespace G3O
