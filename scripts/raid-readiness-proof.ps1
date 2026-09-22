<#
.SYNOPSIS
    Read-only raid-readiness proof harness for exact 10-, 25-, or 40-bot rosters.

.DESCRIPTION
    Dry-runs by default. Live mode observes an already staged encounter through only the existing
    AutoWoW loopback bridge list and combatlog actions. It never forms or moves the raid, starts
    combat, teleports, initializes fixtures, changes characters or quests, creates or kills units,
    grants rewards, or restarts the server.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet(10, 25, 40)][int]$RaidSize,
    [Parameter(Mandatory)][uint32[]]$RosterGuid,
    [Parameter(Mandatory)][uint32[]]$TankGuid,
    [Parameter(Mandatory)][uint32[]]$HealerGuid,
    [uint32]$LeaderGuid = 0,
    [Parameter(Mandatory)][uint32]$TargetGuid,
    [string]$TargetName = 'raid-target',
    [Parameter(Mandatory)][uint32]$ExpectedMapId,
    [Parameter(Mandatory)][uint32]$ExpectedInstanceId,
    [uint32]$ExpectedEncounterId = 0,
    [uint32]$ExpectedEncounterCreditEntry = 0,
    [ValidateRange(-1, 31)][int]$ExpectedEncounterBit = -1,
    [ValidateRange(-1, 1)][int]$ExpectedEncounterDifficulty = -1,
    [ValidateRange(30, 3600)][int]$DurationSeconds = 300,
    [ValidateRange(2, 30)][int]$PollSeconds = 5,
    [ValidateRange(0, 600)][int]$StartTimeoutSeconds = 60,
    [ValidateRange(0.0, 1.0)][double]$MinimumTankThreatRatio = 0.90,
    [ValidateRange(0.0, 1.0)][double]$MinimumHealerTankPriorityRatio = 0.50,
    [ValidateRange(0.0, 1.0)][double]$MinimumNonTankTriageRatio = 0.50,
    [ValidateRange(1.0, 100.0)][double]$TriageHealthPct = 65.0,
    [ValidateRange(1.0, 500.0)][double]$CohesionRadius = 60.0,
    [ValidateRange(0.0, 1.0)][double]$MinimumCohesionRatio = 0.80,
    [ValidateRange(1.0, 5000.0)][double]$TeleportStepThreshold = 100.0,
    [ValidateRange(5.0, 500.0)][double]$MaximumSuspiciousVerticalDrop = 25.0,
    [ValidateRange(-1, 40)][int]$MaximumAllowedDeaths = -1,
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$WorldServerConfigPath = '',
    [string]$MySqlPath = '',
    [string]$CharactersDatabaseName = 'acore_characters',
    [string]$WorldDatabaseName = 'acore_world',
    [string]$ReceiptPath = '',
    [ValidateSet('127.0.0.1')][string]$BridgeHost = '127.0.0.1',
    [ValidateRange(1, 65535)][int]$Port = 18787,
    [ValidateRange(1000, 120000)][int]$TimeoutMs = 5000,
    [switch]$Apply
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$libraryPath = Join-Path $PSScriptRoot 'raid-readiness-proof-lib.ps1'
$controlPath = Join-Path $PSScriptRoot 'autowow-control.ps1'
foreach ($dependency in @($libraryPath, $controlPath)) {
    if (-not (Test-Path -LiteralPath $dependency)) { throw "Missing raid-readiness dependency: $dependency" }
}
. $libraryPath

$pinnedEncounterRequested = $ExpectedEncounterId -ne 0 -or $ExpectedEncounterCreditEntry -ne 0 -or `
    $ExpectedEncounterBit -ge 0 -or $ExpectedEncounterDifficulty -ge 0
if ($pinnedEncounterRequested -and ($ExpectedEncounterId -eq 0 -or $ExpectedEncounterCreditEntry -eq 0 -or `
        $ExpectedEncounterBit -lt 0 -or $ExpectedEncounterDifficulty -lt 0)) {
    throw 'ExpectedEncounterId, ExpectedEncounterCreditEntry, ExpectedEncounterBit, and ExpectedEncounterDifficulty must be supplied together.'
}

