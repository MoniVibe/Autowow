<#
    Fail-closed live-proof fixture for quest 786, Thwarting Kolkar Aggression.

    Dry-run is the default and performs no bridge calls. Apply is restricted to
    one exact fixture party and a named route beside the legitimate quest giver.
#>
[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [ValidateRange(1, 60)][int]$DurationMinutes = 20,
    [ValidateRange(200, 5000)][int]$PollMilliseconds = 500,
    [string]$ReceiptPath = '',
    [string]$FixtureManifestPath = '',
    [switch]$Apply
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$QuestId = [uint32]786
$QuestTitle = 'Thwarting Kolkar Aggression'
$QuestGiverEntry = [uint32]3140
$QuestGiverSpawnGuid = [uint32]4699
$StageRoute = 'q786-lar-prowltusk'
$StageRouteAvailable = $true
$NativeGameObjectCreditPath = 'CMSG_GAMEOBJ_USE'
$ProofGameObjectEntries = [uint32[]]@(3189, 3190)
$AllQuestGameObjectEntries = [uint32[]]@(3189, 3190, 3192)
$MissingStagePrerequisite =
    "AutoWow bridge route '$StageRoute' is missing. It must stage the exact party beside Lar Prowltusk " +
    "(creature entry $QuestGiverEntry, spawn GUID $QuestGiverSpawnGuid) so q786 can be accepted through " +
    'normal quest-giver interaction; existing routes do not provide that exact staging surface.'

$ServerRoot = [System.IO.Path]::GetFullPath($ServerRoot)
if ([string]::IsNullOrWhiteSpace($FixtureManifestPath)) {
    $FixtureManifestPath = Join-Path $ServerRoot 'logs\q786-fixture-manifest.json'
}
$FixtureManifestPath = [System.IO.Path]::GetFullPath($FixtureManifestPath)
if (-not (Test-Path -LiteralPath $FixtureManifestPath -PathType Leaf)) {
    throw "Dedicated q786 fixture manifest is missing: $FixtureManifestPath"
}
$fixtureManifest = Get-Content -LiteralPath $FixtureManifestPath -Raw | ConvertFrom-Json -Depth 30
if ([string]$fixtureManifest.schema -ne 'autowow.q786.fixture-provision.manifest.v1' -or
    [string]$fixtureManifest.status -ne 'READY_FOR_Q786_FIXTURE_INIT' -or
    [uint32]$fixtureManifest.quest.id -ne $QuestId -or
    -not [bool]$fixtureManifest.freshness.level_one_before_fixture_init -or
    [int]$fixtureManifest.freshness.inventory_rows -ne 0 -or
    [int]$fixtureManifest.freshness.quest_log_rows -ne 0 -or
    [int]$fixtureManifest.freshness.rewarded_quest_rows -ne 0) {
    throw 'Dedicated q786 fixture manifest failed schema, status, quest, or freshness validation.'
}
$ownerManifest = @($fixtureManifest.members | Where-Object {
    [string]$_.role -eq 'owner' -and [string]$_.account_name -eq 'AWQ786OWNER' -and
    [string]$_.character_name -eq 'Kolkarown'
})
$helperManifest = @($fixtureManifest.members | Where-Object {
    [string]$_.role -eq 'helper' -and [string]$_.account_name -eq 'AWQ786HELPER' -and
    [string]$_.character_name -eq 'Kolkarhelp'
})
if ($ownerManifest.Count -ne 1 -or $helperManifest.Count -ne 1) {
    throw 'Dedicated q786 fixture manifest must contain the exact owner and helper identities.'
}
$OwnerGuid = [uint32]$fixtureManifest.owner_guid
$HelperGuid = [uint32]$fixtureManifest.helper_guid
if ($OwnerGuid -eq 0 -or $HelperGuid -eq 0 -or $OwnerGuid -eq $HelperGuid -or
    [uint32]$ownerManifest[0].character_guid -ne $OwnerGuid -or
    [uint32]$helperManifest[0].character_guid -ne $HelperGuid) {
    throw 'Dedicated q786 fixture manifest contains invalid or inconsistent GUIDs.'
}
if ([string]::IsNullOrWhiteSpace($ReceiptPath)) {
    $ReceiptPath = Join-Path $ServerRoot ('logs\phase1-{0}\q786-interact-live.jsonl' -f (Get-Date -Format 'yyyyMMdd'))
}
$ReceiptPath = [System.IO.Path]::GetFullPath($ReceiptPath)

$questObjectives = @(
    [pscustomobject][ordered]@{
        slot = 0; kind = 'gameobject_credit'; signed_entry = -3189; entry = 3189
        required = 1; spawn_guid = 12388; native_path_proof_required = $true
    },
    [pscustomobject][ordered]@{
        slot = 1; kind = 'gameobject_credit'; signed_entry = -3190; entry = 3190
        required = 1; spawn_guid = 12389; native_path_proof_required = $true
    },
    [pscustomobject][ordered]@{
        slot = 2; kind = 'gameobject_credit'; signed_entry = -3192; entry = 3192
        required = 1; spawn_guid = 12390; native_path_proof_required = $false
    }
)

$mutationAllowlist = @(
    "activate $OwnerGuid",
    "activate $HelperGuid",
    "fixture init $OwnerGuid 8 0 2",
    "fixture init $HelperGuid 7 0 2",
    "party $OwnerGuid $HelperGuid",
    "route $OwnerGuid $StageRoute",
    "quest $OwnerGuid",
    "quest $OwnerGuid $QuestId"
)

$plan = [pscustomobject][ordered]@{
    schema = 'autowow.interact-fixture-q786.plan.v1'
    applies = [bool]$Apply
    apply_ready = [bool]$StageRouteAvailable
    quest = $QuestId
    title = $QuestTitle
    owner_guid = $OwnerGuid
    helper_guid = $HelperGuid
    fixture_manifest = $FixtureManifestPath
    fixture_guids = @($OwnerGuid, $HelperGuid)
    fixture_facts = @(
        [pscustomobject][ordered]@{ guid = $OwnerGuid; role = 'owner'; level = 8; spec_index = 0; quality = 2 },
        [pscustomobject][ordered]@{ guid = $HelperGuid; role = 'helper'; level = 7; spec_index = 0; quality = 2 }
    )
    quest_giver = [pscustomobject][ordered]@{
        entry = $QuestGiverEntry
        spawn_guid = $QuestGiverSpawnGuid
        name = 'Lar Prowltusk'
        map = 1
        x = -774.774
        y = -4830.36
        z = 19.8326
    }
    stage = [pscustomobject][ordered]@{
        route = $StageRoute
        available = [bool]$StageRouteAvailable
        prerequisite = $MissingStagePrerequisite
    }
    quest_objectives = $questObjectives
    required_native_go_proof_entries = $ProofGameObjectEntries
    native_gameobject_credit_path = $NativeGameObjectCreditPath
    objective_execution_invariants = [pscustomobject][ordered]@{
        teleport_delta = 0
        random_grind_fallback_delta = 0
        unrelated_offensive_target_delta = 0
        direct_quest_db_mutation_delta = 0
        unrelated_objective_progress_count = 0
    }
    mutation_surfaces = @('activate', 'fixture-init', 'party', 'route', 'quest')
    mutation_allowlist = $mutationAllowlist
    max_turnin_directives = 1
    post_turnin_requests_read_only = $true
    read_requests = @(
        'list',
        "fixture status $OwnerGuid",
        "fixture status $HelperGuid",
        "questlog $OwnerGuid",
        "questobjective $OwnerGuid",
        "acceptance $OwnerGuid $QuestId",
        "snapshot $OwnerGuid"
    )
    planned_steps = @(
        'Confirm both exact fixture GUIDs are ungrouped and fixture-allowlisted.',
        'Activate only an exact fixture GUID that is offline.',
        "Initialize only GUID $OwnerGuid at level 8 and GUID $HelperGuid at level 7.",
        "Form exactly party $OwnerGuid $HelperGuid.",
        "Use only the named bridge route '$StageRoute' to stage beside the legitimate q786 giver.",
        'Accept q786 through normal quest-giver interaction with the generic quest action.',
        'Direct the accepted q786 through the explicit quest action; do not grant or complete it.',
        'Consume monotonic direct_go_receipts from questobjective while polling read-only telemetry.',
        'Require successful 0->1 CMSG_GAMEOBJ_USE receipts for GO 3189 and GO 3190.',
        'Preserve a matching completed native directive; otherwise issue at most one turn-in directive, then only poll.',
        'Require the full q786 objective contract, normal completion, and normal reward.',
        'Fail unless objective-time invariant deltas and unrelated objective progress are zero.'
    )
    direct_quest_or_character_database_writes = 0
    evidence = $ReceiptPath
    blocked_by = if ($StageRouteAvailable) { '' } else { $MissingStagePrerequisite }
}

if (-not $Apply) {
    $plan | ConvertTo-Json -Depth 20
    return
}

# This guard intentionally precedes every dot-source, bridge request, directory
# creation, activation, fixture initialization, party operation, and route call.
if (-not $StageRouteAvailable) {
    throw "Apply blocked before any bridge mutation: $MissingStagePrerequisite"
}

$control = Join-Path $PSScriptRoot 'autowow-control.ps1'
$reducer = Join-Path $PSScriptRoot 'interact-fixture-q786.reducer.ps1'
foreach ($requiredFile in @($control, $reducer)) {
    if (-not (Test-Path -LiteralPath $requiredFile)) {
        throw "Required file is missing: $requiredFile"
    }
}

. $reducer
. (Join-Path $PSScriptRoot 'Common.ps1')
. (Join-Path $PSScriptRoot 'QuestLogSource.ps1')

function Write-Q786Evidence {
    param(
        [Parameter(Mandatory = $true)][string]$Event,
        [AllowNull()][object]$Data = $null
    )

    $directory = Split-Path -Parent $ReceiptPath
    if (-not (Test-Path -LiteralPath $directory)) {
        [void](New-Item -ItemType Directory -Path $directory -Force)
    }
    $row = [pscustomobject][ordered]@{
        timestamp_utc = [DateTimeOffset]::UtcNow.ToString('o')
        event = $Event
        data = $Data
    }
    [System.IO.File]::AppendAllText(
        $ReceiptPath,
        (($row | ConvertTo-Json -Compress -Depth 100) + [Environment]::NewLine),
        [System.Text.UTF8Encoding]::new($false))
}

function Invoke-Q786Control {
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet('list', 'activate', 'fixture-init', 'fixture-status', 'party', 'route', 'quest')]
        [string]$Action,
        [uint32]$Guid = 0,
        [uint32[]]$Members = @(),
        [uint32]$Level = 1,
        [uint32]$Quest = 0
    )

    if ($Guid -ne 0 -and $Guid -notin @($OwnerGuid, $HelperGuid)) {
        throw "Control action '$Action' refused non-fixture GUID $Guid."
    }

    $parameters = @{ Action = $Action; BotGuid = $Guid }
    switch ($Action) {
        'fixture-init' {
            if ($Guid -eq $OwnerGuid -and $Level -ne 8) { throw 'Owner fixture level must be exactly 8.' }
            if ($Guid -eq $HelperGuid -and $Level -ne 7) { throw 'Helper fixture level must be exactly 7.' }
            $parameters.Level = $Level
            $parameters.SpecIndex = 0
            $parameters.Quality = 2
        }
        'party' {
            if ($Guid -ne $OwnerGuid -or @($Members).Count -ne 1 -or $Members[0] -ne $HelperGuid) {
                throw "Party mutation must be exactly party $OwnerGuid $HelperGuid."
            }
            $parameters.MemberGuid = $Members
        }
        'route' {
            if ($Guid -ne $OwnerGuid) { throw 'Only the owner may issue the q786 stage route.' }
            $parameters.Destination = $StageRoute
        }
        'quest' {
            if ($Guid -ne $OwnerGuid -or $Quest -notin @(0, $QuestId)) {
                throw 'Quest mutation must be the generic owner action or explicit q786 action.'
            }
            if ($Quest -ne 0) { $parameters.QuestId = $Quest }
        }
    }

    $response = ((& $control @parameters | Out-String).Trim() | ConvertFrom-Json -Depth 100)
    if ($null -eq $response) { throw "Control action '$Action' returned no response." }
    return $response
}

