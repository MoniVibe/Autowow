<#
.SYNOPSIS
    Capture one read-only, engine-authoritative quest-acquisition proof.

.DESCRIPTION
    This script is deliberately a proof capture, not a director or controller. It
    sends only these loopback bridge requests:

      snapshot <leader-guid>
      questlog <guid>
      acceptance <leader-guid> <quest-id>
      observe status <observer-guid>   (only when -ObserverGuid is supplied)

    It never issues quest, movement, recovery, party, teleport, database, or client
    commands. It writes one JSON document atomically below logs\proofs. The script
    can be dot-sourced: its pure normalization and grading functions are used by
    the offline Pester suite without contacting WSL or the bridge.

    `proven-live` is intentionally narrow. It requires an exact questlog entry for
    the requested quest with a recognized incomplete/complete status, complete
    acceptance invariants with zero teleport and direct quest-database mutation
    counters, matching response identities, and a stable WSL worldserver session
    across the capture. A screenshot is a witness only and never substitutes for
    questlog evidence.
#>
[CmdletBinding()]
param(
    [uint32]$LeaderGuid = 0,
    [uint32]$QuestId = 0,
    [uint32]$ObserverGuid = 0,
    [string]$OutputPath = '',
    [string]$ScreenshotPath = '',
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [ValidateSet('127.0.0.1')][string]$BridgeHost = '127.0.0.1',
    [ValidateRange(1, 65535)][int]$BridgePort = 18787,
    [ValidateRange(1000, 120000)][int]$BridgeTimeoutMs = 5000,
    [ValidatePattern('^[A-Za-z0-9._-]+$')][string]$WslDistro = 'Ubuntu-24.04',
    [ValidatePattern('^/[A-Za-z0-9._/-]+$')][string]$ModuleConfigPath = '/usr/local/etc/modules/playerbots.conf'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$script:IsDotSourced = ($MyInvocation.InvocationName -eq '.')

function Get-CaptureProperty {
    [CmdletBinding()]
    param(
        [AllowNull()][object]$Object,
        [Parameter(Mandatory = $true)][string[]]$Names,
        [AllowNull()][object]$Default = $null
    )

    if ($null -eq $Object) { return $Default }
    foreach ($name in $Names) {
        $property = $Object.PSObject.Properties[$name]
        if ($null -ne $property -and $null -ne $property.Value) {
            return $property.Value
        }
    }
    return $Default
}

function Get-CaptureNestedProperty {
    [CmdletBinding()]
    param(
        [AllowNull()][object]$Object,
        [Parameter(Mandatory = $true)][string]$Path,
        [AllowNull()][object]$Default = $null
    )

    $current = $Object
    foreach ($part in ($Path -split '\.')) {
        if ($null -eq $current) { return $Default }
        $property = $current.PSObject.Properties[$part]
        if ($null -eq $property) { return $Default }
        $current = $property.Value
    }
    if ($null -eq $current) { return $Default }
    return $current
}

function ConvertTo-CaptureBoolean {
    [CmdletBinding()]
    param([AllowNull()][object]$Value)

    if ($null -eq $Value) { return $null }
    if ($Value -is [bool]) { return [bool]$Value }
    $text = ([string]$Value).Trim()
    if ($text -match '^(?i:true|yes|on|1)$') { return $true }
    if ($text -match '^(?i:false|no|off|0)$') { return $false }
    return $null
}

function ConvertTo-CaptureInt64 {
    [CmdletBinding()]
    param([AllowNull()][object]$Value)

    if ($null -eq $Value -or $Value -is [bool]) { return $null }
    try { return [int64]$Value } catch { return $null }
}

function Get-CaptureResponseBot {
    [CmdletBinding()]
    param([AllowNull()][object]$Response)

    $bot = Get-CaptureProperty -Object $Response -Names @('bot')
    if ($null -ne $bot) { return $bot }
    return $Response
}

function Get-CaptureResponseGuid {
    [CmdletBinding()]
    param([AllowNull()][object]$Response)

    $bot = Get-CaptureResponseBot -Response $Response
    $guid = Get-CaptureProperty -Object $Response -Names @('guid', 'leader_guid', 'observer')
    if ($null -eq $guid) { $guid = Get-CaptureProperty -Object $bot -Names @('guid') }
    return (ConvertTo-CaptureInt64 -Value $guid)
}

function Test-CaptureResponseIdentity {
    [CmdletBinding()]
    param(
        [AllowNull()][object]$Response,
        [Parameter(Mandatory = $true)][uint32]$ExpectedGuid,
        [Parameter(Mandatory = $true)][string]$Surface
    )

    $observed = Get-CaptureResponseGuid -Response $Response
    $ok = $false
    $reason = "${Surface}_identity_missing"
    if ($null -ne $observed) {
        $ok = ([uint32]$observed -eq $ExpectedGuid)
        $reason = if ($ok) { '' } else { "${Surface}_identity_mismatch_expected_${ExpectedGuid}_observed_${observed}" }
    }
    [pscustomobject][ordered]@{
        surface = $Surface
        expected_guid = [uint32]$ExpectedGuid
        observed_guid = $observed
        valid = $ok
        reason = $reason
    }
}

function Get-CaptureQuestStatusName {
    [CmdletBinding()]
    param([AllowNull()][object]$Quest)

    if ($null -eq $Quest) { return 'unknown' }
    $status = Get-CaptureProperty -Object $Quest -Names @('status')
    if ($null -ne $status) {
        $statusText = ([string]$status).Trim().ToLowerInvariant()
        if ($statusText -in @('incomplete', 'active', 'accepted')) { return 'incomplete' }
        if ($statusText -in @('complete', 'completed', 'ready_to_reward')) { return 'complete' }
        $numericStatus = ConvertTo-CaptureInt64 -Value $status
        if ($null -ne $numericStatus) {
            # AzerothCore's live questlog surface uses 3 for incomplete and 1 for complete.
            if ($numericStatus -eq 3) { return 'incomplete' }
            if ($numericStatus -eq 1) { return 'complete' }
        }
    }

    $isComplete = ConvertTo-CaptureBoolean -Value (Get-CaptureProperty -Object $Quest -Names @('is_complete'))
    if ($isComplete -eq $true) { return 'complete' }
    if ($isComplete -eq $false) { return 'incomplete' }
    return 'unknown'
}

function Get-CaptureExactQuestAssessment {
    [CmdletBinding()]
    param(
        [AllowNull()][object]$QuestlogResponse,
        [Parameter(Mandatory = $true)][uint32]$QuestId,
        [Parameter(Mandatory = $true)][uint32]$Guid
    )

    $identity = Test-CaptureResponseIdentity -Response $QuestlogResponse -ExpectedGuid $Guid -Surface 'questlog'
    $ok = ConvertTo-CaptureBoolean -Value (Get-CaptureProperty -Object $QuestlogResponse -Names @('ok'))
    $quests = @((Get-CaptureProperty -Object $QuestlogResponse -Names @('quests') -Default @()))
    $matches = @($quests | Where-Object {
        $id = ConvertTo-CaptureInt64 -Value (Get-CaptureProperty -Object $_ -Names @('id', 'quest_id'))
        $null -ne $id -and [uint32]$id -eq $QuestId
    })

    $reasons = [System.Collections.Generic.List[string]]::new()
    if ($ok -ne $true) { $reasons.Add('questlog_response_not_ok') }
    if (-not $identity.valid) { $reasons.Add($identity.reason) }
    if ($matches.Count -eq 0) { $reasons.Add('exact_quest_missing_from_questlog') }
    if ($matches.Count -gt 1) { $reasons.Add('duplicate_exact_quest_entries') }

    $entry = if ($matches.Count -eq 1) { $matches[0] } else { $null }
    $status = Get-CaptureQuestStatusName -Quest $entry
    if ($null -ne $entry -and $status -eq 'unknown') { $reasons.Add('quest_status_not_incomplete_or_complete') }

    [pscustomobject][ordered]@{
        guid = [uint32]$Guid
        quest_id = [uint32]$QuestId
        response_ok = ($ok -eq $true)
        identity = $identity
        exact_entry_present = ($matches.Count -eq 1)
        status = $status
        status_accepted_for_proof = ($status -in @('incomplete', 'complete'))
        valid = ($reasons.Count -eq 0 -and $status -in @('incomplete', 'complete'))
        reasons = @($reasons)
        entry = $entry
        objectives = if ($null -ne $entry) { @((Get-CaptureProperty -Object $entry -Names @('objectives') -Default @())) } else { @() }
    }
}

function Get-CaptureCounterAssessment {
    [CmdletBinding()]
    param(
        [AllowNull()][object]$Object,
        [Parameter(Mandatory = $true)][string[]]$Names
    )

    $raw = Get-CaptureProperty -Object $Object -Names $Names
    $value = ConvertTo-CaptureInt64 -Value $raw
    [pscustomobject][ordered]@{
        present = ($null -ne $raw)
        valid = ($null -ne $value)
        value = $value
        field = if ($null -ne $raw) { ($Names | Where-Object { $null -ne (Get-CaptureProperty -Object $Object -Names @($_)) } | Select-Object -First 1) } else { $null }
    }
}

function Get-CaptureAcceptanceAssessment {
    [CmdletBinding()]
    param(
        [AllowNull()][object]$AcceptanceResponse,
        [Parameter(Mandatory = $true)][uint32]$LeaderGuid,
        [Parameter(Mandatory = $true)][uint32]$QuestId
    )

    $identity = Test-CaptureResponseIdentity -Response $AcceptanceResponse -ExpectedGuid $LeaderGuid -Surface 'acceptance'
    $ok = ConvertTo-CaptureBoolean -Value (Get-CaptureProperty -Object $AcceptanceResponse -Names @('ok'))
    $invariants = Get-CaptureProperty -Object $AcceptanceResponse -Names @('invariants')
    $teleport = Get-CaptureCounterAssessment -Object $invariants -Names @('teleport_count', 'teleports', 'teleport_counter')
    $dbMutation = Get-CaptureCounterAssessment -Object $invariants -Names @(
        'direct_quest_db_mutation_count',
        'direct_quest_database_mutation_count',
        'quest_db_mutation_count')
    $readOnly = ConvertTo-CaptureBoolean -Value (Get-CaptureProperty -Object $AcceptanceResponse -Names @('read_only'))

    $reasons = [System.Collections.Generic.List[string]]::new()
    if ($ok -ne $true) { $reasons.Add('acceptance_response_not_ok') }
    if (-not $identity.valid) { $reasons.Add($identity.reason) }
    if (-not $teleport.present) { $reasons.Add('teleport_counter_missing') }
    elseif (-not $teleport.valid) { $reasons.Add('teleport_counter_invalid') }
    elseif ($teleport.value -ne 0) { $reasons.Add("teleport_counter_nonzero_$($teleport.value)") }
    if (-not $dbMutation.present) { $reasons.Add('direct_quest_db_mutation_counter_missing') }
    elseif (-not $dbMutation.valid) { $reasons.Add('direct_quest_db_mutation_counter_invalid') }
    elseif ($dbMutation.value -ne 0) { $reasons.Add("direct_quest_db_mutation_counter_nonzero_$($dbMutation.value)") }
    if ($readOnly -eq $false) { $reasons.Add('acceptance_surface_not_read_only') }

    [pscustomobject][ordered]@{
        leader_guid = [uint32]$LeaderGuid
        quest_id = [uint32]$QuestId
        response_ok = ($ok -eq $true)
        identity = $identity
        read_only_reported = $readOnly
        invariants = $invariants
        teleport = $teleport
        direct_quest_db_mutation = $dbMutation
        zero_teleport = ($teleport.valid -and $teleport.value -eq 0)
        zero_direct_quest_db_mutation = ($dbMutation.valid -and $dbMutation.value -eq 0)
        valid = ($reasons.Count -eq 0)
        reasons = @($reasons)
        raw = $AcceptanceResponse
    }
}

function Get-CaptureGuidList {
    [CmdletBinding()]
    param([AllowNull()][object]$Value)

    $result = [System.Collections.Generic.List[uint32]]::new()
    foreach ($candidate in @($Value)) {
        $raw = if ($candidate -is [ValueType] -or $candidate -is [string]) {
            $candidate
        } else {
            Get-CaptureProperty -Object $candidate -Names @('guid', 'member_guid', 'player_guid')
        }
        $guid = ConvertTo-CaptureInt64 -Value $raw
        if ($null -ne $guid -and $guid -gt 0 -and $guid -le [uint32]::MaxValue -and -not $result.Contains([uint32]$guid)) {
            $result.Add([uint32]$guid)
        }
    }
    return ,@($result)
}

function Get-CaptureGroupRoster {
    [CmdletBinding()]
    param([AllowNull()][object]$SnapshotResponse)

    $bot = Get-CaptureResponseBot -Response $SnapshotResponse
    $group = Get-CaptureProperty -Object $bot -Names @('group')
    $groupCountRaw = Get-CaptureProperty -Object $group -Names @('members', 'member_count', 'count')
    $groupCount = ConvertTo-CaptureInt64 -Value $groupCountRaw
    if ($null -eq $groupCount) { $groupCount = 1 }

    $rosterValue = $null
    foreach ($path in @(
            'bot.group.member_guids', 'bot.group.members_guids', 'bot.group.guids', 'bot.group.roster.guids',
            'bot.group.roster.member_guids', 'group.member_guids', 'group.members_guids', 'group.guids',
            'group.roster.guids', 'group.roster.member_guids', 'member_guids', 'group_member_guids', 'members')) {
        $candidate = Get-CaptureNestedProperty -Object $SnapshotResponse -Path $path
        if ($null -ne $candidate -and @($candidate).Count -gt 0) {
            # A scalar members count is not a roster. Only object arrays or guid arrays qualify.
            if ($path -eq 'members' -and $candidate -is [ValueType]) { continue }
            $rosterValue = $candidate
            break
        }
    }
    $guidList = [System.Collections.Generic.List[uint32]]::new()
    if ($null -ne $rosterValue) {
        foreach ($guidCandidate in @(Get-CaptureGuidList -Value $rosterValue)) {
            foreach ($guid in @($guidCandidate)) {
                $parsedGuid = ConvertTo-CaptureInt64 -Value $guid
                if ($null -ne $parsedGuid -and $parsedGuid -gt 0 -and $parsedGuid -le [uint32]::MaxValue) {
                    $guidList.Add([uint32]$parsedGuid)
                }
            }
        }
    }
    $guids = [uint32[]]$guidList.ToArray()

    [pscustomobject][ordered]@{
        group = $group
        expected_count = [int]$groupCount
        guids = $guids
        guids_available = ($guids.Count -gt 0)
        multi_member = ([int]$groupCount -gt 1 -or $guids.Count -gt 1)
    }
}

function Get-CaptureMemberQuestlogResponse {
    [CmdletBinding()]
    param(
        [AllowNull()][object[]]$MemberQuestlogs,
        [Parameter(Mandatory = $true)][uint32]$Guid
    )

    foreach ($record in @($MemberQuestlogs)) {
        $recordGuid = ConvertTo-CaptureInt64 -Value (Get-CaptureProperty -Object $record -Names @('guid', 'member_guid'))
        if ($null -ne $recordGuid -and [uint32]$recordGuid -eq $Guid) {
            $response = Get-CaptureProperty -Object $record -Names @('response')
            if ($null -ne $response) { return $response }
            return $record
        }
    }
    return $null
}

function Get-CapturePartyAssessment {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][pscustomobject]$Roster,
        [Parameter(Mandatory = $true)][uint32]$LeaderGuid,
        [Parameter(Mandatory = $true)][pscustomobject]$LeaderQuest,
        [AllowNull()][object[]]$MemberQuestlogs
    )

    $reasons = [System.Collections.Generic.List[string]]::new()
    $evidence = [System.Collections.Generic.List[object]]::new()
    if (-not $Roster.multi_member) { $reasons.Add('no_multi_member_party') }
    if (-not $Roster.guids_available) { $reasons.Add('group_member_guids_unavailable') }
    elseif ($Roster.expected_count -gt 0 -and $Roster.expected_count -ne $Roster.guids.Count) {
        $reasons.Add("group_roster_incomplete_expected_$($Roster.expected_count)_observed_$($Roster.guids.Count)")
    }
    if ($Roster.guids_available -and -not ($Roster.guids -contains $LeaderGuid)) { $reasons.Add('leader_missing_from_group_roster') }

    foreach ($guid in @($Roster.guids)) {
        $assessment = if ($guid -eq $LeaderGuid) {
            $LeaderQuest
        } else {
            $response = Get-CaptureMemberQuestlogResponse -MemberQuestlogs $MemberQuestlogs -Guid $guid
            if ($null -eq $response) {
                $reasons.Add("member_questlog_missing_$guid")
                $null
            } else {
                Get-CaptureExactQuestAssessment -QuestlogResponse $response -QuestId $LeaderQuest.quest_id -Guid $guid
            }
        }
        $evidence.Add([pscustomobject][ordered]@{
            guid = [uint32]$guid
            exact_questlog = if ($null -ne $assessment) { [bool]$assessment.valid } else { $false }
            status = if ($null -ne $assessment) { $assessment.status } else { 'missing' }
            assessment = $assessment
        })
        if ($null -ne $assessment -and -not $assessment.valid) {
            foreach ($reason in @($assessment.reasons)) { $reasons.Add("member_${guid}_$reason") }
        }
    }

    [pscustomobject][ordered]@{
        claimed = ($reasons.Count -eq 0 -and $Roster.multi_member -and $Roster.guids_available)
        cohesive_party_acquisition = ($reasons.Count -eq 0 -and $Roster.multi_member -and $Roster.guids_available)
        expected_group_count = $Roster.expected_count
        expected_group_guids = @($Roster.guids)
        roster_guids_available = $Roster.guids_available
        evidence = @($evidence)
        valid = ($reasons.Count -eq 0 -and $Roster.multi_member -and $Roster.guids_available)
        reasons = @($reasons)
    }
}

