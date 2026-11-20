# WORK.md — Автоматизированный тестовый контур для Gothic 3 Optima (G3O)

> **Статус:** Все фазы (`AT-01`–`AT-12`) выполнены и верифицированы на живых и офлайн-прогонах.  
> **Назначение:** Убрать ручное узкое место в цикле разработки G3O (`правка -> сборка -> деплой -> запуск игры -> загрузка сейва -> повторная загрузка -> разбор логов`), чтобы агент мог автономно (по разрешению пользователя) запускать `Gothic3.exe`, прогонять серию загрузок сохранения, собирать профили/диагностику/краш-репорты и получать воспроизводимый вердикт `PASS / FAIL / CRASH / TIMEOUT`.

---

## 1. Архитектура решения

Система состоит из двух взаимодействующих слоёв:

1. **Внутрипроцессный драйвер `AutoTest` внутри `dinput8.dll`:**
   - Активируется **преимущественно через переменные окружения дочернего процесса** (`G3O_AUTOTEST=1`, `G3O_AUTOTEST_SLOT=...`, и т. д.), с опциональным fallback на секцию `[AutoTest]` в `G3O.ini`. Обычный пользовательский запуск и файл `G3O.ini` не загрязняются.
   - Управляется чистой тестируемой машиной состояний (`src/autotest_policy.h`).
   - Инициирует загрузку сохранения **в главном потоке на безопасной точке игрового цикла** (не из `IDirect3DDevice9::Present`!), после детерминированного подтверждения готовности главного меню.
   - Выполняет `N` загрузок заданного слота (по умолчанию `2` — для пары «первая загрузка / повтор», либо `N` для серии), выдерживая между ними `SettleFrames` кадров/тиков движка, а не слепой `Sleep`.
   - Подавляет все блокирующие модальные окна (WER, `ShowCrashDialog`, диалог движка «Where is the guru?» в `HookedCallFatalError1/2`), отключает захват курсора (`WindowCursorLock=false`), ведёт внутренний watchdog и гарантированно сбрасывает логи перед выходом.

2. **Внешний раннер и анализатор (`tools/run_autotest.ps1` + `tools/autotest_parser.ps1`):**
   - Проверяет окружение (интерактивная сессия, отсутствие уже запущенного `Gothic3.exe`, наличие файлов сохранения, опциональная сборка и деплой `dinput8.dll` с бэкапом).
   - Фиксирует **SHA-256** `dinput8.dll` и ключевых DLL игры (`Game.dll`, `Engine.dll`, `SharedBase.dll`, `FileSystem.dll`), закрывая требование привязки профиля к конкретной сборке из `context.txt` (п. 8.1).
   - Запускает `Gothic3.exe` дочерним процессом с изолированными `G3O_AUTOTEST*` env-переменными и внешним таймаут-сторожем.
   - После выхода (или принудительного завершения по таймауту) привязывает созданные для данного `PID` / `RunId` файлы `G3O_LoadProfiler_*.log`, `G3O_diagnostic_*.log`, `G3O_AutoTest_*.json` и `G3O_CrashReport_*.log`/`.dmp`, извлекает метрики, сравнивает с baseline и формирует `summary.json` + консольную сводку `PASS / FAIL / CRASH / TIMEOUT`.

---

## 2. Жёсткие ограничения и инварианты проекта

1. **Никаких кэшей (`context.txt`):** автотест не создаёт файловых или оперативных кэшей, не предзагружает и не переиспользует данные движка между запросами. Только оркестрация запуска/загрузки и сбор телеметрии.
2. **Неприкосновенность запрещённых точек движка:** не хукать и не менять поведение `gCNavigationAdmin::Process()`, курсоров `bCIStream` и `gCInfoManager`.
3. **Неизменность пользовательских данных:** автотест никогда не перезаписывает файлы сохранений (`*.g3savcpx`, `*.g3savcpxdat`), не модифицирует пользовательский `G3O.ini` и не оставляет «грязных» глобальных переменных окружения.
4. **Нулевое влияние на обычный запуск:** если `G3O_AUTOTEST` не задан (и `[AutoTest]` не включён вручную), хуки автотеста не устанавливаются, а поведение `dinput8.dll` и генерируемого `G3O.ini` остаётся прежним (`tests/config_ini` и `tests/diagnostic_context` должны проходить).
5. **Корректность метрик (`context.txt` п. 8.3):** разделять общее время загрузки (`gCHUDFileManager::LoadGame`), время фаз и перекрывающиеся/вложенные счётчики (никогда не складывать inclusive-таймеры в «общее время»). Отдельно учитывать разницу между `Load #1` (активация секторов + `SysDyn`, влияние кэша ОС) и `Load #2..N` (повторная загрузка в том же процессе, где `gCWorld::Activate` может иметь 0 секторов).
6. **Правило разрешения на запуск игры:** реальный запуск `Gothic3.exe` через `tools/run_autotest.ps1` выполняется агентом только в рамках явного разрешения пользователя (одно разрешение на серию прогонов до команды «стоп» либо поштучное подтверждение — как определит пользователь).

---

## 3. План реализации (задачи)

### Фаза 0. Предпосылки и аудит среды

