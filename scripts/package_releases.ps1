param(
    [string]$Version = "v1.7.1"
)

# ==============================================================================
# Gothic 3 Optima - Release Packaging Script
# Generates the core package and modular DXVK/ReShade archives for GitHub Releases
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
$oldPackages = Get-ChildItem -Path $packagesDir -File |
    Where-Object { $_.Name -match '^(Gothic3Optima-.*-Core|DXVK-.*|ReShade-RTGI-Gothic3)\.zip$' }
if ($oldPackages) { 
    foreach ($pkg in $oldPackages) {
        Remove-Item -LiteralPath $pkg.FullName -Force -ErrorAction SilentlyContinue
    }
}

# ------------------------------------------------------------------------------
# 1. Package Gothic3Optima-Core.zip
# ------------------------------------------------------------------------------
Write-Host ">>> Packaging Gothic3Optima Core ($Version)..."
$stageCore = Join-Path $dist "stage_core"
if (Test-Path $stageCore) { Remove-Item $stageCore -Recurse -Force }
New-Item -ItemType Directory -Path $stageCore -Force | Out-Null

Copy-Item $bin (Join-Path $stageCore "dinput8.dll") -Force
Copy-Item (Join-Path $root "G3O.ini") (Join-Path $stageCore "G3O.ini") -Force

$readmeCore = @"
================================================================================
GOTHIC 3 OPTIMA - CORE ($Version)
================================================================================

FEATURES INCLUDED:
- System dinput8.dll proxy.

- Optional legacy G3SE plugin discovery when its DLL is installed.

Back up an existing game-folder dinput8.dll before installing this build.

INSTALLATION:
1. Extract 'dinput8.dll' and 'G3O.ini' from this archive into your Gothic 3 game folder
   (where 'Gothic3.exe' is located). Back up any existing 'G3O.ini' first.
2. [OPTIONAL]: DXVK is an independent graphics component, not part of G3O.

CONFIGURATION AND DIAGNOSTICS:
- Keep 'G3O.ini' beside 'Gothic3.exe'.
- G3O logs, reports, dumps and traces go in a flat 'G3O' folder beside the executable.
- The game folder must be writable for G3O to save these files.

COMPATIBILITY:
- In-game compatibility with PUP and modpacks has not been tested.
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
        Title = "DXVK 1.10.1 - Async (Experimental)";
        Desc = "Older DXVK build with asynchronous compilation enabled. In-game compatibility is not verified."
    },
    @{
        Folder = "1.10.3-async";
        ZipName = "DXVK-1.10.3-Async.zip";
        Title = "DXVK 1.10.3 - Async (Experimental)";
        Desc = "Older DXVK build with asynchronous compilation enabled. In-game compatibility is not verified."
    },
    @{
        Folder = "2.4.1";
        ZipName = "DXVK-2.4.1-Stable.zip";
        Title = "DXVK 2.4.1 - Stable (Defaults)";
        Desc = "DXVK 2.4.1 with upstream defaults. In-game compatibility is not verified."
    },
    @{
        Folder = "3.0.1";
        ZipName = "DXVK-3.0.1.zip";
        Title = "DXVK 3.0.1 (Defaults)";
        Desc = "DXVK 3.0.1 with upstream defaults. In-game compatibility is not verified."
    },
    @{
        Folder = "3.0.1-hdr";
        ZipName = "DXVK-3.0.1-HDR.zip";
        Title = "DXVK 3.0.1 - Legacy HDR-labelled archive (SDR for Gothic 3)";
        Desc = "Gothic 3 uses D3D9; this legacy archive does not enable HDR. Use the standard DXVK 3.0.1 archive instead."
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
- dxvk.conf  (Safe window/overlay creation enabled; async archives also enable async compilation)

INSTALLATION:
1. Extract all files from this archive directly into your Gothic 3 game folder
   (where 'Gothic3.exe' is located).
2. DXVK is independent of the G3O core and of Parallel Universe Patch.
3. Launch Gothic 3!
================================================================================
"@
    Set-Content -Path (Join-Path $stage "README_DXVK.txt") -Value $readme -Encoding UTF8

    $zipPath = Join-Path $packagesDir $v.ZipName
    try {
        if (Test-Path $zipPath) { Remove-Item $zipPath -Force -ErrorAction SilentlyContinue }
        Compress-Archive -Path "$stage\*" -DestinationPath $zipPath -Force -ErrorAction Stop
        Write-Host "[OK] Created: $($v.ZipName)"
    } catch {
        Write-Warning "Could not update $($v.ZipName) (file locked). Existing package preserved."
    }
    Remove-Item $stage -Recurse -Force
}

