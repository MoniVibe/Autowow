<##
    Offline Pester coverage for the WSL build gate.

    Every build-shaped call uses an injected WSL invoker.  No test starts
    wsl.exe, cmake, ninja, worldserver, or any other external process.
##>

Set-StrictMode -Version Latest

BeforeAll {
    $script:LaneScripts = Split-Path -Parent $PSScriptRoot
    $script:LibraryPath = Join-Path $script:LaneScripts 'wsl-build-gate-lib.ps1'
    $script:WrapperPath = Join-Path $script:LaneScripts 'wsl-build-gate.ps1'
    . $script:LibraryPath

    function New-TestGateLock {
        param([int]$FailuresBeforeAcquire = 0)
        $lock = [pscustomobject]@{
            WaitCount = 0
            ReleaseCount = 0
            DisposeCount = 0
            FailuresBeforeAcquire = $FailuresBeforeAcquire
        }
        $lock | Add-Member -MemberType ScriptMethod -Name WaitOne -Value {
            param($Timeout)
            $this.WaitCount++
            return ($this.WaitCount -gt $this.FailuresBeforeAcquire)
        }
        $lock | Add-Member -MemberType ScriptMethod -Name ReleaseMutex -Value {
            $this.ReleaseCount++
        }
        $lock | Add-Member -MemberType ScriptMethod -Name Dispose -Value {
            $this.DisposeCount++
        }
        return $lock
    }

    function New-TestWslInvoker {
        param(
            [bool]$LiveWorldserver = $false,
            [int]$AvailableMemoryMiB = 8192,
            [int]$BuildExitCode = 0
        )
        $script:TestLiveWorldserver = $LiveWorldserver
        $script:TestAvailableMemoryMiB = $AvailableMemoryMiB
        $script:TestBuildExitCode = $BuildExitCode
        $script:TestWslCalls = [System.Collections.Generic.List[object]]::new()
        return {
            param([string[]]$Arguments, [string]$OutputPath, [bool]$CaptureOutput)
            $script:TestWslCalls.Add([pscustomobject]@{
                arguments = @($Arguments)
                output_path = $OutputPath
                capture_output = $CaptureOutput
            })
            if ($Arguments -contains 'pgrep') {
                if ($script:TestLiveWorldserver) {
                    return [pscustomobject]@{ exit_code = 0; output = @('4242'); invocation_error = $null }
                }
                return [pscustomobject]@{ exit_code = 1; output = @(); invocation_error = $null }
            }
            if ($Arguments -contains 'cat') {
                $kb = $script:TestAvailableMemoryMiB * 1024
                return [pscustomobject]@{
                    exit_code = 0
                    output = @('MemAvailable:        {0} kB' -f $kb)
                    invocation_error = $null
                }
            }
            if ($Arguments -contains 'cmake' -or $Arguments -contains 'ninja') {
                return [pscustomobject]@{
                    exit_code = $script:TestBuildExitCode
                    output = @('injected build invoker; no build executed')
                    invocation_error = $null
                }
            }
            throw "Unexpected injected WSL command: $($Arguments -join ' ')"
        }
    }

    function New-TestClock {
        $script:TestClockValue = [DateTime]'2026-07-18T12:00:00Z'
        return { $script:TestClockValue }
    }
}

Describe 'WSL build gate syntax and command construction' {
    BeforeEach {
        $script:TestLock = New-TestGateLock
        $script:TestLockFactory = { param([string]$Name) $script:TestLock }
        $script:TestSleepCalls = [System.Collections.Generic.List[int]]::new()
        $script:TestSleeper = { param([int]$Milliseconds) $script:TestSleepCalls.Add($Milliseconds) }
        $script:TestClock = New-TestClock
    }

    It 'parse-checks the library, wrapper, and this test without invoking them' {
        foreach ($path in @($script:LibraryPath, $script:WrapperPath, $PSCommandPath)) {
            $tokens = $null
            $errors = $null
            [System.Management.Automation.Language.Parser]::ParseFile($path, [ref]$tokens, [ref]$errors) | Out-Null
            @($errors).Count | Should -Be 0 -Because $path
        }
    }

    It 'constructs direct-argv CMake and Ninja commands with the requested jobs' {
        $cmake = @(New-WslBuildGateCommandVector -Tool CMake -Distro Ubuntu-24.04 `
            -BuildDirectory '/root/build with spaces' -Target worldserver -Jobs 8)
        $cmake | Should -Be @('-d','Ubuntu-24.04','-u','root','--','cmake','--build','/root/build with spaces','--target','worldserver','--','-j8')

        $ninja = @(New-WslBuildGateCommandVector -Tool Ninja -Distro Ubuntu-24.04 `
            -BuildDirectory '/root/build' -Target unit_tests,worldserver -Jobs 12)
        $ninja | Should -Be @('-d','Ubuntu-24.04','-u','root','--','ninja','-C','/root/build','-j','12','unit_tests','worldserver')
    }

    It 'does not contain process termination or shell-evaluation primitives' {
        $source = Get-Content -LiteralPath $script:LibraryPath -Raw
        $source | Should -Not -Match '(?i)Invoke-Expression|\biex\b|Stop-Process|\bpkill\b|\bkill\s+-'
        $source | Should -Match '&\s+wsl\.exe\s+@Arguments'
    }
}