function Invoke-Q786Read {
    param([Parameter(Mandatory = $true)][ValidateSet('questlog', 'questobjective', 'acceptance', 'snapshot')][string]$Kind)

    $request = switch ($Kind) {
        'questlog' { "questlog $OwnerGuid" }
        'questobjective' { "questobjective $OwnerGuid" }
        'acceptance' { "acceptance $OwnerGuid $QuestId" }
        'snapshot' { "snapshot $OwnerGuid" }
    }
    $response = Send-Bridge -Request $request | ConvertFrom-Json -Depth 100
    if (-not [bool]$response.ok) {
        throw "Read-only bridge request '$request' failed: $($response | ConvertTo-Json -Compress -Depth 20)"
    }
    return $response
}

function Invoke-Q786FixtureInitWithRetry {
    param(
        [Parameter(Mandatory = $true)][uint32]$Guid,
        [Parameter(Mandatory = $true)][uint32]$Level
    )

    $last = $null
    for ($attempt = 1; $attempt -le 10; $attempt++) {
        $last = Invoke-Q786Control -Action fixture-init -Guid $Guid -Level $Level
        if ([bool]$last.ok) { return $last }
        if ($attempt -lt 10) { Start-Sleep -Seconds 2 }
    }
    throw "Fixture initialization for GUID $Guid did not clear its transient state: $($last | ConvertTo-Json -Compress -Depth 20)"
}

