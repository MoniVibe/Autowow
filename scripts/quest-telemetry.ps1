<#
.SYNOPSIS
    Non-destructive quest telemetry harness for AutoWow / Playerbots.

.DESCRIPTION
    Read-only observation half of Quest Director V1. Every cycle it:
      1. Calls the bridge 'list' order (read-only) for live xp/level/position/combat/travel.
      2. Runs SELECT-only MySQL against character_queststatus + quest_template
         (+ addon + rewarded count) to build a per-objective progress vector
         (have vs required, creature vs gameobject entry) for the tracked bots.
      3. Computes deltas and a no-progress signal, and appends JSONL receipts.

    HARD GUARANTEES (audit-first, non-destructive):
      - Issues ONLY the read-only bridge 'list' order. Never pause/resume/travel/quest/recover.
      - Every SQL statement is asserted SELECT-only; mutation keywords are refused.
      - Never writes character quest/XP/money/item/objective rows.
      - Never enables random movement.
      - Database credentials are read from worldserver.conf and are never printed or logged.

    LIMITATION: character_queststatus is flushed on save (periodic + logout), so mid-run
    objective counters LAG live memory. db_captured_utc is stamped separately from the live
    bridge sample. For live counters, add the read-only 'questlog' bridge endpoint proposed
    in QUEST_CAPABILITY_MATRIX.md.
#>
[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [uint32[]]$BotGuid = @(),
    [ValidateRange(1, 1440)][int]$DurationMinutes = 120,
    [ValidateRange(5, 120)][int]$PollSeconds = 20,
    [ValidateRange(2, 30)][int]$NoProgressWindowCycles = 6,
    [double]$MovedThresholdYards = 8.0,
    [string]$ReceiptPath = ''
)

. (Join-Path $PSScriptRoot 'Common.ps1')

$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
Initialize-AutoWoWLayout -Root $ServerRoot

$control = Join-Path $PSScriptRoot 'autowow-control.ps1'
if (-not (Test-Path -LiteralPath $control)) { throw "AutoWow bridge client is missing: $control" }
$worldConfig = Join-Path $ServerRoot 'server\configs\worldserver.conf'
if (-not (Test-Path -LiteralPath $worldConfig)) { throw "worldserver.conf not found: $worldConfig" }
$playerbotsConfig = Join-Path $ServerRoot 'server\configs\modules\playerbots.conf'
if (-not (Test-Path -LiteralPath $playerbotsConfig)) { throw "playerbots.conf not found: $playerbotsConfig" }

