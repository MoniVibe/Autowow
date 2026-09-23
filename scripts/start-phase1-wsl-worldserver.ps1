[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$WslDistro = 'Ubuntu-24.04',
    [string]$WorldserverBinary = '/root/p1core/build/src/server/apps/worldserver',
    [ValidateRange(30, 300)][int]$StartupTimeoutSeconds = 180
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
$binary = $WorldserverBinary
$config = '/root/p1runtime/worldserver.conf'
$statePath = Join-Path $ServerRoot 'work\phase1-wsl-runtime\runtime-processes.json'
$relayStatusPath = Join-Path $ServerRoot 'work\phase1-wsl-runtime\mysql-relay.json'
$relayScript = Join-Path $PSScriptRoot 'phase1-wsl-mysql-relay.ps1'
$logRoot = Join-Path $ServerRoot 'logs\phase1-runtime'
$stdout = Join-Path $logRoot 'worldserver-wsl-stdout.log'
$stderr = Join-Path $logRoot 'worldserver-wsl-stderr.log'
$relayStdout = Join-Path $logRoot 'mysql-relay-stdout.log'
$relayStderr = Join-Path $logRoot 'mysql-relay-stderr.log'

if (Get-Process -Name worldserver -ErrorAction SilentlyContinue) {
    throw 'A Windows worldserver is still running; refusing dual startup.'
}
$linuxWorldserver = (& wsl.exe -d $WslDistro -u root -- pgrep -x worldserver 2>$null)
if ($LASTEXITCODE -eq 0 -and $linuxWorldserver) {
    throw "A WSL worldserver is already running (pid $linuxWorldserver)."
}
if (Get-NetTCPConnection -State Listen -LocalPort 8085,18787 -ErrorAction SilentlyContinue) {
    throw 'Worldserver ports 8085/18787 are not free.'
}

$authPidFile = Join-Path $ServerRoot 'authserver.pid'
if (-not (Test-Path -LiteralPath $authPidFile)) { throw 'Missing authserver.pid.' }
$authId = [int]([System.IO.File]::ReadAllText($authPidFile).Trim())
$auth = Get-Process -Id $authId -ErrorAction SilentlyContinue
if (-not $auth -or $auth.ProcessName -ne 'authserver') { throw 'Windows authserver is not running.' }

foreach ($linuxPath in @($binary, $config, '/usr/local/etc/modules/playerbots.conf')) {
    & wsl.exe -d $WslDistro -u root -- test -r $linuxPath
    if ($LASTEXITCODE -ne 0) { throw "Missing WSL runtime prerequisite: $linuxPath" }
}

New-Item -ItemType Directory -Path $logRoot,(Split-Path -Parent $statePath) -Force | Out-Null
Remove-Item -LiteralPath $relayStatusPath -Force -ErrorAction SilentlyContinue
# A relay left behind by a crashed/killed world holds the port; no world runs here (checked above), so stop it.
Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
    Where-Object { $_.CommandLine -and $_.CommandLine -match 'phase1-wsl-mysql-relay\.ps1' } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Start-Sleep -Milliseconds 500
$relay = Start-Process -FilePath 'pwsh.exe' -ArgumentList @('-NoProfile','-File',$relayScript) `
    -RedirectStandardOutput $relayStdout -RedirectStandardError $relayStderr `
    -WindowStyle Hidden -PassThru

$world = $null
try {
    $relayDeadline = (Get-Date).AddSeconds(20)
    do {
        Start-Sleep -Milliseconds 250
        $relay.Refresh()
        $relayReady = Test-Path -LiteralPath $relayStatusPath
    } while (-not $relayReady -and -not $relay.HasExited -and (Get-Date) -lt $relayDeadline)
    if ($relay.HasExited -or -not $relayReady) { throw 'Private MySQL relay failed to start.' }

    $world = Start-Process -FilePath 'wsl.exe' `
        -ArgumentList @('-d',$WslDistro,'-u','root','--',$binary,'-c',$config) `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr -WindowStyle Hidden -PassThru

    $state = [ordered]@{
        schema = 'autowow.phase1.wsl-processes.v1'
        wsl_wrapper_pid = $world.Id
        relay_pid = $relay.Id
        distro = $WslDistro
        binary = $binary
        config = $config
        started_utc = (Get-Date).ToUniversalTime().ToString('o')
    }
    [System.IO.File]::WriteAllText(
        $statePath,
        ([pscustomobject]$state | ConvertTo-Json -Depth 4),
        [System.Text.UTF8Encoding]::new($false))

    $deadline = (Get-Date).AddSeconds($StartupTimeoutSeconds)
    $bridgeResponse = $null
    do {
        Start-Sleep -Seconds 1
        $world.Refresh()
        if ($world.HasExited) {
            throw "WSL worldserver exited with code $($world.ExitCode). See $stdout and $stderr"
        }

        $client = [System.Net.Sockets.TcpClient]::new()
        try {
            if ($client.ConnectAsync('127.0.0.1', 18787).Wait(1000) -and $client.Connected) {
                $stream = $client.GetStream()
                $writer = [System.IO.StreamWriter]::new($stream)
                $writer.NewLine = "`n"
                $writer.WriteLine('list')
                $writer.Flush()
                $reader = [System.IO.StreamReader]::new($stream)
                $readTask = $reader.ReadLineAsync()
                if ($readTask.Wait(3000)) { $bridgeResponse = $readTask.Result }
            }
        }
        catch { }
        finally { $client.Dispose() }
    } while ([string]::IsNullOrWhiteSpace($bridgeResponse) -and (Get-Date) -lt $deadline)

    if ([string]::IsNullOrWhiteSpace($bridgeResponse)) {
        throw "WSL worldserver bridge did not become ready within $StartupTimeoutSeconds seconds."
    }
    $bridge = $bridgeResponse | ConvertFrom-Json
    if (-not $bridge.ok) { throw 'WSL worldserver bridge returned a non-ok list response.' }

    Write-Output "Phase 1 WSL worldserver ready (wrapper PID $($world.Id), relay PID $($relay.Id), bots $(@($bridge.bots).Count))."
}
catch {
    & wsl.exe -d $WslDistro -u root -- pkill -TERM -x worldserver 2>$null
    if ($world -and -not $world.HasExited) { Stop-Process -Id $world.Id -Force -ErrorAction SilentlyContinue }
    if (-not $relay.HasExited) { Stop-Process -Id $relay.Id -Force -ErrorAction SilentlyContinue }
    Remove-Item -LiteralPath $statePath,$relayStatusPath -Force -ErrorAction SilentlyContinue
    throw
}