function Get-Q786FromQuestLog {
    param([Parameter(Mandatory = $true)][object]$QuestLog)
    return @($QuestLog.quests | Where-Object { [uint32]$_.id -eq $QuestId })
}

function Assert-Q786QuestContract {
    param(
        [Parameter(Mandatory = $true)][object]$Quest,
        [switch]$RequireZero,
        [switch]$RequireComplete
    )

    $objectives = @($Quest.objectives)
    if ($objectives.Count -ne 3) { throw "q786 must expose exactly three objectives; observed $($objectives.Count)." }
    $observedEntries = @($objectives | ForEach-Object { [uint32]$_.entry } | Sort-Object)
    if (($observedEntries -join ',') -ne (($AllQuestGameObjectEntries | Sort-Object) -join ',')) {
        throw "q786 objective entries do not match 3189,3190,3192: $($observedEntries -join ',')."
    }
    foreach ($objective in $objectives) {
        if ([string]$objective.kind -ne 'gameobject' -or [uint32]$objective.required -ne 1) {
            throw "q786 entry $($objective.entry) is not an exact GameObject 1/1 contract."
        }
        if ($RequireZero -and [uint32]$objective.current -ne 0) {
            throw "q786 entry $($objective.entry) was not clean at acceptance."
        }
        if ($RequireComplete -and [uint32]$objective.current -ne 1) {
            throw "q786 entry $($objective.entry) did not reach 1/1."
        }
    }
}

