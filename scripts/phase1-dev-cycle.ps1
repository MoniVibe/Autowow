<#
.SYNOPSIS
    Safe developer lifecycle for the canonical Phase 1 Playerbots module.

.DESCRIPTION
    Plan is the default and performs no WSL or filesystem mutation. Doctor is
    read-only. Deploy requires -Apply and synchronizes only the canonical module
    into the exact /root/p1core/modules/mod-playerbots staging directory before
    building and restarting the existing Phase 1 WSL runtime.

    This script deliberately does not prepare, edit, or print database/config
    contents. Runtime lifecycle is composed through the existing stop/start
    scripts in this repository.
#>
[CmdletBinding()]
param(
    [ValidateSet('Plan', 'Doctor', 'Deploy')]
    [string]$Action = 'Plan',
    [string]$ServerRoot = 'D:\Games\wowstuff\AutoWoW',
    [string]$WslDistro = 'Ubuntu-24.04',
    [string]$FocusedGTestFilter = '*QuestObjective*:*Dungeon*:*Raid*:*WorkerGather*:*ExactPartyRepair*:*Resurrect*',
    [ValidateRange(1, 4)][int]$BuildJobs = 4,
    [ValidateRange(1, 1024)][int]$MinimumFreeDiskGiB = 5,
    [ValidateRange(256, 1048576)][int]$MinimumAvailableMemoryMiB = 1024,
    [switch]$Apply
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:Phase1DevCycleSchema = 'autowow.phase1.dev-cycle.v1'
$script:Phase1CanonicalWindowsRoot = 'D:\Games\wowstuff\AutoWoW'
$script:Phase1CanonicalModuleWindowsPath = 'D:\Games\wowstuff\AutoWoW\_phase1_worktree\mod-playerbots'
$script:Phase1CanonicalModuleWslPath = '/mnt/d/Games/wowstuff/AutoWoW/_phase1_worktree/mod-playerbots'
$script:Phase1StagingWslPath = '/root/p1core/modules/mod-playerbots'
$script:Phase1CoreWslPath = '/root/p1core'
$script:Phase1BuildWslPath = '/root/p1core/build'
$script:Phase1CacheWslPath = '/root/p1core/build/CMakeCache.txt'
$script:Phase1UnitTestsWslPath = '/root/p1core/build/src/test/unit_tests'
$script:Phase1WorldserverWslPath = '/root/p1core/build/src/server/apps/worldserver'
$script:Phase1RuntimeWslPath = '/root/p1runtime'
$script:Phase1RuntimeConfigWslPath = '/root/p1runtime/worldserver.conf'
$script:Phase1RuntimeLogWslPath = '/mnt/d/Games/wowstuff/AutoWoW/logs/phase1-runtime'
$script:Phase1BridgePort = 18787
$script:Phase1RsyncExcludes = @(
    '--exclude=.git/'
    '--exclude=build/'
    '--exclude=build-*/'
    '--exclude=generated/'
    '--exclude=logs/'
    '--exclude=*.log'
    '--exclude=*.log.*'
)

function New-Phase1DevCycleEvent {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Event,
        [ValidateSet('INFO', 'PASS', 'WARN', 'FAIL_CLOSED')][string]$Status = 'INFO',
        [hashtable]$Details = @{}
    )
    return [pscustomobject][ordered]@{
        schema = $script:Phase1DevCycleSchema
        event = $Event
        status = $Status
        utc = (Get-Date).ToUniversalTime().ToString('o')
        details = [pscustomobject]$Details
    }
}

function Write-Phase1DevCycleEvent {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$ReceiptPath,
        [Parameter(Mandatory)][string]$Event,
        [ValidateSet('INFO', 'PASS', 'WARN', 'FAIL_CLOSED')][string]$Status = 'INFO',
        [hashtable]$Details = @{}
    )
    New-Item -ItemType Directory -Path (Split-Path -Parent $ReceiptPath) -Force | Out-Null
    $record = New-Phase1DevCycleEvent -Event $Event -Status $Status -Details $Details
    [System.IO.File]::AppendAllText($ReceiptPath, (($record | ConvertTo-Json -Compress -Depth 20) + [Environment]::NewLine), [System.Text.UTF8Encoding]::new($false))
    return $record
}

