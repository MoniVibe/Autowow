<#
    QuestDirectorLib.ps1 — pure selection / classification / progress functions for Quest Director V1.

    No side effects, no I/O, no bridge, no database. Dot-source it from the director and from the
    offline tests. These functions encode the deterministic V1 policy:
      - support live-proven talk / kill / loot / creature-targeted item-use / turn-in objectives;
      - choose turn-ins first, then supported objectives with the greatest progress ratio, then the
        lowest quest id;
      - classify everything else as an explicit unsupported state.
#>

# Mirror of ObserverControl/QuestLogView C++ classification so the bridge questlog path and the
# read-only DB fallback agree. Supported classes: talk, kill, loot, item_use, turnin.
function Get-CapabilityClass {
    param(
        [int]$QuestType,
        [int]$SuggestedPlayers,
        [int]$SrcItem,
        [bool]$HasNpc,
        [bool]$HasGo,
        [bool]$HasItem,
        [bool]$IsComplete
    )
    if ($IsComplete) { return 'turnin' }
    # Match the engine's group/elite gate (NewRpgBaseAction::IsQuestCapableDoing): rejected only when
    # BOTH the quest carries a non-normal type AND it suggests 2+ players. Normal WotLK quests have
    # QuestType 2 with SuggestedPlayers 0, so the AND is essential to avoid flagging every quest.
    if ($QuestType -ne 0 -and $SuggestedPlayers -ge 2) { return 'dungeon_group' }
    if ($SrcItem -ne 0 -and $HasNpc) { return 'item_use' }
    if ($HasGo) { return 'gameobject' }
    if ($HasNpc) { return 'kill' }
    if ($HasItem) { return 'loot' }
    return 'talk'
}

function Test-CapabilitySupported {
    param([string]$Class)
    return $Class -in @('talk', 'kill', 'loot', 'item_use', 'turnin')
}

# Parse engine quest-abandonment lines from a chunk of Playerbots.log text (read-only signal).
# The New-RPG engine emits `[New RPG] <BotName> marked as abandoned quest <id>` when it gives up on a
# quest (POI reached, no objective progression). Returns @( @{ guid; quest } ) for known leaders.
function Get-AbandonmentsFromText {
    param([string]$Text, [hashtable]$NameToGuid)
    $result = @()
    if ([string]::IsNullOrEmpty($Text) -or $null -eq $NameToGuid) { return $result }
    foreach ($m in [regex]::Matches($Text, '\[New RPG\]\s+(?<name>\S+)\s+marked as abandoned quest\s+(?<q>\d+)')) {
        $nm = $m.Groups['name'].Value; $q = [int]$m.Groups['q'].Value
        if ($NameToGuid.ContainsKey($nm)) { $result += @{ guid = [uint32]$NameToGuid[$nm]; quest = $q } }
    }
    return $result
}

# Progress ratio in [0,1]: sum(min(current,required)) / sum(required). A quest with no counted
# objective (talk/report) is treated as ready (1.0).
function Get-ObjectiveProgressRatio {
    param([object[]]$Objectives)
    if (-not $Objectives -or @($Objectives).Count -eq 0) { return 1.0 }
    $req = 0; $have = 0
    foreach ($o in $Objectives) {
        $r = [int]$o.required; $c = [int]$o.current
        if ($r -gt 0) { $req += $r; $have += [Math]::Min($c, $r) }
    }
    if ($req -eq 0) { return 1.0 }
    return [double]$have / [double]$req
}

function Get-QuestDirectorProperty {
    param(
        [AllowNull()][object]$Object,
        [Parameter(Mandatory = $true)][string]$Name,
        [AllowNull()][object]$Default = $null
    )
    if ($null -eq $Object) { return $Default }
    if ($Object -is [System.Collections.IDictionary] -and $Object.Contains($Name)) {
        $value = $Object[$Name]
        if ($null -ne $value) { return $value }
        return $Default
    }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property -or $null -eq $property.Value) { return $Default }
    return $property.Value
}

