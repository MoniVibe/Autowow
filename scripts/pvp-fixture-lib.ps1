Set-StrictMode -Version Latest

$script:PvpFixtureReceiptSchema = 'autowow.pvp.fixture.receipt.v1'
$script:PvpFixtureAcceptanceSchema = 'autowow.pvp.fixture.acceptance.v1'
$script:PvpFixtureBridgeActions = @(
    'list',
    'activate',
    'deactivate',
    'party',
    'rally',
    'route',
    'snapshot',
    'pause',
    'resume',
    'engage',
    'wsg-queue',
    'wsg-status',
    'wsg-leave'
)

function ConvertTo-PvpFixtureSqlLiteral {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [AllowEmptyString()]
        [string]$Value
    )

    return "'$(($Value -replace "'", "''"))'"
}

function ConvertTo-PvpFixtureSqlIdentifier {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [string]$Value
    )

    if ($Value -notmatch '^[A-Za-z0-9_]+$') {
        throw "Unsafe SQL identifier: $Value"
    }

    return ('`' + $Value + '`')
}

function Get-PvpFixtureDefinition {
    [CmdletBinding()]
    param(
        [ValidatePattern('^[A-Za-z][A-Za-z0-9]{2,7}$')]
        [string]$FixtureId = 'awpvp1',

        [ValidateRange(1, 80)]
        [int]$Level = 80
    )

    $tag = $FixtureId.ToUpperInvariant()
    $teamPrefix = $FixtureId.ToLowerInvariant()

    $anchor = [ordered]@{
        name = 'ValleyOfTrials'
        map = 1
        x = -615.50
        y = -4326.00
        z = 40.09
        o = 0.0
    }

    $accounts = @()
    foreach ($side in @(
        [pscustomobject]@{ Key = 'A'; Faction = 'Alliance'; Race = 1; Map = 0; X = -8949.95; Y = -132.493; Z = 83.5312; O = 0.0 },
        [pscustomobject]@{ Key = 'H'; Faction = 'Horde'; Race = 2; Map = 1; X = -618.518; Y = -4251.67; Z = 38.718; O = 0.0 }
    )) {
        foreach ($slot in 1..10) {
            $accountName = '{0}{1}{2:D2}' -f $tag, $side.Key, $slot
            $slotLetter = [char]([int][char]'a' + $slot - 1)
            # Account IDs may contain digits, but 3.3.5 character names are
            # letters-only and use normal title casing.
            $characterName = if ($side.Key -eq 'A') { "Awallia$slotLetter" } else { "Awhorde$slotLetter" }
            $accounts += [pscustomobject]@{
                account_name = $accountName
                character_name = $characterName
                faction = $side.Faction
                race = $side.Race
                class = 1
                gender = 0
                level = $Level
                fixture_id = $FixtureId
                position = [pscustomobject]@{
                    map = [int]$side.Map
                    x = [double]$side.X
                    y = [double]$side.Y
                    z = [double]$side.Z
                    o = [double]$side.O
                }
            }
        }
    }

    $stages = @(
        [pscustomobject]@{
            stage_id = 'world-pvp-2v2'
            ordinal = 1
            kind = 'world_pvp'
            display_name = 'World PvP 2v2'
            players_per_side = 2
            map = 1
            supported_start = $true
            supported_stop = $true
            start_surface = 'bridge.engage'
            stop_surface = 'bridge.pause+deactivate'
            rally_route = 'valley-of-trials'
            rally_anchor = [pscustomobject]$anchor
        },
        [pscustomobject]@{
            stage_id = 'ladder-3v3'
            ordinal = 2
            kind = 'arena_ladder'
            display_name = 'Arena ladder 3v3'
            players_per_side = 3
            map = $null
            supported_start = $false
            supported_stop = $false
            start_surface = 'blocked: no deterministic local queue/start API in current bridge/console surface'
            stop_surface = 'blocked: no deterministic local queue/stop API in current bridge/console surface'
            rally_anchor = [pscustomobject]$anchor
        },
        [pscustomobject]@{
            stage_id = 'wsg-10v10'
            ordinal = 3
            kind = 'warsong_gulch'
            display_name = 'Warsong Gulch 10v10'
            players_per_side = 10
            map = 489
            supported_start = $true
            supported_stop = $true
            start_surface = 'bridge.wsg queue (one atomic ordered 20-GUID request)'
            status_surface = 'bridge.wsg status (one atomic ordered 20-GUID request)'
            stop_surface = 'bridge.wsg leave (one atomic ordered 20-GUID request)'
            proof_harness = 'wsg-10v10-proof.ps1'
            requires_party = $false
            requires_rally = $false
            rally_anchor = [pscustomobject]$anchor
        }
    )

    $teams = @(
        [pscustomobject]@{
            team_id = "$teamPrefix-a"
            display_name = "$tag Alliance PvP Fixture"
            faction = 'Alliance'
            roster_cap = 10
            worker_cap = 0
            members = @($accounts | Where-Object faction -eq 'Alliance')
        },
        [pscustomobject]@{
            team_id = "$teamPrefix-h"
            display_name = "$tag Horde PvP Fixture"
            faction = 'Horde'
            roster_cap = 10
            worker_cap = 0
            members = @($accounts | Where-Object faction -eq 'Horde')
        }
    )

    return [pscustomobject]@{
        schema_version = 1
        fixture_id = $FixtureId
        tag = $tag
        local_only = $true
        level = $Level
        account_password_env = 'WOW_ACCOUNT_PASSWORD'
        anchor = [pscustomobject]$anchor
        accounts = @($accounts)
        teams = @($teams)
        stages = @($stages)
    }
}

