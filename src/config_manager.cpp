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

    ofs        << "; ==============================================================================\n"
        << "; Gothic 3 Optima (G3O)\n"
        << "; Конфигурационный файл. Располагается в папке с игрой рядом с Gothic3.exe.\n"
        << "; Значения параметров: true — включено, false — выключено.\n"
        << "; Логи, отчеты об ошибках и дампы памяти сохраняются в папку Gothic 3\\G3O\\.\n"
        << "; ==============================================================================\n"
        << "\n"
        << "[General]\n"
        << "; Главный выключатель G3O.\n"
        << "; При false все хуки и оптимизации отключаются, игра работает как оригинал.\n"
        << "EnableG3O=true\n"
        << "\n"
        << "; Загрузка скриптовых плагинов из папки Data\\G3SE\\Plugins через установленный G3SE/G3SDK.\n"
        << "; Если вы используете моды с кастомными скриптами — оставьте true.\n"
        << "LoadG3SEPlugins=true\n"
        << "\n"
        << "; Автоматическая загрузка графического пакета ReShade (ReShade32.dll), если файл находится в папке с игрой.\n"
        << "; Позволяет использовать шейдеры постобработки без переименования системных библиотек.\n"
        << "LoadReShade=true\n"
        << "\n"
        << "; Исправление конфликтов с оверлеями (например, Bandicam).\n"
        << "; Отключает проблемный Vulkan-слой, из-за которого игра может падать при запуске с DXVK.\n"
        << "FixVulkanLayerConflicts=true\n"
        << "\n"
        << "\n"
        << "[Display]\n"
        << "; Режим окна без рамки (Borderless Fullscreen).\n"
        << "; Позволяет комфортно играть на весь экран с быстрым сворачиванием через Alt+Tab без вылетов и черных экранов.\n"
        << "EnableBorderless=true\n"
        << "\n"
        << "; Автоматически переводить игру в безрамочный полноэкранный режим при запуске.\n"
        << "AutoBorderless=true\n"
        << "\n"
        << "; В безрамочном режиме не дает игре сворачиваться при потере фокуса (например, при клике на второй монитор).\n"
        << "PreventMinimizeOnFocusLoss=true\n"
        << "\n"
        << "; Блокировка курсора мыши внутри окна игры (true / false).\n"
        << "; В полноэкранном окне не позволяет курсору случайно вылетать за границы монитора на второй экран.\n"
        << "; При сворачивании через Alt+Tab или потере фокуса курсор мгновенно освобождается.\n"
        << "WindowCursorLock=true\n"
        << "\n"
        << "; Проброс мультимедийных клавиш клавиатуры (true / false).\n"
        << "; Разрешает работу кнопок регулировки громкости, отключения звука и переключения треков (Play/Pause) прямо во время игры.\n"
        << "InputSystemKeyPassthrough=true\n"
        << "\n"
        << "\n"
        << "[CrashInvestigator]\n"
        << "; Система диагностики и перехвата вылетов.\n"
        << "; Отслеживает критические сбои и сохраняет подробную информацию о причине в папку G3O\\.\n"
        << "EnableCrashInvestigator=true\n"
        << "\n"
        << "; Защита от сбоев при поврежденных объектах (Crash Guards).\n"
        << "; Перехватывает вызовы функций движка с нулевыми указателями (DrawFocusName без актера, кости GetBoneMatrix в регдоллах, FindStack с null-шаблоном)\n"
        << "; и безопасно пропускает их вместо вылета с ошибкой «Where is the guru?» или Access Violation.\n"
        << "EnableCrashGuards=true\n"
        << "\n"
        << "; Показ информативного окна при вылете игры.\n"
        << "; Показывает понятный диалог в случае необработанного Win32-исключения с указанием модуля и адреса сбоя.\n"
        << "ShowCrashDialog=true\n"
        << "\n"
        << "; Создание минидампа памяти (.dmp) при падении.\n"
        << "; Записывает состояние памяти для отладки вылета или внутренней ошибки FatalError движка.\n"
        << "WriteCrashDump=true\n"
        << "\n"
        << "; Сохранять подробный текстовый отчет об ошибке в папку G3O\\, если сбой удалось перехватить.\n"
        << "WriteCrashLog=true\n"
        << "\n"
        << "\n"
        << "[Performance]\n"
        << "; Ускорение базовых математических операций и работы со строками движка (SharedBase.dll).\n"
        << "; Использует быстрые инструкции процессора для проверки границ объектов и сравнения имен в памяти.\n"
        << "; Заметно снижает нагрузку на CPU в сценах с большим количеством объектов и ускоряет загрузку мира.\n"
        << "EnableSharedBaseOptimizations=true\n"
        << "\n"
        << "; Оптимизация поиска компонентов внутри игровых объектов.\n"
        << "; Когда список компонентов отсортирован, находит нужные данные мгновенно без перебора всего списка.\n"
        << "EnableEntitySearchOptimizer=true\n"
        << "\n"
        << "; Быстрая распаковка архивов PAK и сохранений через современную библиотеку libdeflate (AVX2).\n"
        << "; Значительно ускоряет загрузку игры. При сбое пробует аварийную распаковку без проверки контрольной суммы zlib, а затем штатный метод.\n"
        << "EnableFastPakDecompressor=true\n"
        << "\n"
        << "; Кольцевой буфер предзагрузки слоев мира для тяжелых модов (RB_Mod).\n"
        << "; Данные берутся только при полном совпадении сжатого блока с файлом архива. Требует EnableFastPakDecompressor=true.\n"
        << "; Для обычной игры держите выключенным (EnableLayerRingArena=false).\n"
        << "EnableLayerRingArena=false\n"
        << "\n"
        << "; Кэширование свойств объектов при загрузке.\n"
        << "; Запоминает однажды найденные свойства в быстрой хэш-таблице, избавляя от повторных поисков и ускоряя загрузку сохранений.\n"
        << "EnablePropertyCache=true\n"
        << "\n"
        << "; Фоновое чтение и предзагрузка архивов.\n"
        << "; В фоне прогревает файловый кэш Windows перед стартом мира и заранее распаковывает отмеченные PAK-блоки по данным прошлой загрузки.\n"
        << "; Устраняет зависания и микрофризы при входе в игру.\n"
        << "EnablePakPrefetch=true\n"
        << "\n"
        << "; Максимальный объем оперативной памяти под кэш распакованных архивов (в мегабайтах).\n"
        << "; 384 МБ — оптимальный безопасный лимит для 32-битного движка Готики 3 (предотвращает вылеты Out of Memory). 0 выключает кэш.\n"
        << "PakCacheBudgetMB=384\n"
        << "\n"
        << "; Количество фоновых потоков для распаковки ресурсов по трассе сохранения.\n"
        << "; 0 — определить автоматически по числу физических ядер процессора.\n"
        << "PakPrefetchWorkers=0\n"
        << "\n"
        << "; Оптимизация базы видимости мира при загрузке.\n"
        << "; Откладывает частые мелкие перерасчеты и применяет их пачкой, сокращая время загрузочного экрана.\n"
        << "EnableFrustumDatabaseOptimizer=true\n"
        << "\n"
        << "; Быстрый поиск шаблонов игровых сущностей по имени.\n"
        << "; Кэширует шаблоны в памяти, избавляя движок от долгого перебора списков при спавне NPC, монстров и предметов.\n"
        << "EnableHashProxyOptimizer=true\n"
        << "\n"
        << "; Оптимизация навигации.\n"
        << "; Устраняет холостые накладные расходы при обработке путей перемещения NPC во время игры и загрузки.\n"
        << "EnableNavigationOptimizer=true\n"
        << "\n"
        << "; Оптимизация работы с большими секторами мира.\n"
        << "; Когда массив дочерних узлов сцены заполнен и содержит не менее 64 элементов, заранее увеличивает вместимость до 1024, 8192 или 32768.\n"
        << "; Предотвращает миллионы повторных выделений памяти при загрузке мира.\n"
        << "EnableSceneNodeOptimizer=true\n"
        << "\n"
        << "; Оптимизация солнечных теней.\n"
        << "; Быстро удаляет дубликаты из списка источников тени без лишней перетасовки памяти, повышая FPS в лесистой местности.\n"
        << "EnableShadowOptimizer=true\n"
        << "\n"
        << "; Кэширование фабрики классов.\n"
        << "; Запоминает соответствие имен классов внутренним типам движка, ускоряя распознавание объектов при загрузке.\n"
        << "EnableClassFactoryOptimizer=true\n"
        << "\n"
        << "; Оптимизация поиска диалогов NPC.\n"
        << "; Мгновенно находит нужные реплики и условия через хэш-таблицу. Устраняет заикания при приближении к NPC и открытии диалогов.\n"
        << "EnableInfoManagerOptimizer=true\n"
        << "\n"
        << "; Устранение холостых пауз при старте сессии.\n"
        << "; В оригинале движок делает 12 пауз Sleep по 100 мс (1.2 сек). Опция сокращает их до минимума.\n"
        << "EnableSessionStartOptimizer=true\n"
        << "\n"
        << "; Длина паузы при старте сессии в миллисекундах (1 мс вместо 100 мс).\n"
        << "SessionStartSleepMs=1\n"
        << "\n"
        << "; Быстрая загрузка текстур.\n"
        << "; Пропускает повторную фильтрацию для DDS-текстур с уже готовыми mip-уровнями, ускоряя подгрузку мира на лету.\n"
        << "EnableFastTextureLoader=true\n"
        << "\n"
        << "; Оптимизация дальности прорисовки травы и теней от деревьев.\n"
        << "; Отсекает расчет теней от далеких невидимых крон, существенно повышая частоту кадров (FPS) на открытой местности.\n"
        << "EnableVegetationOptimizer=true\n"
        << "\n"
        << "; Адаптивная дальность теней в зависимости от высоты дерева.\n"
        << "; Тени кустов отсекаются ближе, а тени гигантских деревьев видны дальше.\n"
        << "EnableAdaptiveTreeShadows=true\n"
        << "\n"
        << "; Базовая дистанция отображения теней деревьев (в игровых единицах).\n"
        << "TreeShadowDistance=6500.0\n"
        << "\n"
        << "; Ограничение радиуса сетки травы вокруг игрока (в игровых единицах, 0 = без ограничений).\n"
        << "VegetationGridRangeClamp=7500.0\n"
        << "\n"
        << "; Умное отсечение мелких объектов (Selective Culling).\n"
        << "; Не рисует микроскопические камни, посуду и мелкий мусор вдалеке, но всегда сохраняет видимость врагов, NPC и сундуков. Повышает FPS.\n"
        << "EnableSelectiveCulling=true\n"
        << "\n"
        << "; Порог отсечения для мелкого мусора и декораций (чем число больше, тем раньше они перестают нагружать видеокарту).\n"
        << "ClutterCullThreshold=0.008\n"
        << "\n"
        << "; Порог видимости для важных объектов, NPC, врагов и лута (чем меньше, тем дальше они видны).\n"
        << "ImportantCullThreshold=0.002\n"
        << "\n"
        << "; Управление атмосферным туманом.\n"
        << "; Позволяет отодвинуть горизонт тумана и открыть красивые виды на горы и море.\n"
        << "EnableAtmosphericFogControl=true\n"
        << "\n"
        << "; Множитель дальности обычного погодного тумана (2.5 отодвигает туман в 2.5 раза дальше, открывая живописные панорамы).\n"
        << "FogDistanceMultiplier=2.5\n"
        << "\n"
        << "; Минимальная дистанция окончания тумана в ясную погоду.\n"
        << "FogEndMinDistance=250000.0\n"
        << "\n"
        << "; Сохранять оригинальный близкий туман под водой, в пещерах и ядовитом газе, чтобы не нарушать атмосферу.\n"
        << "PreserveUnderwaterFog=true\n"
        << "\n"
        << "; Запрос высокоточного системного таймера Windows (1 мс) для более плавного отсчета времени кадров.\n"
        << "EnableHighPrecisionTimer=true\n"
        << "\n"
        << "; Высокий приоритет процесса Gothic3.exe в Windows. Защищает игру от подтормаживаний из-за фоновых программ.\n"
        << "EnableHighProcessPriority=true\n"
        << "\n"
        << "; Привязка игры к производительным ядрам процессора.\n"
        << "; На гибридных процессорах (Intel 12+ поколений) игра запускается на ядрах с наивысшим классом производительности Windows.\n"
        << "; Выбирает по одному логическому процессору с каждого ядра, избегая медленных энергоэффективных E-ядер.\n"
        << "EnableCPUTopologyAffinity=true\n"
        << "\n"
        << "; Кэширование путей к файлам в виртуальной файловой системе (VFS).\n"
        << "; Мгновенно находит нужные архивы и файлы модов без повторного сканирования диска.\n"
        << "EnableVFSCache=true\n"
        << "\n"
        << "; Количество используемых физических ядер процессора (0 = автоматически).\n"
        << "CPUCores=0\n"
        << "\n"
        << "; Количество логических потоков процессора (0 = автоматически).\n"
        << "CPUThreads=0\n"
        << "\n"
        << "; Ручная шестнадцатеричная маска привязки ядер процессора (0x0 = автоматически).\n"
        << "CPUAffinityMask=0x0\n"
        << "\n"
        << "; Фильтр повторяющихся команд DirectX 9.\n"
        << "; Отсекает отправку видеокарте идентичных состояний рендеринга, экономя процессорное время.\n"
        << "EnableD3D9StateFilter=true\n"
        << "\n"
        << "\n"
        << "[Memory]\n"
        << "; Режим высокопроизводительного менеджера кучи mimalloc (0, 1, 2).\n"
        << "; 0 = Стандартный аллокатор движка (максимальная совместимость).\n"
        << "; 1 = Замена ShortUsage / ScrapHeap (ускоряет покадровые временные буферы без риска межмодульных конфликтов).\n"
        << "; 2 = Полная замена MemHeap и bCMemoryAdmin с Safe Dispatch Free (максимальная скорость и снижение фрагментации VAS).\n"
        << "AllocatorMode=1\n"
        << "\n"
        << "\n"
        << "[Logging]\n"
        << "; Запись подробного журнала работы G3O в файл G3O\\G3O_diagnostic_*.log (для обычной игры держите false).\n"
        << "EnableDiagnosticLog=false\n"
        << "\n"
        << "; Перехват отладочных сообщений игры и плагинов в лог G3O.\n"
        << "InterceptOutputDebugString=false\n"
        << "\n"
        << "; Профилировщик производительности: раз в секунду записывает FPS, время кадров и загрузку CPU.\n"
        << "EnableProfiler=false\n"
        << "\n"
        << "; Запись времени этапов загрузки сохранения в лог G3O\\G3O_LoadProfiler_*.log.\n"
        << "EnableSavegameAudit=false\n"
        << "\n"
        << "; Дополнительный замер времени загрузки слоев мира и чтения свойств (работает вместе с EnableSavegameAudit).\n"
        << "EnableLayerAudit=false\n"
