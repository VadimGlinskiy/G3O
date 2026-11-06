cpp_path = r'E:\Projects\G3O\src\savegame_profiler.cpp'
with open(cpp_path, 'r', encoding='utf-8') as f:
    content = f.read()

target = '''#ifdef G3O_ENABLE_TELEMETRY
static inline bool IsTelemetryActive() { return true; }
#else
static inline bool IsTelemetryActive() { return GetConfig().enableProfiler; }
#endif'''

replacement = '''#ifdef G3O_ENABLE_TELEMETRY
static inline bool IsTelemetryActive() { return true; }
#else
static inline bool IsTelemetryActive() { return GetConfig().enableSavegameAudit || GetConfig().enableProfiler; }
#endif'''

assert target in content, "target not found in savegame_profiler.cpp"
content = content.replace(target, replacement, 1)

with open(cpp_path, 'w', encoding='utf-8') as f:
    f.write(content)

print("SUCCESS: updated IsTelemetryActive in savegame_profiler.cpp")
