[CmdletBinding()]
param(
    [ValidateSet('install','seed','launch','status','roster','record-quest-state','issue-challenge','answer-challenge','resolve-challenge')][string]$Action = 'status',
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [ValidateSet('northstar','ember')][string]$IssuerTeam = 'northstar',
    [ValidateSet('northstar','ember')][string]$TargetTeam = 'ember',
    [ValidateSet('WSG')][string]$Battleground = 'WSG',
    [ValidateRange(2,10)][int]$SquadSize = 10,
    [uint64]$ChallengeId = 0,
    [ValidateSet('accept','decline')][string]$Response = 'accept',
    [ValidateSet('northstar','ember')][string]$WinnerTeam = 'northstar',
    [uint32]$LeaderGuid = 0,
    [ValidateSet('northstar','ember')][string]$TeamId = 'northstar',
    [uint32]$QuestId = 0,
    [ValidateRange(0,255)][int]$QuestStatus = 0,
    [ValidateSet('acquire','objective','turnin','blocked')][string]$QuestPhase = 'acquire',
    [string]$QuestDestination = '',
    [ValidateRange(0,255)][int]$AcceptAttempts = 0,
    [ValidateRange(0,255)][int]$TurnInAttempts = 0,
    [string]$EvidencePath = '',
    # Keep unrelated online laboratory/raid rosters intact while starting the two persistent
    # quest parties. Without this switch, legacy league launch parks all Wayfarer characters.
    [switch]$PreserveOtherFleets,
    [switch]$AsJson
)

. (Join-Path $PSScriptRoot 'Common.ps1')

$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
$coreRoot = Join-Path $ServerRoot 'azerothcore-wotlk'
$configRoot = Join-Path $ServerRoot 'server\configs'
$worldConfig = Join-Path $configRoot 'worldserver.conf'
$playerbotsConfig = Join-Path $configRoot 'modules\playerbots.conf'
$bridge = Join-Path $PSScriptRoot 'autowow-control.ps1'
$migration = Join-Path $coreRoot 'modules\mod-playerbots\data\sql\playerbots\custom\2026_07_12_00_autowow_league_v0.sql'

foreach ($required in @($worldConfig, $playerbotsConfig, $bridge, $migration)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Required AutoWow file is missing: $required" }
}

function Get-DatabaseParts {
    param([string]$ConfigPath, [string]$Key)

    $pattern = "^\s*{0}\s*=\s*" -f [regex]::Escape($Key)
    $line = (Select-String -LiteralPath $ConfigPath -Pattern $pattern | Select-Object -First 1).Line
    if (-not $line) { throw "$Key was not found in $ConfigPath" }
    $parts = (($line -replace $pattern, '').Trim().Trim('"')) -split ';', 5
    if ($parts.Count -ne 5) { throw "$Key has an unexpected connection-string format." }
    return $parts
}

$loginDb = Get-DatabaseParts -ConfigPath $worldConfig -Key 'LoginDatabaseInfo'
$characterDb = Get-DatabaseParts -ConfigPath $worldConfig -Key 'CharacterDatabaseInfo'
$playerbotsDb = Get-DatabaseParts -ConfigPath $playerbotsConfig -Key 'PlayerbotsDatabaseInfo'
$mysql = Get-FirstExistingPath -Candidates @(
    (Join-Path $ServerRoot 'third_party\mysql\bin\mysql.exe'),
    'C:\Program Files\MySQL\MySQL Server 8.4\bin\mysql.exe',
    'C:\Program Files\MySQL\MySQL Server 8.0\bin\mysql.exe'
)
if (-not $mysql) { throw 'MySQL CLI was not found.' }

function ConvertTo-SqlLiteral {
    param([AllowNull()][object]$Value)
    if ($null -eq $Value) { return 'NULL' }
    return "'" + ([string]$Value).Replace("'", "''") + "'"
}

