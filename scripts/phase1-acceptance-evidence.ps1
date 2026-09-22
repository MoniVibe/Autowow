<#
.SYNOPSIS
    Phase 1 acceptance-evidence harness — emits the read-only versioned stream
    `autowow.phase1.acceptance.evidence.v1` during the maintenance-window live proof.

.DESCRIPTION
    Read-only. Each cycle polls `acceptance <guid>` for the objective snapshot, reward postcondition,
    and engine-side violation guards, then appends versioned JSONL. It issues no control order and
    performs no database mutation. Counters are process-lifetime values; the harness captures a baseline
    before the run and gates on deltas, never by resetting server state. Requires the rebuilt server that
    exposes `acceptance`.

    Release gate (autowow.phase1.acceptance.evidence.v1) PASS iff, over the run:
      unrelated_offensive_target_count == 0, random_grind_fallback_count == 0, teleport_count == 0,
      direct_quest_db_mutation_count == 0, and every REQUESTED fixture shows real baseline<current->required
      progression with a resolved finisher and reward postcondition turnin_verified, while
      objective_context_present held whenever an objective was acted on.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][uint32[]]$Leaders,
    [Parameter(Mandatory = $true)][int[]]$Fixtures,
    [ValidateRange(5, 300)][int]$IntervalSeconds = 15,
    [ValidateRange(1, 1440)][int]$DurationMinutes = 60,
    [string]$RunId = '',
    [string]$ReceiptPath = '',
    [string]$BridgeHost = '127.0.0.1',
    [int]$Port = 18787
)

$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($RunId)) { $RunId = 'phase1-accept-' + (Get-Date -Format 'yyyyMMdd-HHmmss') }
if ([string]::IsNullOrWhiteSpace($ReceiptPath)) { $ReceiptPath = Join-Path (Get-Location) ("$RunId.jsonl") }
New-Item -ItemType Directory -Path (Split-Path -Parent $ReceiptPath) -Force | Out-Null
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
$script:Seq = 0

function Send-Bridge {
    param([Parameter(Mandatory = $true)][string]$Request)
    $client = [System.Net.Sockets.TcpClient]::new()
    try {
        if (-not $client.ConnectAsync($BridgeHost, $Port).Wait(4000) -or -not $client.Connected) { throw "bridge unreachable" }
        $s = $client.GetStream(); $s.ReadTimeout = 4000
        $w = [System.IO.StreamWriter]::new($s); $w.NewLine = "`n"; $w.WriteLine($Request); $w.Flush()
        $line = ([System.IO.StreamReader]::new($s)).ReadLine()
        if ([string]::IsNullOrWhiteSpace($line)) { throw 'empty bridge response' }
        return $line
    }
    finally { $client.Dispose() }
}

function Emit {
    param([Parameter(Mandatory = $true)][hashtable]$Record)
    $script:Seq++
    $env = [ordered]@{
        schema = 'autowow.phase1.acceptance.evidence.v1'; schema_version = 1
        run_id = $RunId; seq = $script:Seq; timestamp_utc = (Get-Date).ToUniversalTime().ToString('o'); read_only = $true
    }
    foreach ($k in $Record.Keys) { $env[$k] = $Record[$k] }
    if ($Record.ContainsKey('kind')) { $env['event'] = "phase1_acceptance_$($Record['kind'])" }
    $json = ([pscustomobject]$env | ConvertTo-Json -Depth 12 -Compress)
    [System.IO.File]::AppendAllText($ReceiptPath, $json + [Environment]::NewLine, $utf8NoBom)
    Write-Output $json
}

# Per-fixture accumulators keyed "guid:quest".
$fx = @{}
function Key([uint32]$g, [int]$q) { "$g`:$q" }

function Copy-InvariantCounters {
    param([Parameter(Mandatory = $true)]$Counters)
    return [ordered]@{
        unrelated_offensive_target_count = [int64]$Counters.unrelated_offensive_target_count
        random_grind_fallback_count = [int64]$Counters.random_grind_fallback_count
        teleport_count = [int64]$Counters.teleport_count
        # This is a static surface assertion (the bridge/harness have no direct quest-table write path),
        # not a dynamic engine event counter. It remains in the stream as a release invariant.
        direct_quest_db_mutation_count = [int64]$Counters.direct_quest_db_mutation_count
    }
}

function Get-InvariantDelta {
    param(
        [Parameter(Mandatory = $true)]$Current,
        [Parameter(Mandatory = $true)]$Baseline
    )
    $now = Copy-InvariantCounters $Current
    [ordered]@{
        unrelated_offensive_target_count = $now.unrelated_offensive_target_count - [int64]$Baseline.unrelated_offensive_target_count
        random_grind_fallback_count = $now.random_grind_fallback_count - [int64]$Baseline.random_grind_fallback_count
        teleport_count = $now.teleport_count - [int64]$Baseline.teleport_count
        direct_quest_db_mutation_count = $now.direct_quest_db_mutation_count - [int64]$Baseline.direct_quest_db_mutation_count
    }
}

