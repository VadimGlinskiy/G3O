#include "config_manager.h"
#include "diagnostic_logger.h"
#include <fstream>
#include <algorithm>
#include <cctype>

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
        try {
            int num = std::stoi(key.substr(1));
            if (num >= 1 && num <= 24) {
                return VK_F1 + (num - 1);
            }
        } catch (...) {}
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
        try {
            return (UINT)std::stoul(key, nullptr, 16);
        } catch (...) {}
    }

    // Single character 'A'-'Z' or '0'-'9'
    if (key.size() == 1) {
        char ch = key[0];
        if (ch >= 'A' && ch <= 'Z') return (UINT)ch;
        if (ch >= '0' && ch <= '9') return (UINT)ch;
    }

    // Decimal integer
    try {
        unsigned long val = std::stoul(key, nullptr, 10);
        if (val > 0 && val < 256) return (UINT)val;
    } catch (...) {}

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
    char exePath[MAX_PATH] = { 0 };
    GetModuleFileNameA(NULL, exePath, sizeof(exePath));
    char* lastSlash = strrchr(exePath, '\\');
    if (lastSlash) *(lastSlash + 1) = '\0';

    std::string baseDir = exePath;
    std::string subIni = baseDir + "Ini\\G3O.ini";
    std::string rootIni = baseDir + "G3O.ini";

    DWORD attr = GetFileAttributesA(subIni.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        return subIni;
    }

    return rootIni;
}