function Test-CaptureRuntimeStability {
    [CmdletBinding()]
    param(
        [AllowNull()][object]$Before,
        [AllowNull()][object]$After
    )

    $reasons = [System.Collections.Generic.List[string]]::new()
    $beforeAvailable = (ConvertTo-CaptureBoolean (Get-CaptureProperty $Before @('available', 'worldserver_available')))
    $afterAvailable = (ConvertTo-CaptureBoolean (Get-CaptureProperty $After @('available', 'worldserver_available')))
    if ($beforeAvailable -ne $true) { $reasons.Add('runtime_before_unavailable') }
    if ($afterAvailable -ne $true) { $reasons.Add('runtime_after_unavailable') }

    $beforePid = ConvertTo-CaptureInt64 (Get-CaptureNestedProperty $Before 'session.pid')
    $afterPid = ConvertTo-CaptureInt64 (Get-CaptureNestedProperty $After 'session.pid')
    $beforeStart = [string](Get-CaptureNestedProperty $Before 'session.start_time_utc' '')
    $afterStart = [string](Get-CaptureNestedProperty $After 'session.start_time_utc' '')
    $beforePath = [string](Get-CaptureNestedProperty $Before 'worldserver.path' '')
    $afterPath = [string](Get-CaptureNestedProperty $After 'worldserver.path' '')
    $beforeHash = [string](Get-CaptureNestedProperty $Before 'worldserver.sha256' '')
    $afterHash = [string](Get-CaptureNestedProperty $After 'worldserver.sha256' '')

    foreach ($pair in @(
            @('runtime_before_pid_missing', $beforePid), @('runtime_after_pid_missing', $afterPid),
            @('runtime_before_start_time_missing', $beforeStart), @('runtime_after_start_time_missing', $afterStart),
            @('runtime_before_binary_path_missing', $beforePath), @('runtime_after_binary_path_missing', $afterPath),
            @('runtime_before_binary_hash_missing', $beforeHash), @('runtime_after_binary_hash_missing', $afterHash))) {
        if ($null -eq $pair[1] -or [string]::IsNullOrWhiteSpace([string]$pair[1]) -or $pair[1] -eq 0) { $reasons.Add([string]$pair[0]) }
    }
    if ($beforePid -ne $afterPid -or $beforeStart -cne $afterStart) { $reasons.Add('stale_worldserver_session_identity') }
    if ($beforePath -cne $afterPath -or $beforeHash -cne $afterHash) { $reasons.Add('worldserver_image_changed_during_capture') }

    [pscustomobject][ordered]@{
        stable = ($reasons.Count -eq 0)
        reasons = @($reasons)
        before = $Before
        after = $After
        session_match = ($beforePid -eq $afterPid -and $beforeStart -ceq $afterStart)
        image_match = ($beforePath -ceq $afterPath -and $beforeHash -ceq $afterHash)
    }
}

