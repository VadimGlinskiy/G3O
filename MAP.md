# G3O: Save-loading optimization

## Goal
Цель — измеримо сократить загрузку сохранения Gothic 3; общий приоритет G3O — реальный прирост FPS и скорости загрузки. Профилирование и анализ исходников нужны только для выбора конкретной оптимизации: после локализации безопасного кандидата прекратить углубление, внести одно изменение и сравнить одинаковый сценарий до/после. Не объявлять выигрыш без контролируемого прогона; текущие вложенные таймеры inclusive.

## Latest runtime evidence

Семь прогонов `Autosave_Slot_0001`, 2026-11-07:

| Run | Total load | `gCWorld::Activate` | `SysDyn` sector | `ContextRead` | `ReadV83` |
|---|---:|---:|---:|---:|---:|
| 14:13 | 29.057 s | 11.064 s | 6,872.31 ms | 6,781.63 ms | 5,859.30 ms / 25,489 calls |
| 14:17 | 28.856 s | 10.830 s | 6,733.40 ms | 6,642.06 ms | 5,729.98 ms / 25,489 calls |
| 15:19 | 29.521 s | 11.231 s | 6,974.55 ms | 6,811.78 ms | 5,887.08 ms / 25,489 calls |
| 16:23 (cache off) | 28.442 s | 11.178 s | 6,890.78 ms | 6,792.38 ms | 5,879.20 ms / 25,489 calls |
| 16:34 (cache on) | 29.243 s | 10.660 s | 6,624.16 ms | 6,514.98 ms | 5,607.18 ms / 25,489 calls |
| 18:09 (cache on, 6 prefetch workers) | 29.048 s | 10.727 s | 6,659.04 ms | 6,551.75 ms | 5,636.83 ms / 25,489 calls |
| 18:22 (cache on, 8 prefetch workers) | 29.063 s | 10.699 s | 6,710.37 ms | 6,613.19 ms | 5,688.76 ms / 25,489 calls |

В прогоне 16:23 `ReadV63` — 0; `eCNode::Read` — 12.71 ms / 25,489; `AccessorPropertyStreamRead` — 12,682.72 ms / 346,402 inclusive, 6,792.39 ms exclusive, max depth 3, nested 346,401, unavailable 0. Exclusive здесь исключает только вложенные accessor-вызовы, но включает работу по чтению объектов; это не накладные расходы wrapper. `ReadNewObject` — 5,793.86 ms / 207,724 inclusive; factory — 5,735.43 ms. `ReadV83` — 5,879.20 ms; `PatchWithTemplate` — 841.05 ms. Inclusive-таймеры разных функций не складывать; разницу с предыдущими прогонами нельзя считать ускорением без A/B.

`PropertyReadDispatch`: 31 цель, без overflow/unresolved. Root target `012DE5B0` = `Engine.dll+0x21E5B0` — 6,792.392 ms / 1 вызов, parent-inclusive; остальные 30 целей суммарно 65.075 ms / 9,465 calls. Эта карта покрывает `ReadObject`, не виртуальные цели ветви `ReadNewObject`. Все 18 hooks зарегистрировались; shutdown чистый. Новый capture: `G3O_diagnostic_20261107_162255_431_20044_23176_1.log`, `G3O_LoadProfiler_20261107_162359_750_20044_4588_1.log`, `G3O_profile_20261107_162258_644_20044_23176_1.log`.

## Static trace

`Engine.dll` `eCEntity::ReadV83` (RVA `0x225F40`) вызывает `eCNode::Read` в `0x10225F52` (экспорт RVA `0x20DD50`), затем в цикле свойств (`0x10226215` → back-edge `0x10226356`) — `SharedBase.dll` `operator>>(bCIStream&, bCAccessorPropertyObject&)` по RVA `0x63810` (`0x10226236` callsite; sampler candidate `+0x6381F` лишь подсказка). Ghidra: wrapper `0x10063810` вызывает `bCAccessorPropertyObject::Read` (`0x10063670`), далее ветвится на `ReadObject`/`ReadNewObject`, а factory вызывает virtual `Read` по slot 5. Предыдущий runtime capture подтвердил обе ветви и nesting depth 3, но не разделил targets `ReadNewObject`; новая DLL собирает раздельные target maps. `Engine.dll+0x3039B8` пока не идентифицирован.

SDK template trace: `bTPropertyObject::Read` переходит в `ReadData`, который читает список свойств. При совпадении сохранённого порядка идёт по accessor-итератору; после первого несовпадения оставшиеся имена разрешаются через `bCAccessorProperty`. Подходящие свойства идут в `PropertyRead`; неизвестные/несовпадающие — в `PropertyDeprecate` под критической секцией. Распределение fast/fallback ветвей `ReadData` и отдельное время `PropertyRead`/`PropertyDeprecate` текущими хуками не измеряются.