function Get-Q786InvariantValue {
    param([AllowNull()][object]$Acceptance, [Parameter(Mandatory = $true)][string]$Name)
    if ($null -eq $Acceptance -or $null -eq $Acceptance.PSObject.Properties['invariants']) { return [int64]0 }
    $property = $Acceptance.invariants.PSObject.Properties[$Name]
    if ($null -eq $property -or $null -eq $property.Value) { return [int64]0 }
    return [int64]$property.Value
}

function Get-UnrelatedObjectiveCounts {
    param([Parameter(Mandatory = $true)][object]$QuestLog)

    $counts = @{}
    foreach ($quest in @($QuestLog.quests)) {
        if ([uint32]$quest.id -eq $QuestId) { continue }
        foreach ($objective in @($quest.objectives)) {
            $key = '{0}|{1}|{2}' -f [uint32]$quest.id, [string]$objective.kind, [int64]$objective.entry
            $counts[$key] = [int64]$objective.current
        }
    }
    return $counts
}

function Get-UnrelatedObjectiveProgress {
    param(
        [Parameter(Mandatory = $true)][hashtable]$Baseline,
        [Parameter(Mandatory = $true)][hashtable]$Final
    )

    $progress = [System.Collections.Generic.List[object]]::new()
    $keys = @($Baseline.Keys) + @($Final.Keys) | Sort-Object -Unique
    foreach ($key in $keys) {
        $before = if ($Baseline.ContainsKey($key)) { [int64]$Baseline[$key] } else { [int64]0 }
        $after = if ($Final.ContainsKey($key)) { [int64]$Final[$key] } else { [int64]0 }
        if ($after -gt $before) {
            $progress.Add([pscustomobject][ordered]@{ objective = $key; before = $before; after = $after })
        }
    }
    return @($progress)
}

