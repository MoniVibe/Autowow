[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$WslDistro = 'Ubuntu-24.04',
    [string]$WorldserverBinary = '/root/p1core/build/src/server/apps/worldserver',
    [ValidateRange(30, 300)][int]$StartupTimeoutSeconds = 180,
    [string]$AuthserverBinary = '',
    [string]$AuthserverConfig = '',
    [Parameter(DontShow)][switch]$EmitInvocationPlan,
    [Parameter(DontShow)][hashtable]$TestHooks
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$authBinarySpecified = -not [string]::IsNullOrWhiteSpace($AuthserverBinary)
$authConfigSpecified = -not [string]::IsNullOrWhiteSpace($AuthserverConfig)
if ($authBinarySpecified -xor $authConfigSpecified) {
    throw 'AuthserverBinary and AuthserverConfig must be supplied together.'
}
$useWslAuth = $authBinarySpecified -and $authConfigSpecified

if ($EmitInvocationPlan) {
    $startup = if ($useWslAuth) {
        @('preflight:no-windows-auth', 'preflight:no-wsl-auth', 'relay:start', 'relay:ready',
          'auth:start', 'auth:exact-pid-and-port-3724-ready', 'world:start', 'world:ready')
    }
    else {
        @('preflight:windows-auth-running', 'relay:start', 'relay:ready', 'world:start', 'world:ready')
    }
    $cleanup = if ($useWslAuth) {
        @('world:stop-owned', 'auth:stop-exact-owned', 'relay:stop-owned')
    }
    else {
        @('world:stop-owned', 'relay:stop-owned')
    }
    [pscustomobject]@{
        auth_mode = if ($useWslAuth) { 'wsl' } else { 'windows' }
        startup = $startup
        failure_cleanup = $cleanup
    }
    return
}

function Invoke-Phase1Boundary {
    param(
        [Parameter(Mandatory)][string]$Name,
        [object[]]$Arguments = @(),
        [Parameter(Mandatory)][scriptblock]$ProductionAction
    )

    if ($TestHooks -and $TestHooks.ContainsKey($Name)) {
        return & $TestHooks[$Name] @Arguments
    }
    return & $ProductionAction
}

function Assert-AbsoluteLinuxPath {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$ParameterName
    )

    if (-not $Path.StartsWith('/') -or $Path.IndexOfAny([char[]]@("`0", "`r", "`n")) -ge 0) {
        throw "$ParameterName must be an absolute Linux path without control characters."
    }
}

