[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [uint32]$BotGuid = 0,
    [ValidateRange(1, 1440)][int]$DurationMinutes = 480,
    [ValidateRange(5, 60)][int]$PollSeconds = 20,
    [ValidateRange(5, 240)][int]$TravelEveryMinutes = 20,
    [switch]$NoTravel
)

. (Join-Path $PSScriptRoot 'Common.ps1')

$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
Initialize-AutoWoWLayout -Root $ServerRoot
$agent = Join-Path $PSScriptRoot 'autowow-agent.ps1'
if (-not (Test-Path -LiteralPath $agent)) { throw "Agent script is missing: $agent" }

$pidPath = Join-Path $ServerRoot 'autowow-agent.pid.json'
if (Test-Path -LiteralPath $pidPath) {
    $existing = Get-Content -LiteralPath $pidPath -Raw | ConvertFrom-Json
    $process = Get-Process -Id ([int]$existing.pid) -ErrorAction SilentlyContinue
    if ($process) { throw "An AutoWow agent is already running with PID $($existing.pid). Use scripts\\stop-autowow-agent.ps1 first." }
    Remove-Item -LiteralPath $pidPath -Force
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$receiptPath = Join-Path $ServerRoot "logs\\autowow-agent-$stamp.jsonl"
$stdoutPath = Join-Path $ServerRoot "logs\\autowow-agent-$stamp-stdout.log"
$stderrPath = Join-Path $ServerRoot "logs\\autowow-agent-$stamp-stderr.log"
$powershell = (Get-Command powershell.exe -ErrorAction Stop).Source

function Quote-Argument { param([string]$Value) return '"' + $Value.Replace('"', '\"') + '"' }
$arguments = @(
    '-NoLogo', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Quote-Argument $agent),
    '-ServerRoot', (Quote-Argument $ServerRoot),
    '-DurationMinutes', $DurationMinutes,
    '-PollSeconds', $PollSeconds,
    '-TravelEveryMinutes', $TravelEveryMinutes,
    '-ReceiptPath', (Quote-Argument $receiptPath)
)
if ($BotGuid -ne 0) { $arguments += @('-BotGuid', $BotGuid) }
if ($NoTravel) { $arguments += '-NoTravel' }

$process = Start-Process -FilePath $powershell -ArgumentList ($arguments -join ' ') -WorkingDirectory $ServerRoot -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath -WindowStyle Hidden -PassThru
$metadata = [ordered]@{
    pid = $process.Id
    started_utc = (Get-Date).ToUniversalTime().ToString('o')
    server_root = $ServerRoot
    duration_minutes = $DurationMinutes
    receipt_path = $receiptPath
    stdout_path = $stdoutPath
    stderr_path = $stderrPath
}
[System.IO.File]::WriteAllText($pidPath, ($metadata | ConvertTo-Json), [System.Text.UTF8Encoding]::new($false))
Start-Sleep -Seconds 2
$process.Refresh()
if ($process.HasExited) {
    Remove-Item -LiteralPath $pidPath -Force -ErrorAction SilentlyContinue
    throw "AutoWow agent exited immediately with code $($process.ExitCode). Check $stdoutPath and $stderrPath"
}
Write-Output "AutoWow agent started with PID $($process.Id). Receipts: $receiptPath"
