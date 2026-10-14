param(
    [string]$Version = "v1.3.0"
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
GOTHIC 3 OPTIMA - CORE OPTIMIZATION & STABILITY SUITE ($Version)
================================================================================

FEATURES INCLUDED:
- Multicore Threading Engine (eCThreadPool Expander, Priority Scheduler & Task Offloader)
- Complete Replacement of Genome Engine Memory Allocator (HeapAlloc & VirtualAlloc)
- 128-Byte Cache-Line Aligned Allocations (Eliminates False Sharing)
- Dynamic Large Page Aware / Low-Fragmentation Virtual Sizing
- Atomic Savegame Transaction Guard (Anti-Corruption & Temporary Write Buffer)
- SIMD-Accelerated 3D Math Engine (AVX2 / SSE4.1 Vec4, Matrix4x4, Bounding Boxes)
- High-Performance Work-Stealing Thread Pool with Lock-Free Task Queues
- Safe SEH Exception Filter & Instant Crash Dump Generator
- High Memory Shadow Cache (> 4 GB via Heaven's Gate)
- Fixed Physics Substep (16.6 ms) & Bone Velocity / NaN Ragdoll Clamp
- Dynamic Vertex & Index Buffer Scaling (1-8 MB, 32-bit Index Promotion)
- High-Precision Floating-Point Z-Buffer & Shadow Depth Bias Calibration
- Combatant Frustum Culling Bypass (Continuous AI Pursuit & Aggro Focus)
- Multi-Ray Footprint & Ground Recovery Fall Guard (Seam Fall Prevention)

INSTALLATION:
1. Extract 'dinput8.dll' from this archive directly into your Gothic 3 game folder
   (where 'Gothic3.exe' is located).
2. [OPTIONAL & HIGHLY RECOMMENDED]: Download one of the DXVK packages from the
   same release page matching your GPU to translate DirectX 9 to Vulkan:
   - DXVK-2.4.1-Stable.zip (Default / Recommended for NVIDIA / AMD GPUs)
   - DXVK-3.0.1.zip (Modern GPUs with Vulkan 1.4 support)
   - DXVK-3.0.1-HDR.zip (With Native HDR output support)
   - DXVK-1.10.3-Async.zip (Legacy GPUs lacking Vulkan 1.3)
   - DXVK-1.10.1-Async.zip (Legacy Intel GPUs / Compatibility fallback)

COMPATIBILITY:
- Gothic 3 v1.75.14 (Community Patch)
- Works with Update Pack 1.04+, Update Pack 1.05+, Universal Modpack UNION+ v1.8.2.
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
2. Ensure you have installed 'Gothic3Optima-$Version-Core.zip' (dinput8.dll) as well.
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
