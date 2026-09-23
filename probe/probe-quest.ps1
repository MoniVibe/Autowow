<#
.SYNOPSIS
    Quest completability probe: one fixture bot, one quest, bounded time, verdict from the ledger.

.DESCRIPTION
    Modes
      Observe  read-only. Watches the bot for the quest (bridge questlog + live ledger lines appended
               after the probe starts). Sends only fixture-status, snapshot, questlog reads.
      Probe    Observe plus recorded SETUP, only for a fixture bot (bridge verbs gated server-side by
               AutoWow.Probe.Enable + AutoWow.FixtureGuids, never cohort / Oracle):
                 probe-login           when the fixture bot is offline (normal Playerbots login)
                 probe-setlevel <L>    when the bot level is outside the accept window; sets EXACTLY the
                                       quest level, up or down (talents/spells rebuilt, XP 0, fixture gear)
                 probe-place <quest>   clears the quest (status, reward, quest items) and teleports to the
                                       quest starter spawn; ledger contaminated reason probe_setup.
                                       -DropOtherQuests also abandons every other quest in the log.
               Setup completes before the ledger offset is taken, so setup lines never enter the
               verdict window. Accept / progress / turn-in stay with the bot's normal NewRpg play.

    Safety gates (refuse before any mutation): bot must be listed in the live AutoWow.FixtureGuids key,
    must not be in the Oracle allowlist, the cohort (62955-63004) or the scouts; must be online; faction
    must fit the quest; bot level must be within the NewRpg accept window of the quest
    (questLevel - LowLevelHideDiff .. questLevel + 3). Config is read with a single-key grep only.

    Every SampleSeconds (default 10) the probe records a timeline sample (position, NewRpg directive
    quest, oracle phase + failure reason, objective counter, selected source spawn distance/spawned,
    selected target alive/distance, stuck attempts) and phase entered/exited times. The result names
    the failure STAGE (select, accept, travel_to_source, find_target, interact_credit, loot,
    travel_to_finisher, turn_in) = stage with the longest dwell, plus the pinning evidence
    (fixation on another directive, travel distance not shrinking, source not spawned, ...).

    Verdicts: REWARDED, REWARDED_CONTAMINATED, COMPLETE_NOT_TURNED_IN, PROGRESSING, STALLED (with the
    last ledger reason), NOT_ACCEPTED, BLOCKED_PREFLIGHT (with the gate). One JSON receipt per run in
    probe\runs\. PowerShell 5.1 compatible, ASCII only.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][uint32]$QuestId,
    [Parameter(Mandatory = $true)][uint32]$BotGuid,
    [ValidateSet('Observe', 'Probe')][string]$Mode = 'Observe',
    [switch]$DropOtherQuests,
    [ValidateRange(10, 300)][int]$SetupWaitSeconds = 60,
    [ValidateRange(1, 60)][int]$BudgetMinutes = 15,
    [ValidateRange(5, 300)][int]$PollSeconds = 30,
    [ValidateRange(5, 60)][int]$SampleSeconds = 10,
    [ValidateRange(0, 19)][uint32]$SpecIndex = 0,
    [ValidateRange(0, 5)][uint32]$Quality = 2,
    [int]$LowLevelHideDiff = 4,
    [string]$Distro = 'Ubuntu-24.04',
    [string]$ModuleConf = '/usr/local/etc/modules/playerbots.conf',
    [string]$LedgerPath = '\\wsl.localhost\Ubuntu-24.04\root\autowow-soak\logs\ledger.log',
    [string]$OutDir = '',
    [string]$RunLabel = ''
)

Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $OutDir) { $OutDir = Join-Path $here 'runs' }
$root = Split-Path -Parent $here
$control = Join-Path $root 'scripts\autowow-control.ps1'
$catalogPath = Join-Path $root 'census\quest-catalog.json'
$cohort = 62955..63004
$scouts = @(101, 112, 121, 123, 236, 244)

$receipt = [ordered]@{
    schema = 'autowow.quest-probe.v1'; run_label = $RunLabel; quest = $QuestId; bot = $BotGuid; mode = $Mode
    started = (Get-Date).ToUniversalTime().ToString('o'); budget_minutes = $BudgetMinutes
    setup = @(); polls = 0; ledger = @(); verdict = ''; reason = ''; final_questlog = $null
}

