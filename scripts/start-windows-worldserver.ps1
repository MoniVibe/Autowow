[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [ValidateRange(15, 300)][int]$StartupTimeoutSeconds = 120
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
$installRoot = Join-Path $ServerRoot 'server'
$worldExecutable = Join-Path $installRoot 'worldserver.exe'
$worldPidFile = Join-Path $ServerRoot 'worldserver.pid'
$authPidFile = Join-Path $ServerRoot 'authserver.pid'

foreach ($requiredPath in @($worldExecutable, $authPidFile)) {
    if (-not (Test-Path -LiteralPath $requiredPath)) { throw "Missing prerequisite: $requiredPath" }
}

$authId = [int]([System.IO.File]::ReadAllText($authPidFile).Trim())
$auth = Get-Process -Id $authId -ErrorAction SilentlyContinue
if (-not $auth -or $auth.ProcessName -ne 'authserver') { throw 'Windows authserver is not running.' }

$linuxWorldserver = (& wsl.exe -u root -- pgrep -x worldserver 2>$null)
if ($LASTEXITCODE -eq 0 -and $linuxWorldserver) {
    throw "WSL worldserver is still running (pid $linuxWorldserver); refusing dual startup."
}

$existing = @(Get-Process -Name worldserver -ErrorAction SilentlyContinue)
if ($existing.Count -gt 0) {
    throw "A Windows worldserver is already running (pid(s) $($existing.Id -join ','))."
}
if (Get-NetTCPConnection -State Listen -LocalPort 8085,18787 -ErrorAction SilentlyContinue) {
    throw 'Worldserver ports 8085/18787 are not free.'
}

$stdout = Join-Path $installRoot 'logs\worldserver-stdout.log'
$stderr = Join-Path $installRoot 'logs\worldserver-stderr.log'
$process = Start-Process -FilePath $worldExecutable -WorkingDirectory $installRoot `
    -RedirectStandardOutput $stdout -RedirectStandardError $stderr -WindowStyle Hidden -PassThru
[System.IO.File]::WriteAllText($worldPidFile, [string]$process.Id, [System.Text.UTF8Encoding]::new($false))

try {
    $deadline = (Get-Date).AddSeconds($StartupTimeoutSeconds)
    do {
        Start-Sleep -Milliseconds 500
        $process.Refresh()
        if ($process.HasExited) {
            throw "Windows worldserver exited with code $($process.ExitCode). See $stdout and $stderr"
        }
        $worldReady = $null -ne (Get-NetTCPConnection -State Listen -LocalPort 8085 -ErrorAction SilentlyContinue)
        $bridgeReady = $null -ne (Get-NetTCPConnection -State Listen -LocalPort 18787 -ErrorAction SilentlyContinue)
    } while ((-not $worldReady -or -not $bridgeReady) -and (Get-Date) -lt $deadline)

    if (-not $worldReady -or -not $bridgeReady) {
        throw "Windows worldserver did not expose ports 8085 and 18787 within $StartupTimeoutSeconds seconds."
    }
}
catch {
    if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue }
    Remove-Item -LiteralPath $worldPidFile -Force -ErrorAction SilentlyContinue
    throw
}

Write-Output "Windows worldserver restored (PID $($process.Id)); ports 8085 and 18787 are listening."
