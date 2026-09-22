<##
.SYNOPSIS
    Reconciles the persistent Horde-vs-Alliance campaign support processes.
.DESCRIPTION
    This is a one-shot, restart-safe supervisor. It is dry-run by default. The existing
    Quest Director remains the only component allowed to issue quest/recovery orders; this
    script only discovers, adopts, and (with -Apply) starts the existing runner scripts.

    The supervisor never stops or removes a process, PID file, lock, or stale state record.
    A process is adoptable only when its exact script path, ServerRoot argument, and policy
    flags match this lane's contract.
#>
[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [ValidateSet('questing', 'gathering', 'dungeon', 'raid', 'pvp')]
    [string]$Phase = 'questing',
    [ValidateRange(1, 1440)][int]$DurationMinutes = 480,
    [ValidateRange(10, 300)][int]$QuestPollSeconds = 20,
    [ValidateRange(30, 900)][int]$NoProgressSeconds = 120,
    [ValidateRange(5, 300)][int]$GuardianPollSeconds = 20,
    [ValidateRange(60, 1800)][int]$ProgressionPollSeconds = 300,
    [ValidateRange(300, 3600)][int]$SanityPollSeconds = 900,
    [ValidateRange(1, 120)][int]$LeaseMinutes = 30,
    [string]$EvidencePath = '',
    [string]$StatePath = '',
    [string]$ReceiptPath = '',
    [string]$SnapshotPath = '',
    [string]$LockPath = '',
    [switch]$EnableFuturePromotions,
    [switch]$Apply,
    [switch]$LibraryOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:SupervisorSchema = 'autowow.persistent-campaign-supervisor'
$script:SupervisorSchemaVersion = 1
$script:CampaignId = 'horde-vs-alliance-1-80'
$script:FuturePhases = @('dungeon', 'raid', 'pvp')

function Get-FullPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    return [System.IO.Path]::GetFullPath($Path)
}

function Test-ExactCommandLineToken {
    param(
        [Parameter(Mandatory = $true)][string]$CommandLine,
        [Parameter(Mandatory = $true)][string]$Value
    )

    $normalized = ($CommandLine -replace '[\r\n]+', ' ').Trim()
    $pattern = '(?i)(?:^|\s|")' + [regex]::Escape($Value) + '(?=\s|"|$)'
    return [regex]::IsMatch($normalized, $pattern)
}

function Test-ExactCommandLineParameter {
    param(
        [Parameter(Mandatory = $true)][string]$CommandLine,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Value
    )

    $normalized = ($CommandLine -replace '[\r\n]+', ' ').Trim()
    $escapedValue = [regex]::Escape($Value)
    $pattern = '(?i)(?:^|\s)-' + [regex]::Escape($Name) + '(?:\s+|=)(?:"' + $escapedValue + '"|' + $escapedValue + ')(?=\s|$)'
    return [regex]::IsMatch($normalized, $pattern)
}

function Get-TravelPolicyContradictions {
    param([Parameter(Mandatory = $true)][string]$CommandLine)

    $normalized = ($CommandLine -replace '[\r\n]+', ' ').Trim()
    $patterns = [ordered]@{
        explicit_teleport_switch = '(?i)(?:^|\s)-(?:Teleport|AllowTeleport|UseTeleport)(?:\s|=|$)'
        enabled_teleport_flag = '(?i)(?:allow_teleport|teleport_allowed|enable_teleport)\s*[:=]\s*(?:true|1)'
        teleport_movement_mode = '(?i)-(?:TravelMode|MovementMode)(?:\s|=)(?:"?)(?:teleport|instant)(?:"?)(?=\s|$)'
        direct_teleport_action = '(?i)-(?:Action|BridgeAction)(?:\s|=)(?:"?)(?:teleport|travel)(?:"?)(?=\s|$)'
    }

    $contradictions = [System.Collections.Generic.List[string]]::new()
    foreach ($name in $patterns.Keys) {
        if ([regex]::IsMatch($normalized, $patterns[$name])) { $contradictions.Add([string]$name) }
    }
    return @($contradictions)
}

function Get-PolicyEvaluation {
    param(
        [Parameter(Mandatory = $true)][string]$Role,
        [Parameter(Mandatory = $true)][string]$CommandLine,
        [int]$ExpectedQuestDirectorPid = 0
    )

    $contradictions = @(Get-TravelPolicyContradictions -CommandLine $CommandLine)
    $reasons = [System.Collections.Generic.List[string]]::new()
    foreach ($item in $contradictions) { $reasons.Add([string]$item) }
    if ($Role -eq 'quest_director' -and $CommandLine -match '(?i)(?:^|\s)-ObserveOnly(?:\s|$)') {
        $reasons.Add('quest_director_observe_only')
    }
    if ($Role -eq 'guardian') {
        if ($ExpectedQuestDirectorPid -le 0) {
            $reasons.Add('guardian_quest_director_pid_unavailable')
        }
        elseif (-not (Test-ExactCommandLineParameter -CommandLine $CommandLine -Name 'QuestDirectorPid' -Value ([string]$ExpectedQuestDirectorPid))) {
            $reasons.Add('guardian_quest_director_pid_mismatch')
        }
        if ($CommandLine -notmatch '(?i)(?:^|\s)-ReleaseLiveMutations(?:\s|$)') {
            $reasons.Add('guardian_missing_release_live_mutations')
        }
    }
    [pscustomobject][ordered]@{
        allowed = ($reasons.Count -eq 0)
        contradictions = @($reasons)
        travel_mode = 'real_travel'
        teleport_allowed = $false
    }
}

