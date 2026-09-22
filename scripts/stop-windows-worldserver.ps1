[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot)
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
$pidFile = Join-Path $ServerRoot 'worldserver.pid'

if (-not (Test-Path -LiteralPath $pidFile -PathType Leaf)) {
    throw "Missing authoritative PID file: $pidFile"
}

$processId = [int]([System.IO.File]::ReadAllText($pidFile).Trim())
$process = Get-Process -Id $processId -ErrorAction SilentlyContinue
if (-not $process) {
    Remove-Item -LiteralPath $pidFile -Force
    Write-Output "Windows worldserver PID $processId was already stopped."
    return
}
if ($process.ProcessName -ne 'worldserver') {
    throw "PID $processId is $($process.ProcessName), not worldserver; refusing to stop it."
}

$ownedPorts = @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue |
    Where-Object { $_.OwningProcess -eq $processId -and $_.LocalPort -in @(8085, 18787) })
if (-not $ownedPorts) {
    throw "PID $processId does not own the expected world/bridge listeners; refusing to stop it."
}

Stop-Process -Id $processId -ErrorAction Stop
if (-not $process.WaitForExit(30000)) {
    throw "Windows worldserver PID $processId did not stop within 30 seconds."
}
Remove-Item -LiteralPath $pidFile -Force
Write-Output "Windows worldserver stopped (PID $processId); authserver was not touched."

