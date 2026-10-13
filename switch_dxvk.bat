@echo off
chcp 65001 > nul
setlocal enabledelayedexpansion
cls

set "ROOT=%~dp0"
set "PRESETS=%ROOT%dxvk_presets"

echo ===============================================================================
echo                   GOTHIC 3 OPTIMA - DXVK PROFILE SELECTOR
echo ===============================================================================
echo.
echo [1] DXVK 1.10.1 - Async
echo     (Older version. Use if you have an Intel GPU or newer versions cause issues.)
echo.
echo [2] DXVK 1.10.3 - Async
echo     (Older version. Use if your GPU lacks Vulkan 1.3 support or has issues with DXVK 2.0.)
echo.
echo [3] DXVK 2.4.1 - Stable (Default)
echo     (Recommended for AMD Polaris / GCN and NVIDIA Pascal / Turing / Ampere.)
echo.
echo [4] DXVK 3.0.1
echo     (Current version. Use if your GPU supports all required extensions and Vulkan 1.4.)
echo.
echo [5] DXVK 3.0.1 - HDR
echo     (DXVK 3.0.1 with HDR output enabled for HDR displays.)
echo.
echo [0] Выход
echo ===============================================================================
set /p choice="Выберите профиль (0-5): "

if "%choice%"=="1" goto apply_1
if "%choice%"=="2" goto apply_2
if "%choice%"=="3" goto apply_3
if "%choice%"=="4" goto apply_4
if "%choice%"=="5" goto apply_5
if "%choice%"=="0" goto end
goto invalid

:apply_1
set "SRC=%PRESETS%\1.10.1-async"
set "NAME=DXVK 1.10.1 - Async"
goto copy_files

:apply_2
set "SRC=%PRESETS%\1.10.3-async"
set "NAME=DXVK 1.10.3 - Async"
goto copy_files

:apply_3
set "SRC=%PRESETS%\2.4.1"
set "NAME=DXVK 2.4.1 - Stable"
goto copy_files

:apply_4
set "SRC=%PRESETS%\3.0.1"
set "NAME=DXVK 3.0.1 (Current)"
goto copy_files

:apply_5
set "SRC=%PRESETS%\3.0.1-hdr"
set "NAME=DXVK 3.0.1 - HDR"
goto copy_files

:copy_files
if not exist "%SRC%\d3d9.dll" (
    echo [ОШИБКА] Пресет не найден в %SRC%!
    pause
    goto end
)

echo.
echo Применяю профиль: %NAME%...
copy /Y "%SRC%\d3d9.dll" "%ROOT%d3d9.dll" > nul
copy /Y "%SRC%\dxgi.dll" "%ROOT%dxgi.dll" > nul
copy /Y "%SRC%\dxvk.conf" "%ROOT%dxvk.conf" > nul

echo [УСПЕХ] Профиль %NAME% успешно установлен!
echo Файлы d3d9.dll, dxgi.dll и dxvk.conf обновлены.
echo.
pause
goto end

:invalid
echo Неверный выбор!
pause
goto end

:end
