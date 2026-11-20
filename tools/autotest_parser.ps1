# ==============================================================================
# Gothic 3 Optima (G3O) — AutoTest Log Parser & Evaluator
# ==============================================================================
Set-StrictMode -Version Latest

function Parse-LoadProfilerLog {
    param([Parameter(Mandatory)][string]$Path)

    $res = [ordered]@{
        Path             = $Path
        TotalSec         = $null
        PreLoadSec       = $null
        WorldActivateSec = $null
        SectorCount      = $null
        SysDynMs         = $null
        LoadGameWorldSec = $null
        GotoLoadedPosSec = $null
        GotoPakDecompSec = $null
        GotoReadFileSec  = $null
        GotoSceneSetupSec = $null
        SessionStartSec  = $null
        ReadFileTotalSec = $null
        ReadFileMB       = $null
        ReadFileSpeedMBs = $null
        PakBlocks        = $null
        PakOutMB         = $null
        SaveChunks       = $null
    }
    if (-not (Test-Path -LiteralPath $Path)) { return [pscustomobject]$res }

    $lines = Get-Content -LiteralPath $Path -ErrorAction SilentlyContinue
    foreach ($line in $lines) {
        if ($line -match 'DETAILED SAVEGAME LOAD BENCHMARK \(Total:\s*([0-9.]+)\s*s\)') {
            $res.TotalSec = [double]$Matches[1]
        } elseif ($line -match '1\.\s+Pre-Load Preparation:\s+([0-9.]+)\s*s') {
            $res.PreLoadSec = [double]$Matches[1]
        } elseif ($line -match 'gCWorld::Activate:\s+([0-9.]+)\s*s\s*\[(\d+)\s+sectors') {
            $res.WorldActivateSec = [double]$Matches[1]
            $res.SectorCount = [int]$Matches[2]
        } elseif ($line -match 'SysDyn_[^\s]+\s+([0-9.]+)\s*ms') {
            $res.SysDynMs = [double]$Matches[1]
        } elseif ($line -match '2\.\s+gCWorld::LoadGameWorld:\s+([0-9.]+)\s*s') {
            $res.LoadGameWorldSec = [double]$Matches[1]
        } elseif ($line -match 'gCSession::GotoLoadedPos:\s+([0-9.]+)\s*s') {
            $res.GotoLoadedPosSec = [double]$Matches[1]
        } elseif ($line -match '-\s+Pak Decompress \(libdeflate\):\s+([0-9.]+)\s*s$') {
            $res.GotoPakDecompSec = [double]$Matches[1]
        } elseif ($line -match '-\s+Disk ReadFile I/O:\s+([0-9.]+)\s*s$') {
            $res.GotoReadFileSec = [double]$Matches[1]
        } elseif ($line -match '-\s+Scene & Entity Setup:\s+([0-9.]+)\s*s') {
            $res.GotoSceneSetupSec = [double]$Matches[1]
        } elseif ($line -match 'gCSession::Start \(World\):\s+([0-9.]+)\s*s') {
            $res.SessionStartSec = [double]$Matches[1]
        } elseif ($line -match 'Disk ReadFile I/O:\s+([0-9.]+)\s*s\s*\([^)]+\)\s*\[\d+\s+calls,\s*([0-9.]+)\s*MB read,\s*(?:avg\s+)?speed\s*([0-9.]+)\s*MB/s\]') {
            $res.ReadFileTotalSec = [double]$Matches[1]
            $res.ReadFileMB = [double]$Matches[2]
            $res.ReadFileSpeedMBs = [double]$Matches[3]
        } elseif ($line -match 'Pak Decompress \(libdeflate\):\s+[0-9.]+\s*s\s*\([^)]+\)\s*\[(\d+)\s+blocks,\s*[0-9.]+\s*MB\s*->\s*([0-9.]+)\s*MB\]') {
            $res.PakBlocks = [int]$Matches[1]
            $res.PakOutMB = [double]$Matches[2]
        } elseif ($line -match 'Save Decompress \(libdeflate\):.*\[(\d+)\s+chunks') {
            $res.SaveChunks = [int]$Matches[1]
        }
    }
    return [pscustomobject]$res
}