function Invoke-LeagueSql {
    param(
        [Parameter(Mandatory = $true)][string]$Sql,
        [switch]$Silent
    )

    $priorPassword = $env:MYSQL_PWD
    $env:MYSQL_PWD = $playerbotsDb[3]
    try {
        $raw = & $mysql --protocol=tcp "--host=$($playerbotsDb[0])" "--port=$($playerbotsDb[1])" "--user=$($playerbotsDb[2])" --default-character-set=utf8mb4 --batch --skip-column-names --execute=$Sql 2>&1
        $exitCode = $LASTEXITCODE
    }
    finally {
        if ($null -eq $priorPassword) { Remove-Item Env:MYSQL_PWD -ErrorAction SilentlyContinue }
        else { $env:MYSQL_PWD = $priorPassword }
    }

    $lines = @($raw | ForEach-Object { $_.ToString() } | Where-Object {
        $_ -and $_ -notmatch '^mysql: Unknown OS character set' -and $_ -notmatch '^mysql: Switching to the default character set'
    })
    if ($exitCode -ne 0) {
        $safeError = @($lines | Where-Object { $_ -notmatch '(?i)password|MYSQL_PWD|;[^;]+;[^;]+;[^;]+;' }) -join ' '
        if ([string]::IsNullOrWhiteSpace($safeError)) { $safeError = 'no database error text returned' }
        throw "League database command failed: $safeError"
    }
    if (-not $Silent) { return $lines }
}

function Invoke-Bridge {
    param(
        [Parameter(Mandatory = $true)][ValidateSet('list','activate','deactivate','party','rally','deploy','route','engage','scout','quest','destinations','snapshot','pause','resume','travel','recover')][string]$BridgeAction,
        [uint32]$BotGuid = 0,
        [uint32[]]$MemberGuid = @(),
        [string]$Destination = ''
    )

    $output = switch ($BridgeAction) {
        'list' { & $bridge -Action list }
        'activate' { & $bridge -Action activate -BotGuid $BotGuid }
        'deactivate' { & $bridge -Action deactivate -BotGuid $BotGuid }
        'party' { & $bridge -Action party -BotGuid $BotGuid -MemberGuid $MemberGuid }
        'rally' { & $bridge -Action rally -BotGuid $BotGuid }
        'deploy' { & $bridge -Action deploy -BotGuid $BotGuid }
        'route' { & $bridge -Action route -BotGuid $BotGuid -Destination $Destination }
        'engage' { & $bridge -Action engage -BotGuid $BotGuid }
        'scout' { & $bridge -Action scout -BotGuid $BotGuid }
        'quest' { & $bridge -Action quest -BotGuid $BotGuid }
        'destinations' { & $bridge -Action destinations -BotGuid $BotGuid }
        'snapshot' { & $bridge -Action snapshot -BotGuid $BotGuid }
        'pause' { & $bridge -Action pause -BotGuid $BotGuid }
        'resume' { & $bridge -Action resume -BotGuid $BotGuid }
        'travel' { & $bridge -Action travel -BotGuid $BotGuid -Destination $Destination }
        'recover' { & $bridge -Action recover -BotGuid $BotGuid }
    }
    $raw = (@($output | ForEach-Object { $_.ToString() }) | Select-Object -Last 1)
    if ([string]::IsNullOrWhiteSpace($raw)) { throw "Bridge '$BridgeAction' returned no JSON." }
    $parsed = $raw | ConvertFrom-Json
    if (-not $parsed.ok) {
        $message = if ($parsed.error) { $parsed.error } else { 'unknown bridge error' }
        throw "Bridge '$BridgeAction' failed: $message"
    }
    return $parsed
}

function Write-LeagueEvent {
    param(
        [Parameter(Mandatory = $true)][string]$EventType,
        [string]$TeamId = '',
        [uint32]$CharacterGuid = 0,
        [hashtable]$Payload = @{}
    )

    $json = $Payload | ConvertTo-Json -Compress -Depth 8
    $teamSql = if ([string]::IsNullOrWhiteSpace($TeamId)) { 'NULL' } else { ConvertTo-SqlLiteral $TeamId }
    $guidSql = if ($CharacterGuid -eq 0) { 'NULL' } else { [string]$CharacterGuid }
    $sql = "INSERT INTO $($playerbotsDb[4]).autowow_league_event (event_type, team_id, character_guid, payload_json) VALUES ($(ConvertTo-SqlLiteral $EventType), $teamSql, $guidSql, $(ConvertTo-SqlLiteral $json));"
    Invoke-LeagueSql -Sql $sql -Silent
}