function Test-ExactChildCommandLine {
    param(
        [Parameter(Mandatory = $true)][string]$Role,
        [Parameter(Mandatory = $true)][string]$CommandLine,
        [Parameter(Mandatory = $true)][string]$ScriptPath,
        [string]$ExpectedServerRoot = '',
        [int]$ExpectedQuestDirectorPid = 0
    )

    $scriptFullPath = Get-FullPath -Path $ScriptPath
    $rootFullPath = if ([string]::IsNullOrWhiteSpace($ExpectedServerRoot)) { '' } else { Get-FullPath -Path $ExpectedServerRoot }
    $policy = Get-PolicyEvaluation -Role $Role -CommandLine $CommandLine -ExpectedQuestDirectorPid $ExpectedQuestDirectorPid
    $hasFile = [regex]::IsMatch(($CommandLine -replace '[\r\n]+', ' '), '(?i)(?:^|\s)-File(?:\s|=)')
    $hasScript = Test-ExactCommandLineToken -CommandLine $CommandLine -Value $scriptFullPath
    $hasRoot = if ($Role -eq 'guardian') { $true } else { Test-ExactCommandLineParameter -CommandLine $CommandLine -Name 'ServerRoot' -Value $rootFullPath }
    $hasQuestDirectorPid = if ($Role -eq 'guardian') { Test-ExactCommandLineParameter -CommandLine $CommandLine -Name 'QuestDirectorPid' -Value ([string]$ExpectedQuestDirectorPid) } else { $true }
    [pscustomobject][ordered]@{
        matches = ($hasFile -and $hasScript -and $hasRoot -and $hasQuestDirectorPid -and [bool]$policy.allowed)
        has_file_switch = $hasFile
        has_exact_script_path = $hasScript
        has_exact_server_root = $hasRoot
        has_exact_quest_director_pid = $hasQuestDirectorPid
        policy = $policy
    }
}

function New-RoleSpecifications {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$ScriptsRoot
    )

    $rootFullPath = Get-FullPath -Path $Root
    $scriptsFullPath = Get-FullPath -Path $ScriptsRoot
    $pidRoot = $rootFullPath
    [ordered]@{
        quest_director = [pscustomobject][ordered]@{
            role = 'quest_director'
            display_name = 'Quest Director'
            script_path = Join-Path $scriptsFullPath 'league-simulation-director.ps1'
            launcher_path = Join-Path $scriptsFullPath 'start-league-simulation-director.ps1'
            legacy_pid_paths = @(
                (Join-Path $pidRoot 'league-v0-director.pid.json'),
                (Join-Path $rootFullPath 'work\autowow-ops\quest-director.json')
            )
            receipt_prefix = 'persistent-campaign-quest-director'
        }
        guardian = [pscustomobject][ordered]@{
            role = 'guardian'
            display_name = 'Guardian'
            script_path = Join-Path $scriptsFullPath 'autowow-overnight-guardian.ps1'
            launcher_path = Join-Path $scriptsFullPath 'autowow-overnight-guardian.ps1'
            legacy_pid_paths = @()
            receipt_prefix = 'persistent-campaign-guardian'
        }
        progression_observer = [pscustomobject][ordered]@{
            role = 'progression_observer'
            display_name = 'Progression observer'
            script_path = Join-Path $scriptsFullPath 'league-progression-observer.ps1'
            launcher_path = Join-Path $scriptsFullPath 'start-league-progression-observer.ps1'
            legacy_pid_paths = @(Join-Path $pidRoot 'league-v0-progression-observer.pid.json')
            receipt_prefix = 'persistent-campaign-progression'
        }
        sanity_observer = [pscustomobject][ordered]@{
            role = 'sanity_observer'
            display_name = 'Sanity observer'
            script_path = Join-Path $scriptsFullPath 'league-sanity-observer.ps1'
            launcher_path = Join-Path $scriptsFullPath 'start-league-sanity-observer.ps1'
            legacy_pid_paths = @(Join-Path $pidRoot 'league-v0-sanity-observer.pid.json')
            receipt_prefix = 'persistent-campaign-sanity'
        }
    }
}

function Get-CommandLineParameterValue {
    param(
        [Parameter(Mandatory = $true)][string]$CommandLine,
        [Parameter(Mandatory = $true)][string]$Name
    )

    $normalized = ($CommandLine -replace '[\r\n]+', ' ').Trim()
    $pattern = '(?i)(?:^|\s)-' + [regex]::Escape($Name) + '(?:\s+|=)(?:"([^"]*)"|([^\s]+))'
    $match = [regex]::Match($normalized, $pattern)
    if (-not $match.Success) { return $null }
    if ($match.Groups[1].Success) { return $match.Groups[1].Value }
    return $match.Groups[2].Value
}

