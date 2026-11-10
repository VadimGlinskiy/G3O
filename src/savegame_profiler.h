#pragma once

namespace G3O {

bool InitializeSavegameProfiler();
void ShutdownSavegameProfiler();
bool IsSavegameLoadActive() noexcept;
bool IsGotoLoadedPositionActive() noexcept;

} // namespace G3O