Describe 'WSL capacity and job policy' {
    BeforeEach {
        $script:TestLock = New-TestGateLock
        $script:TestLockFactory = { param([string]$Name) $script:TestLock }
        $script:TestSleepCalls = [System.Collections.Generic.List[int]]::new()
        $script:TestSleeper = { param([int]$Milliseconds) $script:TestSleepCalls.Add($Milliseconds) }
        $script:TestClock = New-TestClock
    }

    It 'defaults to eight jobs while a live worldserver is detected' {
        $wsl = New-TestWslInvoker -LiveWorldserver $true -AvailableMemoryMiB 8192
        $receipt = Join-Path $TestDrive 'live.jsonl'
        $output = Join-Path $TestDrive 'live.output.log'
        $result = Invoke-WslBuildGate -BuildDirectory '/root/build' -Target worldserver -DryRun `
            -WslInvoker $wsl -LockFactory $script:TestLockFactory -Clock $script:TestClock `
            -SleepInvoker $script:TestSleeper -ReceiptPath $receipt -OutputLogPath $output

        $result.live_worldserver | Should -BeTrue
        $result.effective_jobs | Should -Be 8
        $result.build_invoked | Should -BeFalse
        $records = @(Get-Content -LiteralPath $receipt | ForEach-Object { $_ | ConvertFrom-Json })
        $plan = @($records | Where-Object { $_.event -eq 'build_planned' })[0]
        @($plan.details.command) | Should -Contain '-j8'
    }

    It 'defaults to twelve jobs when no worldserver is live' {
        $wsl = New-TestWslInvoker -LiveWorldserver $false -AvailableMemoryMiB 8192
        $receipt = Join-Path $TestDrive 'offline.jsonl'
        $output = Join-Path $TestDrive 'offline.output.log'
        $result = Invoke-WslBuildGate -Tool Ninja -BuildDirectory '/root/build' -Target worldserver `
            -WslInvoker $wsl -LockFactory $script:TestLockFactory -Clock $script:TestClock `
            -SleepInvoker $script:TestSleeper -ReceiptPath $receipt -OutputLogPath $output

        $result.live_worldserver | Should -BeFalse
        $result.effective_jobs | Should -Be 12
        $result.build_invoked | Should -BeTrue
        @($script:TestWslCalls | Where-Object { $_.arguments -contains 'ninja' }).Count | Should -Be 1
    }

    It 'rejects more than eight jobs while the worldserver is live' {
        $wsl = New-TestWslInvoker -LiveWorldserver $true -AvailableMemoryMiB 8192
        { Invoke-WslBuildGate -Jobs 9 -BuildDirectory '/root/build' -Target worldserver `
            -WslInvoker $wsl -LockFactory $script:TestLockFactory -Clock $script:TestClock `
            -SleepInvoker $script:TestSleeper -ReceiptPath (Join-Path $TestDrive 'live-too-many.jsonl') `
            -OutputLogPath (Join-Path $TestDrive 'live-too-many.log') } | Should -Throw '*8-job cap*'
        @($script:TestWslCalls | Where-Object { $_.arguments -contains 'cmake' -or $_.arguments -contains 'ninja' }).Count | Should -Be 0
        $script:TestLock.ReleaseCount | Should -Be 1
    }

    It 'enforces the hard twelve-job maximum at the parameter boundary' {
        { Invoke-WslBuildGate -Jobs 13 -BuildDirectory '/root/build' -Target worldserver } | Should -Throw
    }

    It 'fails closed when available WSL memory is below the threshold' {
        $wsl = New-TestWslInvoker -LiveWorldserver $false -AvailableMemoryMiB 1024
        { Invoke-WslBuildGate -BuildDirectory '/root/build' -Target worldserver `
            -WslInvoker $wsl -LockFactory $script:TestLockFactory -Clock $script:TestClock `
            -SleepInvoker $script:TestSleeper -ReceiptPath (Join-Path $TestDrive 'low-memory.jsonl') `
            -OutputLogPath (Join-Path $TestDrive 'low-memory.log') } | Should -Throw '*below the configured threshold*'
        @($script:TestWslCalls | Where-Object { $_.arguments -contains 'cmake' -or $_.arguments -contains 'ninja' }).Count | Should -Be 0
        $script:TestLock.ReleaseCount | Should -Be 1
    }
}

