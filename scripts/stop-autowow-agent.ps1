[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot)
)

. (Join-Path $PSScriptRoot 'Common.ps1')

$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
$pidPath = Join-Path $ServerRoot 'autowow-agent.pid.json'
if (-not (Test-Path -LiteralPath $pidPath)) {
    Write-Output 'No AutoWow agent PID record exists.'
    return
}

$metadata = Get-Content -LiteralPath $pidPath -Raw | ConvertFrom-Json
$agentPid = [int]$metadata.pid
$process = Get-Process -Id $agentPid -ErrorAction SilentlyContinue
if ($process) {
    $processInfo = Get-CimInstance Win32_Process -Filter "ProcessId = $agentPid" -ErrorAction Stop
    $commandLine = [string]$processInfo.CommandLine
    if ($commandLine -notmatch [regex]::Escape('autowow-agent.ps1') -or $commandLine -notmatch [regex]::Escape($ServerRoot)) {
        throw "PID $agentPid does not look like this AutoWow agent. It was not stopped."
    }
    Stop-Process -Id $agentPid -ErrorAction Stop
    $process.WaitForExit(10000)
    Write-Output "AutoWow agent stopped (PID $agentPid)."
}
Remove-Item -LiteralPath $pidPath -Force
Write-Output "Receipts: $($metadata.receipt_path)"