# Reduce the read-only questobjective response to the small finisher surface the director needs.
# Missing, malformed, or non-finite distance data remains unavailable rather than becoming zero.
function ConvertTo-DirectorFinisherObservation {
    param([AllowNull()][object]$Response)

    $result = [ordered]@{
        available = $false
        quest_id = 0
        phase = ''
        distance = $null
        directive_active = $false
        directive_quest_id = 0
    }
    if (-not [bool](Get-QuestDirectorProperty -Object $Response -Name 'ok' -Default $false)) {
        return [pscustomobject]$result
    }

    $objective = Get-QuestDirectorProperty -Object $Response -Name 'objective' -Default $null
    if ($null -eq $objective) { return [pscustomobject]$result }
    $result.quest_id = [int](Get-QuestDirectorProperty -Object $objective -Name 'quest_id' -Default 0)
    $result.phase = [string](Get-QuestDirectorProperty -Object $objective -Name 'phase' -Default '')
    $result.directive_active = [bool](Get-QuestDirectorProperty -Object $Response -Name 'directive_active' -Default $false)
    $result.directive_quest_id = [int](Get-QuestDirectorProperty -Object $Response -Name 'directive_quest_id' -Default 0)

    $finisher = Get-QuestDirectorProperty -Object $objective -Name 'finisher_live' -Default $null
    $finisherLoaded = [bool](Get-QuestDirectorProperty -Object $finisher -Name 'loaded' -Default $false)
    $rawDistance = if ($finisherLoaded) { Get-QuestDirectorProperty -Object $finisher -Name 'distance' -Default $null } else { $null }
    if ($null -ne $rawDistance -and -not [string]::IsNullOrWhiteSpace([string]$rawDistance)) {
        try {
            $distance = [Convert]::ToDouble($rawDistance, [Globalization.CultureInfo]::InvariantCulture)
            if (-not [double]::IsNaN($distance) -and -not [double]::IsInfinity($distance) -and $distance -ge 0) {
                $result.distance = $distance
            }
        }
        catch { }
    }
    $result.available = $result.quest_id -gt 0
    return [pscustomobject]$result
}

function Get-FinisherPhaseRank {
    param([AllowNull()][string]$Phase)
    switch ([string]$Phase) {
        'travel_to_finisher' { return 1 }
        'interact_finisher' { return 2 }
        'verify_reward' { return 3 }
        default { return 0 }
    }
}

# Objective work stays counter-centric. Turn-in work advances only when the finisher state moves
# forward by a known phase or by at least the bounded distance epsilon from the last progress anchor.
function Get-DirectorQuestProgressDecision {
    param(
        [ValidateSet('objective','turnin')][string]$DirectorPhase,
        [int]$PreviousObjectiveSum,
        [int]$CurrentObjectiveSum,
        [AllowNull()][string]$PreviousFinisherPhase = '',
        [AllowNull()][string]$CurrentFinisherPhase = '',
        [AllowNull()][object]$PreviousFinisherDistance = $null,
        [AllowNull()][object]$CurrentFinisherDistance = $null,
        [ValidateRange(0.1,100.0)][double]$FinisherDistanceEpsilon = 5.0
    )
    if ($DirectorPhase -eq 'objective') {
        if ($CurrentObjectiveSum -gt $PreviousObjectiveSum) {
            return [pscustomobject]@{ made_progress = $true; reason = 'objective_counter_advanced' }
        }
        return [pscustomobject]@{ made_progress = $false; reason = 'objective_counter_flat' }
    }

    $previousRank = Get-FinisherPhaseRank -Phase $PreviousFinisherPhase
    $currentRank = Get-FinisherPhaseRank -Phase $CurrentFinisherPhase
    if ($previousRank -gt 0 -and $currentRank -gt $previousRank) {
        return [pscustomobject]@{ made_progress = $true; reason = 'finisher_phase_advanced' }
    }

    if ($null -ne $PreviousFinisherDistance -and $null -ne $CurrentFinisherDistance -and
        -not [string]::IsNullOrWhiteSpace([string]$PreviousFinisherDistance) -and
        -not [string]::IsNullOrWhiteSpace([string]$CurrentFinisherDistance)) {
        try {
            $previousDistance = [Convert]::ToDouble($PreviousFinisherDistance, [Globalization.CultureInfo]::InvariantCulture)
            $currentDistance = [Convert]::ToDouble($CurrentFinisherDistance, [Globalization.CultureInfo]::InvariantCulture)
            $finite = -not [double]::IsNaN($previousDistance) -and -not [double]::IsInfinity($previousDistance) -and
                -not [double]::IsNaN($currentDistance) -and -not [double]::IsInfinity($currentDistance)
            if ($finite -and ($previousDistance - $currentDistance) -ge $FinisherDistanceEpsilon) {
                return [pscustomobject]@{ made_progress = $true; reason = 'finisher_distance_decreased' }
            }
        }
        catch { }
    }
    return [pscustomobject]@{ made_progress = $false; reason = 'finisher_progress_flat' }
}