$contract = New-RaidReadinessContract -RaidSize $RaidSize -RosterGuid $RosterGuid -TankGuid $TankGuid `
    -HealerGuid $HealerGuid -LeaderGuid $LeaderGuid -TargetGuid $TargetGuid -TargetName $TargetName `
    -ExpectedMapId $ExpectedMapId -ExpectedInstanceId $ExpectedInstanceId
if ($MaximumAllowedDeaths -lt 0) {
    $MaximumAllowedDeaths = [Math]::Max(1, [int][Math]::Floor($RaidSize * 0.20))
}
$runId = "RAID-$RaidSize-$(Get-Date -Format 'yyyyMMdd-HHmmss')-$([guid]::NewGuid().ToString('N').Substring(0, 8))"
$plan = [ordered]@{
    schema = $script:RaidReadinessSchema
    schema_version = 2
    run_id = $runId
    started_at_utc = [datetime]::UtcNow.ToString('o')
    dry_run = (-not [bool]$Apply)
    apply = [bool]$Apply
    mode = if ($Apply) { 'observe-existing-encounter' } else { 'plan-only' }
    local_only = $true
    contract = $contract
    sampling = [ordered]@{
        duration_seconds = $DurationSeconds
        poll_seconds = $PollSeconds
        start_timeout_seconds = $StartTimeoutSeconds
        expected_bridge_requests_per_sample = 1 + $RaidSize
    }
    thresholds = [ordered]@{
        minimum_tank_threat_ratio = $MinimumTankThreatRatio
        minimum_healer_tank_priority_ratio = $MinimumHealerTankPriorityRatio
        minimum_non_tank_triage_ratio = $MinimumNonTankTriageRatio
        triage_health_pct = $TriageHealthPct
        cohesion_radius = $CohesionRadius
        minimum_cohesion_ratio = $MinimumCohesionRatio
        teleport_step_threshold = $TeleportStepThreshold
        maximum_suspicious_vertical_drop = $MaximumSuspiciousVerticalDrop
        maximum_allowed_deaths = $MaximumAllowedDeaths
    }
    bridge_contract = [ordered]@{
        allowed_actions = @('list', 'combatlog')
        mutation_actions = @()
        fixture_staging = [ordered]@{
            supported = $false
            reason = 'The existing bridge has fixture-init but no safe fixture-route action; staging is therefore omitted.'
        }
    }
    forbidden_actions = @(
        'party or raid formation', 'rally/deploy/route/advance/travel/recover', 'engage or attack orders',
        'teleport during encounter', 'fixture-init', 'quest or character-table mutation',
        'spawn or kill mobs', 'gear or XP grants', 'server restart'
    )
    telemetry_expectations = [ordered]@{
        required = @('exact roster', 'primary roles', 'map', 'instance', 'group size', 'leader', 'alive state', 'position', 'exact target death', 'threat links', 'healer target', 'encounter DONE', 'exact roster credit')
        optional = @('interrupt counters')
        unsupported_policy = 'Report unsupported counters honestly. Encounter DONE and exact roster credit remain UNPROVEN and block PASS when list/combatlog do not expose them.'
    }
    authoritative_completion_credit = [ordered]@{
        adapter = if ($pinnedEncounterRequested) { 'pinned-encounter-read-only-db' } elseif ([uint32]$ExpectedMapId -eq $script:RaidReadinessOnyxiaMapId) { 'onyxia-map-249-read-only-db' } else { 'generic-bridge-only' }
        status = 'NOT_RUN'
        required_for_pass = $true
        read_only = $true
        database_mutations = 0
        source = if ($pinnedEncounterRequested -or [uint32]$ExpectedMapId -eq $script:RaidReadinessOnyxiaMapId) { $script:RaidReadinessOnyxiaEncounterAuthority } else { 'not_applicable' }
        encounter_id = if ($pinnedEncounterRequested) { $ExpectedEncounterId } else { $null }
        encounter_credit_entry = if ($pinnedEncounterRequested) { $ExpectedEncounterCreditEntry } else { $null }
        encounter_bit = if ($pinnedEncounterRequested) { $ExpectedEncounterBit } else { $null }
        expected_difficulty = if ($pinnedEncounterRequested) { $ExpectedEncounterDifficulty } else { $null }
    }
    overall_status = if ($Apply) { 'NOT_RUN' } else { 'DRY_RUN' }
}

if (-not $Apply) {
    $plan.completed_at_utc = [datetime]::UtcNow.ToString('o')
    [pscustomobject]$plan | ConvertTo-Json -Depth 20
    return
}

