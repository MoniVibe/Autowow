[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [ValidateRange(1, 80)][int]$BotCount = 40,
    [ValidateSet('World','WSG','AV')][string]$Scenario = 'WSG',
    [ValidateRange(2, 20)][int]$WarmupMinutes = 10,
    [ValidateRange(1, 120)][int]$ObservationMinutes = 20,
    [switch]$KeepTier
)

. (Join-Path $PSScriptRoot 'Common.ps1')

$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
Initialize-AutoWoWLayout -Root $ServerRoot
$runner = Join-Path $PSScriptRoot 'playerbots-load-test.ps1'
if (-not (Test-Path -LiteralPath $runner)) { throw "Load-test runner is missing: $runner" }
$pidPath = Join-Path $ServerRoot 'playerbots-load-test.pid.json'
if (Test-Path -LiteralPath $pidPath) {
    $existing = Get-Content -LiteralPath $pidPath -Raw | ConvertFrom-Json
    if (Get-Process -Id ([int]$existing.pid) -ErrorAction SilentlyContinue) { throw "A Playerbots load test is already running with PID $($existing.pid)." }
    Remove-Item -LiteralPath $pidPath -Force
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$stdoutPath = Join-Path $ServerRoot "logs\playerbots-load-test-$stamp-stdout.log"
$stderrPath = Join-Path $ServerRoot "logs\playerbots-load-test-$stamp-stderr.log"
$powershell = (Get-Command powershell.exe -ErrorAction Stop).Source
function Quote-Argument { param([string]$Value) return '"' + $Value.Replace('"', '\"') + '"' }
$arguments = @('-NoLogo','-NoProfile','-ExecutionPolicy','Bypass','-File',(Quote-Argument $runner),'-ServerRoot',(Quote-Argument $ServerRoot),'-BotCount',$BotCount,'-Scenario',$Scenario,'-WarmupMinutes',$WarmupMinutes,'-ObservationMinutes',$ObservationMinutes)
if ($KeepTier) { $arguments += '-KeepTier' }
$process = Start-Process -FilePath $powershell -ArgumentList ($arguments -join ' ') -WorkingDirectory $ServerRoot -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath -WindowStyle Hidden -PassThru
$metadata = [ordered]@{ pid = $process.Id; started_utc = (Get-Date).ToUniversalTime().ToString('o'); bot_count = $BotCount; scenario = $Scenario; stdout_path = $stdoutPath; stderr_path = $stderrPath }
[System.IO.File]::WriteAllText($pidPath, ($metadata | ConvertTo-Json), [System.Text.UTF8Encoding]::new($false))
Start-Sleep -Seconds 2
$process.Refresh()
if ($process.HasExited) {
    Remove-Item -LiteralPath $pidPath -Force -ErrorAction SilentlyContinue
    throw "Load test exited immediately with code $($process.ExitCode). Check $stdoutPath and $stderrPath"
}
Write-Output "Playerbots load test started with PID $($process.Id). Logs: $stdoutPath and $stderrPath"