# Live Apply implementation. Every operation remains constrained to the plan's explicit GUIDs.
$list = Invoke-Q786Control -Action list
$fixtureStates = @($list.bots | Where-Object { [uint32]$_.guid -in @($OwnerGuid, $HelperGuid) })
foreach ($state in $fixtureStates) {
    if ([int]$state.group.members -ne 0) {
        throw "Fixture GUID $($state.guid) is already grouped; refusing to disturb a foreign party."
    }
}

$online = @($list.bots | ForEach-Object { [uint32]$_.guid })
foreach ($guid in @($OwnerGuid, $HelperGuid)) {
    if ($guid -notin $online) {
        $activation = Invoke-Q786Control -Action activate -Guid $guid
        Write-Q786Evidence -Event 'fixture_activation_queued' -Data $activation
    }
}

$loginDeadline = [DateTimeOffset]::UtcNow.AddSeconds(90)
do {
    Start-Sleep -Milliseconds 500
    $list = Invoke-Q786Control -Action list
    $online = @($list.bots | ForEach-Object { [uint32]$_.guid })
    $missingFixtureGuids = @(@($OwnerGuid, $HelperGuid) | Where-Object { $_ -notin $online })
} while ($missingFixtureGuids.Count -gt 0 -and
    [DateTimeOffset]::UtcNow -lt $loginDeadline)
if ($missingFixtureGuids.Count -gt 0) {
    throw 'The exact q786 fixture bots did not log in within 90 seconds.'
}

$ownerStatus = Invoke-Q786Control -Action fixture-status -Guid $OwnerGuid
$helperStatus = Invoke-Q786Control -Action fixture-status -Guid $HelperGuid
if (-not [bool]$ownerStatus.ok -or -not [bool]$helperStatus.ok) {
    throw 'The exact q786 GUIDs are not both accepted by the fixture allowlist.'
}
Write-Q786Evidence -Event 'fixture_preflight' -Data @{ owner = $ownerStatus; helper = $helperStatus }

$ownerInit = Invoke-Q786FixtureInitWithRetry -Guid $OwnerGuid -Level 8
$helperInit = Invoke-Q786FixtureInitWithRetry -Guid $HelperGuid -Level 7
if (-not [bool]$ownerInit.ok -or -not [bool]$helperInit.ok) {
    throw 'Exact fixture initialization failed.'
}
Write-Q786Evidence -Event 'fixture_initialized' -Data @{ owner = $ownerInit; helper = $helperInit }

$party = Invoke-Q786Control -Action party -Guid $OwnerGuid -Members @($HelperGuid)
if (-not [bool]$party.ok) { throw 'Exact q786 fixture party creation failed.' }
$route = Invoke-Q786Control -Action route -Guid $OwnerGuid
if (-not [bool]$route.ok) { throw "Exact q786 stage route '$StageRoute' failed." }
Write-Q786Evidence -Event 'fixture_staged' -Data @{ party = $party; route = $route; objective_execution_started = $false }

$accepted = $false
for ($attempt = 1; $attempt -le 4 -and -not $accepted; $attempt++) {
    $acceptOrder = Invoke-Q786Control -Action quest -Guid $OwnerGuid
    Write-Q786Evidence -Event 'quest_accept_attempt' -Data @{ attempt = $attempt; response = $acceptOrder }
    Start-Sleep -Seconds 2
    $acceptedLog = Invoke-Q786Read -Kind questlog
    $acceptedQuest = @(Get-Q786FromQuestLog -QuestLog $acceptedLog)
    $accepted = $acceptedQuest.Count -eq 1
}
if (-not $accepted) { throw "Owner GUID $OwnerGuid did not accept q786 through normal quest-giver interaction." }
Assert-Q786QuestContract -Quest $acceptedQuest[0] -RequireZero
Write-Q786Evidence -Event 'quest_accepted' -Data @{
    owner_guid = $OwnerGuid
    quest_id = $QuestId
    normal_quest_giver_interaction = $true
    quest = $acceptedQuest[0]
}

