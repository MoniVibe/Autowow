<##
.SYNOPSIS
    Safety-gated WSL build invocation for AutoWoW.

.DESCRIPTION
    This library deliberately owns only the build-gate boundary.  It does not
    edit source, CMake caches, server configuration, databases, or processes.
    The default WSL command path passes an argument vector directly to
    wsl.exe; it never constructs or evaluates a shell command string.

    The public entry point is Invoke-WslBuildGate.  All external seams are
    injectable so offline Pester tests can exercise lock, capacity, command,
    failure, and cleanup behavior without starting a real build.
##>

Set-StrictMode -Version Latest

$script:WslBuildGateSchema = 'autowow.wsl-build-gate.v1'
$script:WslBuildGateMutexName = 'Global\AutoWoW.WslBuildGate.v1'
$script:WslBuildGateLiveJobs = 8
$script:WslBuildGateOfflineJobs = 12
$script:WslBuildGateHardMaxJobs = 12
$script:WslBuildGateDefaultMemoryMiB = 4096
$script:WslBuildGateDefaultWaitTimeoutSeconds = 300
$script:WslBuildGateDefaultLockPollMilliseconds = 1000
$script:WslBuildGateLaneRoot = Split-Path -Parent $PSScriptRoot | Split-Path -Parent

function Get-WslBuildGateUtcNow {
    [CmdletBinding()]
    param()
    return [DateTime]::UtcNow
}

function Write-WslBuildGateReceipt {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$ReceiptPath,
        [Parameter(Mandatory)][string]$RunId,
        [Parameter(Mandatory)][string]$Event,
        [Parameter(Mandatory)][ValidateSet('INFO', 'PASS', 'WARN', 'FAIL_CLOSED')][string]$Status,
        [Parameter(Mandatory)][DateTime]$TimestampUtc,
        [hashtable]$Details = @{}
    )

    $parent = Split-Path -Parent $ReceiptPath
    if (-not [string]::IsNullOrWhiteSpace($parent)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }

    $detailObject = [ordered]@{}
    foreach ($key in $Details.Keys) {
        $detailObject[[string]$key] = $Details[$key]
    }

    $record = [ordered]@{
        schema = $script:WslBuildGateSchema
        event = $Event
        status = $Status
        run_id = $RunId
        timestamp_utc = $TimestampUtc.ToUniversalTime().ToString('o')
        details = $detailObject
    }
    $json = $record | ConvertTo-Json -Compress -Depth 30
    [System.IO.File]::AppendAllText(
        $ReceiptPath,
        $json + [Environment]::NewLine,
        [System.Text.UTF8Encoding]::new($false))
    return [pscustomobject]$record
}

function Assert-WslBuildGateDistro {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Distro)

    if ($Distro -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$') {
        throw "Unsafe WSL distro name '$Distro'. Expected a simple distro identifier."
    }
}

function Assert-WslBuildGatePathToken {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Name
    )

    if ([string]::IsNullOrWhiteSpace($Path) -or $Path -notmatch '^/') {
        throw "$Name must be a non-empty absolute Linux path beginning with '/'."
    }
    if ($Path -match '[\x00-\x1F\x7F]') {
        throw "$Name contains a control character and cannot be passed to WSL."
    }
    if ($Path -eq '--') {
        throw "$Name cannot be '--'."
    }
}

function Assert-WslBuildGateTarget {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Target)

    # Targets are passed as argv entries, never parsed by a shell.  Keep the
    # accepted grammar deliberately narrow so a typo cannot become an option.
    if ([string]::IsNullOrWhiteSpace($Target) -or
        $Target -notmatch '^[A-Za-z0-9_./:+=@%-]+$' -or
        $Target.StartsWith('-')) {
        throw "Unsafe build target '$Target'. Use a simple CMake/Ninja target name."
    }
}

