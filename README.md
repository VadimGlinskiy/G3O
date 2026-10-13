<div align="center">

[English](README_en.md) · **Русский**

# Gothic 3 Optima (G3O) Тестовая версия

### Библиотека оптимизации движка, интеграция DXVK и защита сохранений для Gothic 3

[![Gothic 3 Version](https://img.shields.io/badge/Gothic%203-v1.75.14%20%2B%20UP%201.04.11-gold.svg?style=flat-square)](file:///E:/SteamLibrary/steamapps/common/Gothic%203)
[![DXVK](https://img.shields.io/badge/Vulkan-DXVK%201.10%20%2F%202.4%20%2F%203.0-red.svg?style=flat-square)](https://github.com/doitsujin/dxvk)
[![Memory Allocator](https://img.shields.io/badge/Allocator-mimalloc%20v3.5.3-blue.svg?style=flat-square)](https://github.com/microsoft/mimalloc)
[![Decompression](https://img.shields.io/badge/Decompressor-libdeflate%20AVX2-green.svg?style=flat-square)](https://github.com/ebiggers/libdeflate)
[![Compiler](https://img.shields.io/badge/Compiler-MSVC%20v143%20(VS%202022)-orange.svg?style=flat-square)](CMakeLists.txt)
[![License](https://img.shields.io/badge/License-MIT-lightgrey.svg?style=flat-square)](LICENSE)

<br />

**[Установка](#установка) · [Архивы релиза](#архивы-релиза) · [Таблица изменений](#таблица-изменений) · [Защита сохранений](#защита-сохранений) · [Сборка](#сборка) · [FAQ](#faq)**

<br />

</div>

---

## О проекте

**Gothic 3 Optima (G3O)** - нативная DLL-библиотека (`dinput8.dll`), которая загружается вместе с `Gothic3.exe` и перехватывает вызовы в библиотеках движка Genome (`Engine.dll`, `Game.dll`, `SharedBase.dll`, `FileSystem.dll`, `PhysXCore.dll`).

Проект решает основные технические проблемы оригинальной игры:
1. **Фризы при перемещении по миру** - за счет многопоточной подгрузки архивов и быстрой декомпрессии через libdeflate.
2. **Вылеты с ошибкой "Where is the guru?"** - старый менеджер кучи заменен на mimalloc, включен LFH и устранены утечки адресного пространства x86.
3. **Повреждение сохранений (файлы 0 байт при сбое)** - запись теперь идет во временный файл с последующей атомарной подменой и бэкапом.
4. **Упор в один поток CPU при рендеринге Direct3D 9** - фильтрация повторяющихся RenderState и трансляция в Vulkan через DXVK.

---

## Установка

Релиз разделен на **основное ядро** и **пакеты DXVK** под разные поколения видеокарт:

```mermaid
graph TD
    subgraph GitHub_Release ["GitHub Release (все файлы на одной странице)"]
        Core["Gothic3Optima-v1.0.0-Core.zip<br/>(dinput8.dll: аллокатор, сейвы, SIMD, потоки)"]
        DXVK1["DXVK-2.4.1-Stable.zip<br/>(Рекомендуется для NVIDIA / AMD)"]
        DXVK2["DXVK-3.0.1.zip<br/>(Для Vulkan 1.4)"]
        DXVK3["DXVK-3.0.1-HDR.zip<br/>(Для мониторов с HDR)"]
        DXVK4["DXVK-1.10.3-Async.zip<br/>(Для старых видеокарт)"]
        DXVK5["DXVK-1.10.1-Async.zip<br/>(Для Intel HD Graphics)"]
    end

    subgraph Game_Folder ["Папка с игрой: Gothic 3/"]
        EXE["Gothic3.exe"]
        DLL["dinput8.dll"]
        D3D9["d3d9.dll"]
        DXGI["dxgi.dll"]
        CONF["dxvk.conf"]
    end

    Core -->|"1. Распаковать"| DLL
    DXVK1 -.->|"2. Распаковать один из пяти"| D3D9
    DXVK1 -.-> DXGI
    DXVK1 -.-> CONF
```

### Порядок установки:

1. **Скачайте ядро:**
   - В релизах скачайте архив `Gothic3Optima-v1.0.0-Core.zip`.
   - Скопируйте `dinput8.dll` в папку игры (туда, где лежит `Gothic3.exe`).
2. **Скачайте подходящий DXVK:**
   - На той же странице выберите архив под вашу видеокарту (список ниже).
   - Скопируйте файлы из архива (`d3d9.dll`, `dxgi.dll`, `dxvk.conf`) в папку игры.
3. **Запустите игру** как обычно через Steam, GOG или `Gothic3.exe`.

---

## Архивы релиза

Все архивы прикреплены к одному релизу на GitHub:

### 1. Основное ядро (обязательно)
| Файл | Размер | Описание |
| :--- | :--- | :--- |
| **`Gothic3Optima-v1.0.0-Core.zip`** | ~80 КБ | Ядро мода (`dinput8.dll`). Содержит замену аллокатора на mimalloc, SaveGame Guard, SIMD-инструкции для геометрии, расширенный пул потоков и перехватчик сбоев. Работает как с нативным D3D9, так и с DXVK. |

### 2. Версии DXVK (выберите одну)
| Файл | Версия | Рекомендации |
| :--- | :--- | :--- |
| **`DXVK-2.4.1-Stable.zip`** | DXVK 2.4.1 Stable | **Рекомендуется по умолчанию.** Оптимальный вариант для большинства видеокарт: AMD Radeon (Polaris, Vega, RDNA 1-3) и NVIDIA GeForce (GTX 10xx, RTX 20xx/30xx/40xx). |
| **`DXVK-3.0.1.zip`** | DXVK 3.0.1 | Актуальная версия. Требует видеокарту и драйвер с полной поддержкой Vulkan 1.4. |
| **`DXVK-3.0.1-HDR.zip`** | DXVK 3.0.1 HDR | Сборка с выводом в HDR. Используйте, если играете на мониторе или ТВ с HDR при включенном режиме Windows HDR. |
| **`DXVK-1.10.3-Async.zip`** | DXVK 1.10.3 Async | Для видеокарт без поддержки Vulkan 1.3 или если версии 2.x вызывают падение драйвера. |
| **`DXVK-1.10.1-Async.zip`** | DXVK 1.10.1 Async | Для встроенной графики Intel (HD / UHD Graphics) или старых видеокарт. |

> [!TIP]
> Для стандартной системы скачивайте пару: **`Gothic3Optima-v1.0.0-Core.zip`** + **`DXVK-2.4.1-Stable.zip`**.

---

## Таблица изменений

В таблице описаны технические проблемы оригинальной игры (CP 1.75.14) и их решения в моде:

| Подсистема | Поведение оригинала | Симптом | Решение в моде | Результат |
| :--- | :--- | :--- | :--- | :--- |
| **Сохранения** | Перезапись файла напрямую (`0x1019c6b0`), размер сбрасывается в 0 при открытии | Сейв 0 байт при вылете во время F5, ошибка "Where is the guru?" | **SaveGame Guard:** сброс кэша памяти, запись в `.tmp`, проверка размера и атомарный `ReplaceFileW` | Сохранение не повредится даже при падении игры или выключении ПК |
| **Память** | Менеджер `bCMemoryAdmin` со спинлоком `MemHeap::s_cLock` + стандартный `HeapAlloc` | Фрагментация кучи, статтеры из-за локов, вылет OOM при ~3 ГБ | Замена на **mimalloc v3.5.3 (x86)** + включение LFH для всех куч | Нет блокировок потоков, куча не фрагментируется, экономия до 400 МБ памяти |
| **Графика** | D3D9 без флага `D3DCREATE_PUREDEVICE`, текстуры дублируются в системной RAM | Высокий расход RAM, нагрузка на CPU валидацией вызовов в D3D9 | **DXVK** + флаг `D3DCREATE_PUREDEVICE` в `CreateDevice` | Текстуры перенесены в VRAM, освобождается до 1 ГБ системной памяти |
| **Состояния D3D9** | Движок шлет тысячи одинаковых `SetRenderState` и `SetSamplerState` за кадр | Процессорный боттлнек по числу draw calls | **State Filter:** теневая таблица состояний отсекает повторные вызовы | Меньше лишних вызовов драйвера, стабильнее минимальный фреймрейт |
| **Потоки загрузки** | Пул `eCThreadPool` (`0x101f4010`) использует всего 1-2 воркера | Замирание картинки при входе в новый сектор мира | Перехват `eCThreadPool::Create` с расширением пула до 4-8 потоков | Параллельная подгрузка и распаковка данных на многоядерных процессорах |
| **Кэш ресурсов** | Агрессивный сброс ресурсов в `eCBaseCache` выгружает недавние ассеты | Статтер при резком развороте камеры назад | Блокировка выгрузки ассетов при наличии свободной памяти | Нет повторной загрузки при беге туда-обратно по одной локации |
| **Декомпрессия** | Скалярный `zlib 1.2.5` (`0x100153dc` в `FileSystem.dll`) | Нагрузка на CPU при распаковке `.pak` контейнеров | Интеграция **libdeflate** с инструкциями AVX2 / SSE4.2 | Распаковка архивов в 2.5-4 раза быстрее |
| **Файловый ввод-вывод** | Вызовы `SetFilePointer` и `ReadFile` с небольшим буфером | Задержки дискового ввода-вывода, лишние переходы в ядро ОС | **Memory-Mapped Files (`MapViewOfFile`):** прямое проецирование архивов | Чтение данных из памяти без ожидания диска |
| **Геометрия и физика** | Вычисления через x87 FPU (`fld`, `fmul`, `fstp`) в `SharedBase.dll` | Медленный Frustum Culling и расчет коллизий | Перевод функций отсечения и матриц на векторные инструкции SSE | Ускорение расчетов видимости объектов |
| **Хэш строк** | Побайтовый цикл 2005 года в `g_GetHashValue<bCString>` | Задержки поиска объектов в таблицах при спавне NPC | Векторизованный алгоритм хэширования строк | Быстрый поиск сущностей и диалогов |
| **Планировщик CPU** | Потоки могут попадать на энергоэффективные E-ядра | Просадки FPS на процессорах Intel 12-14th Gen и AMD Hybrid | Привязка основных потоков к производительным P-ядрам | Стабильная частота кадров на гибридных CPU |
| **Физика PhysX** | PhysX 2.4.1 ограничен 2 потоками, опрос отсутствующей платы Ageia PPU | Просадки кадров в битвах с рэгдоллами | Перехват создания SDK + выделение памяти через mimalloc | Плавная физика в массовых боях |
| **Таймер кадров** | Стандартный системный таймер Windows с шагом 15.6 мс | Неровный график времени кадра при 60 / 120 FPS | Включение `timeBeginPeriod(1)` для шага таймера 1.0 мс | Ровная подача кадров (Frame Pacing) |
| **Звук FMOD** | Размер DSP-буфера в `fmodex.dll` всего 512 сэмплов | Щелчки и заикания звука при скачках нагрузки | Перехват `FMOD_System_Init` с буфером 1024 сэмпла на 4 буфера | Чистый звук без треска при подгрузке локаций |
| **Адресное пространство** | Ограничение 4 ГБ для 32-битного процесса | Нехватка места под кэш всего игрового мира | Модуль **Heaven's Gate (`heavens_gate/`):** переход в x64 режим для аллокации буферов | Возможность держать большие объемы кэша за пределами 4 ГБ |

---

## Защита сохранений

Схема работы SaveGame Guard:

```mermaid
sequenceDiagram
    autonumber
    actor Player as Игрок (F5 / QuickSave)
    participant Game as Game.dll
    participant Guard as SaveGame Guard
    participant Memory as mimalloc
    participant Disk as Диск

    Player->>Game: Нажатие F5
    Game->>Guard: Перехват сохранения (0x1019c6b0)
    Guard->>Guard: Проверка свободной памяти
    alt Свободно < 350 МБ
        Guard->>Memory: Сброс страниц и кэша
    end
    Guard->>Disk: Запись во временный файл .tmp
    Game->>Disk: Сериализация состояния мира
    Game->>Guard: Закрытие файла (CloseHandle)
    Guard->>Disk: Сброс буферов на диск (FlushFileBuffers)
    Guard->>Guard: Проверка размера файла (>256 байт)
    alt Ошибка записи (файл 0 байт)
        Guard->>Disk: Удаление битого .tmp файла
        Guard-->>Player: Сохранение отменено, исходный сейв не тронут
    else Файл в порядке
        Guard->>Disk: Создание бэкапа (.bak)
        Guard->>Disk: Атомарная замена через ReplaceFileW
        Guard-->>Player: Игра сохранена
    end
```

---

## Структура модулей

- `heavens_gate/` - переход из 32-битного режима в 64-битный через селектор сегмента (`retf 0x33`) для выделения памяти за пределами 4 ГБ через 64-битную `ntdll.dll`.
- `src/memory_allocator.cpp` и `src/lfh_enforcer.cpp` - замена стандартных функций выделения памяти `bCMemoryAdmin` на mimalloc, принудительное включение LFH для всех куч процесса.
- `src/d3d9_hook.cpp` - добавление флага `D3DCREATE_PUREDEVICE`, теневой кэш состояний рендера и фильтрация повторяющихся вызовов D3D9.
- `src/streaming_optimizer.cpp` - проецирование архивов `.pak` в память (`MapViewOfFile`) и расширение пула потоков `eCThreadPool` до 4-8 воркеров.
- `src/media_optimizer.cpp` - декомпрессия через `libdeflate` вместо встроенного zlib, увеличение буфера FMOD и точность таймера 1 мс.
- `src/math_simd_optimizer.cpp` - перенос отсечения геометрии `bCFrustum::Cull` и операций с матрицами на SSE-инструкции.
- `src/savegame_guard.cpp` - запись сейва в `.tmp`, валидация размера, создание копии `.bak` и атомарная подмена через `ReplaceFileW`.

---

## Сборка

### Требования:
- Windows 10 / 11 (x64)
- Visual Studio 2022 (компоненты C++ x86)
- CMake 3.20+
- Git

### Команды:

```cmd
git clone https://github.com/VadimGlinskiy/G3O.git
cd G3O

cmake -B build -S . -A Win32 -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Собранная библиотека: `build/Release/dinput8.dll`.

Для сборки релизных архивов запустите:
```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\package_releases.ps1
```
Архивы появятся в каталоге `dist/packages/`.

---

## FAQ

<details>
<summary><strong>Совместим ли мод с Community Patch 1.75.14 и Update Pack 1.04.11?</strong></summary>

Да. Мод писался и тестировался на чистой Gothic 3 v1.75.14 с установленным Update Pack v1.04.11. Все адреса и структуры совпадают.
</details>

<details>
<summary><strong>Работает ли с модами (Questpaket, Content Mod, LCTM)?</strong></summary>

Да. Мод меняет только бинарный код движка и системные вызовы. Шаблоны предметов, скрипты квестов и баланс не затрагиваются.
</details>

<details>
<summary><strong>Как включить оверлей DXVK (FPS, память)?</strong></summary>

Откройте `dxvk.conf` в папке игры и добавьте строку:
```ini
dxvk.hud = fps,frametimes,memory,devinfo
```
</details>

<details>
<summary><strong>Что делать при черном экране или ошибке Vulkan?</strong></summary>

Скачайте архив `DXVK-1.10.1-Async.zip` или `DXVK-1.10.3-Async.zip` и распакуйте в папку игры. У них максимальная совместимость со старыми видеокартами и драйверами.
</details>

<details>
<summary><strong>Где искать бэкапы сохранений?</strong></summary>

В вашей обычной папке сохранений (`Документы\gothic3\`). При каждой перезаписи старый файл сохраняется с расширением `.bak`. Если нужно вернуть его, просто переименуйте в `.g3savdat`.
</details>

---

## Благодарности и сторонние компоненты

Проект Gothic 3 Optima использует и опирается на наработки следующих открытых библиотек, авторов и исследовательских проектов:

| Компонент / Проект | Автор / Организация | Лицензия | Назначение в Gothic 3 Optima |
| :--- | :--- | :--- | :--- |
| **[DXVK](https://github.com/doitsujin/dxvk)** | Philip Rebohle ([doitsujin](https://github.com/doitsujin)) и участники проекта | Zlib License | Трансляция вызовов Direct3D 9 в нативный Vulkan, перенос создания текстур и компиляции шейдеров в фоновые Vulkan Transfer Queues, поддержка 32-битного плавающего буфера глубины. |
| **[DXVK-Async](https://github.com/Sporif/dxvk-async)** | [Sporif](https://github.com/Sporif) | Zlib License | Патч асинхронной компиляции графических конвейеров для классических версий DXVK 1.10.x. |
| **[mimalloc](https://github.com/microsoft/mimalloc)** | Daan Leijen ([Microsoft Research](https://github.com/microsoft)) | MIT License | Компактный высокоскоростной распределитель памяти, исключающий внешнюю фрагментацию и заменяющий медленный менеджер кучи `bCMemoryAdmin`. |
| **[libdeflate](https://github.com/ebiggers/libdeflate)** | Eric Biggers ([ebiggers](https://github.com/ebiggers)) | MIT License | Высокоскоростной декомпрессор zlib/deflate с аппаратной SIMD-векторизацией SSE4.2 / AVX2 для быстрой распаковки мешей и текстур из `.pak` архивов. |
| **[MinHook](https://github.com/TsudaKageyu/minhook)** | Tsuda Kageyu ([TsudaKageyu](https://github.com/TsudaKageyu)) | BSD 2-Clause | Минималистичная и потокобезопасная библиотека низкоуровневых API-перехватов (detours/trampolines) для x86. |
| **[Heaven's Gate](https://github.com/Gothic3Optima)** | Roy M. ([ReWolf](https://github.com/mwrl)), mixcode, antivm | Public Domain | Архитектурная концепция переключения процессора x86 $\to$ x64 через селектор сегмента `0x33` для доступа к памяти за пределами 4 ГБ в Windows x64. |
| **Community Patch Team (CPT)** | CPT Team | - | За титанический труд над Community Patch 1.75.14, декомпиляцию бинарников Genome Engine и спасение игры. |
| **Update Pack Team** | zandr, Dimus и сообщество World of Players | - | За разработку Update Pack 1.04.11, исправление сотен скриптов и стабилизацию библиотек `Engine.dll` / `Game.dll`. |
| **Piranha Bytes** | Piranha Bytes | - | Создатели бессмертной вселенной Gothic и оригинального движка Genome Engine. |

---

## Лицензия

Исходный код Gothic 3 Optima распространяется под лицензией [MIT](LICENSE).  
Все сторонние компоненты остаются собственностью их авторов и распространяются под своими оригинальными лицензиями.