function Install-LeagueSchema {
    $sql = "USE ``$($playerbotsDb[4])``;`n" + (Get-Content -LiteralPath $migration -Raw)
    Invoke-LeagueSql -Sql $sql -Silent
    $tables = @(Invoke-LeagueSql -Sql "SELECT table_name FROM information_schema.tables WHERE table_schema = $(ConvertTo-SqlLiteral $playerbotsDb[4]) AND table_name LIKE 'autowow_league_%' ORDER BY table_name;")
    if ($tables.Count -lt 8) { throw "League schema verification failed; expected eight tables, found $($tables.Count)." }
    return $tables
}

function Get-LeagueMembers {
    $sql = @"
SELECT m.character_guid, COALESCE(m.team_id,''), m.affiliation, m.role, m.class_plan,
       m.profession_one, m.profession_two, m.active, c.name, c.race, c.class, c.level, c.money, c.online
FROM $($playerbotsDb[4]).autowow_league_member m
LEFT JOIN $($characterDb[4]).characters c ON c.guid = m.character_guid
ORDER BY COALESCE(m.team_id,'wayfarers'), m.affiliation, m.role, m.character_guid;
"@
    $rows = @()
    foreach ($line in @(Invoke-LeagueSql -Sql $sql)) {
        $parts = $line -split "`t", 14
        if ($parts.Count -ne 14) { continue }
        $rows += [pscustomobject]@{
            guid = [uint32]$parts[0]; team = $parts[1]; affiliation = $parts[2]; role = $parts[3]
            class_plan = $parts[4]; profession_one = $parts[5]; profession_two = $parts[6]
            active = ([int]$parts[7] -ne 0); name = $parts[8]; race = [int]$parts[9]; class = [int]$parts[10]
            level = [int]$parts[11]; money = [uint64]$parts[12]; online = ([int]$parts[13] -ne 0)
        }
    }
    return $rows
}

function Get-LeagueTeams {
    $sql = "SELECT team_id, display_name, faction, roster_cap, worker_cap, treasury_copper FROM $($playerbotsDb[4]).autowow_league_team ORDER BY team_id;"
    $rows = @()
    foreach ($line in @(Invoke-LeagueSql -Sql $sql)) {
        $parts = $line -split "`t", 6
        if ($parts.Count -ne 6) { continue }
        $rows += [pscustomobject]@{ id = $parts[0]; name = $parts[1]; faction = $parts[2]; roster_cap = [int]$parts[3]; worker_cap = [int]$parts[4]; treasury_copper = [int64]$parts[5] }
    }
    return $rows
}

function Get-LeagueChallenges {
    $sql = "SELECT challenge_id, issuer_team_id, target_team_id, battleground, squad_size, status, issued_at, respond_by, COALESCE(winner_team_id,''), boon_key, boon_expires_at FROM $($playerbotsDb[4]).autowow_league_challenge ORDER BY challenge_id DESC LIMIT 20;"
    $rows = @()
    foreach ($line in @(Invoke-LeagueSql -Sql $sql)) {
        $parts = $line -split "`t", 11
        if ($parts.Count -ne 11) { continue }
        $rows += [pscustomobject]@{ id = [uint64]$parts[0]; issuer = $parts[1]; target = $parts[2]; battleground = $parts[3]; squad_size = [int]$parts[4]; status = $parts[5]; issued_at = $parts[6]; respond_by = $parts[7]; winner = $parts[8]; boon = $parts[9]; boon_expires_at = $parts[10] }
    }
    return $rows
}