function New-WslBuildGateCommandVector {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][ValidateSet('CMake', 'Ninja')][string]$Tool,
        [Parameter(Mandatory)][string]$Distro,
        [Parameter(Mandatory)][string]$BuildDirectory,
        [Parameter(Mandatory)][AllowEmptyCollection()][string[]]$Target,
        [Parameter(Mandatory)][ValidateRange(1, 12)][int]$Jobs
    )

    Assert-WslBuildGateDistro -Distro $Distro
    Assert-WslBuildGatePathToken -Path $BuildDirectory -Name 'BuildDirectory'
    if (@($Target).Count -lt 1) { throw 'At least one build target is required.' }
    foreach ($targetName in @($Target)) {
        Assert-WslBuildGateTarget -Target $targetName
    }

    $prefix = [string[]]@('-d', $Distro, '-u', 'root', '--')
    if ($Tool -eq 'CMake') {
        $arguments = $prefix + [string[]]@('cmake', '--build', $BuildDirectory, '--target') +
            [string[]]@($Target) + [string[]]@('--', ('-j{0}' -f $Jobs))
    }
    else {
        $arguments = $prefix + [string[]]@('ninja', '-C', $BuildDirectory, '-j', [string]$Jobs) +
            [string[]]@($Target)
    }
    return [string[]]$arguments
}

function ConvertTo-WslBuildGateCommandText {
    [CmdletBinding()]
    param([Parameter(Mandatory)][AllowEmptyCollection()][string[]]$Arguments)

    $parts = foreach ($argument in @($Arguments)) {
        if ($argument -match '[\s"'']') {
            '"' + ($argument -replace '(["\\])', '\$1') + '"'
        }
        else { $argument }
    }
    return ($parts -join ' ')
}

function Invoke-WslBuildGateDefaultCommand {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][AllowEmptyCollection()][string[]]$Arguments,
        [Parameter(Mandatory)][string]$OutputPath,
        [Parameter(Mandatory)][bool]$CaptureOutput
    )

    $captured = [System.Collections.Generic.List[string]]::new()
    $writer = $null
    $exitCode = -1
    $invocationError = $null
    try {
        $parent = Split-Path -Parent $OutputPath
        if (-not [string]::IsNullOrWhiteSpace($parent)) {
            New-Item -ItemType Directory -Path $parent -Force | Out-Null
        }
        $writer = [System.IO.StreamWriter]::new(
            $OutputPath,
            $true,
            [System.Text.UTF8Encoding]::new($false))
        $writer.AutoFlush = $true

        $oldPreference = $ErrorActionPreference
        try {
            # The argument vector is passed directly to wsl.exe.  There is no
            # shell interpolation or command-string evaluation.
            $ErrorActionPreference = 'Continue'
            & wsl.exe @Arguments 2>&1 | ForEach-Object {
                $line = [string]$_
                $writer.WriteLine($line)
                if ($CaptureOutput) { $captured.Add($line) }
            }
            $exitCode = [int]$LASTEXITCODE
        }
        finally {
            $ErrorActionPreference = $oldPreference
        }
    }
    catch {
        $invocationError = $_.Exception.Message
        $exitCode = -1
    }
    finally {
        if ($null -ne $writer) { $writer.Dispose() }
    }

    return [pscustomobject][ordered]@{
        exit_code = $exitCode
        output = @($captured)
        invocation_error = $invocationError
    }
}

