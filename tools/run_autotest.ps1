param(
    [string]$GameDir = "E:\SteamLibrary\steamapps\common\Gothic 3",
    [string]$Slot = "Autosave_Slot_0001",
    [int]$Loads = 2,
    [int]$SettleFrames = 60,
    [int]$MenuSettleTicks = 30,
    [int]$TimeoutSec = 180,
    [int]$ProcessRuns = 1,
    [switch]$Build,
    [switch]$Deploy,
    [ValidateSet("Release", "Telemetry")]
    [string]$BuildConfig = "Release",
    [bool]$SkipIntro = $true,
    [bool]$DisableReShade = $true,
    [bool]$LayerAudit = $false,
    [string]$Baseline = "",
    [double]$MaxRegressionPct = 10.0,
    [string]$OutDir = "",
    [switch]$KillExisting,
    [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "autotest_parser.ps1")

function Get-FileSha256Safe([string]$p) {
    if (Test-Path -LiteralPath $p) {
        return (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash
    }
    return "<missing>"
}

# 1. Preflight before any build/deploy side effect
$exePath = Join-Path $GameDir "Gothic3.exe"
$dllPath = Join-Path $GameDir "dinput8.dll"
$iniPath = Join-Path $GameDir "G3O.ini"
$g3oLogDir = Join-Path $GameDir "G3O"
$saveDir = Join-Path ([Environment]::GetFolderPath('MyDocuments')) "gothic3"
$saveFile1 = Join-Path $saveDir "$Slot.g3savcpx"
$saveFile2 = Join-Path $saveDir "$Slot.g3savcpxdat"

if (-not (Test-Path -LiteralPath $exePath)) { throw "Gothic3.exe not found: $exePath" }
if (-not (Test-Path -LiteralPath $dllPath) -and -not $Deploy) { throw "dinput8.dll not found: $dllPath" }
if (-not (Test-Path -LiteralPath $g3oLogDir) -and -not $DryRun) { New-Item -ItemType Directory -Path $g3oLogDir -Force | Out-Null }
if (-not [Environment]::UserInteractive) { throw "Interactive Windows desktop session is required for D3D9." }

$existing = @(Get-Process -Name "Gothic3" -ErrorAction SilentlyContinue)
if ($existing.Count -gt 0) {
    if ($KillExisting) {
        $existing | Stop-Process -Force
        Start-Sleep -Milliseconds 500
        $existing = @(Get-Process -Name "Gothic3" -ErrorAction SilentlyContinue)
        if ($existing.Count -gt 0) { throw "Gothic3.exe is still running after termination (PID: $($existing.Id -join ', '))." }
    } else {
        throw "Gothic3.exe is already running (PID: $($existing.Id -join ', ')). Pass -KillExisting to terminate it first."
    }
}

$slotExists = (Test-Path -LiteralPath $saveFile1) -and (Test-Path -LiteralPath $saveFile2)
if (-not $slotExists) {
    Write-Warning "Savegame files for slot '$Slot' not found in $saveDir (expected for negative tests only)."
}
$installedDllHashAtPreflight = Get-FileSha256Safe $dllPath

if ($DryRun) {
    Write-Host "[DryRun] Preflight passed. No build, deploy, or process launch performed."
    exit 0
}

# 2. Optional Build & Deploy
if ($Build) {
    $cmakeExe = @(
        "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\CMake\bin\cmake.exe"
    ) | Where-Object { Test-Path $_ } | Select-Object -First 1
    $buildDir = if ($BuildConfig -eq "Telemetry") { Join-Path $repoRoot "build-telemetry" } else { Join-Path $repoRoot "build" }
    Write-Host "[AutoTest] Building dinput8 ($BuildConfig) in $buildDir..."
    if ($cmakeExe) {
        & $cmakeExe --build $buildDir --config Release --target dinput8
    } else {
        $pythonExe = (Get-Command python.exe -ErrorAction SilentlyContinue).Source
        if (-not $pythonExe) { throw "CMake and python.exe were not found for -Build." }
        & $pythonExe -m cmake --build $buildDir --config Release --target dinput8
    }
    if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE." }
}

if ($Deploy) {
    $buildDir = if ($BuildConfig -eq "Telemetry") { Join-Path $repoRoot "build-telemetry" } else { Join-Path $repoRoot "build" }
    $builtDll = Join-Path $buildDir "Release\dinput8.dll"
    if (-not (Test-Path -LiteralPath $builtDll)) { throw "Built DLL not found: $builtDll" }
    $targetDll = Join-Path $GameDir "dinput8.dll"
    $builtHash = Get-FileSha256Safe $builtDll
    $targetBeforeHash = "<missing>"
    $backupPath = ""
    if (Test-Path -LiteralPath $targetDll) {
        $targetBeforeHash = Get-FileSha256Safe $targetDll
    }
    if ($targetBeforeHash -ne $installedDllHashAtPreflight) {
        throw "Installed dinput8.dll changed since preflight; deployment cancelled."
    }
    if ($builtHash -eq $targetBeforeHash) {
        Write-Host "[AutoTest] Installed dinput8.dll already matches the build artifact (SHA-256=$builtHash); replacement skipped."
    } else {
        if ($targetBeforeHash -ne "<missing>") {
            $backupTag = (Get-Date -Format "yyyyMMdd-HHmmss") + "-" + ([guid]::NewGuid().ToString("N").Substring(0, 6))
            $backupPath = Join-Path $GameDir ("dinput8.dll.backup-" + $backupTag)
        }

        $stagePath = Join-Path $GameDir (".dinput8.dll." + [guid]::NewGuid().ToString("N") + ".tmp")
        try {
            Copy-Item -LiteralPath $builtDll -Destination $stagePath
            if ((Get-FileSha256Safe $stagePath) -ne $builtHash) {
                throw "Staged DLL hash differs from build artifact; deployment cancelled."
            }

            $existing = @(Get-Process -Name "Gothic3" -ErrorAction SilentlyContinue)
            if ($existing.Count -gt 0) { throw "Gothic3.exe started before deployment (PID: $($existing.Id -join ', ')); deployment cancelled." }

            if ($targetBeforeHash -ne "<missing>") {
                if ((Get-FileSha256Safe $targetDll) -ne $targetBeforeHash) {
                    throw "Installed dinput8.dll changed during build; deployment cancelled."
                }
                [System.IO.File]::Replace($stagePath, $targetDll, $backupPath)
                if ((Get-FileSha256Safe $backupPath) -ne $targetBeforeHash) {
                    [System.IO.File]::Copy($backupPath, $targetDll, $true)
                    if ((Get-FileSha256Safe $targetDll) -ne $targetBeforeHash) {
                        throw "Atomic backup hash mismatch and original DLL restoration failed: $backupPath"
                    }
                    throw "Atomic backup hash mismatch; original DLL restored."
                }
            } else {
                if (Test-Path -LiteralPath $targetDll) { throw "dinput8.dll appeared during build; deployment cancelled." }
                [System.IO.File]::Move($stagePath, $targetDll)
            }

            $deployedHash = Get-FileSha256Safe $targetDll
            if ($deployedHash -ne $builtHash) {
                if ($backupPath) {
                    [System.IO.File]::Copy($backupPath, $targetDll, $true)
                    if ((Get-FileSha256Safe $targetDll) -ne $targetBeforeHash) {
                        throw "Deployment hash mismatch and backup restoration failed: $backupPath"
                    }
                }
                throw "Deployed DLL hash mismatch; original DLL restored when available."
            }
            if ($backupPath) {
                Write-Host "[AutoTest] Backed up existing dinput8.dll -> $backupPath (SHA-256=$targetBeforeHash)"
            }
            Write-Host "[AutoTest] Deployed $builtDll -> $targetDll (SHA-256=$deployedHash)"
        } finally {
            if (Test-Path -LiteralPath $stagePath) { Remove-Item -LiteralPath $stagePath -Force }
        }
    }
}

# 3. Hashes and run output
$hashes = [ordered]@{
    "Gothic3.exe"    = Get-FileSha256Safe $exePath
    "dinput8.dll"    = Get-FileSha256Safe $dllPath
    "Game.dll"       = Get-FileSha256Safe (Join-Path $GameDir "Game.dll")
    "Engine.dll"     = Get-FileSha256Safe (Join-Path $GameDir "Engine.dll")
    "SharedBase.dll" = Get-FileSha256Safe (Join-Path $GameDir "SharedBase.dll")
    "FileSystem.dll" = Get-FileSha256Safe (Join-Path $GameDir "FileSystem.dll")
}

$batchId = (Get-Date -Format "yyyyMMdd_HHmmss") + "_" + ([guid]::NewGuid().ToString("N").Substring(0, 6))
if (-not $OutDir) {
    $OutDir = Join-Path $repoRoot "scratch\autotest_runs\$batchId"
}

Write-Host "========================================================================"
Write-Host " G3O AutoTest Runner  |  RunId: $batchId"
Write-Host " GameDir: $GameDir"
Write-Host " Slot: $Slot (exists=$slotExists) | Loads: $Loads | ProcessRuns: $ProcessRuns | Timeout: ${TimeoutSec}s"
Write-Host " dinput8.dll SHA-256: $($hashes['dinput8.dll'])"
Write-Host "========================================================================"

New-Item -ItemType Directory -Path $OutDir -Force | Out-Null

$overallExitCode = 0
$allRunSummaries = @()

for ($runIdx = 1; $runIdx -le $ProcessRuns; $runIdx++) {
    $runId = if ($ProcessRuns -gt 1) { "${batchId}_r${runIdx}" } else { $batchId }
    $startTime = Get-Date
    Write-Host "`n>>> Starting run $runIdx/$ProcessRuns (RunId=$runId)..."

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exePath
    $psi.WorkingDirectory = $GameDir
    $psi.UseShellExecute = $false
    $psi.Environment["G3O_AUTOTEST"] = "1"
    $psi.Environment["G3O_AUTOTEST_SLOT"] = $Slot
    $psi.Environment["G3O_AUTOTEST_LOADS"] = "$Loads"
    $psi.Environment["G3O_AUTOTEST_SETTLE_FRAMES"] = "$SettleFrames"
    $psi.Environment["G3O_AUTOTEST_MENU_SETTLE_TICKS"] = "$MenuSettleTicks"
    $psi.Environment["G3O_AUTOTEST_TIMEOUT_SEC"] = "$TimeoutSec"
    $psi.Environment["G3O_AUTOTEST_SKIP_INTRO"] = if ($SkipIntro) { "1" } else { "0" }
    $psi.Environment["G3O_AUTOTEST_DISABLE_RESHADE"] = if ($DisableReShade) { "1" } else { "0" }
    $psi.Environment["G3O_AUTOTEST_LAYER_AUDIT"] = if ($LayerAudit) { "1" } else { "0" }
    $psi.Environment["G3O_AUTOTEST_RUN_ID"] = $runId

    $proc = [System.Diagnostics.Process]::Start($psi)
    $childPid = $proc.Id
    Write-Host "[AutoTest] Launched Gothic3.exe (PID=$childPid). Waiting up to $($TimeoutSec + 15)s..."

    $timedOut = -not $proc.WaitForExit(($TimeoutSec + 15) * 1000)
    $procExitCode = -1
    if ($timedOut) {
        Write-Warning "[AutoTest] Watchdog timeout reached! Terminating PID=$childPid..."
        try { $proc.Kill() } catch {}
        $proc.WaitForExit(5000) | Out-Null
    } else {
        $procExitCode = $proc.ExitCode
    }
    $elapsedSec = [Math]::Round(((Get-Date) - $startTime).TotalSeconds, 2)

    # Correlate logs by PID and RunId
    $autoJsonPath = Join-Path $g3oLogDir "G3O_AutoTest_${runId}.json"
    if (-not (Test-Path -LiteralPath $autoJsonPath)) {
        $autoJsonPath = Join-Path $g3oLogDir "G3O_AutoTest_${childPid}.json"
    }
    $diagLog = Get-ChildItem -Path $g3oLogDir -Filter "G3O_diagnostic_*_${childPid}_*.log" -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    $loadLogs = @(Get-ChildItem -Path $g3oLogDir -Filter "G3O_LoadProfiler_*_${childPid}_*.log" -ErrorAction SilentlyContinue |
        Sort-Object Name | ForEach-Object { $_.FullName })
    $crashLogs = @(Get-ChildItem -Path $g3oLogDir -Filter "G3O_CrashReport_*_${childPid}_*.log" -ErrorAction SilentlyContinue |
        Sort-Object Name | ForEach-Object { $_.FullName })

    $eval = Evaluate-AutoTestRun `
        -ProcessExitCode $procExitCode `
        -TimedOut $timedOut `
        -ExpectedLoads $Loads `
        -AutoTestJsonPath $autoJsonPath `
        -DiagnosticLogPath $(if ($diagLog) { $diagLog.FullName } else { "" }) `
        -LoadProfilerPaths $loadLogs `
        -CrashReportPaths $crashLogs `
        -BaselinePath $Baseline `
        -MaxRegressionPct $MaxRegressionPct

    $summary = [ordered]@{
        RunId           = $runId
        Timestamp       = (Get-Date -Format "o")
        Pid             = $childPid
        ElapsedWallSec  = $elapsedSec
        GameDir         = $GameDir
        Slot            = $Slot
        Hashes          = $hashes
        Status          = $eval.Status
        ExitCode        = $eval.ExitCode
        ProcessExitCode = $eval.ProcessExitCode
        TimedOut        = $eval.TimedOut
        ExpectedLoads   = $eval.ExpectedLoads
        CompletedLoads  = $eval.CompletedLoads
        Reasons         = $eval.Reasons
        Loads           = $eval.Loads
        BaselineDiff    = $eval.BaselineDiff
        Anomalies       = [ordered]@{
            HookFailures    = $eval.Diagnostic.HookFailures
            TextureFailures = $eval.Diagnostic.TextureFailures
            FatalErrors     = $eval.Diagnostic.FatalErrors
            CrashGuards     = $eval.Diagnostic.CrashGuards
            DecompFallbacks = $eval.Diagnostic.DecompFallbacks
        }
        LastPhase       = $eval.Diagnostic.LastPhase
        Artifacts       = [ordered]@{
            AutoTestJson   = if (Test-Path -LiteralPath $autoJsonPath) { $autoJsonPath } else { "" }
            DiagnosticLog  = if ($diagLog) { $diagLog.FullName } else { "" }
            LoadProfilers  = $loadLogs
            CrashReports   = $crashLogs
        }
    }

    $summaryPath = Join-Path $OutDir "summary_${runId}.json"
    ($summary | ConvertTo-Json -Depth 8) | Set-Content -LiteralPath $summaryPath -Encoding UTF8
    if ($ProcessRuns -eq 1) {
        Copy-Item -LiteralPath $summaryPath -Destination (Join-Path $OutDir "summary.json") -Force
    }
    $allRunSummaries += $summary

    Write-Host "------------------------------------------------------------------------"
    Write-Host " RESULT: $($eval.Status) (exit=$($eval.ExitCode), procExit=$procExitCode, wall=${elapsedSec}s)"
    if ($eval.Reasons.Count -gt 0) {
        Write-Host " Reasons: $($eval.Reasons -join ', ')"
    }
    foreach ($l in $eval.Loads) {
        Write-Host ("  Load #{0}: Total={1:N3}s | PreLoad={2:N3}s (Activate={3:N3}s, {4} sectors, SysDyn={5}ms) | World={6:N3}s | GotoPos={7:N3}s (Pak={8:N3}s, Setup={9:N3}s)" -f `
            $l.LoadIndex, $l.TotalSec, $l.PreLoadSec, $l.WorldActivateSec, $l.SectorCount, $l.SysDynMs, $l.LoadGameWorldSec, $l.GotoLoadedPosSec, $l.GotoPakDecompSec, $l.GotoSceneSetupSec)
    }
    if ($eval.BaselineDiff.Count -gt 0) {
        foreach ($d in $eval.BaselineDiff) {
            Write-Host ("  Baseline Load #{0}: {1:N3}s -> {2:N3}s ({3:+#0.000;-#0.000;0.000}s / {4:+#0.00;-#0.00;0.00}%){5}" -f `
                $d.LoadIndex, $d.BaselineSec, $d.CurrentSec, $d.DeltaSec, $d.DeltaPct, $(if ($d.Regressed) { " [REGRESSION]" } else { "" }))
        }
    }
    if ($eval.Status -in @("CRASH", "TIMEOUT") -and $eval.Diagnostic.TailLines.Count -gt 0) {
        Write-Host " Last diagnostic lines (phase=$($eval.Diagnostic.LastPhase)):"
        $eval.Diagnostic.TailLines | Select-Object -Last 10 | ForEach-Object { Write-Host "   $_" }
    }
    Write-Host " Summary JSON: $summaryPath"
    Write-Host "------------------------------------------------------------------------"

    if ($eval.ExitCode -gt $overallExitCode) {
        $overallExitCode = $eval.ExitCode
    }
}

exit $overallExitCode