$baselineAcceptance = Invoke-Q786Read -Kind acceptance
$baselineQuestLog = Invoke-Q786Read -Kind questlog
$baselineUnrelated = Get-UnrelatedObjectiveCounts -QuestLog $baselineQuestLog
$baselineDirectGoReceiptSequence = Get-Q786MaxDirectGoReceiptSequence -Receipts @($baselineAcceptance.direct_go_receipts)
$baselineInvariants = [ordered]@{
    unrelated_offensive_target_count = Get-Q786InvariantValue $baselineAcceptance 'unrelated_offensive_target_count'
    random_grind_fallback_count = Get-Q786InvariantValue $baselineAcceptance 'random_grind_fallback_count'
    teleport_count = Get-Q786InvariantValue $baselineAcceptance 'teleport_count'
    direct_quest_db_mutation_count = Get-Q786InvariantValue $baselineAcceptance 'direct_quest_db_mutation_count'
}
Write-Q786Evidence -Event 'objective_baseline' -Data @{
    acceptance = $baselineAcceptance
    questlog = $baselineQuestLog
    direct_go_receipt_sequence = $baselineDirectGoReceiptSequence
    invariants = $baselineInvariants
}

$objectiveOrder = Invoke-Q786Control -Action quest -Guid $OwnerGuid -Quest $QuestId
if (-not [bool]$objectiveOrder.ok -or [string]$objectiveOrder.phase -eq 'blocked') {
    throw "Explicit q786 objective directive failed: $($objectiveOrder | ConvertTo-Json -Compress -Depth 20)"
}
Write-Q786Evidence -Event 'objective_execution_started' -Data @{
    directive = $objectiveOrder
    native_gameobject_credit_path = $NativeGameObjectCreditPath
}

$proof = New-Q786DirectGoProofState -BaselineSequence $baselineDirectGoReceiptSequence
$rewarded = $false
$sawComplete = $false
$turnInDirectiveIssued = $false
$turnInDirectiveCount = 0
$sample = 0
$deadline = [DateTimeOffset]::UtcNow.AddMinutes($DurationMinutes)

while ([DateTimeOffset]::UtcNow -lt $deadline) {
    $sample++
    $objectiveResponse = Invoke-Q786Read -Kind questobjective
    $acceptance = Invoke-Q786Read -Kind acceptance
    $questLog = Invoke-Q786Read -Kind questlog
    $snapshot = Invoke-Q786Read -Kind snapshot
    Update-Q786DirectGoProof -State $proof -Receipts @($objectiveResponse.direct_go_receipts)
    $proofView = Get-Q786DirectGoProofView -State $proof

    $invariantDeltas = [ordered]@{
        unrelated_offensive_target_count =
            (Get-Q786InvariantValue $acceptance 'unrelated_offensive_target_count') - $baselineInvariants.unrelated_offensive_target_count
        random_grind_fallback_count =
            (Get-Q786InvariantValue $acceptance 'random_grind_fallback_count') - $baselineInvariants.random_grind_fallback_count
        teleport_count =
            (Get-Q786InvariantValue $acceptance 'teleport_count') - $baselineInvariants.teleport_count
        direct_quest_db_mutation_count =
            (Get-Q786InvariantValue $acceptance 'direct_quest_db_mutation_count') - $baselineInvariants.direct_quest_db_mutation_count
    }
    if (@($invariantDeltas.Values | Where-Object { [int64]$_ -ne 0 }).Count -ne 0) {
        Write-Q786Evidence -Event 'invariant_failure' -Data $invariantDeltas
        throw 'Objective execution changed a forbidden acceptance invariant.'
    }

    $activeQuest = @(Get-Q786FromQuestLog -QuestLog $questLog)
    if ($activeQuest.Count -eq 1 -and [bool]$activeQuest[0].is_complete) {
        $sawComplete = $true
        Assert-Q786QuestContract -Quest $activeQuest[0] -RequireComplete

        $directiveActive = [bool]$acceptance.directive_active
        $directiveQuestId = [uint32]$acceptance.directive_quest_id
        if ($directiveActive -and $directiveQuestId -ne $QuestId) {
            throw "Completed q786 observed while directive $directiveQuestId is active; refusing to replace it."
        }
        if (-not $directiveActive -and -not $turnInDirectiveIssued) {
            # Set the one-shot guard before dispatch. If the bridge call fails, the script stops and
            # cannot accidentally send a second directive after an ambiguous first result.
            $turnInDirectiveIssued = $true
            $turnInDirectiveCount = 1
            $turnIn = Invoke-Q786Control -Action quest -Guid $OwnerGuid -Quest $QuestId
            Write-Q786Evidence -Event 'quest_turnin_directive_issued' -Data $turnIn
        }
    }

    $rewarded = [bool]$acceptance.reward.reward_status
    Write-Q786Evidence -Event 'objective_sample' -Data @{
        sample = $sample
        objective = $objectiveResponse
        acceptance = $acceptance
        questlog = $questLog
        snapshot = $snapshot
        native_go_proof = $proofView
        turnin_directive_count = $turnInDirectiveCount
        invariant_deltas = $invariantDeltas
    }
    if ($rewarded) { break }
    Start-Sleep -Milliseconds $PollMilliseconds
}