$resolvedRoot = (Resolve-Path -LiteralPath $ServerRoot -ErrorAction Stop).Path
$logsRoot = Join-Path $resolvedRoot 'logs'
if (-not (Test-Path -LiteralPath $logsRoot)) { New-Item -ItemType Directory -Path $logsRoot -Force | Out-Null }
if ([string]::IsNullOrWhiteSpace($ReceiptPath)) {
    $ReceiptPath = Join-Path $logsRoot "raid-readiness-$RaidSize-$runId.jsonl"
} elseif (-not [System.IO.Path]::IsPathRooted($ReceiptPath)) {
    $ReceiptPath = Join-Path $logsRoot $ReceiptPath
}
$ReceiptPath = [System.IO.Path]::GetFullPath($ReceiptPath)
$logsPrefix = [System.IO.Path]::GetFullPath($logsRoot).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
if (-not $ReceiptPath.StartsWith($logsPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "ReceiptPath must remain under the AutoWoW logs directory: $ReceiptPath"
}
if (Test-Path -LiteralPath $ReceiptPath) { throw "Refusing to overwrite an existing raid-readiness receipt: $ReceiptPath" }

function Write-RaidReadinessReceipt {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Event,
        [System.Collections.IDictionary]$Fields = @{}
    )

    $row = [ordered]@{
        schema = $script:RaidReadinessSchema
        timestamp_utc = [datetime]::UtcNow.ToString('o')
        run_id = $runId
        event = $Event
    }
    foreach ($key in $Fields.Keys) { $row[$key] = $Fields[$key] }
    [System.IO.File]::AppendAllText(
        $ReceiptPath,
        (($row | ConvertTo-Json -Compress -Depth 30) + [Environment]::NewLine),
        [System.Text.UTF8Encoding]::new($false))
}

function Invoke-RaidReadinessBridgeJson {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][ValidateSet('list', 'combatlog')][string]$Action,
        [uint32]$Guid = 0,
        [switch]$AllowBridgeError
    )

    $raw = if ($Action -eq 'list') {
        & $controlPath -Action list -BridgeHost $BridgeHost -Port $Port -TimeoutMs $TimeoutMs
    } else {
        & $controlPath -Action combatlog -BotGuid $Guid -BridgeHost $BridgeHost -Port $Port -TimeoutMs $TimeoutMs
    }
    $jsonLines = @($raw | ForEach-Object { [string]$_ } | Where-Object { $_ -match '^\s*\{' })
    if ($jsonLines.Count -eq 0) { throw "Bridge action $Action returned no JSON response." }
    try { $response = $jsonLines[-1] | ConvertFrom-Json } catch { throw "Bridge action $Action returned malformed JSON." }
    $ok = Get-RaidReadinessProperty -Object $response -Name @('ok')
    if ($null -ne $ok -and -not [bool]$ok -and -not $AllowBridgeError) {
        throw "Bridge action $Action failed for GUID ${Guid}: $(Get-RaidReadinessProperty -Object $response -Name @('error') -Default 'unknown_error')"
    }
    return $response
}