- [x] **AT-01. Проверить условия запуска `Gothic3.exe` и параметры тестового окружения**
  - **Цель:** Убедиться, что прямой запуск дочернего `Gothic3.exe` из `E:\SteamLibrary\steamapps\common\Gothic 3` воспроизводим и предсказуем.
  - **Что проверить:**
    - Требуется ли запущенный клиент Steam или `steam_appid.txt` (`39500`) при прямом старте `Gothic3.exe` (без `steam://rungameid/...`).
    - Какие файлы сохранений доступны в `%USERPROFILE%\Documents\gothic3` (сейчас видны `Autosave_Slot_0001.g3savcpx/.g3savcpxdat` и `G3_World_01_0001.g3savcpx/.g3savcpxdat`), какой слот назначить эталонным по умолчанию.
    - Влияние `DXVK` (`dxvk.conf`, `Gothic3.dxvk-cache`) и `ReShade32.dll` (в папке игры присутствует `ReShade.log`) на время инициализации и риск зависания при выходе; проверить необходимость флага `G3O_AUTOTEST_DISABLE_RESHADE=1`.
    - Поведение оконного / безрамочного режима (`src/borderless_window.cpp`, `Ini\ge3.ini`), чтобы при запуске в фоне/без фокуса не возникало `D3DERR_DEVICELOST` и захвата мыши.
  - **Критерий готовности (DoD):** Зафиксированы проверенные требования к запуску (`WorkingDirectory`, Steam, DXVK/ReShade, слот, интерактивность сессии).
  - **Результаты аудита (2026-11-19):**
    - **Steam / DRM:** `Gothic3.exe` (204 800 байт) импортирует только `KERNEL32.dll`, `USER32.dll`, `paru_s1.dll`, `paru_s2.dll`, `SharedBase.dll`, `Importer.dll`, `Game.dll`, `Engine.dll`, `fmodex.dll`. Импорта `steam_api.dll` нет, `steam_appid.txt` отсутствует и не требуется — прямой запуск `Gothic3.exe` с `WorkingDirectory = E:\SteamLibrary\steamapps\common\Gothic 3` не зависит от Steam DRM.
    - **Сессия и права:** `UserInteractive = True`, `SessionId = 1`, папка `E:\SteamLibrary\steamapps\common\Gothic 3\G3O` доступна на запись.
    - **DXVK / ReShade:** в текущей папке игры `d3d9.dll`, `dxgi.dll`, `ReShade32.dll` и `ReShade.ini` отсутствуют (игра работает на нативном D3D9; `dxvk.conf` и `ReShade.log` — остаточные файлы). Флаг `G3O_AUTOTEST_DISABLE_RESHADE=1` всё равно оставляем в дизайне на случай установки ReShade в будущем.
    - **Окно и текущие настройки `G3O.ini`:** в `Documents\gothic3\UserOptions.ini` задано `WindowMode=1` (`1920x1080@60`); в развёрнутом `G3O.ini` включены `EnableBorderless=true`, `AutoBorderless=true`, `PreventMinimizeOnFocusLoss=true`, `WindowCursorLock=true`, `ShowCrashDialog=true`, `EnableDiagnosticLog=true`, `EnableSavegameAudit=true`, `EnableLayerAudit=true`, `EnableProfiler=true`. Это подтверждает необходимость runtime-оверрайдов `windowCursorLock=false` и `showCrashDialog=false` в режиме `AutoTest`.
    - **Сохранения (`%USERPROFILE%\Documents\gothic3`):** доступны 3 полные пары (`.g3savcpx` + `.g3savcpxdat`): `Autosave_Slot_0001` (4.88 MB, эталон по умолчанию), `G3_World_01_0001` (4.88 MB) и `QuickSave` (4.88 MB).
    - **Зафиксированные SHA-256 ключевых модулей:**
      - `Gothic3.exe`: `8D47A07E1158AFF941E7AF28215FC0605EF954956004BE937595CF8AFA5B98B1`
      - `dinput8.dll`: `95737D1CE18743E2F0BC976CECD2F7DAA8DBCA77512D63EA477CE8298CD7850C`
      - `Game.dll`: `4728A47B244ADCB0EA2EC3501DB04994909DCFCFF338C7B8075F2B68DD8D6E00`
      - `Engine.dll`: `991CF674A9CF1BC60CDDC204BE0993E8251336F4D3471543A37846CC6840BF5A`
      - `SharedBase.dll`: `3087BB4E6C8C7F3BFCF87F79BA889A8C9DF223CB71D977963E97DFB51206D4A4`
      - `FileSystem.dll`: `1DB1E4FF19CF8A2FF1C012AC22B30DE8109FEE1235E56CE8B6723B9C0592B4C1`

---

### Фаза 1. Статический анализ движка (`Game.dll`, `Engine.dll`, `Gothic3.exe`)

> Блокер для Фазы 3. Все адреса, сигнатуры и соглашения вызова должны быть подтверждены по бинарникам (через существующий проект Ghidra `tools/ghidra/proj/G3.gpr` и справочные заголовки `E:\Projects\gothic3sdk`), без догадок.