function Invoke-WslBuildGateLoggedCommand {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Label,
        [Parameter(Mandatory)][AllowEmptyCollection()][string[]]$Arguments,
        [Parameter(Mandatory)][string]$OutputPath,
        [Parameter(Mandatory)][scriptblock]$Invoker,
        [Parameter(Mandatory)][scriptblock]$Clock,
        [Parameter(Mandatory)][string]$ReceiptPath,
        [Parameter(Mandatory)][string]$RunId,
        [int[]]$AllowedExitCodes = @(0),
        [int]$MemoryAvailableMiB = -1,
        [bool]$CaptureOutput = $true
    )

    $started = [DateTime](& $Clock).ToUniversalTime()
    $commandText = ConvertTo-WslBuildGateCommandText -Arguments $Arguments
    $startDetails = @{
        label = $Label
        command = @('wsl.exe') + @($Arguments)
        command_text = ('wsl.exe ' + $commandText)
        output_log = $OutputPath
        started_utc = $started.ToString('o')
        available_memory_mib = $MemoryAvailableMiB
    }
    $null = Write-WslBuildGateReceipt -ReceiptPath $ReceiptPath -RunId $RunId `
        -Event 'command_started' -Status INFO -TimestampUtc $started -Details $startDetails

    $raw = $null
    $invocationError = $null
    try {
        $raw = & $Invoker -Arguments ([string[]]$Arguments) -OutputPath $OutputPath -CaptureOutput $CaptureOutput
    }
    catch {
        $invocationError = $_.Exception.Message
        $raw = [pscustomobject]@{ exit_code = -1; output = @(); invocation_error = $invocationError }
    }
    if ($null -eq $raw) {
        $raw = [pscustomobject]@{ exit_code = -1; output = @(); invocation_error = 'Command invoker returned no result.' }
    }

    $ended = [DateTime](& $Clock).ToUniversalTime()
    $exitCode = [int]$raw.exit_code
    $allowed = @($AllowedExitCodes | ForEach-Object { [int]$_ })
    $success = $allowed -contains $exitCode -and [string]::IsNullOrWhiteSpace([string]$raw.invocation_error)
    $finishDetails = @{
        label = $Label
        command = @('wsl.exe') + @($Arguments)
        command_text = ('wsl.exe ' + $commandText)
        output_log = $OutputPath
        started_utc = $started.ToString('o')
        ended_utc = $ended.ToString('o')
        duration_ms = [math]::Round(($ended - $started).TotalMilliseconds, 0)
        exit_code = $exitCode
        allowed_exit_codes = $allowed
        invocation_error = $raw.invocation_error
        output_lines_captured = @($raw.output).Count
        available_memory_mib = $MemoryAvailableMiB
    }
    $null = Write-WslBuildGateReceipt -ReceiptPath $ReceiptPath -RunId $RunId `
        -Event 'command_finished' -Status $(if ($success) { 'PASS' } else { 'FAIL_CLOSED' }) `
        -TimestampUtc $ended -Details $finishDetails

    return [pscustomobject][ordered]@{
        exit_code = $exitCode
        output = @($raw.output)
        invocation_error = $raw.invocation_error
        started_utc = $started
        ended_utc = $ended
        duration_ms = [math]::Round(($ended - $started).TotalMilliseconds, 0)
        success = $success
        command = @('wsl.exe') + @($Arguments)
    }
}

function Get-WslBuildGateWorldserverPids {
    [CmdletBinding()]
    param([Parameter(Mandatory)]$CommandResult)

    $lines = @($CommandResult.output) | ForEach-Object { ([string]$_).Trim() } |
        Where-Object { $_ -match '^\d+$' }
    if ($CommandResult.exit_code -eq 1 -and @($lines).Count -eq 0) { return @() }
    if ($CommandResult.exit_code -ne 0 -or @($lines).Count -eq 0) {
        throw "Unable to determine whether WSL worldserver is live (pgrep exit $($CommandResult.exit_code))."
    }
    return @($lines | ForEach-Object { [int]$_ })
}

function Get-WslBuildGateAvailableMemoryMiB {
    [CmdletBinding()]
    param([Parameter(Mandatory)]$CommandResult)

    if (-not $CommandResult.success) {
        throw "Unable to read WSL MemAvailable (exit $($CommandResult.exit_code))."
    }
    $text = (@($CommandResult.output) -join "`n")
    $match = [regex]::Match($text, '(?im)^\s*MemAvailable:\s*(?<kb>\d+)\s*kB\s*$')
    if (-not $match.Success) {
        throw 'WSL /proc/meminfo did not contain a parseable MemAvailable row.'
    }
    return [int][math]::Floor(([int64]$match.Groups['kb'].Value) / 1024)
}

function New-WslBuildGateMutex {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Name)

    [bool]$createdNew = $false
    # Global\ makes this a machine-wide named mutex across sessions/users.
    return [System.Threading.Mutex]::new($false, $Name, [ref]$createdNew)
}