if ([string]::IsNullOrWhiteSpace($ReceiptPath)) {
    $ReceiptPath = Join-Path $ServerRoot ('logs\quest-telemetry-{0}.jsonl' -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
New-Item -ItemType Directory -Path (Split-Path -Parent $ReceiptPath) -Force | Out-Null
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)

function Write-QuestTelemetry {
    param([Parameter(Mandatory = $true)][string]$Event, [hashtable]$Fields = @{})
    $record = [ordered]@{ timestamp_utc = (Get-Date).ToUniversalTime().ToString('o'); event = $Event }
    foreach ($key in $Fields.Keys) { $record[$key] = $Fields[$key] }
    $line = ([pscustomobject]$record | ConvertTo-Json -Depth 10 -Compress)
    [System.IO.File]::AppendAllText($ReceiptPath, $line + [Environment]::NewLine, $utf8NoBom)
    Write-Output $line
}

# --- Read-only SQL enforcement -------------------------------------------------
$script:ForbiddenSql = '\b(INSERT|UPDATE|DELETE|REPLACE|DROP|ALTER|CREATE|TRUNCATE|GRANT|REVOKE|SET|CALL|LOAD|RENAME|LOCK|UNLOCK|MERGE)\b'
function Assert-ReadOnlySql {
    param([Parameter(Mandatory = $true)][string]$Sql)
    $trimmed = $Sql.TrimStart()
    if ($trimmed -notmatch '^(?i)SELECT\b') { throw 'Refusing non-SELECT statement in read-only telemetry harness.' }
    if ($trimmed -match "(?i)$script:ForbiddenSql") { throw 'Refusing statement containing a mutation keyword.' }
    if ($trimmed -match ';\s*\S') { throw 'Refusing multi-statement SQL in read-only telemetry harness.' }
}

# --- Connection parsing (host;port;user;pass;db). Password never emitted. ------
function Get-DatabaseParts {
    param([Parameter(Mandatory = $true)][string]$ConfigPath, [Parameter(Mandatory = $true)][string]$Key)
    $pattern = "^\s*{0}\s*=\s*" -f [regex]::Escape($Key)
    $line = (Select-String -LiteralPath $ConfigPath -Pattern $pattern | Select-Object -First 1).Line
    if (-not $line) { throw "$Key was not found in $ConfigPath" }
    $parts = (($line -replace $pattern, '').Trim().Trim('"')) -split ';', 5
    if ($parts.Count -ne 5) { throw "$Key has an unexpected connection-string format." }
    return $parts
}

$charInfo  = Get-DatabaseParts -ConfigPath $worldConfig -Key 'CharacterDatabaseInfo'
$worldInfo = Get-DatabaseParts -ConfigPath $worldConfig -Key 'WorldDatabaseInfo'
$charDb  = $charInfo[4]
$worldDb = $worldInfo[4]
# character and world DBs are served by the same instance/user in this bootstrap.
$dbHost = $charInfo[0]; $dbPort = $charInfo[1]; $dbUser = $charInfo[2]; $dbPass = $charInfo[3]

$mysql = Get-FirstExistingPath -Candidates @(
    (Join-Path $ServerRoot 'third_party\mysql\bin\mysql.exe'),
    'C:\Program Files\MySQL\MySQL Server 8.4\bin\mysql.exe',
    'C:\Program Files\MySQL\MySQL Server 8.0\bin\mysql.exe'
)
if (-not $mysql) { throw 'MySQL CLI was not found for telemetry queries.' }

function Invoke-ReadOnlyMySql {
    param([Parameter(Mandatory = $true)][string]$Sql)
    Assert-ReadOnlySql -Sql $Sql
    $oldPwd = $env:MYSQL_PWD
    $env:MYSQL_PWD = $dbPass
    try {
        # No default database: all table references below are schema-qualified.
        $raw = & $mysql --protocol=tcp "--host=$dbHost" "--port=$dbPort" "--user=$dbUser" `
            --default-character-set=utf8mb4 --batch --skip-column-names --execute=$Sql 2>&1
        $exit = $LASTEXITCODE
    }
    finally {
        if ($null -eq $oldPwd) { Remove-Item Env:MYSQL_PWD -ErrorAction SilentlyContinue } else { $env:MYSQL_PWD = $oldPwd }
    }
    $lines = @($raw | ForEach-Object { $_.ToString() } | Where-Object { $_ -notmatch '^mysql:' -and $_ -ne '' })
    if ($exit -ne 0) { throw 'Read-only telemetry query failed. Connection details were not emitted.' }
    return $lines
}

# --- Resolve tracked guids -----------------------------------------------------
$guids = @($BotGuid | Where-Object { $_ -ne 0 } | Select-Object -Unique)
if ($guids.Count -eq 0) {
    # Auto-discover the league roster from acore_playerbots if that connection is configured.
    try {
        $pbInfo = Get-DatabaseParts -ConfigPath $playerbotsConfig -Key 'PlayerbotsDatabaseInfo'
        $pbDb = $pbInfo[4]
        $rows = Invoke-ReadOnlyMySql -Sql "SELECT character_guid FROM $pbDb.autowow_league_member WHERE retired_at IS NULL"
        $guids = @($rows | ForEach-Object { [uint32]($_.Trim()) })
    }
    catch {
        throw 'No -BotGuid supplied and league roster auto-discovery failed. Pass -BotGuid explicitly.'
    }
}
if ($guids.Count -eq 0) { throw 'No bots to track.' }
$guidList = ($guids -join ',')

# --- Bridge (read-only list only) ----------------------------------------------
function Get-LiveBots {
    $raw = & $control -Action list
    $parsed = (@($raw | ForEach-Object { $_.ToString() }) | Select-Object -Last 1) | ConvertFrom-Json
    if (-not $parsed.ok) { throw "Bridge list failed: $($parsed.error)" }
    return @($parsed.bots)
}

# --- Objective vector query ----------------------------------------------------
$reqCols = @()
for ($i = 1; $i -le 4; $i++) { $reqCols += "qt.RequiredNpcOrGo$i","qt.RequiredNpcOrGoCount$i" }
for ($i = 1; $i -le 6; $i++) { $reqCols += "qt.RequiredItemId$i","qt.RequiredItemCount$i" }
$statusCols = @('qs.mobcount1','qs.mobcount2','qs.mobcount3','qs.mobcount4',
                'qs.itemcount1','qs.itemcount2','qs.itemcount3','qs.itemcount4','qs.itemcount5','qs.itemcount6',
                'qs.playercount','qs.explored','qs.status')

function Get-QuestStatusRows {
    $sql = "SELECT qs.guid, qs.quest, qt.LogTitle, qt.QuestType, qt.QuestInfoID, qt.Flags, " +
           "COALESCE(qta.SpecialFlags,0) AS SpecialFlags, " +
           ($statusCols -join ', ') + ", " + ($reqCols -join ', ') + " " +
           "FROM $charDb.character_queststatus qs " +
           "JOIN $worldDb.quest_template qt ON qt.ID = qs.quest " +
           "LEFT JOIN $worldDb.quest_template_addon qta ON qta.ID = qs.quest " +
           "WHERE qs.guid IN ($guidList)"
    $header = @('guid','quest','LogTitle','QuestType','QuestInfoID','Flags','SpecialFlags') + $statusCols + $reqCols |
        ForEach-Object { ($_ -replace '^(qs|qt)\.','') }
    $rows = @()
    foreach ($line in (Invoke-ReadOnlyMySql -Sql $sql)) {
        $cols = $line -split "`t"
        if ($cols.Count -ne $header.Count) { continue }
        $obj = [ordered]@{}
        for ($i = 0; $i -lt $header.Count; $i++) { $obj[$header[$i]] = $cols[$i] }
        $rows += [pscustomobject]$obj
    }
    return $rows
}

function Get-RewardedCounts {
    $sql = "SELECT guid, COUNT(*) FROM $charDb.character_queststatus_rewarded WHERE guid IN ($guidList) GROUP BY guid"
    $map = @{}
    foreach ($line in (Invoke-ReadOnlyMySql -Sql $sql)) {
        $c = $line -split "`t"
        if ($c.Count -eq 2) { $map[[string]$c[0]] = [int]$c[1] }
    }
    return $map
}

# Build per-objective vector + a single summed "objective progress" scalar per bot.
function Build-ObjectiveVector {
    param([pscustomobject]$Row)
    $objectives = @()
    $sumHave = 0
    for ($i = 1; $i -le 4; $i++) {
        $entry = [int]$Row."RequiredNpcOrGo$i"; $req = [int]$Row."RequiredNpcOrGoCount$i"
        if ($req -gt 0) {
            $have = [int]$Row."mobcount$i"
            $kind = if ($entry -lt 0) { 'gameobject' } else { 'npc' }
            $objectives += [ordered]@{ slot = $i; kind = $kind; entry = [math]::Abs($entry); have = $have; required = $req; done = ($have -ge $req) }
            $sumHave += [math]::Min($have, $req)
        }
    }
    for ($i = 1; $i -le 6; $i++) {
        $item = [int]$Row."RequiredItemId$i"; $req = [int]$Row."RequiredItemCount$i"
        if ($item -gt 0 -and $req -gt 0) {
            $have = [int]$Row."itemcount$i"
            $objectives += [ordered]@{ slot = $i; kind = 'item'; entry = $item; have = $have; required = $req; done = ($have -ge $req) }
            $sumHave += [math]::Min($have, $req)
        }
    }
    return @{ objectives = $objectives; sum_have = $sumHave }
}

# --- Main loop -----------------------------------------------------------------
$prev = @{}                # guid -> @{ xp; rewarded; sum_have; x; y; z; map; flat_cycles }
$deadline = (Get-Date).AddMinutes($DurationMinutes)
$consecutiveErrors = 0

Write-QuestTelemetry -Event 'quest_telemetry_started' -Fields @{
    tracked_guids = @($guids); duration_minutes = $DurationMinutes; poll_seconds = $PollSeconds
    no_progress_window_cycles = $NoProgressWindowCycles; bridge = '127.0.0.1:18787'
    character_db = $charDb; world_db = $worldDb; read_only = $true
}

try {
    while ((Get-Date) -lt $deadline) {
        try {
            $liveBots = Get-LiveBots
            $liveByGuid = @{}
            foreach ($b in $liveBots) { $liveByGuid[[string]$b.guid] = $b }

            $dbCapturedUtc = (Get-Date).ToUniversalTime().ToString('o')
            $rows = Get-QuestStatusRows
            $rewarded = Get-RewardedCounts
            $rowsByGuid = $rows | Group-Object guid

            foreach ($group in $rowsByGuid) {
                $guid = [string]$group.Name
                $live = $liveByGuid[$guid]
                $rewardedCount = if ($rewarded.ContainsKey($guid)) { $rewarded[$guid] } else { 0 }

                # Active (accepted, not rewarded) quests with an objective vector.
                $quests = @()
                $botSumHave = 0
                foreach ($r in $group.Group) {
                    $vec = Build-ObjectiveVector -Row $r
                    $botSumHave += $vec.sum_have
                    $special = [int]$r.SpecialFlags
                    # 0x0002 is AzerothCore's QUEST_SPECIAL_FLAGS_EXPLORATION_OR_EVENT.
                    # Escort/dungeon/raid category is carried by QuestInfoID, not QuestType.
                    $isExplorationOrEvent = (($special -band 2) -ne 0)
                    $quests += [ordered]@{
                        id = [int]$r.quest; title = $r.LogTitle; status = [int]$r.status
                        quest_type = [int]$r.QuestType; quest_info_id = [int]$r.QuestInfoID; special_flags = $special
                        likely_exploration_or_event = $isExplorationOrEvent
                        objectives = $vec.objectives
                    }
                }

                $xp = if ($live) { [int64]$live.progress.xp } else { -1 }
                $level = if ($live) { [int]$live.progress.level } else { 0 }
                $combat = if ($live) { [bool]$live.combat } else { $false }
                $x = if ($live) { [double]$live.position.x } else { 0 }
                $y = if ($live) { [double]$live.position.y } else { 0 }
                $z = if ($live) { [double]$live.position.z } else { 0 }
                $map = if ($live) { [int]$live.position.map } else { -1 }
                $botName = if ($live) { [string]$live.name } else { '' }

                # Deltas + no-progress accounting.
                $moved = 0.0; $flatCycles = 0
                $xpDelta = 0; $objDelta = 0; $rewDelta = 0
                if ($prev.ContainsKey($guid)) {
                    $p = $prev[$guid]
                    $xpDelta = $xp - $p.xp
                    $objDelta = $botSumHave - $p.sum_have
                    $rewDelta = $rewardedCount - $p.rewarded
                    if ($map -eq $p.map) { $moved = [math]::Sqrt([math]::Pow($x-$p.x,2)+[math]::Pow($y-$p.y,2)+[math]::Pow($z-$p.z,2)) }
                    else { $moved = [double]::PositiveInfinity }
                    $progressed = ($xpDelta -gt 0) -or ($objDelta -gt 0) -or ($rewDelta -gt 0) -or $combat -or ($moved -gt $MovedThresholdYards)
                    $flatCycles = if ($progressed) { 0 } else { [int]$p.flat_cycles + 1 }
                }
                $movedYards = if ([double]::IsInfinity($moved)) { -1 } else { [math]::Round($moved,1) }
                $travelStatus = if ($live) { [string]$live.travel.status } else { '' }

                Write-QuestTelemetry -Event 'quest_sample' -Fields @{
                    bot_guid = [uint32]$guid; name = $botName
                    online = [bool]$live; level = $level; xp = $xp; combat = $combat
                    map = $map; rewarded_count = $rewardedCount
                    active_quests = @($quests | Where-Object { $_.status -ne 0 -or $_.objectives.Count -gt 0 })
                    objective_sum_have = $botSumHave
                    xp_delta = $xpDelta; objective_delta = $objDelta; rewarded_delta = $rewDelta
                    moved_yards = $movedYards
                    travel_status = $travelStatus
                    flat_cycles = $flatCycles
                    db_captured_utc = $dbCapturedUtc
                    db_lag_note = 'objective counters reflect last character save; not live memory'
                }

                if ($flatCycles -ge $NoProgressWindowCycles) {
                    $reason = if (-not $live) { 'bot_offline' }
                              elseif (@($quests | Where-Object { $_.status -ne 0 }).Count -eq 0) { 'no_active_quest' }
                              elseif (@($quests | Where-Object { $_.likely_exploration_or_event -or $_.quest_info_id -in @(62,81,84,85,88,89) }).Count -gt 0) { 'unsupported_objective_type' }
                              else { 'stuck_or_unroutable_objective' }
                    Write-QuestTelemetry -Event 'no_progress' -Fields @{
                        bot_guid = [uint32]$guid; name = $botName
                        flat_cycles = $flatCycles; window = $NoProgressWindowCycles; reason = $reason
                        note = 'observation only; harness issues no control order'
                    }
                }

                $prev[$guid] = @{ xp = $xp; rewarded = $rewardedCount; sum_have = $botSumHave; x = $x; y = $y; z = $z; map = $map; flat_cycles = $flatCycles }
            }
            $consecutiveErrors = 0
        }
        catch {
            $consecutiveErrors++
            Write-QuestTelemetry -Event 'collector_error' -Fields @{ consecutive_errors = $consecutiveErrors; error = $_.Exception.Message }
            if ($consecutiveErrors -ge 6) { throw "Quest telemetry stopped after $consecutiveErrors consecutive errors." }
        }
        Start-Sleep -Seconds $PollSeconds
    }
    Write-QuestTelemetry -Event 'quest_telemetry_completed' -Fields @{ tracked_guids = @($guids) }
}
finally {
    Write-QuestTelemetry -Event 'quest_telemetry_stopped' -Fields @{ tracked_guids = @($guids); receipt = $ReceiptPath }
}
