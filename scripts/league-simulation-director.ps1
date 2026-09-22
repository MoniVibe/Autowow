[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [ValidateRange(1,1440)][int]$DurationMinutes = 480,
    [ValidateRange(10,300)][int]$ScoutEverySeconds = 20,
    [ValidateRange(30,900)][int]$NoProgressSeconds = 120,
    # Zero derives from NoProgressSeconds; explicit values still honor the conservative safe floor.
    [ValidateRange(0,86400)][int]$DeathOrCombatStallSeconds = 0,
    [ValidateRange(1,5)][int]$MaxRecoveriesPerQuest = 2,
    [ValidateRange(1,240)][int]$BackoffMinutes = 15,
    # A real acquire response with state=issued is held while the party is moving, but cannot latch
    # an unchanged empty quest log forever. One retry is allowed after this no-log/no-movement window.
    [ValidateRange(1,86400)][int]$AcquireNoLogExpirySeconds = 300,
    [ValidateRange(0.1,1000.0)][double]$AcquireMovementEpsilon = 5.0,
    # Restrict to explicit leader GUIDs (e.g. 7,10). Empty = discover northstar/ember leaders via roster.
    [uint32[]]$Leaders = @(),
    # Process-launch-safe form for background orchestration. PowerShell's native command-line
    # binder can collapse `-Leaders 7,10` into 710, so wrappers pass this explicit CSV string.
    [string]$LeaderCsv = '',
    # Observation proof: compute + log every decision but issue NO quest/recover order. Safe to run
    # alongside a live issuing director without conflicting on the same leaders.
    [switch]$ObserveOnly,
    # Playerbots log used only for read-only engine-abandonment detection. When omitted, prefer
    # the active isolated WSL runtime log and fall back to the legacy Windows runtime path.
    [string]$PlayerbotsLogPath = '',
    [string]$BackoffLedgerPath = '',
    [string]$ReceiptPath = ''
)

. (Join-Path $PSScriptRoot 'Common.ps1')
. (Join-Path $PSScriptRoot 'QuestDirectorLib.ps1')
. (Join-Path $PSScriptRoot 'QuestLogSource.ps1')