function Get-LeagueStatus {
    $members = @(Get-LeagueMembers)
    $teams = @(Get-LeagueTeams)
    $bridgeByGuid = @{}
    $bridgeError = $null
    try {
        foreach ($bot in @((Invoke-Bridge -BridgeAction list).bots)) { $bridgeByGuid[[uint32]$bot.guid] = $bot }
    }
    catch { $bridgeError = $_.Exception.Message }

    $teamStatus = @()
    foreach ($team in $teams) {
        $teamMembers = @($members | Where-Object { $_.team -eq $team.id })
        $online = @($teamMembers | Where-Object { $bridgeByGuid.ContainsKey([uint32]$_.guid) })
        $teamStatus += [ordered]@{
            id = $team.id
            name = $team.name
            faction = $team.faction
            roster_cap = $team.roster_cap
            worker_cap = $team.worker_cap
            treasury_copper = $team.treasury_copper
            enrolled = $teamMembers.Count
            active = @($teamMembers | Where-Object active).Count
            online = $online.Count
            level_one = @($teamMembers | Where-Object { $_.level -eq 1 }).Count
        }
    }

    $wayfarers = @($members | Where-Object { $_.affiliation -eq 'wayfarer' })
    return [ordered]@{
        generated_utc = (Get-Date).ToUniversalTime().ToString('o')
        teams = $teamStatus
        wayfarers = @($wayfarers | ForEach-Object { [ordered]@{ guid = $_.guid; name = $_.name; role = $_.role; level = $_.level; online = $bridgeByGuid.ContainsKey([uint32]$_.guid) } })
        challenges = @(Get-LeagueChallenges)
        bridge_online_bots = $bridgeByGuid.Count
        bridge_error = $bridgeError
    }
}

function Record-QuestState {
    if ($LeaderGuid -eq 0) { throw 'LeaderGuid is required for record-quest-state.' }
    Install-LeagueSchema | Out-Null
    $sql = @"
INSERT INTO $($playerbotsDb[4]).autowow_league_quest_state
    (leader_guid,team_id,quest_id,quest_status,phase,destination,accept_attempts,turnin_attempts)
VALUES
    ($LeaderGuid,$(ConvertTo-SqlLiteral $TeamId),$QuestId,$QuestStatus,$(ConvertTo-SqlLiteral $QuestPhase),$(ConvertTo-SqlLiteral $QuestDestination),$AcceptAttempts,$TurnInAttempts)
ON DUPLICATE KEY UPDATE
    team_id=VALUES(team_id), quest_id=VALUES(quest_id), quest_status=VALUES(quest_status), phase=VALUES(phase),
    destination=VALUES(destination), accept_attempts=VALUES(accept_attempts), turnin_attempts=VALUES(turnin_attempts);
"@
    Invoke-LeagueSql -Sql $sql -Silent
}

