<##
.SYNOPSIS
    Runs the existing persistent campaign supervisor on a bounded polling loop.
.DESCRIPTION
    This sidecar owns only its own exclusive lock and JSONL receipt. It delegates
    every reconcile to persistent-campaign-supervisor.ps1 and never discovers,
    starts, stops, deletes, or repairs campaign children itself.

    The watcher is dry-run by default. -Apply is required before the delegated
    supervisor receives -Apply.
#>
[CmdletBinding()]
param(
    [ValidateSet('questing', 'gathering')]
    [string]$Phase = 'questing',
    [ValidateRange(1, 24)]
    [int]$DurationHours = 24,
    [ValidateRange(60, 900)]
    [int]$PollSeconds = 60,
    [switch]$Apply,
    [switch]$LibraryOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:WatchSchema = 'autowow.persistent-campaign-watch'
$script:WatchSchemaVersion = 1

function Get-WatchFullPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    return [System.IO.Path]::GetFullPath($Path)
}

function Get-FixedWatchPaths {
    [string]$root = Get-WatchFullPath (Join-Path $PSScriptRoot '..')
    [ordered]@{
        server_root = $root
        supervisor_path = Get-WatchFullPath (Join-Path $PSScriptRoot 'persistent-campaign-supervisor.ps1')
        work_root = Get-WatchFullPath (Join-Path $root 'work\persistent-campaign-watch')
        lock_path = Get-WatchFullPath (Join-Path $root 'work\persistent-campaign-watch\watch.lock')
        receipt_path = Get-WatchFullPath (Join-Path $root 'work\persistent-campaign-watch\persistent-campaign-watch.jsonl')
    }
}

function Test-WatchSupervisor {
    param([Parameter(Mandatory = $true)][string]$Path)

    $fullPath = Get-WatchFullPath $Path
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
        return [pscustomobject][ordered]@{
            valid = $false
            path = $fullPath
            reason = 'supervisor_missing'
            parse_errors = @()
        }
    }

    $tokens = $null
    $errors = $null
    try {
        [System.Management.Automation.Language.Parser]::ParseFile($fullPath, [ref]$tokens, [ref]$errors) | Out-Null
    }
    catch {
        return [pscustomobject][ordered]@{
            valid = $false
            path = $fullPath
            reason = 'supervisor_parse_exception'
            parse_errors = @([string]$_.Exception.Message)
        }
    }

    $parseErrors = @($errors | ForEach-Object { [string]$_.Message })
    [pscustomobject][ordered]@{
        valid = ($parseErrors.Count -eq 0)
        path = $fullPath
        reason = if ($parseErrors.Count -eq 0) { 'parse_clean' } else { 'supervisor_parse_invalid' }
        parse_errors = $parseErrors
    }
}

function New-WatchSupervisorArguments {
    param(
        [Parameter(Mandatory = $true)][string]$SupervisorPath,
        [Parameter(Mandatory = $true)][string]$ServerRoot,
        [Parameter(Mandatory = $true)][ValidateSet('questing', 'gathering')][string]$Phase,
        [Parameter(Mandatory = $true)][ValidateRange(1, 1440)][int]$DurationMinutes,
        [Parameter(Mandatory = $true)][bool]$Apply
    )

    $arguments = [System.Collections.Generic.List[string]]::new()
    foreach ($value in @(
        '-NoProfile',
        '-ExecutionPolicy', 'Bypass',
        '-File', (Get-WatchFullPath $SupervisorPath),
        '-ServerRoot', (Get-WatchFullPath $ServerRoot),
        '-Phase', $Phase,
        '-DurationMinutes', ([string]$DurationMinutes)
    )) {
        $arguments.Add([string]$value)
    }
    if ($Apply) { $arguments.Add('-Apply') }
    return @($arguments)
}

function ConvertTo-WatchProcessArgument {
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Value)

    if ($Value -notmatch '[\s"]') { return $Value }
    return '"' + $Value.Replace('"', '\"') + '"'
}