Describe 'WSL build gate lock behavior and cleanup' {
    BeforeEach {
        $script:TestLock = New-TestGateLock
        $script:TestLockFactory = { param([string]$Name) $script:TestLock }
        $script:TestSleepCalls = [System.Collections.Generic.List[int]]::new()
        $script:TestSleeper = { param([int]$Milliseconds) $script:TestSleepCalls.Add($Milliseconds) }
        $script:TestClock = New-TestClock
    }

    It 'refuses a held lock and emits a clear refusal receipt without touching WSL' {
        $script:TestLock = New-TestGateLock -FailuresBeforeAcquire 999
        $wsl = New-TestWslInvoker -AvailableMemoryMiB 8192
        $receipt = Join-Path $TestDrive 'refused.jsonl'
        { Invoke-WslBuildGate -BuildDirectory '/root/build' -Target worldserver `
            -WslInvoker $wsl -LockFactory $script:TestLockFactory -Clock $script:TestClock `
            -SleepInvoker $script:TestSleeper -ReceiptPath $receipt -OutputLogPath (Join-Path $TestDrive 'refused.log') } |
            Should -Throw '*already held*'
        @($script:TestWslCalls).Count | Should -Be 0
        @((Get-Content -LiteralPath $receipt) | ForEach-Object { $_ | ConvertFrom-Json } |
            Where-Object { $_.event -eq 'lock_refused' }).Count | Should -Be 1
        $script:TestLock.ReleaseCount | Should -Be 0
    }

    It 'waits when requested, records waiting, and releases after acquisition' {
        $script:TestLock = New-TestGateLock -FailuresBeforeAcquire 1
        $wsl = New-TestWslInvoker -AvailableMemoryMiB 8192
        $receipt = Join-Path $TestDrive 'waited.jsonl'
        $result = Invoke-WslBuildGate -BuildDirectory '/root/build' -Target worldserver -DryRun -Wait `
            -LockPollMilliseconds 100 -WaitTimeoutSeconds 2 -WslInvoker $wsl `
            -LockFactory $script:TestLockFactory -Clock $script:TestClock -SleepInvoker $script:TestSleeper `
            -ReceiptPath $receipt -OutputLogPath (Join-Path $TestDrive 'waited.log')
        $result.exit_code | Should -Be 0
        $script:TestLock.WaitCount | Should -Be 2
        $script:TestSleepCalls.Count | Should -Be 1
        $script:TestLock.ReleaseCount | Should -Be 1
        $events = @(Get-Content -LiteralPath $receipt | ForEach-Object { $_ | ConvertFrom-Json })
        @($events | Where-Object { $_.event -eq 'lock_waiting' }).Count | Should -Be 1
        @($events | Where-Object { $_.event -eq 'lock_acquired' }).Count | Should -Be 1
        @($events | Where-Object { $_.event -eq 'lock_released' }).Count | Should -Be 1
    }

    It 'times out a queued lock with a typed receipt and no WSL call' {
        $script:TestLock = New-TestGateLock -FailuresBeforeAcquire 999
        $script:TimeoutClockCalls = 0
        $timeoutClock = {
            $script:TimeoutClockCalls++
            return ([DateTime]'2026-07-18T12:00:00Z').AddSeconds($script:TimeoutClockCalls)
        }
        $wsl = New-TestWslInvoker -AvailableMemoryMiB 8192
        $receipt = Join-Path $TestDrive 'wait-timeout.jsonl'
        { Invoke-WslBuildGate -BuildDirectory '/root/build' -Target worldserver -Wait `
            -LockPollMilliseconds 100 -WaitTimeoutSeconds 1 -WslInvoker $wsl `
            -LockFactory $script:TestLockFactory -Clock $timeoutClock -SleepInvoker $script:TestSleeper `
            -ReceiptPath $receipt -OutputLogPath (Join-Path $TestDrive 'wait-timeout.log') } |
            Should -Throw '*Timed out*'
        @($script:TestWslCalls).Count | Should -Be 0
        @((Get-Content -LiteralPath $receipt) | ForEach-Object { $_ | ConvertFrom-Json } |
            Where-Object { $_.event -eq 'lock_wait_timeout' }).Count | Should -Be 1
        $script:TestLock.ReleaseCount | Should -Be 0
    }

    It 'releases the lock when the injected build fails' {
        $wsl = New-TestWslInvoker -AvailableMemoryMiB 8192 -BuildExitCode 17
        $receipt = Join-Path $TestDrive 'build-failure.jsonl'
        { Invoke-WslBuildGate -BuildDirectory '/root/build' -Target worldserver `
            -WslInvoker $wsl -LockFactory $script:TestLockFactory -Clock $script:TestClock `
            -SleepInvoker $script:TestSleeper -ReceiptPath $receipt -OutputLogPath (Join-Path $TestDrive 'build-failure.log') } |
            Should -Throw '*failed with exit code 17*'
        $script:TestLock.ReleaseCount | Should -Be 1
        $events = @(Get-Content -LiteralPath $receipt | ForEach-Object { $_ | ConvertFrom-Json })
        @($events | Where-Object { $_.event -eq 'lock_released' }).Count | Should -Be 1
        @($events | Where-Object { $_.event -eq 'gate_failed' -and $_.details.exit_code -eq 17 }).Count | Should -Be 1
    }

    It 'dry-runs the exact plan and never invokes the injected build command' {
        $wsl = New-TestWslInvoker -AvailableMemoryMiB 8192
        $receipt = Join-Path $TestDrive 'dry-run.jsonl'
        $result = Invoke-WslBuildGate -BuildDirectory '/root/build' -Target unit_tests -DryRun `
            -WslInvoker $wsl -LockFactory $script:TestLockFactory -Clock $script:TestClock `
            -SleepInvoker $script:TestSleeper -ReceiptPath $receipt -OutputLogPath (Join-Path $TestDrive 'dry-run.log')
        $result.dry_run | Should -BeTrue
        @($script:TestWslCalls | Where-Object { $_.arguments -contains 'cmake' -or $_.arguments -contains 'ninja' }).Count | Should -Be 0
        $records = @(Get-Content -LiteralPath $receipt | ForEach-Object { $_ | ConvertFrom-Json })
        @($records | Where-Object { $_.event -eq 'dry_run_skipped_build' -and $_.details.build_invoked -eq $false }).Count | Should -Be 1
    }
}