function Seed-League {
    Install-LeagueSchema | Out-Null
    $seed = @(
        @{ guid = 11; team = 'northstar'; affiliation = 'guild'; role = 'adventurer'; class_plan = 'warrior' },
        @{ guid = 14; team = 'northstar'; affiliation = 'guild'; role = 'adventurer'; class_plan = 'rogue' },
        @{ guid = 15; team = 'northstar'; affiliation = 'guild'; role = 'adventurer'; class_plan = 'priest' },
        @{ guid = 19; team = 'northstar'; affiliation = 'guild'; role = 'adventurer'; class_plan = 'warlock' },
        @{ guid = 10; team = 'northstar'; affiliation = 'guild'; role = 'adventurer'; class_plan = 'druid' },
        @{ guid = 12; team = 'ember'; affiliation = 'guild'; role = 'adventurer'; class_plan = 'paladin' },
        @{ guid = 13; team = 'ember'; affiliation = 'guild'; role = 'adventurer'; class_plan = 'hunter' },
        @{ guid = 7; team = 'ember'; affiliation = 'guild'; role = 'adventurer'; class_plan = 'shaman' },
        @{ guid = 18; team = 'ember'; affiliation = 'guild'; role = 'adventurer'; class_plan = 'mage' },
        @{ guid = 20; team = 'ember'; affiliation = 'guild'; role = 'adventurer'; class_plan = 'druid' },
        @{ guid = 17; team = $null; affiliation = 'wayfarer'; role = 'worker'; class_plan = 'shaman' }
    )
    $guids = ($seed.guid -join ',')
    $checkSql = "SELECT guid, race, level FROM $($characterDb[4]).characters WHERE guid IN ($guids) ORDER BY guid;"
    $found = @{}
    foreach ($line in @(Invoke-LeagueSql -Sql $checkSql)) {
        $parts = $line -split "`t", 3
        if ($parts.Count -eq 3) { $found[[uint32]$parts[0]] = [pscustomobject]@{ race = [int]$parts[1]; level = [int]$parts[2] } }
    }
    $enrolledSql = "SELECT character_guid FROM $($playerbotsDb[4]).autowow_league_member WHERE character_guid IN ($guids);"
    $enrolled = @{}
    foreach ($line in @(Invoke-LeagueSql -Sql $enrolledSql)) {
        if ($line -match '^\d+$') { $enrolled[[uint32]$line] = $true }
    }
    foreach ($entry in $seed) {
        if (-not $found.ContainsKey([uint32]$entry.guid)) { throw "Seed character GUID $($entry.guid) is missing." }
        if ($found[[uint32]$entry.guid].level -ne 1 -and -not $enrolled.ContainsKey([uint32]$entry.guid)) {
            throw "Seed character GUID $($entry.guid) is above level one but is not enrolled; refusing to claim pre-existing character history."
        }
    }

    $sql = [System.Text.StringBuilder]::new()
    [void]$sql.AppendLine('START TRANSACTION;')
    [void]$sql.AppendLine("INSERT INTO $($playerbotsDb[4]).autowow_league_team (team_id,display_name,faction,roster_cap,worker_cap,treasury_copper) VALUES ('northstar','Northstar','Alliance',20,10,0),('ember','Ember','Horde',20,10,0),('wayfarers','The Wayfarers'' Exchange','Neutral',0,0,0) ON DUPLICATE KEY UPDATE display_name=VALUES(display_name), faction=VALUES(faction);")
    foreach ($entry in $seed) {
        $team = if ($null -eq $entry.team) { 'NULL' } else { ConvertTo-SqlLiteral $entry.team }
        [void]$sql.AppendLine("INSERT IGNORE INTO $($playerbotsDb[4]).autowow_league_member (character_guid,team_id,affiliation,role,class_plan,profession_one,profession_two,active) VALUES ($($entry.guid),$team,$(ConvertTo-SqlLiteral $entry.affiliation),$(ConvertTo-SqlLiteral $entry.role),$(ConvertTo-SqlLiteral $entry.class_plan),'','',0);")
    }
    [void]$sql.AppendLine("INSERT INTO $($playerbotsDb[4]).autowow_league_unlock (team_id,unlock_key,quantity) VALUES ('northstar','roster_slots',5),('northstar','worker_contracts',0),('ember','roster_slots',5),('ember','worker_contracts',0) ON DUPLICATE KEY UPDATE quantity=GREATEST(quantity,VALUES(quantity));")
    [void]$sql.AppendLine('COMMIT;')
    Invoke-LeagueSql -Sql $sql.ToString() -Silent
    Write-LeagueEvent -EventType 'league_seeded' -Payload @{ teams = 2; adventurers = 10; wayfarers = 1; roster_slots = 5 }
}

function Wait-ForLeagueBots {
    param([uint32[]]$Guids, [int]$TimeoutSeconds = 90)
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        $online = @((Invoke-Bridge -BridgeAction list).bots | ForEach-Object { [uint32]$_.guid })
        if (@($Guids | Where-Object { $_ -notin $online }).Count -eq 0) { return }
        Start-Sleep -Seconds 2
    }
    throw 'Timed out waiting for the seeded league bots to log in.'
}