function Assert-PvpFixtureDefinition {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [psobject]$Definition
    )

    if ($Definition.local_only -ne $true) {
        throw 'PvP fixture definitions must be local-only.'
    }

    $accounts = @($Definition.accounts)
    if ($accounts.Count -ne 20) {
        throw "PvP fixture must contain exactly 20 accounts/characters; found $($accounts.Count)."
    }

    $duplicateNames = @($accounts | Group-Object character_name | Where-Object Count -gt 1)
    if ($duplicateNames.Count -gt 0) {
        throw "Duplicate fixture character name: $($duplicateNames[0].Name)"
    }

    foreach ($member in $accounts) {
        if ($member.account_name -notmatch '^[A-Z][A-Z0-9]{2,15}$') {
            throw "Fixture account name is outside the safe account-name shape: $($member.account_name)"
        }
        if ($member.character_name -notmatch '^[A-Z][a-z]{2,11}$') {
            throw "Fixture character name is outside the safe character-name shape: $($member.character_name)"
        }
        if ([int]$member.level -ne [int]$Definition.level) {
            throw "Fixture level mismatch for $($member.character_name)."
        }
        if ($member.faction -notin @('Alliance', 'Horde')) {
            throw "Fixture faction is not supported: $($member.faction)"
        }
        if ([int]$member.race -notin @(1, 2)) {
            throw "Fixture race is not supported: $($member.race)"
        }
    }

    if (@($accounts | Where-Object faction -eq 'Alliance').Count -ne 10 -or
        @($accounts | Where-Object faction -eq 'Horde').Count -ne 10) {
        throw 'PvP fixture must contain exactly ten Alliance and ten Horde members.'
    }

    $expectedStageIds = @('world-pvp-2v2', 'ladder-3v3', 'wsg-10v10')
    $stages = @($Definition.stages | Sort-Object ordinal)
    if (($stages.stage_id -join ',') -ne ($expectedStageIds -join ',')) {
        throw 'PvP fixture stages are not in the required 2v2, 3v3, WSG order.'
    }
    if ([int]$stages[0].players_per_side -ne 2 -or
        [int]$stages[1].players_per_side -ne 3 -or
        [int]$stages[2].players_per_side -ne 10) {
        throw 'PvP fixture stage sizes are not 2v2, 3v3, and 10v10.'
    }

    if ($stages[0].supported_start -ne $true -or $stages[0].supported_stop -ne $true) {
        throw 'World PvP 2v2 must have supported bridge start and stop surfaces.'
    }
    if ($stages[1].supported_start -ne $false) {
        throw 'The unsupported arena ladder stage must fail closed.'
    }
    if ($stages[2].supported_start -ne $true -or $stages[2].supported_stop -ne $true -or
        [bool]$stages[2].requires_party -or [bool]$stages[2].requires_rally) {
        throw 'WSG 10v10 must use supported atomic queue/status/leave surfaces without grouping or rally.'
    }

    return $true
}