function ConvertTo-NativeCommandLineArgument {
    param([Parameter(Mandatory)][string]$Value)

    if ($Value.Length -gt 0 -and $Value -notmatch '[\s"]') { return $Value }
    $builder = New-Object System.Text.StringBuilder
    [void]$builder.Append('"')
    $backslashes = 0
    foreach ($character in $Value.ToCharArray()) {
        if ($character -eq '\') {
            $backslashes++
            continue
        }
        if ($character -eq '"') {
            [void]$builder.Append((('\' * (($backslashes * 2) + 1)) -join ''))
            [void]$builder.Append('"')
            $backslashes = 0
            continue
        }
        if ($backslashes) { [void]$builder.Append((('\' * $backslashes) -join '')) }
        [void]$builder.Append($character)
        $backslashes = 0
    }
    if ($backslashes) { [void]$builder.Append((('\' * ($backslashes * 2)) -join '')) }
    [void]$builder.Append('"')
    return $builder.ToString()
}

function Join-NativeCommandLine {
    param([Parameter(Mandatory)][string[]]$Arguments)
    return (($Arguments | ForEach-Object { ConvertTo-NativeCommandLineArgument -Value $_ }) -join ' ')
}

function Test-WslProcessIdentity {
    param(
        [Parameter(Mandatory)][int]$LinuxPid,
        [Parameter(Mandatory)][string]$ExpectedBinary,
        [Parameter(Mandatory)][string]$ExpectedConfig
    )

    if ($TestHooks -and $TestHooks.ContainsKey('AuthIdentityMatches')) {
        return [bool](& $TestHooks['AuthIdentityMatches'] $LinuxPid $ExpectedBinary $ExpectedConfig)
    }
    $identityScript = 'pid="$1"; expected="$(readlink -f -- "$2")" || exit 1; actual="$(readlink -f -- "/proc/$pid/exe")" || exit 1; [ "$actual" = "$expected" ] || exit 1; arg1="$(tr ''\000'' ''\n'' < "/proc/$pid/cmdline" | sed -n ''2p'')"; arg2="$(tr ''\000'' ''\n'' < "/proc/$pid/cmdline" | sed -n ''3p'')"; [ "$arg1" = "-c" ] && [ "$arg2" = "$3" ]'
    & wsl.exe -d $WslDistro -u root -- sh -c $identityScript 'autowow-auth-identity' `
        $LinuxPid $ExpectedBinary $ExpectedConfig *> $null
    return $LASTEXITCODE -eq 0
}

function Test-WslAuthPidExists {
    param([Parameter(Mandatory)][int]$LinuxPid)
    if ($TestHooks -and $TestHooks.ContainsKey('AuthPidExists')) {
        return [bool](& $TestHooks['AuthPidExists'] $LinuxPid)
    }
    & wsl.exe -d $WslDistro -u root -- test -d "/proc/$LinuxPid" *> $null
    return $LASTEXITCODE -eq 0
}

function Test-WslAuthReady {
    param(
        [Parameter(Mandatory)][int]$LinuxPid,
        [Parameter(Mandatory)][string]$ExpectedBinary,
        [Parameter(Mandatory)][string]$ExpectedConfig
    )

    if (-not (Test-WslProcessIdentity -LinuxPid $LinuxPid -ExpectedBinary $ExpectedBinary `
        -ExpectedConfig $ExpectedConfig)) { return $false }
    $listenScript = 'ss -H -ltnp 2>/dev/null | grep -E '':3724[[:space:]]'' | grep -F "pid=$1," >/dev/null'
    & wsl.exe -d $WslDistro -u root -- sh -c $listenScript 'autowow-auth-listen' $LinuxPid *> $null
    return $LASTEXITCODE -eq 0
}

function Stop-OwnedWslAuth {
    param(
        [Parameter(Mandatory)][int]$LinuxPid,
        [Parameter(Mandatory)][string]$ExpectedBinary,
        [Parameter(Mandatory)][string]$ExpectedConfig
    )

    if (-not (Test-WslAuthPidExists -LinuxPid $LinuxPid)) { return }
    if (-not (Test-WslProcessIdentity -LinuxPid $LinuxPid -ExpectedBinary $ExpectedBinary `
        -ExpectedConfig $ExpectedConfig)) {
        throw 'Recorded WSL authserver PID is alive but its executable or config does not match; refusing to signal it.'
    }
    Invoke-Phase1Boundary -Name 'SignalAuth' -Arguments @('TERM', $LinuxPid) -ProductionAction {
        & wsl.exe -d $WslDistro -u root -- kill -TERM $LinuxPid 2>$null
    }
    $stopDeadline = (Get-Date).AddSeconds(15)
    do {
        Start-Sleep -Milliseconds 250
        $exists = Test-WslAuthPidExists -LinuxPid $LinuxPid
        if ($exists -and -not (Test-WslProcessIdentity -LinuxPid $LinuxPid -ExpectedBinary $ExpectedBinary `
            -ExpectedConfig $ExpectedConfig)) {
            throw 'WSL authserver PID identity changed during cleanup; refusing a force kill.'
        }
    } while ($exists -and (Get-Date) -lt $stopDeadline)
    if ($exists) {
        if (-not (Test-WslAuthPidExists -LinuxPid $LinuxPid)) { return }
        if (-not (Test-WslProcessIdentity -LinuxPid $LinuxPid -ExpectedBinary $ExpectedBinary `
            -ExpectedConfig $ExpectedConfig)) {
            throw 'WSL authserver PID identity changed before force cleanup; refusing a force kill.'
        }
        Invoke-Phase1Boundary -Name 'SignalAuth' -Arguments @('KILL', $LinuxPid) -ProductionAction {
            & wsl.exe -d $WslDistro -u root -- kill -KILL $LinuxPid 2>$null
        }
        Start-Sleep -Seconds 1
        if (Test-WslAuthPidExists -LinuxPid $LinuxPid) {
            throw 'Owned WSL authserver remained alive after cleanup; runtime state and relay must be preserved.'
        }
    }
}

function Write-Phase1RuntimeState {
    param([Parameter(Mandatory)]$State)
    [System.IO.File]::WriteAllText(
        $statePath,
        ([pscustomobject]$State | ConvertTo-Json -Depth 4),
        [System.Text.UTF8Encoding]::new($false))
}

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
$authStdout = Join-Path $logRoot 'authserver-wsl-stdout.log'
$authStderr = Join-Path $logRoot 'authserver-wsl-stderr.log'
$authLaunchPidFile = "/tmp/autowow-phase1-auth-$([guid]::NewGuid().ToString('N')).pid"

Invoke-Phase1Boundary -Name 'Preflight' -ProductionAction {
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

    if ($useWslAuth) {
        Assert-AbsoluteLinuxPath -Path $AuthserverBinary -ParameterName 'AuthserverBinary'
        Assert-AbsoluteLinuxPath -Path $AuthserverConfig -ParameterName 'AuthserverConfig'
        if (Get-Process -Name authserver -ErrorAction SilentlyContinue) {
            throw 'A Windows authserver is running; refusing dual auth startup.'
        }
        $linuxAuthserver = (& wsl.exe -d $WslDistro -u root -- pgrep -x authserver 2>$null)
        if ($LASTEXITCODE -eq 0 -and $linuxAuthserver) {
            throw "A WSL authserver is already running (pid $linuxAuthserver)."
        }
        & wsl.exe -d $WslDistro -u root -- test -r $AuthserverBinary
        if ($LASTEXITCODE -ne 0) { throw 'The requested WSL authserver binary is not readable.' }
        & wsl.exe -d $WslDistro -u root -- test -x $AuthserverBinary
        if ($LASTEXITCODE -ne 0) { throw 'The requested WSL authserver binary is not executable.' }
        & wsl.exe -d $WslDistro -u root -- test -r $AuthserverConfig
        if ($LASTEXITCODE -ne 0) { throw 'The requested WSL authserver config is not readable.' }
    }
    else {
        $authPidFile = Join-Path $ServerRoot 'authserver.pid'
        if (-not (Test-Path -LiteralPath $authPidFile)) { throw 'Missing authserver.pid.' }
        $authId = [int]([System.IO.File]::ReadAllText($authPidFile).Trim())
        $auth = Get-Process -Id $authId -ErrorAction SilentlyContinue
        if (-not $auth -or $auth.ProcessName -ne 'authserver') { throw 'Windows authserver is not running.' }
    }

    foreach ($linuxPath in @($binary, $config, '/usr/local/etc/modules/playerbots.conf')) {
        & wsl.exe -d $WslDistro -u root -- test -r $linuxPath
        if ($LASTEXITCODE -ne 0) { throw "Missing WSL runtime prerequisite: $linuxPath" }
    }
}

New-Item -ItemType Directory -Path $logRoot,(Split-Path -Parent $statePath) -Force | Out-Null
Remove-Item -LiteralPath $relayStatusPath -Force -ErrorAction SilentlyContinue
$relay = $null
$world = $null
$wslAuth = $null
$authLinuxPid = 0
$state = $null
try {
    $relay = Invoke-Phase1Boundary -Name 'StartRelay' -ProductionAction {
        # A relay left behind by a crashed/killed world holds the port; no world runs here (checked above), so stop it.
        Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
            Where-Object { $_.CommandLine -and $_.CommandLine -match 'phase1-wsl-mysql-relay\.ps1' } |
            ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
        Start-Sleep -Milliseconds 500
        Start-Process -FilePath 'pwsh.exe' -ArgumentList @('-NoProfile','-File',$relayScript) `
            -RedirectStandardOutput $relayStdout -RedirectStandardError $relayStderr `
            -WindowStyle Hidden -PassThru
    }
    Invoke-Phase1Boundary -Name 'WaitRelayReady' -ProductionAction {
        $relayDeadline = (Get-Date).AddSeconds(20)
        do {
            Start-Sleep -Milliseconds 250
            $relay.Refresh()
            $relayReady = Test-Path -LiteralPath $relayStatusPath
        } while (-not $relayReady -and -not $relay.HasExited -and (Get-Date) -lt $relayDeadline)
        if ($relay.HasExited -or -not $relayReady) { throw 'Private MySQL relay failed to start.' }
    }

    if ($useWslAuth) {
        $wslAuth = Invoke-Phase1Boundary -Name 'StartAuth' -ProductionAction {
            $authLaunchScript = 'umask 077; printf ''%s\n'' "$$" > "$3"; exec "$1" -c "$2"'
            $authArguments = Join-NativeCommandLine -Arguments @(
                '-d', $WslDistro, '-u', 'root', '--', 'sh', '-c', $authLaunchScript,
                'autowow-auth-launch', $AuthserverBinary, $AuthserverConfig, $authLaunchPidFile)
            Start-Process -FilePath 'wsl.exe' -ArgumentList $authArguments `
                -RedirectStandardOutput $authStdout -RedirectStandardError $authStderr `
                -WindowStyle Hidden -PassThru
        }

        $state = [ordered]@{
            schema = 'autowow.phase1.wsl-processes.v2'
            wsl_wrapper_pid = 0
            relay_pid = $relay.Id
            distro = $WslDistro
            binary = $binary
            config = $config
            started_utc = (Get-Date).ToUniversalTime().ToString('o')
            auth_mode = 'wsl'
            auth_wsl_wrapper_pid = $wslAuth.Id
            auth_linux_pid = 0
            auth_binary = $AuthserverBinary
            auth_config = $AuthserverConfig
        }
        Write-Phase1RuntimeState -State $state

        $authLinuxPid = [int](Invoke-Phase1Boundary -Name 'GetAuthLinuxPid' -ProductionAction {
            $pidDeadline = (Get-Date).AddSeconds(10)
            $pidText = $null
            do {
                Start-Sleep -Milliseconds 100
                $wslAuth.Refresh()
                if ($wslAuth.HasExited) {
                    throw "WSL authserver wrapper exited with code $($wslAuth.ExitCode). See authserver WSL logs."
                }
                $pidText = (& wsl.exe -d $WslDistro -u root -- cat $authLaunchPidFile 2>$null)
            } while ([string]::IsNullOrWhiteSpace($pidText) -and (Get-Date) -lt $pidDeadline)
            & wsl.exe -d $WslDistro -u root -- rm -f $authLaunchPidFile 2>$null
            if ([string]::IsNullOrWhiteSpace($pidText) -or $pidText.Trim() -notmatch '^\d+$') {
                throw 'WSL authserver did not publish a valid owned Linux PID.'
            }
            [int]$pidText.Trim()
        })
        $state['auth_linux_pid'] = $authLinuxPid
        Write-Phase1RuntimeState -State $state

        Invoke-Phase1Boundary -Name 'WaitAuthReady' -ProductionAction {
            $authDeadline = (Get-Date).AddSeconds([Math]::Min(60, $StartupTimeoutSeconds))
            do {
                Start-Sleep -Milliseconds 250
                $wslAuth.Refresh()
                if ($wslAuth.HasExited) {
                    throw "WSL authserver exited with code $($wslAuth.ExitCode). See authserver WSL logs."
                }
                if (-not (Test-WslProcessIdentity -LinuxPid $authLinuxPid -ExpectedBinary $AuthserverBinary `
                    -ExpectedConfig $AuthserverConfig)) {
                    throw 'The owned WSL authserver PID no longer matches the requested executable and config.'
                }
                $authReady = Test-WslAuthReady -LinuxPid $authLinuxPid -ExpectedBinary $AuthserverBinary `
                    -ExpectedConfig $AuthserverConfig
            } while (-not $authReady -and (Get-Date) -lt $authDeadline)
            if (-not $authReady) { throw 'WSL authserver did not own a listener on port 3724 before timeout.' }
        }
    }

    $world = Invoke-Phase1Boundary -Name 'StartWorld' -ProductionAction {
        Start-Process -FilePath 'wsl.exe' `
            -ArgumentList @('-d',$WslDistro,'-u','root','--',$binary,'-c',$config) `
            -RedirectStandardOutput $stdout -RedirectStandardError $stderr -WindowStyle Hidden -PassThru
    }

    if ($useWslAuth) {
        $state['wsl_wrapper_pid'] = $world.Id
    }
    else {
        $state = [ordered]@{
            schema = 'autowow.phase1.wsl-processes.v1'
            wsl_wrapper_pid = $world.Id
            relay_pid = $relay.Id
            distro = $WslDistro
            binary = $binary
            config = $config
            started_utc = (Get-Date).ToUniversalTime().ToString('o')
            auth_mode = 'windows'
        }
    }
    Write-Phase1RuntimeState -State $state

    $bridge = Invoke-Phase1Boundary -Name 'WaitWorldReady' -ProductionAction {
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
        $bridgeResponse | ConvertFrom-Json
    }
    if (-not $bridge.ok) { throw 'WSL worldserver bridge returned a non-ok list response.' }

    Write-Output "Phase 1 WSL worldserver ready (wrapper PID $($world.Id), relay PID $($relay.Id), bots $(@($bridge.bots).Count))."
}
catch {
    $startupError = $_
    Invoke-Phase1Boundary -Name 'StopWorld' -ProductionAction {
        & wsl.exe -d $WslDistro -u root -- pkill -TERM -x worldserver 2>$null
        if ($world -and -not $world.HasExited) {
            Stop-Process -InputObject $world -Force -ErrorAction SilentlyContinue
        }
    }
    if ($useWslAuth) {
        try {
            if ($authLinuxPid -le 0) {
                $ownedPidText = (& wsl.exe -d $WslDistro -u root -- cat $authLaunchPidFile 2>$null)
                if (-not [string]::IsNullOrWhiteSpace($ownedPidText) -and $ownedPidText.Trim() -match '^\d+$') {
                    $authLinuxPid = [int]$ownedPidText.Trim()
                    if ($state) {
                        $state['auth_linux_pid'] = $authLinuxPid
                        Write-Phase1RuntimeState -State $state
                    }
                }
            }
            if ($authLinuxPid -gt 0) {
                Invoke-Phase1Boundary -Name 'StopAuth' -ProductionAction {
                    Stop-OwnedWslAuth -LinuxPid $authLinuxPid -ExpectedBinary $AuthserverBinary `
                        -ExpectedConfig $AuthserverConfig
                }
            }
            elseif ($wslAuth -and -not $wslAuth.HasExited) {
                throw 'The WSL authserver wrapper is still alive but no exact Linux PID was recovered.'
            }

            Invoke-Phase1Boundary -Name 'StopAuthWrapper' -ProductionAction {
                if ($wslAuth -and -not $wslAuth.HasExited) {
                    Stop-Process -InputObject $wslAuth -Force -ErrorAction SilentlyContinue
                }
            }
            Invoke-Phase1Boundary -Name 'RemoveAuthPidFile' -ProductionAction {
                & wsl.exe -d $WslDistro -u root -- rm -f $authLaunchPidFile 2>$null
            }
        }
        catch {
            throw "Startup failed: $($startupError.Exception.Message) Auth cleanup could not be proven: $($_.Exception.Message) Runtime state and relay were preserved."
        }
    }
    if ($relay) {
        Invoke-Phase1Boundary -Name 'StopRelay' -ProductionAction {
            if (-not $relay.HasExited) {
                Stop-Process -InputObject $relay -Force -ErrorAction SilentlyContinue
            }
        }
    }
    Remove-Item -LiteralPath $statePath,$relayStatusPath -Force -ErrorAction SilentlyContinue
    throw $startupError.Exception
}