В shipped `SharedBase.dll` factory `ReadObject`/`ReadNewObject` читают заголовок и виртуально вызывают `Read` по slot `+0x14`; `ReadNewObject` напрямую вызывает экспорт `CloneObjectInstance` из `SharedBase.dll+0x5937C` (RVA функции `0x59000`) перед чтением. Новая инструментировка разделяет virtual targets по factory-пути, измеряет inclusive-время clone и снимает hits/misses property-cache строго в окне `OnLoadDynamicContext` сектора `SysDyn`. Частоты fast/fallback ветвей `ReadData` пока не измеряются. Перед factory находится hash-table lookup по строковому ключу (Ghidra `FUN_10057FB0`, интерпретация как поиск типа следует из caller/data flow).

## Instrumentation and deployment

Добавлены per-context метрики `ReadV83`/`ReadV63`, `eCNode::Read`, обе ветви accessor, nesting/exclusive ticks; отдельные таблицы virtual `Read` для `ReadObject` и `ReadNewObject`; inclusive-таймер `CloneObjectInstance`; hits/misses string/ID property-cache только внутри `OnLoadDynamicContext` сектора `SysDyn` (load thread). Для `GotoLoadedPosition` добавлены отдельные thread-local QPC-метрики `eCPVSPrefetcher3::Process` и `WaitForEmptyThreadQueue`; они inclusive и не складываются. Release build и focused test прошли.

Установлена диагностическая DLL SHA-256 `41c1d9c1d821d9499506577126ffffc8914a9d59cd601178f9515348ba1f14ad`; совпадает с Release-артефактом. Предыдущая DLL (`b61c1b682e0ec67e5dada09b3eae170788493774126f6cb296b1cedb8a87e2a3`) сохранена в `build/deployment-backups/dinput8.dll.pre-pvs-direct-timing.20261107_193236_202010.bak`. INI не менялся: `PakPrefetchWorkers=0`, `EnablePropertyCache=true`, бюджет 192 MB.

В прогоне 16:34 property cache был активен: 1,065,186 hits / 1,065,280 resolves (94 misses). Относительно прогона 16:23 `SysDyn` ниже на 266.62 ms (3.87%), но общий load дольше на 0.801 s: post-load вырос на 1.301 s. Это один прогон на настройку, поэтому ускорение total load не подтверждено. Диагностический лог завершился штатно.

Прогон 18:09: общий load 29.048 s; `SysDyn` 6,659.04 ms / `ReadV83` 5,636.83 ms; `CloneObjectInstance` 785.76 ms / 556,166 calls (inclusive). `GotoLoadedPos` — 12.729 s, включая `Scene & Entity Setup` 7.667 s; PAK decompression 3.566 s. Property cache: 1,065,186/1,065,280 hits; PAK prefetch: 6 workers, 192 MB budget, 2,260 regular candidates. Cache-on run всё ещё не показывает доказанного улучшения общего load; post-load варьируется. Профиль этого запуска не содержит gameplay FPS (во время capture FPS=0).

Прогон 18:22 с 8 PAK workers: profiler total 29.063 s, `GotoLoadedPos` 12.806 s, `SysDyn` 6,710.37 ms; 3.649 s PAK decompression в `GotoLoadedPos`, 7.355 s residual `Scene & Entity Setup`. Пользовательский секундомер показал 28.69 s; это не тот же временной интервал, что profiler. Сравнение профайлера с 6-worker capture 18:09: +15 ms по total — улучшения нет; `PakPrefetchWorkers` возвращён в auto (`0`), cache и budget прежние.

После одно-проходного hash-изменения два capture: 29.458 s / `SysDyn` 6,720.08 ms и 29.557 s / 6,813.37 ms. База 18:09 со старой DLL: 29.048 s / 6,659.04 ms. Прироста не видно; одно-проходное изменение удалено, установлена DLL из текущей Release-сборки SHA-256 `b61c1b682e0ec67e5dada09b3eae170788493774126f6cb296b1cedb8a87e2a3`.

## Next step

Следующий шаг: пользователь запускает тот же сейв один раз на установленной DLL; по прямым `PVS Process`/queue-wait замерам решаем, есть ли там существенная доля остатка `Scene & Entity Setup` (~7.3–7.7 s). Затем вносим одну безопасную оптимизацию и проверяем тем же сейвом; улучшение загрузки/FPS пока не подтверждено.