- [x] **AT-02. Дизассемблировать `gCHUDFileManager::LoadGame` (`Game.dll+0xA9A10`) и её вызывающий путь**
  - **Цель:** Точно определить, как программно и безопасно инициировать загрузку указанного слота сохранения изнутри `dinput8.dll`.
  - **Исходные ориентиры:**
    - В `src/savegame_profiler.cpp` (`FindHUDLoadGameTarget`, строки 1278, 1849–1884) уже хукается `Game.dll+0xA9A10` с сигнатурой `55 8B EC 83 E4 F8 8B 45 08 83 EC 5C 83 78 04 00 53 56 57`, но параметры пока типизированы как `(void* pThis, void* a, void* b)`.
    - Пролог `mov eax, [ebp+8]; cmp dword ptr [eax+4], 0` явно указывает, что первый аргумент — указатель на структуру/объект, а не сырой `const char*`.
  - **Что установить статически:**
    1. Точные типы и ABI для `pThis`, `a`, `b` (в т. ч. нужен ли `bCString` из `SharedBase.dll`, его конструктор/деструктор и рефкаунтинг).
    2. Откуда движок получает экземпляр `gCHUDFileManager*` (синглтон, поле в `gCGUIManager`, глобальный указатель в `Game.dll`, либо перехват `pThis` при инициализации меню).
    3. Все вызывающие места (callers) `Game.dll+0xA9A10`: вызывается ли функция напрямую из обработчика меню загрузки, или движок использует отложенную очередь/флаг, подбираемый на следующем тике.
    4. Совместимость вызова из двух контекстов: (a) из главного меню (`gEHUDMenu_Main`) и (b) из активного геймплея (`Phase_Gameplay`, повторная загрузка). Какие предусловия по состоянию меню/HUD/сессии ожидает функция.
    5. Проверить в `Gothic3.exe` / `Game.dll` / `Engine.dll`, есть ли встроенные аргументы командной строки (через `eCApplication::GetCommandLineA`) или консольные команды для загрузки сохранения.
    6. Если прямой вызов `gCHUDFileManager::LoadGame` требует специфического состояния UI-окна загрузки (`gEHUDMenu_Load`), определить более чистую точку входа (caller / контроллер загрузки / отложенный триггер) и зафиксировать запасной вариант.
  - **Критерий готовности (DoD):** Задокументированы точная C++-сигнатура целевой функции загрузки, способ получения её `this`, правила конструирования/освобождения аргументов (`bCString` / дескриптор слота) и подтверждена пригодность для Load #1 (из меню) и Load #2..N (из геймплея).
  - **Результаты статического разбора (`Game.dll`, 2026-11-19):**
    - **Критическая находка по ABI:** `Game.dll+0xA9A10` (`gCHUDFileManager::LoadGame`) — это **`__stdcall` с 2 стековыми аргументами (`ret 8`)**, а НЕ `__thiscall`!
      ```cpp
      using HUDLoadGame_t = bool (__stdcall*)(void* pHUDFileManager, const void* pSlotNameBString);
      ```
      *(В `src/savegame_profiler.cpp` текущий `typedef` объявлен как `__thiscall` с 3 аргументами и работал только потому, что проброс `ecx` игнорировался, а `a, b` ложились на стек с `ret 8`. Для прямого вызова из `AutoTest` обязательно использовать `__stdcall(pMgr, &bstr)`!)*
    - **Получение экземпляра `pHUDFileManager`:** `Game.dll+0xA92D0` (`void* (__cdecl*)()`) — ленивый синглтон, возвращающий указатель на статический `gCHUDFileManager` (`Game.dll+0x403C20`). Поле `[pMgr + 4]` — указатель на прикреплённый контроллер главного меню (`gCHUD`, проверяется на `nullptr` в `0x100A9A1C`: `"gCHUDFileManager::LoadGame -> Main menu is not attached."`).
    - **Оба вызывающих места в `Game.dll` (`0x1007DA5E` — QuickLoad в `gCGUIManager`, `0x1008EA42` — кнопка Load в меню):**
      1. Если загрузка идёт из геймплея (как в QuickLoad `0x1007DA3A`), сначала вызывается `gCGUIManager::OpenMenu()` (`Game.dll+0x07C2E0`, экспорт `?OpenMenu@gCGUIManager@@QAE_NXZ`) на `gCSession::AccessSession().GetGUIManager()` (`Game.dll+0x172F80`, `Game.dll+0x174470`).
      2. На стеке конструируется `bCString` (4 байта) через `SharedBase.dll!??0bCString@@QAE@PBD@Z` (`0x0029C0`, `__thiscall`).
      3. Вызывается `pMgr = HUDFileManager_GetInstance()` (`Game.dll+0xA92D0`), затем `LoadGame(pMgr, &bstr)`.
      4. Вызывается деструктор `SharedBase.dll!??1bCString@@QAE@XZ` (`0x001A00`, `__thiscall`).
    - **Пригодность для Load #1 и Load #2..N:** `LoadGame` сама вызывает `gCWorld::GetSaveGameHeader` (при отсутствии слота сразу возвращает `false`), `gCSession::Stop()`, `eCThreadPool::EnableAllThreads(false)`, `eCGUIModule::ShowCursor(false)`, закрывает активное меню, вызывает `gCWorld::LoadGameWorld`, `gCSession::GotoLoadedPosition` и `gCSession::Start`. Подходит для обоих сценариев напрямую; запасной ввод через `SendInput` не требуется.

