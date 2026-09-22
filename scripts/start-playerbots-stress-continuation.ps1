[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot)
)

. (Join-Path $PSScriptRoot 'Common.ps1')

$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
Initialize-AutoWoWLayout -Root $ServerRoot
$runner = Join-Path $PSScriptRoot 'continue-playerbots-stress.ps1'
if (-not (Test-Path -LiteralPath $runner)) { throw "Stress continuation runner is missing: $runner" }
$pidPath = Join-Path $ServerRoot 'playerbots-stress-continuation.pid.json'
if (Test-Path -LiteralPath $pidPath) {
    $existing = Get-Content -LiteralPath $pidPath -Raw | ConvertFrom-Json
    if (Get-Process -Id ([int]$existing.pid) -ErrorAction SilentlyContinue) { throw "A stress continuation is already running with PID $($existing.pid)." }
    Remove-Item -LiteralPath $pidPath -Force
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$stdoutPath = Join-Path $ServerRoot "logs\playerbots-stress-continuation-$stamp-stdout.log"
$stderrPath = Join-Path $ServerRoot "logs\playerbots-stress-continuation-$stamp-stderr.log"
$powershell = (Get-Command powershell.exe -ErrorAction Stop).Source
function Quote-Argument { param([string]$Value) return '"' + $Value.Replace('"', '\"') + '"' }
$arguments = @('-NoLogo','-NoProfile','-ExecutionPolicy','Bypass','-File',(Quote-Argument $runner),'-ServerRoot',(Quote-Argument $ServerRoot))
$process = Start-Process -FilePath $powershell -ArgumentList ($arguments -join ' ') -WorkingDirectory $ServerRoot -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath -WindowStyle Hidden -PassThru
$metadata = [ordered]@{ pid = $process.Id; started_utc = (Get-Date).ToUniversalTime().ToString('o'); stdout_path = $stdoutPath; stderr_path = $stderrPath }
[System.IO.File]::WriteAllText($pidPath, ($metadata | ConvertTo-Json), [System.Text.UTF8Encoding]::new($false))
Start-Sleep -Seconds 2
$process.Refresh()
if ($process.HasExited) {
    Remove-Item -LiteralPath $pidPath -Force -ErrorAction SilentlyContinue
    throw "Stress continuation exited immediately with code $($process.ExitCode). Check $stdoutPath and $stderrPath"
}
Write-Output "Playerbots stress continuation started with PID $($process.Id)."
