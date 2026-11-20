Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "autotest_parser.ps1")

function Assert-Equal($Actual, $Expected, [string]$Msg) {
    if ($Actual -ne $Expected) {
        throw "FAIL: $Msg (expected='$Expected', actual='$Actual')"
    }
}

$tmp = Join-Path ([IO.Path]::GetTempPath()) ("g3o_autotest_parser_test_" + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Path $tmp -Force | Out-Null

try {
    $lp1 = Join-Path $tmp "G3O_LoadProfiler_20261119_120000_000_1234_1_1.log"
    $lp2 = Join-Path $tmp "G3O_LoadProfiler_20261119_120020_000_1234_1_2.log"
    @"
================================================================================
[LoadProfile] === DETAILED SAVEGAME LOAD BENCHMARK (Total: 25.500 s) ===
--------------------------------------------------------------------------------
Detailed Phase Breakdown:
  1. Pre-Load Preparation:          10.200 s ( 40.0%)
       * gCWorld::Activate:          9.500 s [345 sectors, 9.400 s, avg 27.2 ms/sector]
       * Top 1 Slowest Sectors in Activation:
           [ 1] SysDyn_{9A103CC2-4190-4DB3-9618-0419E5445AAD} 5800.25 ms
  2. gCWorld::LoadGameWorld:         2.100 s (  8.2%) [10 pak blocks, 5.00 MB decomp, 12.00 MB read]
  3. Post-Load Processing:          13.200 s ( 51.8%)
       * gCSession::GotoLoadedPos:  12.500 s [3000 pak blocks, 2800.00 MB decomp, 1700.00 MB read]
           - Pak Decompress (libdeflate):  6.000 s
           - Disk ReadFile I/O:            1.500 s
           - Scene & Entity Setup:         5.000 s
       * gCSession::Start (World):   0.500 s [0 pak blocks, 0.00 MB read]
Subsystem Activity:
  - Disk ReadFile I/O:               1.750 s (  6.9%) [40000 calls, 1800.00 MB read, speed 1028.6 MB/s]
  - Pak Decompress (libdeflate):     6.100 s ( 23.9%) [3010 blocks, 1600.00 MB -> 2805.00 MB]
  - Save Decompress (libdeflate):    0.200 s (  0.8%) [12 chunks, 5.00 MB -> 60.00 MB]
================================================================================
"@ | Set-Content -LiteralPath $lp1 -Encoding UTF8

    @"
[LoadProfile] === DETAILED SAVEGAME LOAD BENCHMARK (Total: 14.000 s) ===
  1. Pre-Load Preparation:           0.500 s (  3.6%)
       * gCWorld::Activate:          0.001 s [0 sectors, 0.000 s, avg 0.0 ms/sector]
  2. gCWorld::LoadGameWorld:        10.000 s ( 71.4%)
       * gCSession::GotoLoadedPos:   3.000 s
           - Pak Decompress (libdeflate):  1.000 s
           - Disk ReadFile I/O:            0.500 s
           - Scene & Entity Setup:         1.500 s
       * gCSession::Start (World):   0.400 s
"@ | Set-Content -LiteralPath $lp2 -Encoding UTF8

    $diagPass = Join-Path $tmp "diag_pass.log"
    @"
[12:00:00.000] [TID:1] [Init] [AutoTest] START runId=r1 pid=1234 slot=Autosave_Slot_0001 loads=2
[12:00:05.000] [TID:1] [Init] [AutoTest] MENU_READY elapsedMs=5000
[12:00:45.000] [TID:1] [Gameplay] [AutoTest] DONE loadsCompleted=2/2 exitCode=0
"@ | Set-Content -LiteralPath $diagPass -Encoding UTF8

    # 1. PASS scenario
    $rPass = Evaluate-AutoTestRun -ProcessExitCode 0 -TimedOut $false -ExpectedLoads 2 `
        -DiagnosticLogPath $diagPass -LoadProfilerPaths @($lp1, $lp2)
    Assert-Equal $rPass.Status "PASS" "PASS status"
    Assert-Equal $rPass.ExitCode 0 "PASS exit code"
    Assert-Equal $rPass.Loads.Count 2 "2 loads parsed"
    Assert-Equal $rPass.Loads[0].TotalSec 25.5 "Load #1 TotalSec"
    Assert-Equal $rPass.Loads[0].SysDynMs 5800.25 "Load #1 SysDynMs"
    Assert-Equal $rPass.Loads[1].TotalSec 14.0 "Load #2 TotalSec"

    # 2. FAIL scenario (baseline regression > 10%)
    $baseJson = Join-Path $tmp "baseline.json"
    @{ Loads = @(@{ LoadIndex = 1; TotalSec = 20.0 }, @{ LoadIndex = 2; TotalSec = 14.0 }) } |
        ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $baseJson -Encoding UTF8
    $rReg = Evaluate-AutoTestRun -ProcessExitCode 0 -TimedOut $false -ExpectedLoads 2 `
        -DiagnosticLogPath $diagPass -LoadProfilerPaths @($lp1, $lp2) -BaselinePath $baseJson -MaxRegressionPct 10.0
    Assert-Equal $rReg.Status "FAIL" "Regression should FAIL"
    Assert-Equal $rReg.ExitCode 1 "Regression exit code 1"

    # 3. CRASH scenario
    $crashFile = Join-Path $tmp "G3O_CrashReport_20261119_120010_000_1234_1_1.log"
    "CRASH" | Set-Content -LiteralPath $crashFile -Encoding UTF8
    $rCrash = Evaluate-AutoTestRun -ProcessExitCode 21 -TimedOut $false -ExpectedLoads 2 `
        -DiagnosticLogPath $diagPass -LoadProfilerPaths @($lp1) -CrashReportPaths @($crashFile)
    Assert-Equal $rCrash.Status "CRASH" "CRASH status"
    Assert-Equal $rCrash.ExitCode 2 "CRASH exit code 2"

    # 4. TIMEOUT scenario
    $rTimeout = Evaluate-AutoTestRun -ProcessExitCode -1 -TimedOut $true -ExpectedLoads 2 `
        -DiagnosticLogPath $diagPass -LoadProfilerPaths @($lp1)
    Assert-Equal $rTimeout.Status "TIMEOUT" "TIMEOUT status"
    Assert-Equal $rTimeout.ExitCode 3 "TIMEOUT exit code 3"

    Write-Host "PASS: autotest_parser verified (PASS, FAIL/regression, CRASH, TIMEOUT)"
} finally {
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}