function Parse-DiagnosticLog {
    param([string]$Path)

    $res = [ordered]@{
        Path            = $Path
        DoneMarker      = $false
        AbortMarker     = $false
        AbortReason     = ""
        MenuReadyMs     = $null
        LastPhase       = "Unknown"
        HookFailures    = 0
        TextureFailures = 0
        FatalErrors     = 0
        CrashGuards     = 0
        DecompFallbacks = 0
        TailLines       = @()
    }
    if (-not $Path -or -not (Test-Path -LiteralPath $Path)) { return [pscustomobject]$res }

    $lines = @(Get-Content -LiteralPath $Path -ErrorAction SilentlyContinue)
    foreach ($line in $lines) {
        if ($line -match '^\[[^\]]+\]\s+\[TID:\d+\]\s+\[([^\]]+)\]') {
            $res.LastPhase = $Matches[1]
        }
        if ($line -match '\[AutoTest\]\s+DONE\b') {
            $res.DoneMarker = $true
        }
        if ($line -match '\[AutoTest\]\s+ABORT\s+reason=([^\s]+)') {
            $res.AbortMarker = $true
            $res.AbortReason = $Matches[1]
        }
        if ($line -match '\[AutoTest\]\s+MENU_READY\s+elapsedMs=(\d+)') {
            $res.MenuReadyMs = [int64]$Matches[1]
        }
        if ($line -match 'hook (creation|enable) failed|Failed to install hook') {
            $res.HookFailures++
        }
        if ($line -match '\[FAILED hr=0x') {
            $res.TextureFailures++
        }
        if ($line -match '\[AutoTest\]\s+ABORT\s+reason=engine_fatal_error|\[EngineFatal\]|FATAL ERROR:') {
            $res.FatalErrors++
        }
        if ($line -match 'CrashGuard') {
            $res.CrashGuards++
        }
        if ($line -match 'libdeflate.*fallback|raw-DEFLATE fallback') {
            $res.DecompFallbacks++
        }
    }

    if ($lines.Count -gt 0) {
        $start = [Math]::Max(0, $lines.Count - 20)
        $res.TailLines = @($lines[$start..($lines.Count - 1)])
    }
    return [pscustomobject]$res
}