$finalAcceptance = Invoke-Q786Read -Kind acceptance
$finalQuestLog = Invoke-Q786Read -Kind questlog
$finalUnrelated = Get-UnrelatedObjectiveCounts -QuestLog $finalQuestLog
$unrelatedProgress = @(Get-UnrelatedObjectiveProgress -Baseline $baselineUnrelated -Final $finalUnrelated)
$finalInvariantDeltas = [ordered]@{
    unrelated_offensive_target_count =
        (Get-Q786InvariantValue $finalAcceptance 'unrelated_offensive_target_count') - $baselineInvariants.unrelated_offensive_target_count
    random_grind_fallback_count =
        (Get-Q786InvariantValue $finalAcceptance 'random_grind_fallback_count') - $baselineInvariants.random_grind_fallback_count
    teleport_count =
        (Get-Q786InvariantValue $finalAcceptance 'teleport_count') - $baselineInvariants.teleport_count
    direct_quest_db_mutation_count =
        (Get-Q786InvariantValue $finalAcceptance 'direct_quest_db_mutation_count') - $baselineInvariants.direct_quest_db_mutation_count
}
$nativeProofPassed = Test-Q786DirectGoProofComplete -State $proof
$finalProofView = Get-Q786DirectGoProofView -State $proof
$invariantsPassed = @($finalInvariantDeltas.Values | Where-Object { [int64]$_ -ne 0 }).Count -eq 0
$rewarded = [bool]$finalAcceptance.reward.reward_status
$passed = $rewarded -and $sawComplete -and $nativeProofPassed -and $invariantsPassed -and $unrelatedProgress.Count -eq 0

$verdict = [pscustomobject][ordered]@{
    schema = 'autowow.interact-fixture-q786.verdict.v1'
    passed = $passed
    owner_guid = $OwnerGuid
    helper_guid = $HelperGuid
    quest_id = $QuestId
    normal_acceptance_observed = $true
    native_completion_observed = $sawComplete
    normal_reward_observed = $rewarded
    native_gameobject_credit_path = $NativeGameObjectCreditPath
    native_go_proof = $finalProofView
    turnin_directive_issued = $turnInDirectiveIssued
    turnin_directive_count = $turnInDirectiveCount
    post_turnin_requests_read_only = $true
    invariant_deltas = $finalInvariantDeltas
    unrelated_objective_progress = $unrelatedProgress
    direct_quest_or_character_database_writes = 0
    evidence = $ReceiptPath
}
Write-Q786Evidence -Event 'verdict' -Data $verdict
$verdict | ConvertTo-Json -Depth 20
if (-not $passed) { exit 2 }