function Invoke-Bridge([string]$Action, [hashtable]$Extra = @{}) {
    $args2 = @{ Action = $Action; BotGuid = $BotGuid; TimeoutMs = 10000 } + $Extra
    $raw = & $control @args2
    return ($raw | ConvertFrom-Json)
}

function Get-ConfGuids([string]$Key) {
    # Single-key grep: no other config line is read or printed.
    $line = & wsl.exe -d $Distro -u root -e grep -E ('^' + [regex]::Escape($Key) + '\s*=') $ModuleConf
    if (-not $line) { return @() }
    $value = ([string]$line).Split('=', 2)[1].Trim().Trim('"')
    return @($value.Split(',') | Where-Object { $_.Trim() -match '^\d+$' } | ForEach-Object { [uint32]$_.Trim() })
}

function Finish([string]$Verdict, [string]$Reason) {
    $receipt.verdict = $Verdict
    $receipt.reason = $Reason
    $receipt.finished = (Get-Date).ToUniversalTime().ToString('o')
    if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir | Out-Null }
    $name = 'q{0}-b{1}-{2}.json' -f $QuestId, $BotGuid, (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssZ')
    $path = Join-Path $OutDir $name
    $json = $receipt | ConvertTo-Json -Depth 8
    [IO.File]::WriteAllText($path, $json, (New-Object Text.ASCIIEncoding))
    Write-Output ('{0} q{1} bot {2}: {3} {4} -> {5}' -f $Mode, $QuestId, $BotGuid, $Verdict, $Reason, $path)
    exit 0
}

function Get-LedgerLength {
    $fs = [IO.File]::Open($LedgerPath, 'Open', 'Read', 'ReadWrite')
    try { return $fs.Length } finally { $fs.Dispose() }
}

function Read-LedgerSince([long]$Offset) {
    # Only lines appended after the probe started; filtered to this bot (bot-level events carry quest 0).
    $fs = [IO.File]::Open($LedgerPath, 'Open', 'Read', 'ReadWrite')
    try {
        if ($fs.Length -le $Offset) { return @() }
        [void]$fs.Seek($Offset, 'Begin')
        $text = (New-Object IO.StreamReader($fs)).ReadToEnd()
    } finally { $fs.Dispose() }
    $out = @()
    foreach ($line in $text -split "`n") {
        $i = $line.IndexOf('{"v":')
        if ($i -lt 0) { continue }
        try { $ev = $line.Substring($i) | ConvertFrom-Json } catch { continue }
        if ($ev.bot -eq $BotGuid -and ($ev.quest -eq $QuestId -or $ev.quest -eq 0)) { $out += $ev }
    }
    return $out
}

function Get-Dist($a, $b) {
    if ($null -eq $a -or $null -eq $b) { return $null }
    return [Math]::Round([Math]::Sqrt([Math]::Pow($a.x - $b.x, 2) + [Math]::Pow($a.y - $b.y, 2)), 1)
}

function Get-QuestEntry {
    $log = Invoke-Bridge 'questlog'
    return @($log.quests | Where-Object { $_.id -eq $QuestId }) | Select-Object -First 1
}

# ---- quest facts (census) ----
$catalog = (Get-Content -Raw $catalogPath | ConvertFrom-Json).quests
$quest = $catalog | Where-Object { $_.id -eq $QuestId } | Select-Object -First 1
if (-not $quest) { Finish 'BLOCKED_PREFLIGHT' 'quest_not_in_census' }
$qlevel = [int]$quest.questLevel
if ($qlevel -le 0) { $qlevel = [int]$quest.minLevel }
$receipt.quest_facts = [ordered]@{ title = $quest.title; family = $quest.family; level = $qlevel; faction = $quest.faction; zone = $quest.zoneName; available = $quest.available }
if (-not $quest.available) { Finish 'BLOCKED_PREFLIGHT' ('quest_unavailable:' + $quest.unavailableReason) }

# ---- safety gates ----
$fixture = Get-ConfGuids 'AutoWow.FixtureGuids'
$oracle = Get-ConfGuids 'AutoWow.OracleRuntime.BotGuids'
if ($Mode -eq 'Probe') {
    if ($fixture -notcontains $BotGuid) { Finish 'BLOCKED_PREFLIGHT' 'bot_not_in_AutoWow.FixtureGuids' }
    if ($oracle -contains $BotGuid) { Finish 'BLOCKED_PREFLIGHT' 'bot_in_oracle_allowlist' }
}
if ($cohort -contains $BotGuid -or $scouts -contains $BotGuid) { Finish 'BLOCKED_PREFLIGHT' 'bot_is_cohort_or_scout' }

