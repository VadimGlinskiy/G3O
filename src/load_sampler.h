#pragma once
#include <windows.h>
#include <stdio.h>

namespace G3O {
bool StartLoadSampling(DWORD threadId);
unsigned StopLoadSampling(FILE* report);
}