function Test-CleanInvariants {
    param($Counters)
    return $Counters -and ([int64]$Counters.unrelated_offensive_target_count -eq 0) -and
        ([int64]$Counters.random_grind_fallback_count -eq 0) -and
        ([int64]$Counters.teleport_count -eq 0) -and
        ([int64]$Counters.direct_quest_db_mutation_count -eq 0)
}

$requestedFixtures = @($Fixtures | Select-Object -Unique)
$counterBaseline = $null
foreach ($g in $Leaders) {
    try {
        $baselineResponse = Send-Bridge -Request "acceptance $g" | ConvertFrom-Json
        if ($baselineResponse.ok -and $null -ne $baselineResponse.invariants) {
            $counterBaseline = Copy-InvariantCounters $baselineResponse.invariants
            break
        }
    }
    catch { }
}

Emit @{
    kind = 'run_started'; fixtures = @($requestedFixtures); leaders = @($Leaders)
    source = 'isolated maintenance-window proof'; invariant_counter_baseline = $counterBaseline
    counter_scope = 'process_lifetime_baselined_to_run'
}

$deadline = (Get-Date).AddMinutes($DurationMinutes)
try {
    while ((Get-Date) -lt $deadline) {
        foreach ($g in $Leaders) {
            try {
                $acc = Send-Bridge -Request "acceptance $g" | ConvertFrom-Json
            }
            catch { Emit @{ kind = 'collector_error'; bot_guid = [uint32]$g; error = $_.Exception.Message }; continue }
            if (-not $acc.ok) {
                Emit @{ kind = 'collector_error'; bot_guid = [uint32]$g; error = [string]$acc.error }
                continue
            }
            $obj = $acc.objective
            $qid = [int]$obj.quest_id
            if ($qid -notin $requestedFixtures) { continue }   # only record the fixtures under proof

            $k = Key $g $qid
            if (-not $fx.ContainsKey($k)) {
                $fx[$k] = @{
                    baseline = [int]$obj.current_count; progressed = $false; completed = $false
                    finisher_resolved = $false; reward = $false; reward_initial = [bool]$acc.reward.reward_status
                    teleport = $false; unrelated = $false; context_missing = -not [bool]$acc.objective_context_present
                }
            }
            $state = $fx[$k]

            if ($null -eq $counterBaseline) {
                Emit @{ kind = 'collector_error'; bot_guid = [uint32]$g; error = 'invariant_counter_baseline_unavailable' }
                continue
            }
            $inv = Get-InvariantDelta -Current $acc.invariants -Baseline $counterBaseline
            if ([int64]$inv.teleport_count -gt 0) { $state.teleport = $true }
            if ([int64]$inv.unrelated_offensive_target_count -gt 0) { $state.unrelated = $true }
            if (-not [bool]$acc.objective_context_present) { $state.context_missing = $true }
            if ([int]$obj.current_count -gt [int]$state.baseline) { $state.progressed = $true }
            if ($state.progressed -and [int]$obj.current_count -ge [int]$obj.required_count -and [int]$obj.required_count -gt 0) { $state.completed = $true }
            if ([int]$acc.reward.finisher_entry -ne 0 -and [bool]$acc.reward.relation_verified) { $state.finisher_resolved = $true }
            if (-not $state.reward_initial -and [bool]$acc.reward.reward_status) { $state.reward = $true }

            Emit @{
                kind = 'fixture_sample'; bot_guid = [uint32]$g; bot_name = ''; quest_id = $qid
                objective_context_present = [bool]$acc.objective_context_present
                objective = @{
                    family = [string]$obj.objective_family; slot = [int]$obj.objective_slot; kind = [string]$obj.objective_kind
                    phase = [string]$obj.phase; failure_reason = [string]$obj.failure_reason
                    supported = [bool]$obj.supported; required_entry = [int]$obj.required_npc_or_go_entry; required_item = [int]$obj.required_item_id
                    baseline_count = [int]$state.baseline; current_count = [int]$obj.current_count; required_count = [int]$obj.required_count
                    progress_delta = ([int]$obj.current_count - [int]$state.baseline)
                }
                selected_source_entry = [int]$obj.selected_source_entry; selected_target_guid = [int]$obj.selected_target_guid
                finisher = @{ entry = [int]$acc.reward.finisher_entry; guid = [int]$acc.reward.finisher_guid; relation_verified = [bool]$acc.reward.relation_verified }
                reward = @{ quest_status = [int]$acc.reward.quest_status; can_reward = [bool]$acc.reward.can_reward; reward_status = [bool]$acc.reward.reward_status; initially_rewarded = [bool]$state.reward_initial; turnin_verified = [bool]$state.reward }
                provenance = @{ objective_counts = 'acceptance'; reward_status = 'acceptance'; invariant_counters = 'engine_authoritative_process_lifetime_baselined' }
            }
            Emit @{
                kind = 'invariants'; window = 'run_delta'; counter_baseline = $counterBaseline
                unrelated_offensive_target_count = [int64]$inv.unrelated_offensive_target_count
                random_grind_fallback_count = [int64]$inv.random_grind_fallback_count
                teleport_count = [int64]$inv.teleport_count
                direct_quest_db_mutation_count = [int64]$inv.direct_quest_db_mutation_count
            }
        }
        Start-Sleep -Seconds $IntervalSeconds
    }
}
finally {
    # Query each observed fixture explicitly. The optional quest-id form reads its reward postcondition
    # even after the active objective has advanced; it never manufactures progress for an unseen quest.
    foreach ($k in @($fx.Keys)) {
        $state = $fx[$k]; $parts = $k -split ':'; $g = [uint32]$parts[0]; $qid = [int]$parts[1]
        try {
            $finalFixture = Send-Bridge -Request "acceptance $g $qid" | ConvertFrom-Json
            if ($finalFixture.ok -and [int]$finalFixture.tracked_quest_id -eq $qid -and
                -not $state.reward_initial -and [bool]$finalFixture.reward.reward_status) {
                $state.reward = $true
            }
        }
        catch { Emit @{ kind = 'collector_error'; bot_guid = $g; quest_id = $qid; error = "final_fixture_query_failed: $($_.Exception.Message)" } }
    }

    # Final invariant delta from any reachable leader.
    $finalRawInv = $null
    foreach ($g in $Leaders) {
        try {
            $response = Send-Bridge -Request "acceptance $g" | ConvertFrom-Json
            if ($response.ok -and $null -ne $response.invariants) { $finalRawInv = $response.invariants; break }
        }
        catch { }
    }
    $finalInv = if ($counterBaseline -and $finalRawInv) { Get-InvariantDelta -Current $finalRawInv -Baseline $counterBaseline } else { $null }

    $accepted = 0
    $missing = [System.Collections.Generic.List[int]]::new()
    foreach ($qid in $requestedFixtures) {
        $matches = @($fx.Keys | Where-Object { [int](($_ -split ':')[1]) -eq [int]$qid } | Sort-Object)
        if ($matches.Count -eq 0) {
            [void]$missing.Add([int]$qid)
            Emit @{ kind = 'fixture_result'; quest_id = [int]$qid; bot_guid = 0; objective_completed = $false
                finisher_reached = $false; reward_verified = $false; teleport_used = $false; unrelated_pull_used = $false
                outcome = 'blocked:not_observed' }
            continue
        }

        $stateResults = foreach ($k in $matches) {
            $s = $fx[$k]
            [pscustomobject]@{
                key = $k; state = $s
                accepted = ($s.progressed -and $s.completed -and $s.finisher_resolved -and $s.reward -and
                    -not $s.teleport -and -not $s.unrelated -and -not $s.context_missing)
            }
        }
        $chosen = @($stateResults | Sort-Object @{ Expression = 'accepted'; Descending = $true }, key | Select-Object -First 1)[0]
        $s = $chosen.state; $parts = $chosen.key -split ':'
        $ok = [bool]$chosen.accepted
        if ($ok) { $accepted++ }
        $outcome = 'blocked:incomplete_or_violation'; if ($ok) { $outcome = 'accepted' }
        Emit @{ kind = 'fixture_result'; quest_id = [int]$parts[1]; bot_guid = [uint32]$parts[0]
            objective_completed = [bool]$s.completed; finisher_reached = [bool]($s.finisher_resolved -and $s.reward); reward_verified = [bool]$s.reward
            teleport_used = [bool]$s.teleport; unrelated_pull_used = [bool]$s.unrelated
            outcome = $outcome }
    }
    $invClean = Test-CleanInvariants $finalInv
    $gate = if ($invClean -and $missing.Count -eq 0 -and $accepted -eq $requestedFixtures.Count) { 'PASS' } else { 'FAIL' }
    Emit @{ kind = 'run_completed'; invariants = $finalInv; fixtures_accepted = $accepted; fixtures_total = $requestedFixtures.Count
        fixtures_missing = @($missing); release_gate = $gate }
}