$status = Invoke-Bridge 'fixture-status'
if (-not $status.ok -and $status.error -eq 'fixture_target_not_online' -and $Mode -eq 'Probe') {
    # SETUP: log the offline fixture in (server gate: AutoWow.Probe.Enable + FixtureGuids).
    $r = Invoke-Bridge 'probe-login'
    $receipt.setup += [ordered]@{ verb = 'probe-login'; ok = $r.ok; state = $(if ($r.ok) { $r.state } else { '' }); error = $(if ($r.ok) { '' } else { $r.error }) }
    if (-not $r.ok) { Finish 'BLOCKED_PREFLIGHT' ('probe_login:' + $r.error) }
    $until = (Get-Date).AddSeconds($SetupWaitSeconds)
    while (-not $status.ok -and (Get-Date) -lt $until) {
        Start-Sleep -Seconds 5
        $status = Invoke-Bridge 'fixture-status'
    }
}
if (-not $status.ok) { Finish 'BLOCKED_PREFLIGHT' ('fixture_status:' + $status.error) }
$level = [int]$status.level
$faction = [string]$status.identity.faction
$receipt.bot_facts = [ordered]@{ name = $status.identity.name; class = $status.class.name; faction = $faction; level = $level; free_slots = $status.inventory.free_slots }
if ($quest.faction -ne 'Both' -and $quest.faction -ne $faction) { Finish 'BLOCKED_PREFLIGHT' ('faction_mismatch:' + $faction) }
$entry = Get-QuestEntry
$receipt.baseline_in_log = [bool]$entry
if ($entry) { $receipt.baseline_status = $entry.status }

# ---- recorded setup (Probe only) ----
if ($Mode -eq 'Probe') {
    # Exact quest level, up or down, when outside the NewRpg accept window.
    $target = [Math]::Min(80, [Math]::Max(1, $qlevel))
    if ($level -gt $qlevel + $LowLevelHideDiff -or $level + 3 -lt $qlevel) {
        $r = Invoke-Bridge 'probe-setlevel' @{ Level = [uint32]$target; SpecIndex = $SpecIndex; Quality = $Quality }
        $receipt.setup += [ordered]@{ verb = 'probe-setlevel'; from = $level; level = $target; ok = $r.ok; error = $(if ($r.ok) { '' } else { $r.error }) }
        if (-not $r.ok) { Finish 'BLOCKED_PREFLIGHT' ('probe_setlevel:' + $r.error) }
        $level = [int]$r.level
        $receipt.bot_facts.level = $level
    }
    # Fresh accept at the starter: the quest is cleared server-side, then the bot is teleported there.
    $placeArgs = @{ QuestId = $QuestId }
    if ($DropOtherQuests) { $placeArgs.DropOtherQuests = $true }
    $r = Invoke-Bridge 'probe-place' $placeArgs
    $receipt.setup += [ordered]@{ verb = 'probe-place'; drop_others = [bool]$DropOtherQuests; ok = $r.ok
        starter = $(if ($r.ok) { $r.starter } else { $null }); dropped_others = $(if ($r.ok) { @($r.dropped_others) } else { @() })
        error = $(if ($r.ok) { '' } else { $r.error }) }
    if (-not $r.ok) { Finish 'BLOCKED_PREFLIGHT' ('probe_place:' + $r.error) }
    $starter = $r.starter.position
    $until = (Get-Date).AddSeconds($SetupWaitSeconds)
    $arrived = $false
    while (-not $arrived -and (Get-Date) -lt $until) {
        $ps = Invoke-Bridge 'probe-status' @{ QuestId = $QuestId }
        $arrived = $ps.ok -and [int]$ps.position.map -eq [int]$starter.map -and (Get-Dist $ps.position $starter) -lt 30
        if (-not $arrived) { Start-Sleep -Seconds 2 }
    }
    if (-not $arrived) { Finish 'BLOCKED_PREFLIGHT' 'probe_place:not_at_starter_after_teleport' }
    $entry = $null
}
# NewRpg IsQuestWorthDoing: a giver's quest is skipped when bot level > questLevel + LowLevelHideDiff.
# Only gates acceptance; a quest already in the log is still worked and turned in.
if (-not $entry -and $level -gt $qlevel + $LowLevelHideDiff) { Finish 'BLOCKED_PREFLIGHT' ('bot_level_{0}_hides_quest_level_{1}' -f $level, $qlevel) }