function Get-ProcessRows {
    try {
        return @(Get-CimInstance -ClassName Win32_Process -ErrorAction Stop | Where-Object { $_.ProcessId -and $_.CommandLine })
    }
    catch {
        throw "Cannot inspect Windows process command lines; refusing to start campaign children: $($_.Exception.Message)"
    }
}

function Read-JsonObject {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    try {
        $raw = Get-Content -LiteralPath $Path -Raw -ErrorAction Stop
        if ([string]::IsNullOrWhiteSpace($raw)) { throw 'file is empty' }
        return (ConvertFrom-Json -InputObject $raw -ErrorAction Stop)
    }
    catch {
        throw "Invalid JSON at $Path; refusing destructive recovery or overwrite: $($_.Exception.Message)"
    }
}

function Get-IntegerProperty {
    param([AllowNull()][object]$Object, [string[]]$Names)
    if ($null -eq $Object) { return $null }
    foreach ($name in $Names) {
        $property = $Object.PSObject.Properties[$name]
        if ($null -ne $property -and $null -ne $property.Value) {
            try { return [int]$property.Value } catch { return $null }
        }
    }
    return $null
}

function Get-StringProperty {
    param([AllowNull()][object]$Object, [string[]]$Names)
    if ($null -eq $Object) { return $null }
    foreach ($name in $Names) {
        $property = $Object.PSObject.Properties[$name]
        if ($null -ne $property -and $null -ne $property.Value -and -not [string]::IsNullOrWhiteSpace([string]$property.Value)) {
            return [string]$property.Value
        }
    }
    return $null
}

function Get-PriorRoleState {
    param([AllowNull()][object]$State, [Parameter(Mandatory = $true)][string]$Role)
    if ($null -eq $State -or $null -eq $State.children) { return $null }
    $property = $State.children.PSObject.Properties[$Role]
    if ($null -eq $property) { return $null }
    return $property.Value
}

function Get-LegacyCandidatePids {
    param([Parameter(Mandatory = $true)][object]$Spec)
    $pids = [System.Collections.Generic.List[int]]::new()
    foreach ($path in @($Spec.legacy_pid_paths)) {
        $metadata = Read-JsonObject -Path $path
        $candidatePid = Get-IntegerProperty -Object $metadata -Names @('process_id', 'pid')
        if ($null -ne $candidatePid -and $candidatePid -gt 0 -and $candidatePid -notin $pids) { $pids.Add($candidatePid) }
    }
    return @($pids)
}

function Get-RoleAssessment {
    param(
        [Parameter(Mandatory = $true)][object]$Spec,
        [Parameter(Mandatory = $true)][string]$ServerRoot,
        [Parameter(Mandatory = $true)][object[]]$ProcessRows,
        [AllowNull()][object]$PriorState,
        [Parameter(Mandatory = $true)][string]$PlannedReceiptPath,
        [Parameter(Mandatory = $true)][string]$PlannedStdoutPath,
        [Parameter(Mandatory = $true)][string]$PlannedStderrPath,
        [int]$ExpectedQuestDirectorPid = 0
    )

    $exact = @($ProcessRows | Where-Object {
        $normalized = ([string]$_.CommandLine -replace '[\r\n]+', ' ').Trim()
        [regex]::IsMatch($normalized, '(?i)(?:^|\s)-File(?:\s|=)') -and
        (Test-ExactCommandLineToken -CommandLine $normalized -Value (Get-FullPath -Path $Spec.script_path)) -and
        ($Spec.role -eq 'guardian' -or (Test-ExactCommandLineParameter -CommandLine $normalized -Name 'ServerRoot' -Value (Get-FullPath -Path $ServerRoot)))
    })
    $valid = @()
    $contradictory = @()
    foreach ($row in $exact) {
        $evaluation = Test-ExactChildCommandLine -Role $Spec.role -CommandLine ([string]$row.CommandLine) -ScriptPath $Spec.script_path -ExpectedServerRoot $ServerRoot -ExpectedQuestDirectorPid $ExpectedQuestDirectorPid
        if ($evaluation.matches) { $valid += [pscustomobject][ordered]@{ row = $row; evaluation = $evaluation } }
        else { $contradictory += [pscustomobject][ordered]@{ row = $row; evaluation = $evaluation } }
    }

    $priorPid = Get-IntegerProperty -Object $PriorState -Names @('pid', 'process_id')
    $priorReceipt = Get-StringProperty -Object $PriorState -Names @('receipt_path')
    $priorStdout = Get-StringProperty -Object $PriorState -Names @('stdout_path', 'stdout')
    $priorStderr = Get-StringProperty -Object $PriorState -Names @('stderr_path', 'stderr')
    $legacyPidHints = @(Get-LegacyCandidatePids -Spec $Spec)
    $base = [ordered]@{
        role = $Spec.role
        display_name = $Spec.display_name
        desired_count = 1
        pid = $null
        process_id = $null
        status = 'missing'
        action = 'start'
        adopted = $false
        stale_prior_pid = $priorPid
        legacy_pid_hints = $legacyPidHints
        command_line = $null
        script_path = (Get-FullPath -Path $Spec.script_path)
        launcher_path = (Get-FullPath -Path $Spec.launcher_path)
        receipt_path = if ($priorReceipt) { $priorReceipt } else { $PlannedReceiptPath }
        stdout_path = if ($priorStdout) { $priorStdout } else { $PlannedStdoutPath }
        stderr_path = if ($priorStderr) { $priorStderr } else { $PlannedStderrPath }
        contradictions = @()
    }

    if ($contradictory.Count -gt 0) {
        $base.status = 'blocked_contradictory_child'
        $base.action = 'fail_closed'
        $base.contradictions = @($contradictory | ForEach-Object { $_.evaluation.policy.contradictions })
        return [pscustomobject]$base
    }
    if ($valid.Count -gt 1) {
        $base.status = 'blocked_duplicate_valid_children'
        $base.action = 'fail_closed'
        $base.pid = @($valid | ForEach-Object { [int]$_.row.ProcessId })
        $base.process_id = $base.pid
        $base.command_line = @($valid | ForEach-Object { [string]$_.row.CommandLine })
        return [pscustomobject]$base
    }
    if ($valid.Count -eq 1) {
        $row = $valid[0].row
        $base.pid = [int]$row.ProcessId
        $base.process_id = [int]$row.ProcessId
        $base.status = 'running'
        $base.action = 'adopt'
        $base.adopted = $true
        $base.command_line = [string]$row.CommandLine
        $childReceipt = Get-CommandLineParameterValue -CommandLine ([string]$row.CommandLine) -Name 'ReceiptPath'
        if ($childReceipt) { $base.receipt_path = $childReceipt }
        return [pscustomobject]$base
    }
    if ($priorPid) { $base.status = 'missing_stale_prior_pid' }
    return [pscustomobject]$base
}

