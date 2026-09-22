[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [ValidateRange(1, 1440)][int]$DurationMinutes = 480,
    [ValidateRange(5, 60)][int]$PollSeconds = 10
)

. (Join-Path $PSScriptRoot 'Common.ps1')

$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
Initialize-AutoWoWLayout -Root $ServerRoot
$runner = Join-Path $PSScriptRoot 'league-telemetry.ps1'
if (-not (Test-Path -LiteralPath $runner)) { throw "Telemetry runner is missing: $runner" }
$pidPath = Join-Path $ServerRoot 'league-telemetry.pid.json'
if (Test-Path -LiteralPath $pidPath) {
    $existing = Get-Content -LiteralPath $pidPath -Raw | ConvertFrom-Json
    if (Get-Process -Id ([int]$existing.pid) -ErrorAction SilentlyContinue) { throw "League telemetry is already running with PID $($existing.pid)." }
    Remove-Item -LiteralPath $pidPath -Force
}
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$receiptPath = Join-Path $ServerRoot "leagues\results\telemetry-$stamp.jsonl"
$stdoutPath = Join-Path $ServerRoot "logs\league-telemetry-$stamp-stdout.log"
$stderrPath = Join-Path $ServerRoot "logs\league-telemetry-$stamp-stderr.log"
$powershell = (Get-Command powershell.exe -ErrorAction Stop).Source
function Quote-Argument { param([string]$Value) return '"' + $Value.Replace('"', '\"') + '"' }
$arguments = @('-NoLogo','-NoProfile','-ExecutionPolicy','Bypass','-File',(Quote-Argument $runner),'-ServerRoot',(Quote-Argument $ServerRoot),'-DurationMinutes',$DurationMinutes,'-PollSeconds',$PollSeconds,'-ReceiptPath',(Quote-Argument $receiptPath))
$process = Start-Process -FilePath $powershell -ArgumentList ($arguments -join ' ') -WorkingDirectory $ServerRoot -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath -WindowStyle Hidden -PassThru
$metadata = [ordered]@{ pid = $process.Id; started_utc = (Get-Date).ToUniversalTime().ToString('o'); receipt_path = $receiptPath; stdout_path = $stdoutPath; stderr_path = $stderrPath }
[System.IO.File]::WriteAllText($pidPath, ($metadata | ConvertTo-Json), [System.Text.UTF8Encoding]::new($false))
Start-Sleep -Seconds 2
$process.Refresh()
if ($process.HasExited) {
    Remove-Item -LiteralPath $pidPath -Force -ErrorAction SilentlyContinue
    throw "League telemetry exited immediately with code $($process.ExitCode). Check $stdoutPath and $stderrPath"
}
Write-Output "League telemetry started with PID $($process.Id). Receipts: $receiptPath"