# Deterministic selection. $Quests is an array of normalized quest objects (see QuestLogSource.ps1):
#   id, is_complete(bool), supported(bool), capability_class(string), objectives(array of {required,current})
# $BackoffKeys is a HashSet[string] of "<leaderGuid>:<questId>" currently in backoff.
# Returns: action('issue'|'idle'), quest_id, phase('turnin'|'objective'|''), capability_class,
#          progress_ratio, reason, detail.
function Select-DirectorQuest {
    param(
        [object[]]$Quests,
        [System.Collections.Generic.HashSet[string]]$BackoffKeys,
        [uint32]$LeaderGuid
    )
    $result = [ordered]@{ action = 'idle'; quest_id = 0; phase = ''; capability_class = ''; progress_ratio = 0.0; reason = ''; detail = '' }
    $active = @($Quests | Where-Object { $_ -ne $null })
    if ($active.Count -eq 0) { $result.reason = 'no_active_quest'; return [pscustomobject]$result }

    if ($null -eq $BackoffKeys) { $BackoffKeys = [System.Collections.Generic.HashSet[string]]::new() }
    $isBacked = { param($q) $BackoffKeys.Contains(("{0}:{1}" -f [uint32]$LeaderGuid, [int]$q.id)) }

    $notBacked = @($active | Where-Object { -not (& $isBacked $_) })

    # 1. Turn-ins first (lowest id among supported completes).
    $turnins = @($notBacked | Where-Object { $_.is_complete -and $_.supported } | Sort-Object { [int]$_.id })
    if ($turnins.Count -gt 0) {
        $q = $turnins[0]
        $result.action = 'issue'; $result.quest_id = [int]$q.id; $result.phase = 'turnin'
        $result.capability_class = [string]$q.capability_class; $result.progress_ratio = 1.0; $result.reason = 'turnin_ready'
        return [pscustomobject]$result
    }

    # 2. Supported objectives: greatest progress ratio, then lowest id.
    $objq = @($notBacked | Where-Object { -not $_.is_complete -and $_.supported })
    if ($objq.Count -gt 0) {
        $ranked = $objq | Sort-Object `
            @{ Expression = { [double](Get-ObjectiveProgressRatio $_.objectives) }; Descending = $true }, `
            @{ Expression = { [int]$_.id }; Descending = $false }
        $q = @($ranked)[0]
        $result.action = 'issue'; $result.quest_id = [int]$q.id; $result.phase = 'objective'
        $result.capability_class = [string]$q.capability_class
        $result.progress_ratio = [double](Get-ObjectiveProgressRatio $q.objectives); $result.reason = 'best_supported_objective'
        return [pscustomobject]$result
    }

    # 3. Idle, with an explicit reason.
    $supportedAll = @($active | Where-Object { $_.supported })
    if ($supportedAll.Count -gt 0) {
        $result.reason = 'all_supported_backed_off'
    }
    else {
        $classes = (@($active | ForEach-Object { [string]$_.capability_class }) | Sort-Object -Unique) -join ','
        $result.reason = 'unsupported_only'; $result.detail = $classes
    }
    return [pscustomobject]$result
}