- [x] **AT-03. Найти безопасную точку главного цикла, предикат готовности меню, пропуск интро и способ выхода**
  - **Цель:** Исключить вызов загрузки из `IDirect3DDevice9::Present` и убрать ненадёжные «слепые» таймауты.
  - **Что установить статически (ориентиры из `gothic3sdk`):**
    1. **Безопасная точка тика в главном потоке:** проверить `eCApplication::OnProcess()` / `eCApplication::Process()` (`Engine.dll`), `gCSession::Process()` и `gCGUIManager::OnProcess()` (`Game.dll`) — выбрать точку на границе кадра вне активного рендер-пасса (`BeginScene`/`EndScene`) и вне внутренних локов рендерера/DXVK.
    2. **Детерминированный признак «Главное меню готово»:** проверить комбинацию `eCApplication::IsInitialised()`, `gCHUDMenu::OnMenuChanged(gEHUDMenu, gEHUDMenu)` (`gEHUDMenu_Main = 0`, `gEHUDMenu_Intro = 12`, `gEHUDMenu_Loading = 13`), `gCGUIManager::IsMenuOpen()` и `!gCSession::IsGameRunning()`.
    3. **Пропуск вступительных роликов в режиме `AutoTest`:** проверить `eCApplication::PlayVideo(bCString const&)` (`Engine.dll`) и логику показа `gEHUDMenu_Intro`, чтобы безопасно пропускать интро только при `autoTestSkipIntro == true` (экономия ~10–15 с на каждом запуске, без правки файлов игры).
    4. **Стратегия выхода процесса:** проверить `eCApplication::OnQuit` / `WM_CLOSE` и оценить риск дедлоков в `DllMain(DLL_PROCESS_DETACH)` при `ExitProcess` под DXVK; зафиксировать безопасную последовательность: запись `AUTOTEST_DONE` + `flush`/`fclose` всех логов -> контролируемый выход (`TerminateProcess(GetCurrentProcess(), exitCode)` либо `ExitProcess` с коротким fallback-таймаутом в watchdog).
  - **Критерий готовности (DoD):** Зафиксированы конкретные экспортируемые имена / RVA / сигнатуры для (1) тика главного потока, (2) сигнала готовности меню, (3) пропуска интро и (4) завершения процесса.
  - **Результаты статического разбора (`Game.dll` / `Engine.dll`, 2026-11-19):**
    1. **Безопасная точка главного цикла:** хук экспортируемой `gCSession::Process()` (`Game.dll+0x1769F0`, `?Process@gCSession@@UAEXXZ`) — выполняется в главном потоке один раз на тик вне `Present` и в главном меню, и в геймплее (альтернатива: `gCGUIManager::OnProcess`, `Game.dll+0x07DEA0`, `?OnProcess@gCGUIManager@@QAEXXZ`).
    2. **Детерминированный предикат `IsMainMenuReady()`:**
       - `pMgr = HUDFileManager_GetInstance()` (`Game.dll+0xA92D0`) != `nullptr`;
       - `pMainMenu = *(void**)((BYTE*)pMgr + 4)` != `nullptr` (главное меню прикреплено);
       - `currentMenu = *(int*)((BYTE*)pMainMenu + 0x48) == 0` (`gEHUDMenu_Main = 0`, т. е. не `gEHUDMenu_Intro = 12` и не `gEHUDMenu_Loading = 13`, подтверждено по `Game.dll+0xA9B39`);
       - `gCSession::AccessSession().IsGameRunning() == false` (`Game.dll+0x174390`, `?IsGameRunning@gCSession@@QBE?B_NXZ`) для Load #1.
    3. **Пропуск интро:** в `Ini\logo.ini` перечислены 3 ролика (`G3_Logo_01.bik`, `Publisher.bik`, `CPT.bik`). Для пропуска без правки `logo.ini` в режиме `autoTestSkipIntro == true` хукается `gCHUDMenu::OpenMenu` (`Game.dll+0x09EA10`, `?OpenMenu@gCHUDMenu@@IAEXW4gEHUDMenu@@@Z`): при попытке открыть `gEHUDMenu_Intro (12)` подменяем аргумент на `gEHUDMenu_Main (0)`.
    4. **Стратегия выхода процесса:** после записи `[AutoTest] DONE`/`ABORT` и `G3O_AutoTest_<RunId>.json` вызываем `FlushDiagnosticLog()`, `ShutdownProfiler()` (если активен), `ShutdownDiagnosticLogger()` (сброс и закрытие файловых дескрипторов), и сразу `TerminateProcess(GetCurrentProcess(), exitCode)` — это полностью исключает дедлоки `DllMain(DLL_PROCESS_DETACH)` / D3D9 / DXVK / воркеров на выходе.

---

### Фаза 2. Конфигурация и чистая логика (`*_policy.h`)