function Get-CaptureObserverAssessment {
    [CmdletBinding()]
    param(
        [AllowNull()][object]$ObserverResponse,
        [Parameter(Mandatory = $true)][uint32]$ObserverGuid,
        [AllowNull()][object]$LeaderSnapshot
    )

    $identity = Test-CaptureResponseIdentity -Response $ObserverResponse -ExpectedGuid $ObserverGuid -Surface 'observer'
    $ok = ConvertTo-CaptureBoolean (Get-CaptureProperty $ObserverResponse @('ok'))
    $observer = Get-CaptureProperty $ObserverResponse @('observer')
    $observerPosition = Get-CaptureProperty $observer @('position')
    if ($null -eq $observerPosition) { $observerPosition = Get-CaptureProperty $ObserverResponse @('position') }
    $leaderBot = Get-CaptureResponseBot -Response $LeaderSnapshot
    $leaderPosition = Get-CaptureProperty $leaderBot @('position')
    $leaderGuid = ConvertTo-CaptureInt64 (Get-CaptureProperty $leaderBot @('guid'))
    $watchedGuid = ConvertTo-CaptureInt64 (Get-CaptureProperty $ObserverResponse @('watched_leader_guid', 'watched_guid', 'leader_guid'))
    if ($null -eq $watchedGuid) {
        $watched = Get-CaptureProperty $ObserverResponse @('watched')
        $watchedGuid = ConvertTo-CaptureInt64 (Get-CaptureProperty $watched @('guid', 'leader_guid', 'leader'))
    }
    $observerMap = ConvertTo-CaptureInt64 (Get-CaptureProperty $observerPosition @('map', 'map_id'))
    $leaderMap = ConvertTo-CaptureInt64 (Get-CaptureProperty $leaderPosition @('map', 'map_id'))
    $watched = Get-CaptureProperty $ObserverResponse @('watched')
    $reportedSameMap = ConvertTo-CaptureBoolean (Get-CaptureProperty $watched @('same_map'))
    $sameMap = if ($null -ne $observerMap -and $null -ne $leaderMap) {
        ($observerMap -eq $leaderMap)
    } elseif ($null -ne $reportedSameMap) {
        $reportedSameMap
    } else {
        $false
    }
    $reasons = [System.Collections.Generic.List[string]]::new()
    if ($ok -ne $true) { $reasons.Add('observer_response_not_ok') }
    if (-not $identity.valid) { $reasons.Add($identity.reason) }
    if ($null -eq $watchedGuid) { $reasons.Add('observer_watched_leader_missing') }
    elseif ($null -eq $leaderGuid -or [uint32]$watchedGuid -ne [uint32]$leaderGuid) { $reasons.Add('observer_watched_leader_mismatch') }
    if (($null -eq $observerMap -or $null -eq $leaderMap) -and $null -eq $reportedSameMap) { $reasons.Add('observer_or_leader_map_missing') }

    [pscustomobject][ordered]@{
        enabled = $true
        observer_guid = [uint32]$ObserverGuid
        identity = $identity
        watched_leader_guid = $watchedGuid
        watched_leader_matches = ($null -ne $watchedGuid -and $null -ne $leaderGuid -and [uint32]$watchedGuid -eq [uint32]$leaderGuid)
        observer_position = $observerPosition
        leader_map = $leaderMap
        observer_map = $observerMap
        same_map = $sameMap
        valid = ($reasons.Count -eq 0)
        reasons = @($reasons)
        raw = $ObserverResponse
    }
}