function Evaluate-AutoTestRun {
    param(
        [Parameter(Mandatory)][int]$ProcessExitCode,
        [Parameter(Mandatory)][bool]$TimedOut,
        [Parameter(Mandatory)][int]$ExpectedLoads,
        [string]$AutoTestJsonPath = "",
        [string]$DiagnosticLogPath = "",
        [string[]]$LoadProfilerPaths = @(),
        [string[]]$CrashReportPaths = @(),
        [string]$BaselinePath = "",
        [double]$MaxRegressionPct = 10.0
    )

    $diag = Parse-DiagnosticLog -Path $DiagnosticLogPath
    $autoJson = $null
    if ($AutoTestJsonPath -and (Test-Path -LiteralPath $AutoTestJsonPath)) {
        try {
            $autoJson = Get-Content -LiteralPath $AutoTestJsonPath -Raw | ConvertFrom-Json
        } catch {
            $autoJson = $null
        }
    }

    $sortedProfiles = @($LoadProfilerPaths | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Sort-Object)
    $parsedLoads = @()
    $idx = 1
    foreach ($p in $sortedProfiles) {
        $m = Parse-LoadProfilerLog -Path $p
        $m | Add-Member -NotePropertyName LoadIndex -NotePropertyValue $idx -Force
        $parsedLoads += $m
        $idx++
    }

    $crashReports = @($CrashReportPaths | Where-Object { $_ -and (Test-Path -LiteralPath $_) })
    $reasons = New-Object System.Collections.Generic.List[string]
    $status = "PASS"
    $evalExitCode = 0

    $isWin32CrashCode = (-not $TimedOut) -and ($ProcessExitCode -eq 20 -or $ProcessExitCode -eq 21 -or $ProcessExitCode -lt 0 -or $ProcessExitCode -ge 0xC0000000L)

    if ($crashReports.Count -gt 0 -or $isWin32CrashCode -or $diag.FatalErrors -gt 0) {
        $status = "CRASH"
        $evalExitCode = 2
        if ($crashReports.Count -gt 0) { $reasons.Add("crash_report_found:$($crashReports.Count)") }
        if ($isWin32CrashCode) { $reasons.Add("crash_exit_code:$ProcessExitCode") }
        if ($diag.FatalErrors -gt 0) { $reasons.Add("engine_fatal_errors:$($diag.FatalErrors)") }
    } elseif ($TimedOut -or $ProcessExitCode -eq 12) {
        $status = "TIMEOUT"
        $evalExitCode = 3
        if ($TimedOut) { $reasons.Add("watchdog_timeout") }
        if ($ProcessExitCode -eq 12) { $reasons.Add("dll_internal_timeout") }
    } else {
        if ($ProcessExitCode -ne 0) {
            $reasons.Add("nonzero_exit_code:$ProcessExitCode")
        }
        $hasDone = $diag.DoneMarker -or ($autoJson -and $autoJson.status -eq "DONE")
        if (-not $hasDone) {
            $reasons.Add("missing_autotest_done_marker")
        }
        if ($diag.AbortMarker) {
            $reasons.Add("autotest_aborted:$($diag.AbortReason)")
        }
        $completedLoads = if ($autoJson -and $null -ne $autoJson.loadsCompleted) {
            [int]$autoJson.loadsCompleted
        } else {
            $parsedLoads.Count
        }
        if ($completedLoads -lt $ExpectedLoads) {
            $reasons.Add("loads_completed_mismatch:${completedLoads}/${ExpectedLoads}")
        }
        if ($diag.HookFailures -gt 0) {
            $reasons.Add("hook_failures:$($diag.HookFailures)")
        }
        if ($reasons.Count -gt 0) {
            $status = "FAIL"
            $evalExitCode = 1
        }
    }

    # Optional baseline comparison (Load #1 vs Load #1, Load #2 vs Load #2, etc.)
    $baselineDiff = @()
    if ($BaselinePath -and (Test-Path -LiteralPath $BaselinePath)) {
        try {
            $baseObj = Get-Content -LiteralPath $BaselinePath -Raw | ConvertFrom-Json
            $baseLoads = @($baseObj.Loads)
            foreach ($cur in $parsedLoads) {
                $match = $baseLoads | Where-Object { [int]$_.LoadIndex -eq [int]$cur.LoadIndex } | Select-Object -First 1
                if ($match -and $null -ne $match.TotalSec -and $null -ne $cur.TotalSec -and [double]$match.TotalSec -gt 0) {
                    $deltaSec = [Math]::Round(([double]$cur.TotalSec - [double]$match.TotalSec), 3)
                    $deltaPct = [Math]::Round(($deltaSec / [double]$match.TotalSec) * 100.0, 2)
                    $regressed = ($deltaPct -gt $MaxRegressionPct)
                    $baselineDiff += [pscustomobject]@{
                        LoadIndex    = $cur.LoadIndex
                        BaselineSec  = [double]$match.TotalSec
                        CurrentSec   = [double]$cur.TotalSec
                        DeltaSec     = $deltaSec
                        DeltaPct     = $deltaPct
                        Regressed    = $regressed
                    }
                    if ($regressed -and $status -eq "PASS") {
                        $status = "FAIL"
                        $evalExitCode = 1
                        $reasons.Add("regression_load_$($cur.LoadIndex):+${deltaPct}%>max_${MaxRegressionPct}%")
                    }
                }
            }
        } catch {
            $reasons.Add("baseline_parse_error:$($_.Exception.Message)")
        }
    }

    return [pscustomobject]@{
        Status            = $status
        ExitCode          = $evalExitCode
        ProcessExitCode   = $ProcessExitCode
        TimedOut          = $TimedOut
        ExpectedLoads     = $ExpectedLoads
        CompletedLoads    = $parsedLoads.Count
        Reasons           = @($reasons)
        Diagnostic        = $diag
        AutoTestJson      = $autoJson
        Loads             = @($parsedLoads)
        CrashReports      = @($crashReports)
        BaselineDiff      = @($baselineDiff)
    }
}