function Acquire-WslBuildGateLock {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]$Lock,
        [Parameter(Mandatory)][bool]$Wait,
        [Parameter(Mandatory)][int]$WaitTimeoutSeconds,
        [Parameter(Mandatory)][int]$PollMilliseconds,
        [Parameter(Mandatory)][scriptblock]$Clock,
        [Parameter(Mandatory)][scriptblock]$SleepInvoker,
        [Parameter(Mandatory)][string]$ReceiptPath,
        [Parameter(Mandatory)][string]$RunId,
        [Parameter(Mandatory)][string]$LockName
    )

    $attempt = 0
    $abandoned = $false
    $acquired = $false
    $deadline = ([DateTime](& $Clock).ToUniversalTime()).AddSeconds($WaitTimeoutSeconds)
    while (-not $acquired) {
        $attempt++
        try {
            if ($Wait) {
                $acquired = [bool]$Lock.WaitOne([TimeSpan]::FromMilliseconds($PollMilliseconds))
            }
            else {
                $acquired = [bool]$Lock.WaitOne(0)
            }
        }
        catch [System.Threading.AbandonedMutexException] {
            $acquired = $true
            $abandoned = $true
        }

        if ($acquired) { break }

        if (-not $Wait) {
            $now = ([DateTime](& $Clock).ToUniversalTime())
            $null = Write-WslBuildGateReceipt -ReceiptPath $ReceiptPath -RunId $RunId `
                -Event 'lock_refused' -Status FAIL_CLOSED -TimestampUtc $now -Details @{
                    attempts = $attempt
                    wait_requested = $false
                    lock_name = $LockName
                }
            throw 'WSL build gate is already held; refusing to start a concurrent build. Re-run with -Wait to queue.'
        }

        $now = ([DateTime](& $Clock).ToUniversalTime())
        if ($now -ge $deadline) {
            $null = Write-WslBuildGateReceipt -ReceiptPath $ReceiptPath -RunId $RunId `
                -Event 'lock_wait_timeout' -Status FAIL_CLOSED -TimestampUtc $now -Details @{
                    attempts = $attempt
                        wait_requested = $true
                        wait_timeout_seconds = $WaitTimeoutSeconds
                        lock_name = $LockName
                }
            throw "Timed out after $WaitTimeoutSeconds seconds waiting for the WSL build gate."
        }

        $null = Write-WslBuildGateReceipt -ReceiptPath $ReceiptPath -RunId $RunId `
            -Event 'lock_waiting' -Status INFO -TimestampUtc $now -Details @{
                attempt = $attempt
                wait_timeout_seconds = $WaitTimeoutSeconds
                lock_name = $LockName
            }
        $remainingMs = [int][math]::Max(1, [math]::Min(
            $PollMilliseconds,
            (($deadline - $now).TotalMilliseconds)))
        & $SleepInvoker $remainingMs
    }

    $acquiredAt = ([DateTime](& $Clock).ToUniversalTime())
    $null = Write-WslBuildGateReceipt -ReceiptPath $ReceiptPath -RunId $RunId `
        -Event 'lock_acquired' -Status PASS -TimestampUtc $acquiredAt -Details @{
            attempts = $attempt
            abandoned_mutex_recovered = $abandoned
            lock_name = $LockName
        }
    return [pscustomobject][ordered]@{
        acquired = $true
        attempts = $attempt
        abandoned_mutex_recovered = $abandoned
    }
}