function New-CaptureProofDocument {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][uint32]$LeaderGuid,
        [Parameter(Mandatory = $true)][uint32]$QuestId,
        [Parameter(Mandatory = $true)][object]$SnapshotResponse,
        [Parameter(Mandatory = $true)][object]$QuestlogResponse,
        [Parameter(Mandatory = $true)][object]$AcceptanceResponse,
        [AllowNull()][object[]]$MemberQuestlogs = @(),
        [AllowNull()][object]$ObserverResponse = $null,
        [uint32]$ObserverGuid = 0,
        [AllowNull()][object]$RuntimeBefore = $null,
        [AllowNull()][object]$RuntimeAfter = $null,
        [string]$ScreenshotPath = '',
        [string[]]$CaptureErrors = @(),
        [DateTimeOffset]$CapturedUtc = ([DateTimeOffset]::UtcNow)
    )

    $snapshotIdentity = Test-CaptureResponseIdentity -Response $SnapshotResponse -ExpectedGuid $LeaderGuid -Surface 'snapshot'
    $leaderQuest = Get-CaptureExactQuestAssessment -QuestlogResponse $QuestlogResponse -QuestId $QuestId -Guid $LeaderGuid
    $acceptance = Get-CaptureAcceptanceAssessment -AcceptanceResponse $AcceptanceResponse -LeaderGuid $LeaderGuid -QuestId $QuestId
    $runtime = Test-CaptureRuntimeStability -Before $RuntimeBefore -After $RuntimeAfter
    $roster = Get-CaptureGroupRoster -SnapshotResponse $SnapshotResponse
    $party = Get-CapturePartyAssessment -Roster $roster -LeaderGuid $LeaderGuid -LeaderQuest $leaderQuest -MemberQuestlogs $MemberQuestlogs

    $leaderBot = Get-CaptureResponseBot -Response $SnapshotResponse
    $leaderPosition = Get-CaptureProperty $leaderBot @('position')
    $leaderGroup = Get-CaptureProperty $leaderBot @('group')
    $leaderIdentity = [pscustomobject][ordered]@{
        guid = $LeaderGuid
        name = Get-CaptureProperty $leaderBot @('name')
        online = ConvertTo-CaptureBoolean (Get-CaptureProperty $leaderBot @('online'))
        alive = ConvertTo-CaptureBoolean (Get-CaptureProperty $leaderBot @('alive'))
        snapshot_identity = $snapshotIdentity
    }

    $reasons = [System.Collections.Generic.List[string]]::new()
    foreach ($reason in @($CaptureErrors)) { if (-not [string]::IsNullOrWhiteSpace($reason)) { $reasons.Add($reason) } }
    if (-not $snapshotIdentity.valid) { $reasons.Add($snapshotIdentity.reason) }
    foreach ($reason in @($leaderQuest.reasons)) { $reasons.Add($reason) }
    foreach ($reason in @($acceptance.reasons)) { $reasons.Add($reason) }
    foreach ($reason in @($runtime.reasons)) { $reasons.Add($reason) }

    $observer = if ($ObserverGuid -ne 0) {
        Get-CaptureObserverAssessment -ObserverResponse $ObserverResponse -ObserverGuid $ObserverGuid -LeaderSnapshot $SnapshotResponse
    } else {
        [pscustomobject][ordered]@{ enabled = $false; valid = $true; reasons = @(); watched_leader_matches = $null; same_map = $null; raw = $null }
    }
    if ($observer.enabled -and -not $observer.valid) { foreach ($reason in @($observer.reasons)) { $reasons.Add($reason) } }

    $leaderAcquisition = (
        $snapshotIdentity.valid -and $leaderQuest.valid -and $acceptance.valid -and $runtime.stable -and
        (-not $observer.enabled -or $observer.valid)
    )
    $grade = if ($leaderAcquisition) { 'proven-live' } else { 'unproven' }
    $screenshot = if ([string]::IsNullOrWhiteSpace($ScreenshotPath)) {
        [pscustomobject][ordered]@{ supplied = $false; path = $null; exists = $false; substitutes_for_questlog = $false }
    } else {
        [pscustomobject][ordered]@{ supplied = $true; path = [IO.Path]::GetFullPath($ScreenshotPath); exists = (Test-Path -LiteralPath $ScreenshotPath -PathType Leaf); substitutes_for_questlog = $false }
    }

    [pscustomobject][ordered]@{
        schema = 'autowow.quest.acquisition.proof.v1'
        schema_version = 1
        captured_utc = $CapturedUtc.ToUniversalTime().ToString('o')
        read_only = $true
        bridge = [pscustomobject][ordered]@{
            host = '127.0.0.1'
            allowed_verbs = @('snapshot', 'questlog', 'acceptance', 'observe status')
            mutation_verbs_issued = @()
            requests = @(
                "snapshot $LeaderGuid",
                "questlog $LeaderGuid",
                "acceptance $LeaderGuid $QuestId"
            ) + @($MemberQuestlogs | ForEach-Object { "questlog $($_.guid)" }) +
            $(if ($ObserverGuid -ne 0) { @("observe status $ObserverGuid") } else { @() })
        }
        runtime = [pscustomobject][ordered]@{
            before = $RuntimeBefore
            after = $RuntimeAfter
            stable_session = $runtime.stable
            session_match = $runtime.session_match
            image_match = $runtime.image_match
            stability_reasons = @($runtime.reasons)
        }
        leader = [pscustomobject][ordered]@{
            identity = $leaderIdentity
            position = $leaderPosition
            group = $leaderGroup
            snapshot = $SnapshotResponse
        }
        questlog = [pscustomobject][ordered]@{
            leader_response = $QuestlogResponse
            exact_requested_quest = $leaderQuest
        }
        acceptance = $acceptance
        party = $party
        observer = $observer
        witness = [pscustomobject][ordered]@{
            screenshot = $screenshot
            screenshot_is_not_questlog_evidence = $true
        }
        grading = [pscustomobject][ordered]@{
            mode = $grade
            grade = $grade
            proven_live = ($grade -eq 'proven-live')
            leader_acquisition = [bool]$leaderAcquisition
            cohesive_party_acquisition = [bool]$party.cohesive_party_acquisition
            acceptance_invariants_zero = [bool]($acceptance.zero_teleport -and $acceptance.zero_direct_quest_db_mutation)
            questlog_exact_status = $leaderQuest.status
            fail_closed = ($grade -ne 'proven-live')
            reasons = @($reasons | Select-Object -Unique)
            prohibited_inferences = @('screenshot', 'movement', 'xp', 'ok_true_alone', 'questlog_disappearance')
        }
    }
}