$ErrorActionPreference = 'Stop'
$effectiveDeathOrCombatStallSeconds = Get-DeathOrCombatStallThresholdSeconds `
    -NoProgressSeconds $NoProgressSeconds -ConfiguredSeconds $DeathOrCombatStallSeconds
if (-not [string]::IsNullOrWhiteSpace($LeaderCsv)) {
    $parsedLeaders = [System.Collections.Generic.List[uint32]]::new()
    foreach ($token in ($LeaderCsv -split ',')) {
        [uint32]$parsed = 0
        if (-not [uint32]::TryParse($token.Trim(), [ref]$parsed) -or $parsed -eq 0) {
            throw "LeaderCsv contains an invalid positive GUID token: '$token'."
        }
        if ($parsed -notin $parsedLeaders) { $parsedLeaders.Add($parsed) }
    }
    $Leaders = @($parsedLeaders)
}
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
$simulation = Join-Path $PSScriptRoot 'league-simulation.ps1'
if ([string]::IsNullOrWhiteSpace($ReceiptPath)) {
    $ReceiptPath = Join-Path $ServerRoot ('leagues\results\quest-director-v1-{0}.jsonl' -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
if ([string]::IsNullOrWhiteSpace($BackoffLedgerPath)) {
    $BackoffLedgerPath = Join-Path $ServerRoot 'leagues\results\quest-director-backoff.json'
}
New-Item -ItemType Directory -Path (Split-Path -Parent $ReceiptPath) -Force | Out-Null
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)

function Write-Receipt {
    param([string]$Event, [hashtable]$Fields = @{})
    $record = [ordered]@{ timestamp_utc = (Get-Date).ToUniversalTime().ToString('o'); event = $Event }
    foreach ($key in $Fields.Keys) { $record[$key] = $Fields[$key] }
    [System.IO.File]::AppendAllText($ReceiptPath, (($record | ConvertTo-Json -Compress -Depth 10) + [Environment]::NewLine), $utf8NoBom)
}

function Invoke-BridgeJson {
    param([Parameter(Mandatory = $true)][string]$Request)
    $parsed = Send-Bridge -Request $Request | ConvertFrom-Json
    return $parsed
}

function Get-OptionalJsonField {
    param(
        [AllowNull()][object]$Object,
        [Parameter(Mandatory = $true)][string]$Name,
        [AllowNull()][object]$Default = $null
    )
    if ($null -eq $Object) { return $Default }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property -or $null -eq $property.Value) { return $Default }
    return $property.Value
}

# --- Backoff ledger (external, JSON, expiring) --------------------------------------------------
function Read-BackoffLedger {
    if (-not (Test-Path -LiteralPath $BackoffLedgerPath)) { return @{} }
    try {
        $raw = Get-Content -LiteralPath $BackoffLedgerPath -Raw
        if ([string]::IsNullOrWhiteSpace($raw)) { return @{} }
        $obj = $raw | ConvertFrom-Json
        $map = @{}
        foreach ($p in $obj.PSObject.Properties) { $map[$p.Name] = $p.Value }
        return $map
    }
    catch { return @{} }
}

function Save-BackoffLedger {
    param([hashtable]$Ledger)
    $ordered = [ordered]@{}
    foreach ($k in ($Ledger.Keys | Sort-Object)) { $ordered[$k] = $Ledger[$k] }
    [System.IO.File]::WriteAllText($BackoffLedgerPath, (([pscustomobject]$ordered) | ConvertTo-Json -Depth 6), $utf8NoBom)
}

# Returns an ARRAY of still-active backoff keys (and prunes expired entries from $Ledger). Returning
# an array — not a HashSet — avoids PowerShell unrolling a HashSet return into null/string/Object[].
function Get-ActiveBackoffKeys {
    param([hashtable]$Ledger, [datetime]$Now)
    $active = [System.Collections.Generic.List[string]]::new()
    $expired = @()
    foreach ($key in @($Ledger.Keys)) {
        $until = [datetime]::Parse([string]$Ledger[$key].until_utc).ToUniversalTime()
        if ($until -gt $Now) { $active.Add([string]$key) } else { $expired += $key }
    }
    foreach ($k in $expired) { $Ledger.Remove($k) }
    return @($active)
}

function Get-PositionDistance {
    param([object]$Left, [object]$Right)
    if ($null -eq $Left -or $null -eq $Right) { return [double]::PositiveInfinity }
    try {
        if ([int]$Left.map -ne [int]$Right.map) { return [double]::PositiveInfinity }
        $x = [double]$Left.x - [double]$Right.x; $y = [double]$Left.y - [double]$Right.y; $z = [double]$Left.z - [double]$Right.z
        return [Math]::Sqrt(($x * $x) + ($y * $y) + ($z * $z))
    }
    catch { return [double]::PositiveInfinity }
}

function Get-ObjectiveSum {
    param([object]$Quests, [int]$QuestId)
    $q = @($Quests | Where-Object { [int]$_.id -eq $QuestId })
    if ($q.Count -eq 0) { return 0 }
    $sum = 0
    foreach ($o in @($q[0].objectives)) { $sum += [Math]::Min([int]$o.current, [int]$o.required) }
    return $sum
}

# --- Leader discovery --------------------------------------------------------------------------
function Get-Leaders {
    $listRaw = Invoke-BridgeJson -Request 'list'
    if (-not $listRaw.ok) { throw "Bridge list failed: $($listRaw.error)" }
    $bots = @($listRaw.bots)
    if ($Leaders.Count -gt 0) {
        $wanted = @($Leaders | ForEach-Object { [uint32]$_ })
        return @($bots | Where-Object { [uint32]$_.guid -in $wanted } | ForEach-Object { $_ | Add-Member -NotePropertyName team -NotePropertyValue 'observed' -PassThru -Force })
    }
    $teamByGuid = @{}
    $rosterRaw = (& $simulation -Action roster -ServerRoot $ServerRoot -AsJson | Out-String).Trim()
    # ConvertFrom-Json's pipeline input can preserve the JSON array as one
    # System.Object[] under Windows PowerShell. Use -InputObject and normalize
    # the result before reading scalar guid/team properties.
    $rosterParsed = ConvertFrom-Json -InputObject $rosterRaw
    $rosterEntries = if ($rosterParsed -is [System.Array]) { @($rosterParsed) } else { @($rosterParsed) }
    foreach ($m in $rosterEntries) {
        if ($m -is [System.Array]) {
            foreach ($entry in $m) {
                if ($null -ne $entry -and $null -ne $entry.guid) { $teamByGuid[[uint32]$entry.guid] = [string]$entry.team }
            }
        }
        elseif ($null -ne $m -and $null -ne $m.guid) {
            $teamByGuid[[uint32]$m.guid] = [string]$m.team
        }
    }
    return @($bots | Where-Object {
            $teamByGuid.ContainsKey([uint32]$_.guid) -and $teamByGuid[[uint32]$_.guid] -in @('northstar', 'ember') -and
            [uint32]$_.group.leader_guid -eq [uint32]$_.guid -and [int]$_.group.members -ge 2 -and -not [bool]$_.paused
        } | ForEach-Object { $_ | Add-Member -NotePropertyName team -NotePropertyValue $teamByGuid[[uint32]$_.guid] -PassThru -Force })
}

# Read-only engine-abandonment signal from the Playerbots log. Start at end-of-file so we only react
# to abandonments that happen during this run.
$script:PlayerbotsLogPath = if (-not [string]::IsNullOrWhiteSpace($PlayerbotsLogPath)) {
    if ([System.IO.Path]::IsPathRooted($PlayerbotsLogPath)) {
        [System.IO.Path]::GetFullPath($PlayerbotsLogPath)
    }
    else {
        [System.IO.Path]::GetFullPath((Join-Path $ServerRoot $PlayerbotsLogPath))
    }
}
else {
    $phase1Log = Join-Path $ServerRoot 'logs\phase1-runtime\Playerbots.log'
    if (Test-Path -LiteralPath $phase1Log) { $phase1Log }
    else { Join-Path $ServerRoot 'server\logs\Playerbots.log' }
}
$script:PlayerbotsLogOffset = if (Test-Path -LiteralPath $script:PlayerbotsLogPath) { (Get-Item -LiteralPath $script:PlayerbotsLogPath).Length } else { 0 }

function Get-NewAbandonments {
    param([hashtable]$NameToGuid)
    if (-not (Test-Path -LiteralPath $script:PlayerbotsLogPath)) { return @() }
    try {
        $len = (Get-Item -LiteralPath $script:PlayerbotsLogPath).Length
        if ($len -lt $script:PlayerbotsLogOffset) { $script:PlayerbotsLogOffset = 0 }   # rotated/truncated
        if ($len -le $script:PlayerbotsLogOffset) { return @() }
        $fs = [System.IO.File]::Open($script:PlayerbotsLogPath, 'Open', 'Read', 'ReadWrite')
        try {
            [void]$fs.Seek($script:PlayerbotsLogOffset, 'Begin')
            $sr = [System.IO.StreamReader]::new($fs)
            $text = $sr.ReadToEnd()
        }
        finally { $fs.Dispose() }
        $script:PlayerbotsLogOffset = $len
        return @(Get-AbandonmentsFromText -Text $text -NameToGuid $NameToGuid)
    }
    catch { return @() }
}

$dbContext = New-QuestDbContext -ServerRoot $ServerRoot
$deadline = (Get-Date).AddMinutes($DurationMinutes)
$failures = 0
$progressByLeader = @{}      # guid -> @{ quest_id; phase; xp; position; obj_sum; last_progress_utc; recovery_attempts }
$lastIssued = @{}            # guid -> "questId:phase"
$lastAcquireGeneration = @{} # guid -> stable quest-log + active-backoff generation
$issuedAcquireByLeader = @{} # guid -> state=issued/backoff + issued_utc + last_movement_utc
$deferQuestContext = @{}     # guid -> last healthy @{ quest_id; generation }
$deferStallByLeader = @{}    # guid -> continuous dead/combat defer state
$backoff = Read-BackoffLedger
$mode = if ($ObserveOnly) { 'observe_only' } else { 'issue' }

Write-Receipt -Event 'quest_director_v1_started' -Fields @{
    mode = $mode; duration_minutes = $DurationMinutes; quest_every_seconds = $ScoutEverySeconds
    playerbots_log_path = $script:PlayerbotsLogPath
    no_progress_seconds = $NoProgressSeconds; max_recoveries_per_quest = $MaxRecoveriesPerQuest
    death_or_combat_stall_seconds = $effectiveDeathOrCombatStallSeconds
    backoff_minutes = $BackoffMinutes; acquire_no_log_expiry_seconds = $AcquireNoLogExpirySeconds
    acquire_movement_epsilon = $AcquireMovementEpsilon; leaders = @($Leaders); db_fallback = [bool]$dbContext
    backoff_ledger = $BackoffLedgerPath
}

try {
    while ((Get-Date) -lt $deadline) {
        try {
            $now = (Get-Date).ToUniversalTime()
            $backoffSet = [System.Collections.Generic.HashSet[string]]::new()
            foreach ($k in @(Get-ActiveBackoffKeys -Ledger $backoff -Now $now)) { [void]$backoffSet.Add([string]$k) }
            # NOTE: do not name this $leaders — PowerShell is case-insensitive and it would collide
            # with the [uint32[]]$Leaders parameter, coercing leader objects to uint32[].
            $activeLeaders = Get-Leaders

            # Detect engine quest abandonments (read-only) and back them off immediately so the director
            # stops re-selecting a quest the engine has given up on. This is the definitive "activity
            # without objective progress" signal and is independent of save-lagged DB counters.
            $nameToGuid = @{}; foreach ($l in $activeLeaders) { $nameToGuid[[string]$l.name] = [uint32]$l.guid }
            foreach ($ab in (Get-NewAbandonments -NameToGuid $nameToGuid)) {
                $abKey = "{0}:{1}" -f [uint32]$ab.guid, [int]$ab.quest
                if (-not $backoffSet.Contains($abKey)) {
                    $backoff[$abKey] = [pscustomobject]@{ until_utc = $now.AddMinutes($BackoffMinutes).ToString('o'); reason = 'engine_abandoned'; leader_guid = [int]$ab.guid; quest_id = [int]$ab.quest }
                    [void]$backoffSet.Add($abKey); Save-BackoffLedger -Ledger $backoff
                    Write-Receipt -Event 'quest_abandonment_detected' -Fields @{ leader_guid = [int]$ab.guid; quest_id = [int]$ab.quest; action = 'backed_off'; backoff_minutes = $BackoffMinutes; source = 'playerbots_log' }
                }
            }

            foreach ($leader in $activeLeaders) {
                $guid = [uint32]$leader.guid
                $key = [string]$guid
                $team = [string]$leader.team

                # Refresh only the read-only quest-log view before the defer gate. This lets objective
                # progress or log changes reset a long combat timer without opening any action path.
                $logResult = Get-NormalizedQuestLog -Guid $guid -DbContext $dbContext
                $quests = @($logResult.quests)
                $selection = Select-DirectorQuest -Quests $quests -BackoffKeys $backoffSet -LeaderGuid $guid
                $observedDeferContext = Get-DeathOrCombatQuestContext -SelectedQuestId ([int]$selection.quest_id) `
                    -LastIssued ([string]$lastIssued[$key]) -Quests $quests

                # BEGIN prolonged death/combat defer policy (ledger-only; no bridge action).
                if (-not $leader.alive -or $leader.combat) {
                    $context = $deferQuestContext[$key]
                    $priorQuestStillKnown = $null -ne $context -and [int]$context.quest_id -gt 0 -and
                        @($quests | Where-Object { [int]$_.id -eq [int]$context.quest_id }).Count -gt 0
                    if ($priorQuestStillKnown) {
                        # Pin the deferred quest so its new backoff cannot cascade across the rest of
                        # the log while the leader remains unhealthy; still refresh the generation.
                        $context = [pscustomobject]@{
                            quest_id = [int]$context.quest_id
                            generation = [string]$observedDeferContext.generation
                        }
                    }
                    else {
                        $context = $observedDeferContext
                    }
                    $deferQuestContext[$key] = $context
                    $contextQuestId = if ($null -ne $context) { [int]$context.quest_id } else { 0 }
                    $contextGeneration = if ($null -ne $context) { [string]$context.generation } else { '' }
                    $deferDecision = Get-DeathOrCombatStallDecision -PreviousState $deferStallByLeader[$key] `
                        -IsDeferred $true -QuestId $contextQuestId -Generation $contextGeneration `
                        -NowUtc $now -ThresholdSeconds $effectiveDeathOrCombatStallSeconds
                    if ($null -ne $deferDecision.state) { $deferStallByLeader[$key] = $deferDecision.state }
                    else { $deferStallByLeader.Remove($key) | Out-Null }

                    Write-Receipt -Event 'quest_cycle_deferred' -Fields @{
                        team = $team; leader_guid = $guid; alive = [bool]$leader.alive; combat = [bool]$leader.combat
                        quest_id = [int]$deferDecision.quest_id; deferred_seconds = [int]$deferDecision.deferred_seconds
                        stall_threshold_seconds = $effectiveDeathOrCombatStallSeconds; policy_action = [string]$deferDecision.action
                        policy_reason = [string]$deferDecision.reason
                    }

                    if ($deferDecision.action -eq 'backoff') {
                        $stallKey = "{0}:{1}" -f $guid, [int]$deferDecision.quest_id
                        if (-not $backoffSet.Contains($stallKey)) {
                            $backoff[$stallKey] = [pscustomobject]@{
                                until_utc = $now.AddMinutes($BackoffMinutes).ToString('o')
                                reason = 'repeated_death_or_combat_stall'
                                leader_guid = [int]$guid
                                quest_id = [int]$deferDecision.quest_id
                            }
                            Save-BackoffLedger -Ledger $backoff
                            [void]$backoffSet.Add($stallKey)
                            $progressByLeader.Remove($key) | Out-Null
                            Write-Receipt -Event 'quest_backoff_registered' -Fields @{
                                team = $team; leader_guid = $guid; quest_id = [int]$deferDecision.quest_id
                                idle_seconds = [int]$deferDecision.deferred_seconds; backoff_minutes = $BackoffMinutes
                                reason = 'repeated_death_or_combat_stall'
                            }
                        }
                        # Require another full prolonged defer interval before considering renewal.
                        $deferStallByLeader.Remove($key) | Out-Null
                    }
                    continue
                }
                # END prolonged death/combat defer policy.

                # Any healthy observation breaks continuity, even if the same quest is selected again.
                $healthyDecision = Get-DeathOrCombatStallDecision -PreviousState $deferStallByLeader[$key] `
                    -IsDeferred $false -QuestId 0 -Generation '' -NowUtc $now `
                    -ThresholdSeconds $effectiveDeathOrCombatStallSeconds
                if ($healthyDecision.action -eq 'reset') { $deferStallByLeader.Remove($key) | Out-Null }
                $deferQuestContext[$key] = $observedDeferContext

                $xp = [uint64]$leader.progress.xp
                $objSum = if ($selection.quest_id -ne 0) { Get-ObjectiveSum -Quests $quests -QuestId ([int]$selection.quest_id) } else { 0 }
                $finisherObservation = ConvertTo-DirectorFinisherObservation -Response $null
                $completeSupported = @($quests | Where-Object { [bool]$_.is_complete -and [bool]$_.supported })
                $needsFinisherObservation = $selection.phase -eq 'turnin' -or
                    ($selection.reason -eq 'all_supported_backed_off' -and $completeSupported.Count -gt 0)
                if ($needsFinisherObservation) {
                    try {
                        $finisherResponse = Invoke-BridgeJson -Request "questobjective $guid"
                        $finisherObservation = ConvertTo-DirectorFinisherObservation -Response $finisherResponse
                    }
                    catch {
                        Write-Receipt -Event 'quest_finisher_observation_failed' -Fields @{
                            team = $team; leader_guid = $guid; error = $_.Exception.Message
                        }
                    }
                }
                $selectedFinisherPhase = ''
                $selectedFinisherDistance = $null
                if ([int]$finisherObservation.quest_id -eq [int]$selection.quest_id) {
                    $selectedFinisherPhase = [string]$finisherObservation.phase
                    $selectedFinisherDistance = $finisherObservation.distance
                }
                $activeFinisherQuestId = 0
                if ([bool]$finisherObservation.directive_active -and [int]$finisherObservation.directive_quest_id -gt 0) {
                    $activeFinisherQuestId = [int]$finisherObservation.directive_quest_id
                }

                Write-Receipt -Event 'quest_evaluated' -Fields @{
                    team = $team; leader_guid = $guid; source = $logResult.source; source_note = $logResult.note
                    active_quests = $quests.Count; selection = [string]$selection.reason
                    selected_quest = [int]$selection.quest_id; phase = [string]$selection.phase
                    capability_class = [string]$selection.capability_class; progress_ratio = [Math]::Round([double]$selection.progress_ratio, 3)
                    objective_sum = $objSum; xp = $xp; map = [int]$leader.position.map
                    x = [double]$leader.position.x; y = [double]$leader.position.y; z = [double]$leader.position.z
                    finisher_phase = $selectedFinisherPhase; finisher_distance = $selectedFinisherDistance
                    active_finisher_directive_quest = $activeFinisherQuestId
                }

                if ($selection.action -eq 'idle') {
                    # Explicit idle. Never fall back to random movement or generic grind.
                    Write-Receipt -Event 'quest_idle' -Fields @{ team = $team; leader_guid = $guid; reason = [string]$selection.reason; detail = [string]$selection.detail; unsupported_classes = [string]$selection.detail }
                    $acquire = Get-AcquireDecision -SelectionReason ([string]$selection.reason) -LeaderGuid $guid `
                        -Quests $quests -BackoffKeys $backoffSet -LastGeneration ([string]$lastAcquireGeneration[$key]) `
                        -ActiveFinisherQuestId $activeFinisherQuestId -IssuedAcquireState $issuedAcquireByLeader[$key] `
                        -NowUtc $now -AcquireNoLogExpirySeconds $AcquireNoLogExpirySeconds `
                        -CurrentPosition $leader.position -AcquireMovementEpsilon $AcquireMovementEpsilon `
                        -AcquireBackoffSeconds ($BackoffMinutes * 60)
                    $acquireDecisionState = Get-QuestDirectorProperty -Object $acquire -Name 'state' -Default $null
                    $acquireElapsedSeconds = [int](Get-QuestDirectorProperty -Object $acquire -Name 'elapsed_seconds' -Default 0)
                    $acquireNoMovementSeconds = [int](Get-QuestDirectorProperty -Object $acquire -Name 'no_movement_seconds' -Default 0)
                    $acquireMovementDistance = Get-QuestDirectorProperty -Object $acquire -Name 'movement_distance' -Default $null
                    $acquireBackoffUntilUtc = Get-QuestDirectorProperty -Object $acquire -Name 'backoff_until_utc' -Default $null
                    if ($null -ne $acquireDecisionState) {
                        $issuedAcquireByLeader[$key] = $acquireDecisionState
                    }
                    else {
                        $issuedAcquireByLeader.Remove($key) | Out-Null
                    }
                    if ($acquire.action -eq 'acquire') {
                        # Mark before transport so one unstable bridge call cannot turn into command spam.
                        $lastAcquireGeneration[$key] = [string]$acquire.generation
                        $acquirePath = [string]$acquire.path
                        if ($acquirePath -eq 'retry') {
                            Write-Receipt -Event 'quest_acquire_retry' -Fields @{
                                team = $team; leader_guid = $guid; reason = [string]$acquire.reason
                                generation = [string]$acquire.generation; request = "quest $guid acquire"
                                elapsed_seconds = $acquireElapsedSeconds; no_movement_seconds = $acquireNoMovementSeconds
                                expiry_seconds = $AcquireNoLogExpirySeconds; retry_count = [int](Get-QuestDirectorProperty -Object $acquireDecisionState -Name 'retry_count' -Default 0)
                            }
                        }
                        elseif ($acquirePath -eq 'switch') {
                            Write-Receipt -Event 'quest_acquire_switch' -Fields @{
                                team = $team; leader_guid = $guid; reason = [string]$acquire.reason
                                generation = [string]$acquire.generation; request = "quest $guid acquire"
                                backoff_until_utc = [string]$acquireBackoffUntilUtc
                            }
                        }
                        if ($ObserveOnly) {
                            Write-Receipt -Event 'quest_acquire_would_issue' -Fields @{
                                team = $team; leader_guid = $guid; reason = [string]$acquire.reason
                                generation = [string]$acquire.generation; request = "quest $guid acquire"; path = $acquirePath
                            }
                        }
                        else {
                            $acquireResult = Invoke-BridgeJson -Request "quest $guid acquire"
                            $acquireOk = [bool](Get-OptionalJsonField -Object $acquireResult -Name 'ok' -Default $false)
                            $acquireState = [string](Get-OptionalJsonField -Object $acquireResult -Name 'state' -Default '')
                            $acquireQuestId = [int](Get-OptionalJsonField -Object $acquireResult -Name 'quest_id' -Default 0)
                            $acquireGiverEntry = [int](Get-OptionalJsonField -Object $acquireResult -Name 'giver_entry' -Default 0)
                            $acquireIssuedUtc = if ($acquireState -eq 'issued') { $now.ToString('o') } else { '' }
                            $acquireRetryCount = [int](Get-QuestDirectorProperty -Object $acquireDecisionState -Name 'retry_count' -Default 0)
                            Write-Receipt -Event 'quest_acquire_issued' -Fields @{
                                team = $team; leader_guid = $guid; generation = [string]$acquire.generation
                                path = $acquirePath; ok = $acquireOk; state = $acquireState
                                quest_id = $acquireQuestId; giver_entry = $acquireGiverEntry
                                giver_guid = [uint64](Get-OptionalJsonField -Object $acquireResult -Name 'giver_guid' -Default 0)
                                giver_map = [int](Get-OptionalJsonField -Object $acquireResult -Name 'giver_map' -Default 0)
                                reason = [string](Get-OptionalJsonField -Object $acquireResult -Name 'reason' -Default '')
                                movement_activation = [string](Get-OptionalJsonField -Object $acquireResult -Name 'movement_activation' -Default 'not_reported')
                                movement_kick_accepted = [bool](Get-OptionalJsonField -Object $acquireResult -Name 'movement_kick_accepted' -Default $false)
                                issued_utc = $acquireIssuedUtc; retry_count = $acquireRetryCount
                                expiry_seconds = $AcquireNoLogExpirySeconds
                                error = [string](Get-OptionalJsonField -Object $acquireResult -Name 'error' -Default '')
                            }
                            if ($acquireOk -and $acquireState -eq 'issued') {
                                $issuedAcquireByLeader[$key] = New-AcquireIssuedState -Generation ([string]$acquire.generation) `
                                    -IssuedUtc $now -Position $leader.position -RetryCount $acquireRetryCount `
                                    -QuestId $acquireQuestId -GiverEntry $acquireGiverEntry
                            }
                            elseif (-not ($acquirePath -in @('retry', 'switch')) -or $acquireState -ne '') {
                                # Terminal/invalid responses retire the issued gate. A transport exception
                                # leaves the pre-armed retry/switch state in place for the bounded window.
                                $issuedAcquireByLeader.Remove($key) | Out-Null
                            }
                        }
                    }
                    elseif ($acquire.action -eq 'backoff') {
                        Write-Receipt -Event 'quest_acquire_backoff' -Fields @{
                            team = $team; leader_guid = $guid; reason = [string]$acquire.reason
                            generation = [string]$acquire.generation; path = [string]$acquire.path
                            quest_id = [int](Get-QuestDirectorProperty -Object $acquireDecisionState -Name 'quest_id' -Default 0)
                            retry_count = [int](Get-QuestDirectorProperty -Object $acquireDecisionState -Name 'retry_count' -Default 0)
                            no_movement_seconds = $acquireNoMovementSeconds
                            backoff_until_utc = [string](Get-QuestDirectorProperty -Object $acquireDecisionState -Name 'backoff_until_utc' -Default '')
                        }
                    }
                    elseif ($acquire.action -eq 'hold') {
                        Write-Receipt -Event 'quest_acquire_hold' -Fields @{
                            team = $team; leader_guid = $guid; reason = [string]$acquire.reason
                            generation = [string]$acquire.generation; path = [string]$acquire.path
                            state = [string](Get-QuestDirectorProperty -Object $acquireDecisionState -Name 'state' -Default '')
                            issued_utc = [string](Get-QuestDirectorProperty -Object $acquireDecisionState -Name 'issued_utc' -Default '')
                            elapsed_seconds = $acquireElapsedSeconds
                            no_movement_seconds = $acquireNoMovementSeconds
                            expiry_seconds = $AcquireNoLogExpirySeconds
                            retry_count = [int](Get-QuestDirectorProperty -Object $acquireDecisionState -Name 'retry_count' -Default 0)
                            movement_distance = $acquireMovementDistance
                            backoff_until_utc = [string](Get-QuestDirectorProperty -Object $acquireDecisionState -Name 'backoff_until_utc' -Default '')
                        }
                    }
                    $progressByLeader.Remove($key) | Out-Null
                    continue
                }

                # A newly visible supported quest always takes over through the normal exact quest
                # directive. Acquisition state is not allowed to mask it.
                $lastAcquireGeneration.Remove($key) | Out-Null
                $issuedAcquireByLeader.Remove($key) | Out-Null

                # Progress + recovery accounting for the currently selected quest phase.
                $state = $progressByLeader[$key]
                $selKey = "$([int]$selection.quest_id):$([string]$selection.phase)"
                if ($null -eq $state -or [string]$state.quest_key -ne $selKey) {
                    $state = [ordered]@{
                        quest_key = $selKey; quest_id = [int]$selection.quest_id; phase = [string]$selection.phase
                        xp = $xp; position = $leader.position; obj_sum = $objSum
                        finisher_phase = $selectedFinisherPhase; finisher_distance = $selectedFinisherDistance
                        last_progress_utc = $now; recovery_attempts = 0
                    }
                    $progressByLeader[$key] = $state
                }
                else {
                    $dist = Get-PositionDistance -Left $state.position -Right $leader.position
                    $progressDecision = Get-DirectorQuestProgressDecision -DirectorPhase ([string]$selection.phase) `
                        -PreviousObjectiveSum ([int]$state.obj_sum) -CurrentObjectiveSum $objSum `
                        -PreviousFinisherPhase ([string]$state.finisher_phase) -CurrentFinisherPhase $selectedFinisherPhase `
                        -PreviousFinisherDistance $state.finisher_distance -CurrentFinisherDistance $selectedFinisherDistance
                    $made = [bool]$progressDecision.made_progress
                    $state.position = $leader.position; $state.xp = $xp; $state.obj_sum = $objSum; $state.moved = [Math]::Round($dist, 1)
                    $state.progress_reason = [string]$progressDecision.reason
                    # Adopt the first usable finisher sample as a baseline without treating mere
                    # telemetry availability as progress. Later decreases accumulate against the
                    # last accepted anchor, so sub-epsilon movement cannot permanently mask a stall.
                    if ($selection.phase -eq 'turnin') {
                        if ([string]::IsNullOrWhiteSpace([string]$state.finisher_phase) -and
                            -not [string]::IsNullOrWhiteSpace($selectedFinisherPhase)) {
                            $state.finisher_phase = $selectedFinisherPhase
                        }
                        if ($null -eq $state.finisher_distance -and $null -ne $selectedFinisherDistance) {
                            $state.finisher_distance = $selectedFinisherDistance
                        }
                    }
                    if ($made) {
                        if (-not [string]::IsNullOrWhiteSpace($selectedFinisherPhase)) { $state.finisher_phase = $selectedFinisherPhase }
                        if ($null -ne $selectedFinisherDistance) { $state.finisher_distance = $selectedFinisherDistance }
                        $state.last_progress_utc = $now; $state.recovery_attempts = 0
                    }
                }

                $idleSeconds = [Math]::Floor(($now - [datetime]$state.last_progress_utc).TotalSeconds)
                # Objective work and turn-ins both need forward progress. A completed quest that
                # cannot locate its turn-in NPC must not remain selected forever just because its
                # objective counter is already full.
                $stalled = ($selection.phase -in @('objective', 'turnin')) -and ($idleSeconds -ge $NoProgressSeconds)

                if ($stalled -and [int]$state.recovery_attempts -lt $MaxRecoveriesPerQuest) {
                    $state.recovery_attempts = [int]$state.recovery_attempts + 1
                    $state.last_progress_utc = $now
                    if ($ObserveOnly) {
                        Write-Receipt -Event 'quest_recovery_would_issue' -Fields @{ team = $team; leader_guid = $guid; quest_id = [int]$selection.quest_id; idle_seconds = [int]$idleSeconds; recovery_attempt = [int]$state.recovery_attempts }
                    }
                    else {
                        $rec = Invoke-BridgeJson -Request "recover $guid"
                        $recoveryOk = [bool](Get-OptionalJsonField -Object $rec -Name 'ok' -Default $false)
                        $reissueCleared = Clear-LastIssuedAfterSuccessfulRecovery -LastIssuedByLeader $lastIssued -LeaderKey $key -RecoveryOk $recoveryOk
                        Write-Receipt -Event 'quest_recovery_issued' -Fields @{ team = $team; leader_guid = $guid; quest_id = [int]$selection.quest_id; idle_seconds = [int]$idleSeconds; recovery_attempt = [int]$state.recovery_attempts; reset_members = [int](Get-OptionalJsonField -Object $rec -Name 'reset_members' -Default 0); ok = $recoveryOk; reissue_cleared = $reissueCleared; error = [string](Get-OptionalJsonField -Object $rec -Name 'error' -Default '') }
                    }
                    continue
                }

                if ($stalled) {
                    # Recoveries exhausted: record the blocked objective in the external backoff ledger.
                    $bkey = "{0}:{1}" -f $guid, [int]$selection.quest_id
                    $backoff[$bkey] = [pscustomobject]@{ until_utc = $now.AddMinutes($BackoffMinutes).ToString('o'); reason = 'no_progress_after_recoveries'; leader_guid = [int]$guid; quest_id = [int]$selection.quest_id }
                    Save-BackoffLedger -Ledger $backoff
                    [void]$backoffSet.Add($bkey)
                    $progressByLeader.Remove($key) | Out-Null
                    Write-Receipt -Event 'quest_backoff_registered' -Fields @{ team = $team; leader_guid = $guid; quest_id = [int]$selection.quest_id; idle_seconds = [int]$idleSeconds; backoff_minutes = $BackoffMinutes; reason = 'no_progress_after_recoveries' }
                    continue
                }

                # Issue-on-change only.
                $issueKey = "$([int]$selection.quest_id):$([string]$selection.phase)"
                if (-not (Test-ShouldIssueDirective -LastIssued ([string]$lastIssued[$key]) -IssueKey $issueKey)) {
                    Write-Receipt -Event 'quest_hold' -Fields @{ team = $team; leader_guid = $guid; quest_id = [int]$selection.quest_id; phase = [string]$selection.phase; idle_seconds = [int]$idleSeconds }
                    continue
                }

                if ($ObserveOnly) {
                    Write-Receipt -Event 'quest_would_issue' -Fields @{ team = $team; leader_guid = $guid; quest_id = [int]$selection.quest_id; phase = [string]$selection.phase; capability_class = [string]$selection.capability_class; reason = [string]$selection.reason }
                }
                else {
                    $res = Invoke-BridgeJson -Request "quest $guid $([int]$selection.quest_id)"
                    $resultOk = [bool](Get-OptionalJsonField -Object $res -Name 'ok' -Default $false)
                    $resultPhase = [string](Get-OptionalJsonField -Object $res -Name 'phase' -Default '')
                    $resultQuest = [int](Get-OptionalJsonField -Object $res -Name 'quest_id' -Default 0)
                    $resultDestination = [string](Get-OptionalJsonField -Object $res -Name 'destination' -Default '')
                    $resultReason = [string](Get-OptionalJsonField -Object $res -Name 'reason' -Default '')
                    $resultError = [string](Get-OptionalJsonField -Object $res -Name 'error' -Default '')
                    Write-Receipt -Event 'quest_issued' -Fields @{ team = $team; leader_guid = $guid; requested_quest = [int]$selection.quest_id; phase = [string]$selection.phase; ok = $resultOk; result_phase = $resultPhase; result_quest = $resultQuest; destination = $resultDestination; reason = $resultReason; error = $resultError }

                    # A command can be transport-valid yet semantically blocked (for example a
                    # completed quest with no discoverable turn-in starter). Treat that as a
                    # bounded failure and move on; never hold a completed-but-unturnable quest
                    # forever. Rejections are handled the same way and remain fully visible in
                    # the receipt.
                    if (-not $resultOk -or $resultPhase -eq 'blocked') {
                        $blockedKey = "{0}:{1}" -f $guid, [int]$selection.quest_id
                        $blockedReason = if ($resultPhase -eq 'blocked') { 'bridge_blocked' } else { 'bridge_rejected' }
                        $backoff[$blockedKey] = [pscustomobject]@{ until_utc = $now.AddMinutes($BackoffMinutes).ToString('o'); reason = $blockedReason; leader_guid = [int]$guid; quest_id = [int]$selection.quest_id }
                        Save-BackoffLedger -Ledger $backoff
                        [void]$backoffSet.Add($blockedKey)
                        $progressByLeader.Remove($key) | Out-Null
                        Write-Receipt -Event 'quest_backoff_registered' -Fields @{ team = $team; leader_guid = $guid; quest_id = [int]$selection.quest_id; idle_seconds = [int]$idleSeconds; backoff_minutes = $BackoffMinutes; reason = $blockedReason; bridge_error = $resultError }
                    }
                    else {
                        $lastIssued[$key] = $issueKey
                    }
                }
                if ($ObserveOnly) { $lastIssued[$key] = $issueKey }
            }
            $failures = 0
        }
        catch {
            $failures++
            Write-Receipt -Event 'director_error' -Fields @{ consecutive_failures = $failures; error = $_.Exception.Message; line = $_.InvocationInfo.ScriptLineNumber; statement = ([string]$_.InvocationInfo.Line).Trim() }
            if ($failures -ge 3) { throw "Director stopped after $failures consecutive failures: $($_.Exception.Message)" }
        }
        Start-Sleep -Seconds $ScoutEverySeconds
    }
    Write-Receipt -Event 'quest_director_v1_completed'
}
finally {
    Write-Receipt -Event 'quest_director_v1_stopped'
    try { Save-BackoffLedger -Ledger $backoff } catch { }
}

Write-Output "Quest Director V1 receipt: $ReceiptPath"