function Get-PvpFixtureTeamForMember {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [psobject]$Definition,

        [Parameter(Mandatory)]
        [psobject]$Member
    )

    $team = @($Definition.teams | Where-Object faction -eq $Member.faction)
    if ($team.Count -ne 1) {
        throw "Could not resolve one fixture team for $($Member.character_name)."
    }
    return $team[0]
}

function Get-PvpFixtureStageMembers {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [psobject]$Definition,

        [Parameter(Mandatory)]
        [psobject]$Stage
    )

    $size = [int]$Stage.players_per_side
    return [pscustomobject]@{
        alliance = @($Definition.accounts | Where-Object faction -eq 'Alliance' | Select-Object -First $size)
        horde = @($Definition.accounts | Where-Object faction -eq 'Horde' | Select-Object -First $size)
    }
}

function Get-PvpFixtureAcceptanceContract {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [psobject]$Definition
    )

    return [ordered]@{
        schema = $script:PvpFixtureAcceptanceSchema
        schema_version = 1
        fixture_id = $Definition.fixture_id
        invariants = [ordered]@{
            local_only = $true
            explicit_execute_switch = $true
            dry_run_default = $true
            expected_account_count = 20
            expected_character_count = 20
            expected_alliance_count = 10
            expected_horde_count = 10
            expected_level = $Definition.level
            equal_level_field = 'progress.level'
            team_membership_field = 'autowow_league_member.team_id'
            world_pvp_bracket = '2v2'
            ladder_bracket = '3v3'
            wsg_bracket = '10v10'
            wsg_atomic_roster_guid_count = 20
            wsg_poll_interval_seconds = '1..2'
            wsg_max_wait_minutes = 35
            wsg_queue_instance_proof = 'same nonzero instance_id and exact 10 Alliance + 10 Horde roster'
            wsg_gameplay_telemetry_proof = 'kill/death and flag state/capture/return deltas plus final score and winner'
            wsg_no_grouping = $true
            rally_group_field = 'group.members'
            rally_leader_field = 'group.leader_guid'
            rally_position_tolerance = 0.5
            world_pvp_start_proof = 'opposing_target_guid_observed >= 1'
            world_pvp_stop_proof = 'paused_all_stage_bots = true and online_stage_bots = 0'
            unsupported_stage_proof = 'stage_blocked with explicit unsupported_surface reason'
            forbidden_mutation_proof = 'unrelated_mutation_count = 0'
        }
        required_receipt_events = @(
            'run_started',
            'fixture_definition',
            'acceptance_contract',
            'fixture_roster_verified',
            'equal_level_verified',
            'team_membership_verified',
            'stage_rallied',
            'telemetry_sample',
            'stage_started',
            'stage_stopped',
            'stage_blocked',
            'wsg_queue_dispatched',
            'wsg_instance_verified',
            'wsg_gameplay_telemetry',
            'wsg_result',
            'wsg_cleanup_verified',
            'run_completed'
        )
        telemetry_fields = @(
            'stage_id',
            'phase',
            'fixture_id',
            'team_id',
            'guid',
            'character_name',
            'online',
            'alive',
            'combat',
            'paused',
            'level',
            'map',
            'position_x',
            'position_y',
            'position_z',
            'position_o',
            'health_pct',
            'target_guid',
            'target_name',
            'target_health_pct',
            'group_members',
            'group_leader_guid',
            'rally_distance',
            'opposing_target_observed'
            'instance_id'
            'queue_state'
            'battleground_state'
            'kills'
            'deaths'
            'flag_state'
            'flag_captures'
            'flag_returns'
            'alliance_score'
            'horde_score'
            'winner'
        )
        aggregate_fields = @(
            'online_stage_bots',
            'alive_stage_bots',
            'combat_stage_bots',
            'same_level_count',
            'opposing_target_count',
            'rally_max_distance',
            'team_population',
            'bridge_commands',
            'console_commands',
            'db_read_statements',
            'db_write_statements',
            'unrelated_mutation_count',
            'unsupported_stage_count'
            'exact_wsg_roster_count'
            'wsg_kill_delta'
            'wsg_death_delta'
            'wsg_flag_state_delta'
            'wsg_flag_capture_delta'
            'wsg_flag_return_delta'
        )
    }
}