function Invoke-IndependentQuestDispatch {
    param([Parameter(Mandatory = $true)][uint32]$LeaderGuid)

    # A restart restores the persisted group before every PlayerbotAI instance has completed its
    # first group-maintenance tick.  The bridge re-arms independent mode at the quest boundary,
    # but the first request can race that login transition.  Keep this repair bounded and
    # idempotent: issue the real quest order, inspect the live strategy set, and retry only when
    # the leader still exposes the permanent follow strategy.  Never hide a failed re-arm.
    $lastResponse = $null
    for ($attempt = 1; $attempt -le 3; $attempt++) {
        $lastResponse = Invoke-Bridge -BridgeAction quest -BotGuid $LeaderGuid
        Start-Sleep -Milliseconds 250
        $leaderState = @((Invoke-Bridge -BridgeAction list).bots | Where-Object {
            [uint32]$_.guid -eq $LeaderGuid
        } | Select-Object -First 1)
        if ($leaderState.Count -eq 0) {
            throw "Leader $LeaderGuid disappeared while re-arming independent quest execution."
        }

        $hasPermanentFollow = @($leaderState[0].strategies.non_combat) -contains 'follow'
        if (-not $hasPermanentFollow) {
            return [pscustomobject]@{ response = $lastResponse; attempts = $attempt }
        }
    }

    throw "Leader $LeaderGuid retained the permanent follow strategy after three bounded quest dispatches."
}

function Launch-League {
    Seed-League
    $members = @(Get-LeagueMembers)
    # Campaign launch owns exactly the two seeded racing teams. Historical PvP/role fixtures also
    # use affiliation=guild and must never be swept into a persistent quest launch.
    $targetGuids = @($members | Where-Object {
        $_.affiliation -eq 'guild' -and $_.team -in @('northstar','ember')
    } | ForEach-Object { [uint32]$_.guid })
    $allMemberGuids = @($members | ForEach-Object { [uint32]$_.guid })
    $currentlyOnline = @((Invoke-Bridge -BridgeAction list).bots | ForEach-Object { [uint32]$_.guid })
    foreach ($guid in $targetGuids) {
        if ($guid -notin $currentlyOnline) { Invoke-Bridge -BridgeAction activate -BotGuid $guid | Out-Null }
    }
    Wait-ForLeagueBots -Guids $targetGuids

    foreach ($team in @('northstar','ember')) {
        $party = @($members | Where-Object { $_.team -eq $team -and $_.affiliation -eq 'guild' } | Sort-Object guid)
        if ($party.Count -ne 5) { throw "Team $team does not have exactly five seeded adventurers." }
        $leader = [uint32]$party[0].guid
        $memberGuids = @($party | Select-Object -Skip 1 | ForEach-Object { [uint32]$_.guid })
        $botStates = @((Invoke-Bridge -BridgeAction list).bots | Where-Object { [uint32]$_.guid -in @($leader) + $memberGuids })
        $alreadyGrouped = $botStates.Count -eq 5 -and @($botStates | Where-Object {
            $_.group.members -eq 5 -and [uint32]$_.group.leader_guid -eq $leader
        }).Count -eq 5
        if (-not $alreadyGrouped) {
            if (@($botStates | Where-Object { $_.group.members -ne 0 }).Count -ne 0) { throw "Team $team has a partial or foreign group; refusing to regroup it." }
            Invoke-Bridge -BridgeAction party -BotGuid $leader -MemberGuid $memberGuids | Out-Null
        }
        # Staging is a one-time recovery/bootstrap step. Once the quest director has recorded a
        # leader state, the party resumes from its persisted world position rather than teleporting
        # back to a starter zone on every server restart.
        $state = @(Invoke-LeagueSql -Sql "SELECT leader_guid FROM $($playerbotsDb[4]).autowow_league_quest_state WHERE leader_guid=$leader LIMIT 1;")
        if ($state.Count -eq 0) {
            $route = if ($team -eq 'northstar') { 'teldrassil' } else { 'valley-of-trials' }
            Invoke-Bridge -BridgeAction route -BotGuid $leader -Destination $route | Out-Null
            Write-LeagueEvent -EventType 'party_bootstrap_staged' -TeamId $team -CharacterGuid $leader -Payload @{ route = $route; party_members = 5 }
        }
        else {
            Write-LeagueEvent -EventType 'party_resumed' -TeamId $team -CharacterGuid $leader -Payload @{ party_members = 5 }
        }

        # The quest bridge uses Playerbots' genuine quest, RPG-POI, combat, and reward actions.
        # It intentionally leaves a party idle if no valid quest work is available.
        $independence = Invoke-IndependentQuestDispatch -LeaderGuid $leader
        Write-LeagueEvent -EventType 'party_independence_rearmed' -TeamId $team -CharacterGuid $leader `
            -Payload @{ attempts = $independence.attempts; party_members = 5 }
    }

    $workerGuids = @($members | Where-Object { $_.affiliation -eq 'wayfarer' } | ForEach-Object { [uint32]$_.guid })
    # Neutral workers stay offline until the dedicated economic-worker loop exists; they must not be
    # represented as gathering production merely because a random-movement profile was enabled.
    $bridgeBots = @((Invoke-Bridge -BridgeAction list).bots | ForEach-Object { [uint32]$_.guid })
    foreach ($workerGuid in $workerGuids) {
        if (-not $PreserveOtherFleets -and $workerGuid -in $bridgeBots) {
            Invoke-Bridge -BridgeAction deactivate -BotGuid $workerGuid | Out-Null
        }
        Write-LeagueEvent -EventType $(if ($PreserveOtherFleets) { 'worker_preserved' } else { 'worker_offline' }) `
            -CharacterGuid $workerGuid -Payload @{ reason = if ($PreserveOtherFleets) { 'unrelated_fleet_preserved' } else { 'economic_loop_not_implemented' } }
    }

    $activeSql = if ($PreserveOtherFleets) {
        "UPDATE $($playerbotsDb[4]).autowow_league_member SET active=1 WHERE character_guid IN ($($targetGuids -join ','));"
    }
    else {
        "UPDATE $($playerbotsDb[4]).autowow_league_member SET active=CASE WHEN character_guid IN ($($targetGuids -join ',')) THEN 1 ELSE 0 END WHERE character_guid IN ($($allMemberGuids -join ','));"
    }
    Invoke-LeagueSql -Sql $activeSql -Silent
    Write-LeagueEvent -EventType 'simulation_launched' -Payload @{ active_bots = $targetGuids.Count; parties = 2; workers = 1; preserve_other_fleets = [bool]$PreserveOtherFleets; travel = 'one_time_bootstrap_then_persistent_positions'; behavior = 'party_questing_worker_offline' }
}