static void CreateDefaultIniFile(const std::string& path) {
    std::ofstream ofs(path);
    if (!ofs.is_open()) return;

    ofs << "; ==============================================================================\n"
        << "; Gothic 3 Optima (G3O) — Конфигурационный файл (Configuration File)\n"
        << "; ==============================================================================\n\n"
        << "[General]\n"
        << "; Главный переключатель G3O (true / false).\n"
        << "; Если false, G3O работает исключительно как мост dinput8.dll, отключая все хуки.\n"
        << "EnableG3O=true\n\n"
        << "; Отображать версию G3O в главном меню игры\n"
        << "ShowWatermark=true\n\n"
        << "; Загружать сторонние плагины из папки Data\\G3SE\\Plugins (если есть)\n"
        << "LoadG3SEPlugins=true\n\n"
        << "; Устранять конфликты со сторонними оверлеями (Bandicam, Steam, RTSS)\n"
        << "FixVulkanLayerConflicts=true\n\n\n"
        << "[Display]\n"
        << "; Включить модуль Borderless Window\n"
        << "EnableBorderless=true\n\n"
        << "; Горячая клавиша для переключения Borderless Fullscreen\n"
        << "; Поддерживаются: F1-F12, INSERT, DELETE, HOME, END, PGUP, PGDN, SCROLL, PAUSE, NONE, hex (0x79)\n"
        << "; По умолчанию: F10 (F11 освобождена)\n"
        << "ToggleBorderlessKey=F10\n\n"
        << "; Автоматически переводить окно в Borderless Fullscreen, если игра запущена в оконном режиме\n"
        << "AutoBorderless=true\n\n"
        << "; Предотвращать сворачивание и зависание Gothic 3 при Alt+Tab\n"
        << "PreventMinimizeOnFocusLoss=true\n\n\n"
        << "[Camera]\n"
        << "; Включить динамический контроллер угла обзора (FOV) для современных мониторов\n"
        << "EnableDynamicFOV=true\n\n"
        << "; Угол обзора по умолчанию в градусах (в оригинале Gothic 3: 67.5, в G3O: 85.0)\n"
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
        << "; Включить встроенный модуль расследования сбоев и поиска виновных DLL\n"
        << "EnableCrashInvestigator=true\n\n"
        << "; Показывать понятное информационное окно с диагнозом и советами при сбое\n"
        << "ShowCrashDialog=true\n\n"
        << "; Создавать аварийный дамп памяти (.dmp) для глубокого анализа\n"
        << "WriteCrashDump=true\n\n"
        << "; Записывать подробный отчёт о сбое в G3O_CrashReport.log\n"
        << "WriteCrashLog=true\n\n\n"
        << "[Performance]\n"
        << "; Аппаратный SIMD Frustum Culling (SSE2-векторизация отсечения невидимой геометрии)\n"
        << "; Значительно поднимает FPS при экстремальной дальности прорисовки\n"
        << "EnableSIMDFrustumCulling=true\n\n"
        << "; SIMD-ускоритель декомпрессии архивов .pak и сохранений (libdeflate)\n"
        << "EnableFastPakDecompressor=true\n\n"
        << "; O(1) хэш-кэш свойств сущностей для ускорения загрузки сохранений\n"
        << "EnablePropertyCache=true\n\n"
        << "; Фоновая предзагрузка .pak в кэш Windows при старте (ускоряет первую загрузку сохранения)\n"
        << "EnablePakPrefetch=true\n\n"
        << "; ЭКСПЕРИМЕНТАЛЬНО: пропуск активации секторов при загрузке из меню (нестабильно, вылеты в Engine.dll)\n"
        << "SkipRedundantPreLoadActivate=false\n\n"
        << "; Аппаратная фильтрация избыточных вызовов состояний Direct3D 9 (SetRenderState/SetSamplerState)\n"
        << "EnableD3D9StateFilter=true\n\n"
        << "; Fast Texture Loader (d3dx9_40.dll)\n"
        << "EnableFastTextureLoader=true\n\n"
        << "; Vegetation & SpeedTree Optimizer (clamp distant tree terrain shadows)\n"
        << "EnableVegetationOptimizer=true\n\n"
        << "; Tree terrain shadow clamp distance in units (6500.0 = 65 meters)\n"
        << "TreeShadowDistance=6500.0\n\n"
        << "; Высокоточный мультимедийный таймер Windows (квант 1 мс) для устранения микрофризов\n"
        << "EnableHighPrecisionTimer=true\n\n"
        << "; Назначить высокий приоритет процессу (HIGH_PRIORITY_CLASS) для защиты от фоновых служб\n"
        << "EnableHighProcessPriority=true\n\n"
        << "; Оптимизировать планировщик CPU под P-ядра процессора\n"
        << "EnableCPUTopologyAffinity=true\n\n\n"
        << "[Logging]\n"
        << "; Записывать диагностический журнал событий в G3O_diagnostic.log\n"
        << "EnableDiagnosticLog=true\n\n"
        << "; Перехватывать системный отладочный вывод игры (OutputDebugStringA)\n"
        << "InterceptOutputDebugString=true\n\n"
        << "; Профайлер: пишет G3O_profile.log (FPS, фризы, чтение файлов, нагрузка CPU) раз в секунду\n"
        << "EnableProfiler=false\n";
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

static std::string ReadIniString(const char* section, const char* key, const char* defaultVal, const char* path) {
    char buf[256] = { 0 };
    GetPrivateProfileStringA(section, key, defaultVal, buf, sizeof(buf), path);
    return Trim(buf);
}

static float ReadIniFloat(const char* section, const char* key, float defaultVal, const char* path) {
    char buf[64] = { 0 };
    char defBuf[32];
    snprintf(defBuf, sizeof(defBuf), "%.2f", defaultVal);
    GetPrivateProfileStringA(section, key, defBuf, buf, sizeof(buf), path);
    try {
        return std::stof(buf);
    } catch (...) {
        return defaultVal;
    }
}

static int ReadIniInt(const char* section, const char* key, int defaultVal, const char* path) {
    return (int)GetPrivateProfileIntA(section, key, defaultVal, path);
}

static DWORD_PTR ReadIniHex(const char* section, const char* key, DWORD_PTR defaultVal, const char* path) {
    char buf[64] = { 0 };
    GetPrivateProfileStringA(section, key, "0", buf, sizeof(buf), path);
    char* end = nullptr;
    unsigned long long val = strtoull(buf, &end, 0);
    return (val != 0) ? (DWORD_PTR)val : defaultVal;
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
    s_config.defaultFOV = ReadIniFloat("Camera", "DefaultFOV", 85.0f, path);
    s_config.enableFOVHotkeys = ReadIniBool("Camera", "EnableFOVHotkeys", true, path);
    std::string decKeyStr = ReadIniString("Camera", "DecreaseFOVKey", "[", path);
    s_config.decreaseFOVKey = ParseKeyCode(decKeyStr, VK_OEM_4);
    std::string incKeyStr = ReadIniString("Camera", "IncreaseFOVKey", "]", path);
    s_config.increaseFOVKey = ParseKeyCode(incKeyStr, VK_OEM_6);
    s_config.fovStep = ReadIniFloat("Camera", "FOVStep", 5.0f, path);

    // [CrashInvestigator]
    s_config.enableCrashInvestigator = ReadIniBool("CrashInvestigator", "EnableCrashInvestigator", true, path);
    s_config.showCrashDialog = ReadIniBool("CrashInvestigator", "ShowCrashDialog", true, path);
    s_config.writeCrashDump = ReadIniBool("CrashInvestigator", "WriteCrashDump", true, path);
    s_config.writeCrashLog = ReadIniBool("CrashInvestigator", "WriteCrashLog", true, path);

    // [Performance]
    s_config.enableSIMDFrustumCulling = ReadIniBool("Performance", "EnableSIMDFrustumCulling", true, path);
    s_config.enableFastPakDecompressor = ReadIniBool("Performance", "EnableFastPakDecompressor", true, path);
    s_config.enablePropertyCache = ReadIniBool("Performance", "EnablePropertyCache", true, path);
    s_config.enablePakPrefetch = ReadIniBool("Performance", "EnablePakPrefetch", true, path);
    s_config.skipRedundantPreLoadActivate = ReadIniBool("Performance", "SkipRedundantPreLoadActivate", false, path);
    s_config.enableD3D9StateFilter = ReadIniBool("Performance", "EnableD3D9StateFilter", false, path);
    s_config.enableFastTextureLoader = ReadIniBool("Performance", "EnableFastTextureLoader", true, path);
    s_config.enableVegetationOptimizer = ReadIniBool("Performance", "EnableVegetationOptimizer", true, path);
    s_config.treeShadowDistance = ReadIniFloat("Performance", "TreeShadowDistance", 6500.0f, path);
    s_config.vegetationGridRangeClamp = ReadIniFloat("Performance", "VegetationGridRangeClamp", 7500.0f, path);
    s_config.enableHighPrecisionTimer = ReadIniBool("Performance", "EnableHighPrecisionTimer", true, path);
    s_config.enableHighProcessPriority = ReadIniBool("Performance", "EnableHighProcessPriority", true, path);
    s_config.enableCPUTopologyAffinity = ReadIniBool("Performance", "EnableCPUTopologyAffinity", true, path);
    s_config.enableVFSCache = ReadIniBool("Performance", "EnableVFSCache", false, path);
    s_config.cpuCores = ReadIniInt("Performance", "CPUCores", 0, path);
    s_config.cpuThreads = ReadIniInt("Performance", "CPUThreads", 0, path);
    s_config.cpuAffinityMask = ReadIniHex("Performance", "CPUAffinityMask", 0, path);
    s_config.pakCacheBudgetMB = ReadIniInt("Performance", "PakCacheBudgetMB", 192, path);
    s_config.pakPrefetchWorkers = ReadIniInt("Performance", "PakPrefetchWorkers", 0, path);

    // [Logging]
    s_config.enableDiagnosticLog = ReadIniBool("Logging", "EnableDiagnosticLog", true, path);
    s_config.interceptOutputDebugString = ReadIniBool("Logging", "InterceptOutputDebugString", true, path);
    s_config.enableProfiler = ReadIniBool("Logging", "EnableProfiler", false, path);

    Log("G3O", "Configuration loaded from '%s':", path);
    Log("G3O", "  [General] EnableG3O=%d, ShowWatermark=%d", (int)s_config.enableG3O, (int)s_config.showWatermark);
    Log("G3O", "  [Display] EnableBorderless=%d, ToggleKey=%s, AutoBorderless=%d, PreventMinimize=%d",
        (int)s_config.enableBorderless, KeyCodeToString(s_config.toggleBorderlessKey).c_str(),
        (int)s_config.autoBorderless, (int)s_config.preventMinimizeOnFocusLoss);
    Log("G3O", "  [Camera]  EnableFOV=%d, DefaultFOV=%.1f, Hotkeys=%d (Dec: %s, Inc: %s, Step: %.1f)",
        (int)s_config.enableDynamicFOV, s_config.defaultFOV, (int)s_config.enableFOVHotkeys,
        KeyCodeToString(s_config.decreaseFOVKey).c_str(), KeyCodeToString(s_config.increaseFOVKey).c_str(), s_config.fovStep);
    Log("G3O", "  [Crash]   Enable=%d, Dialog=%d, Dump=%d, Log=%d",
        (int)s_config.enableCrashInvestigator, (int)s_config.showCrashDialog, (int)s_config.writeCrashDump, (int)s_config.writeCrashLog);
    Log("G3O", "  [Perform] SIMDFrustum=%d, FastPak=%d, PropCache=%d, D3D9Filter=%d, FastTexLoader=%d, VegOpt=%d (Shadow: %.1f, VegClamp: %.1f), 1msTimer=%d, HighPriority=%d, CPUTopology=%d",
        (int)s_config.enableSIMDFrustumCulling, (int)s_config.enableFastPakDecompressor, (int)s_config.enablePropertyCache, (int)s_config.enableD3D9StateFilter, (int)s_config.enableFastTextureLoader,
        (int)s_config.enableVegetationOptimizer, s_config.treeShadowDistance, s_config.vegetationGridRangeClamp, (int)s_config.enableHighPrecisionTimer,
        (int)s_config.enableHighProcessPriority, (int)s_config.enableCPUTopologyAffinity);
}

} // namespace G3O
