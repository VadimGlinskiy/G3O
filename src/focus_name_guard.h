#pragma once

#include "crash_guards.h"

namespace G3O {

inline void InitializeFocusNameGuard() {
    InitializeCrashGuards();
}

inline void ShutdownFocusNameGuard() {
    ShutdownCrashGuards();
}

} // namespace G3O
