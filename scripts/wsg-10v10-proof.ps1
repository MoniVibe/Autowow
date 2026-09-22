<#
.SYNOPSIS
    Deterministic AutoWoW Warsong Gulch 10v10 control proof.

.DESCRIPTION
    Dry-runs by default. Live local control requires the explicit -Apply switch and exactly twenty
    ordered fixture GUIDs: the first ten Alliance, followed by ten Horde. The script activates only
    that roster, normalizes it by default, sends one atomic queue request, polls one atomic status
    request at a one- or two-second cadence for at most thirty-five minutes, and always attempts
    exact-roster leave/deactivation cleanup.

    Queue/instance proof and gameplay-telemetry proof are reported independently. Entering one
    nonzero instance with the exact roster does not claim that kill/death, flag, score, or winner
    telemetry succeeded.

    -SkipFixtureInit is an explicit role-preserving opt-in. It skips all fixture-init calls and
    instead requires read-only fixture-status evidence for the exact online roster: level 80,
    rare quality 3 gear, and initialized/configured existing specs.
#>
[CmdletBinding()]
param(
    [uint32[]]$RosterGuid = @(),
    [ValidatePattern('^[A-Za-z][A-Za-z0-9]{2,7}$')][string]$FixtureId = 'awpvp1',
    [ValidateRange(10, 80)][int]$Level = 80,
    [ValidateRange(0, 19)][int]$SpecIndex = 0,
    [ValidateRange(0, 5)][int]$Quality = 2,
    [ValidateRange(1, 2)][int]$PollSeconds = 2,
    [ValidateRange(1, 35)][int]$MaxWaitMinutes = 35,
    [ValidateRange(10, 300)][int]$ActivationTimeoutSeconds = 120,
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$ReportPath = '',
    [ValidateSet('127.0.0.1')][string]$BridgeHost = '127.0.0.1',
    [ValidateRange(1, 65535)][int]$Port = 18787,
    [ValidateRange(1000, 120000)][int]$TimeoutMs = 5000,
    [switch]$SkipFixtureInit,
    [switch]$Apply
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$fixtureLibrary = Join-Path $PSScriptRoot 'pvp-fixture-lib.ps1'
$proofLibrary = Join-Path $PSScriptRoot 'wsg-10v10-proof-lib.ps1'
$controlScript = Join-Path $PSScriptRoot 'autowow-control.ps1'
foreach ($requiredPath in @($fixtureLibrary, $proofLibrary, $controlScript)) {
    if (-not (Test-Path -LiteralPath $requiredPath)) { throw "Missing WSG proof dependency: $requiredPath" }
}
. $fixtureLibrary
. $proofLibrary

$orderedRoster = @($RosterGuid | ForEach-Object { [uint32]$_ })
if ($orderedRoster.Count -ne 20 -or @($orderedRoster | Sort-Object -Unique).Count -ne 20 -or @($orderedRoster | Where-Object { $_ -eq 0 }).Count -ne 0) {
    throw 'RosterGuid must contain exactly 20 unique positive GUIDs in Alliance-then-Horde order.'
}

if ($SkipFixtureInit -and $PSBoundParameters.ContainsKey('Level') -and $Level -ne 80) {
    throw '-SkipFixtureInit requires level 80; existing role-balanced WSG fixtures are level 80.'
}
if ($SkipFixtureInit -and $PSBoundParameters.ContainsKey('Quality') -and $Quality -ne 3) {
    throw '-SkipFixtureInit requires quality 3 (rare); existing role-balanced WSG fixtures use rare gear.'
}

$validationLevel = if ($SkipFixtureInit) { 80 } else { $Level }
$validationQuality = if ($SkipFixtureInit) { 3 } else { $Quality }
$fixtureInitMode = if ($SkipFixtureInit) { 'PRESERVE_EXISTING' } else { 'NORMALIZE_GLOBAL_SPEC' }
$specPolicy = if ($SkipFixtureInit) { 'preserve_existing' } else { 'global_spec_index' }

$definition = Get-PvpFixtureDefinition -FixtureId $FixtureId -Level $Level
Assert-PvpFixtureDefinition -Definition $definition | Out-Null
$stage = @($definition.stages | Where-Object stage_id -eq 'wsg-10v10')
if ($stage.Count -ne 1) { throw 'The PvP fixture definition does not expose exactly one wsg-10v10 stage.' }
$stage = $stage[0]
$expectedMembers = @($definition.accounts | Where-Object faction -eq 'Alliance') + @($definition.accounts | Where-Object faction -eq 'Horde')
$expectedBracket = if ($validationLevel -eq 80) { 7 } else { [Math]::Floor(($validationLevel - 10) / 10) }
$qualityNames = @('poor', 'normal', 'uncommon', 'rare', 'epic', 'legendary')
$expectedQualityName = $qualityNames[$validationQuality]

$queueRequest = & $controlScript -Action wsg-queue -MemberGuid $orderedRoster -EmitRequestOnly
$statusRequest = & $controlScript -Action wsg-status -MemberGuid $orderedRoster -EmitRequestOnly
$leaveRequest = & $controlScript -Action wsg-leave -MemberGuid $orderedRoster -EmitRequestOnly
$runId = "WSG10-$(Get-Date -Format 'yyyyMMdd-HHmmss')-$([guid]::NewGuid().ToString('N').Substring(0, 8))"
$report = [ordered]@{
    schema = 'autowow.wsg.10v10.proof.v1'
    schema_version = 1
    run_id = $runId
    fixture_id = $FixtureId
    stage_id = 'wsg-10v10'
    started_at_utc = [datetime]::UtcNow.ToString('o')
    dry_run = (-not [bool]$Apply)
    apply = [bool]$Apply
    local_only = $true
    ordered_roster = @($orderedRoster)
    alliance_guids = @($orderedRoster | Select-Object -First 10)
    horde_guids = @($orderedRoster | Select-Object -Skip 10)
    expected_characters = @($expectedMembers | ForEach-Object character_name)
    expected_level = $validationLevel
    expected_bracket_index = $expectedBracket
    expected_spec_index = if ($SkipFixtureInit) { $null } else { $SpecIndex }
    expected_quality = $validationQuality
    fixture_init_mode = $fixtureInitMode
    spec_policy = $specPolicy
    poll_seconds = $PollSeconds
    max_wait_minutes = $MaxWaitMinutes
    control_plan = [ordered]@{
        activate_requests = 20
        fixture_init_requests = if ($SkipFixtureInit) { 0 } else { 20 }
        fixture_status_requests = 20
        fixture_preflight = if ($SkipFixtureInit) { 'strict_existing_fixture_status' } else { 'post_fixture_init_status' }
        grouping_requests = 0
        queue_requests = 1
        queue_request = [string]$queueRequest
        repeated_status_request = [string]$statusRequest
        leave_requests = 1
        leave_request = [string]$leaveRequest
        cleanup_deactivate_requests = 20
    }
    loadout_validation = [ordered]@{ status = 'NOT_RUN'; bracket_index = $expectedBracket; members = @(); reasons = @() }
    queue_instance = [ordered]@{ status = 'NOT_RUN'; instance_id = 0; reasons = @('live_queue_not_run'); status_samples = 0 }
    gameplay_telemetry = [ordered]@{
        status = 'NOT_RUN'
        reasons = @('live_gameplay_not_run')
        deltas = [ordered]@{ kills = 0; deaths = 0; flag_state_changes = 0; flag_captures = 0; flag_returns = 0 }
        final_score = $null
        winner = ''
    }
    cleanup = [ordered]@{ status = 'NOT_RUN'; leave_verified = $false; deactivated_count = 0; remaining_online_guids = @(); reasons = @() }
    overall_status = 'DRY_RUN'
    errors = @()
}

if (-not $Apply) {
    $report.completed_at_utc = [datetime]::UtcNow.ToString('o')
    $report | ConvertTo-Json -Depth 16
    return
}

$resolvedRoot = (Resolve-Path -LiteralPath $ServerRoot -ErrorAction Stop).Path
$logsRoot = Join-Path $resolvedRoot 'logs'
if (-not (Test-Path -LiteralPath $logsRoot)) { New-Item -ItemType Directory -Path $logsRoot -Force | Out-Null }
if ([string]::IsNullOrWhiteSpace($ReportPath)) {
    $ReportPath = Join-Path $logsRoot "wsg-10v10-proof-$runId.json"
}
elseif (-not [System.IO.Path]::IsPathRooted($ReportPath)) {
    $ReportPath = Join-Path $logsRoot $ReportPath
}
$ReportPath = [System.IO.Path]::GetFullPath($ReportPath)
$logsPrefix = [System.IO.Path]::GetFullPath($logsRoot).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
if (-not $ReportPath.StartsWith($logsPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "ReportPath must remain under the AutoWoW logs directory: $ReportPath"
}
if (Test-Path -LiteralPath $ReportPath) { throw "Refusing to overwrite an existing WSG proof report: $ReportPath" }

function Get-WsgProofBridgeResponse {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Action,
        [uint32]$BotGuid = 0,
        [uint32[]]$Members = @(),
        [int]$FixtureLevel = 1,
        [int]$FixtureSpecIndex = 0,
        [int]$FixtureQuality = 2
    )

    $raw = switch ($Action) {
        'list' { & $controlScript -Action list -BridgeHost $BridgeHost -Port $Port -TimeoutMs $TimeoutMs }
        'activate' { & $controlScript -Action activate -BotGuid $BotGuid -BridgeHost $BridgeHost -Port $Port -TimeoutMs $TimeoutMs }
        'deactivate' { & $controlScript -Action deactivate -BotGuid $BotGuid -BridgeHost $BridgeHost -Port $Port -TimeoutMs $TimeoutMs }
        'fixture-init' { & $controlScript -Action fixture-init -BotGuid $BotGuid -Level $FixtureLevel -SpecIndex $FixtureSpecIndex -Quality $FixtureQuality -BridgeHost $BridgeHost -Port $Port -TimeoutMs $TimeoutMs }
        'fixture-status' { & $controlScript -Action fixture-status -BotGuid $BotGuid -BridgeHost $BridgeHost -Port $Port -TimeoutMs $TimeoutMs }
        'wsg-queue' { & $controlScript -Action wsg-queue -MemberGuid $Members -BridgeHost $BridgeHost -Port $Port -TimeoutMs $TimeoutMs }
        'wsg-status' { & $controlScript -Action wsg-status -MemberGuid $Members -BridgeHost $BridgeHost -Port $Port -TimeoutMs $TimeoutMs }
        'wsg-leave' { & $controlScript -Action wsg-leave -MemberGuid $Members -BridgeHost $BridgeHost -Port $Port -TimeoutMs $TimeoutMs }
        default { throw "Unsupported WSG proof control action: $Action" }
    }
    $jsonLines = @($raw | ForEach-Object { [string]$_ } | Where-Object { $_ -match '^\s*\{' })
    if ($jsonLines.Count -eq 0) { throw "Control action $Action returned no JSON response." }
    try { $response = $jsonLines[-1] | ConvertFrom-Json } catch { throw "Control action $Action returned malformed JSON." }
    $ok = Get-Wsg10v10Property -Object $response -Name @('ok')
    if ($null -ne $ok -and -not [bool]$ok) {
        $errorCode = [string](Get-Wsg10v10Property -Object $response -Name @('error'))
        throw "Control action $Action failed: $errorCode"
    }
    return $response
}

function Get-WsgProofOnlineGuids {
    $response = Get-WsgProofBridgeResponse -Action list
    $bots = @(Get-Wsg10v10Property -Object $response -Name @('bots'))
    return @($bots | ForEach-Object { [uint32](Get-Wsg10v10Property -Object $_ -Name @('guid')) })
}

function Get-WsgProofQualitySignature {
    [CmdletBinding()]
    param([Parameter(Mandatory)][object]$Status)

    $gear = Get-Wsg10v10Property -Object $Status -Name @('gear')
    $counts = Get-Wsg10v10Property -Object $gear -Name @('quality_counts')
    $properties = @($counts.PSObject.Properties | Sort-Object Name | ForEach-Object { "$($_.Name)=$($_.Value)" })
    return "equipped=$(Get-Wsg10v10Property -Object $gear -Name @('equipped_slots'));other=$(Get-Wsg10v10Property -Object $gear -Name @('other_quality_slots'));$($properties -join ';')"
}

$queueAttempted = $false
$instanceSamples = New-Object System.Collections.Generic.List[object]
$instanceId = 0L
$instanceMaintained = $true
$instanceFailureStreak = 0
$maxInstanceFailureStreak = 0
$instanceFailureCount = 0
$terminalObserved = $false
$applyError = $null
$cleanupReasons = New-Object System.Collections.Generic.List[string]
$cleanupWarnings = New-Object System.Collections.Generic.List[string]

try {
    $initialOnlineGuids = @(Get-WsgProofOnlineGuids)
    foreach ($guid in $orderedRoster) {
        if ($guid -notin $initialOnlineGuids) {
            Get-WsgProofBridgeResponse -Action activate -BotGuid $guid | Out-Null
        }
    }
    $activationDeadline = (Get-Date).AddSeconds($ActivationTimeoutSeconds)
    do {
        $onlineGuids = @(Get-WsgProofOnlineGuids)
        $missing = @($orderedRoster | Where-Object { $onlineGuids -notcontains $_ })
        if ($missing.Count -eq 0) { break }
        Start-Sleep -Seconds $PollSeconds
    } while ((Get-Date) -lt $activationDeadline)
    if ($missing.Count -ne 0) { throw "Timed out activating exact WSG roster GUIDs: $($missing -join ', ')" }

    $loadoutRows = @()
    foreach ($guid in $orderedRoster) {
        if (-not $SkipFixtureInit) {
            Get-WsgProofBridgeResponse -Action fixture-init -BotGuid $guid -FixtureLevel $Level -FixtureSpecIndex $SpecIndex -FixtureQuality $Quality | Out-Null
        }
        $status = Get-WsgProofBridgeResponse -Action fixture-status -BotGuid $guid
        $statusOkValue = Get-Wsg10v10Property -Object $status -Name @('ok')
        $statusGuidValue = ConvertTo-Wsg10v10Int (Get-Wsg10v10Property -Object $status -Name @('guid'))
        $statusOperation = [string](Get-Wsg10v10Property -Object $status -Name @('operation'))
        $levelValue = [int](Get-Wsg10v10Property -Object $status -Name @('level'))
        $class = Get-Wsg10v10Property -Object $status -Name @('class')
        $spec = Get-Wsg10v10Property -Object $status -Name @('spec')
        $gear = Get-Wsg10v10Property -Object $status -Name @('gear')
        $qualityCounts = Get-Wsg10v10Property -Object $gear -Name @('quality_counts')
        $specIndexValue = [int](Get-Wsg10v10Property -Object $spec -Name @('stored_index'))
        $specNameValue = [string](Get-Wsg10v10Property -Object $spec -Name @('name'))
        $equippedSlotsValue = [int](Get-Wsg10v10Property -Object $gear -Name @('equipped_slots'))
        $equipmentSlotsCheckedValue = [int](Get-Wsg10v10Property -Object $gear -Name @('equipment_slots_checked'))
        $expectedQualitySlotsValue = [int](Get-Wsg10v10Property -Object $qualityCounts -Name @($expectedQualityName))
        $otherQualitySlotsValue = [int](Get-Wsg10v10Property -Object $gear -Name @('other_quality_slots'))
        $onlineValue = @($onlineGuids | Where-Object { [uint32]$_ -eq [uint32]$guid }).Count -eq 1
        $fixtureEligibleValue = $onlineValue -and
            $null -ne $statusOkValue -and [bool]$statusOkValue -and
            $statusOperation -eq 'fixture_status' -and $statusGuidValue -eq [uint32]$guid
        $gearInitializedValue = if ($SkipFixtureInit) {
            $equipmentSlotsCheckedValue -gt 0 -and $equippedSlotsValue -ge ($equipmentSlotsCheckedValue - 1) -and
                $expectedQualitySlotsValue -ge ($equippedSlotsValue - 1)
        } else {
            $expectedQualitySlotsValue -eq $equippedSlotsValue
        }
        $initializedValue = $specIndexValue -ge 0 -and $specIndexValue -le 19 -and
            -not [string]::IsNullOrWhiteSpace($specNameValue) -and $specNameValue -notin @('unknown', 'unconfigured') -and
            $equippedSlotsValue -gt 0 -and $gearInitializedValue -and
            $otherQualitySlotsValue -eq 0
        $loadoutRows += [pscustomobject]@{
            guid = [uint32]$guid
            online = $onlineValue
            fixture_eligible = $fixtureEligibleValue
            status_ok = [bool]$statusOkValue
            status_operation = $statusOperation
            status_guid = $statusGuidValue
            level = $levelValue
            class_id = [int](Get-Wsg10v10Property -Object $class -Name @('id'))
            spec_index = $specIndexValue
            spec_name = $specNameValue
            initialized = $initializedValue
            quality_signature = Get-WsgProofQualitySignature -Status $status
            equipped_slots = $equippedSlotsValue
            equipment_slots_checked = $equipmentSlotsCheckedValue
            expected_quality_slots = $expectedQualitySlotsValue
            other_quality_slots = $otherQualitySlotsValue
            bracket_index = if ($levelValue -eq 80) { 7 } else { [Math]::Floor(($levelValue - 10) / 10) }
        }
    }
    if ($SkipFixtureInit) {
        $preflight = Test-Wsg10v10ExistingFixturePreflight -OrderedRosterGuid $orderedRoster -EvidenceRows $loadoutRows `
            -ExpectedLevel $validationLevel -ExpectedBracket $expectedBracket -ExpectedQuality $validationQuality `
            -ExpectedQualityName $expectedQualityName
        $report.loadout_validation = $preflight
        if ($preflight.status -ne 'PASS') {
            throw "WSG existing fixture preflight failed: $($preflight.reasons -join ', ')"
        }
    }
    else {
        $loadoutReasons = New-Object System.Collections.Generic.List[string]
        if (@($loadoutRows | Where-Object level -ne $validationLevel).Count -ne 0) { $loadoutReasons.Add('level_mismatch') }
        if (@($loadoutRows | Where-Object bracket_index -ne $expectedBracket).Count -ne 0) { $loadoutReasons.Add('bracket_mismatch') }
        if (@($loadoutRows | Where-Object spec_index -ne $SpecIndex).Count -ne 0) { $loadoutReasons.Add('spec_index_mismatch') }
        if (@($loadoutRows | Select-Object -ExpandProperty class_id -Unique).Count -ne 1) { $loadoutReasons.Add('class_loadouts_differ') }
        if (@($loadoutRows | Where-Object class_id -ne 1).Count -ne 0) { $loadoutReasons.Add('fixture_class_mismatch') }
        if (@($loadoutRows | Select-Object -ExpandProperty quality_signature -Unique).Count -ne 1) { $loadoutReasons.Add('gear_loadouts_differ') }
        if (@($loadoutRows | Where-Object { $_.equipped_slots -le 0 -or $_.expected_quality_slots -ne $_.equipped_slots -or $_.other_quality_slots -ne 0 }).Count -ne 0) {
            $loadoutReasons.Add("gear_quality_mismatch:$expectedQualityName")
        }
        $report.loadout_validation = [ordered]@{
            status = if ($loadoutReasons.Count -eq 0) { 'PASS' } else { 'FAIL' }
            bracket_index = $expectedBracket
            members = $loadoutRows
            reasons = @($loadoutReasons)
        }
        if ($loadoutReasons.Count -ne 0) { throw "WSG loadout validation failed: $($loadoutReasons -join ', ')" }
    }

    $queueAttempted = $true
    Get-WsgProofBridgeResponse -Action wsg-queue -Members $orderedRoster | Out-Null
    $pollDeadline = (Get-Date).AddMinutes($MaxWaitMinutes)
    do {
        $rawStatus = Get-WsgProofBridgeResponse -Action wsg-status -Members $orderedRoster
        $canonical = ConvertTo-Wsg10v10CanonicalStatus -Response $rawStatus
        $instanceCheck = Test-Wsg10v10InstanceStatus -Status $canonical -OrderedRosterGuid $orderedRoster
        if ($instanceCheck.passed) {
            $instanceFailureStreak = 0
            if ($instanceId -eq 0) { $instanceId = [long]$instanceCheck.instance_id }
            elseif ($instanceId -ne [long]$instanceCheck.instance_id) { $instanceMaintained = $false }
            $instanceSamples.Add($canonical)
            $terminalObserved = $canonical.battleground_state -match '^(complete|completed|ended|finished|wait_leave|closed)$'
            if ($terminalObserved) { break }
        }
        elseif ($instanceId -ne 0) {
            # A single status poll can transiently miss a respawning/transitioning member. Only
            # treat the exact roster as lost after three consecutive failures (at least 6 seconds
            # at the default cadence); isolated misses remain visible in the receipt.
            $instanceFailureCount++
            $instanceFailureStreak++
            $maxInstanceFailureStreak = [Math]::Max($maxInstanceFailureStreak, $instanceFailureStreak)
            if ($instanceFailureStreak -ge 3) { $instanceMaintained = $false }
        }
        Start-Sleep -Seconds $PollSeconds
    } while ((Get-Date) -lt $pollDeadline)

    $queueReasons = New-Object System.Collections.Generic.List[string]
    if ($instanceId -eq 0) { $queueReasons.Add('no_exact_nonzero_10v10_instance_observed') }
    if (-not $instanceMaintained) { $queueReasons.Add('instance_or_exact_roster_changed_after_entry') }
    $queuePassed = $instanceId -gt 0 -and $instanceMaintained
    $report.queue_instance = [ordered]@{
        status = if ($queuePassed) { 'PASS' } else { 'FAIL' }
        instance_id = $instanceId
        reasons = @($queueReasons)
        status_samples = $instanceSamples.Count
        transient_status_failures = $instanceFailureCount
        max_consecutive_status_failures = $maxInstanceFailureStreak
        exact_alliance_count = if ($queuePassed) { 10 } else { 0 }
        exact_horde_count = if ($queuePassed) { 10 } else { 0 }
    }
    # PowerShell 7 can throw "Argument types do not match" when a generic List[object]
    # is expanded directly through @(...). Materialize a real object[] first so long
    # live runs preserve their telemetry instead of failing after the polling window.
    [object[]]$sampleArray = $instanceSamples.ToArray()
    $gameplayVerdict = Get-Wsg10v10GameplayVerdict -Samples $sampleArray -InstanceRosterMaintained $instanceMaintained
    $report.gameplay_telemetry = [ordered]@{
        status = $gameplayVerdict.status
        reasons = @($gameplayVerdict.reasons)
        deltas = $gameplayVerdict.deltas
        final_score = $gameplayVerdict.final_score
        winner = $gameplayVerdict.winner
        final_state = $gameplayVerdict.final_state
        terminal_observed = $terminalObserved
        sample_count = $gameplayVerdict.sample_count
        first_sample = if ($instanceSamples.Count -gt 0) { $instanceSamples[0] } else { $null }
        final_sample = if ($instanceSamples.Count -gt 0) { $instanceSamples[$instanceSamples.Count - 1] } else { $null }
    }
}
catch {
    $applyError = $_
    $report.errors += $_.Exception.Message
}
finally {
    $leaveVerified = $false
    if ($queueAttempted) {
        try {
            Get-WsgProofBridgeResponse -Action wsg-leave -Members $orderedRoster | Out-Null
            $postLeaveRaw = Get-WsgProofBridgeResponse -Action wsg-status -Members $orderedRoster
            $postLeave = ConvertTo-Wsg10v10CanonicalStatus -Response $postLeaveRaw
            $inInstance = @($postLeave.roster | Where-Object { $null -ne $_.instance_id -and [long]$_.instance_id -gt 0 })
            $stillQueued = @($postLeave.roster | Where-Object { [string]$_.queue_state -match 'queue|queued|invite|wait_join|in_progress' })
            $leaveVerified = $inInstance.Count -eq 0 -and $stillQueued.Count -eq 0
            if (-not $leaveVerified) { $cleanupWarnings.Add('post_leave_status_still_queued_or_instanced') }
        }
        catch { $cleanupWarnings.Add("leave_verification_error:$($_.Exception.Message)") }
    }
    else { $leaveVerified = $true }

    $deactivatedCount = 0
    foreach ($guid in $orderedRoster) {
        try {
            Get-WsgProofBridgeResponse -Action deactivate -BotGuid $guid | Out-Null
            $deactivatedCount++
        }
        catch { $cleanupReasons.Add("deactivate_${guid}_error:$($_.Exception.Message)") }
    }
    $cleanupDeadline = (Get-Date).AddSeconds($ActivationTimeoutSeconds)
    $remaining = @($orderedRoster)
    try {
        do {
            $onlineGuids = @(Get-WsgProofOnlineGuids)
            $remaining = @($orderedRoster | Where-Object { $onlineGuids -contains $_ })
            if ($remaining.Count -eq 0) { break }
            Start-Sleep -Seconds $PollSeconds
        } while ((Get-Date) -lt $cleanupDeadline)
    }
    catch { $cleanupReasons.Add("online_cleanup_verification_error:$($_.Exception.Message)") }
    if ($remaining.Count -ne 0) { $cleanupReasons.Add("fixture_guids_still_online:$($remaining -join ',')") }
    # Exact deactivation plus an empty online roster is the authoritative cleanup gate. A match
    # can reject explicit leave while still removing every bot safely during logout.
    $cleanupPassed = $remaining.Count -eq 0 -and $deactivatedCount -eq 20 -and $cleanupReasons.Count -eq 0
    $report.cleanup = [ordered]@{
        status = if ($cleanupPassed) { 'PASS' } else { 'FAIL' }
        leave_verified = $leaveVerified
        deactivated_count = $deactivatedCount
        remaining_online_guids = @($remaining)
        reasons = @($cleanupReasons)
        warnings = @($cleanupWarnings)
    }
}

$queueStatus = [string]$report.queue_instance.status
$gameplayStatus = [string]$report.gameplay_telemetry.status
$cleanupStatus = [string]$report.cleanup.status
if ($queueStatus -eq 'PASS' -and $gameplayStatus -eq 'PASS' -and $cleanupStatus -eq 'PASS' -and $null -eq $applyError) {
    $report.overall_status = 'PASS'
}
elseif ($queueStatus -eq 'PASS' -and $cleanupStatus -eq 'PASS') {
    $report.overall_status = 'PARTIAL'
}
else {
    $report.overall_status = 'FAIL'
}
$report.completed_at_utc = [datetime]::UtcNow.ToString('o')
$report.report_path = $ReportPath
$reportJson = $report | ConvertTo-Json -Depth 24
[System.IO.File]::WriteAllText($ReportPath, $reportJson, [System.Text.UTF8Encoding]::new($false))

[pscustomobject]@{
    schema = $report.schema
    run_id = $runId
    overall_status = $report.overall_status
    queue_instance_status = $report.queue_instance.status
    gameplay_telemetry_status = $report.gameplay_telemetry.status
    cleanup_status = $report.cleanup.status
    instance_id = $report.queue_instance.instance_id
    final_score = $report.gameplay_telemetry.final_score
    winner = $report.gameplay_telemetry.winner
    report_path = $ReportPath
    errors = @($report.errors)
} | ConvertTo-Json -Depth 8