- [x] **AT-04. Добавить конфигурацию `AutoTest` с приоритетом переменных окружения над INI**
  - **Файлы:** `src/config_manager.h`, `src/config_manager.cpp`, `src/applied_config_snapshot.h`, `src/savegame_profiler_policy.h`, `tests/config_ini/test.cpp`, `tests/diagnostic_context/test.cpp`.
  - **Требования:**
    - Добавить в `G3O::Config` поля `AutoTest`:
      - `bool autoTestEnabled = false` (`G3O_AUTOTEST`, fallback `[AutoTest] Enabled`)
      - `std::string autoTestSlot = "Autosave_Slot_0001"` (`G3O_AUTOTEST_SLOT`, fallback `Slot`)
      - `int autoTestLoads = 2` (диапазон `1..20`, `G3O_AUTOTEST_LOADS`, fallback `Loads`)
      - `int autoTestSettleFrames = 60` (диапазон `1..600`, `G3O_AUTOTEST_SETTLE_FRAMES`, fallback `SettleFrames`)
      - `int autoTestMenuSettleTicks = 30` (диапазон `1..600`, `G3O_AUTOTEST_MENU_SETTLE_TICKS`, fallback `MenuSettleTicks`)
      - `int autoTestTimeoutSec = 180` (диапазон `30..1800`, `G3O_AUTOTEST_TIMEOUT_SEC`, fallback `TimeoutSec`)
      - `bool autoTestSkipIntro = true` (`G3O_AUTOTEST_SKIP_INTRO`, fallback `SkipIntro`)
      - `bool autoTestDisableReShade = true` (`G3O_AUTOTEST_DISABLE_RESHADE`, fallback `DisableReShade`)
      - `bool autoTestEnableLayerAudit = false` (`G3O_AUTOTEST_LAYER_AUDIT`, fallback `EnableLayerAudit`)
      - `std::string autoTestRunId` (`G3O_AUTOTEST_RUN_ID`, опциональный идентификатор прогона)
    - **Приоритет:** переменные окружения всегда перекрывают `G3O.ini`.
    - **Чистота релизного INI:** `CreateDefaultIniFile()` в `src/config_manager.cpp` и проектный `G3O.ini` **не** должны рекламировать разработческий `[AutoTest]` для обычных игроков (сохраняются все проверки `tests/config_ini/test.cpp`).
    - **Безопасные runtime-оверрайды в памяти при `autoTestEnabled == true`** (без записи обратно в `G3O.ini`):
      - `enableDiagnosticLog = true`
      - `enableSavegameAudit = true`
      - `enableLayerAudit = cfg.enableLayerAudit || cfg.autoTestEnableLayerAudit`
      - `enableCrashInvestigator = true`, `writeCrashLog = true`
      - `showCrashDialog = false` (обязательно — никаких `MessageBox`)
      - `windowCursorLock = false` (не запирать курсор пользователя)
      - при `autoTestDisableReShade == true` -> `loadReShade = false`
    - Обновить `ShouldInstallSavegameProfilerHooks(cfg)` и `ShouldCollectSavegameAudit(cfg)` в `src/savegame_profiler_policy.h`, чтобы при `autoTestEnabled == true` хуки загрузки и отчёт `G3O_LoadProfiler` всегда были активны.
    - В `AppendAppliedConfigSnapshot()` выводить блок `[AutoTest]` только при `config.autoTestEnabled == true` (чтобы сохранить обратную совместимость снапшота для обычного режима и `tests/diagnostic_context`).
  - **Критерий готовности (DoD):** Парсинг env/INI и оверрайды покрыты unit-тестами; `tests/config_ini`, `tests/diagnostic_context` и `tests/savegame_profiler_policy` проходят без сбоев.

- [x] **AT-05. Спроектировать и протестировать машину состояний `src/autotest_policy.h`**
  - **Файлы:** `src/autotest_policy.h`, `tests/autotest_policy/CMakeLists.txt`, `tests/autotest_policy/test.cpp`.
  - **Требования:**
    - Вынести всю логику переходов, таймаутов и кодов завершения в чистый header-only модуль без зависимостей от WinAPI/MinHook (в стиле остальных `*_policy.h` проекта).
    - **Состояния:**
      - `Disabled`
      - `WaitingForMainMenu`
      - `MenuSettling` (отсчёт `autoTestMenuSettleTicks` тиков после `isMenuReady`)
      - `Loading` (отправлена команда загрузки `loadIndex`, ожидание `OnLoadFinished`)
      - `PostLoadSettling` (отсчёт `autoTestSettleFrames` тиков/кадров геймплея после успешной загрузки)
      - `Finishing`
      - `Done`
      - `Aborted` (с явной причиной и кодом возврата)
    - **Единая таблица кодов возврата процесса:**
      - `0` — `Done` (все `N` загрузок успешны)
      - `10` — `LoadFailed` (`LoadGame` вернул `false` или не стартовал)
      - `11` — `HookUnavailable` (критичный хук или объект движка не найден)
      - `12` — `InternalTimeout` (сработал внутренний таймаут старта / меню / загрузки / общий)
      - `20` — `EngineFatalError` (`bCErrorAdmin::CallFatalError`)
      - `21` — `UnhandledException` (перехвачен Win32 crash)
    - **Защиты:**
      - Re-entrancy guard (нельзя запустить новую загрузку, пока `Loading` или `isLoadActive == true`).
      - Отдельные дедлайны: на выход в меню, на одну загрузку и общий `autoTestTimeoutSec`.
      - Немедленный `Aborted(LoadFailed)` при `OnLoadFinished(false, ...)`.
    - **Unit-тесты (`tests/autotest_policy`):**
      - нормальный цикл на 1, 2 и 3 загрузки (проверка точного числа `TriggerLoad` и финального `FlushAndExit(0)`);
      - отказ `LoadGame == false` на 1-й и на 2-й загрузке -> `Aborted(10)`;
      - таймаут ожидания главного меню и таймаут во время загрузки -> `Aborted(12)`;
      - недоступность критичного хука -> `Aborted(11)`;
      - проверка пауз `MenuSettling` и `PostLoadSettling` в тиках, а не во времени;
      - проверка приоритета env над INI и корректности runtime-оверрайдов.
  - **Критерий готовности (DoD):** Новый тест `tests/autotest_policy` собирается и полностью проходит.

---

### Фаза 3. Внутрипроцессный драйвер в `dinput8.dll`