# Stable quest-log generation used to issue acquisition once for a given observed log. Objective
# counters are included so a genuinely changed log can replan; object/property enumeration order is
# not trusted.
function Get-QuestLogGeneration {
    param([object[]]$Quests)
    $parts = @()
    foreach ($quest in @($Quests | Where-Object { $null -ne $_ } | Sort-Object { [int]$_.id })) {
        $objectives = @()
        $slot = 0
        foreach ($objective in @($quest.objectives)) {
            $objectives += ('{0}:{1}:{2}:{3}' -f $slot, [string]$objective.kind, [int]$objective.required, [int]$objective.current)
            $slot++
        }
        $parts += ('{0}:{1}:{2}:{3}:{4}:[{5}]' -f [int]$quest.id, [int]$quest.status,
            [bool]$quest.is_complete, [bool]$quest.supported, [string]$quest.capability_class,
            ($objectives -join ','))
    }
    return ($parts -join '|')
}

# Resolve the quest that a future dead/combat defer may conservatively be associated with. Prefer
# the current deterministic selection; otherwise retain the last-issued quest only while it is
# still present in the observed quest log. The generation lets callers distinguish log changes.
function Get-DeathOrCombatQuestContext {
    param(
        [int]$SelectedQuestId,
        [string]$LastIssued,
        [object[]]$Quests
    )
    $knownQuestIds = [System.Collections.Generic.HashSet[int]]::new()
    foreach ($quest in @($Quests | Where-Object { $null -ne $_ })) {
        if ([int]$quest.id -gt 0) { [void]$knownQuestIds.Add([int]$quest.id) }
    }

    $questId = 0
    if ($SelectedQuestId -gt 0 -and $knownQuestIds.Contains($SelectedQuestId)) {
        $questId = $SelectedQuestId
    }
    elseif (-not [string]::IsNullOrWhiteSpace($LastIssued)) {
        [int]$lastIssuedQuestId = 0
        $idToken = @($LastIssued -split ':', 2)[0]
        if ([int]::TryParse($idToken, [ref]$lastIssuedQuestId) -and $lastIssuedQuestId -gt 0 -and
            $knownQuestIds.Contains($lastIssuedQuestId)) {
            $questId = $lastIssuedQuestId
        }
    }

    return [pscustomobject]@{
        quest_id = $questId
        generation = Get-QuestLogGeneration -Quests $Quests
    }
}

# A zero override derives the prolonged-defer threshold from the existing no-progress policy. The
# five-minute floor keeps ordinary combat and routine corpse runs in the defer-only path.
function Get-DeathOrCombatStallThresholdSeconds {
    param(
        [ValidateRange(1,86400)][int]$NoProgressSeconds,
        [ValidateRange(0,86400)][int]$ConfiguredSeconds = 0
    )
    $safeFloorSeconds = 300
    $requestedSeconds = if ($ConfiguredSeconds -gt 0) {
        $ConfiguredSeconds
    }
    else {
        [Math]::Min(86400, ($NoProgressSeconds * 2))
    }
    return [int][Math]::Max($safeFloorSeconds, $requestedSeconds)
}

# Pure state transition for continuous leader death/combat defers. Callers own the returned state
# and perform any ledger I/O only when action=backoff. A healthy cycle or changed quest/generation
# resets the timer; an unknown quest can never produce a backoff decision.
function Get-DeathOrCombatStallDecision {
    param(
        [AllowNull()][object]$PreviousState,
        [bool]$IsDeferred,
        [int]$QuestId,
        [string]$Generation,
        [datetime]$NowUtc,
        [ValidateRange(1,86400)][int]$ThresholdSeconds
    )
    $now = $NowUtc.ToUniversalTime()
    if (-not $IsDeferred) {
        return [pscustomobject]@{
            action = 'reset'; state = $null; quest_id = 0; generation = ''
            deferred_seconds = 0; reason = 'healthy_cycle'
        }
    }
    if ($QuestId -le 0) {
        return [pscustomobject]@{
            action = 'defer'; state = $null; quest_id = 0; generation = [string]$Generation
            deferred_seconds = 0; reason = 'no_known_quest'
        }
    }

    $generationKey = [string]$Generation
    $sameContext = $null -ne $PreviousState -and [int]$PreviousState.quest_id -eq $QuestId -and
        [string]$PreviousState.generation -eq $generationKey
    $started = $now
    if ($sameContext) {
        try { $started = ([datetime]$PreviousState.started_utc).ToUniversalTime() }
        catch { $sameContext = $false; $started = $now }
    }

    $state = [pscustomobject]@{
        quest_id = $QuestId
        generation = $generationKey
        started_utc = $started
    }
    if (-not $sameContext) {
        return [pscustomobject]@{
            action = 'defer'; state = $state; quest_id = $QuestId; generation = $generationKey
            deferred_seconds = 0; reason = 'defer_context_started'
        }
    }

    $deferredSeconds = [int][Math]::Max(0, [Math]::Floor(($now - $started).TotalSeconds))
    if ($deferredSeconds -ge $ThresholdSeconds) {
        return [pscustomobject]@{
            action = 'backoff'; state = $state; quest_id = $QuestId; generation = $generationKey
            deferred_seconds = $deferredSeconds; reason = 'repeated_death_or_combat_stall'
        }
    }
    return [pscustomobject]@{
        action = 'defer'; state = $state; quest_id = $QuestId; generation = $generationKey
        deferred_seconds = $deferredSeconds; reason = 'death_or_combat_deferred'
    }
}

