param(
    [string]$Version = "v1.3.1"
)

# ==============================================================================
# Gothic 3 Optima - Release Packaging Script
# Generates 1 Core Package + 5 Modular DXVK Variant Archives for GitHub Releases
# ==============================================================================
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $root "dist"
$packagesDir = Join-Path $dist "packages"
$dxvkDir = Join-Path $root "resources\dxvk"
$bin = Join-Path $root "build\Release\dinput8.dll"

if (-not (Test-Path $bin)) {
    Write-Error "Binary dinput8.dll not found at $bin! Build Release target first."
    exit 1
}

if (-not (Test-Path $packagesDir)) {
    New-Item -ItemType Directory -Path $packagesDir -Force | Out-Null
}

# ------------------------------------------------------------------------------
# 1. Package Gothic3Optima-Core.zip
# ------------------------------------------------------------------------------
Write-Host ">>> Packaging Gothic3Optima Core ($Version)..."
$stageCore = Join-Path $dist "stage_core"
if (Test-Path $stageCore) { Remove-Item $stageCore -Recurse -Force }
New-Item -ItemType Directory -Path $stageCore -Force | Out-Null

Copy-Item $bin (Join-Path $stageCore "dinput8.dll") -Force

$readmeCore = @"
================================================================================
GOTHIC 3 OPTIMA - PUP-COMPATIBLE DEVELOPMENT CORE ($Version)
================================================================================

FEATURES INCLUDED:
- System dinput8.dll proxy.

- G3O version label in the main menu.
- Optional legacy G3SE plugin discovery when its DLL is installed.

PUP owns background resource work, caches, prefetching, file system,
FMOD audio and asynchronous save writing. This core does not modify them.
The older 1.3.0 release had different features; see README.md.
Back up an existing game-folder dinput8.dll before installing this build.
In-game compatibility testing is still required.

INSTALLATION:
1. Extract 'dinput8.dll' from this archive directly into your Gothic 3 game folder
   (where 'Gothic3.exe' is located).
2. [OPTIONAL]: DXVK is an independent graphics component, not part of G3O.

COMPATIBILITY:
- Gothic 3 v1.75.14 (Community Patch)
- Designed to avoid overlap with installed Parallel Universe Patch 1.1.1.
- In-game compatibility with modpacks and Update Pack is not yet verified.
================================================================================
"@
Set-Content -Path (Join-Path $stageCore "README_INSTALL.txt") -Value $readmeCore -Encoding UTF8

$coreZip = Join-Path $packagesDir "Gothic3Optima-$Version-Core.zip"
Compress-Archive -Path "$stageCore\*" -DestinationPath $coreZip -Force
Remove-Item $stageCore -Recurse -Force
Write-Host "[OK] Created: Gothic3Optima-$Version-Core.zip"

# ------------------------------------------------------------------------------
# 2. Package Modular DXVK Addons
# ------------------------------------------------------------------------------
$dxvkVariants = @(
    @{
        Folder = "1.10.1-async";
        ZipName = "DXVK-1.10.1-Async.zip";
        Title = "DXVK 1.10.1 - Async (Legacy Intel & Fallback)";
        Desc = "Older async DXVK build. Recommended if you have an older Intel GPU or newer DXVK versions experience crashes/driver timeouts."
    },
    @{
        Folder = "1.10.3-async";
        ZipName = "DXVK-1.10.3-Async.zip";
        Title = "DXVK 1.10.3 - Async (Pre-Vulkan 1.3 Legacy GPU)";
        Desc = "Async build for GPUs lacking Vulkan 1.3 support or facing stability issues on DXVK 2.x+."
    },
    @{
        Folder = "2.4.1";
        ZipName = "DXVK-2.4.1-Stable.zip";
        Title = "DXVK 2.4.1 - Stable (Recommended)";
        Desc = "Rock-solid DXVK release. Highly recommended for AMD Polaris/Vega/RDNA & NVIDIA Pascal/Turing/Ampere/Ada."
    },
    @{
        Folder = "3.0.1";
        ZipName = "DXVK-3.0.1.zip";
        Title = "DXVK 3.0.1 (Current Vulkan 1.4)";
        Desc = "Latest generation DXVK translation layer. Requires modern GPU drivers supporting Vulkan 1.4."
    },
    @{
        Folder = "3.0.1-hdr";
        ZipName = "DXVK-3.0.1-HDR.zip";
        Title = "DXVK 3.0.1 - HDR (High Dynamic Range Output)";
        Desc = "DXVK 3.0.1 with true HDR color pipeline. Use if you have an HDR monitor and Windows HDR is enabled."
    }
)

foreach ($v in $dxvkVariants) {
    Write-Host ">>> Packaging $($v.ZipName)..."
    $stage = Join-Path $dist "stage_$($v.Folder)"
    if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
    New-Item -ItemType Directory -Path $stage -Force | Out-Null

    $variantSource = Join-Path $dxvkDir $v.Folder
    Copy-Item (Join-Path $variantSource "*") $stage -Force

    $readme = @"
================================================================================
$($v.Title)
Gothic 3 Optima - Vulkan Translation Addon
================================================================================

DESCRIPTION:
$($v.Desc)

INCLUDED FILES:
- d3d9.dll   (DirectX 9 to Vulkan translation layer)
- dxgi.dll   (DXGI interface)
- dxvk.conf  (Tuned configuration for Gothic 3: async compile, VRAM limits)

INSTALLATION:
1. Extract all files from this archive directly into your Gothic 3 game folder
   (where 'Gothic3.exe' is located).
2. DXVK is independent of the G3O core and of Parallel Universe Patch.
3. Launch Gothic 3!
================================================================================
"@
    Set-Content -Path (Join-Path $stage "README_DXVK.txt") -Value $readme -Encoding UTF8

    $zipPath = Join-Path $packagesDir $v.ZipName
    Compress-Archive -Path "$stage\*" -DestinationPath $zipPath -Force
    Remove-Item $stage -Recurse -Force

    Write-Host "[OK] Created: $($v.ZipName)"
}

Write-Host ""
Write-Host "========================================================================"
Write-Host "All 6 release packages successfully created in: $packagesDir"
Get-ChildItem -Path $packagesDir | Select-Object Name, Length
Write-Host "========================================================================"