- [x] **AT-06. Сделать обработку ошибок, крашей и Guru FatalError полностью неблокирующей в режиме `AutoTest`**
  - **Файлы:** `src/main.cpp`, `src/crash_handler.cpp`, `src/autotest_runner.h` (общие константы exit codes).
  - **Требования:**
    - В `OptimaInitThread` (`src/main.cpp`), сразу после `LoadConfig()`, если `cfg.autoTestEnabled`:
      - Вызвать `SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX)` (отключить системные окна WER / критических ошибок).
    - В `src/crash_handler.cpp`:
      - Убедиться, что при `autoTestEnabled` диалоги `MessageBoxW`/`MessageBoxA` гарантированно не вызываются (`showCrashDialog == false`).
      - В `HookedCallFatalError1` и `HookedCallFatalError2`: при `cfg.autoTestEnabled`, **после** `ReportEngineFatalError(...)`, записать в лог `[AutoTest] ABORT reason=engine_fatal_error module=... file=... line=...`, выполнить `FlushDiagnosticLog()` и завершить процесс через `TerminateProcess(GetCurrentProcess(), kAutoTestExitEngineFatal)` **без** вызова `s_origCallFatalError1/2` (чтобы не зависнуть на окне «Where is the guru?»).
      - В обработчике Win32-исключений при `cfg.autoTestEnabled` гарантировать запись маркера `[AutoTest] ABORT reason=unhandled_exception code=0x...` и `FlushDiagnosticLog()` до `TerminateProcess`.
  - **Критерий готовности (DoD):** Ни один перехваченный краш или `CallFatalError` в режиме `AutoTest` не показывает модальных окон; `G3O_CrashReport_*.log` и диагностический лог полностью сброшены на диск до выхода процесса.

- [x] **AT-07. Реализовать `src/autotest_runner.h` / `src/autotest_runner.cpp` и подключить к `dinput8.dll`**
  - **Файлы:** `src/autotest_runner.h`, `src/autotest_runner.cpp`, `src/savegame_profiler.h`, `src/savegame_profiler.cpp`, `src/main.cpp`, `CMakeLists.txt`.
  - **Требования:**
    - Если `!GetConfig().autoTestEnabled`, `InitializeAutoTestRunner()` сразу возвращает `true` и **не устанавливает ни одного хука** и **не создаёт watchdog-поток**.
    - При активном `AutoTest`:
      - Установить хук безопасного тика главного потока (из `AT-03`), хук детекции меню и (при `autoTestSkipIntro`) хук пропуска интро.
      - Подключить минимальный мост уведомлений из `src/savegame_profiler.cpp`: в `Hooked_HUDLoadGame` уведомлять `AutoTestOnLoadStarted()` и `AutoTestOnLoadFinished(bool succeeded, double totalSec, const char* loadProfilerPath)`.
      - Реализовать запуск загрузки слота `cfg.autoTestSlot` в главном потоке по схеме, доказанной в `AT-02` (включая корректную работу с `bCString` / объектом HUD).
      - Запустить фоновый watchdog-поток (шаг 500 мс), проверяющий дедлайны `autotest_policy.h` и страхующий случай, если главный поток завис в бесконечном цикле или мёртвой блокировке.
      - Писать единый машиночитаемый протокол в `G3O_diagnostic_*.log`:
        - `[AutoTest] START runId=... pid=... slot=... loads=... settleFrames=... timeoutSec=... build=...`
        - `[AutoTest] MENU_READY elapsedMs=...`
        - `[AutoTest] LOAD_BEGIN index=1/2 slot=...`
        - `[AutoTest] LOAD_END index=1/2 ok=1 totalSec=... loadProfiler=...`
        - `[AutoTest] DONE loadsCompleted=2/2 exitCode=0` (или `[AutoTest] ABORT reason=... exitCode=...`)
      - Записывать компактный итоговый файл `G3O\G3O_AutoTest_<RunId или PID>.json` с результатами всех загрузок, путями к созданным `G3O_LoadProfiler_*.log` и `G3O_diagnostic_*.log`, статусом и причиной завершения.
      - **Безопасное завершение процесса:**
        1. Записать `[AutoTest] DONE` / `ABORT` и `G3O_AutoTest_*.json`.
        2. Явно сбросить и закрыть все открытые логи (`FlushDiagnosticLog()`, `ShutdownProfiler()` если активен, `ShutdownDiagnosticLogger()`).
        3. Завершить процесс по стратегии из `AT-03` (гарантированное отсутствие зависаний в `DLL_PROCESS_DETACH` с DXVK).
  - **Критерий готовности (DoD):** `dinput8.dll` собирается в `Release` и `Telemetry` без ошибок/новых предупреждений; при выключенном `AutoTest` поведение DLL полностью неизменно.

---

### Фаза 4. Внешний скрипт запуска, парсер логов и сравнение с baseline

