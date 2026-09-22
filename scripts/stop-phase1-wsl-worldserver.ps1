[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$WslDistro = 'Ubuntu-24.04'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
$statePath = Join-Path $ServerRoot 'work\phase1-wsl-runtime\runtime-processes.json'
$relayStatusPath = Join-Path $ServerRoot 'work\phase1-wsl-runtime\mysql-relay.json'
$state = $null
if (Test-Path -LiteralPath $statePath) {
    $state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
}

& wsl.exe -d $WslDistro -u root -- pkill -TERM -x worldserver 2>$null
$deadline = (Get-Date).AddSeconds(30)
do {
    Start-Sleep -Milliseconds 500
    & wsl.exe -d $WslDistro -u root -- pgrep -x worldserver *> $null
    $running = $LASTEXITCODE -eq 0
} while ($running -and (Get-Date) -lt $deadline)
if ($running) {
    & wsl.exe -d $WslDistro -u root -- pkill -KILL -x worldserver 2>$null
    Start-Sleep -Seconds 1
}

if ($state) {
    foreach ($processId in @([int]$state.wsl_wrapper_pid, [int]$state.relay_pid)) {
        $process = Get-Process -Id $processId -ErrorAction SilentlyContinue
        if ($process) { Stop-Process -Id $processId -Force -ErrorAction SilentlyContinue }
    }
}

Remove-Item -LiteralPath $statePath,$relayStatusPath -Force -ErrorAction SilentlyContinue
Write-Output 'Phase 1 WSL worldserver and private MySQL relay are stopped.'
