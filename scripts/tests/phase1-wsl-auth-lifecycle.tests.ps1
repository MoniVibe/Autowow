Set-StrictMode -Version Latest

Describe 'Phase 1 WSL auth lifecycle' {
BeforeAll {
    $script:ScriptsRoot = Split-Path -Parent $PSScriptRoot
    $script:StartScript = Join-Path $script:ScriptsRoot 'start-phase1-wsl-worldserver.ps1'
    $script:StopScript = Join-Path $script:ScriptsRoot 'stop-phase1-wsl-worldserver.ps1'
    $script:StartSource = Get-Content -LiteralPath $script:StartScript -Raw
    $script:StopSource = Get-Content -LiteralPath $script:StopScript -Raw

    $tokens = $null
    $errors = $null
    $startTree = [Management.Automation.Language.Parser]::ParseFile($script:StartScript, [ref]$tokens, [ref]$errors)
    foreach ($functionName in @('ConvertTo-NativeCommandLineArgument', 'Join-NativeCommandLine', 'Invoke-WslCommand')) {
        $functionAst = $startTree.Find({
            param($node)
            $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $functionName
        }, $true)
        . ([scriptblock]::Create($functionAst.Extent.Text))
    }

    function Write-RuntimeState {
        param(
            [Parameter(Mandatory)][string]$Root,
            [Parameter(Mandatory)][hashtable]$State
        )
        $statePath = Join-Path $Root 'work\phase1-wsl-runtime\runtime-processes.json'
        New-Item -ItemType Directory -Path (Split-Path -Parent $statePath) -Force | Out-Null
        [System.IO.File]::WriteAllText(
            $statePath,
            ([pscustomobject]$State | ConvertTo-Json -Depth 4),
            [System.Text.UTF8Encoding]::new($false))
        return $statePath
    }
}

    BeforeEach {
        $global:Phase1TestEvents = [System.Collections.Generic.List[string]]::new()
        $global:Phase1AuthPidChecks = 0
        $script:RuntimeRoot = Join-Path $TestDrive ([guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $script:RuntimeRoot -Force | Out-Null
    }

    It 'runs the real WSL-auth orchestration in relay then auth then world order and records authority' {
        $hooks = @{
            Preflight = { $global:Phase1TestEvents.Add('preflight') }
            StartRelay = { $global:Phase1TestEvents.Add('relay:start'); [pscustomobject]@{ Id = 101; HasExited = $false } }
            WaitRelayReady = { $global:Phase1TestEvents.Add('relay:ready') }
            StartAuth = { $global:Phase1TestEvents.Add('auth:start'); [pscustomobject]@{ Id = 202; HasExited = $false } }
            GetAuthLinuxPid = { $global:Phase1TestEvents.Add('auth:pid'); 303 }
            WaitAuthReady = { $global:Phase1TestEvents.Add('auth:ready') }
            StartWorld = { $global:Phase1TestEvents.Add('world:start'); [pscustomobject]@{ Id = 404; HasExited = $false } }
            WaitWorldReady = {
                $global:Phase1TestEvents.Add('world:ready')
                [pscustomobject]@{ ok = $true; bots = @() }
            }
        }

        & $script:StartScript -ServerRoot $script:RuntimeRoot `
            -AuthserverBinary '/exact/build/authserver' -AuthserverConfig '/exact/runtime/authserver.conf' `
            -TestHooks $hooks | Out-Null

        ($global:Phase1TestEvents -join '|') | Should Be `
            'preflight|relay:start|relay:ready|auth:start|auth:pid|auth:ready|world:start|world:ready'
        $statePath = Join-Path $script:RuntimeRoot 'work\phase1-wsl-runtime\runtime-processes.json'
        $state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
        $state.schema | Should Be 'autowow.phase1.wsl-processes.v2'
        $state.auth_mode | Should Be 'wsl'
        $state.auth_wsl_wrapper_pid | Should Be 202
        $state.auth_linux_pid | Should Be 303
        $state.auth_binary | Should Be '/exact/build/authserver'
        $state.auth_config | Should Be '/exact/runtime/authserver.conf'
    }

    It 'preserves sh positional arguments through the native WSL exec transport' {
        $stdout = Join-Path $TestDrive 'wsl-argv-stdout.txt'
        $stderr = Join-Path $TestDrive 'wsl-argv-stderr.txt'
        $probeScript = 'printf ''arg0=<%s> arg1=<%s> arg2=<%s> arg3=<%s> pid=<%s>\n'' "$0" "$1" "$2" "$3" "$$"'
        $arguments = Join-NativeCommandLine -Arguments @(
            '-d', 'Ubuntu-24.04', '-u', 'root', '--exec', 'sh', '-c', $probeScript,
            'autowow-argv-sentinel', 'value one', 'value two', '/tmp/path with spaces')

        $probe = Start-Process -FilePath 'wsl.exe' -ArgumentList $arguments `
            -RedirectStandardOutput $stdout -RedirectStandardError $stderr `
            -WindowStyle Hidden -Wait -PassThru

        $probe.ExitCode | Should Be 0
        ([System.IO.File]::ReadAllText($stderr)).Trim() | Should Be ''
        ([System.IO.File]::ReadAllText($stdout)).Trim() | Should Match `
            '^arg0=<autowow-argv-sentinel> arg1=<value one> arg2=<value two> arg3=</tmp/path with spaces> pid=<\d+>$'
        $script:StartSource | Should Match '''--exec'', ''sh'', ''-c'', \$authLaunchScript'
        $script:StartSource | Should Not Match '-u root -- sh -c'
        $script:StopSource | Should Not Match '-u root -- sh -c'
    }

    It 'preserves exact identity and listener PID arguments through native WSL exec probes' {
        $identityScript = 'pid="$1"; expected="$(readlink -f -- "$2")" || exit 1; actual="$(readlink -f -- "/proc/$pid/exe")" || exit 1; [ "$actual" = "$expected" ] || exit 1; arg1="$(tr ''\000'' ''\n'' < "/proc/$pid/cmdline" | sed -n ''2p'')"; arg2="$(tr ''\000'' ''\n'' < "/proc/$pid/cmdline" | sed -n ''3p'')"; [ "$arg1" = "-c" ] && [ "$arg2" = "$3" ]'
        $identityOuter = 'sh -c "$1" autowow-auth-identity "$$" /bin/sh "$2"; good=$?; if sh -c "$1" autowow-auth-identity "$$" /bin/sh wrong-config; then wrong_config=1; else wrong_config=0; fi; if sh -c "$1" autowow-auth-identity "$(( $$ + 1 ))" /bin/sh "$2"; then wrong_pid=1; else wrong_pid=0; fi; [ "$good" -eq 0 ] && [ "$wrong_config" -eq 0 ] && [ "$wrong_pid" -eq 0 ]'
        $identityResult = Invoke-WslCommand -Arguments @(
            '-d', 'Ubuntu-24.04', '-u', 'root', '--exec', 'sh', '-c', $identityOuter,
            'autowow-identity-sentinel', $identityScript, $identityOuter)
        $identityResult.exit_code | Should Be 0

        $listenerPython = 'import socket,sys,time;s=socket.socket();s.bind(("127.0.0.1",0));s.listen(1);open(sys.argv[1],"w").write(str(s.getsockname()[1]));time.sleep(10)'
        $listenScript = 'ss -H -ltnp 2>/dev/null | grep -E ":$2[[:space:]]" | grep -F "pid=$1," >/dev/null'
        $listenerOuter = 'tmp="/tmp/autowow-listener-$$"; python3 -c "$1" "$tmp" & listener=$!; i=0; while [ ! -s "$tmp" ] && [ "$i" -lt 100 ]; do i=$((i+1)); sleep .02; done; port=$(cat "$tmp"); good=1; i=0; while [ "$good" -ne 0 ] && [ "$i" -lt 100 ]; do i=$((i+1)); sh -c "$2" autowow-auth-listen "$listener" "$port"; good=$?; [ "$good" -eq 0 ] || sleep .02; done; if sh -c "$2" autowow-auth-listen "$((listener+1))" "$port"; then wrong=1; else wrong=0; fi; rm -f "$tmp"; kill -TERM "$listener" 2>/dev/null; wait "$listener" 2>/dev/null; [ "$good" -eq 0 ] && [ "$wrong" -eq 0 ]'
        $listenerResult = Invoke-WslCommand -Arguments @(
            '-d', 'Ubuntu-24.04', '-u', 'root', '--exec', 'sh', '-c', $listenerOuter,
            'autowow-listener-sentinel', $listenerPython, $listenScript)
        $listenerResult.exit_code | Should Be 0
    }

    It 'keeps Windows-auth mode on the existing relay then world path' {
        $hooks = @{
            Preflight = { $global:Phase1TestEvents.Add('preflight:windows') }
            StartRelay = { $global:Phase1TestEvents.Add('relay:start'); [pscustomobject]@{ Id = 111; HasExited = $false } }
            WaitRelayReady = { $global:Phase1TestEvents.Add('relay:ready') }
            StartAuth = { throw 'WSL auth must not start in Windows mode.' }
            StartWorld = { $global:Phase1TestEvents.Add('world:start'); [pscustomobject]@{ Id = 444; HasExited = $false } }
            WaitWorldReady = {
                $global:Phase1TestEvents.Add('world:ready')
                [pscustomobject]@{ ok = $true; bots = @() }
            }
        }

        & $script:StartScript -ServerRoot $script:RuntimeRoot -TestHooks $hooks | Out-Null

        ($global:Phase1TestEvents -join '|') | Should Be `
            'preflight:windows|relay:start|relay:ready|world:start|world:ready'
        $statePath = Join-Path $script:RuntimeRoot 'work\phase1-wsl-runtime\runtime-processes.json'
        (Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json).auth_mode | Should Be 'windows'
    }

    It 'rejects partial auth flags before any process boundary can mutate state' {
        $hooks = @{
            Preflight = { $global:Phase1TestEvents.Add('unexpected:preflight') }
            StartRelay = { $global:Phase1TestEvents.Add('unexpected:relay') }
        }

        $message = $null
        try {
            & $script:StartScript -ServerRoot $script:RuntimeRoot `
                -AuthserverBinary '/exact/build/authserver' -TestHooks $hooks
        }
        catch { $message = $_.Exception.Message }
        $message | Should Match 'must be supplied together'
        @($global:Phase1TestEvents).Count | Should Be 0
    }

    It 'cleans only owned auth before the relay when auth readiness fails' {
        $hooks = @{
            Preflight = { $global:Phase1TestEvents.Add('preflight') }
            StartRelay = { $global:Phase1TestEvents.Add('relay:start'); [pscustomobject]@{ Id = 121; HasExited = $false } }
            WaitRelayReady = { $global:Phase1TestEvents.Add('relay:ready') }
            StartAuth = { $global:Phase1TestEvents.Add('auth:start'); [pscustomobject]@{ Id = 232; HasExited = $false } }
            GetAuthLinuxPid = { $global:Phase1TestEvents.Add('auth:pid'); 343 }
            WaitAuthReady = { $global:Phase1TestEvents.Add('auth:missing'); throw 'mock missing auth listener' }
            StopWorld = { $global:Phase1TestEvents.Add('cleanup:world') }
            AuthPidExists = {
                param($linuxPid)
                $global:Phase1AuthPidChecks++
                $global:Phase1TestEvents.Add("cleanup:pid-exists:$($global:Phase1AuthPidChecks)")
                $global:Phase1AuthPidChecks -eq 1
            }
            AuthIdentityMatches = { $global:Phase1TestEvents.Add('cleanup:identity-match'); $true }
            SignalAuth = { param($signal, $linuxPid) $global:Phase1TestEvents.Add("cleanup:signal:${signal}:$linuxPid") }
            StopAuthWrapper = { $global:Phase1TestEvents.Add('cleanup:auth-wrapper') }
            RemoveAuthPidFile = { $global:Phase1TestEvents.Add('cleanup:pid-file') }
            StopRelay = { $global:Phase1TestEvents.Add('cleanup:relay') }
        }

        $message = $null
        try {
            & $script:StartScript -ServerRoot $script:RuntimeRoot `
                -AuthserverBinary '/exact/build/authserver' -AuthserverConfig '/exact/runtime/authserver.conf' `
                -TestHooks $hooks
        }
        catch { $message = $_.Exception.Message }
        $message | Should Match 'mock missing auth listener'

        ($global:Phase1TestEvents -join '|') | Should Be `
            'preflight|relay:start|relay:ready|auth:start|auth:pid|auth:missing|cleanup:world|cleanup:pid-exists:1|cleanup:identity-match|cleanup:signal:TERM:343|cleanup:pid-exists:2|cleanup:auth-wrapper|cleanup:pid-file|cleanup:relay'
        Test-Path -LiteralPath (Join-Path $script:RuntimeRoot 'work\phase1-wsl-runtime\runtime-processes.json') |
            Should Be $false
    }

    It 'preserves the partial receipt and relay when failed-start auth identity is uncertain' {
        $hooks = @{
            Preflight = { $global:Phase1TestEvents.Add('preflight') }
            StartRelay = { $global:Phase1TestEvents.Add('relay:start'); [pscustomobject]@{ Id = 121; HasExited = $false } }
            WaitRelayReady = { $global:Phase1TestEvents.Add('relay:ready') }
            StartAuth = { $global:Phase1TestEvents.Add('auth:start'); [pscustomobject]@{ Id = 232; HasExited = $false } }
            GetAuthLinuxPid = { $global:Phase1TestEvents.Add('auth:pid'); 343 }
            WaitAuthReady = { $global:Phase1TestEvents.Add('auth:missing'); throw 'mock missing auth listener' }
            StopWorld = { $global:Phase1TestEvents.Add('cleanup:world') }
            AuthPidExists = { $global:Phase1TestEvents.Add('cleanup:pid-alive'); $true }
            AuthIdentityMatches = { $global:Phase1TestEvents.Add('cleanup:identity-uncertain'); $false }
            SignalAuth = { $global:Phase1TestEvents.Add('unexpected:signal') }
            StopAuthWrapper = { $global:Phase1TestEvents.Add('unexpected:auth-wrapper') }
            RemoveAuthPidFile = { $global:Phase1TestEvents.Add('unexpected:pid-file') }
            StopRelay = { $global:Phase1TestEvents.Add('unexpected:relay') }
        }

        $message = $null
        try {
            & $script:StartScript -ServerRoot $script:RuntimeRoot `
                -AuthserverBinary '/exact/build/authserver' -AuthserverConfig '/exact/runtime/authserver.conf' `
                -TestHooks $hooks
        }
        catch { $message = $_.Exception.Message }

        $message | Should Match 'cleanup could not be proven'
        $message | Should Match 'state and relay were preserved'
        ($global:Phase1TestEvents -join '|') | Should Be `
            'preflight|relay:start|relay:ready|auth:start|auth:pid|auth:missing|cleanup:world|cleanup:pid-alive|cleanup:identity-uncertain'
        $statePath = Join-Path $script:RuntimeRoot 'work\phase1-wsl-runtime\runtime-processes.json'
        $state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json
        $state.auth_linux_pid | Should Be 343
        $state.auth_wsl_wrapper_pid | Should Be 232
        $state.relay_pid | Should Be 121
        $state.wsl_wrapper_pid | Should Be 0
    }

    It 'never force kills when the owned auth PID identity changes after TERM' {
        $global:Phase1AuthIdentityChecks = 0
        $hooks = @{
            Preflight = { }
            StartRelay = { [pscustomobject]@{ Id = 121; HasExited = $false } }
            WaitRelayReady = { }
            StartAuth = { [pscustomobject]@{ Id = 232; HasExited = $false } }
            GetAuthLinuxPid = { 343 }
            WaitAuthReady = { throw 'mock readiness failure' }
            StopWorld = { }
            AuthPidExists = { $true }
            AuthIdentityMatches = {
                $global:Phase1AuthIdentityChecks++
                $global:Phase1AuthIdentityChecks -eq 1
            }
            SignalAuth = { param($signal, $linuxPid) $global:Phase1TestEvents.Add("signal:${signal}:$linuxPid") }
            StopAuthWrapper = { $global:Phase1TestEvents.Add('unexpected:auth-wrapper') }
            StopRelay = { $global:Phase1TestEvents.Add('unexpected:relay') }
        }

        $message = $null
        try {
            & $script:StartScript -ServerRoot $script:RuntimeRoot `
                -AuthserverBinary '/exact/build/authserver' -AuthserverConfig '/exact/runtime/authserver.conf' `
                -TestHooks $hooks
        }
        catch { $message = $_.Exception.Message }

        $message | Should Match 'identity changed during cleanup'
        @($global:Phase1TestEvents) | Should Be @('signal:TERM:343')
        Test-Path -LiteralPath (Join-Path $script:RuntimeRoot 'work\phase1-wsl-runtime\runtime-processes.json') |
            Should Be $true
    }

    It 'refuses a stale or unrelated auth PID and leaves relay state intact' {
        $statePath = Write-RuntimeState -Root $script:RuntimeRoot -State ([ordered]@{
            schema = 'autowow.phase1.wsl-processes.v2'
            wsl_wrapper_pid = 404
            relay_pid = 101
            distro = 'Ubuntu-24.04'
            auth_mode = 'wsl'
            auth_wsl_wrapper_pid = 202
            auth_linux_pid = 303
            auth_binary = '/exact/build/authserver'
            auth_config = '/exact/runtime/authserver.conf'
        })
        $hooks = @{
            StopWorld = { $global:Phase1TestEvents.Add('world:stop') }
            PidExists = { param($linuxPid) $global:Phase1TestEvents.Add("pid:exists:$linuxPid"); $true }
            IdentityMatches = { param($linuxPid, $binary, $config) $global:Phase1TestEvents.Add("pid:mismatch:$linuxPid"); $false }
            SignalAuth = { $global:Phase1TestEvents.Add('unexpected:signal') }
            StopWorldWrapper = { $global:Phase1TestEvents.Add('unexpected:world-wrapper') }
            StopRelay = { $global:Phase1TestEvents.Add('unexpected:relay') }
        }

        $message = $null
        try { & $script:StopScript -ServerRoot $script:RuntimeRoot -TestHooks $hooks }
        catch { $message = $_.Exception.Message }
        $message | Should Match 'does not match its recorded executable and config'
        ($global:Phase1TestEvents -join '|') | Should Be 'world:stop|pid:exists:303|pid:mismatch:303'
        Test-Path -LiteralPath $statePath | Should Be $true
    }

    It 'stops an exact owned auth PID before stopping the relay' {
        $statePath = Write-RuntimeState -Root $script:RuntimeRoot -State ([ordered]@{
            schema = 'autowow.phase1.wsl-processes.v2'
            wsl_wrapper_pid = 404
            relay_pid = 101
            distro = 'Ubuntu-24.04'
            auth_mode = 'wsl'
            auth_wsl_wrapper_pid = 202
            auth_linux_pid = 303
            auth_binary = '/exact/build/authserver'
            auth_config = '/exact/runtime/authserver.conf'
        })
        $global:Phase1PidChecks = 0
        $hooks = @{
            StopWorld = { $global:Phase1TestEvents.Add('world:stop') }
            PidExists = {
                param($linuxPid)
                $global:Phase1PidChecks++
                $global:Phase1TestEvents.Add("pid:exists:${linuxPid}:$($global:Phase1PidChecks)")
                $global:Phase1PidChecks -eq 1
            }
            IdentityMatches = { param($linuxPid, $binary, $config) $global:Phase1TestEvents.Add("pid:match:$linuxPid"); $true }
            SignalAuth = { param($signal, $linuxPid) $global:Phase1TestEvents.Add("auth:${signal}:$linuxPid") }
            StopAuthWrapper = { $global:Phase1TestEvents.Add('auth-wrapper:stop') }
            StopWorldWrapper = { $global:Phase1TestEvents.Add('world-wrapper:stop') }
            StopRelay = { $global:Phase1TestEvents.Add('relay:stop') }
        }

        & $script:StopScript -ServerRoot $script:RuntimeRoot -TestHooks $hooks | Out-Null

        $authSignalIndex = $global:Phase1TestEvents.IndexOf('auth:TERM:303')
        $authWrapperIndex = $global:Phase1TestEvents.IndexOf('auth-wrapper:stop')
        $relayIndex = $global:Phase1TestEvents.IndexOf('relay:stop')
        $authSignalIndex | Should BeGreaterThan $global:Phase1TestEvents.IndexOf('world:stop')
        $authWrapperIndex | Should BeGreaterThan $authSignalIndex
        $relayIndex | Should BeGreaterThan $authWrapperIndex
        Test-Path -LiteralPath $statePath | Should Be $false
    }
}