function Get-PvpFixtureBridgePlan {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [psobject]$Definition
    )

    $plan = @()
    foreach ($stage in @($Definition.stages | Sort-Object ordinal)) {
        $members = Get-PvpFixtureStageMembers -Definition $Definition -Stage $stage
        if ($stage.kind -eq 'warsong_gulch') {
            $orderedRoster = @($members.alliance) + @($members.horde)
            $plan += [pscustomobject]@{
                stage_id = $stage.stage_id
                side = 'both'
                activate = @($orderedRoster | ForEach-Object character_name)
                party_leader = $null
                party_members = @()
                rally = $null
                start = 'wsg-queue'
                status = 'wsg-status'
                stop = @('wsg-leave', 'deactivate')
                atomic_roster_count = $orderedRoster.Count
                proof_harness = $stage.proof_harness
                unsupported_surface = $null
            }
            continue
        }
        foreach ($side in @('alliance', 'horde')) {
            $sideMembers = @($members.$side)
            $plan += [pscustomobject]@{
                stage_id = $stage.stage_id
                side = $side
                activate = @($sideMembers | ForEach-Object character_name)
                party_leader = $sideMembers[0].character_name
                party_members = @($sideMembers | Select-Object -Skip 1 | ForEach-Object character_name)
                rally = $sideMembers[0].character_name
                rally_route = if ($stage.PSObject.Properties['rally_route']) { $stage.rally_route } else { $null }
                start = if ($stage.supported_start) { 'engage' } else { $null }
                stop = if ($stage.supported_stop) { @('pause', 'deactivate') } else { $null }
                unsupported_surface = if ($stage.supported_start) { $null } else { $stage.start_surface }
            }
        }
    }
    return @($plan)
}

function Get-PvpFixturePlan {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [psobject]$Definition
    )

    Assert-PvpFixtureDefinition -Definition $Definition | Out-Null

    $accountCommands = @($Definition.accounts | ForEach-Object {
        [ordered]@{
            command = ".account create $($_.account_name) <WOW_ACCOUNT_PASSWORD>"
            scope = $_.account_name
            would_execute = $true
        }
    })

    $characterSqlScope = @($Definition.accounts | ForEach-Object {
        [ordered]@{
            character_name = $_.character_name
            account_name = $_.account_name
            mutation = 'insert only when exact account exists and exact character name is absent'
            scope_predicate = "account=<resolved:$($_.account_name)> AND name=$($_.character_name)"
            would_execute = $true
        }
    })

    return [ordered]@{
        fixture_id = $Definition.fixture_id
        local_only = $true
        account_console_commands = $accountCommands
        character_db_mutations = $characterSqlScope
        team_db_mutations = @(
            'upsert exactly two autowow_league_team rows with fixture-prefixed team_id values',
            'upsert exactly twenty autowow_league_member rows keyed by the resolved fixture character_guid values'
        )
        bridge_commands = @(Get-PvpFixtureBridgePlan -Definition $Definition)
        stages = @($Definition.stages | Sort-Object ordinal | ForEach-Object {
            [ordered]@{
                ordinal = $_.ordinal
                stage_id = $_.stage_id
                display_name = $_.display_name
                players_per_side = $_.players_per_side
                start_surface = $_.start_surface
                stop_surface = $_.stop_surface
                supported_start = $_.supported_start
                supported_stop = $_.supported_stop
            }
        })
        acceptance = Get-PvpFixtureAcceptanceContract -Definition $Definition
    }
}