# ---- bounded observation with phase timeline ----
# Stage vocabulary shared with build_registry.py (PHASE_STAGE): select, accept, travel_to_source,
# find_target, interact_credit, loot, travel_to_finisher, turn_in.
$phaseStage = @{
    resolve_objective = 'select'; travel_to_source = 'travel_to_source'; acquire_target = 'find_target'
    wait_for_respawn = 'find_target'; self_defense = 'find_target'; engage_target = 'interact_credit'
    interact_source = 'interact_credit'; use_quest_item = 'interact_credit'; escort_event = 'interact_credit'
    verify_progress = 'interact_credit'; loot_source = 'loot'; resolve_finisher = 'travel_to_finisher'
    travel_to_finisher = 'travel_to_finisher'; interact_finisher = 'turn_in'; verify_reward = 'turn_in'
}
function Get-Prop($o, [string]$path) {
    foreach ($p in $path.Split('.')) {
        if ($null -eq $o) { return $null }
        $pp = $o.PSObject.Properties[$p]
        if ($null -eq $pp) { return $null }
        $o = $pp.Value
    }
    return $o
}
$offset = Get-LedgerLength
$t0 = Get-Date
$deadline = $t0.AddMinutes($BudgetMinutes)
$contaminated = ''
$lastReason = ''
$sawAccept = [bool]$entry
$startCounters = $null
if ($entry) { $startCounters = (@($entry.objectives | ForEach-Object { $_.current }) -join ',') }
$progressed = $false
$samples = New-Object System.Collections.ArrayList
$phases = New-Object System.Collections.ArrayList
$tick = 0
function Close-Phase([double]$t) {
    if ($phases.Count -and $null -eq $phases[$phases.Count - 1].exited_s) { $phases[$phases.Count - 1].exited_s = $t }
}
function Add-Phase([string]$name, [double]$t) {
    if ($phases.Count -and $phases[$phases.Count - 1].phase -eq $name -and $null -eq $phases[$phases.Count - 1].exited_s) { return }
    Close-Phase $t
    $st = $name
    if ($phaseStage.ContainsKey($name)) { $st = $phaseStage[$name] }
    [void]$phases.Add([ordered]@{ phase = $name; stage = $st; entered_s = $t; exited_s = $null })
}
function Save-Timeline { $receipt.timeline = [ordered]@{ phases = @($phases); samples = @($samples) } }