function Issue-Challenge {
    if ($IssuerTeam -eq $TargetTeam) { throw 'A guild cannot challenge itself.' }
    Install-LeagueSchema | Out-Null
    $sql = "INSERT INTO $($playerbotsDb[4]).autowow_league_challenge (issuer_team_id,target_team_id,battleground,squad_size,status,respond_by,notes) VALUES ($(ConvertTo-SqlLiteral $IssuerTeam),$(ConvertTo-SqlLiteral $TargetTeam),$(ConvertTo-SqlLiteral $Battleground),$SquadSize,'pending',DATE_ADD(UTC_TIMESTAMP(), INTERVAL 30 MINUTE),'Awaiting named-roster confirmation'); SELECT LAST_INSERT_ID();"
    $id = @(Invoke-LeagueSql -Sql $sql | Select-Object -Last 1)[0]
    Write-LeagueEvent -EventType 'challenge_issued' -TeamId $IssuerTeam -Payload @{ challenge_id = $id; target = $TargetTeam; battleground = $Battleground; squad_size = $SquadSize }
    return [uint64]$id
}

function Answer-Challenge {
    if ($ChallengeId -eq 0) { throw 'ChallengeId is required.' }
    $status = if ($Response -eq 'accept') { 'accepted' } else { 'forfeit' }
    $winner = if ($Response -eq 'decline') { $null } else { $null }
    $extra = if ($Response -eq 'decline') { ", boon_key='forfeit_xp_2pct', boon_expires_at=DATE_ADD(UTC_TIMESTAMP(), INTERVAL 30 MINUTE)" } else { '' }
    $sql = "UPDATE $($playerbotsDb[4]).autowow_league_challenge SET status=$(ConvertTo-SqlLiteral $status)$extra WHERE challenge_id=$ChallengeId AND status='pending'; SELECT ROW_COUNT();"
    $affected = [int](@(Invoke-LeagueSql -Sql $sql | Select-Object -Last 1)[0])
    if ($affected -ne 1) { throw "Challenge $ChallengeId is not pending." }
    Write-LeagueEvent -EventType ("challenge_{0}" -f $Response) -Payload @{ challenge_id = $ChallengeId }
}