function New-PvpFixtureCharacterInsertSql {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [string]$CharactersDatabaseName,

        [Parameter(Mandatory)]
        [int]$AccountId,

        [Parameter(Mandatory)]
        [psobject]$Member
    )

    $database = ConvertTo-PvpFixtureSqlIdentifier -Value $CharactersDatabaseName
    $name = ConvertTo-PvpFixtureSqlLiteral -Value $Member.character_name
    $map = [int]$Member.position.map
    $x = [double]$Member.position.x
    $y = [double]$Member.position.y
    $z = [double]$Member.position.z
    $o = [double]$Member.position.o
    $level = [int]$Member.level
    $lockName = ConvertTo-PvpFixtureSqlLiteral -Value 'autowow.pvp.fixture'

    return @"
SELECT GET_LOCK($lockName, 30) INTO @pvp_fixture_lock;
START TRANSACTION;
SELECT COALESCE(MAX(guid), 0) + 1 INTO @pvp_fixture_guid FROM $database.characters;
INSERT INTO $database.characters
    (guid, account, name, race, class, gender, level, map, position_x, position_y, position_z, orientation, taximask, cinematic, health, innTriggerId, at_login)
SELECT @pvp_fixture_guid, $AccountId, $name, $([int]$Member.race), $([int]$Member.class), $([int]$Member.gender), $level, $map, $x, $y, $z, $o, '', 1, 1, 0, 0
WHERE @pvp_fixture_lock = 1
  AND NOT EXISTS (SELECT 1 FROM $database.characters WHERE name = $name OR guid = @pvp_fixture_guid);
INSERT INTO $database.character_homebind (guid, mapId, zoneId, posX, posY, posZ)
SELECT @pvp_fixture_guid,
       CASE WHEN $([int]$Member.race) = 1 THEN 0 ELSE 1 END,
       CASE WHEN $([int]$Member.race) = 1 THEN 12 ELSE 14 END,
       CASE WHEN $([int]$Member.race) = 1 THEN -8949.95 ELSE -618.518 END,
       CASE WHEN $([int]$Member.race) = 1 THEN -132.493 ELSE -4251.67 END,
       CASE WHEN $([int]$Member.race) = 1 THEN 83.5312 ELSE 38.718 END
WHERE @pvp_fixture_lock = 1
  AND EXISTS (SELECT 1 FROM $database.characters WHERE guid = @pvp_fixture_guid AND account = $AccountId AND name = $name)
  AND NOT EXISTS (SELECT 1 FROM $database.character_homebind WHERE guid = @pvp_fixture_guid);
SELECT @pvp_fixture_guid AS allocated_guid, ROW_COUNT() AS inserted_rows, @pvp_fixture_lock AS lock_acquired;
COMMIT;
SELECT RELEASE_LOCK($lockName) AS released;
"@
}