Write-Host ""
Write-Host "========================================================================"
$reshadeDir = Join-Path $root "resources\reshade"
if (Test-Path $reshadeDir) {
    Write-Host ">>> Packaging ReShade-RTGI-Gothic3.zip..."
    $stageReShade = Join-Path $dist "stage_reshade"
    if (Test-Path $stageReShade) { Remove-Item $stageReShade -Recurse -Force }
    New-Item -ItemType Directory -Path $stageReShade -Force | Out-Null

    Copy-Item (Join-Path $reshadeDir "*") $stageReShade -Recurse -Force

    $readmeReShade = @"
================================================================================
Gothic 3 - ReShade 6.8 Enhanced Visuals Addon
================================================================================

DESCRIPTION:
Optional visual enhancement pack providing ambient occlusion (MXAO),
anti-aliasing (SMAA), color debanding (qUINT Deband), screen-space reflections (SSR),
and atmospheric light diffusion (Solaris Bloom).

INCLUDED FILES:
- ReShade32.dll    (ReShade 6.8.0 32-bit runtime - automatically loaded by G3O)
- ReShade.ini      (Pre-configured with Home key overlay and depth buffer settings)
- reshade-shaders/ (Curated Marty's Mods and qUINT shaders)

INSTALLATION:
1. Extract all contents of this archive directly into your Gothic 3 game folder
   (where 'Gothic3.exe' is located).
2. Launch Gothic 3. Gothic 3 Optima (G3O) will automatically load ReShade32.dll.
3. Press 'Home' on your keyboard to open the ReShade configuration overlay.
   (On compact/laptop keyboards, press 'Fn + Home' or 'Fn + Left Arrow').

IMPORTANT RENDERER COMPATIBILITY:
- Compute Shaders vs Native DirectX 9:
  Advanced effects from Marty's Mods (RTGI ray tracing, physical film grain, Solaris)
  require Compute Shaders (cs_5_0), which are NOT supported by native Direct3D 9.
  To use the full Marty's Mods / RTGI suite, DXVK (DXVK-2.4.1-Stable or 1.10.3-Async)
  is REQUIRED. DXVK translates D3D9 to Vulkan with full Compute Shader support.
- Native DirectX 9 (Without DXVK):
  If you play on native DirectX 9 without DXVK, use the qUINT shader suite
  (qUINT_mxao, qUINT_bloom, qUINT_ssr, qUINT_dof, qUINT_lightroom, qUINT_sharp,
  qUINT_deband) and MartysMods_SMAA. They are 100% Direct3D 9 compliant and
  compile instantly on any graphics card without crashes.

NOTE:
ReShade is completely optional and separate from the core G3O performance optimizations.
================================================================================
"@
    Set-Content -Path (Join-Path $stageReShade "README_RESHADE.txt") -Value $readmeReShade -Encoding UTF8

    $reshadeZip = Join-Path $packagesDir "ReShade-RTGI-Gothic3.zip"
    try {
        Compress-Archive -Path "$stageReShade\*" -DestinationPath $reshadeZip -Force -ErrorAction Stop
        Write-Host "[OK] Created: ReShade-RTGI-Gothic3.zip"
    } catch {
        Write-Warning "Could not update ReShade-RTGI-Gothic3.zip because it is currently open in another program (e.g. 7-Zip). Existing package preserved."
    }
    Remove-Item $stageReShade -Recurse -Force
}

Write-Host "All release packages successfully created in: $packagesDir"
Get-ChildItem -Path $packagesDir | Select-Object Name, Length
Write-Host "========================================================================"
