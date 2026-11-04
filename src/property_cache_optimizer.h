#pragma once

namespace G3O {

// Hooks SharedBase.dll!ResolveProperty (both by string and by PropertyID) with an O(1) thread-local hash cache.
// Eliminates repetitive byte-by-byte string comparison loops and inheritance tree walks
// during savegame loading (where 100,000+ properties are resolved).
bool InitializePropertyCacheOptimizer();
void ShutdownPropertyCacheOptimizer();

} // namespace G3O