;
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
    s_config.loadReShade = ReadIniBool("General", "LoadReShade", true, path);
    s_config.fixVulkanLayerConflicts = ReadIniBool("General", "FixVulkanLayerConflicts", true, path);

    // [Display]
    s_config.enableBorderless = ReadIniBool("Display", "EnableBorderless", true, path);
    s_config.autoBorderless = ReadIniBool("Display", "AutoBorderless", true, path);
    s_config.preventMinimizeOnFocusLoss = ReadIniBool("Display", "PreventMinimizeOnFocusLoss", true, path);
    s_config.windowCursorLock = ReadIniBool("Display", "WindowCursorLock", true, path);
    s_config.inputSystemKeyPassthrough = ReadIniBool("Display", "InputSystemKeyPassthrough", true, path);

    // [CrashInvestigator]
    s_config.enableCrashInvestigator = ReadIniBool("CrashInvestigator", "EnableCrashInvestigator", true, path);
    s_config.enableCrashGuards = ReadIniBool("CrashInvestigator", "EnableCrashGuards", true, path);
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

    s_config.enableInfoManagerOptimizer = ReadIniBool("Performance", "EnableInfoManagerOptimizer", true, path);
    s_config.enableSessionStartOptimizer = ReadIniBool("Performance", "EnableSessionStartOptimizer", true, path);
    s_config.sessionStartSleepMs = static_cast<uint32_t>(ReadIniInt("Performance", "SessionStartSleepMs", 1, 0, 100, path));
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
    s_config.pakCacheBudgetMB = ReadIniInt("Performance", "PakCacheBudgetMB", 384, 0, 1024, path);
    s_config.pakPrefetchWorkers = ReadIniInt("Performance", "PakPrefetchWorkers", 0, 0, 8, path);

    // [Memory]
    s_config.allocatorMode = ReadIniInt("Memory", "AllocatorMode", 1, 0, 2, path);

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
