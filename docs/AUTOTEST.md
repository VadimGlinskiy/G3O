# Gothic 3 Optima (G3O) — Автоматизированный тестовый контур (`AutoTest`)

Система позволяет запускать `Gothic3.exe` дочерним процессом, автоматически пропускать интро, выполнять `N` загрузок указанного сохранения, собирать профили/диагностику/краш-репорты и получать машиночитаемый вердикт `PASS / FAIL / CRASH / TIMEOUT` без ручного участия.

---

## Быстрый старт

```powershell
# 1. Проверка окружения, слота и SHA-256 без запуска игры
powershell -ExecutionPolicy Bypass -File tools/run_autotest.ps1 -DryRun

# 2. Сборка Release, деплой dinput8.dll (с бэкапом) и прогон пары «первая / повтор»
powershell -ExecutionPolicy Bypass -File tools/run_autotest.ps1 -Build -Deploy -Loads 2

# 3. Прогон уже развёрнутой DLL (1 загрузка, smoke-тест)
powershell -ExecutionPolicy Bypass -File tools/run_autotest.ps1 -Loads 1

# 4. Сравнение с базовым прогоном (baseline)
powershell -ExecutionPolicy Bypass -File tools/run_autotest.ps1 -Loads 2 -Baseline scratch/autotest_runs/<baseline_id>/summary.json -MaxRegressionPct 10

# 5. Глубокий замер слоёв (LayerAudit) или несколько независимых запусков процесса
powershell -ExecutionPolicy Bypass -File tools/run_autotest.ps1 -Loads 2 -LayerAudit:$true -ProcessRuns 3
```

---

## Как это работает

- **Управление через переменные окружения дочернего процесса** (`G3O_AUTOTEST=1`, `G3O_AUTOTEST_SLOT`, `G3O_AUTOTEST_LOADS`, `G3O_AUTOTEST_SETTLE_FRAMES`, `G3O_AUTOTEST_TIMEOUT_SEC`, `G3O_AUTOTEST_SKIP_INTRO`, `G3O_AUTOTEST_DISABLE_RESHADE`, `G3O_AUTOTEST_LAYER_AUDIT`, `G3O_AUTOTEST_RUN_ID`).
- **Пользовательский `G3O.ini` и файлы сохранений не модифицируются.** При обычном запуске игры (`G3O_AUTOTEST` не задан) хуки автотеста не устанавливаются.
- Внутри `dinput8.dll` при `G3O_AUTOTEST=1` автоматически применяются безопасные оверрайды в памяти:
  - `EnableDiagnosticLog=true`, `EnableSavegameAudit=true`, `EnableCrashInvestigator=true`, `WriteCrashLog=true`
  - `ShowCrashDialog=false`, `WindowCursorLock=false`, `SetErrorMode(...)` (никаких модальных окон и зависаний на «Where is the guru?»)
  - `LoadReShade=false` (если `-DisableReShade:$true`, по умолчанию включено).

---

## Коды возврата и артефакты

- **`0` (`PASS`)**: все `N` загрузок завершились успешно, найден маркер `[AutoTest] DONE`, нет крашей/ошибок хуков, нет регрессии относительно `-Baseline`.
- **`1` (`FAIL`)**: провал загрузки (например, несуществующий слот), несовпадение числа загрузок, ошибки хуков или регрессия выше `-MaxRegressionPct`.
- **`2` (`CRASH`)**: найден `G3O_CrashReport_*_<PID>_*.log` или необработанное исключение / `CallFatalError`.
- **`3` (`TIMEOUT`)**: процесс завершён сторожевым таймером (в сводку выводится последняя фаза и хвост диагностического лога).

Каждый прогон сохраняет структурированный `summary.json` в `scratch/autotest_runs/<RunId>/` (папка `scratch/` не коммитится) с:
- `SHA-256` для `Gothic3.exe`, `dinput8.dll`, `Game.dll`, `Engine.dll`, `SharedBase.dll`, `FileSystem.dll`;
- раздельными метриками для каждой загрузки (`Load #1`, `Load #2`, ...);
- счётчиками аномалий и путями ко всем сырым логам в `<GameDir>\G3O\`.

---

## Офлайн-тесты (без запуска игры)

```powershell
# Парсер логов и evaluator (PASS / FAIL / CRASH / TIMEOUT)
powershell -ExecutionPolicy Bypass -File tools/test_autotest_parser.ps1
```

C++ unit-тесты (Win32 Release): `tests/autotest_policy`, `tests/config_ini`, `tests/diagnostic_context`, `tests/savegame_profiler_policy`.

---

## Ограничения и правила

- Требуется активная интерактивная графическая сессия Windows (D3D9 не инициализируется в Session 0 или при заблокированном экране).
- При сравнении замеров учитывайте кэш ОС (Windows Standby List): `Load #1` (первая загрузка, включает `gCWorld::Activate` + `SysDyn`) сравнивается только с `Load #1`, а `Load #2..N` — с `Load #2..N`.
- Реальный запуск `Gothic3.exe` агентом выполняется только при наличии разрешения пользователя.