function Get-Phase1DevCyclePlan {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Root,
        [Parameter(Mandatory)][string]$Distro,
        [Parameter(Mandatory)][string]$GTestFilter,
        [ValidateRange(1, 4)][int]$BuildJobs = 4,
        [Parameter(Mandatory)][int]$MinimumDiskGiB,
        [Parameter(Mandatory)][int]$MinimumMemoryMiB
    )
    $resolvedRoot = [System.IO.Path]::GetFullPath($Root)
    # --archive includes --times.  That is unsafe across the Windows/WSL rsync
    # boundary: a changed source file can retain an older mtime than the object
    # already in the build tree, so CMake can incorrectly consider it current.
    # --checksum detects same-size edits without trusting cross-filesystem mtimes, while
    # --no-times gives each transferred source a fresh destination mtime for CMake.
    # Unchanged content is not transferred, so this preserves incremental builds.
    $rsync = @('rsync', '--archive', '--checksum', '--no-times', '--delete') + $script:Phase1RsyncExcludes + @("$($script:Phase1CanonicalModuleWslPath)/", "$($script:Phase1StagingWslPath)/")
    $buildSequence = @(
        ,@('cmake', '--build', $script:Phase1BuildWslPath, '--target', 'unit_tests', '--', "-j$BuildJobs")
        ,@($script:Phase1UnitTestsWslPath, "--gtest_filter=$GTestFilter")
        ,@('cmake', '--build', $script:Phase1BuildWslPath, '--target', 'worldserver', '--', "-j$BuildJobs")
    )
    return [pscustomobject][ordered]@{
        schema = $script:Phase1DevCycleSchema
        action = 'Plan'
        apply = $false
        dry_run = $true
        distro = $Distro
        canonical_windows_root = $script:Phase1CanonicalWindowsRoot
        canonical_module_windows_path = $script:Phase1CanonicalModuleWindowsPath
        canonical_module_wsl_path = $script:Phase1CanonicalModuleWslPath
        staging_wsl_path = $script:Phase1StagingWslPath
        build_wsl_path = $script:Phase1BuildWslPath
        runtime_wsl_path = $script:Phase1RuntimeWslPath
        runtime_config_wsl_path = $script:Phase1RuntimeConfigWslPath
        focused_gtest_filter = $GTestFilter
        build_jobs = $BuildJobs
        minimum_free_disk_gib = $MinimumDiskGiB
        minimum_available_memory_mib = $MinimumMemoryMiB
        rsync_arguments = @($rsync)
        build_sequence = $buildSequence
        lifecycle = [pscustomobject][ordered]@{
            stop_script = Join-Path $resolvedRoot 'scripts\stop-phase1-wsl-worldserver.ps1'
            start_script = Join-Path $resolvedRoot 'scripts\start-phase1-wsl-worldserver.ps1'
            stop_only_if_worldserver_or_state_is_present = $true
            start_after_successful_hash_only = $true
            restart_stale_server_after_build_failure = $false
            safe_rollback = $false
        }
        receipts = [pscustomobject][ordered]@{
            directory = Join-Path $resolvedRoot 'logs\phase1-dev-cycle'
            format = 'JSONL receipt plus JSON summary'
        }
    }
}

function Invoke-Phase1DevCycleWsl {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Distro, [Parameter(Mandatory)][AllowEmptyCollection()][string[]]$Command)
    try {
        $output = @(& wsl.exe -d $Distro -u root -- @Command 2>&1 | ForEach-Object { [string]$_ })
        return [pscustomobject][ordered]@{ exit_code = [int]$LASTEXITCODE; command = @($Command); output = @($output); invocation_error = $null }
    } catch {
        return [pscustomobject][ordered]@{ exit_code = -1; command = @($Command); output = @(); invocation_error = 'WSL invocation failed.' }
    }
}

