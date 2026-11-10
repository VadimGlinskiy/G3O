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
        << "; ==============================================================================\n\n"
        << "[General]\n"
        << "; Главный переключатель мода (true / false).\n"
        << "; Что делает: при false отключает установку всех хуков и оптимизаций G3O.\n"
        << "; Что дает: позволяет мгновенно вернуть чистое поведение игры без удаления dinput8.dll.\n"
        << "EnableG3O=true\n\n"
        << "; Отображение водяного знака в главном меню (true / false).\n"
        << "; Что делает: рисует строку с версией G3O в левом нижнем углу меню.\n"
        << "; Что дает: визуальное подтверждение того, что мод успешно загружен в процесс.\n"
        << "ShowWatermark=true\n\n"
        << "; Загрузка внешних плагинов Script Extender (true / false).\n"
        << "; Что делает: сканирует каталог Data\\G3SE\\Plugins и загружает найденные DLL.\n"
        << "; Что дает: работу модов на базе G3SE без необходимости ставить отдельный инжектор.\n"
        << "LoadG3SEPlugins=true\n\n"
        << "; Устранение конфликтов Vulkan-слоев (true / false).\n"
        << "; Что делает: блокирует перехватчики сторонних программ (например, оверлей Bandicam) при запуске через DXVK.\n"
        << "; Что дает: предотвращает падения видеодрайвера и вылеты при старте игры.\n"
        << "FixVulkanLayerConflicts=true\n\n\n"
        << "[Display]\n"
        << "; Режим окна без рамок / Borderless Windowed (true / false).\n"
        << "; Что делает: убирает системную рамку окна и растягивает его на весь экран.\n"
        << "; Что дает: моментальный Alt-Tab без зависаний, мерцания экрана и сброса разрешения.\n"
        << "EnableBorderless=true\n\n"
        << "; Клавиша ручного переключения режима окна без рамок.\n"
        << "; Что делает: переключает режим отображения прямо во время игры.\n"
        << "; Что дает: возможность на лету сменить видеорежим (F1-F12, INSERT, DELETE, HOME, END, NONE).\n"
        << "ToggleBorderlessKey=F10\n\n"
        << "; Автоматический запуск в окне без рамок (true / false).\n"
        << "; Что делает: переводит игру в полноэкранное окно при старте, если в игре задан оконный режим.\n"
        << "; Что дает: не требует нажимать клавишу переключения при каждом запуске.\n"
        << "AutoBorderless=true\n\n"
        << "; Запрет сворачивания при потере фокуса (true / false).\n"
        << "; Что делает: блокирует реакцию окна на клики вне игры (на втором мониторе или в других окнах).\n"
        << "; Что дает: игра не сворачивается и не заикается, изображение и звук продолжают работать.\n"
        << "PreventMinimizeOnFocusLoss=true\n\n\n"
        << "[Camera]\n"
        << "; Динамическая коррекция угла обзора / FOV (true / false).\n"
        << "; Что делает: пересчитывает вертикальный угол камеры под пропорции экрана (16:9, 21:9, 32:9).\n"
        << "; Что дает: исправляет сплющенную перспективу и эффект «рыбьего глаза» на широких мониторах.\n"
        << "EnableDynamicFOV=true\n\n"
        << "; Базовый угол обзора камеры в градусах.\n"
        << "; Что делает: задает исходный FOV для камеры главного героя (в ваниле ~67°).\n"
        << "; Что дает: комфортный обзор мира без искажения геометрии по краям (рекомендуется 80–90°).\n"
        << "DefaultFOV=85.0\n\n"
        << "; Горячие клавиши регулировки FOV на лету (true / false).\n"
        << "; Что делает: включает обработку комбинаций клавиш для приближения/отдаления камеры.\n"
        << "; Что дает: оперативную подстройку масштаба прямо во время геймплея.\n"
        << "EnableFOVHotkeys=true\n\n"
        << "; Клавиша уменьшения FOV при зажатом Ctrl (дублируется на NumPad -).\n"
        << "; Что делает: приближает камеру к персонажу.\n"
        << "DecreaseFOVKey=[\n\n"
        << "; Клавиша увеличения FOV при зажатом Ctrl (дублируется на NumPad +).\n"
        << "; Что делает: отдаляет камеру от персонажа, расширяя угол обзора.\n"
        << "IncreaseFOVKey=]\n\n"
        << "; Шаг изменения FOV в градусах за одно нажатие горячей клавиши.\n"
        << "FOVStep=5.0\n\n\n"
        << "[CrashInvestigator]\n"
        << "; Перехватчик фатальных ошибок и сбоев (true / false).\n"
        << "; Что делает: регистрирует глобальный обработчик исключений Windows (SEH), перехватывает FatalError и Guru.\n"
        << "; Что дает: предотвращает бесследный вылет процесса, записывая отчет о причинах сбоя.\n"
        << "EnableCrashInvestigator=true\n\n"
        << "; Графическое окно ошибки при сбое (true / false).\n"
        << "; Что делает: показывает диалоговое окно для необработанного Win32-исключения с именем модуля и адресом (для FatalError используется движковый вывод).\n"
        << "; Что дает: позволяет сразу увидеть виновника падения (мод, движок или PhysX).\n"
        << "ShowCrashDialog=true\n\n"
        << "; Создание минидампа памяти (true / false).\n"
        << "; Что делает: сохраняет файл G3O_Crash_*.dmp в папке G3O\\ при необработанном Win32-исключении (FatalError движка контекст дампа не предоставляет).\n"
        << "; Что дает: возможность открыть дамп в Visual Studio / WinDbg и изучить переменные и стек вызовов.\n"
        << "WriteCrashDump=true\n\n"
        << "; Запись текстового отчета о сбое (true / false).\n"
        << "; Что делает: сохраняет лог G3O_CrashReport_*.log с датой, состоянием регистров и стеком.\n"
        << "; Что дает: готовый отчет для локализации бага или отправки авторам модов.\n"
        << "WriteCrashLog=true\n\n\n"
        << "[Performance]\n"
        << "; Оптимизации математики и строк в SharedBase.dll (true / false).\n"
        << "; Что делает: заменяет скалярные проверки пересечения коробок AABB на SSE, ускоряет сравнение строк.\n"
        << "; Что дает: снижает нагрузку на CPU при отсечении невидимых объектов и сглаживает микрофризы.\n"
        << "EnableSharedBaseOptimizations=true\n\n"
        << "; Оптимизатор поиска компонентов сущностей (true / false).\n"
        << "; Что делает: кэширует указатели vptr таблиц в eCEntity::SearchForEntity с быстрым ранним выходом.\n"
        << "; Что дает: ускоряет на 30–40% поиск свойств и компонентов у NPC и предметов при высокой плотности объектов.\n"
        << "EnableEntitySearchOptimizer=true\n\n"
        << "; Аппаратный декомпрессор PAK и сохранений libdeflate (true / false).\n"
        << "; Что делает: заменяет скалярный zlib в FileSystem.dll и Game.dll на SIMD-декомпрессор (AVX2 + BMI2).\n"
        << "; Что дает: ускоряет распаковку модов и файлов сохранений более чем в 4 раза (с 9.2 с до 2.2 с).\n"
        << "EnableFastPakDecompressor=true\n\n"
        << "; Быстрый кэш свойств bCAccessorProperty::ResolveProperty (true / false).\n"
        << "; Что делает: плоский O(1) хэш-кэш для сопоставления идентификаторов свойств объекта.\n"
        << "; Что дает: 100% попаданий (1 000 000+ вызовов за загрузку), исключая холостой поиск в хеш-таблицах движка.\n"
        << "EnablePropertyCache=true\n\n"
        << "; Спекулятивный предраспаковщик PAK (true / false).\n"
        << "; Что делает: запускает фоновый пул потоков, заранее разжимающий блоки .pak по трассе предыдущих загрузок.\n"
        << "; Что дает: разжимает архивы параллельно в фоне до их запроса движком, срезая 4–8 секунд ожидания диска.\n"
        << "EnablePakPrefetch=true\n\n"
        << "; Лимит памяти кэша предварительной декомпрессии PAK в МБ (64–1024).\n"
        << "; Что делает: ограничивает размер буфера разжатых блоков в оперативной памяти.\n"
        << "; Что дает: предотвращает исчерпание 4 ГБ адресного пространства 32-битного процесса (рекомендуется 768 для 300+ модов).\n"
        << "PakCacheBudgetMB=768\n\n"
        << "; Число фоновых потоков предраспаковщика PAK (0 = автоматически).\n"
        << "; Что делает: задает количество рабочих потоков для параллельной декомпрессии блоков (до 6–8 ядер).\n"
        << "; Что дает: максимальную загрузку многоядерных процессоров во время экрана загрузки.\n"
        << "PakPrefetchWorkers=0\n\n"
        << "; Оптимизатор базы пространственной видимости сцены (true / false).\n"
        << "; Что делает: откладывает и пакетирует вызовы eCFrustumDatabase::ExtractFromEntity во время загрузки.\n"
        << "; Что дает: устраняет до 120 000+ холостых промежуточных перестроений базы видимости при спавне сущностей.\n"
        << "EnableFrustumDatabaseOptimizer=true\n\n"
        << "; Быстрое хэширование строк и кэш шаблонов eCTemplateEntityProxy (true / false).\n"
        << "; Что делает: заменяет хэш-функции строк на регистровый ассемблерный Fast DJB2 и кэширует шаблонные сущности.\n"
        << "; Что дает: снимает миллионы вызовов CRT-хэширования за загрузку и ускоряет поиск модовых шаблонов.\n"
        << "EnableHashProxyOptimizer=true\n\n"
        << "; Оптимизатор навигационного графа при загрузке (true / false).\n"
        << "; Что делает: глушит холостые тики gCNavigationAdmin::Process, пока идет загрузка сохранения.\n"
        << "; Что дает: отдает 100% времени CPU главному потоку на создание геометрии и объектов, исключая пустой поиск путей.\n"
        << "EnableNavigationOptimizer=true\n\n"
        << "; Экспоненциальное предвыделение узлов сцены eCNode::AttachChild (true / false).\n"
        << "; Что делает: заменяет медленный 25%-й рост массива детей на ступенчатое выделение (1024 -> 8192 -> 32768) через нативный Realloc.\n"
        << "; Что дает: полностью убирает 25–30 каскадных переаллокаций памяти на каждом крупном секторе (сектор RB_Mod быстрее на ~160 мс).\n"
        << "EnableSceneNodeOptimizer=true\n\n"
        << "; Оптимизатор теневых кастеров eCShadowMapDirectionalLightBase (true / false).\n"
        << "; Что делает: заменяет линейный поиск и memmove на O(1) Swap-and-Pop и хэш-индексацию.\n"
        << "; Что дает: ускоряет телепортацию камеры и спавн в GotoLoadedPosition на 5–8 секунд, убирает микрофризы теней в геймплее.\n"
        << "EnableShadowOptimizer=true\n\n"
        << "; Оптимизатор фабрики классов SharedBase bCPropertyObjectSingleton::ReadObject (true / false).\n"
        << "; Что делает: кэширует reflection-дескрипторы типов (L1 кэш) и заменяет Malloc/Free на Zero-Alloc переиспользование буфера bCString.\n"
        << "; Что дает: ускоряет десериализацию объектов SysDyn и кэшей на 3–5 секунд, исключает 60 000+ аллокаций памяти движка при загрузке.\n"
        << "EnableClassFactoryOptimizer=true\n\n"
        << "; Оптимизатор префетчера PVS eCPVSPrefetcher3 (true / false).\n"
        << "; Что делает: сжимает радиус синхронного спавна в GotoLoadedPosition до ближней зоны коллизий террейна,\n"
        << "; отсекает избыточные дублирующие синхронные прогоны и плавно догружает дальние ячейки после старта.\n"
        << "; Что дает: срезает 7–9 секунд с блокирующей фазы GotoLoadedPosition, исключая провалы сквозь землю.\n"
        << "EnablePVSPrefetchOptimizer=true\n\n"
        << "; Радиус сферы начального спавна PVS в GotoLoadedPosition (в см, по умолчанию 3800).\n"
        << "PVSBootRadius=3800\n\n"
        << "; Порог дедупликации повторных синхронных вызовов PVS в GotoLoadedPosition (в см, по умолчанию 200).\n"
        << "PVSRedundantDistance=200\n\n"
        << "; Количество кадров плавного расширения радиуса PVS до максимума после загрузки (по умолчанию 24).\n"
        << "PVSRampUpCalls=24\n\n"
        << "; Подавление фоновых квестовых проверок при загрузке (true / false).\n"
        << "; Что делает: блокирует вызовы HaveNPCsAllRequiredItems и HasImportantInfo во время экрана загрузки мира.\n"
        << "; Что дает: исключает бесполезный перебор всех квестов и инвентарей при добавлении каждого предмета во время спавна.\n"
        << "EnableQuestLoadOptimizer=true\n\n"
        << "; Оптимизатор поиска диалогов в gCInfoManager_PS (true / false).\n"
        << "; Что делает: заменяет 43-бакетный обход связных списков на плоскую теневую хэш-таблицу O(1).\n"
        << "; Что дает: сокращает фазу десериализации диалогов в SysDyn с 3.7 секунд до менее чем 5 миллисекунд.\n"
        << "EnableInfoManagerOptimizer=true\n\n"
        << "; Бинарный кэш диалогов и квестов (true / false).\n"
        << "; Что делает: сохраняет разобранные 24,555 .info файлов в скомпилированный бинарный образ (Data\\Infos\\_compiledinfos_<World>.bin) и загружает его через быстрый буферизованный поток в памяти.\n"
        << "; Что дает: ускоряет загрузку сохранений на ~2.8-2.9 секунды за счет полного пропуска медленного парсинга десятков тысяч текстовых файлов.\n"
        << "EnableInfoManagerBinaryCache=true\n\n"
        << "; Ускоритель загрузки текстур (true / false).\n"
        << "; Что делает: перехватывает загрузку текстур через Direct3D 9, пропуская избыточные промежуточные конвертации.\n"
        << "; Что дает: снижает заикания при фоновой подгрузке текстур в процессе перемещения по миру.\n"
        << "EnableFastTextureLoader=true\n\n"
        << "; Оптимизатор геометрии и теней растительности (true / false).\n"
        << "; Что делает: ограничивает предельную дистанцию расчета теней крон деревьев и радиус сетки плотной травы.\n"
        << "; Что дает: защищает от резких просадок FPS в густых лесах Миртаны и Нордмара.\n"
        << "EnableVegetationOptimizer=true\n\n"
        << "; Предельная дальность теней деревьев в игровых единицах (100 единиц = 1 метр).\n"
        << "; Что дает: 6500.0 (65 метров) сохраняет тени вокруг игрока, снижая нагрузку на CPU на дальних планах.\n"
        << "TreeShadowDistance=6500.0\n\n"
        << "; Предельный радиус сетки генерации травы в игровых единицах (0 = без ограничения).\n"
        << "; Что дает: 7500.0 (75 метров) защищает от переполнения буфера полигонов травы на максимальных настройках.\n"
        << "VegetationGridRangeClamp=7500.0\n\n"
        << "; Высокоточный системный таймер 1 мс (true / false).\n"
        << "; Что делает: вызывает timeBeginPeriod(1) на время работы процесса игры.\n"
        << "; Что дает: равномерное время кадра (Frame Pacing), стабильные тайминги потоков и снижение микрофризов.\n"
        << "EnableHighPrecisionTimer=true\n\n"
        << "; Высокий приоритет процесса игры (true / false).\n"
        << "; Что делает: выставляет процессу Gothic3.exe приоритет HIGH_PRIORITY_CLASS в Windows.\n"
        << "; Что дает: защищает игру от отбора квантов процессора фоновыми приложениями ОС.\n"
        << "EnableHighProcessPriority=true\n\n"
        << "; Привязка к производительным ядрам CPU (true / false).\n"
        << "; Что делает: анализирует архитектуру CPU и удерживает потоки игры на P-ядрах, избегая E-ядер (Intel) и медленных ядер.\n"
        << "; Что дает: максимальную вычислительную мощность для однопоточного рендера Genome Engine.\n"
        << "EnableCPUTopologyAffinity=true\n\n"
        << "; Кэширование путей в виртуальной файловой системе VFS (true / false).\n"
        << "; Что делает: сохраняет успешные пути к файлам в .pak архивах для быстрого повторного поиска.\n"
        << "; Что дает: сокращает накладные расходы при частых проверках наличия ресурсов в архивах.\n"
        << "EnableVFSCache=true\n\n"
        << "; Ручное ограничение числа физических ядер (0 = автоматически).\n"
        << "CPUCores=0\n\n"
        << "; Ручное ограничение числа логических потоков (0 = автоматически).\n"
        << "CPUThreads=0\n\n"
        << "; Явная шестнадцатеричная маска привязки ядер процессора (0 = автоматически).\n"
        << "CPUAffinityMask=0x0\n\n"
        << "; Фильтрация повторяющихся состояний Direct3D 9 (true / false).\n"
        << "; Что делает: пропускает повторные вызовы SetRenderState / SetTextureStageState / SetSamplerState.\n"
        << "; Что дает: на однопоточном устройстве пропускает повторы; при многопоточном режиме или неподдерживаемых state blocks отключается.\n"
        << "EnableD3D9StateFilter=true\n\n\n"
        << "[Logging]\n"
        << "; Запись подробного диагностического журнала (true / false).\n"
        << "; Что делает: создает G3O\\G3O_diagnostic_*.log с фиксацией запуска всех хуков, модулей и параметров системы.\n"
        << "; Что дает: журнал запуска и работы оптимизаций; можно отключить, если подробный лог не нужен.\n"
        << "EnableDiagnosticLog=true\n\n"
        << "; Перехват системных сообщений OutputDebugStringA (true / false).\n"
        << "; Что делает: перенаправляет скрытый вывод движка и сторонних библиотек в лог G3O.\n"
        << "; Что дает: сбор полной информации об ошибках движка.\n"
        << "InterceptOutputDebugString=true\n\n"
        << "; Ежесекундный профилировщик производительности (true / false).\n"
        << "; Что делает: записывает в G3O\\G3O_profile_*.log данные FPS, 1% Low, p99 времени кадра и нагрузку CPU.\n"
        << "; Что дает: детальный мониторинг плавности геймплея и поведения игры под нагрузкой.\n"
        << "EnableProfiler=true\n\n"
        << "; Высокоточный аудит загрузки сохранений (true / false).\n"
        << "; Что делает: сохраняет в G3O\\G3O_LoadProfiler_*.log подробную разбивку времени загрузки сейва до миллисекунд.\n"
        << "; Что дает: точные замеры времени декомпрессии, чтения диска, спавна секторов и узлов сцены.\n"
        << "EnableSavegameAudit=true\n\n"
        << "; Глубокий микропрофайлинг слоев и свойств мира (true / false).\n"
        << "; Что делает: активирует 19 высокочастотных таймеров QPC на уровне отдельных сущностей и слоев.\n"
        << "; Что дает: инструмент разработчиков для поиска узких мест (добавляет ~0.2–0.3 с накладных расходов на загрузку).\n"
        << "EnableLayerAudit=true\n";
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
    s_config.enableEntitySearchOptimizer = ReadIniBool("Performance", "EnableEntitySearchOptimizer", true, path);
    s_config.enableFastPakDecompressor = ReadIniBool("Performance", "EnableFastPakDecompressor", true, path);
    s_config.enablePropertyCache = ReadIniBool("Performance", "EnablePropertyCache", true, path);
    s_config.enablePakPrefetch = ReadIniBool("Performance", "EnablePakPrefetch", true, path);
    s_config.enableFrustumDatabaseOptimizer = ReadIniBool("Performance", "EnableFrustumDatabaseOptimizer", true, path);
    s_config.enableHashProxyOptimizer = ReadIniBool("Performance", "EnableHashProxyOptimizer", true, path);
    s_config.enableNavigationOptimizer = ReadIniBool("Performance", "EnableNavigationOptimizer", true, path);
    s_config.enableSceneNodeOptimizer = ReadIniBool("Performance", "EnableSceneNodeOptimizer", true, path);
    s_config.enableShadowOptimizer = ReadIniBool("Performance", "EnableShadowOptimizer", true, path);
    s_config.enableClassFactoryOptimizer = ReadIniBool("Performance", "EnableClassFactoryOptimizer", true, path);
    s_config.enablePVSPrefetchOptimizer = ReadIniBool("Performance", "EnablePVSPrefetchOptimizer", true, path);
    s_config.pvsBootRadius = ReadIniFloat("Performance", "PVSBootRadius", 3800.0f, 500.0f, 50000.0f, path);
    s_config.pvsRedundantDistance = ReadIniFloat("Performance", "PVSRedundantDistance", 200.0f, 10.0f, 5000.0f, path);
    s_config.pvsRampUpCalls = static_cast<uint32_t>(ReadIniInt("Performance", "PVSRampUpCalls", 24, 0, 1000, path));
    s_config.enableQuestLoadOptimizer = ReadIniBool("Performance", "EnableQuestLoadOptimizer", true, path);
    s_config.enableInfoManagerOptimizer = ReadIniBool("Performance", "EnableInfoManagerOptimizer", true, path);
    s_config.enableInfoManagerBinaryCache = ReadIniBool("Performance", "EnableInfoManagerBinaryCache", true, path);

    s_config.enableD3D9StateFilter = ReadIniBool("Performance", "EnableD3D9StateFilter", true, path);
    s_config.enableFastTextureLoader = ReadIniBool("Performance", "EnableFastTextureLoader", true, path);
    s_config.enableVegetationOptimizer = ReadIniBool("Performance", "EnableVegetationOptimizer", true, path);
    s_config.treeShadowDistance = ReadIniFloat("Performance", "TreeShadowDistance", 6500.0f, 0.0f, 100000.0f, path);
    s_config.vegetationGridRangeClamp = ReadIniFloat("Performance", "VegetationGridRangeClamp", 7500.0f, 0.0f, 100000.0f, path);
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
    s_config.enableDiagnosticLog = ReadIniBool("Logging", "EnableDiagnosticLog", true, path);
    s_config.interceptOutputDebugString = ReadIniBool("Logging", "InterceptOutputDebugString", true, path);
    s_config.enableProfiler = ReadIniBool("Logging", "EnableProfiler", true, path);
    s_config.enableSavegameAudit = ReadIniBool("Logging", "EnableSavegameAudit", true, path);
    s_config.enableLayerAudit = ReadIniBool("Logging", "EnableLayerAudit", true, path);

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