function Resolve-CaptureOutputPath {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$ServerRoot,
        [string]$OutputPath = '',
        [Parameter(Mandatory = $true)][uint32]$LeaderGuid,
        [Parameter(Mandatory = $true)][uint32]$QuestId
    )

    $proofRoot = [IO.Path]::GetFullPath((Join-Path $ServerRoot 'logs\proofs')).TrimEnd('\')
    $path = if ([string]::IsNullOrWhiteSpace($OutputPath)) {
        Join-Path $proofRoot ('live-quest-acquisition-g{0}-q{1}-{2}.json' -f $LeaderGuid, $QuestId, (Get-Date -Format 'yyyyMMdd-HHmmss'))
    } elseif ([IO.Path]::IsPathRooted($OutputPath)) {
        $OutputPath
    } else {
        Join-Path $proofRoot $OutputPath
    }
    $full = [IO.Path]::GetFullPath($path)
    $prefix = $proofRoot + [IO.Path]::DirectorySeparatorChar
    if (-not ([string]::Equals($full, $proofRoot, [StringComparison]::OrdinalIgnoreCase) -or
            $full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase))) {
        throw "Proof output must remain below logs\proofs: $full"
    }
    if ([string]::Equals($full, $proofRoot, [StringComparison]::OrdinalIgnoreCase) -or
        (Test-Path -LiteralPath $full -PathType Container)) {
        throw "Proof output must be a file below logs\proofs: $full"
    }
    return $full
}