while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds $SampleSeconds
    $tick++
    $t = [Math]::Round(((Get-Date) - $t0).TotalSeconds, 1)
    $snap = Invoke-Bridge 'snapshot'
    $qo = Invoke-Bridge 'questobjective'
    $pos = Get-Prop $snap 'bot.position'
    $directive = [int](Get-Prop $qo 'directive_quest_id')
    $obj = Get-Prop $qo 'objective'
    $phaseName = 'none'
    $s = [ordered]@{ t = $t; x = (Get-Prop $pos 'x'); y = (Get-Prop $pos 'y'); zone = (Get-Prop $pos 'zone'); directive = $directive }
    if ($obj -and [int]$obj.quest_id -eq $QuestId) {
        $phaseName = [string]$obj.phase
        $s.phase = $phaseName
        $s.failure = $obj.failure_reason
        $s.count = '{0}/{1}' -f $obj.current_count, $obj.required_count
        $src = Get-Prop $obj 'selected_source_spawn'
        if ($src) { $s.src_dist = Get-Dist $pos $src; $s.src_spawned = $src.spawned; $s.src_in_world = $src.in_world }
        $tgt = Get-Prop $obj 'selected_target'
        if ($tgt) { $s.tgt_loaded = $tgt.loaded; $s.tgt_alive = $tgt.alive; $s.tgt_dist = $tgt.distance }
        $tp = Get-Prop $obj 'travel_position'
        if ($tp) { $s.travel_dist = Get-Dist $pos $tp }
        $s.stuck_attempts = Get-Prop $qo 'movement.stuck_attempts'
    } elseif ($directive -and $directive -ne $QuestId) {
        $phaseName = 'starved_by_directive'
    }
    Add-Phase $phaseName $t
    [void]$samples.Add($s)
    if ($tick % [Math]::Max(1, [int]($PollSeconds / $SampleSeconds)) -ne 0) { continue }

    $receipt.polls++
    $events = @(Read-LedgerSince $offset)
    foreach ($ev in $events) {
        if ($ev.ev -eq 'contaminated' -and -not $contaminated) { $contaminated = [string]$ev.reason }
        if ($ev.quest -ne $QuestId) { continue }
        if ($ev.ev -eq 'accepted') { $sawAccept = $true }
        if ($ev.ev -eq 'progress') { $progressed = $true }
        if ($ev.ev -in @('blocked', 'deferred', 'abandoned')) { $lastReason = '{0}:{1}' -f $ev.ev, $ev.reason }
    }
    $receipt.ledger = @($events | Where-Object { $_.quest -eq $QuestId -or $_.ev -eq 'contaminated' } |
        ForEach-Object { [ordered]@{ ms = $_.ms; ev = $_.ev; reason = $_.reason; phase = $_.phase; c = ($_.c -join ',') } } |
        Select-Object -Last 40)
    if (@($events | Where-Object { $_.quest -eq $QuestId -and $_.ev -eq 'rewarded' }).Count) {
        Close-Phase $t
        Save-Timeline
        $receipt.stage = 'done'
        if ($contaminated) { Finish 'REWARDED_CONTAMINATED' $contaminated }
        Finish 'REWARDED' 'ledger rewarded event'
    }
    $entry = Get-QuestEntry
    if ($entry) {
        $sawAccept = $true
        $now = (@($entry.objectives | ForEach-Object { $_.current }) -join ',')
        if ($null -ne $startCounters -and $now -ne $startCounters) { $progressed = $true }
        if ($null -eq $startCounters) { $startCounters = $now }
        $receipt.final_questlog = $entry
    }
}
Close-Phase ([Math]::Round(((Get-Date) - $t0).TotalSeconds, 1))
Save-Timeline
if ($Mode -eq 'Probe') {
    # Server-side counters for the probed quest (level, position, status, objective current/required).
    $ps = Invoke-Bridge 'probe-status' @{ QuestId = $QuestId }
    if ($ps.ok) { $receipt.final_probe_status = $ps }
}

# ---- failure point: stage with the longest dwell, plus the evidence that pins it ----
$dwell = @{}
foreach ($p in $phases) {
    $k = $p.stage
    if (-not $dwell.ContainsKey($k)) { $dwell[$k] = 0.0 }
    $dwell[$k] += ($p.exited_s - $p.entered_s)
}
$stage = 'unknown'
if ($dwell.Count) { $stage = ($dwell.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 1).Key }
$receipt.stage_dwell_s = $dwell
if ($stage -eq 'starved_by_directive') {
    $others = @($samples | Where-Object { $_.directive -and $_.directive -ne $QuestId } | ForEach-Object { $_.directive } | Sort-Object -Unique)
    $stage = 'select'
    if (-not $lastReason) { $lastReason = 'fixation:directive_' + ($others -join '_') }
} elseif ($stage -eq 'travel_to_source' -or $stage -eq 'travel_to_finisher') {
    $d = @($samples | Where-Object { $_.Contains('travel_dist') -and $null -ne $_['travel_dist'] } | ForEach-Object { $_['travel_dist'] })
    if (-not $lastReason -and $d.Count -ge 3 -and ($d[0] - $d[$d.Count - 1]) -lt 10) {
        $lastReason = 'travel_no_progress(observed {0}->{1} yd)' -f $d[0], $d[$d.Count - 1]
    }
} elseif ($stage -eq 'find_target' -and $samples.Count) {
    $last = $samples[$samples.Count - 1]
    if (-not $lastReason -and $last.Contains('src_spawned')) { $lastReason = 'source_spawned={0},target_alive={1}' -f $last['src_spawned'], $last['tgt_alive'] }
}
$receipt.stage = $stage
if ($entry -and $entry.is_complete) {
    if ($stage -ne 'turn_in' -and $stage -ne 'select') { $receipt.stage = 'travel_to_finisher' }
    Finish 'COMPLETE_NOT_TURNED_IN' $lastReason
}
if (-not $sawAccept) { $receipt.stage = 'accept'; Finish 'NOT_ACCEPTED' $lastReason }
if ($progressed) { Finish 'PROGRESSING' $lastReason }
if ($lastReason) { Finish 'STALLED' $lastReason }
Finish 'STALLED' 'no_counter_change'
