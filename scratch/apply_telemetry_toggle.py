cpp_path = r'E:\Projects\G3O\src\savegame_profiler.cpp'
with open(cpp_path, 'r', encoding='utf-8') as f:
    content = f.read()

# 1. Add IsTelemetryActive definition
old_abi = 'static_assert(sizeof(void*) == 4, "Savegame profiler hooks require the x86 game ABI.");\n'
new_abi = '''static_assert(sizeof(void*) == 4, "Savegame profiler hooks require the x86 game ABI.");

#ifdef G3O_ENABLE_TELEMETRY
static inline bool IsTelemetryActive() { return true; }
#else
static inline bool IsTelemetryActive() { return GetConfig().enableProfiler; }
#endif
'''
assert old_abi in content, "old_abi not found"
content = content.replace(old_abi, new_abi, 1)

# 2. In Hooked_OnLoadDynamicContext: check IsTelemetryActive()
old_dyn_log = 'if (totalMs >= 20.0) { // Log any layer taking >= 20 ms'
new_dyn_log = 'if (IsTelemetryActive() && totalMs >= 20.0) { // Log any layer taking >= 20 ms'
assert old_dyn_log in content, "old_dyn_log not found"
content = content.replace(old_dyn_log, new_dyn_log, 1)

# 3. In InitMetricsHooks: return early if not active
old_init_hooks = 'static bool InitMetricsHooks() {\n    if (s_metricsHooksInitialized) return true;'
new_init_hooks = '''static bool InitMetricsHooks() {
    if (s_metricsHooksInitialized) return true;
    if (!IsTelemetryActive()) return true;'''
assert old_init_hooks in content, "old_init_hooks not found"
content = content.replace(old_init_hooks, new_init_hooks, 1)

# 4. In Hooked_SectorOnActivate: check IsTelemetryActive()
old_sec_log = 'if (totalMs >= 50.0) {'
new_sec_log = 'if (IsTelemetryActive() && totalMs >= 50.0) {'
assert old_sec_log in content, "old_sec_log not found"
content = content.replace(old_sec_log, new_sec_log, 1)

# 5. In LogLoadReport: only write G3O_LoadProfiler.log if IsTelemetryActive()
old_log_report = 'FILE* fp = fopen("G3O_LoadProfiler.log", "w");'
new_log_report = 'FILE* fp = IsTelemetryActive() ? fopen("G3O_LoadProfiler.log", "w") : nullptr;'
assert old_log_report in content, "old_log_report not found"
content = content.replace(old_log_report, new_log_report, 1)

with open(cpp_path, 'w', encoding='utf-8') as f:
    f.write(content)

print("SUCCESS: savegame_profiler.cpp telemetry toggle applied cleanly!")