function Write-CaptureJsonAtomic {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][object]$Document
    )

    $directory = Split-Path -Parent $Path
    [IO.Directory]::CreateDirectory($directory) | Out-Null
    $temporary = "$Path.$([guid]::NewGuid().ToString('N')).tmp"
    try {
        [IO.File]::WriteAllText($temporary, ($Document | ConvertTo-Json -Depth 100), [Text.UTF8Encoding]::new($false))
        [IO.File]::Move($temporary, $Path, $true)
    }
    finally {
        if (Test-Path -LiteralPath $temporary -PathType Leaf) { Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue }
    }
    return $Path
}

function New-CaptureWslRuntimeProbeScript {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][ValidatePattern('^/[A-Za-z0-9._/-]+$')][string]$ModuleConfigPath
    )

    $probe = @'
set +e
worldserver_pids=$(pgrep -x worldserver 2>/dev/null)
count=$(printf '%s\n' "$worldserver_pids" | awk 'NF {n++} END {print n+0}')
if [ "$count" -ne 1 ]; then
  printf 'RUNTIME_ERROR\tworldserver_process_count_%s\n' "$count"
else
  worldserver_pid=$(printf '%s\n' "$worldserver_pids" | awk 'NF {print; exit}')
  exe=$(readlink -f "/proc/$worldserver_pid/exe" 2>/dev/null)
  sha=$(sha256sum -- "$exe" 2>/dev/null | awk '{print $1}')
  ticks=$(awk '{print $22}' "/proc/$worldserver_pid/stat" 2>/dev/null)
  btime=$(awk '$1 == "btime" {print $2; exit}' /proc/stat 2>/dev/null)
  hz=$(getconf CLK_TCK 2>/dev/null)
  start=''
  if [ -n "$ticks" ] && [ -n "$btime" ] && [ -n "$hz" ]; then
    start=$(awk -v b="$btime" -v t="$ticks" -v h="$hz" 'BEGIN {printf "%.6f", b + (t / h)}')
  fi
  printf 'WORLD_PID\t%s\n' "$worldserver_pid"
  printf 'WORLD_PATH\t%s\n' "$exe"
  printf 'WORLD_SHA256\t%s\n' "$sha"
  printf 'WORLD_START_EPOCH\t%s\n' "$start"
fi
module='__MODULE_CONFIG_PATH__'
if [ -f "$module" ]; then
  printf 'MODULE_EXISTS\t1\n'
  printf 'MODULE_SHA256\t%s\n' "$(sha256sum -- "$module" 2>/dev/null | awk '{print $1}')"
else
  printf 'MODULE_EXISTS\t0\n'
  printf 'RUNTIME_ERROR\tmodule_config_missing_%s\n' "$module"
fi
'@
    # ModuleConfigPath is restricted to slash-safe path characters, so embedding it in
    # single quotes cannot introduce shell syntax. The complete rendered probe is
    # base64-encoded by the invocation builder before it crosses wsl.exe.
    return $probe.Replace('__MODULE_CONFIG_PATH__', $ModuleConfigPath)
}