function Invoke-WatchReconcile {
    param(
        [Parameter(Mandatory = $true)][string]$SupervisorPath,
        [Parameter(Mandatory = $true)][string]$ServerRoot,
        [Parameter(Mandatory = $true)][ValidateSet('questing', 'gathering')][string]$Phase,
        [Parameter(Mandatory = $true)][ValidateRange(1, 1440)][int]$DurationMinutes,
        [Parameter(Mandatory = $true)][bool]$Apply
    )

    $startedUtc = [datetime]::UtcNow
    $process = $null
    $hostPath = $null
    $arguments = @()
    $commandLine = ''

    try {
        $hostCommand = Get-Command pwsh.exe -ErrorAction SilentlyContinue
        if ($null -eq $hostCommand) { $hostCommand = Get-Command powershell.exe -ErrorAction Stop }
        $hostPath = $hostCommand.Source
        $arguments = New-WatchSupervisorArguments -SupervisorPath $SupervisorPath -ServerRoot $ServerRoot -Phase $Phase -DurationMinutes $DurationMinutes -Apply $Apply
        $commandLine = $hostPath + ' ' + (($arguments | ForEach-Object { ConvertTo-WatchProcessArgument ([string]$_) }) -join ' ')
        $info = [System.Diagnostics.ProcessStartInfo]::new()
        $info.FileName = $hostPath
        $info.UseShellExecute = $false
        $info.CreateNoWindow = $true
        $info.RedirectStandardOutput = $true
        $info.RedirectStandardError = $true

        $argumentListProperty = $info.PSObject.Properties['ArgumentList']
        if ($null -ne $argumentListProperty) {
            foreach ($argument in $arguments) { [void]$argumentListProperty.Value.Add([string]$argument) }
        }
        else {
            $info.Arguments = (($arguments | ForEach-Object { ConvertTo-WatchProcessArgument ([string]$_) }) -join ' ')
        }

        $process = [System.Diagnostics.Process]::new()
        $process.StartInfo = $info
        if (-not $process.Start()) { throw 'The supervisor child process did not start.' }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $process.WaitForExit()
        $stdout = $stdoutTask.GetAwaiter().GetResult()
        $stderr = $stderrTask.GetAwaiter().GetResult()
        $exitCode = [int]$process.ExitCode
        [pscustomobject][ordered]@{
            success = ($exitCode -eq 0)
            exit_code = $exitCode
            stdout = [string]$stdout
            stderr = [string]$stderr
            error = $null
            command_line = $commandLine
            started_utc = $startedUtc.ToString('o')
            completed_utc = [datetime]::UtcNow.ToString('o')
            duration_ms = [int]([datetime]::UtcNow - $startedUtc).TotalMilliseconds
            apply = [bool]$Apply
        }
    }
    catch {
        [pscustomobject][ordered]@{
            success = $false
            exit_code = -1
            stdout = ''
            stderr = ''
            error = [string]$_.Exception.Message
            command_line = $commandLine
            started_utc = $startedUtc.ToString('o')
            completed_utc = [datetime]::UtcNow.ToString('o')
            duration_ms = [int]([datetime]::UtcNow - $startedUtc).TotalMilliseconds
            apply = [bool]$Apply
        }
    }
    finally {
        if ($null -ne $process) { $process.Dispose() }
    }
}

function Get-WatchBackoffSeconds {
    param(
        [Parameter(Mandatory = $true)][ValidateRange(1, 900)][int]$PollSeconds,
        [Parameter(Mandatory = $true)][ValidateRange(1, 30)][int]$ConsecutiveFailures
    )

    $multiplier = [Math]::Pow(2, [Math]::Min(4, $ConsecutiveFailures))
    return [int][Math]::Min(900, [Math]::Max($PollSeconds, [Math]::Ceiling($PollSeconds * $multiplier)))
}

function Acquire-WatchLock {
    param([Parameter(Mandatory = $true)][string]$Path)

    $parent = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    try {
        $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::OpenOrCreate, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
    }
    catch {
        throw "Another persistent campaign watcher instance holds the non-destructive lock: $Path"
    }

    $owner = [ordered]@{
        schema = 'autowow.persistent-campaign-watch.lock.v1'
        pid = $PID
        acquired_utc = [datetime]::UtcNow.ToString('o')
    }
    $bytes = [System.Text.UTF8Encoding]::new($false).GetBytes(($owner | ConvertTo-Json -Compress))
    $stream.Position = 0
    $stream.Write($bytes, 0, $bytes.Length)
    $stream.Flush()
    return $stream
}