function Get-PlannedRoleAssessment {
    param(
        [Parameter(Mandatory = $true)][object]$Spec,
        [Parameter(Mandatory = $true)][string]$ServerRoot,
        [Parameter(Mandatory = $true)][object[]]$ProcessRows,
        [AllowNull()][object]$PriorState,
        [Parameter(Mandatory = $true)][string]$Stamp,
        [int]$ExpectedQuestDirectorPid = 0
    )

    $paths = New-ChildPaths -Root $ServerRoot -Prefix $Spec.receipt_prefix -Stamp $Stamp
    return Get-RoleAssessment -Spec $Spec -ServerRoot $ServerRoot -ProcessRows $ProcessRows -PriorState $PriorState -PlannedReceiptPath $paths.receipt_path -PlannedStdoutPath $paths.stdout_path -PlannedStderrPath $paths.stderr_path -ExpectedQuestDirectorPid $ExpectedQuestDirectorPid
}

function New-ChildPaths {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Prefix,
        [Parameter(Mandatory = $true)][string]$Stamp
    )
    $receiptRoot = Join-Path $Root 'leagues\results'
    $logRoot = Join-Path $Root 'logs'
    [pscustomobject][ordered]@{
        receipt_path = Join-Path $receiptRoot ("{0}-{1}.jsonl" -f $Prefix, $Stamp)
        stdout_path = Join-Path $logRoot ("{0}-{1}-stdout.log" -f $Prefix, $Stamp)
        stderr_path = Join-Path $logRoot ("{0}-{1}-stderr.log" -f $Prefix, $Stamp)
    }
}

function New-ChildArguments {
    param(
        [Parameter(Mandatory = $true)][string]$Role,
        [Parameter(Mandatory = $true)][string]$ServerRoot,
        [Parameter(Mandatory = $true)][string]$ReceiptPath,
        [Parameter(Mandatory = $true)][int]$DurationMinutes,
        [Parameter(Mandatory = $true)][int]$QuestPollSeconds,
        [Parameter(Mandatory = $true)][int]$NoProgressSeconds,
        [Parameter(Mandatory = $true)][int]$GuardianPollSeconds,
        [Parameter(Mandatory = $true)][int]$ProgressionPollSeconds,
        [Parameter(Mandatory = $true)][int]$SanityPollSeconds,
        [Parameter(Mandatory = $true)][int]$QuestDirectorPid,
        [Parameter(Mandatory = $true)][bool]$Apply
    )

    switch ($Role) {
        'quest_director' {
            return @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Get-FullPath (Join-Path $PSScriptRoot 'league-simulation-director.ps1')), '-ServerRoot', (Get-FullPath $ServerRoot), '-DurationMinutes', $DurationMinutes, '-ScoutEverySeconds', $QuestPollSeconds, '-NoProgressSeconds', $NoProgressSeconds, '-MaxRecoveriesPerQuest', 2, '-PlayerbotsLogPath', (Get-FullPath (Join-Path $ServerRoot 'logs\phase1-runtime\Playerbots.log')), '-ReceiptPath', (Get-FullPath $ReceiptPath))
        }
        'guardian' {
            $durationHours = [Math]::Max(1, [Math]::Min(24, [int][Math]::Ceiling(([double]$DurationMinutes) / 60.0)))
            $arguments = @('-NoProfile', '-File', (Get-FullPath (Join-Path $PSScriptRoot 'autowow-overnight-guardian.ps1')), '-DurationHours', $durationHours, '-PollSeconds', $GuardianPollSeconds, '-QuestDirectorPid', $QuestDirectorPid, '-ReceiptPath', (Get-FullPath $ReceiptPath))
            if ($Apply) { $arguments += '-ReleaseLiveMutations' }
            return $arguments
        }
        'progression_observer' {
            return @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Get-FullPath (Join-Path $PSScriptRoot 'league-progression-observer.ps1')), '-ServerRoot', (Get-FullPath $ServerRoot), '-DurationMinutes', $DurationMinutes, '-PollSeconds', $ProgressionPollSeconds, '-ReceiptPath', (Get-FullPath $ReceiptPath))
        }
        'sanity_observer' {
            return @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Get-FullPath (Join-Path $PSScriptRoot 'league-sanity-observer.ps1')), '-ServerRoot', (Get-FullPath $ServerRoot), '-DurationMinutes', $DurationMinutes, '-PollSeconds', $SanityPollSeconds, '-ReceiptPath', (Get-FullPath $ReceiptPath))
        }
        default { throw "Unknown campaign child role: $Role" }
    }
}