function Get-RaidReadinessLiveSample {
    param([uint32[]]$ObservedDeadGuid = @())

    $list = Invoke-RaidReadinessBridgeJson -Action list
    $combatResponses = [ordered]@{}
    foreach ($guid in $contract.roster_guids) {
        $combatResponses[[string]$guid] = Invoke-RaidReadinessBridgeJson -Action combatlog `
            -Guid ([uint32]$guid) -AllowBridgeError
    }
    return ConvertTo-RaidReadinessCanonicalBridgeSample -ListResponse $list `
        -CombatResponseByGuid $combatResponses -Contract $contract -ObservedDeadGuid $ObservedDeadGuid
}

$samples = New-Object System.Collections.Generic.List[object]
$guardReasons = New-Object System.Collections.Generic.List[string]
$observedAttrition = [System.Collections.Generic.HashSet[uint32]]::new()
$observedOfflineAttrition = [System.Collections.Generic.HashSet[uint32]]::new()
$fatalError = $null
$verdict = $null
$completionCreditGate = $null
Write-RaidReadinessReceipt -Event 'raid_readiness_started' -Fields ([ordered]@{
    plan = $plan
    receipt_path = $ReceiptPath
})

try {
    $preflight = Get-RaidReadinessLiveSample
    $preflightGuard = Test-RaidReadinessRosterGuard -Sample $preflight -Contract $contract
    Write-RaidReadinessReceipt -Event 'raid_readiness_preflight' -Fields ([ordered]@{
        roster_guard = $preflightGuard
        members = $preflight.members
        target = $preflight.target
    })
    if (-not $preflightGuard.passed) {
        foreach ($reason in $preflightGuard.reasons) { $guardReasons.Add([string]$reason) }
        throw "Preflight failed closed: $($preflightGuard.reasons -join '; ')"
    }

    $encounterStarted = $false
    $startDeadline = (Get-Date).AddSeconds($StartTimeoutSeconds)
    $encounterDeadline = $null
    $quietSamples = 0
    $nextSample = $preflight

    while ($true) {
        $sample = $nextSample
        $nextSample = $null
        $guard = Test-RaidReadinessRosterGuard -Sample $sample -Contract $contract -AllowReleasedDead
        if (-not $guard.passed) {
            foreach ($reason in $guard.reasons) { $guardReasons.Add([string]$reason) }
            Write-RaidReadinessReceipt -Event 'raid_readiness_guard_failed' -Fields ([ordered]@{ roster_guard = $guard; members = $sample.members })
            throw "Encounter roster guard failed closed: $($guard.reasons -join '; ')"
        }
        $newAttrition = @($guard.attrition_guids | Where-Object { $observedAttrition.Add([uint32]$_) })
        $newOfflineAttrition = @($guard.offline_dead_guids | Where-Object { $observedOfflineAttrition.Add([uint32]$_) })
        if ($newAttrition.Count -gt 0 -or $newOfflineAttrition.Count -gt 0) {
            Write-RaidReadinessReceipt -Event 'raid_readiness_attrition' -Fields ([ordered]@{
                dead_guids = @($newAttrition | ForEach-Object { [uint32]$_ })
                offline_dead_guids = @($newOfflineAttrition | ForEach-Object { [uint32]$_ })
                cumulative_dead_guids = @($observedAttrition | Sort-Object)
                maximum_allowed_deaths = $MaximumAllowedDeaths
            })
        }

        $anyCombat = @($sample.members | Where-Object { [bool]$_.in_combat }).Count -gt 0
        $targetObserved = [bool]$sample.target.observed
        if (-not $encounterStarted -and ($anyCombat -or $targetObserved)) {
            $encounterStarted = $true
            $encounterDeadline = (Get-Date).AddSeconds($DurationSeconds)
            Write-RaidReadinessReceipt -Event 'raid_encounter_observed' -Fields ([ordered]@{ observed_at_utc = $sample.observed_at_utc; target = $sample.target })
        }

        if ($encounterStarted) {
            $samples.Add($sample)
            Write-RaidReadinessReceipt -Event 'raid_readiness_sample' -Fields ([ordered]@{
                sample_index = $samples.Count
                observed_at_utc = $sample.observed_at_utc
                target = $sample.target
                target_owner_guid = $sample.target_owner_guid
                target_threat_available = $sample.target_threat_available
                hostile_owner_events = $sample.hostile_owner_events
                tank_hostile_owner_events = $sample.tank_hostile_owner_events
                threat_links_truncated = $sample.threat_links_truncated
                members = $sample.members
            })
            if ($null -ne $sample.target.alive -and -not [bool]$sample.target.alive) { break }
            if (-not $anyCombat -and -not $targetObserved) { $quietSamples++ } else { $quietSamples = 0 }
            if ($quietSamples -ge 2) { break }
            if ((Get-Date) -ge $encounterDeadline) { break }
        } elseif ((Get-Date) -ge $startDeadline) {
            throw "No encounter was observed within $StartTimeoutSeconds seconds."
        }

        Start-Sleep -Seconds $PollSeconds
        $nextSample = Get-RaidReadinessLiveSample -ObservedDeadGuid @($observedAttrition)
    }

    if ($pinnedEncounterRequested) {
        $completionCreditGate = Get-RaidReadinessPinnedEncounterCompletionCreditFromDatabase -Contract $contract `
            -ServerRoot $resolvedRoot -EncounterId $ExpectedEncounterId -CreditEntry $ExpectedEncounterCreditEntry `
            -EncounterBit $ExpectedEncounterBit -ExpectedDifficulty $ExpectedEncounterDifficulty `
            -WorldServerConfigPath $WorldServerConfigPath -MySqlPath $MySqlPath `
            -CharactersDatabaseName $CharactersDatabaseName -WorldDatabaseName $WorldDatabaseName
        Write-RaidReadinessReceipt -Event 'raid_authoritative_completion_credit' -Fields ([ordered]@{ completion_credit = $completionCreditGate })
    } elseif ([uint32]$contract.expected_map_id -eq $script:RaidReadinessOnyxiaMapId) {
        $completionCreditGate = Get-RaidReadinessOnyxiaCompletionCreditFromDatabase -Contract $contract `
            -ServerRoot $resolvedRoot -WorldServerConfigPath $WorldServerConfigPath -MySqlPath $MySqlPath `
            -CharactersDatabaseName $CharactersDatabaseName -WorldDatabaseName $WorldDatabaseName
        Write-RaidReadinessReceipt -Event 'raid_authoritative_completion_credit' -Fields ([ordered]@{
            completion_credit = $completionCreditGate
        })
    }

    $verdict = Measure-RaidReadinessSamples -Samples $samples.ToArray() -Contract $contract `
        -MinimumTankThreatRatio $MinimumTankThreatRatio `
        -MinimumHealerTankPriorityRatio $MinimumHealerTankPriorityRatio `
        -MinimumNonTankTriageRatio $MinimumNonTankTriageRatio `
        -TriageHealthPct $TriageHealthPct -CohesionRadius $CohesionRadius `
        -MinimumCohesionRatio $MinimumCohesionRatio -TeleportStepThreshold $TeleportStepThreshold `
        -MaximumSuspiciousVerticalDrop $MaximumSuspiciousVerticalDrop `
        -MaximumAllowedDeaths $MaximumAllowedDeaths `
        -CompletionCreditGate $completionCreditGate `
        -RosterGuardMaintained $true
}
catch {
    $fatalError = $_.Exception.Message
    if ($guardReasons.Count -eq 0) { $guardReasons.Add($fatalError) }
    if (($pinnedEncounterRequested -or [uint32]$contract.expected_map_id -eq $script:RaidReadinessOnyxiaMapId) -and $null -eq $completionCreditGate) {
        $completionCreditGate = New-RaidReadinessBlockedCompletionCreditGate -Contract $contract `
            -Reasons @('authoritative_completion_credit_not_observed_after_live_failure') `
            -Source 'live_observation_failed'
    }
    $verdict = Measure-RaidReadinessSamples -Samples $samples.ToArray() -Contract $contract `
        -MinimumTankThreatRatio $MinimumTankThreatRatio `
        -MinimumHealerTankPriorityRatio $MinimumHealerTankPriorityRatio `
        -MinimumNonTankTriageRatio $MinimumNonTankTriageRatio `
        -TriageHealthPct $TriageHealthPct -CohesionRadius $CohesionRadius `
        -MinimumCohesionRatio $MinimumCohesionRatio -TeleportStepThreshold $TeleportStepThreshold `
        -MaximumSuspiciousVerticalDrop $MaximumSuspiciousVerticalDrop `
        -MaximumAllowedDeaths $MaximumAllowedDeaths `
        -CompletionCreditGate $completionCreditGate `
        -RosterGuardMaintained $false -RosterGuardReasons $guardReasons.ToArray()
    Write-RaidReadinessReceipt -Event 'raid_readiness_error' -Fields ([ordered]@{ error = $fatalError; roster_guard_reasons = $guardReasons.ToArray() })
}

Write-RaidReadinessReceipt -Event 'raid_readiness_verdict' -Fields ([ordered]@{
    verdict = $verdict
    receipt_path = $ReceiptPath
    fatal_error = $fatalError
})

[pscustomobject][ordered]@{
    schema = $script:RaidReadinessSchema
    run_id = $runId
    overall_status = $verdict.overall_status
    raid_size = $RaidSize
    sample_count = $verdict.sample_count
    receipt_path = $ReceiptPath
    fatal_error = $fatalError
    verdict = $verdict
} | ConvertTo-Json -Depth 30

if ($verdict.overall_status -notin @('PASS', 'PARTIAL')) { exit 2 }
