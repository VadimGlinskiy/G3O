# Script to package Gothic 3 Optima release ZIP archives for each DXVK preset
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $root "dist"
$packagesDir = Join-Path $dist "packages"
$presetsDir = Join-Path $root "dxvk_presets"
$bin = Join-Path $root "build\Release\dinput8.dll"

if (-not (Test-Path $bin)) {
    Write-Error "Binary dinput8.dll not found at $bin! Build Release target first."
    exit 1
}

New-Item -ItemType Directory -Path $packagesDir -Force | Out-Null

$presets = @(
    @{
        Folder = "1.10.1-async";
        ZipName = "Gothic3Optima-v1.0-DXVK-1.10.1-Async.zip";
        Desc = "Older version. Use if you have an Intel GPU or newer versions cause issues."
    },
    @{
        Folder = "1.10.3-async";
        ZipName = "Gothic3Optima-v1.0-DXVK-1.10.3-Async.zip";
        Desc = "Older version. Use if your GPU lacks Vulkan 1.3 support or has issues with DXVK 2.0."
    },
    @{
        Folder = "2.4.1";
        ZipName = "Gothic3Optima-v1.0-DXVK-2.4.1-Stable.zip";
        Desc = "Stable version. Recommended for AMD Polaris/GCN & NVIDIA Pascal/Turing/Ampere."
    },
    @{
        Folder = "3.0.1";
        ZipName = "Gothic3Optima-v1.0-DXVK-3.0.1.zip";
        Desc = "Current version. Use if your GPU supports all required extensions and Vulkan 1.4."
    },
    @{
        Folder = "3.0.1-hdr";
        ZipName = "Gothic3Optima-v1.0-DXVK-3.0.1-HDR.zip";
        Desc = "DXVK 3.0.1 with HDR output. Use if you have an HDR-capable monitor and Windows HDR enabled."
    }
)

foreach ($p in $presets) {
    $stage = Join-Path $dist "stage_$($p.Folder)"
    New-Item -ItemType Directory -Path $stage -Force | Out-Null

    Copy-Item $bin (Join-Path $stage "dinput8.dll") -Force
    Copy-Item (Join-Path $presetsDir "$($p.Folder)\*") $stage -Force

    $readme = @"
========================================================================
GOTHIC 3 OPTIMA - DXVK $($p.Folder.ToUpper()) EDITION
========================================================================
Description:
$($p.Desc)

INSTALLATION:
1. Copy all files from this archive into your Gothic 3 game directory
   (where Gothic3.exe is located).
2. Launch Gothic 3 through Steam, GOG, or Gothic3.exe directly.
========================================================================
"@
    Set-Content -Path (Join-Path $stage "README_INSTALL.txt") -Value $readme -Encoding UTF8

    $zipPath = Join-Path $packagesDir $p.ZipName
    Compress-Archive -Path "$stage\*" -DestinationPath $zipPath -Force
    Remove-Item $stage -Recurse -Force

    Write-Host "[OK] Created release package: $($p.ZipName)"
}

Write-Host "All release packages created in $packagesDir!"