function Start-ManagedChild {
    param(
        [Parameter(Mandatory = $true)][object]$Assessment,
        [Parameter(Mandatory = $true)][string]$HostPath,
        [Parameter(Mandatory = $true)][string]$ServerRoot,
        [Parameter(Mandatory = $true)][object[]]$Arguments
    )

    $started = Start-Process -FilePath $HostPath -ArgumentList $Arguments -WorkingDirectory $ServerRoot -WindowStyle Hidden -RedirectStandardOutput $Assessment.stdout_path -RedirectStandardError $Assessment.stderr_path -PassThru
    $Assessment.pid = [int]$started.Id
    $Assessment.process_id = [int]$started.Id
    $Assessment.status = 'started'
    $Assessment.action = 'start'
    $Assessment.adopted = $false
    $Assessment.command_line = $HostPath + ' ' + (($Arguments | ForEach-Object {
                $value = [string]$_
                if ($value -match '[\s"]') { '"' + $value.Replace('"', '\"') + '"' } else { $value }
            }) -join ' ')
    return $started
}

function Get-PhaseGate {
    param(
        [Parameter(Mandatory = $true)][ValidateSet('questing', 'gathering', 'dungeon', 'raid', 'pvp')][string]$RequestedPhase,
        [string]$EvidencePath = '',
        [switch]$EnableFuturePromotions
    )

    if ($RequestedPhase -notin $script:FuturePhases) {
        return [pscustomobject][ordered]@{
            requested_phase = $RequestedPhase
            status = 'open'
            current_phase_supported = $true
            future_promotion = $false
            mutating_promotion_enabled = $false
            reason = 'questing_and_gathering_are_current_metadata_gates; no promotion order is issued'
            evidence_path = $null
        }
    }
    if (-not $EnableFuturePromotions) {
        return [pscustomobject][ordered]@{
            requested_phase = $RequestedPhase
            status = 'locked'
            current_phase_supported = $false
            future_promotion = $true
            mutating_promotion_enabled = $false
            reason = 'future promotion requires -EnableFuturePromotions and a passing evidence file'
            evidence_path = if ($EvidencePath) { Get-FullPath $EvidencePath } else { $null }
        }
    }
    if ([string]::IsNullOrWhiteSpace($EvidencePath)) {
        return [pscustomobject][ordered]@{
            requested_phase = $RequestedPhase
            status = 'blocked'
            current_phase_supported = $false
            future_promotion = $true
            mutating_promotion_enabled = $false
            reason = 'future promotion was explicitly enabled but EvidencePath was not supplied'
            evidence_path = $null
        }
    }
    $fullEvidencePath = Get-FullPath $EvidencePath
    $evidence = Read-JsonObject -Path $fullEvidencePath
    $failures = [System.Collections.Generic.List[string]]::new()
    if ($null -eq $evidence) { $failures.Add('evidence_file_missing') }
    else {
        if ([string]$evidence.schema -ne 'autowow.campaign.evidence.v1') { $failures.Add('evidence_schema_mismatch') }
        if ([string]$evidence.phase -ne $RequestedPhase) { $failures.Add('evidence_phase_mismatch') }
        if ([string]$evidence.verdict -ne 'PASS') { $failures.Add('evidence_verdict_not_PASS') }
        foreach ($field in @('no_teleport', 'no_database_writes', 'no_server_restart')) {
            $property = $evidence.PSObject.Properties[$field]
            if ($null -eq $property -or -not [bool]$property.Value) { $failures.Add("evidence_gate_$field") }
        }
    }
    [pscustomobject][ordered]@{
        requested_phase = $RequestedPhase
        status = if ($failures.Count -eq 0) { 'eligible_metadata_only' } else { 'blocked' }
        current_phase_supported = $false
        future_promotion = $true
        mutating_promotion_enabled = ($failures.Count -eq 0)
        reason = if ($failures.Count -eq 0) { 'parameters and evidence gates pass; no future gameplay mutator is implemented in this lane' } else { 'evidence gate failed closed' }
        evidence_path = $fullEvidencePath
        failures = @($failures)
    }
}

