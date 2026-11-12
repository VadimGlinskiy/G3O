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
        << "; Файл конфигурации. Располагается рядом с Gothic3.exe.\n"
        << "; Логи, отчеты о сбоях и дампы памяти сохраняются в папку Gothic 3\\G3O\\.\n"
        << "; ==============================================================================\n"
        << "\n"
        << "[General]\n"
        << "; Главный переключатель G3O (true / false).\n"
        << "; При false системный DirectInput-прокси загружается, а инициализация G3O прекращается.\n"
        << "EnableG3O=true\n"
        << "\n"
        << "; Загрузка плагинов G3SE (true / false).\n"
        << "; G3O ищет G3SDK.dll или G3SE.dll и вызывает их функцию LoadPlugins для Data\\G3SE\\Plugins, если находит её.\n"
        << "LoadG3SEPlugins=true\n"
        << "\n"
        << "; Меняет настройку одного Vulkan-слоя в окружении процесса.\n"
        << "; Устанавливает DISABLE_VK_LAYER_bandicam_helper_1=1; другие слои эта настройка не меняет.\n"
        << "FixVulkanLayerConflicts=true\n"
        << "\n"
        << "[Display]\n"
        << "; Окно без рамок (true / false).\n"
        << "; Убирает рамку окна игры и растягивает его до границ монитора.\n"
        << "EnableBorderless=true\n"
        << "\n"
        << "; Клавиша переключения режима окна; NONE отключает эту клавишу.\n"
        << "ToggleBorderlessKey=F10\n"
        << "\n"
        << "; Включает окно без рамок при запуске игры.\n"
        << "AutoBorderless=true\n"
        << "\n"
        << "; В режиме без рамок подавляет обработку игрой сообщений о потере фокуса и сворачивании.\n"
        << "PreventMinimizeOnFocusLoss=true\n"
        << "\n"
        << "[Camera]\n"
        << "; Подменяет значения FOV от 50 до 75 градусов на DefaultFOV (true / false).\n"
        << "; FOV не пересчитывается по соотношению сторон экрана.\n"
        << "EnableDynamicFOV=true\n"
        << "\n"
        << "; FOV, подставляемый вместо игровых значений 50–75 градусов.\n"
        << "; Код принимает значения от 50 до 115 градусов; это технический диапазон, не рекомендация.\n"
        << "DefaultFOV=85.0\n"
        << "\n"
        << "; Разрешает менять FOV горячими клавишами. Для настроенной клавиши нужно удерживать Ctrl; NumPad -/+ работают отдельно.\n"
        << "EnableFOVHotkeys=true\n"
        << "\n"
        << "; Клавиша уменьшения FOV при зажатом Ctrl (дублируется на NumPad -).\n"
        << "DecreaseFOVKey=[\n"
        << "\n"
        << "; Клавиша увеличения FOV при зажатом Ctrl (дублируется на NumPad +).\n"
        << "IncreaseFOVKey=]\n"
        << "\n"
        << "; Шаг изменения FOV в градусах за одно нажатие горячей клавиши.\n"
        << "FOVStep=5.0\n"
        << "\n"
        << "[CrashInvestigator]\n"
        << "; Включает обработчики G3O для необработанных исключений Windows и выбранных ошибок движка.\n"
        << "EnableCrashInvestigator=true\n"
        << "\n"
        << "; Диалог G3O предназначен для необработанного Win32-исключения; при FatalError движка отдельное окно не показывается.\n"
        << "ShowCrashDialog=true\n"
        << "\n"
        << "; Записывает минидамп для необработанного Win32-исключения и FatalError движка; для FatalError фиксируется момент обработки, а не исходный сбой.\n"
        << "WriteCrashDump=true\n"
        << "\n"
        << "; Записывает текстовый отчёт G3O для сбоя, который удалось обработать.\n"
        << "WriteCrashLog=true\n"
        << "\n"
        << "[Performance]\n"
        << "; Подменяет проверку пересечения габаритных коробок объектов, два сравнения строк и проверку _IsNT в SharedBase.dll.\n"
        << "; Коробки сравниваются по всем трём координатам; одно сравнение строк учитывает регистр, другое — нет; _IsNT всегда возвращает TRUE.\n"
        << "EnableSharedBaseOptimizations=true\n"
        << "\n"
        << "; Использует отдельный путь поиска компонентов только когда список помечен отсортированным; в остальных случаях вызывает поиск движка.\n"
        << "EnableEntitySearchOptimizer=true\n"
        << "\n"
        << "; Распаковывает данные PAK и сохранений через libdeflate (AVX2 — если его поддерживает процессор).\n"
        << "; Если zlib-распаковка не удалась, пробует DEFLATE без проверки контрольной суммы zlib; при неудаче вызывает декомпрессор игры.\n"
        << "EnableFastPakDecompressor=true\n"
        << "\n"
        << "; Для RB_Mod заранее распаковывает в фоне точные .node-блоки из Data\\Projects_compiled.m17.\n"
        << "; Результат используется только при полном совпадении сжатого блока и готовности буфера; иначе выполняется обычная распаковка. Требуется активный хук EnableFastPakDecompressor.\n"
        << "EnableLayerRingArena=false\n"
        << "\n"
        << "; Кэширует результаты ResolveProperty по имени свойства или ID; при промахе вызывается исходная функция.\n"
        << "EnablePropertyCache=true\n"
        << "\n"
        << "; Читает архивы из папки Data в фоновом потоке с низким приоритетом и тем самым прогревает файловый кэш Windows.\n"
        << "; Если есть трасса предыдущей загрузки этого сохранения и включён EnableFastPakDecompressor, заранее распаковывает отмеченные PAK-блоки; иначе выполняется только чтение архивов.\n"
        << "EnablePakPrefetch=true\n"
        << "\n"
        << "; Максимальный объём памяти для кэша распакованных блоков PAK, в МБ (0–1024; 0 отключает этот кэш).\n"
        << "PakCacheBudgetMB=768\n"
        << "\n"
        << "; Число фоновых потоков для распаковки блоков по трассе (0 = автоматически; 1–8 = запрос, который код может ограничить).\n"
        << "PakPrefetchWorkers=0\n"
        << "\n"
        << "; Откладывает выбранные обновления базы видимости eCFrustumDatabase и выполняет их пакетами во время загрузки.\n"
        << "EnableFrustumDatabaseOptimizer=true\n"
        << "\n"
        << "; Использует быструю хэш-функцию в выбранных путях и кэширует поиск шаблонов eCTemplateEntityProxy.\n"
        << "EnableHashProxyOptimizer=true\n"
        << "\n"
        << "; Пропускает вызовы gCNavigationAdmin::Process, пока активна загрузка сохранения.\n"
        << "EnableNavigationOptimizer=true\n"
        << "\n"
        << "; Когда массив дочерних узлов заполнен и содержит не менее 64 элементов, заранее увеличивает вместимость до 1024, 8192 или 32768.\n"
        << "EnableSceneNodeOptimizer=true\n"
        << "\n"
        << "; Убирает дубликаты из списка теневых кастеров направленного света и удаляет записи без сдвига остальных.\n"
        << "EnableShadowOptimizer=true\n"
        << "\n"
        << "; Кэширует успешный поиск имени класса в карте SharedBase. Хук ставится только при совпадении проверяемых байтов по адресу в SharedBase.dll.\n"
        << "EnableClassFactoryOptimizer=true\n"
        << "\n"
        << "; В GotoLoadedPosition ограничивает радиус PVS-сферы и пропускает близкие соседние повторные запросы.\n"
        << "; Затем радиус возвращается к исходному за PVSRampUpCalls вызовов. Настройка не меняет данные коллизий.\n"
        << "EnablePVSPrefetchOptimizer=false\n"
        << "\n"
        << "; Вместе с EnablePVSPrefetchOptimizer временно меняет Entity.ROI, если известна исходная цель и EnableDynamicROI=false.\n"
        << "; Меняет только Entity.ROI; геометрию и данные коллизий этот параметр не меняет.\n"
        << "EnableAdaptiveROI=true\n"
        << "\n"
        << "; Временное значение Entity.ROI (100–50000, в тех же единицах, что в ge3.ini); применяется только при известном исходном значении.\n"
        << "ROIBootRadius=3600\n"
        << "\n"
        << "; Максимальный радиус входящей PVS-сферы в GotoLoadedPosition (500–50000); меньшие сферы не меняются.\n"
        << "PVSBootRadius=3800\n"
        << "\n"
        << "; Пропускает соседний синхронный PVS-запрос, только если изменения центра и радиуса не превышают это значение (10–5000, единицы игры).\n"
        << "PVSRedundantDistance=200\n"
        << "\n"
        << "; Число последующих PVS-вызовов для возврата радиуса сферы к запрошенному значению (0–1000; 0 отключает переход).\n"
        << "PVSRampUpCalls=24\n"
        << "\n"

        << "\n"
        << "; Ищет диалоги через дополнительную хэш-таблицу; при промахе обращается к поиску игры.\n"
        << "; При загрузке расширяет исходную таблицу диалогов и пишет в журнал время чтения её данных.\n"
        << "EnableInfoManagerOptimizer=true\n"
        << "\n"
        << "; Меняет длительность вызова Sleep в gCSession::Start; патч ставится только при совпадении байтов и адреса Sleep.\n"
        << "EnableSessionStartOptimizer=true\n"
        << "\n"
        << "; Длительность этой паузы в миллисекундах (0–100).\n"
        << "SessionStartSleepMs=1\n"
        << "\n"
        << "; Меняет Entity.ROI по состоянию прицела: AimingROI при видимом прицеле, BaseROI в остальных случаях.\n"
        << "; Переход занимает 15 шагов вверх и 30 шагов вниз; это общий радиус симуляции, не дистанция до цели.\n"
        << "EnableDynamicROI=false\n"
        << "\n"
        << "; Entity.ROI при невидимом прицеле (0–50000, в тех же единицах, что в ge3.ini).\n"
        << "; 0 сохраняет текущее значение Entity.ROI из настроек игры.\n"
        << "BaseROI=0\n"
        << "\n"
        << "; Entity.ROI при видимом прицеле (0–50000, в тех же единицах, что в ge3.ini).\n"
        << "; 0 использует BaseROI.\n"
        << "AimingROI=7500\n"
        << "\n"
        << "; Если игра просит стандартную или треугольную фильтрацию, для DDS в памяти с готовыми mip-уровнями отключает её, а в остальных случаях заменяет на BOX.\n"
        << "EnableFastTextureLoader=true\n"
        << "\n"
        << "; Ограничивает дальность регистрации теней деревьев и, отдельно, радиус сетки растительности.\n"
        << "EnableVegetationOptimizer=true\n"
        << "\n"
        << "; Выбирает порог регистрации тени дерева по высоте его ограничивающего ящика; иначе используется один порог.\n"
        << "EnableAdaptiveTreeShadows=true\n"
        << "\n"
        << "; Базовый порог дальности для теней деревьев, в единицах игры.\n"
        << "TreeShadowDistance=6500.0\n"
        << "\n"
        << "; Ограничение радиуса сетки растительности, в единицах игры (0 = не ограничивать).\n"
        << "VegetationGridRangeClamp=7500.0\n"
        << "\n"
        << "; Относит NPC, деревья, предметы, интерактивные объекты, свет и эффекты к группе Important; остальные сущности — к Clutter.\n"
        << "; Перед штатным отсечением пересчитывает коэффициент площади по порогу выбранной группы.\n"
        << "EnableSelectiveCulling=true\n"
        << "\n"
        << "; Порог для Clutter — сущностей без перечисленных выше свойств (0.0001–0.1; это не размер в пикселях).\n"
        << "ClutterCullThreshold=0.008\n"
        << "\n"
        << "; Порог для группы Important (0.0001–0.1; это не размер в пикселях).\n"
        << "ImportantCullThreshold=0.002\n"
        << "\n"
        << "; Меняет значения начала и конца тумана, которые игра передаёт устройству Direct3D 9.\n"
        << "EnableAtmosphericFogControl=true\n"
        << "\n"
        << "; Множитель дальности тумана, который передаёт игра.\n"
        << "FogDistanceMultiplier=2.5\n"
        << "\n"
        << "; Нижняя граница FogEnd после обработки; единицы совпадают со значением, которое передаёт игра.\n"
        << "FogEndMinDistance=250000.0\n"
        << "\n"
        << "; Оставляет туман без изменений, если код распознаёт его как туман ближней зоны.\n"
        << "PreserveUnderwaterFog=true\n"
        << "\n"
        << "; Запрашивает у Windows период таймера 1 мс; при успешном запросе возвращает прежний период при завершении G3O.\n"
        << "EnableHighPrecisionTimer=true\n"
        << "\n"
        << "; Просит Windows установить процессу Gothic3.exe приоритет HIGH_PRIORITY_CLASS.\n"
        << "EnableHighProcessPriority=true\n"
        << "\n"
        << "; На гибридном CPU выбирает ядра с наивысшим классом производительности Windows (EfficiencyClass); маска действует на весь процесс, включая потоки PUP.\n"
        << "; Приоритет ручных настроек: CPUAffinityMask, затем CPUThreads, затем CPUCores; если они равны 0, выбор автоматический.\n"
        << "EnableCPUTopologyAffinity=true\n"
        << "\n"
        << "; Кэширует успешные поиски в каталоге VFS. Промахи и пути длиной 96 символов и больше идут в исходную функцию.\n"
        << "EnableVFSCache=true\n"
        << "\n"
        << "; Число физических ядер: выбирает по одному логическому процессору с каждого ядра (0 = автоматически; CPUThreads и CPUAffinityMask имеют приоритет).\n"
        << "CPUCores=0\n"
        << "\n"
        << "; Число логических процессоров для привязки (0 = автоматически; действует, если CPUAffinityMask равна 0).\n"
        << "CPUThreads=0\n"
        << "\n"
        << "; Ручная маска процессорной привязки в шестнадцатеричном виде (0 = использовать CPUThreads/CPUCores и автонастройку).\n"
        << "CPUAffinityMask=0x0\n"
        << "\n"
        << "; Пропускает повторные SetRenderState, SetTextureStageState и SetSamplerState с теми же значениями.\n"
        << "; Кэш обходится при записи/применении state block и сбрасывается после Reset устройства.\n"
        << "EnableD3D9StateFilter=true\n"
        << "\n"
        << "[Logging]\n"
        << "; Записывает сообщения G3O и прочитанные параметры конфигурации в G3O\\G3O_diagnostic_*.log.\n"
        << "EnableDiagnosticLog=false\n"
        << "\n"
        << "; Копирует вызовы OutputDebugStringA и выбранный вывод движка в журнал G3O; исходный OutputDebugStringA тоже вызывается.\n"
        << "InterceptOutputDebugString=false\n"
        << "\n"
        << "; Раз в секунду записывает FPS, 1% low, p99/max времени кадра, CPU, чтение файлов и вызовы отсечения.\n"
        << "; Раз в 15 секунд пишет выборку адресов выполнения занятого потока; это не стек вызовов.\n"
        << "EnableProfiler=false\n"
        << "\n"
        << "; Записывает время выбранных этапов загрузки мира, секторов и сохранения в G3O\\G3O_LoadProfiler_*.log.\n"
        << "EnableSavegameAudit=false\n"
        << "\n"
        << "; Вместе с EnableSavegameAudit измеряет создание и загрузку слоёв, а также чтение свойств; содержимое данных не меняет.\n"
        << "EnableLayerAudit=false\n"
        << "\n";
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
    s_config.enableEntitySearchOptimizer = ReadIniBool("Performance", "EnableEntitySearchOptimizer", true, path);
    s_config.enableFastPakDecompressor = ReadIniBool("Performance", "EnableFastPakDecompressor", true, path);
    s_config.enableLayerRingArena = ReadIniBool("Performance", "EnableLayerRingArena", false, path);
    s_config.enablePropertyCache = ReadIniBool("Performance", "EnablePropertyCache", true, path);
    s_config.enablePakPrefetch = ReadIniBool("Performance", "EnablePakPrefetch", true, path);
    s_config.enableFrustumDatabaseOptimizer = ReadIniBool("Performance", "EnableFrustumDatabaseOptimizer", true, path);
    s_config.enableHashProxyOptimizer = ReadIniBool("Performance", "EnableHashProxyOptimizer", true, path);
    s_config.enableNavigationOptimizer = ReadIniBool("Performance", "EnableNavigationOptimizer", true, path);
    s_config.enableSceneNodeOptimizer = ReadIniBool("Performance", "EnableSceneNodeOptimizer", true, path);
    s_config.enableShadowOptimizer = ReadIniBool("Performance", "EnableShadowOptimizer", true, path);
    s_config.enableClassFactoryOptimizer = ReadIniBool("Performance", "EnableClassFactoryOptimizer", true, path);
    s_config.enablePVSPrefetchOptimizer = ReadIniBool("Performance", "EnablePVSPrefetchOptimizer", false, path);
    s_config.enableAdaptiveROI = ReadIniBool("Performance", "EnableAdaptiveROI", true, path);
    s_config.roiBootRadius = ReadIniFloat("Performance", "ROIBootRadius", 3600.0f, 100.0f, 50000.0f, path);
    s_config.pvsBootRadius = ReadIniFloat("Performance", "PVSBootRadius", 3800.0f, 500.0f, 50000.0f, path);
    s_config.pvsRedundantDistance = ReadIniFloat("Performance", "PVSRedundantDistance", 200.0f, 10.0f, 5000.0f, path);
    s_config.pvsRampUpCalls = static_cast<uint32_t>(ReadIniInt("Performance", "PVSRampUpCalls", 24, 0, 1000, path));

    s_config.enableInfoManagerOptimizer = ReadIniBool("Performance", "EnableInfoManagerOptimizer", true, path);
    s_config.enableSessionStartOptimizer = ReadIniBool("Performance", "EnableSessionStartOptimizer", true, path);
    s_config.sessionStartSleepMs = static_cast<uint32_t>(ReadIniInt("Performance", "SessionStartSleepMs", 1, 0, 100, path));
    s_config.enableDynamicROI = ReadIniBool("Performance", "EnableDynamicROI", false, path);
    s_config.baseROI = ReadIniFloat("Performance", "BaseROI", 0.0f, 0.0f, 50000.0f, path);
    s_config.aimingROI = ReadIniFloat("Performance", "AimingROI", 7500.0f, 0.0f, 50000.0f, path);
    s_config.enableD3D9StateFilter = ReadIniBool("Performance", "EnableD3D9StateFilter", true, path);
    s_config.enableFastTextureLoader = ReadIniBool("Performance", "EnableFastTextureLoader", true, path);
    s_config.enableVegetationOptimizer = ReadIniBool("Performance", "EnableVegetationOptimizer", true, path);
    s_config.enableAdaptiveTreeShadows = ReadIniBool("Performance", "EnableAdaptiveTreeShadows", true, path);
    s_config.treeShadowDistance = ReadIniFloat("Performance", "TreeShadowDistance", 6500.0f, 0.0f, 100000.0f, path);
    s_config.vegetationGridRangeClamp = ReadIniFloat("Performance", "VegetationGridRangeClamp", 7500.0f, 0.0f, 100000.0f, path);
    s_config.enableSelectiveCulling = ReadIniBool("Performance", "EnableSelectiveCulling", true, path);
    s_config.clutterCullThreshold = ReadIniFloat("Performance", "ClutterCullThreshold", 0.008f, 0.0001f, 0.1f, path);
    s_config.importantCullThreshold = ReadIniFloat("Performance", "ImportantCullThreshold", 0.002f, 0.0001f, 0.1f, path);
    s_config.enableAtmosphericFogControl = ReadIniBool("Performance", "EnableAtmosphericFogControl", true, path);
    s_config.fogDistanceMultiplier = ReadIniFloat("Performance", "FogDistanceMultiplier", 2.5f, 0.1f, 10.0f, path);
    s_config.fogEndMinDistance = ReadIniFloat("Performance", "FogEndMinDistance", 250000.0f, 0.0f, 1000000.0f, path);
    s_config.preserveUnderwaterFog = ReadIniBool("Performance", "PreserveUnderwaterFog", true, path);
    s_config.enableHighPrecisionTimer = ReadIniBool("Performance", "EnableHighPrecisionTimer", true, path);
    s_config.enableHighProcessPriority = ReadIniBool("Performance", "EnableHighProcessPriority", true, path);
    s_config.enableCPUTopologyAffinity = ReadIniBool("Performance", "EnableCPUTopologyAffinity", true, path);
    s_config.enableVFSCache = ReadIniBool("Performance", "EnableVFSCache", true, path);
    s_config.cpuCores = ReadIniInt("Performance", "CPUCores", 0, 0, 32, path);
    s_config.cpuThreads = ReadIniInt("Performance", "CPUThreads", 0, 0, 32, path);
    s_config.cpuAffinityMask = ReadIniHex("Performance", "CPUAffinityMask", 0, path);
    s_config.pakCacheBudgetMB = ReadIniInt("Performance", "PakCacheBudgetMB", 768, 0, 1024, path);
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