function Get-AcquireGenerationKey {
    param(
        [uint32]$LeaderGuid,
        [object[]]$Quests,
        [System.Collections.Generic.HashSet[string]]$BackoffKeys
    )
    $prefix = "${LeaderGuid}:"
    $leaderBackoffs = @()
    if ($null -ne $BackoffKeys) {
        $leaderBackoffs = @($BackoffKeys | Where-Object { ([string]$_).StartsWith($prefix) } | Sort-Object)
    }
    return ('leader={0};log={1};backoff={2}' -f $LeaderGuid, (Get-QuestLogGeneration -Quests $Quests), ($leaderBackoffs -join ','))
}

function New-AcquireIssuedState {
    param(
        [Parameter(Mandatory = $true)][string]$Generation,
        [Parameter(Mandatory = $true)][datetime]$IssuedUtc,
        [AllowNull()][object]$Position = $null,
        [ValidateRange(0, 2147483647)][int]$RetryCount = 0,
        [int]$QuestId = 0,
        [int]$GiverEntry = 0
    )
    $issued = $IssuedUtc.ToUniversalTime()
    return [pscustomobject][ordered]@{
        state = 'issued'
        generation = $Generation
        issued_utc = $issued
        last_movement_utc = $issued
        position = $Position
        retry_count = $RetryCount
        quest_id = $QuestId
        giver_entry = $GiverEntry
    }
}

function Get-AcquirePositionObservation {
    param(
        [AllowNull()][object]$PreviousPosition,
        [AllowNull()][object]$CurrentPosition,
        [ValidateRange(0.1, 1000.0)][double]$MovementEpsilon = 5.0
    )
    $result = [ordered]@{
        observed = $false
        moved = $false
        distance = $null
        reason = 'no_position_sample'
    }
    if ($null -eq $PreviousPosition -or $null -eq $CurrentPosition) {
        return [pscustomobject]$result
    }
    try {
        if ([int]$PreviousPosition.map -ne [int]$CurrentPosition.map) {
            $result.observed = $true
            $result.moved = $true
            $result.reason = 'map_changed'
            return [pscustomobject]$result
        }
        $x = [double]$PreviousPosition.x - [double]$CurrentPosition.x
        $y = [double]$PreviousPosition.y - [double]$CurrentPosition.y
        $z = [double]$PreviousPosition.z - [double]$CurrentPosition.z
        $distance = [Math]::Sqrt(($x * $x) + ($y * $y) + ($z * $z))
        if ([double]::IsNaN($distance) -or [double]::IsInfinity($distance)) {
            return [pscustomobject]$result
        }
        $result.observed = $true
        $result.distance = $distance
        $result.moved = $distance -ge $MovementEpsilon
        $result.reason = if ($result.moved) { 'meaningful_position_change' } else { 'position_flat' }
    }
    catch { }
    return [pscustomobject]$result
}