function New-TravelPolicyMetadata {
    [ordered]@{
        movement_policy = 'real_travel'
        teleport_allowed = $false
        teleport_fallback = 'disabled_for_league_parties'
        supervisor_issues_quest_orders = $false
        supervisor_issues_party_orders = $false
        supervisor_issues_gather_orders = $false
        supervisor_issues_teleport_orders = $false
        authoritative_quest_director_role = 'quest_director'
        contradiction_action = 'fail_closed'
    }
}

function Acquire-SupervisorLock {
    param([Parameter(Mandatory = $true)][string]$Path)
    $parent = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $parent -PathType Container)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    try {
        $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::OpenOrCreate, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
    }
    catch {
        throw "Another persistent campaign supervisor instance holds the non-destructive lock: $Path"
    }
    $owner = [ordered]@{ schema = 'autowow.persistent-campaign-supervisor.lock.v1'; pid = $PID; acquired_utc = [datetime]::UtcNow.ToString('o') }
    $bytes = [System.Text.UTF8Encoding]::new($false).GetBytes(($owner | ConvertTo-Json -Compress))
    $stream.SetLength(0)
    $stream.Write($bytes, 0, $bytes.Length)
    $stream.Flush()
    return $stream
}

function Write-JsonLine {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Event,
        [hashtable]$Fields = @{}
    )
    $record = [ordered]@{ schema = $script:SupervisorSchema; schema_version = $script:SupervisorSchemaVersion; timestamp_utc = [datetime]::UtcNow.ToString('o'); event = $Event }
    foreach ($key in $Fields.Keys) { $record[$key] = $Fields[$key] }
    [System.IO.File]::AppendAllText($Path, (($record | ConvertTo-Json -Compress -Depth 12) + [Environment]::NewLine), [System.Text.UTF8Encoding]::new($false))
}

function Write-StatusSnapshot {
    param([Parameter(Mandatory = $true)][string]$Path, [Parameter(Mandatory = $true)][object]$Snapshot)
    [System.IO.File]::WriteAllText($Path, ($Snapshot | ConvertTo-Json -Depth 12), [System.Text.UTF8Encoding]::new($false))
}

if ($LibraryOnly) { return }

$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot -ErrorAction Stop).Path
$stateRoot = Join-Path $ServerRoot 'work\persistent-campaign-supervisor'
if ([string]::IsNullOrWhiteSpace($StatePath)) { $StatePath = Join-Path $stateRoot 'state.json' }
if ([string]::IsNullOrWhiteSpace($ReceiptPath)) { $ReceiptPath = Join-Path $ServerRoot 'leagues\results\persistent-campaign-supervisor.jsonl' }
if ([string]::IsNullOrWhiteSpace($SnapshotPath)) { $SnapshotPath = Join-Path $ServerRoot 'logs\persistent-campaign-supervisor-status.json' }
if ([string]::IsNullOrWhiteSpace($LockPath)) { $LockPath = Join-Path $stateRoot 'supervisor.lock' }
$StatePath = Get-FullPath $StatePath
$ReceiptPath = Get-FullPath $ReceiptPath
$SnapshotPath = Get-FullPath $SnapshotPath
$LockPath = Get-FullPath $LockPath