function Resolve-Challenge {
    if ($ChallengeId -eq 0) { throw 'ChallengeId is required.' }
    if ([string]::IsNullOrWhiteSpace($EvidencePath) -or -not (Test-Path -LiteralPath $EvidencePath)) { throw 'Resolve requires an existing observed-result evidence file.' }
    $evidence = (Resolve-Path -LiteralPath $EvidencePath).Path
    $sql = "UPDATE $($playerbotsDb[4]).autowow_league_challenge SET status='resolved', winner_team_id=$(ConvertTo-SqlLiteral $WinnerTeam), boon_key='wsg_xp_5pct', boon_expires_at=DATE_ADD(UTC_TIMESTAMP(), INTERVAL 2 HOUR), notes=$(ConvertTo-SqlLiteral ('Observed result: ' + $evidence)) WHERE challenge_id=$ChallengeId AND status='accepted'; SELECT ROW_COUNT();"
    $affected = [int](@(Invoke-LeagueSql -Sql $sql | Select-Object -Last 1)[0])
    if ($affected -ne 1) { throw "Challenge $ChallengeId is not accepted." }
    Write-LeagueEvent -EventType 'challenge_resolved' -TeamId $WinnerTeam -Payload @{ challenge_id = $ChallengeId; evidence = $evidence; boon = 'wsg_xp_5pct'; duration_minutes = 120 }
}

switch ($Action) {
    'install' {
        $result = [ordered]@{ installed_tables = @(Install-LeagueSchema) }
        if ($AsJson) { $result | ConvertTo-Json -Depth 8 } else { "League schema installed: $($result.installed_tables -join ', ')" }
    }
    'seed' {
        Seed-League
        if ($AsJson) { Get-LeagueStatus | ConvertTo-Json -Depth 8 } else { 'League v0 seed completed.' }
    }
    'launch' {
        Launch-League
        if ($AsJson) { Get-LeagueStatus | ConvertTo-Json -Depth 8 } else { 'League v0 is running: two persistent quest parties are active; the Wayfarer worker is offline.' }
    }
    'roster' {
        $result = @(Get-LeagueMembers)
        if ($AsJson) { $result | ConvertTo-Json -Depth 8 } else { $result | Format-Table guid,team,affiliation,role,class_plan,name,level,active,online -AutoSize }
    }
    'record-quest-state' {
        Record-QuestState
        if ($AsJson) {
            [ordered]@{ leader_guid = $LeaderGuid; team_id = $TeamId; quest_id = $QuestId; phase = $QuestPhase } | ConvertTo-Json
        }
    }
    'status' {
        $result = Get-LeagueStatus
        if ($AsJson) { $result | ConvertTo-Json -Depth 8 } else { $result | ConvertTo-Json -Depth 8 }
    }
    'issue-challenge' {
        $id = Issue-Challenge
        if ($AsJson) { [ordered]@{ challenge_id = $id; status = 'pending' } | ConvertTo-Json } else { "Challenge issued: $id" }
    }
    'answer-challenge' {
        Answer-Challenge
        if ($AsJson) { [ordered]@{ challenge_id = $ChallengeId; response = $Response } | ConvertTo-Json } else { "Challenge $ChallengeId recorded as $Response." }
    }
    'resolve-challenge' {
        Resolve-Challenge
        if ($AsJson) { [ordered]@{ challenge_id = $ChallengeId; winner = $WinnerTeam; status = 'resolved' } | ConvertTo-Json } else { "Challenge $ChallengeId resolved for $WinnerTeam." }
    }
}
