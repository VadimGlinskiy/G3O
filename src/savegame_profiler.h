#pragma once

namespace G3O {

bool InitializeSavegameProfiler();
void ShutdownSavegameProfiler();
bool IsSavegameLoadActive() noexcept;

} // namespace G3O
