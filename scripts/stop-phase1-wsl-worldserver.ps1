[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$WslDistro = 'Ubuntu-24.04',
    [Parameter(DontShow)][ValidateSet('', 'Windows', 'Wsl')]
    [string]$EmitInvocationPlanForAuthMode = '',
    [Parameter(DontShow)][hashtable]$TestHooks
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($EmitInvocationPlanForAuthMode) {
    $steps = @('world:stop')
    if ($EmitInvocationPlanForAuthMode -eq 'Wsl') {
        $steps += @('auth:verify-exact-and-stop', 'auth-wrapper:stop-owned')
    }
    $steps += @('world-wrapper:stop-owned', 'relay:stop-owned', 'state:remove')
    [pscustomobject]@{
        auth_mode = $EmitInvocationPlanForAuthMode.ToLowerInvariant()
        shutdown = $steps
    }
    return
}

function Invoke-StopBoundary {
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

function Test-StateProperty {
    param(
        [Parameter(Mandatory)]$InputObject,
        [Parameter(Mandatory)][string]$Name
    )
    return $null -ne $InputObject.PSObject.Properties[$Name]
}

function ConvertTo-NativeCommandLineArgument {
    param([Parameter(Mandatory)][string]$Value)
    if ($Value.Length -gt 0 -and $Value -notmatch '[\s"]') { return $Value }
    $builder = New-Object System.Text.StringBuilder
    [void]$builder.Append('"')
    $backslashes = 0
    foreach ($character in $Value.ToCharArray()) {
        if ($character -eq '\') { $backslashes++; continue }
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

function Invoke-WslCommand {
    param([Parameter(Mandatory)][string[]]$Arguments)
    $startInfo = New-Object System.Diagnostics.ProcessStartInfo
    $startInfo.FileName = 'wsl.exe'
    $startInfo.Arguments = Join-NativeCommandLine -Arguments $Arguments
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $startInfo
    if (-not $process.Start()) { throw 'Could not start wsl.exe.' }
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    $process.WaitForExit()
    [pscustomobject]@{ exit_code = $process.ExitCode; stdout = $stdout.Result; stderr = $stderr.Result }
}

function Test-WslPidExists {
    param([Parameter(Mandatory)][int]$LinuxPid)
    return [bool](Invoke-StopBoundary -Name 'PidExists' -Arguments @($LinuxPid) -ProductionAction {
        & wsl.exe -d $script:RuntimeDistro -u root --exec test -d "/proc/$LinuxPid" *> $null
        $LASTEXITCODE -eq 0
    })
}

function Test-WslProcessIdentity {
    param(
        [Parameter(Mandatory)][int]$LinuxPid,
        [Parameter(Mandatory)][string]$ExpectedBinary,
        [Parameter(Mandatory)][string]$ExpectedConfig
    )

    return [bool](Invoke-StopBoundary -Name 'IdentityMatches' `
        -Arguments @($LinuxPid, $ExpectedBinary, $ExpectedConfig) -ProductionAction {
            $identityScript = 'pid="$1"; expected="$(readlink -f -- "$2")" || exit 1; actual="$(readlink -f -- "/proc/$pid/exe")" || exit 1; [ "$actual" = "$expected" ] || exit 1; arg1="$(tr ''\000'' ''\n'' < "/proc/$pid/cmdline" | sed -n ''2p'')"; arg2="$(tr ''\000'' ''\n'' < "/proc/$pid/cmdline" | sed -n ''3p'')"; [ "$arg1" = "-c" ] && [ "$arg2" = "$3" ]'
            $result = Invoke-WslCommand -Arguments @(
                '-d', $script:RuntimeDistro, '-u', 'root', '--exec', 'sh', '-c', $identityScript,
                'autowow-auth-identity', [string]$LinuxPid, $ExpectedBinary, $ExpectedConfig)
            $result.exit_code -eq 0
        })
}

function Test-WslWrapperIdentity {
    param(
        [Parameter(Mandatory)][int]$WrapperPid,
        [Parameter(Mandatory)][string]$ExpectedBinary,
        [Parameter(Mandatory)][string]$ExpectedConfig
    )

    return [bool](Invoke-StopBoundary -Name 'AuthWrapperIdentityMatches' `
        -Arguments @($WrapperPid, $ExpectedBinary, $ExpectedConfig) -ProductionAction {
            $record = Get-CimInstance Win32_Process -Filter "ProcessId = $WrapperPid" -ErrorAction SilentlyContinue
            if (-not $record -or [string]::IsNullOrWhiteSpace([string]$record.CommandLine)) { return $false }
            $record.CommandLine.IndexOf($ExpectedBinary, [StringComparison]::Ordinal) -ge 0 -and
                $record.CommandLine.IndexOf($ExpectedConfig, [StringComparison]::Ordinal) -ge 0
        })
}

function Stop-ExactWslAuth {
    param(
        [Parameter(Mandatory)][int]$LinuxPid,
        [Parameter(Mandatory)][string]$ExpectedBinary,
        [Parameter(Mandatory)][string]$ExpectedConfig
    )

    if (-not (Test-WslPidExists -LinuxPid $LinuxPid)) { return }
    if (-not (Test-WslProcessIdentity -LinuxPid $LinuxPid -ExpectedBinary $ExpectedBinary `
        -ExpectedConfig $ExpectedConfig)) {
        throw 'Recorded WSL authserver PID does not match its recorded executable and config; refusing to stop it or the relay.'
    }

    Invoke-StopBoundary -Name 'SignalAuth' -Arguments @('TERM', $LinuxPid) -ProductionAction {
        & wsl.exe -d $script:RuntimeDistro -u root --exec kill -TERM $LinuxPid 2>$null
    }
    $deadline = (Get-Date).AddSeconds(30)
    do {
        Start-Sleep -Milliseconds 250
        $exists = Test-WslPidExists -LinuxPid $LinuxPid
        if ($exists -and -not (Test-WslProcessIdentity -LinuxPid $LinuxPid -ExpectedBinary $ExpectedBinary `
            -ExpectedConfig $ExpectedConfig)) {
            throw 'WSL authserver PID identity changed during shutdown; refusing a force kill or relay stop.'
        }
    } while ($exists -and (Get-Date) -lt $deadline)

    if ($exists) {
        Invoke-StopBoundary -Name 'SignalAuth' -Arguments @('KILL', $LinuxPid) -ProductionAction {
            & wsl.exe -d $script:RuntimeDistro -u root --exec kill -KILL $LinuxPid 2>$null
        }
        Start-Sleep -Seconds 1
        if (Test-WslPidExists -LinuxPid $LinuxPid) {
            throw 'Owned WSL authserver did not stop; relay remains running.'
        }
    }
}

$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
$statePath = Join-Path $ServerRoot 'work\phase1-wsl-runtime\runtime-processes.json'
$relayStatusPath = Join-Path $ServerRoot 'work\phase1-wsl-runtime\mysql-relay.json'
$state = $null
if (Test-Path -LiteralPath $statePath) {
    $state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
}
$script:RuntimeDistro = if ($state -and (Test-StateProperty -InputObject $state -Name 'distro') -and
    -not [string]::IsNullOrWhiteSpace([string]$state.distro)) { [string]$state.distro } else { $WslDistro }

Invoke-StopBoundary -Name 'StopWorld' -ProductionAction {
    & wsl.exe -d $script:RuntimeDistro -u root --exec pkill -TERM -x worldserver 2>$null
    $deadline = (Get-Date).AddSeconds(30)
    do {
        Start-Sleep -Milliseconds 500
        & wsl.exe -d $script:RuntimeDistro -u root --exec pgrep -x worldserver *> $null
        $running = $LASTEXITCODE -eq 0
    } while ($running -and (Get-Date) -lt $deadline)
    if ($running) {
        & wsl.exe -d $script:RuntimeDistro -u root --exec pkill -KILL -x worldserver 2>$null
        Start-Sleep -Seconds 1
    }
}

if ($state) {
    $wslAuthMode = (Test-StateProperty -InputObject $state -Name 'auth_mode') -and $state.auth_mode -eq 'wsl'
    if ($wslAuthMode) {
        foreach ($requiredProperty in @('auth_wsl_wrapper_pid', 'auth_linux_pid', 'auth_binary', 'auth_config')) {
            if (-not (Test-StateProperty -InputObject $state -Name $requiredProperty)) {
                throw "WSL authserver state is missing $requiredProperty; refusing auth or relay shutdown."
            }
        }
        Stop-ExactWslAuth -LinuxPid ([int]$state.auth_linux_pid) `
            -ExpectedBinary ([string]$state.auth_binary) -ExpectedConfig ([string]$state.auth_config)
        Invoke-StopBoundary -Name 'StopAuthWrapper' -ProductionAction {
            $authWrapper = Get-Process -Id ([int]$state.auth_wsl_wrapper_pid) -ErrorAction SilentlyContinue
            if ($authWrapper -and ($authWrapper.ProcessName -ne 'wsl' -or
                -not (Test-WslWrapperIdentity -WrapperPid ([int]$state.auth_wsl_wrapper_pid) `
                    -ExpectedBinary ([string]$state.auth_binary) -ExpectedConfig ([string]$state.auth_config)))) {
                throw 'Recorded WSL authserver wrapper identity is uncertain; refusing to stop it or the relay.'
            }
            if ($authWrapper) {
                Stop-Process -InputObject $authWrapper -Force -ErrorAction SilentlyContinue
            }
        }
    }

    Invoke-StopBoundary -Name 'StopWorldWrapper' -ProductionAction {
        $worldWrapper = Get-Process -Id ([int]$state.wsl_wrapper_pid) -ErrorAction SilentlyContinue
        if ($worldWrapper -and $worldWrapper.ProcessName -eq 'wsl') {
            Stop-Process -InputObject $worldWrapper -Force -ErrorAction SilentlyContinue
        }
    }
    Invoke-StopBoundary -Name 'StopRelay' -ProductionAction {
        $relayProcess = Get-Process -Id ([int]$state.relay_pid) -ErrorAction SilentlyContinue
        if ($relayProcess) {
            Stop-Process -InputObject $relayProcess -Force -ErrorAction SilentlyContinue
        }
    }
}

Remove-Item -LiteralPath $statePath,$relayStatusPath -Force -ErrorAction SilentlyContinue
Write-Output 'Phase 1 WSL runtime processes and private MySQL relay are stopped.'