function New-CaptureWslRuntimeProbeInvocation {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][ValidatePattern('^[A-Za-z0-9._-]+$')][string]$Distro,
        [Parameter(Mandatory = $true)][ValidatePattern('^/[A-Za-z0-9._/-]+$')][string]$ModuleConfigPath
    )

    $probe = New-CaptureWslRuntimeProbeScript -ModuleConfigPath $ModuleConfigPath
    $probeBytes = [System.Text.Encoding]::UTF8.GetBytes([string]$probe)
    $encodedProbe = [Convert]::ToBase64String($probeBytes)
    $bashCommand = "echo $encodedProbe | base64 -d | bash"
    [pscustomobject][ordered]@{
        executable = 'wsl.exe'
        arguments = @('--distribution', $Distro, '--', 'bash', '-lc', $bashCommand)
        probe = $probe
        probe_base64 = $encodedProbe
        bash_command = $bashCommand
        module_config_path = $ModuleConfigPath
    }
}

function ConvertFrom-CaptureWslRuntimeOutput {
    [CmdletBinding()]
    param(
        [AllowNull()][object[]]$RawLines = @(),
        [Parameter(Mandatory = $true)][string]$Distro,
        [Parameter(Mandatory = $true)][ValidatePattern('^/[A-Za-z0-9._/-]+$')][string]$ModuleConfigPath,
        [int]$ExitCode = 0
    )

    $worldserverPid = $null; $path = ''; $hash = ''; $startEpoch = $null; $moduleExists = $false; $moduleHash = ''
    $errors = [System.Collections.Generic.List[string]]::new()
    foreach ($lineObject in @($RawLines)) {
        $line = [string]$lineObject
        $parts = $line -split "`t", 2
        if ($parts.Count -ne 2) { continue }
        switch ($parts[0]) {
            'WORLD_PID' { $worldserverPid = ConvertTo-CaptureInt64 $parts[1] }
            'WORLD_PATH' { $path = $parts[1] }
            'WORLD_SHA256' { $hash = $parts[1] }
            'WORLD_START_EPOCH' { $startEpoch = $parts[1] }
            'MODULE_EXISTS' { $moduleExists = ($parts[1] -eq '1') }
            'MODULE_SHA256' { $moduleHash = $parts[1] }
            'RUNTIME_ERROR' { $errors.Add($parts[1]) }
        }
    }
    if ($ExitCode -ne 0) { $errors.Add("wsl_probe_exit_$ExitCode") }
    $startUtc = ''
    if (-not [string]::IsNullOrWhiteSpace([string]$startEpoch)) {
        try {
            $seconds = [double]$startEpoch
            $startUtc = [DateTimeOffset]::FromUnixTimeMilliseconds([int64][Math]::Round($seconds * 1000.0)).ToUniversalTime().ToString('o')
        } catch { $errors.Add('worldserver_start_time_unparseable') }
    }
    $worldAvailable = ($null -ne $worldserverPid -and $worldserverPid -gt 0 -and -not [string]::IsNullOrWhiteSpace($path) -and
        $hash -match '^[0-9a-fA-F]{64}$')
    $sessionAvailable = ($worldAvailable -and -not [string]::IsNullOrWhiteSpace($startUtc))
    [pscustomobject][ordered]@{
        captured_utc = [DateTimeOffset]::UtcNow.ToString('o')
        distro = $Distro
        available = $worldAvailable
        session_identity_available = $sessionAvailable
        worldserver = [pscustomobject][ordered]@{
            pid = $worldserverPid
            path = $path
            sha256 = if ([string]::IsNullOrWhiteSpace($hash)) { $null } else { $hash.ToLowerInvariant() }
        }
        session = [pscustomobject][ordered]@{
            pid = $worldserverPid
            start_time_utc = if ([string]::IsNullOrWhiteSpace($startUtc)) { $null } else { $startUtc }
        }
        module_config = [pscustomobject][ordered]@{
            path = $ModuleConfigPath
            exists = $moduleExists
            sha256 = if ([string]::IsNullOrWhiteSpace($moduleHash)) { $null } else { $moduleHash.ToLowerInvariant() }
        }
        errors = @($errors)
        probe_exit_code = $ExitCode
    }
}

function Invoke-CaptureWslRuntimeProbe {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Distro,
        [Parameter(Mandatory = $true)][ValidatePattern('^/[A-Za-z0-9._/-]+$')][string]$ModuleConfigPath
    )

    $invocation = New-CaptureWslRuntimeProbeInvocation -Distro $Distro -ModuleConfigPath $ModuleConfigPath
    $oldErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        # Encode the complete probe so Windows/wsl.exe quote parsing cannot break its
        # awk, printf, or module-path shell fragments. The bash command itself contains
        # only base64-safe text and a simple decode-and-execute pipeline.
        $raw = & $invocation.executable @($invocation.arguments) 2>&1
        $exitCode = $LASTEXITCODE
    }
    catch {
        $raw = @($_.Exception.Message)
        $exitCode = 1
    }
    finally { $ErrorActionPreference = $oldErrorActionPreference }
    return (ConvertFrom-CaptureWslRuntimeOutput -RawLines @($raw) -Distro $Distro `
        -ModuleConfigPath $ModuleConfigPath -ExitCode $exitCode)
}

function Invoke-CaptureReadOnlyBridge {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Request,
        [ValidateSet('127.0.0.1')][string]$BridgeHost = '127.0.0.1',
        [ValidateRange(1, 65535)][int]$Port = 18787,
        [ValidateRange(1000, 120000)][int]$TimeoutMs = 5000
    )

    if ($Request -notmatch '^(?:snapshot|questlog) [1-9][0-9]*$' -and
        $Request -notmatch '^acceptance [1-9][0-9]* [1-9][0-9]*$' -and
        $Request -notmatch '^observe status [1-9][0-9]*$') {
        throw "Refusing non-read-only bridge request: $Request"
    }
    $client = [System.Net.Sockets.TcpClient]::new()
    try {
        $connect = $client.ConnectAsync($BridgeHost, $Port)
        if (-not $connect.Wait([Math]::Min($TimeoutMs, 10000)) -or -not $client.Connected) {
            throw "Could not connect to the AutoWow bridge at ${BridgeHost}:$Port."
        }
        $stream = $client.GetStream()
        $stream.ReadTimeout = $TimeoutMs
        $writer = [IO.StreamWriter]::new($stream)
        $writer.NewLine = "`n"
        $writer.WriteLine($Request)
        $writer.Flush()
        $reader = [IO.StreamReader]::new($stream)
        $line = $reader.ReadLine()
        if ([string]::IsNullOrWhiteSpace($line)) { throw "Bridge returned an empty response for '$Request'." }
        return ($line | ConvertFrom-Json -Depth 100)
    }
    finally { $client.Dispose() }
}