Describe 'WSL build gate input and receipt contracts' {
    BeforeEach {
        $script:TestLock = New-TestGateLock
        $script:TestLockFactory = { param([string]$Name) $script:TestLock }
        $script:TestSleepCalls = [System.Collections.Generic.List[int]]::new()
        $script:TestSleeper = { param([int]$Milliseconds) $script:TestSleepCalls.Add($Milliseconds) }
        $script:TestClock = New-TestClock
    }

    It 'rejects shell-shaped target text before any external command' {
        $wsl = New-TestWslInvoker -AvailableMemoryMiB 8192
        { Invoke-WslBuildGate -BuildDirectory '/root/build' -Target 'worldserver;touch /tmp/pwned' `
            -WslInvoker $wsl -LockFactory $script:TestLockFactory -Clock $script:TestClock `
            -SleepInvoker $script:TestSleeper -ReceiptPath (Join-Path $TestDrive 'unsafe.jsonl') `
            -OutputLogPath (Join-Path $TestDrive 'unsafe.log') } | Should -Throw '*Unsafe build target*'
        @($script:TestWslCalls).Count | Should -Be 0
    }

    It 'records command start/end, exit code, and memory observations' {
        $wsl = New-TestWslInvoker -AvailableMemoryMiB 8192
        $receipt = Join-Path $TestDrive 'receipt-contract.jsonl'
        $output = Join-Path $TestDrive 'receipt-contract.log'
        $null = Invoke-WslBuildGate -BuildDirectory '/root/build' -Target worldserver `
            -WslInvoker $wsl -LockFactory $script:TestLockFactory -Clock $script:TestClock `
            -SleepInvoker $script:TestSleeper -ReceiptPath $receipt -OutputLogPath $output
        $events = @(Get-Content -LiteralPath $receipt | ForEach-Object { $_ | ConvertFrom-Json })
        $buildStart = @($events | Where-Object { $_.event -eq 'command_started' -and $_.details.label -eq 'build' })[0]
        $buildEnd = @($events | Where-Object { $_.event -eq 'command_finished' -and $_.details.label -eq 'build' })[0]
        $plan = @($events | Where-Object { $_.event -eq 'build_planned' })[0]
        $buildStart.details.started_utc | Should -Not -BeNullOrEmpty
        $buildStart.details.available_memory_mib | Should -Be 8192
        $buildEnd.details.ended_utc | Should -Not -BeNullOrEmpty
        $buildEnd.details.exit_code | Should -Be 0
        @($plan.details.command) | Should -Contain '-j12'
        @($events | Where-Object { $_.event -eq 'post_build_memory' -and $_.details.available_memory_mib -eq 8192 }).Count | Should -Be 1
        if (Test-Path -LiteralPath $output -PathType Leaf) {
            (Get-Content -LiteralPath $output -Raw) | Should -BeNullOrEmpty
        }
    }
}