function Invoke-Phase1DevCycleWindowsScript {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$ScriptPath, [Parameter(Mandatory)][string]$Root, [Parameter(Mandatory)][string]$Distro)
    $stdoutPath = [System.IO.Path]::GetTempFileName()
    $stderrPath = [System.IO.Path]::GetTempFileName()
    $process = $null
    try {
        # File-backed redirection prevents a long-lived runtime descendant from inheriting this
        # orchestration pipeline and keeping Deploy open after the lifecycle child has exited.
        $process = Start-Process -FilePath 'pwsh.exe' -ArgumentList @(
            '-NoProfile','-File',$ScriptPath,'-ServerRoot',$Root,'-WslDistro',$Distro
        ) -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath `
          -WindowStyle Hidden -PassThru
        # Start-Process -Wait waits for the entire descendant tree on Windows. The successful
        # start lifecycle intentionally leaves worldserver and its relay alive, so wait only
        # for the direct lifecycle script process to exit.
        $process.WaitForExit()
        $process.Refresh()
        $output = @(
            @(Get-Content -LiteralPath $stdoutPath -ErrorAction SilentlyContinue)
            @(Get-Content -LiteralPath $stderrPath -ErrorAction SilentlyContinue)
        ) | ForEach-Object { [string]$_ }
        return [pscustomobject][ordered]@{ exit_code = [int]$process.ExitCode; output = @($output); invocation_error = $null }
    } catch {
        return [pscustomobject][ordered]@{ exit_code = -1; output = @(); invocation_error = 'Lifecycle script invocation failed.' }
    } finally {
        if ($null -ne $process) { $process.Dispose() }
        Remove-Item -LiteralPath $stdoutPath,$stderrPath -Force -ErrorAction SilentlyContinue
    }
}

function Get-Phase1DevCycleLastOutputLine {
    [CmdletBinding()]
    param([Parameter(Mandatory)][AllowEmptyCollection()][string[]]$Output)
    $line = ($Output | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Select-Object -Last 1) -as [string]
    if ($null -eq $line) { return '' }
    return $line.Trim()
}

function Write-Phase1DevCycleCommandLog {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$LogPath,
        [Parameter(Mandatory)][string]$Label,
        [Parameter(Mandatory)]$Result
    )
    New-Item -ItemType Directory -Path (Split-Path -Parent $LogPath) -Force | Out-Null
    $header = "[$(Get-Date -Format o)] $Label exit_code=$($Result.exit_code) command=$($Result.command -join ' ')"
    $output = @($Result.output)
    $text = @($header) + $(if ($output.Count -gt 0) { $output } else { '[no command output]' }) + @('')
    [System.IO.File]::AppendAllText($LogPath, (($text -join [Environment]::NewLine) + [Environment]::NewLine), [System.Text.UTF8Encoding]::new($false))
    return $LogPath
}

function Get-Phase1DevCycleResolvedWslPath {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Distro, [Parameter(Mandatory)][string]$Path)
    $result = Invoke-Phase1DevCycleWsl -Distro $Distro -Command @('readlink', '-f', '--', $Path)
    return [pscustomobject][ordered]@{ result = $result; resolved = Get-Phase1DevCycleLastOutputLine -Output $result.output }
}

function New-Phase1DevCycleCheck {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][ValidateSet('PASS', 'FAIL_CLOSED', 'WARN', 'INFO')][string]$Status,
        [hashtable]$Details = @{}
    )
    return [pscustomobject][ordered]@{ name = $Name; status = $Status; details = [pscustomobject]$Details }
}

function Get-Phase1DevCycleDistroCheck {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Distro)
    try {
        $raw = @(& wsl.exe -l -q 2>&1 | ForEach-Object { ([string]$_) -replace [char]0, '' })
        $exitCode = [int]$LASTEXITCODE
    } catch { $raw = @(); $exitCode = -1 }
    $distros = @($raw | ForEach-Object { $_.Trim() } | Where-Object { $_ })
    $present = $exitCode -eq 0 -and ($distros -contains $Distro)
    return New-Phase1DevCycleCheck -Name 'wsl_distro' -Status $(if ($present) { 'PASS' } else { 'FAIL_CLOSED' }) -Details @{
        requested = $Distro; present = $present; exit_code = $exitCode; installed_distros = @($distros)
    }
}

function Get-Phase1DevCycleToolCheck {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Distro, [Parameter(Mandatory)][ValidateSet('rsync', 'cmake')][string]$Tool)
    $result = Invoke-Phase1DevCycleWsl -Distro $Distro -Command @('sh', '-c', "command -v $Tool")
    $found = $result.exit_code -eq 0 -and -not [string]::IsNullOrWhiteSpace((Get-Phase1DevCycleLastOutputLine -Output $result.output))
    return New-Phase1DevCycleCheck -Name "tool_$Tool" -Status $(if ($found) { 'PASS' } else { 'FAIL_CLOSED' }) -Details @{
        tool = $Tool; available = $found; exit_code = $result.exit_code
    }
}

function Get-Phase1DevCycleExactPathCheck {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Distro,
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$ExpectedPath,
        [Parameter(Mandatory)][ValidateSet('-d', '-f', '-x')][string]$TestFlag
    )
    $test = Invoke-Phase1DevCycleWsl -Distro $Distro -Command @('test', $TestFlag, $Path)
    $resolvedResult = Get-Phase1DevCycleResolvedWslPath -Distro $Distro -Path $Path
    $resolved = $resolvedResult.resolved
    $exact = $resolvedResult.result.exit_code -eq 0 -and $resolved -ceq $ExpectedPath
    $present = $test.exit_code -eq 0
    return New-Phase1DevCycleCheck -Name $Name -Status $(if ($present -and $exact) { 'PASS' } else { 'FAIL_CLOSED' }) -Details @{
        path = $Path; expected_resolved_path = $ExpectedPath; resolved_path = $resolved; present = $present; exact = $exact
        test_exit_code = $test.exit_code; resolve_exit_code = $resolvedResult.result.exit_code
    }
}

function ConvertFrom-Phase1DevCycleCMakeCache {
    [CmdletBinding()]
    param([Parameter(Mandatory)][AllowEmptyString()][string]$Text)
    $cache = @{}
    foreach ($line in ($Text -split [Environment]::NewLine)) {
        if ($line -match '^(?<key>[^#/:=][^:=]*):[^=]+=(?<value>.*)$') { $cache[$Matches.key] = $Matches.value }
    }
    return $cache
}

function Get-Phase1DevCycleCacheCheck {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Distro)
    $result = Invoke-Phase1DevCycleWsl -Distro $Distro -Command @('cat', '--', $script:Phase1CacheWslPath)
    $cache = ConvertFrom-Phase1DevCycleCMakeCache -Text ($result.output -join [Environment]::NewLine)
    $source = if ($cache.ContainsKey('CMAKE_HOME_DIRECTORY')) { [string]$cache.CMAKE_HOME_DIRECTORY } else { '' }
    $sourceGuard = $source -ceq $script:Phase1CoreWslPath
    return [pscustomobject][ordered]@{
        name = 'cmake_cache_source_guard'
        status = if ($result.exit_code -eq 0 -and $sourceGuard) { 'PASS' } else { 'FAIL_CLOSED' }
        details = [pscustomobject][ordered]@{
            cache_path = $script:Phase1CacheWslPath; cache_read = $result.exit_code -eq 0
            cmake_home_directory = $source; expected_cmake_home_directory = $script:Phase1CoreWslPath; source_guard = $sourceGuard
        }
    }
}

function Get-Phase1DevCycleRuntimeState {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Distro)
    $process = Invoke-Phase1DevCycleWsl -Distro $Distro -Command @('pgrep', '-x', 'worldserver')
    $listeners = Invoke-Phase1DevCycleWsl -Distro $Distro -Command @('ss', '-ltn')
    $worldserverRunning = $process.exit_code -eq 0 -and @($process.output).Count -gt 0
    $processQueryOkay = $process.exit_code -in @(0, 1)
    $bridgeListening = (($listeners.output -join [Environment]::NewLine) -match "(?m):$($script:Phase1BridgePort)(?:\s|$)")
    return [pscustomobject][ordered]@{
        name = 'bridge_worldserver_state'
        status = if ($processQueryOkay -and $listeners.exit_code -eq 0) { 'PASS' } else { 'FAIL_CLOSED' }
        details = [pscustomobject][ordered]@{
            worldserver_running = $worldserverRunning; worldserver_pids = @($process.output | Where-Object { $_ -match '^\d+$' })
            bridge_port = $script:Phase1BridgePort; bridge_listening = $bridgeListening
            process_query_exit_code = $process.exit_code; listener_query_exit_code = $listeners.exit_code
        }
    }
}

function Get-Phase1DevCycleCapacityChecks {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Distro, [Parameter(Mandatory)][int]$MinimumDiskGiB, [Parameter(Mandatory)][int]$MinimumMemoryMiB)
    $disk = Invoke-Phase1DevCycleWsl -Distro $Distro -Command @('df', '-Pk', $script:Phase1BuildWslPath)
    [string[]]$diskLine = @($disk.output | Where-Object { $_ -match '^\S+\s+\d+\s+\d+\s+\d+\s+\d+%\s+\S+\s*$' } | Select-Object -Last 1)
    [string[]]$diskFields = if (@($diskLine).Length -eq 1) { [string[]]@($diskLine[0] -split '\s+') } else { [string[]]@() }
    $availableKb = $null
    if (@($diskFields).Length -ge 5 -and $diskFields[3] -match '^\d+$') { $availableKb = [int64]$diskFields[3] }
    $diskGiB = if ($null -eq $availableKb) { $null } else { [Math]::Round($availableKb / 1MB, 2) }
    $diskPass = $disk.exit_code -eq 0 -and $null -ne $diskGiB -and $diskGiB -ge $MinimumDiskGiB
    $memory = Invoke-Phase1DevCycleWsl -Distro $Distro -Command @('free', '-m')
    [string[]]$memoryLine = @($memory.output | Where-Object { $_ -match '^Mem:\s+' } | Select-Object -First 1)
    [string[]]$memoryFields = if (@($memoryLine).Length -eq 1) { [string[]]@($memoryLine[0] -split '\s+') } else { [string[]]@() }
    $availableMiB = $null
    if (@($memoryFields).Length -ge 7 -and $memoryFields[6] -match '^\d+$') { $availableMiB = [int64]$memoryFields[6] }
    $memoryPass = $memory.exit_code -eq 0 -and $null -ne $availableMiB -and $availableMiB -ge $MinimumMemoryMiB
    return @(
        (New-Phase1DevCycleCheck -Name 'disk_capacity' -Status $(if ($diskPass) { 'PASS' } else { 'FAIL_CLOSED' }) -Details @{
            path = $script:Phase1BuildWslPath; available_gib = $diskGiB; minimum_free_gib = $MinimumDiskGiB; query_exit_code = $disk.exit_code
        })
        (New-Phase1DevCycleCheck -Name 'memory_capacity' -Status $(if ($memoryPass) { 'PASS' } else { 'FAIL_CLOSED' }) -Details @{
            available_mib = $availableMiB; minimum_available_mib = $MinimumMemoryMiB; query_exit_code = $memory.exit_code
        })
    )
}

function Get-Phase1DevCycleDoctorReport {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Root, [Parameter(Mandatory)][string]$Distro, [Parameter(Mandatory)][int]$MinimumDiskGiB, [Parameter(Mandatory)][int]$MinimumMemoryMiB)
    $resolvedRoot = [System.IO.Path]::GetFullPath($Root)
    $checks = [System.Collections.Generic.List[object]]::new()
    [void]$checks.Add((Get-Phase1DevCycleDistroCheck -Distro $Distro))
    [void]$checks.Add((Get-Phase1DevCycleToolCheck -Distro $Distro -Tool 'rsync'))
    [void]$checks.Add((Get-Phase1DevCycleToolCheck -Distro $Distro -Tool 'cmake'))
    [void]$checks.Add((Get-Phase1DevCycleExactPathCheck -Distro $Distro -Name 'canonical_module' -Path $script:Phase1CanonicalModuleWslPath -ExpectedPath $script:Phase1CanonicalModuleWslPath -TestFlag '-d'))
    [void]$checks.Add((Get-Phase1DevCycleExactPathCheck -Distro $Distro -Name 'staging_module' -Path $script:Phase1StagingWslPath -ExpectedPath $script:Phase1StagingWslPath -TestFlag '-d'))
    [void]$checks.Add((Get-Phase1DevCycleExactPathCheck -Distro $Distro -Name 'build_root' -Path $script:Phase1BuildWslPath -ExpectedPath $script:Phase1BuildWslPath -TestFlag '-d'))
    [void]$checks.Add((Get-Phase1DevCycleExactPathCheck -Distro $Distro -Name 'cmake_cache' -Path $script:Phase1CacheWslPath -ExpectedPath $script:Phase1CacheWslPath -TestFlag '-f'))
    [void]$checks.Add((Get-Phase1DevCycleExactPathCheck -Distro $Distro -Name 'runtime_root' -Path $script:Phase1RuntimeWslPath -ExpectedPath $script:Phase1RuntimeWslPath -TestFlag '-d'))
    [void]$checks.Add((Get-Phase1DevCycleExactPathCheck -Distro $Distro -Name 'runtime_config' -Path $script:Phase1RuntimeConfigWslPath -ExpectedPath $script:Phase1RuntimeConfigWslPath -TestFlag '-f'))
    [void]$checks.Add((Get-Phase1DevCycleExactPathCheck -Distro $Distro -Name 'runtime_log_directory' -Path $script:Phase1RuntimeLogWslPath -ExpectedPath $script:Phase1RuntimeLogWslPath -TestFlag '-d'))
    [void]$checks.Add((Get-Phase1DevCycleCacheCheck -Distro $Distro))
    [void]$checks.Add((Get-Phase1DevCycleRuntimeState -Distro $Distro))
    foreach ($capacityCheck in (Get-Phase1DevCycleCapacityChecks -Distro $Distro -MinimumDiskGiB $MinimumDiskGiB -MinimumMemoryMiB $MinimumMemoryMiB)) { [void]$checks.Add($capacityCheck) }
    $localSource = [System.IO.Path]::GetFullPath($script:Phase1CanonicalModuleWindowsPath)
    $localLogDirectory = Join-Path $resolvedRoot 'logs\phase1-runtime'
    $localChecks = @(
        (New-Phase1DevCycleCheck -Name 'local_server_root' -Status $(if ($resolvedRoot -ieq $script:Phase1CanonicalWindowsRoot) { 'PASS' } else { 'FAIL_CLOSED' }) -Details @{ resolved = $resolvedRoot; expected = $script:Phase1CanonicalWindowsRoot })
        (New-Phase1DevCycleCheck -Name 'local_canonical_module' -Status $(if ((Test-Path -LiteralPath $localSource -PathType Container) -and $localSource -ieq $script:Phase1CanonicalModuleWindowsPath) { 'PASS' } else { 'FAIL_CLOSED' }) -Details @{ path = $localSource; expected = $script:Phase1CanonicalModuleWindowsPath; present = Test-Path -LiteralPath $localSource -PathType Container })
        (New-Phase1DevCycleCheck -Name 'local_runtime_log_directory' -Status $(if (Test-Path -LiteralPath $localLogDirectory -PathType Container) { 'PASS' } else { 'FAIL_CLOSED' }) -Details @{ path = $localLogDirectory; present = Test-Path -LiteralPath $localLogDirectory -PathType Container })
    )
    foreach ($localCheck in $localChecks) { [void]$checks.Add($localCheck) }
    foreach ($artifact in @(
        [pscustomobject][ordered]@{ path = $script:Phase1UnitTestsWslPath; kind = 'unit_tests' }
        [pscustomobject][ordered]@{ path = $script:Phase1WorldserverWslPath; kind = 'worldserver' }
    )) {
        $test = Invoke-Phase1DevCycleWsl -Distro $Distro -Command @('test', '-x', $artifact.path)
        [void]$checks.Add((New-Phase1DevCycleCheck -Name "artifact_$($artifact.kind)" -Status $(if ($test.exit_code -eq 0) { 'PASS' } else { 'WARN' }) -Details @{ path = $artifact.path; executable = $test.exit_code -eq 0; advisory = $true }))
    }
    $requiredFailures = @($checks | Where-Object status -eq 'FAIL_CLOSED')
    return [pscustomobject][ordered]@{
        schema = $script:Phase1DevCycleSchema; action = 'Doctor'; status = if ($requiredFailures.Count -eq 0) { 'PASS' } else { 'FAIL_CLOSED' }
        read_only = $true; apply = $false; distro = $Distro
        canonical_module_windows_path = $script:Phase1CanonicalModuleWindowsPath; canonical_module_wsl_path = $script:Phase1CanonicalModuleWslPath
        staging_wsl_path = $script:Phase1StagingWslPath; build_wsl_path = $script:Phase1BuildWslPath; runtime_wsl_path = $script:Phase1RuntimeWslPath
        checks = @($checks); required_failure_count = $requiredFailures.Count; warnings = @($checks | Where-Object status -eq 'WARN')
        generated_utc = (Get-Date).ToUniversalTime().ToString('o')
    }
}

function Assert-Phase1DevCycleDeployRoot {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Root)
    $resolved = [System.IO.Path]::GetFullPath($Root)
    if ($resolved -ine $script:Phase1CanonicalWindowsRoot) { throw "Deploy root guard failed: resolved '$resolved', expected '$($script:Phase1CanonicalWindowsRoot)'." }
    if ([System.IO.Path]::GetFullPath($script:Phase1CanonicalModuleWindowsPath) -ine [System.IO.Path]::GetFullPath((Join-Path $resolved '_phase1_worktree\mod-playerbots'))) { throw 'Canonical module path guard failed.' }
}

function Get-Phase1DevCycleSha256 {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Distro, [Parameter(Mandatory)][string]$Path)
    $result = Invoke-Phase1DevCycleWsl -Distro $Distro -Command @('sha256sum', '--', $Path)
    $line = Get-Phase1DevCycleLastOutputLine -Output $result.output
    if ($result.exit_code -ne 0 -or $line -notmatch '^(?<hash>[0-9a-fA-F]{64})\s+') { throw "Could not hash expected artifact '$Path'." }
    return $Matches.hash.ToLowerInvariant()
}

function Invoke-Phase1DevCycleDeploy {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Root, [Parameter(Mandatory)][string]$Distro, [Parameter(Mandatory)][string]$GTestFilter, [ValidateRange(1, 4)][int]$BuildJobs = 4, [Parameter(Mandatory)][int]$MinimumDiskGiB, [Parameter(Mandatory)][int]$MinimumMemoryMiB)
    Assert-Phase1DevCycleDeployRoot -Root $Root
    $resolvedRoot = [System.IO.Path]::GetFullPath($Root)
    $receiptDirectory = Join-Path $resolvedRoot 'logs\phase1-dev-cycle'
    $runId = (Get-Date).ToUniversalTime().ToString('yyyyMMdd-HHmmssfff')
    $receiptPath = Join-Path $receiptDirectory "phase1-dev-cycle-$runId.jsonl"
    $summaryPath = Join-Path $receiptDirectory "phase1-dev-cycle-$runId.json"
    $buildLogPath = Join-Path $receiptDirectory "phase1-dev-cycle-$runId.build.log"
    $logDirectory = Join-Path $resolvedRoot 'logs\phase1-runtime'
    $playerbotsLogPath = Join-Path $logDirectory 'Playerbots.log'
    $archivePath = Join-Path $logDirectory "Playerbots.log.before-deploy-$runId.log"
    $stopScript = Join-Path $resolvedRoot 'scripts\stop-phase1-wsl-worldserver.ps1'
    $startScript = Join-Path $resolvedRoot 'scripts\start-phase1-wsl-worldserver.ps1'
    $plan = Get-Phase1DevCyclePlan -Root $resolvedRoot -Distro $Distro -GTestFilter $GTestFilter -BuildJobs $BuildJobs -MinimumDiskGiB $MinimumDiskGiB -MinimumMemoryMiB $MinimumMemoryMiB
    $summary = [ordered]@{
        schema = $script:Phase1DevCycleSchema; action = 'Deploy'; apply = $true; status = 'IN_PROGRESS'; run_id = $runId
        started_utc = (Get-Date).ToUniversalTime().ToString('o'); distro = $Distro
        canonical_module_windows_path = $script:Phase1CanonicalModuleWindowsPath; canonical_module_wsl_path = $script:Phase1CanonicalModuleWslPath
        staging_wsl_path = $script:Phase1StagingWslPath; focused_gtest_filter = $GTestFilter; build_jobs = $BuildJobs
        receipt_path = $receiptPath; summary_path = $summaryPath; build_log_path = $buildLogPath; archive_path = $null; artifact = $null
        runtime_was_running = $false; runtime_stopped = $false; runtime_started = $false
        restart_skipped = $false; restart_skipped_reason = $null; failure = $null
    }
    try {
        [void](Write-Phase1DevCycleEvent -ReceiptPath $receiptPath -Event 'deploy_started' -Details @{ run_id = $runId; dry_run = $false; database_or_config_mutation = $false; build_log_path = $buildLogPath })
        $doctor = Get-Phase1DevCycleDoctorReport -Root $resolvedRoot -Distro $Distro -MinimumDiskGiB $MinimumDiskGiB -MinimumMemoryMiB $MinimumMemoryMiB
        if ($doctor.status -ne 'PASS') { throw "Doctor failed closed with $($doctor.required_failure_count) required failure(s)." }
        [void](Write-Phase1DevCycleEvent -ReceiptPath $receiptPath -Event 'doctor_passed' -Status PASS -Details @{ required_failure_count = $doctor.required_failure_count })
        $state = @($doctor.checks | Where-Object name -eq 'bridge_worldserver_state' | Select-Object -First 1)
        $runtimeWasRunning = [bool]$state[0].details.worldserver_running
        $statePath = Join-Path $resolvedRoot 'work\phase1-wsl-runtime\runtime-processes.json'
        $statePresent = Test-Path -LiteralPath $statePath -PathType Leaf
        $summary.runtime_was_running = $runtimeWasRunning -or $statePresent
        if (-not (Test-Path -LiteralPath $startScript -PathType Leaf)) { throw "Missing existing start script '$startScript'." }
        if (($runtimeWasRunning -or $statePresent) -and -not (Test-Path -LiteralPath $stopScript -PathType Leaf)) { throw "Missing existing stop script '$stopScript'." }
        [void](Write-Phase1DevCycleEvent -ReceiptPath $receiptPath -Event 'runtime_kept_live_for_build' -Status PASS -Details @{ worldserver_running = $runtimeWasRunning; state_file_present = $statePresent })
        $target = Get-Phase1DevCycleResolvedWslPath -Distro $Distro -Path $script:Phase1StagingWslPath
        if ($target.result.exit_code -ne 0 -or $target.resolved -cne $script:Phase1StagingWslPath) { throw "Staging target guard failed: resolved '$($target.resolved)', expected '$($script:Phase1StagingWslPath)'." }
        $rsync = Invoke-Phase1DevCycleWsl -Distro $Distro -Command $plan.rsync_arguments
        [void](Write-Phase1DevCycleCommandLog -LogPath $buildLogPath -Label 'module_rsynced' -Result $rsync)
        if ($rsync.exit_code -ne 0) { throw 'Canonical module rsync failed.' }
        [void](Write-Phase1DevCycleEvent -ReceiptPath $receiptPath -Event 'module_rsynced' -Status PASS -Details @{ source = $script:Phase1CanonicalModuleWslPath; target = $script:Phase1StagingWslPath; archive = $true; delete = $true; preserves_file_times = $false; excludes = @($script:Phase1RsyncExcludes) })
        $buildUnit = Invoke-Phase1DevCycleWsl -Distro $Distro -Command $plan.build_sequence[0]
        [void](Write-Phase1DevCycleCommandLog -LogPath $buildLogPath -Label 'unit_tests_build' -Result $buildUnit)
        if ($buildUnit.exit_code -ne 0) { throw 'unit_tests build failed.' }
        [void](Write-Phase1DevCycleEvent -ReceiptPath $receiptPath -Event 'unit_tests_built' -Status PASS -Details @{ target = 'unit_tests' })
        $test = Invoke-Phase1DevCycleWsl -Distro $Distro -Command $plan.build_sequence[1]
        [void](Write-Phase1DevCycleCommandLog -LogPath $buildLogPath -Label 'focused_gtest' -Result $test)
        if ($test.exit_code -ne 0) { throw 'Focused gtest filter failed.' }
        [void](Write-Phase1DevCycleEvent -ReceiptPath $receiptPath -Event 'focused_gtest_passed' -Status PASS -Details @{ filter = $GTestFilter })
        $buildWorld = Invoke-Phase1DevCycleWsl -Distro $Distro -Command $plan.build_sequence[2]
        [void](Write-Phase1DevCycleCommandLog -LogPath $buildLogPath -Label 'worldserver_build' -Result $buildWorld)
        if ($buildWorld.exit_code -ne 0) { throw 'worldserver build failed.' }
        [void](Write-Phase1DevCycleEvent -ReceiptPath $receiptPath -Event 'worldserver_built' -Status PASS -Details @{ target = 'worldserver' })
        $worldHash = Get-Phase1DevCycleSha256 -Distro $Distro -Path $script:Phase1WorldserverWslPath
        $summary.artifact = [pscustomobject][ordered]@{ path = $script:Phase1WorldserverWslPath; sha256 = $worldHash }
        [void](Write-Phase1DevCycleEvent -ReceiptPath $receiptPath -Event 'worldserver_hashed' -Status PASS -Details @{ path = $script:Phase1WorldserverWslPath; sha256 = $worldHash })
        if (Test-Path -LiteralPath $playerbotsLogPath -PathType Leaf) {
            Copy-Item -LiteralPath $playerbotsLogPath -Destination $archivePath -Force
            $summary.archive_path = $archivePath
            [void](Write-Phase1DevCycleEvent -ReceiptPath $receiptPath -Event 'playerbots_log_archived' -Status PASS -Details @{ archive_path = $archivePath; sha256 = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant() })
        } else {
            [void](Write-Phase1DevCycleEvent -ReceiptPath $receiptPath -Event 'playerbots_log_archived' -Status WARN -Details @{ archive_path = $null; present = $false })
        }
        if ($runtimeWasRunning -or $statePresent) {
            [void](Write-Phase1DevCycleEvent -ReceiptPath $receiptPath -Event 'runtime_stop_requested' -Details @{ worldserver_running = $runtimeWasRunning; state_file_present = $statePresent })
            $stop = Invoke-Phase1DevCycleWindowsScript -ScriptPath $stopScript -Root $resolvedRoot -Distro $Distro
            if ($stop.exit_code -ne 0) { throw 'Existing Phase 1 stop script failed.' }
            $summary.runtime_stopped = $true
            [void](Write-Phase1DevCycleEvent -ReceiptPath $receiptPath -Event 'runtime_stopped' -Status PASS -Details @{})
        }
        [void](Write-Phase1DevCycleEvent -ReceiptPath $receiptPath -Event 'runtime_start_requested' -Details @{ artifact_sha256 = $worldHash })
        $start = Invoke-Phase1DevCycleWindowsScript -ScriptPath $startScript -Root $resolvedRoot -Distro $Distro
        if ($start.exit_code -ne 0) { throw 'Existing Phase 1 start script failed.' }
        $summary.runtime_started = $true
        $summary.status = 'DEPLOYED'
        [void](Write-Phase1DevCycleEvent -ReceiptPath $receiptPath -Event 'runtime_started' -Status PASS -Details @{})
    } catch {
        $summary.status = 'FAILED_CLOSED'
        $summary.failure = $_.Exception.Message
        $summary.restart_skipped = -not [bool]$summary.runtime_started
        $summary.restart_skipped_reason = if ($summary.restart_skipped) { 'No automatic restart after failure; stale server rollback is not implemented.' } else { $null }
        [void](Write-Phase1DevCycleEvent -ReceiptPath $receiptPath -Event 'deploy_failed' -Status FAIL_CLOSED -Details @{ reason = $summary.failure; runtime_stopped = $summary.runtime_stopped; restart_skipped = $summary.restart_skipped; build_log_path = $buildLogPath })
    } finally {
        $summary.completed_utc = (Get-Date).ToUniversalTime().ToString('o')
        New-Item -ItemType Directory -Path $receiptDirectory -Force | Out-Null
        [System.IO.File]::WriteAllText($summaryPath, ([pscustomobject]$summary | ConvertTo-Json -Depth 20), [System.Text.UTF8Encoding]::new($false))
    }
    return [pscustomobject]$summary
}

function Invoke-Phase1DevCycleMain {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][ValidateSet('Plan', 'Doctor', 'Deploy')][string]$SelectedAction,
        [Parameter(Mandatory)][string]$Root,
        [Parameter(Mandatory)][string]$Distro,
        [Parameter(Mandatory)][string]$GTestFilter,
        [ValidateRange(1, 4)][int]$BuildJobs = 4,
        [Parameter(Mandatory)][int]$MinimumDiskGiB,
        [Parameter(Mandatory)][int]$MinimumMemoryMiB,
        [switch]$AllowApply
    )
    if ($SelectedAction -eq 'Deploy' -and -not $AllowApply) { throw 'Deploy is dry-run by default; pass -Action Deploy -Apply explicitly.' }
    if ($SelectedAction -ne 'Deploy' -and $AllowApply) { throw '-Apply is accepted only with -Action Deploy.' }
    if ([string]::IsNullOrWhiteSpace($GTestFilter)) { throw 'FocusedGTestFilter must not be empty.' }
    switch ($SelectedAction) {
        'Plan' { return Get-Phase1DevCyclePlan -Root $Root -Distro $Distro -GTestFilter $GTestFilter -BuildJobs $BuildJobs -MinimumDiskGiB $MinimumDiskGiB -MinimumMemoryMiB $MinimumMemoryMiB }
        'Doctor' { return Get-Phase1DevCycleDoctorReport -Root $Root -Distro $Distro -MinimumDiskGiB $MinimumDiskGiB -MinimumMemoryMiB $MinimumMemoryMiB }
        'Deploy' { return Invoke-Phase1DevCycleDeploy -Root $Root -Distro $Distro -GTestFilter $GTestFilter -BuildJobs $BuildJobs -MinimumDiskGiB $MinimumDiskGiB -MinimumMemoryMiB $MinimumMemoryMiB }
    }
}

if ($MyInvocation.InvocationName -ne '.') {
    $result = Invoke-Phase1DevCycleMain -SelectedAction $Action -Root $ServerRoot -Distro $WslDistro -GTestFilter $FocusedGTestFilter -BuildJobs $BuildJobs -MinimumDiskGiB $MinimumFreeDiskGiB -MinimumMemoryMiB $MinimumAvailableMemoryMiB -AllowApply:$Apply
    $result | ConvertTo-Json -Depth 20
    if ($result.PSObject.Properties.Name -contains 'status' -and $result.status -in @('FAIL_CLOSED', 'FAILED_CLOSED')) { exit 1 }
}