# Pure state machine for an acquire command which returned state=issued while the observed quest log
# is still empty. One retry is allowed after a bounded no-log/no-movement window; a second expired
# window enters a per-leader backoff and then opens one fresh deterministic switch-path acquire. The
# caller owns the returned state and any transport/receipt side effects.
function Get-AcquireIssuedStateDecision {
    param(
        [AllowNull()][object]$IssuedState,
        [Parameter(Mandatory = $true)][string]$Generation,
        [Parameter(Mandatory = $true)][datetime]$NowUtc,
        [ValidateRange(1, 86400)][int]$ExpirySeconds = 300,
        [AllowNull()][object]$CurrentPosition = $null,
        [ValidateRange(0.1, 1000.0)][double]$MovementEpsilon = 5.0,
        [ValidateRange(1, 86400)][int]$BackoffSeconds = 900
    )
    $now = $NowUtc.ToUniversalTime()
    $none = [pscustomobject]@{ action = 'none'; path = ''; state = $null; reason = ''; generation = $Generation; elapsed_seconds = 0; no_movement_seconds = 0; movement_distance = $null; movement_observed = $false; movement_detected = $false }
    if ($null -eq $IssuedState) { return $none }
    if ([string](Get-QuestDirectorProperty -Object $IssuedState -Name 'generation' -Default '') -ne $Generation) { return $none }

    $stateKind = [string](Get-QuestDirectorProperty -Object $IssuedState -Name 'state' -Default '')
    if ($stateKind -eq 'backoff') {
        $until = $null
        try { $until = ([datetime](Get-QuestDirectorProperty -Object $IssuedState -Name 'backoff_until_utc' -Default $null)).ToUniversalTime() }
        catch { }
        if ($null -ne $until -and $now -lt $until) {
            $remaining = [int][Math]::Max(0, [Math]::Ceiling(($until - $now).TotalSeconds))
            return [pscustomobject]@{
                action = 'hold'; path = 'backoff'; state = $IssuedState; generation = $Generation
                reason = 'acquire_backoff_active'; elapsed_seconds = 0; no_movement_seconds = 0
                movement_distance = $null; movement_observed = $false; movement_detected = $false
                backoff_remaining_seconds = $remaining; backoff_until_utc = $until
            }
        }
        $retryState = New-AcquireIssuedState -Generation $Generation -IssuedUtc $now -Position $CurrentPosition -RetryCount 0 `
            -QuestId ([int](Get-QuestDirectorProperty -Object $IssuedState -Name 'quest_id' -Default 0)) `
            -GiverEntry ([int](Get-QuestDirectorProperty -Object $IssuedState -Name 'giver_entry' -Default 0))
        return [pscustomobject]@{
            action = 'switch'; path = 'switch'; state = $retryState; generation = $Generation
            reason = 'acquire_backoff_expired_switch'; elapsed_seconds = 0; no_movement_seconds = 0
            movement_distance = $null; movement_observed = $false; movement_detected = $false
            backoff_until_utc = $until
        }
    }
    if ($stateKind -ne 'issued') { return $none }

    $issued = $null
    try { $issued = ([datetime](Get-QuestDirectorProperty -Object $IssuedState -Name 'issued_utc' -Default $null)).ToUniversalTime() }
    catch { }
    if ($null -eq $issued) { $issued = $now }
    $lastMovement = $null
    try { $lastMovement = ([datetime](Get-QuestDirectorProperty -Object $IssuedState -Name 'last_movement_utc' -Default $null)).ToUniversalTime() }
    catch { }
    if ($null -eq $lastMovement) { $lastMovement = $issued }

    $retryCount = 0
    try { $retryCount = [Math]::Max(0, [int](Get-QuestDirectorProperty -Object $IssuedState -Name 'retry_count' -Default 0)) }
    catch { $retryCount = 0 }
    $previousPosition = Get-QuestDirectorProperty -Object $IssuedState -Name 'position' -Default $null
    $positionObservation = Get-AcquirePositionObservation -PreviousPosition $previousPosition -CurrentPosition $CurrentPosition -MovementEpsilon $MovementEpsilon
    $nextPosition = if ($null -ne $CurrentPosition) { $CurrentPosition } else { $previousPosition }
    $nextState = [ordered]@{
        state = 'issued'
        generation = $Generation
        issued_utc = $issued
        last_movement_utc = $lastMovement
        position = $nextPosition
        retry_count = $retryCount
        quest_id = [int](Get-QuestDirectorProperty -Object $IssuedState -Name 'quest_id' -Default 0)
        giver_entry = [int](Get-QuestDirectorProperty -Object $IssuedState -Name 'giver_entry' -Default 0)
    }
    if ([bool]$positionObservation.moved) {
        $nextState.last_movement_utc = $now
        return [pscustomobject]@{
            action = 'hold'; path = 'travel'; state = [pscustomobject]$nextState; generation = $Generation
            reason = 'acquire_travel_progress'
            elapsed_seconds = [int][Math]::Max(0, [Math]::Floor(($now - $issued).TotalSeconds))
            no_movement_seconds = 0; movement_distance = $positionObservation.distance
            movement_observed = [bool]$positionObservation.observed; movement_detected = $true
        }
    }

    $elapsedSeconds = [int][Math]::Max(0, [Math]::Floor(($now - $issued).TotalSeconds))
    $noMovementSeconds = [int][Math]::Max(0, [Math]::Floor(($now - $lastMovement).TotalSeconds))
    if ($noMovementSeconds -lt $ExpirySeconds) {
        return [pscustomobject]@{
            action = 'hold'; path = 'pending'; state = [pscustomobject]$nextState; generation = $Generation
            reason = 'acquire_pending'; elapsed_seconds = $elapsedSeconds; no_movement_seconds = $noMovementSeconds
            movement_distance = $positionObservation.distance; movement_observed = [bool]$positionObservation.observed
            movement_detected = $false
        }
    }

    $questId = [int]$nextState.quest_id
    $giverEntry = [int]$nextState.giver_entry
    if ($retryCount -eq 0) {
        $retryState = New-AcquireIssuedState -Generation $Generation -IssuedUtc $now -Position $nextPosition -RetryCount 1 `
            -QuestId $questId -GiverEntry $giverEntry
        return [pscustomobject]@{
            action = 'retry'; path = 'retry'; state = $retryState; generation = $Generation
            reason = 'acquire_no_log_no_movement_retry'; elapsed_seconds = $elapsedSeconds
            no_movement_seconds = $noMovementSeconds; movement_distance = $positionObservation.distance
            movement_observed = [bool]$positionObservation.observed; movement_detected = $false
        }
    }

    $backoffUntil = $now.AddSeconds($BackoffSeconds)
    $backoffState = [pscustomobject][ordered]@{
        state = 'backoff'
        generation = $Generation
        backoff_started_utc = $now
        backoff_until_utc = $backoffUntil
        retry_count = $retryCount
        position = $nextPosition
        quest_id = $questId
        giver_entry = $giverEntry
    }
    return [pscustomobject]@{
        action = 'backoff'; path = 'backoff_switch'; state = $backoffState; generation = $Generation
        reason = 'acquire_no_log_no_movement_backoff'; elapsed_seconds = $elapsedSeconds
        no_movement_seconds = $noMovementSeconds; movement_distance = $positionObservation.distance
        movement_observed = [bool]$positionObservation.observed; movement_detected = $false
        backoff_until_utc = $backoffUntil
    }
}

# Pure director decision: an empty quest log or the all-supported-backed-off state may open
# acquisition, and only once per stable quest-log + active-backoff generation. A real state=issued
# response adds a bounded per-leader no-log/no-movement gate on top of that generation latch.
function Get-AcquireDecision {
    param(
        [string]$SelectionReason,
        [uint32]$LeaderGuid,
        [object[]]$Quests,
        [System.Collections.Generic.HashSet[string]]$BackoffKeys,
        [string]$LastGeneration,
        [int]$ActiveFinisherQuestId = 0,
        [AllowNull()][object]$IssuedAcquireState = $null,
        [datetime]$NowUtc = [datetime]::UtcNow,
        [ValidateRange(1, 86400)][int]$AcquireNoLogExpirySeconds = 300,
        [AllowNull()][object]$CurrentPosition = $null,
        [ValidateRange(0.1, 1000.0)][double]$AcquireMovementEpsilon = 5.0,
        [ValidateRange(1, 86400)][int]$AcquireBackoffSeconds = 900
    )
    if ($SelectionReason -notin @('all_supported_backed_off', 'no_active_quest')) {
        return [pscustomobject]@{ action = 'idle'; generation = ''; reason = $SelectionReason; path = ''; state = $null }
    }

    $generation = Get-AcquireGenerationKey -LeaderGuid $LeaderGuid -Quests $Quests -BackoffKeys $BackoffKeys
    $activeFinisherOwner = @($Quests | Where-Object {
        $null -ne $_ -and [int]$_.id -eq $ActiveFinisherQuestId -and [bool]$_.is_complete -and [bool]$_.supported
    })
    if ($ActiveFinisherQuestId -gt 0 -and $activeFinisherOwner.Count -gt 0) {
        return [pscustomobject]@{ action = 'hold'; generation = $generation; reason = 'active_supported_finisher_directive'; path = 'finisher'; state = $null }
    }

    $issuedDecision = Get-AcquireIssuedStateDecision -IssuedState $IssuedAcquireState -Generation $generation `
        -NowUtc $NowUtc -ExpirySeconds $AcquireNoLogExpirySeconds -CurrentPosition $CurrentPosition `
        -MovementEpsilon $AcquireMovementEpsilon -BackoffSeconds $AcquireBackoffSeconds
    if ($issuedDecision.action -ne 'none') {
        $action = switch ([string]$issuedDecision.action) {
            'retry' { 'acquire'; break }
            'switch' { 'acquire'; break }
            default { [string]$issuedDecision.action }
        }
        return [pscustomobject]@{
            action = $action; generation = $generation
            reason = [string](Get-QuestDirectorProperty -Object $issuedDecision -Name 'reason' -Default '')
            path = [string](Get-QuestDirectorProperty -Object $issuedDecision -Name 'path' -Default '')
            state = Get-QuestDirectorProperty -Object $issuedDecision -Name 'state' -Default $null
            elapsed_seconds = [int](Get-QuestDirectorProperty -Object $issuedDecision -Name 'elapsed_seconds' -Default 0)
            no_movement_seconds = [int](Get-QuestDirectorProperty -Object $issuedDecision -Name 'no_movement_seconds' -Default 0)
            movement_distance = Get-QuestDirectorProperty -Object $issuedDecision -Name 'movement_distance' -Default $null
            movement_observed = [bool](Get-QuestDirectorProperty -Object $issuedDecision -Name 'movement_observed' -Default $false)
            movement_detected = [bool](Get-QuestDirectorProperty -Object $issuedDecision -Name 'movement_detected' -Default $false)
            backoff_until_utc = Get-QuestDirectorProperty -Object $issuedDecision -Name 'backoff_until_utc' -Default $null
        }
    }
    if ($LastGeneration -eq $generation) {
        return [pscustomobject]@{ action = 'hold'; generation = $generation; reason = 'acquire_generation_unchanged'; path = 'generation'; state = $null }
    }
    return [pscustomobject]@{ action = 'acquire'; generation = $generation; reason = $SelectionReason; path = 'initial'; state = $null }
}

function Test-ShouldIssueDirective {
    param([string]$LastIssued, [string]$IssueKey)
    return [string]::IsNullOrEmpty($LastIssued) -or $LastIssued -ne $IssueKey
}

# Successful recovery resets transient movement/RPG state, so retaining the old issue key would
# incorrectly turn the next cycle into a hold. Return true only when a key was actually cleared.
function Clear-LastIssuedAfterSuccessfulRecovery {
    param([hashtable]$LastIssuedByLeader, [string]$LeaderKey, [bool]$RecoveryOk)
    if (-not $RecoveryOk -or $null -eq $LastIssuedByLeader) { return $false }
    if (-not $LastIssuedByLeader.ContainsKey($LeaderKey)) { return $false }
    $LastIssuedByLeader.Remove($LeaderKey)
    return $true
}
