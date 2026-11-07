#include "config_manager.h"
#include "diagnostic_logger.h"
#include "applied_config_snapshot.h"
#include "config_value_parser.h"
#include <fstream>
#include <algorithm>
#include <cctype>
#include <cstring>

namespace G3O {

static Config s_config;

static std::string Trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

static std::string ToUpper(std::string str) {
    for (char& c : str) {
        c = (char)::toupper((unsigned char)c);
    }
    return str;
}

UINT ParseKeyCode(const std::string& rawKeyStr, UINT defaultKey) {
    std::string key = ToUpper(Trim(rawKeyStr));
    if (key.empty()) return defaultKey;

    if (key == "NONE" || key == "DISABLED" || key == "OFF" || key == "0") return 0;

    // Function keys F1 - F24
    if (key.size() >= 2 && key[0] == 'F') {
        int num = 0;
        if (IniValue::ParseInt(key.c_str() + 1, 1, 24, num)) return VK_F1 + (num - 1);
    }

    // Special keys
    if (key == "INSERT" || key == "INS") return VK_INSERT;
    if (key == "DELETE" || key == "DEL") return VK_DELETE;
    if (key == "HOME") return VK_HOME;
    if (key == "END") return VK_END;
    if (key == "PAGEUP" || key == "PGUP" || key == "PRIOR") return VK_PRIOR;
    if (key == "PAGEDOWN" || key == "PGDN" || key == "NEXT") return VK_NEXT;
    if (key == "SCROLL" || key == "SCROLLLOCK") return VK_SCROLL;
    if (key == "PAUSE" || key == "BREAK") return VK_PAUSE;
    if (key == "PRTSC" || key == "PRINTSCREEN" || key == "SNAPSHOT") return VK_SNAPSHOT;
    if (key == "TAB") return VK_TAB;
    if (key == "SPACE") return VK_SPACE;
    if (key == "BACKSPACE" || key == "BS") return VK_BACK;
    if (key == "RETURN" || key == "ENTER") return VK_RETURN;
    if (key == "ESC" || key == "ESCAPE") return VK_ESCAPE;
    if (key == "CAPS" || key == "CAPSLOCK") return VK_CAPITAL;

    // Numpad
    if (key == "NUMPAD0") return VK_NUMPAD0;
    if (key == "NUMPAD1") return VK_NUMPAD1;
    if (key == "NUMPAD2") return VK_NUMPAD2;
    if (key == "NUMPAD3") return VK_NUMPAD3;
    if (key == "NUMPAD4") return VK_NUMPAD4;
    if (key == "NUMPAD5") return VK_NUMPAD5;
    if (key == "NUMPAD6") return VK_NUMPAD6;
    if (key == "NUMPAD7") return VK_NUMPAD7;
    if (key == "NUMPAD8") return VK_NUMPAD8;
    if (key == "NUMPAD9") return VK_NUMPAD9;
    if (key == "NUMPAD_ADD" || key == "NUMPAD+" || key == "ADD") return VK_ADD;
    if (key == "NUMPAD_SUBTRACT" || key == "NUMPAD-" || key == "SUBTRACT") return VK_SUBTRACT;
    if (key == "NUMPAD_MULTIPLY" || key == "NUMPAD*" || key == "MULTIPLY") return VK_MULTIPLY;
    if (key == "NUMPAD_DIVIDE" || key == "NUMPAD/" || key == "DIVIDE") return VK_DIVIDE;
    if (key == "NUMPAD_DECIMAL" || key == "NUMPAD.") return VK_DECIMAL;

    // OEM Punctuation & symbols
    if (key == "[" || key == "{" || key == "BRACKET_LEFT" || key == "LBRACKET") return VK_OEM_4;
    if (key == "]" || key == "}" || key == "BRACKET_RIGHT" || key == "RBRACKET") return VK_OEM_6;
    if (key == "-" || key == "_" || key == "MINUS") return VK_OEM_MINUS;
    if (key == "=" || key == "+" || key == "EQUALS" || key == "PLUS") return VK_OEM_PLUS;
    if (key == ";" || key == ":" || key == "SEMICOLON") return VK_OEM_1;
    if (key == "'" || key == "\"" || key == "QUOTE" || key == "APOSTROPHE") return VK_OEM_7;
    if (key == "`" || key == "~" || key == "TILDE" || key == "GRAVE") return VK_OEM_3;
    if (key == "\\" || key == "|" || key == "BACKSLASH") return VK_OEM_5;
    if (key == "/" || key == "?" || key == "SLASH") return VK_OEM_2;
    if (key == "," || key == "<" || key == "COMMA") return VK_OEM_COMMA;
    if (key == "." || key == ">" || key == "PERIOD" || key == "DOT") return VK_OEM_PERIOD;

    // Hex integer code e.g. "0x79"
    if (key.size() > 2 && key[0] == '0' && key[1] == 'X') {
        std::uintptr_t value = 0;
        if (IniValue::ParseUnsigned(key.c_str(), 0xFF, value)) return static_cast<UINT>(value);
    }

    // Single character 'A'-'Z' or '0'-'9'
    if (key.size() == 1) {
        char ch = key[0];
        if (ch >= 'A' && ch <= 'Z') return (UINT)ch;
        if (ch >= '0' && ch <= '9') return (UINT)ch;
    }

    // Decimal integer
    int value = 0;
    if (IniValue::ParseInt(key.c_str(), 1, 255, value)) return static_cast<UINT>(value);

    return defaultKey;
}

std::string KeyCodeToString(UINT vkCode) {
    if (vkCode == 0) return "NONE";
    if (vkCode >= VK_F1 && vkCode <= VK_F24) {
        return "F" + std::to_string(vkCode - VK_F1 + 1);
    }
    switch (vkCode) {
    case VK_INSERT: return "INSERT";
    case VK_DELETE: return "DELETE";
    case VK_HOME: return "HOME";
    case VK_END: return "END";
    case VK_PRIOR: return "PAGEUP";
    case VK_NEXT: return "PAGEDOWN";
    case VK_SCROLL: return "SCROLLLOCK";
    case VK_PAUSE: return "PAUSE";
    case VK_SNAPSHOT: return "PRINTSCREEN";
    case VK_TAB: return "TAB";
    case VK_SPACE: return "SPACE";
    case VK_BACK: return "BACKSPACE";
    case VK_RETURN: return "ENTER";
    case VK_ESCAPE: return "ESCAPE";
    case VK_OEM_4: return "[";
    case VK_OEM_6: return "]";
    case VK_OEM_MINUS: return "-";
    case VK_OEM_PLUS: return "=";
    case VK_OEM_1: return ";";
    case VK_OEM_7: return "'";
    case VK_OEM_3: return "`";
    case VK_OEM_5: return "\\";
    case VK_OEM_2: return "/";
    case VK_OEM_COMMA: return ",";
    case VK_OEM_PERIOD: return ".";
    case VK_ADD: return "NUMPAD+";
    case VK_SUBTRACT: return "NUMPAD-";
    default:
        if (vkCode >= 'A' && vkCode <= 'Z') return std::string(1, (char)vkCode);
        if (vkCode >= '0' && vkCode <= '9') return std::string(1, (char)vkCode);
        char hexBuf[16];
        snprintf(hexBuf, sizeof(hexBuf), "0x%02X", vkCode);
        return hexBuf;
    }
}

static std::string ResolveIniPath() {
    char exePath[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};

    char* lastSlash = strrchr(exePath, '\\');
    char* forwardSlash = strrchr(exePath, '/');
    if (!lastSlash || (forwardSlash && forwardSlash > lastSlash)) lastSlash = forwardSlash;
    if (!lastSlash) return {};
    *(lastSlash + 1) = '\0';
    return std::string(exePath) + "G3O.ini";
}

static void CreateDefaultIniFile(const std::string& path) {
    std::ofstream ofs(path);
    if (!ofs.is_open()) return;

    ofs << "; ==============================================================================\n"
        << "; Gothic 3 Optima (G3O)\n"
        << "; Настройки хранятся рядом с Gothic3.exe. Файлы диагностики G3O сохраняются в папке G3O рядом с игрой.\n"
        << "; ==============================================================================\n\n"
        << "[General]\n"
        << "; Главный переключатель G3O (true / false).\n"
        << "; Если false, G3O работает исключительно как мост dinput8.dll, отключая все хуки.\n"
        << "EnableG3O=true\n\n"
        << "; Отображать версию G3O в главном меню игры\n"
        << "ShowWatermark=true\n\n"
        << "; Загружать сторонние плагины из папки Data\\G3SE\\Plugins (если есть)\n"
        << "LoadG3SEPlugins=true\n\n"
        << "; Отключать слой Vulkan от Bandicam при запуске с DXVK.\n"
        << "FixVulkanLayerConflicts=true\n\n\n"
        << "[Display]\n"
        << "; Включить режим окна без рамок.\n"
        << "EnableBorderless=true\n\n"
        << "; Клавиша переключения режима окна без рамок.\n"
        << "; Поддерживаются: F1-F12, INSERT, DELETE, HOME, END, PGUP, PGDN, SCROLL, PAUSE, NONE, hex (0x79)\n"
        << "; По умолчанию: F10.\n"
        << "ToggleBorderlessKey=F10\n\n"
        << "; При запуске в оконном режиме включать окно без рамок.\n"
        << "AutoBorderless=true\n\n"
        << "; Не сворачивать окно игры при потере фокуса.\n"
        << "PreventMinimizeOnFocusLoss=true\n\n\n"
        << "[Camera]\n"
        << "; Включить динамическое управление углом обзора (FOV).\n"
        << "EnableDynamicFOV=true\n\n"
        << "; Угол обзора камеры по умолчанию, в градусах.\n"
        << "DefaultFOV=85.0\n\n"
        << "; Разрешить изменение угла обзора на лету горячими клавишами\n"
        << "EnableFOVHotkeys=true\n\n"
        << "; Клавиша уменьшения FOV (работает при зажатом Ctrl, также поддерживается NumPad -)\n"
        << "DecreaseFOVKey=[\n\n"
        << "; Клавиша увеличения FOV (работает при зажатом Ctrl, также поддерживается NumPad +)\n"
        << "IncreaseFOVKey=]\n\n"
        << "; Шаг изменения FOV в градусах при каждом нажатии\n"
        << "FOVStep=5.0\n\n\n"
        << "[CrashInvestigator]\n"
        << "; Перехватывать фатальные ошибки движка, Guru и необработанные исключения.\n"
        << "EnableCrashInvestigator=true\n\n"
        << "; Показывать окно G3O для необработанного Win32-исключения. Для сообщений Guru/FatalError используется интерфейс движка.\n"
        << "ShowCrashDialog=true\n\n"
        << "; Создавать минидамп (.dmp) при необработанном Win32-исключении; движковый FatalError не предоставляет его контекст.\n"
        << "WriteCrashDump=true\n\n"
        << "; Записывать отдельный отчёт о сбое с временной меткой в папке G3O.\n"
        << "WriteCrashLog=true\n\n\n"
        << "[Performance]\n"
        << "; Включить оптимизации SharedBase: пересечение коробок, сравнение строк и проверку Windows NT.\n"
        << "EnableSharedBaseOptimizations=true\n\n"
        << "; Распаковывать PAK и сохранения с помощью libdeflate.\n"
        << "EnableFastPakDecompressor=true\n\n"
        << "; Кэшировать поиск свойств сущностей.\n"
        << "EnablePropertyCache=false\n\n"
        << "; Прогрев файлов PAK и распаковка блоков по трассе сохранения (для распаковки требуется EnableFastPakDecompressor=true).\n"
        << "EnablePakPrefetch=true\n\n"
        << "; Фильтровать повторные установки состояний Direct3D 9.\n"
        << "EnableD3D9StateFilter=false\n\n"
        << "; Включить ускоренный путь загрузки текстур.\n"
        << "EnableFastTextureLoader=true\n\n"
        << "; Ограничивать расчёт дальних теней растительности.\n"
        << "EnableVegetationOptimizer=true\n\n"
        << "; Дальность теней деревьев в игровых единицах (100 единиц = 1 метр).\n"
        << "TreeShadowDistance=6500.0\n\n"
        << "; Ограничение радиуса сетки травы в игровых единицах (0 = отключить).\n"
        << "VegetationGridRangeClamp=7500.0\n\n"
        << "; Запрашивать период системного таймера Windows 1 мс.\n"
        << "EnableHighPrecisionTimer=true\n\n"
        << "; Устанавливать процессу приоритет HIGH_PRIORITY_CLASS.\n"
        << "EnableHighProcessPriority=true\n\n"
        << "; Учитывать топологию CPU при настройке привязки к ядрам.\n"
        << "EnableCPUTopologyAffinity=true\n\n"
        << "; Кэшировать положительные результаты поиска VFS.\n"
        << "EnableVFSCache=false\n\n"
        << "; Ограничение числа физических ядер (0 = автоматически).\n"
        << "CPUCores=0\n\n"
        << "; Ограничение числа логических потоков (0 = автоматически; имеет приоритет над CPUCores).\n"
        << "CPUThreads=0\n\n"
        << "; Явная маска affinity (0 = автоматически).\n"
        << "CPUAffinityMask=0x0\n\n"
        << "; Общий лимит кэша предварительной распаковки в МБ (0 = отключить).\n"
        << "PakCacheBudgetMB=192\n\n"
        << "; Число потоков предварительной распаковки (0 = автоматически, максимум 8).\n"
        << "PakPrefetchWorkers=0\n\n\n"
        << "[Logging]\n"
        << "; Записывать отдельный диагностический журнал с временной меткой в G3O.\n"
        << "EnableDiagnosticLog=false\n\n"
        << "; Включить запись сообщений OutputDebugStringA в диагностический журнал.\n"
        << "InterceptOutputDebugString=false\n\n"
        << "; Записывать профилировщик в G3O в отдельный файл с временной меткой.\n"
        << "EnableProfiler=false\n\n"
        << "; Записывать подробные замеры загрузки сохранения в G3O.\n"
        << "EnableSavegameAudit=false\n";
    ofs.close();
}

static bool ReadIniBool(const char* section, const char* key, bool defaultVal, const char* path) {
    char buf[32] = { 0 };
    GetPrivateProfileStringA(section, key, defaultVal ? "true" : "false", buf, sizeof(buf), path);
    std::string val = ToUpper(Trim(buf));
    if (val == "TRUE" || val == "1" || val == "YES" || val == "ON") return true;
    if (val == "FALSE" || val == "0" || val == "NO" || val == "OFF") return false;
    return defaultVal;
}

static bool ReadIniBoolWithLegacyKey(const char* section, const char* key,
                                     const char* legacyKey, bool defaultVal,
                                     const char* path) {
    char value[32] = {};
    if (GetPrivateProfileStringA(section, key, "", value, sizeof(value), path) > 0) {
        return ReadIniBool(section, key, defaultVal, path);
    }
    return ReadIniBool(section, legacyKey, defaultVal, path);
}

static std::string ReadIniString(const char* section, const char* key, const char* defaultVal, const char* path) {
    char buf[256] = { 0 };
    GetPrivateProfileStringA(section, key, defaultVal, buf, sizeof(buf), path);
    return Trim(buf);
}

static float ReadIniFloat(const char* section, const char* key, float defaultVal,
                          float minimum, float maximum, const char* path) {
    char buf[64] = { 0 };
    char defBuf[32];
    snprintf(defBuf, sizeof(defBuf), "%.9g", defaultVal);
    GetPrivateProfileStringA(section, key, defBuf, buf, sizeof(buf), path);
    float value = defaultVal;
    return IniValue::ParseFloat(buf, minimum, maximum, value) ? value : defaultVal;
}

static int ReadIniInt(const char* section, const char* key, int defaultVal,
                      int minimum, int maximum, const char* path) {
    char buf[64] = {};
    char defBuf[32] = {};
    snprintf(defBuf, sizeof(defBuf), "%d", defaultVal);
    GetPrivateProfileStringA(section, key, defBuf, buf, sizeof(buf), path);
    int value = defaultVal;
    return IniValue::ParseInt(buf, minimum, maximum, value) ? value : defaultVal;
}

static DWORD_PTR ReadIniHex(const char* section, const char* key, DWORD_PTR defaultVal, const char* path) {
    char buf[64] = {};
    GetPrivateProfileStringA(section, key, "0", buf, sizeof(buf), path);
    std::uintptr_t value = 0;
    const std::uintptr_t maximum = static_cast<std::uintptr_t>(~static_cast<DWORD_PTR>(0));
    return IniValue::ParseUnsigned(buf, maximum, value)
        ? static_cast<DWORD_PTR>(value)
        : defaultVal;
}

const Config& GetConfig() {
    return s_config;
}

void LoadConfig() {
    std::string iniPath = ResolveIniPath();
    s_config.iniFilePath = iniPath;

    DWORD attr = GetFileAttributesA(iniPath.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        CreateDefaultIniFile(iniPath);
    }

    const char* path = iniPath.c_str();

    // [General]
    s_config.enableG3O = ReadIniBool("General", "EnableG3O", true, path);
    s_config.showWatermark = ReadIniBool("General", "ShowWatermark", true, path);
    s_config.loadG3SEPlugins = ReadIniBool("General", "LoadG3SEPlugins", true, path);
    s_config.fixVulkanLayerConflicts = ReadIniBool("General", "FixVulkanLayerConflicts", true, path);

    // [Display]
    s_config.enableBorderless = ReadIniBool("Display", "EnableBorderless", true, path);
    std::string borderlessKeyStr = ReadIniString("Display", "ToggleBorderlessKey", "F10", path);
    s_config.toggleBorderlessKey = ParseKeyCode(borderlessKeyStr, VK_F10);
    s_config.autoBorderless = ReadIniBool("Display", "AutoBorderless", true, path);
    s_config.preventMinimizeOnFocusLoss = ReadIniBool("Display", "PreventMinimizeOnFocusLoss", true, path);

    // [Camera]
    s_config.enableDynamicFOV = ReadIniBool("Camera", "EnableDynamicFOV", true, path);
    s_config.defaultFOV = ReadIniFloat("Camera", "DefaultFOV", 85.0f, 50.0f, 115.0f, path);
    s_config.enableFOVHotkeys = ReadIniBool("Camera", "EnableFOVHotkeys", true, path);
    std::string decKeyStr = ReadIniString("Camera", "DecreaseFOVKey", "[", path);
    s_config.decreaseFOVKey = ParseKeyCode(decKeyStr, VK_OEM_4);
    std::string incKeyStr = ReadIniString("Camera", "IncreaseFOVKey", "]", path);
    s_config.increaseFOVKey = ParseKeyCode(incKeyStr, VK_OEM_6);
    s_config.fovStep = ReadIniFloat("Camera", "FOVStep", 5.0f, 0.1f, 30.0f, path);

    // [CrashInvestigator]
    s_config.enableCrashInvestigator = ReadIniBool("CrashInvestigator", "EnableCrashInvestigator", true, path);
    s_config.showCrashDialog = ReadIniBool("CrashInvestigator", "ShowCrashDialog", true, path);
    s_config.writeCrashDump = ReadIniBool("CrashInvestigator", "WriteCrashDump", true, path);
    s_config.writeCrashLog = ReadIniBool("CrashInvestigator", "WriteCrashLog", true, path);

    // [Performance]
    s_config.enableSharedBaseOptimizations = ReadIniBoolWithLegacyKey(
        "Performance", "EnableSharedBaseOptimizations", "EnableSIMDFrustumCulling", true, path);
    s_config.enableFastPakDecompressor = ReadIniBool("Performance", "EnableFastPakDecompressor", true, path);
    s_config.enablePropertyCache = ReadIniBool("Performance", "EnablePropertyCache", false, path);
    s_config.enablePakPrefetch = ReadIniBool("Performance", "EnablePakPrefetch", true, path);

    s_config.enableD3D9StateFilter = ReadIniBool("Performance", "EnableD3D9StateFilter", false, path);
    s_config.enableFastTextureLoader = ReadIniBool("Performance", "EnableFastTextureLoader", true, path);
    s_config.enableVegetationOptimizer = ReadIniBool("Performance", "EnableVegetationOptimizer", true, path);
    s_config.treeShadowDistance = ReadIniFloat("Performance", "TreeShadowDistance", 6500.0f, 0.0f, 100000.0f, path);
    s_config.vegetationGridRangeClamp = ReadIniFloat("Performance", "VegetationGridRangeClamp", 7500.0f, 0.0f, 100000.0f, path);
    s_config.enableHighPrecisionTimer = ReadIniBool("Performance", "EnableHighPrecisionTimer", true, path);
    s_config.enableHighProcessPriority = ReadIniBool("Performance", "EnableHighProcessPriority", true, path);
    s_config.enableCPUTopologyAffinity = ReadIniBool("Performance", "EnableCPUTopologyAffinity", true, path);
    s_config.enableVFSCache = ReadIniBool("Performance", "EnableVFSCache", false, path);
    s_config.cpuCores = ReadIniInt("Performance", "CPUCores", 0, 0, 32, path);
    s_config.cpuThreads = ReadIniInt("Performance", "CPUThreads", 0, 0, 32, path);
    s_config.cpuAffinityMask = ReadIniHex("Performance", "CPUAffinityMask", 0, path);
    s_config.pakCacheBudgetMB = ReadIniInt("Performance", "PakCacheBudgetMB", 192, 0, 1024, path);
    s_config.pakPrefetchWorkers = ReadIniInt("Performance", "PakPrefetchWorkers", 0, 0, 8, path);

    // [Logging]
    s_config.enableDiagnosticLog = ReadIniBool("Logging", "EnableDiagnosticLog", false, path);
    s_config.interceptOutputDebugString = ReadIniBool("Logging", "InterceptOutputDebugString", false, path);
    s_config.enableProfiler = ReadIniBool("Logging", "EnableProfiler", false, path);
    s_config.enableSavegameAudit = ReadIniBool("Logging", "EnableSavegameAudit", false, path);
    s_config.enableLayerAudit = ReadIniBool("Logging", "EnableLayerAudit", false, path);

}

void LogLoadedConfig() {
    Log("G3O", "Configuration loaded from '%s'. Full applied values follow:", s_config.iniFilePath.c_str());
    char snapshotStorage[8192] = {};
    FixedTextBuffer snapshot(snapshotStorage, sizeof(snapshotStorage));
    AppendAppliedConfigSnapshot(snapshot, s_config);
    const char* lineStart = snapshot.CStr();
    while (*lineStart) {
        const char* lineEnd = std::strchr(lineStart, '\n');
        const size_t lineLength = lineEnd
            ? static_cast<size_t>(lineEnd - lineStart)
            : std::strlen(lineStart);
        if (lineLength > 0) {
            Log("Config", "%.*s", static_cast<int>(lineLength), lineStart);
        }
        if (!lineEnd) break;
        lineStart = lineEnd + 1;
    }
}

} // namespace G3O