function Write-WatchReceipt {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][ValidateSet('started', 'reconcile_result', 'failure', 'completed')][string]$Event,
        [hashtable]$Fields = @{}
    )

    $parent = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    $record = [ordered]@{
        schema = $script:WatchSchema
        schema_version = $script:WatchSchemaVersion
        timestamp_utc = [datetime]::UtcNow.ToString('o')
        event = $Event
    }
    foreach ($key in $Fields.Keys) { $record[$key] = $Fields[$key] }
    [System.IO.File]::AppendAllText($Path, (($record | ConvertTo-Json -Compress -Depth 16) + [Environment]::NewLine), [System.Text.UTF8Encoding]::new($false))
}

function Invoke-PersistentCampaignWatch {
    param(
        [Parameter(Mandatory = $true)][ValidateSet('questing', 'gathering')][string]$Phase,
        [Parameter(Mandatory = $true)][ValidateRange(1, 24)][int]$DurationHours,
        [Parameter(Mandatory = $true)][ValidateRange(60, 900)][int]$PollSeconds,
        [Parameter(Mandatory = $true)][bool]$Apply,
        [Parameter(Mandatory = $true)][hashtable]$Paths
    )

    $startedUtc = [datetime]::UtcNow
    $lockStream = $null
    $receiptStarted = $false
    $reconcileCount = 0
    $successCount = 0
    $failureCount = 0
    $consecutiveFailures = 0
    $lastError = $null
    $status = 'completed'

    try {
        $lockStream = Acquire-WatchLock -Path $Paths.lock_path
        Write-WatchReceipt -Path $Paths.receipt_path -Event started -Fields @{
            phase = $Phase
            duration_hours = $DurationHours
            poll_seconds = $PollSeconds
            apply = [bool]$Apply
            dry_run = (-not $Apply)
            server_root = $Paths.server_root
            supervisor_path = $Paths.supervisor_path
        }
        $receiptStarted = $true

        $validation = Test-WatchSupervisor -Path $Paths.supervisor_path
        if (-not $validation.valid) {
            $status = 'blocked_invalid_supervisor'
            $lastError = $validation.reason
            Write-WatchReceipt -Path $Paths.receipt_path -Event failure -Fields @{
                phase = $Phase
                kind = 'preflight'
                reason = $validation.reason
                parse_errors = @($validation.parse_errors)
            }
            return [pscustomobject][ordered]@{
                schema = $script:WatchSchema
                schema_version = $script:WatchSchemaVersion
                status = $status
                dry_run = (-not $Apply)
                apply = [bool]$Apply
                phase = $Phase
                duration_hours = $DurationHours
                poll_seconds = $PollSeconds
                reconciles = 0
                successes = 0
                failures = 1
                receipt_path = $Paths.receipt_path
                error = $lastError
                started_utc = $startedUtc.ToString('o')
            }
        }

        $deadlineUtc = $startedUtc.AddHours($DurationHours)
        $durationMinutes = [int]$DurationHours * 60
        while ([datetime]::UtcNow -lt $deadlineUtc) {
            $validation = Test-WatchSupervisor -Path $Paths.supervisor_path
            if (-not $validation.valid) {
                $status = 'blocked_invalid_supervisor'
                $lastError = $validation.reason
                Write-WatchReceipt -Path $Paths.receipt_path -Event failure -Fields @{
                    phase = $Phase
                    kind = 'preflight'
                    reason = $validation.reason
                    parse_errors = @($validation.parse_errors)
                }
                break
            }

            $reconcile = Invoke-WatchReconcile -SupervisorPath $Paths.supervisor_path -ServerRoot $Paths.server_root -Phase $Phase -DurationMinutes $durationMinutes -Apply $Apply
            $reconcileCount++
            $reconcileFields = @{
                phase = $Phase
                reconcile_number = $reconcileCount
                success = [bool]$reconcile.success
                exit_code = $reconcile.exit_code
                stdout = $reconcile.stdout
                stderr = $reconcile.stderr
                error = $reconcile.error
                command_line = $reconcile.command_line
                duration_ms = $reconcile.duration_ms
                apply = [bool]$Apply
            }
            Write-WatchReceipt -Path $Paths.receipt_path -Event reconcile_result -Fields $reconcileFields

            if ($reconcile.success) {
                $successCount++
                $consecutiveFailures = 0
                $delaySeconds = $PollSeconds
            }
            else {
                $failureCount++
                $consecutiveFailures++
                $lastError = if ($reconcile.error) { [string]$reconcile.error } elseif ($reconcile.stderr) { [string]$reconcile.stderr } else { "Supervisor exited with code $($reconcile.exit_code)." }
                $delaySeconds = Get-WatchBackoffSeconds -PollSeconds $PollSeconds -ConsecutiveFailures ([Math]::Min(30, $consecutiveFailures))
                Write-WatchReceipt -Path $Paths.receipt_path -Event failure -Fields @{
                    phase = $Phase
                    kind = 'reconcile'
                    reconcile_number = $reconcileCount
                    exit_code = $reconcile.exit_code
                    consecutive_failures = $consecutiveFailures
                    backoff_seconds = $delaySeconds
                    error = $lastError
                }
            }

            $remainingSeconds = [int][Math]::Floor(($deadlineUtc - [datetime]::UtcNow).TotalSeconds)
            if ($remainingSeconds -le 0) { break }
            Start-Sleep -Seconds ([Math]::Min($delaySeconds, $remainingSeconds))
        }

        if ($status -eq 'completed' -and $failureCount -gt 0) { $status = 'completed_with_errors' }
        [pscustomobject][ordered]@{
            schema = $script:WatchSchema
            schema_version = $script:WatchSchemaVersion
            status = $status
            dry_run = (-not $Apply)
            apply = [bool]$Apply
            phase = $Phase
            duration_hours = $DurationHours
            poll_seconds = $PollSeconds
            reconciles = $reconcileCount
            successes = $successCount
            failures = $failureCount
            consecutive_failures = $consecutiveFailures
            receipt_path = $Paths.receipt_path
            error = $lastError
            started_utc = $startedUtc.ToString('o')
        }
    }
    catch {
        $status = if ($null -eq $lockStream) { 'already_running' } else { 'failed' }
        $lastError = [string]$_.Exception.Message
        if ($receiptStarted) {
            Write-WatchReceipt -Path $Paths.receipt_path -Event failure -Fields @{ phase = $Phase; kind = 'watcher'; error = $lastError }
        }
        [pscustomobject][ordered]@{
            schema = $script:WatchSchema
            schema_version = $script:WatchSchemaVersion
            status = $status
            dry_run = (-not $Apply)
            apply = [bool]$Apply
            phase = $Phase
            duration_hours = $DurationHours
            poll_seconds = $PollSeconds
            reconciles = $reconcileCount
            successes = $successCount
            failures = $failureCount
            consecutive_failures = $consecutiveFailures
            receipt_path = $Paths.receipt_path
            error = $lastError
            started_utc = $startedUtc.ToString('o')
        }
    }
    finally {
        if ($receiptStarted) {
            try {
                Write-WatchReceipt -Path $Paths.receipt_path -Event completed -Fields @{
                    phase = $Phase
                    apply = [bool]$Apply
                    reconciles = $reconcileCount
                    successes = $successCount
                    failures = $failureCount
                    status = $status
                }
            }
            catch {
                # Receipt I/O cannot safely change the process/child safety boundary.
            }
        }
        if ($null -ne $lockStream) { $lockStream.Dispose() }
    }
}

if ($LibraryOnly) { return }

$paths = Get-FixedWatchPaths
$result = Invoke-PersistentCampaignWatch -Phase $Phase -DurationHours $DurationHours -PollSeconds $PollSeconds -Apply ([bool]$Apply) -Paths $paths
$result | ConvertTo-Json -Compress -Depth 8
if ($result.status -in @('already_running', 'blocked_invalid_supervisor', 'failed')) { exit 1 }
exit 0