function Invoke-CaptureLiveQuestAcquisitionProof {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][ValidateRange(1, [uint32]::MaxValue)][uint32]$LeaderGuid,
        [Parameter(Mandatory = $true)][ValidateRange(1, [uint32]::MaxValue)][uint32]$QuestId,
        [uint32]$ObserverGuid = 0,
        [string]$OutputPath = '',
        [string]$ScreenshotPath = '',
        [Parameter(Mandatory = $true)][string]$ServerRoot,
        [ValidateSet('127.0.0.1')][string]$BridgeHost = '127.0.0.1',
        [ValidateRange(1, 65535)][int]$BridgePort = 18787,
        [ValidateRange(1000, 120000)][int]$BridgeTimeoutMs = 5000,
        [ValidatePattern('^[A-Za-z0-9._-]+$')][string]$WslDistro = 'Ubuntu-24.04',
        [ValidatePattern('^/[A-Za-z0-9._/-]+$')][string]$ModuleConfigPath = '/usr/local/etc/modules/playerbots.conf'
    )

    $output = Resolve-CaptureOutputPath -ServerRoot $ServerRoot -OutputPath $OutputPath -LeaderGuid $LeaderGuid -QuestId $QuestId
    $errors = [System.Collections.Generic.List[string]]::new()
    $runtimeBefore = Invoke-CaptureWslRuntimeProbe -Distro $WslDistro -ModuleConfigPath $ModuleConfigPath
    $snapshot = $null; $questlog = $null; $acceptance = $null; $observer = $null
    $memberQuestlogs = [System.Collections.Generic.List[object]]::new()

    try { $snapshot = Invoke-CaptureReadOnlyBridge -Request "snapshot $LeaderGuid" -BridgeHost $BridgeHost -Port $BridgePort -TimeoutMs $BridgeTimeoutMs }
    catch { $errors.Add("snapshot_read_failed:$($_.Exception.Message)") }
    try { $questlog = Invoke-CaptureReadOnlyBridge -Request "questlog $LeaderGuid" -BridgeHost $BridgeHost -Port $BridgePort -TimeoutMs $BridgeTimeoutMs }
    catch { $errors.Add("questlog_read_failed:$($_.Exception.Message)") }
    try { $acceptance = Invoke-CaptureReadOnlyBridge -Request "acceptance $LeaderGuid $QuestId" -BridgeHost $BridgeHost -Port $BridgePort -TimeoutMs $BridgeTimeoutMs }
    catch { $errors.Add("acceptance_read_failed:$($_.Exception.Message)") }

    $roster = if ($null -ne $snapshot) { Get-CaptureGroupRoster -SnapshotResponse $snapshot } else { $null }
    if ($null -ne $roster -and $roster.guids_available) {
        foreach ($memberGuid in @($roster.guids | Where-Object { $_ -ne $LeaderGuid })) {
            try {
                $memberResponse = Invoke-CaptureReadOnlyBridge -Request "questlog $memberGuid" -BridgeHost $BridgeHost -Port $BridgePort -TimeoutMs $BridgeTimeoutMs
                $memberQuestlogs.Add([pscustomobject][ordered]@{ guid = [uint32]$memberGuid; response = $memberResponse })
            }
            catch { $errors.Add("member_questlog_read_failed_${memberGuid}:$($_.Exception.Message)") }
        }
    }
    if ($ObserverGuid -ne 0) {
        try { $observer = Invoke-CaptureReadOnlyBridge -Request "observe status $ObserverGuid" -BridgeHost $BridgeHost -Port $BridgePort -TimeoutMs $BridgeTimeoutMs }
        catch { $errors.Add("observer_status_read_failed:$($_.Exception.Message)") }
    }

    $runtimeAfter = Invoke-CaptureWslRuntimeProbe -Distro $WslDistro -ModuleConfigPath $ModuleConfigPath
    $document = New-CaptureProofDocument -LeaderGuid $LeaderGuid -QuestId $QuestId `
        -SnapshotResponse $(if ($null -ne $snapshot) { $snapshot } else { [pscustomobject]@{} }) `
        -QuestlogResponse $(if ($null -ne $questlog) { $questlog } else { [pscustomobject]@{} }) `
        -AcceptanceResponse $(if ($null -ne $acceptance) { $acceptance } else { [pscustomobject]@{} }) `
        -MemberQuestlogs @($memberQuestlogs) -ObserverResponse $observer -ObserverGuid $ObserverGuid -RuntimeBefore $runtimeBefore -RuntimeAfter $runtimeAfter `
        -ScreenshotPath $ScreenshotPath -CaptureErrors @($errors)
    [void](Write-CaptureJsonAtomic -Path $output -Document $document)
    return $document
}

if (-not $script:IsDotSourced) {
    if ($LeaderGuid -eq 0) { throw 'LeaderGuid is required and must be positive.' }
    if ($QuestId -eq 0) { throw 'QuestId is required and must be positive.' }
    $document = Invoke-CaptureLiveQuestAcquisitionProof -LeaderGuid $LeaderGuid -QuestId $QuestId `
        -ObserverGuid $ObserverGuid -OutputPath $OutputPath -ScreenshotPath $ScreenshotPath -ServerRoot $ServerRoot `
        -BridgeHost $BridgeHost -BridgePort $BridgePort -BridgeTimeoutMs $BridgeTimeoutMs -WslDistro $WslDistro `
        -ModuleConfigPath $ModuleConfigPath
    Write-Output ($document | ConvertTo-Json -Depth 100)
}