function New-PvpFixtureTeamMembershipSql {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [string]$PlayerbotsDatabaseName,

        [Parameter(Mandatory)]
        [psobject]$Definition,

        [Parameter(Mandatory)]
        [hashtable]$CharacterGuidByName
    )

    $database = ConvertTo-PvpFixtureSqlIdentifier -Value $PlayerbotsDatabaseName
    $statements = New-Object System.Collections.Generic.List[string]

    foreach ($team in @($Definition.teams)) {
        $teamId = ConvertTo-PvpFixtureSqlLiteral -Value $team.team_id
        $displayName = ConvertTo-PvpFixtureSqlLiteral -Value $team.display_name
        $faction = ConvertTo-PvpFixtureSqlLiteral -Value $team.faction
        $rosterCap = [int]$team.roster_cap
        $statements.Add("INSERT INTO $database.autowow_league_team (team_id,display_name,faction,roster_cap,worker_cap,treasury_copper) VALUES ($teamId,$displayName,$faction,$rosterCap,0,0) ON DUPLICATE KEY UPDATE display_name=VALUES(display_name),faction=VALUES(faction),roster_cap=VALUES(roster_cap),worker_cap=VALUES(worker_cap);")
    }

    foreach ($member in @($Definition.accounts)) {
        if (-not $CharacterGuidByName.ContainsKey($member.character_name)) {
            throw "Missing character GUID for fixture member $($member.character_name)."
        }
        $guid = [int]$CharacterGuidByName[$member.character_name]
        $team = Get-PvpFixtureTeamForMember -Definition $Definition -Member $member
        $teamId = ConvertTo-PvpFixtureSqlLiteral -Value $team.team_id
        $classPlan = ConvertTo-PvpFixtureSqlLiteral -Value 'warrior'
        $statements.Add("INSERT INTO $database.autowow_league_member (character_guid,team_id,affiliation,role,class_plan,profession_one,profession_two,active,retired_at) VALUES ($guid,$teamId,'guild','adventurer',$classPlan,'','',0,NULL) ON DUPLICATE KEY UPDATE team_id=VALUES(team_id),class_plan=VALUES(class_plan),active=0,retired_at=NULL;")
    }

    return ($statements -join "`n")
}

function New-PvpFixtureActiveMembershipSql {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [string]$PlayerbotsDatabaseName,

        [Parameter(Mandatory)]
        [int[]]$CharacterGuids,

        [Parameter(Mandatory)]
        [bool]$Active,

        [string[]]$TeamIds = @()
    )

    if ($CharacterGuids.Count -lt 1) {
        throw 'At least one exact fixture character GUID is required for active membership mutation.'
    }
    $database = ConvertTo-PvpFixtureSqlIdentifier -Value $PlayerbotsDatabaseName
    $guidList = (($CharacterGuids | ForEach-Object { [int]$_ }) -join ',')
    $activeValue = if ($Active) { 1 } else { 0 }
    $teamPredicate = if ($TeamIds.Count -gt 0) {
        $teamList = (($TeamIds | ForEach-Object { ConvertTo-PvpFixtureSqlLiteral -Value $_ }) -join ',')
        " AND team_id IN ($teamList)"
    }
    else { '' }
    return "UPDATE $database.autowow_league_member SET active=$activeValue WHERE character_guid IN ($guidList)$teamPredicate;"
}

function New-PvpFixtureCleanupSql {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [string]$PlayerbotsDatabaseName,

        [Parameter(Mandatory)]
        [string]$CharactersDatabaseName,

        [Parameter(Mandatory)]
        [int[]]$CharacterGuids,

        [Parameter(Mandatory)]
        [string[]]$TeamIds
    )

    if ($CharacterGuids.Count -ne 20 -or @($CharacterGuids | Sort-Object -Unique).Count -ne 20 -or $TeamIds.Count -ne 2) {
        throw 'Cleanup requires the exact twenty-character scope and two exact fixture team ids.'
    }
    $playerbots = ConvertTo-PvpFixtureSqlIdentifier -Value $PlayerbotsDatabaseName
    $characters = ConvertTo-PvpFixtureSqlIdentifier -Value $CharactersDatabaseName
    $guidList = (($CharacterGuids | ForEach-Object { [int]$_ }) -join ',')
    $teamList = (($TeamIds | ForEach-Object { ConvertTo-PvpFixtureSqlLiteral -Value $_ }) -join ',')

    return @"
DELETE FROM $playerbots.autowow_league_member WHERE character_guid IN ($guidList) AND team_id IN ($teamList);
DELETE FROM $playerbots.autowow_league_team WHERE team_id IN ($teamList);
DELETE FROM $characters.characters WHERE guid IN ($guidList);
"@
}

function Get-PvpFixtureSafeBridgeActions {
    return @($script:PvpFixtureBridgeActions)
}
