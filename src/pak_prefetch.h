#pragma once
namespace G3O {
// Warms the Windows file cache with Data\*.pak in a low-priority background thread.
void InitializePakPrefetch();
}