- [x] **AT-08. Написать скрипт-оркестратор `tools/run_autotest.ps1`**
  - **Файлы:** `tools/run_autotest.ps1`.
  - **Интерфейс (параметры):**
    - `-GameDir` (по умолчанию `E:\SteamLibrary\steamapps\common\Gothic 3`)
    - `-Slot` (по умолчанию `Autosave_Slot_0001`)
    - `-Loads` (по умолчанию `2`)
    - `-SettleFrames` (по умолчанию `60`)
    - `-TimeoutSec` (по умолчанию `180`)
    - `-ProcessRuns` (по умолчанию `1`; при `> 1` делает несколько независимых запусков `Gothic3.exe` для оценки разброса / медианы Load #1)
    - `-Build` / `-Deploy` / `-BuildConfig` (`Release` | `Telemetry`) — опциональная сборка и копирование `dinput8.dll` (и `dinput8.pdb`) с автоматическим бэкапом предыдущей DLL; по умолчанию тестирует уже развёрнутую DLL
    - `-SkipIntro` (default `$true`), `-DisableReShade` (default `$true`), `-LayerAudit` (default `$false`)
    - `-Baseline` (путь к эталонному `summary.json`) и `-MaxRegressionPct` (по умолчанию `10.0`)
    - `-OutDir` (по умолчанию `scratch/autotest_runs/<RunId>`)
    - `-KillExisting` (по умолчанию `$false` — не убивать чужую запущенную игру без явного флага)
    - `-DryRun` (выполнить весь preflight, проверку слота и расчёт SHA-256 без запуска `Gothic3.exe`)
  - **Поведение:**
    - **Preflight:** проверка наличия `Gothic3.exe`, `dinput8.dll`, `G3O.ini`, существования `<Slot>.g3savcpx` и `<Slot>.g3savcpxdat`, проверка что `Gothic3.exe` не запущен, проверка интерактивности сессии Windows (`[Environment]::UserInteractive` и статус desktop).
    - **Фиксация хэшей:** посчитать `SHA-256` для `dinput8.dll`, `Game.dll`, `Engine.dll`, `SharedBase.dll`, `FileSystem.dll` и сохранить метаданные билда в `summary.json`.
    - **Изолированный запуск:** сгенерировать `RunId`, запомнить время старта, запустить `Gothic3.exe` через `System.Diagnostics.Process` с `WorkingDirectory = $GameDir` и `G3O_AUTOTEST*` в `StartInfo.Environment` (не меняя `G3O.ini` и окружение системы).
    - **Watchdog:** ждать завершения процесса до `TimeoutSec + 15` с; при превышении — зафиксировать `TIMEOUT`, сохранить хвост `G3O_diagnostic_*.log`, завершить процесс `Kill($true)`.
    - **Корреляция артефактов:** собрать только файлы в `<GameDir>\G3O\`, относящиеся к данному `PID` / `RunId` (созданные после старта процесса), скопировать ссылки/копии в `OutDir` для воспроизводимости.
  - **Критерий готовности (DoD):** `-DryRun` работает без запуска игры; watchdog надёжно завершает зависший дочерний процесс; `G3O.ini` и сохранения не затрагиваются.

- [x] **AT-09. Реализовать парсер логов и evaluator `PASS/FAIL/CRASH/TIMEOUT` (`tools/autotest_parser.ps1`)**
  - **Файлы:** `tools/autotest_parser.ps1`, `tools/test_autotest_parser.ps1`.
  - **Что разбирать:**
    - `G3O_AutoTest_<RunId>.json` и строки `[AutoTest]` из `G3O_diagnostic_*.log`;
    - каждый `G3O_LoadProfiler_*_<PID>_*.log` в порядке последовательности (`_1.log`, `_2.log`, ...):
      - `TotalSec` (`DETAILED SAVEGAME LOAD BENCHMARK (Total: X.XXX s)`)
      - `PreLoadSec`, `WorldActivateSec`, `SectorCount`, `SysDynMs` (из `Top Slowest Sectors` / `[SectorAudit]`)
      - `LoadGameWorldSec`
      - `GotoLoadedPosSec`, `GotoPakDecompSec`, `GotoReadFileSec`, `GotoSceneSetupSec`
      - `SessionStartSec`
      - `ReadFileTotalSec`, `ReadFileMB`, `ReadFileSpeedMBs`, `PakBlocks`, `PakOutMB`, `SaveChunks`
    - аномалии в `G3O_diagnostic_*.log` и наличие `G3O_CrashReport_*_<PID>_*.log`:
      - краши, `FatalError`, `hook creation failed` / `hook enable failed`, `FAILED hr=`, ошибки `libdeflate` / fallback, срабатывания `CrashGuards`.
  - **Правила классификации исхода:**
    - `CRASH` (exit `2`): найден `G3O_CrashReport` для данного `PID`, либо exit code `20`/`21`/Win32 exception code (`0xC0000005` и т. п.). Вывести модуль-виновник, RVA, тип ошибки и путь к отчёту/дампу.
    - `TIMEOUT` (exit `3`): процесс убит watchdog-ом или вышел с кодом `12`. Вывести последнюю фазу `[Phase]` и последние 20 строк диагностического лога.
    - `FAIL` (exit `1`): нет маркера `AUTOTEST_DONE`, `loadsCompleted < expectedLoads`, exit code `10`/`11`, критические ошибки хуков/инвариантов, либо регрессия относительно `-Baseline` выше `-MaxRegressionPct` (при сравнении Load #1 сравнивается с Load #1, а Load #2+ — с Load #2+!).
    - `PASS` (exit `0`): все проверки пройдены.
  - **Вывод:** структурированный `summary.json` + лаконичная сводная таблица в stdout (время каждой загрузки, ключевые фазы, `SysDyn`, `GotoLoadedPos`, ошибки, diff с baseline, вердикт).
  - **Критерий готовности (DoD):** `tools/test_autotest_parser.ps1` прогоняет синтетические/фикстурные логи всех 4 исходов (`PASS`, `FAIL`, `CRASH`, `TIMEOUT`) и сценарий сравнения с baseline, и все проверки проходят.

---

### Фаза 5. Верификация, пробные прогоны и регламент

- [x] **AT-10. Провести полную офлайн-верификацию (без запуска игры)**
  - Собрать `dinput8.dll` (`Release` и `Telemetry`).
  - Прогнать C++ unit-тесты:
    - `tests/autotest_policy`
    - `tests/config_ini`
    - `tests/diagnostic_context`
    - `tests/savegame_profiler_policy`
  - Прогнать `tools/test_autotest_parser.ps1`.
  - Прогнать `tools/run_autotest.ps1 -DryRun` против `E:\SteamLibrary\steamapps\common\Gothic 3` и проверить `SHA-256`, проверку слота и формат вывода.
  - **Критерий готовности (DoD):** Все сборки и офлайн-тесты зелёные до первого реального старта `Gothic3.exe`.

- [x] **AT-11. Провести серию реальных проверочных прогонов (по разрешению пользователя)**
  - **Сценарий 1 (Smoke, `-Loads 1`):** старт -> пропуск интро -> меню -> 1 загрузка `Autosave_Slot_0001` -> `SettleFrames` -> чистый выход -> проверка `PASS`, `G3O_LoadProfiler_*.log`, `summary.json`.
  - **Сценарий 2 (Пара «первая / повтор», `-Loads 2` и `-Loads 3`):** проверить стабильность повторной загрузки в том же процессе, отсутствие зависаний при выходе под DXVK, корректное разделение метрик Load #1 и Load #2..3.
  - **Сценарий 3 (Негативный, `-Slot __Missing_Slot__`):** проверить быстрый детерминированный выход с кодом `10` (`FAIL`), без зависания до таймаута и без модальных окон.
  - **Сценарий 4 (Регрессия обычного запуска):** запустить игру штатно без `G3O_AUTOTEST` и убедиться, что `AutoTest` не активируется, `G3O.ini` не изменён, `WindowCursorLock` и `ShowCrashDialog` работают как обычно.
  - **Сценарий 5 (Эталонный baseline текущей сборки без кэшей):** снять чистый базовый `summary.json` с привязанным `SHA-256` текущей `dinput8.dll` (одновременно закрывает нехватку привязанного профиля для п. 8.1 из `context.txt`).
  - **Критерий готовности (DoD):** Все 5 сценариев подтверждены сохранёнными логами и отчётами.

- [x] **AT-12. Зафиксировать регламент автономного цикла и гигиену репозитория**
  - Проверить `.gitignore` (убедиться, что `scratch/` / `scratch/autotest_runs/` игнорируются и не попадают в коммиты).
  - Оформить краткую инструкцию `docs/AUTOTEST.md`:
    - команды запуска (smoke, 2-load pair, A/B-сравнение с baseline, `-LayerAudit`, `-ProcessRuns 3`);
    - правило получения/действия разрешения пользователя на запуск игры агентом;
    - известные ограничения (нужна активная графическая сессия Windows; шум от Standby List Windows при сравнении Load #1 между запусками; не трогать мышь/окно во время прогона или использовать отдельный запуск без ReShade).
  - **Критерий готовности (DoD):** Цикл `правка -> тесты -> сборка -> tools/run_autotest.ps1 -Deploy -> summary.json` полностью документирован и готов к регулярной работе.

---

## 4. Граф зависимостей задач

```text
AT-01 (аудит среды) ─────────────────────────┐
                                             ▼
AT-02 (анализ LoadGame) ─► AT-03 (тик/меню/выход) ─► AT-07 (autotest_runner.cpp) ─┐
                                                                                  ├─► AT-10 (офлайн-тесты) ─► AT-11 (живые прогоны) ─► AT-12 (регламент)
AT-04 (env+INI config)  ─► AT-05 (autotest_policy) ─► AT-06 (неблок. краши) ──────┤
                                                                                  │
                           AT-08 (run_autotest.ps1) ─► AT-09 (парсер + тесты) ────┘
```

---

## 5. Ключевые риски и меры снижения

| Риск | Влияние | Мера снижения |
| :--- | :--- | :--- |
| Вызов `gCHUDFileManager::LoadGame` из неподходящего состояния меню или рендер-пасса `Present` | Краш / порча состояния D3D9/DXVK | Статический разбор в `AT-02`/`AT-03`; вызов строго в безопасной точке главного цикла, не из `Present`; запасной вариант через более высокий контроллер загрузки. |
| Дедлок в `DllMain(DLL_PROCESS_DETACH)` при `ExitProcess` под DXVK или рабочими потоками | Зависание процесса на выходе до таймаута | Сначала запись `AUTOTEST_DONE` + `flush`/`fclose` всех логов, затем контролируемый выход (`TerminateProcess` после закрытия файлов или `ExitProcess` с коротким watchdog). |
| Модальные окна при ошибке (`MessageBox`, WER, «Where is the guru?») | Потеря времени до `TimeoutSec`, нет быстрого `CRASH` | `SetErrorMode`, принудительный `showCrashDialog=false`, перехват `HookedCallFatalError1/2` без проброса в оригинальный GUI-диалог в режиме `AutoTest`. |
| «Грязный» `G3O.ini` после падения или прерывания скрипта | Автотест сработает при обычной игре пользователя | Управление через `G3O_AUTOTEST*` env-переменные дочернего процесса; `G3O.ini` скриптом вообще не редактируется. |
| Шум от файлового кэша Windows (Standby List) в замерах | Ложные выводы об ускорении/замедлении | Раздельный учёт `Load #1` vs `Load #2..N`, фиксация `SHA-256` всех модулей, поддержка `-ProcessRuns M` для медианных сравнений. |
| Заблокированный экран / отсутствие интерактивного рабочего стола / не запущен Steam | Игра не инициализирует D3D9 или не стартует | Preflight-проверки в `AT-01` и `AT-08`, понятная диагностика причины отказа до запуска. |
