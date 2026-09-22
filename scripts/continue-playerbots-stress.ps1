[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [ValidateRange(5, 180)][int]$WaitForCurrentTestMinutes = 60,
    [ValidateRange(4096, 16384)][int]$MinimumAvailableMemoryMiB = 5120
)

. (Join-Path $PSScriptRoot 'Common.ps1')

$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
Initialize-AutoWoWLayout -Root $ServerRoot
$pidPath = Join-Path $ServerRoot 'playerbots-load-test.pid.json'
$reportPath = Join-Path $ServerRoot 'PLAYERBOTS_LOAD_TEST_REPORT.md'
$fortyBotReportPath = Join-Path $ServerRoot 'PLAYERBOTS_LOAD_TEST_40_REPORT.md'
$logPath = Join-Path $ServerRoot ('logs\playerbots-stress-continuation-{0}.log' -f (Get-Date -Format 'yyyyMMdd-HHmmss'))

function Write-ContinuationLog {
    param([string]$Message)
    $line = '{0} {1}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz'), $Message
    Add-Content -LiteralPath $logPath -Value $line
    Write-Output $line
}

try {
    if (-not (Test-Path -LiteralPath $pidPath)) { throw 'No active Playerbots load-test PID record exists to continue from.' }
    $metadata = Get-Content -LiteralPath $pidPath -Raw | ConvertFrom-Json
    $deadline = (Get-Date).AddMinutes($WaitForCurrentTestMinutes)
    Write-ContinuationLog "Waiting for 40-bot test PID $($metadata.pid) to finish."
    while ((Get-Date) -lt $deadline) {
        if (-not (Get-Process -Id ([int]$metadata.pid) -ErrorAction SilentlyContinue)) { break }
        Start-Sleep -Seconds 30
    }
    if (Get-Process -Id ([int]$metadata.pid) -ErrorAction SilentlyContinue) { throw "Timed out waiting for 40-bot test PID $($metadata.pid)." }
    if (-not (Test-Path -LiteralPath $reportPath)) { throw 'The 40-bot test did not write a report.' }
    $report = Get-Content -LiteralPath $reportPath -Raw
    if ($report -notmatch '\| Result \| PASS \|') { throw 'The 40-bot load test did not pass; 80-bot escalation was cancelled.' }
    Copy-Item -LiteralPath $reportPath -Destination $fortyBotReportPath -Force
    Write-ContinuationLog "Preserved 40-bot report at $fortyBotReportPath."

    $available = (Get-Counter '\Memory\Available MBytes').CounterSamples[0].CookedValue
    if ($available -lt $MinimumAvailableMemoryMiB) {
        throw "Only $([math]::Round($available)) MiB is available; 80-bot escalation requires at least $MinimumAvailableMemoryMiB MiB."
    }

    Write-ContinuationLog "40-bot test passed with $([math]::Round($available)) MiB available. Starting 80-bot AV test."
    & (Join-Path $PSScriptRoot 'start-playerbots-load-test.ps1') -ServerRoot $ServerRoot -BotCount 80 -Scenario AV -WarmupMinutes 12 -ObservationMinutes 30 -KeepTier
    Write-ContinuationLog '80-bot AV test was launched. Its own guarded runner will retain 80 on PASS or restore 40 on FAIL.'
}
catch {
    Write-ContinuationLog "80-bot escalation cancelled: $($_.Exception.Message)"
    throw
}