$phaseGate = Get-PhaseGate -RequestedPhase $Phase -EvidencePath $EvidencePath -EnableFuturePromotions:$EnableFuturePromotions
if ($Apply -and [string]$phaseGate.status -in @('locked', 'blocked')) {
    throw "Phase gate is $($phaseGate.status): $($phaseGate.reason)"
}
$priorState = Read-JsonObject -Path $StatePath
if ($null -ne $priorState) {
    if ([string]$priorState.schema -ne $script:SupervisorSchema -or [int]$priorState.schema_version -ne $script:SupervisorSchemaVersion) {
        throw "Existing supervisor state has an unsupported schema/version at $StatePath; refusing overwrite."
    }
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$specs = New-RoleSpecifications -Root $ServerRoot -ScriptsRoot $PSScriptRoot
$processRows = @(Get-ProcessRows)
$assessments = [ordered]@{}
$assessments['quest_director'] = Get-PlannedRoleAssessment -Spec $specs['quest_director'] -ServerRoot $ServerRoot -ProcessRows $processRows -PriorState (Get-PriorRoleState -State $priorState -Role 'quest_director') -Stamp $stamp
$expectedQuestDirectorPid = if ($assessments['quest_director'].status -eq 'running') { [int]$assessments['quest_director'].pid } else { 0 }
foreach ($role in @($specs.Keys | Where-Object { $_ -ne 'quest_director' })) {
    $assessments[$role] = Get-PlannedRoleAssessment -Spec $specs[$role] -ServerRoot $ServerRoot -ProcessRows $processRows -PriorState (Get-PriorRoleState -State $priorState -Role $role) -Stamp $stamp -ExpectedQuestDirectorPid $expectedQuestDirectorPid
}

$blocked = @($assessments.Values | Where-Object { $_.status -like 'blocked_*' })
$baseSnapshot = [ordered]@{
    schema = $script:SupervisorSchema
    schema_version = $script:SupervisorSchemaVersion
    campaign = $script:CampaignId
    generated_at_utc = [datetime]::UtcNow.ToString('o')
    dry_run = (-not $Apply)
    apply = [bool]$Apply
    server_root = $ServerRoot
    current_phase = if ($priorState -and $priorState.current_phase) { [string]$priorState.current_phase } else { $Phase }
    requested_phase = $Phase
    phase_gate = $phaseGate
    policy = New-TravelPolicyMetadata
    state_path = $StatePath
    receipt_path = $ReceiptPath
    snapshot_path = $SnapshotPath
    lock_path = $LockPath
    children = $assessments
    safety = [ordered]@{
        bridge_orders_issued_by_supervisor = 0
        database_writes_by_supervisor = 0
        server_restarts_by_supervisor = 0
        process_starts = 0
        process_stops = 0
        destructive_cleanup_actions = 0
    }
}

if (-not $Apply) {
    $baseSnapshot.plan = @($assessments.Values | ForEach-Object { [ordered]@{ role = $_.role; action = $_.action; status = $_.status; pid = $_.pid; reason = if ($_.status -eq 'missing_stale_prior_pid') { 'stale PID is recorded but will not be killed or removed' } else { $null } } })
    $baseSnapshot | ConvertTo-Json -Depth 12
    return
}

if ($blocked.Count -gt 0) { throw ('Refusing to start children because a role is blocked: ' + (($blocked | ForEach-Object { $_.role + ':' + $_.status }) -join ', ')) }

$lockStream = $null
try {
    $lockStream = Acquire-SupervisorLock -Path $LockPath
    # Re-read process truth after taking the lock, so two racing supervisors cannot both start a role.
    $processRows = @(Get-ProcessRows)
    $assessments = [ordered]@{}
    $assessments['quest_director'] = Get-PlannedRoleAssessment -Spec $specs['quest_director'] -ServerRoot $ServerRoot -ProcessRows $processRows -PriorState (Get-PriorRoleState -State $priorState -Role 'quest_director') -Stamp $stamp
    $expectedQuestDirectorPid = if ($assessments['quest_director'].status -eq 'running') { [int]$assessments['quest_director'].pid } else { 0 }
    foreach ($role in @($specs.Keys | Where-Object { $_ -ne 'quest_director' })) {
        $assessments[$role] = Get-PlannedRoleAssessment -Spec $specs[$role] -ServerRoot $ServerRoot -ProcessRows $processRows -PriorState (Get-PriorRoleState -State $priorState -Role $role) -Stamp $stamp -ExpectedQuestDirectorPid $expectedQuestDirectorPid
    }
    $blocked = @($assessments.Values | Where-Object { $_.status -like 'blocked_*' })
    if ($blocked.Count -gt 0) { throw ('Refusing to start children because a role is blocked: ' + (($blocked | ForEach-Object { $_.role + ':' + $_.status }) -join ', ')) }

    New-Item -ItemType Directory -Path (Split-Path -Parent $StatePath), (Split-Path -Parent $ReceiptPath), (Split-Path -Parent $SnapshotPath) -Force | Out-Null
    Write-JsonLine -Path $ReceiptPath -Event 'supervisor_reconcile_started' -Fields @{ campaign = $script:CampaignId; phase = $Phase; lock_path = $LockPath }
    $hostPath = (Get-Command pwsh.exe -ErrorAction SilentlyContinue).Source
    if ([string]::IsNullOrWhiteSpace($hostPath)) { $hostPath = (Get-Command powershell.exe -ErrorAction Stop).Source }
    $now = [datetime]::UtcNow
    $leaseExpires = $now.AddMinutes($LeaseMinutes)
    $childState = [ordered]@{}
    $childPids = [ordered]@{}
    $childReceipts = [ordered]@{}
    $childLeases = [ordered]@{}

    # The guardian must be tied to the actual director PID. If the director is missing,
    # start it first, then reassess the guardian before any other child is started.
    if ($assessments['quest_director'].status -notin @('running', 'started')) {
        $director = $assessments['quest_director']
        $directorArguments = New-ChildArguments -Role 'quest_director' -ServerRoot $ServerRoot -ReceiptPath $director.receipt_path -DurationMinutes $DurationMinutes -QuestPollSeconds $QuestPollSeconds -NoProgressSeconds $NoProgressSeconds -GuardianPollSeconds $GuardianPollSeconds -ProgressionPollSeconds $ProgressionPollSeconds -SanityPollSeconds $SanityPollSeconds -QuestDirectorPid 0 -Apply $true
        $startedDirector = Start-ManagedChild -Assessment $director -HostPath $hostPath -ServerRoot $ServerRoot -Arguments $directorArguments
        Write-JsonLine -Path $ReceiptPath -Event 'child_start_requested' -Fields @{ role = 'quest_director'; pid = [int]$startedDirector.Id; script_path = $director.script_path; receipt_path = $director.receipt_path }
        $baseSnapshot.safety.process_starts++
    }
    $expectedQuestDirectorPid = [int]$assessments['quest_director'].pid
    if ($expectedQuestDirectorPid -le 0) { throw 'Quest Director did not provide a valid PID for guardian association.' }
    $processRows = @(Get-ProcessRows)
    foreach ($role in @($specs.Keys | Where-Object { $_ -ne 'quest_director' })) {
        $assessments[$role] = Get-PlannedRoleAssessment -Spec $specs[$role] -ServerRoot $ServerRoot -ProcessRows $processRows -PriorState (Get-PriorRoleState -State $priorState -Role $role) -Stamp $stamp -ExpectedQuestDirectorPid $expectedQuestDirectorPid
    }
    $blocked = @($assessments.Values | Where-Object { $_.status -like 'blocked_*' })
    if ($blocked.Count -gt 0) { throw ('Refusing to start children because a role is blocked: ' + (($blocked | ForEach-Object { $_.role + ':' + $_.status }) -join ', ')) }

    foreach ($role in $specs.Keys) {
        $assessment = $assessments[$role]
        if ($assessment.status -in @('running', 'started')) {
            if ($assessment.status -eq 'running') {
                Write-JsonLine -Path $ReceiptPath -Event 'child_adopted' -Fields @{ role = $role; pid = $assessment.pid; command_line = $assessment.command_line; receipt_path = $assessment.receipt_path }
            }
        }
        else {
            $arguments = New-ChildArguments -Role $role -ServerRoot $ServerRoot -ReceiptPath $assessment.receipt_path -DurationMinutes $DurationMinutes -QuestPollSeconds $QuestPollSeconds -NoProgressSeconds $NoProgressSeconds -GuardianPollSeconds $GuardianPollSeconds -ProgressionPollSeconds $ProgressionPollSeconds -SanityPollSeconds $SanityPollSeconds -QuestDirectorPid $expectedQuestDirectorPid -Apply $true
            $started = Start-ManagedChild -Assessment $assessment -HostPath $hostPath -ServerRoot $ServerRoot -Arguments $arguments
            Write-JsonLine -Path $ReceiptPath -Event 'child_start_requested' -Fields @{ role = $role; pid = [int]$started.Id; script_path = $assessment.script_path; receipt_path = $assessment.receipt_path }
            $baseSnapshot.safety.process_starts++
        }
        $childPids[$role] = $assessment.pid
        $childReceipts[$role] = $assessment.receipt_path
        $childLeases[$role] = [ordered]@{ lease_started_utc = $now.ToString('o'); lease_renewed_utc = $now.ToString('o'); lease_expires_utc = $leaseExpires.ToString('o') }
        $childState[$role] = [ordered]@{
            role = $role
            pid = $assessment.pid
            process_id = $assessment.pid
            status = $assessment.status
            adopted = [bool]$assessment.adopted
            script_path = $assessment.script_path
            launcher_path = $assessment.launcher_path
            command_line = $assessment.command_line
            receipt_path = $assessment.receipt_path
            stdout_path = $assessment.stdout_path
            stderr_path = $assessment.stderr_path
            lease = $childLeases[$role]
        }
        if ($role -eq 'guardian') { $childState[$role].quest_director_pid = $expectedQuestDirectorPid }
    }

    $state = [ordered]@{
        schema = $script:SupervisorSchema
        schema_version = $script:SupervisorSchemaVersion
        campaign = $script:CampaignId
        updated_at_utc = [datetime]::UtcNow.ToString('o')
        current_phase = if ($phaseGate.future_promotion -and $phaseGate.mutating_promotion_enabled) { $Phase } else { $Phase }
        phase_gate = $phaseGate
        policy = New-TravelPolicyMetadata
        child_process_pids = $childPids
        receipt_paths = $childReceipts
        lease_timestamps = $childLeases
        lease = [ordered]@{ acquired_utc = $now.ToString('o'); renewed_utc = $now.ToString('o'); expires_utc = $leaseExpires.ToString('o') }
        children = $childState
        supervisor_receipt_path = $ReceiptPath
        status_snapshot_path = $SnapshotPath
        state_path = $StatePath
    }
    [System.IO.File]::WriteAllText($StatePath, ($state | ConvertTo-Json -Depth 12), [System.Text.UTF8Encoding]::new($false))
    $baseSnapshot.children = $childState
    $baseSnapshot.current_phase = $state.current_phase
    $baseSnapshot.safety.child_process_pids = $childPids
    $baseSnapshot.safety.receipt_paths = $childReceipts
    Write-JsonLine -Path $ReceiptPath -Event 'supervisor_reconcile_completed' -Fields @{ phase = $state.current_phase; child_process_pids = $childPids; process_starts = $baseSnapshot.safety.process_starts }
    Write-StatusSnapshot -Path $SnapshotPath -Snapshot $baseSnapshot
    $baseSnapshot
}
finally {
    if ($null -ne $lockStream) { $lockStream.Dispose() }
}