function Invoke-WslBuildGate {
    [CmdletBinding()]
    param(
        [ValidateSet('CMake', 'Ninja')][string]$Tool = 'CMake',
        [Parameter(Mandatory)][string]$BuildDirectory,
        [Parameter(Mandatory)][AllowEmptyCollection()][string[]]$Target,
        [string]$WslDistro = 'Ubuntu-24.04',
        [ValidateRange(0, 12)][int]$Jobs = 0,
        [ValidateRange(256, 1048576)][int]$MinimumAvailableMemoryMiB = $script:WslBuildGateDefaultMemoryMiB,
        [switch]$Wait,
        [ValidateRange(1, 86400)][int]$WaitTimeoutSeconds = $script:WslBuildGateDefaultWaitTimeoutSeconds,
        [ValidateRange(100, 10000)][int]$LockPollMilliseconds = $script:WslBuildGateDefaultLockPollMilliseconds,
        [switch]$DryRun,
        [string]$LockName = $script:WslBuildGateMutexName,
        [string]$ReceiptPath = '',
        [string]$OutputLogPath = '',
        [scriptblock]$WslInvoker,
        [scriptblock]$LockFactory,
        [scriptblock]$Clock,
        [scriptblock]$SleepInvoker
    )

    if ($null -eq $WslInvoker) {
        $WslInvoker = {
            param([string[]]$Arguments, [string]$OutputPath, [bool]$CaptureOutput)
            Invoke-WslBuildGateDefaultCommand -Arguments $Arguments -OutputPath $OutputPath -CaptureOutput $CaptureOutput
        }
    }
    if ($null -eq $LockFactory) {
        $LockFactory = { param([string]$Name) New-WslBuildGateMutex -Name $Name }
    }
    if ($null -eq $Clock) {
        $Clock = { Get-WslBuildGateUtcNow }
    }
    if ($null -eq $SleepInvoker) {
        $SleepInvoker = { param([int]$Milliseconds) Start-Sleep -Milliseconds $Milliseconds }
    }

    $startedUtc = ([DateTime](& $Clock).ToUniversalTime())
    $runId = 'wsl-build-gate-{0}-{1}-{2}' -f $startedUtc.ToString('yyyyMMdd-HHmmss'), $PID, ([guid]::NewGuid().ToString('N').Substring(0, 8))
    $logRoot = Join-Path $script:WslBuildGateLaneRoot 'logs'
    if ([string]::IsNullOrWhiteSpace($ReceiptPath)) {
        $ReceiptPath = Join-Path $logRoot ("$runId.jsonl")
    }
    if ([string]::IsNullOrWhiteSpace($OutputLogPath)) {
        $OutputLogPath = Join-Path $logRoot ("$runId.output.log")
    }

    $null = Write-WslBuildGateReceipt -ReceiptPath $ReceiptPath -RunId $runId `
        -Event 'run_started' -Status INFO -TimestampUtc $startedUtc -Details @{
            tool = $Tool
            build_directory = $BuildDirectory
            targets = @($Target)
            distro = $WslDistro
            requested_jobs = $Jobs
            minimum_available_memory_mib = $MinimumAvailableMemoryMiB
            wait_requested = [bool]$Wait
            wait_timeout_seconds = $WaitTimeoutSeconds
            dry_run = [bool]$DryRun
            output_log = $OutputLogPath
        }

    $lock = $null
    $lockAcquired = $false
    $finalExitCode = -1
    $summary = $null
    try {
        Assert-WslBuildGateDistro -Distro $WslDistro
        Assert-WslBuildGatePathToken -Path $BuildDirectory -Name 'BuildDirectory'
        if (@($Target).Count -lt 1) { throw 'At least one build target is required.' }
        foreach ($targetName in @($Target)) { Assert-WslBuildGateTarget -Target $targetName }
        if ($Jobs -lt 0 -or $Jobs -gt $script:WslBuildGateHardMaxJobs) {
            throw "Jobs must be between 0 (automatic) and $($script:WslBuildGateHardMaxJobs)."
        }

        $lock = & $LockFactory -Name $LockName
        if ($null -eq $lock) { throw 'Lock factory returned no lock object.' }
        $lockAttemptAt = ([DateTime](& $Clock).ToUniversalTime())
        $null = Write-WslBuildGateReceipt -ReceiptPath $ReceiptPath -RunId $runId `
            -Event 'lock_attempted' -Status INFO -TimestampUtc $lockAttemptAt -Details @{
                lock_name = $LockName
                wait_requested = [bool]$Wait
                wait_timeout_seconds = $WaitTimeoutSeconds
            }
        $lockResult = Acquire-WslBuildGateLock -Lock $lock -Wait ([bool]$Wait) `
            -WaitTimeoutSeconds $WaitTimeoutSeconds -PollMilliseconds $LockPollMilliseconds `
            -Clock $Clock -SleepInvoker $SleepInvoker -ReceiptPath $ReceiptPath -RunId $runId -LockName $LockName
        $lockAcquired = [bool]$lockResult.acquired

        $worldserverProbe = Invoke-WslBuildGateLoggedCommand -Label 'worldserver_probe' `
            -Arguments @('-d', $WslDistro, '-u', 'root', '--', 'pgrep', '-x', 'worldserver') `
            -OutputPath $OutputLogPath -Invoker $WslInvoker -Clock $Clock -ReceiptPath $ReceiptPath `
            -RunId $runId -AllowedExitCodes @(0, 1) -CaptureOutput $true
        $worldserverPids = @(Get-WslBuildGateWorldserverPids -CommandResult $worldserverProbe)
        $liveWorldserver = $worldserverPids.Count -gt 0
        $null = Write-WslBuildGateReceipt -ReceiptPath $ReceiptPath -RunId $runId `
            -Event 'worldserver_state' -Status PASS -TimestampUtc $worldserverProbe.ended_utc -Details @{
                live = $liveWorldserver
                pids = $worldserverPids
                observation = 'read-only pgrep -x worldserver; no process was terminated'
            }

        $memoryProbe = Invoke-WslBuildGateLoggedCommand -Label 'memory_probe' `
            -Arguments @('-d', $WslDistro, '-u', 'root', '--', 'cat', '/proc/meminfo') `
            -OutputPath $OutputLogPath -Invoker $WslInvoker -Clock $Clock -ReceiptPath $ReceiptPath `
            -RunId $runId -AllowedExitCodes @(0) -CaptureOutput $true
        $availableMemoryMiB = Get-WslBuildGateAvailableMemoryMiB -CommandResult $memoryProbe
        $null = Write-WslBuildGateReceipt -ReceiptPath $ReceiptPath -RunId $runId `
            -Event 'memory_observed' -Status PASS -TimestampUtc $memoryProbe.ended_utc -Details @{
                available_memory_mib = $availableMemoryMiB
                minimum_required_mib = $MinimumAvailableMemoryMiB
            }
        if ($availableMemoryMiB -lt $MinimumAvailableMemoryMiB) {
            throw "WSL available memory ${availableMemoryMiB} MiB is below the configured threshold ${MinimumAvailableMemoryMiB} MiB."
        }

        $jobCap = if ($liveWorldserver) { $script:WslBuildGateLiveJobs } else { $script:WslBuildGateOfflineJobs }
        $effectiveJobs = if ($Jobs -eq 0) { $jobCap } else { $Jobs }
        if ($effectiveJobs -gt $jobCap) {
            $mode = if ($liveWorldserver) { 'live worldserver' } else { 'offline WSL' }
            throw "Requested $effectiveJobs jobs exceeds the $jobCap-job cap for $mode."
        }
        if ($effectiveJobs -gt $script:WslBuildGateHardMaxJobs) {
            throw "Effective jobs $effectiveJobs exceeds hard maximum $($script:WslBuildGateHardMaxJobs)."
        }

        $buildArguments = New-WslBuildGateCommandVector -Tool $Tool -Distro $WslDistro `
            -BuildDirectory $BuildDirectory -Target $Target -Jobs $effectiveJobs
        $buildCommandText = ConvertTo-WslBuildGateCommandText -Arguments $buildArguments
        $planAt = ([DateTime](& $Clock).ToUniversalTime())
        $null = Write-WslBuildGateReceipt -ReceiptPath $ReceiptPath -RunId $runId `
            -Event 'build_planned' -Status PASS -TimestampUtc $planAt -Details @{
                live_worldserver = $liveWorldserver
                worldserver_pids = $worldserverPids
                available_memory_mib = $availableMemoryMiB
                requested_jobs = $Jobs
                effective_jobs = $effectiveJobs
                live_job_cap = $script:WslBuildGateLiveJobs
                offline_job_cap = $script:WslBuildGateOfflineJobs
                hard_max_jobs = $script:WslBuildGateHardMaxJobs
                command = @('wsl.exe') + @($buildArguments)
                command_text = ('wsl.exe ' + $buildCommandText)
                dry_run = [bool]$DryRun
            }

        if ($DryRun) {
            $finalExitCode = 0
            $dryRunAt = ([DateTime](& $Clock).ToUniversalTime())
            $null = Write-WslBuildGateReceipt -ReceiptPath $ReceiptPath -RunId $runId `
                -Event 'dry_run_skipped_build' -Status PASS -TimestampUtc $dryRunAt -Details @{
                    command = @('wsl.exe') + @($buildArguments)
                    command_text = ('wsl.exe ' + $buildCommandText)
                    build_invoked = $false
                    available_memory_mib = $availableMemoryMiB
                }
        }
        else {
            $buildResult = Invoke-WslBuildGateLoggedCommand -Label 'build' -Arguments $buildArguments `
                -OutputPath $OutputLogPath -Invoker $WslInvoker -Clock $Clock -ReceiptPath $ReceiptPath `
                -RunId $runId -AllowedExitCodes @(0) -MemoryAvailableMiB $availableMemoryMiB -CaptureOutput $false
            $finalExitCode = [int]$buildResult.exit_code

            # A post-build read is observational only.  Preserve the build's
            # exit code if WSL was too unhealthy to answer after an OOM.
            try {
                $postMemoryProbe = Invoke-WslBuildGateLoggedCommand -Label 'post_build_memory_probe' `
                    -Arguments @('-d', $WslDistro, '-u', 'root', '--', 'cat', '/proc/meminfo') `
                    -OutputPath $OutputLogPath -Invoker $WslInvoker -Clock $Clock -ReceiptPath $ReceiptPath `
                    -RunId $runId -AllowedExitCodes @(0) -CaptureOutput $true
                $postMemoryMiB = Get-WslBuildGateAvailableMemoryMiB -CommandResult $postMemoryProbe
                $null = Write-WslBuildGateReceipt -ReceiptPath $ReceiptPath -RunId $runId `
                    -Event 'post_build_memory' -Status PASS -TimestampUtc $postMemoryProbe.ended_utc -Details @{
                        available_memory_mib = $postMemoryMiB
                    }
            }
            catch {
                $null = Write-WslBuildGateReceipt -ReceiptPath $ReceiptPath -RunId $runId `
                    -Event 'post_build_memory_unavailable' -Status WARN -TimestampUtc (([DateTime](& $Clock).ToUniversalTime())) -Details @{
                        error = $_.Exception.Message
                    }
            }

            if ($buildResult.exit_code -ne 0 -or -not $buildResult.success) {
                throw "Build command failed with exit code $($buildResult.exit_code). See $OutputLogPath."
            }
            $null = Write-WslBuildGateReceipt -ReceiptPath $ReceiptPath -RunId $runId `
                -Event 'build_succeeded' -Status PASS -TimestampUtc $buildResult.ended_utc -Details @{
                    exit_code = $buildResult.exit_code
                    command = @('wsl.exe') + @($buildArguments)
                }
        }

        $finalExitCode = 0
        $summary = [pscustomobject][ordered]@{
            schema = $script:WslBuildGateSchema
            run_id = $runId
            receipt_path = $ReceiptPath
            output_log_path = $OutputLogPath
            tool = $Tool
            build_directory = $BuildDirectory
            targets = @($Target)
            distro = $WslDistro
            live_worldserver = $liveWorldserver
            worldserver_pids = $worldserverPids
            available_memory_mib = $availableMemoryMiB
            requested_jobs = $Jobs
            effective_jobs = $effectiveJobs
            dry_run = [bool]$DryRun
            build_invoked = -not [bool]$DryRun
            exit_code = 0
        }
        $null = Write-WslBuildGateReceipt -ReceiptPath $ReceiptPath -RunId $runId `
            -Event 'gate_succeeded' -Status PASS -TimestampUtc (([DateTime](& $Clock).ToUniversalTime())) -Details @{
                exit_code = 0
                build_invoked = -not [bool]$DryRun
            }
        return $summary
    }
    catch {
        if ($finalExitCode -eq -1) { $finalExitCode = 1 }
        $errorAt = ([DateTime](& $Clock).ToUniversalTime())
        $null = Write-WslBuildGateReceipt -ReceiptPath $ReceiptPath -RunId $runId `
            -Event 'gate_failed' -Status FAIL_CLOSED -TimestampUtc $errorAt -Details @{
                exit_code = $finalExitCode
                error = $_.Exception.Message
            }
        throw
    }
    finally {
        if ($lockAcquired -and $null -ne $lock) {
            try {
                $lock.ReleaseMutex()
                $null = Write-WslBuildGateReceipt -ReceiptPath $ReceiptPath -RunId $runId `
                    -Event 'lock_released' -Status PASS -TimestampUtc (([DateTime](& $Clock).ToUniversalTime())) -Details @{
                        lock_name = $LockName
                    }
            }
            catch {
                $finalExitCode = 1
                $null = Write-WslBuildGateReceipt -ReceiptPath $ReceiptPath -RunId $runId `
                    -Event 'lock_release_failed' -Status FAIL_CLOSED -TimestampUtc (([DateTime](& $Clock).ToUniversalTime())) -Details @{
                        lock_name = $LockName
                        error = $_.Exception.Message
                    }
            }
            finally {
                if ($lock.PSObject.Methods.Name -contains 'Dispose') { $lock.Dispose() }
            }
        }
        elseif ($null -ne $lock -and $lock.PSObject.Methods.Name -contains 'Dispose') {
            $lock.Dispose()
        }

        $null = Write-WslBuildGateReceipt -ReceiptPath $ReceiptPath -RunId $runId `
            -Event 'run_finished' -Status $(if ($finalExitCode -eq 0) { 'PASS' } else { 'FAIL_CLOSED' }) `
            -TimestampUtc (([DateTime](& $Clock).ToUniversalTime())) -Details @{
                exit_code = $finalExitCode
                lock_released = $lockAcquired
            }
    }
}
