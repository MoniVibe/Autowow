Set-StrictMode -Version Latest

$script:DungeonFixturePlanSchema = 'autowow.dungeon.fixture.plan.v1'
$script:DungeonFixtureReceiptSchema = 'autowow.dungeon.fixture.receipt.v1'
$script:DungeonFixtureAcceptanceSchema = 'autowow.dungeon.fixture.acceptance.v1'
$script:DungeonFixtureEvidenceSchema = 'autowow.dungeon.fixture.evidence.v1'
$script:DungeonFixtureStageIds = @('readiness', 'elite-pack', 'ingvar')
$script:DungeonFixtureBridgeActions = @('list', 'activate', 'deactivate', 'party', 'snapshot', 'pause', 'resume')

function Get-DungeonFixtureProperty {
    [CmdletBinding()]
    param(
        [AllowNull()][object]$Object,
        [Parameter(Mandatory = $true)][string]$Name,
        [AllowNull()][object]$Default = $null
    )

    if ($null -eq $Object) {
        return $Default
    }

    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) {
        return $Default
    }

    return $property.Value
}

function Get-DungeonFixtureArray {
    [CmdletBinding()]
    param([AllowNull()][object]$Value)

    if ($null -eq $Value) {
        return @()
    }

    return @($Value)
}

function ConvertTo-DungeonFixtureSqlLiteral {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [string]$Value
    )

    return ("'{0}'" -f ($Value -replace "'", "''"))
}

function ConvertTo-DungeonFixtureSqlIdentifier {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][string]$Value)

    if ($Value -notmatch '^[A-Za-z0-9_]+$') {
        throw "Unsafe SQL identifier: $Value"
    }

    return ('`' + $Value + '`')
}

function Assert-DungeonFixtureId {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][string]$FixtureId)

    if ($FixtureId -notmatch '^[A-Za-z][A-Za-z0-9]{2,6}$') {
        throw "FixtureId '$FixtureId' must be 3-7 alphanumeric characters and start with a letter."
    }
}

function Get-DungeonFixtureDefinition {
    [CmdletBinding()]
    param(
        [string]$FixtureId = 'awdk1',
        [ValidateRange(68, 80)][int]$Level = 70,
        [ValidateSet('common', 'uncommon', 'rare', 'epic', 'legendary')][string]$GearQuality = 'rare'
    )

    Assert-DungeonFixtureId -FixtureId $FixtureId

    $tag = $FixtureId.ToUpperInvariant()
    $memberRows = @(
        [ordered]@{ fixture_role = 'tank'; name_token = 'TANK'; class_name = 'Paladin'; class_id = 2; role = 'tank'; spec_name = 'prot'; spec_label = 'prot pve'; spec_index = 1 },
        [ordered]@{ fixture_role = 'healer'; name_token = 'HEAL'; class_name = 'Priest'; class_id = 5; role = 'healer'; spec_name = 'disc'; spec_label = 'disc pve'; spec_index = 0 },
        [ordered]@{ fixture_role = 'dps-mage'; name_token = 'MAGE'; class_name = 'Mage'; class_id = 8; role = 'dps'; spec_name = 'fire'; spec_label = 'fire pve'; spec_index = 1 },
        [ordered]@{ fixture_role = 'dps-rogue'; name_token = 'ROGUE'; class_name = 'Rogue'; class_id = 4; role = 'dps'; spec_name = 'combat'; spec_label = 'combat pve'; spec_index = 1 },
        [ordered]@{ fixture_role = 'dps-hunter'; name_token = 'HUNT'; class_name = 'Hunter'; class_id = 3; role = 'dps'; spec_name = 'bm'; spec_label = 'bm pve'; spec_index = 0 }
    )

    $members = foreach ($row in $memberRows) {
        $name = '{0}{1}' -f $tag, $row.name_token
        [pscustomobject][ordered]@{
            fixture_role = $row.fixture_role
            account_name = $name
            character_name = $name
            faction = 'Alliance'
            class_name = $row.class_name
            class_id = $row.class_id
            role = $row.role
            spec_name = $row.spec_name
            spec_label = $row.spec_label
            spec_index = $row.spec_index
            level = $Level
            gear_quality = $GearQuality
            template_provider = 'PlayerbotFactory.Randomize(false)'
            account_password_env = 'WOW_ACCOUNT_PASSWORD'
        }
    }

    $definition = [pscustomobject][ordered]@{
        schema = 'autowow.dungeon.fixture.definition.v1'
        schema_version = 1
        fixture_id = $FixtureId
        tag = $tag
        local_only = $true
        disposable = $true
        faction = 'Alliance'
        level = $Level
        gear_quality = $GearQuality
        dungeon = [pscustomobject][ordered]@{
            target_id = 'utgarde-keep-normal'
            display_name = 'Utgarde Keep (Normal)'
            map_id = 574
            difficulty = 0
            boss = 'Ingvar the Plunderer'
        }
        members = @($members)
        stages = @(
            [pscustomobject][ordered]@{
                stage_id = 'readiness'
                ordinal = 0
                display_name = 'Readiness and party formation'
                start_intent = 'read-only preflight and exact fixture roster resolution'
            },
            [pscustomobject][ordered]@{
                stage_id = 'elite-pack'
                ordinal = 1
                display_name = 'First Utgarde Keep elite pack'
                start_intent = 'enter dungeon, move naturally, and engage only the first selected elite pack'
            },
            [pscustomobject][ordered]@{
                stage_id = 'ingvar'
                ordinal = 2
                display_name = 'Ingvar the Plunderer'
                start_intent = 'start only after the elite-pack gate is PASS'
            }
        )
        idempotence = [pscustomobject][ordered]@{
            identity = 'fixture_id plus exact account_name and character_name'
            existing_object_policy = 'read first; reuse only an exact owned match; repair only missing state through the approved factory surface'
            mismatch_policy = 'fail closed on class, faction, ownership, or non-fixture name mismatch'
            random_selection_allowed = $false
            direct_character_sql_allowed = $false
        }
        teardown = [pscustomobject][ordered]@{
            ownership_key = 'fixture_id and exact five-character manifest'
            preserve_receipts = $true
            delete_unrelated_rows = $false
            delete_strategy = 'future owner-aware adapter only; no hand-written DELETE statements'
        }
    }

    Assert-DungeonFixtureDefinition -Definition $definition | Out-Null
    return $definition
}

function Assert-DungeonFixtureDefinition {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][psobject]$Definition)

    if ($Definition.schema -ne 'autowow.dungeon.fixture.definition.v1') {
        throw 'Unexpected dungeon fixture definition schema.'
    }
    if ($Definition.local_only -ne $true -or $Definition.disposable -ne $true) {
        throw 'Dungeon fixtures must be local-only and disposable.'
    }
    if ([int]$Definition.level -lt 68 -or [int]$Definition.level -gt 80) {
        throw 'Utgarde Keep fixture level must be between 68 and 80.'
    }
    if ($Definition.dungeon.target_id -ne 'utgarde-keep-normal' -or [int]$Definition.dungeon.map_id -ne 574 -or
        [int]$Definition.dungeon.difficulty -ne 0 -or $Definition.dungeon.boss -ne 'Ingvar the Plunderer') {
        throw 'Dungeon target must be Utgarde Keep normal with Ingvar as the final boss.'
    }

    $members = Get-DungeonFixtureArray -Value $Definition.members
    if ($members.Count -ne 5) {
        throw "Dungeon fixture must contain exactly five members; found $($members.Count)."
    }

    $roles = @('tank', 'healer', 'dps-mage', 'dps-rogue', 'dps-hunter')
    if ((($members.fixture_role | Sort-Object) -join ',') -ne (($roles | Sort-Object) -join ',')) {
        throw 'Dungeon fixture roles must be Prot Paladin, Disc Priest, Mage, Rogue, and Hunter.'
    }

    $seenNames = @{}
    foreach ($member in $members) {
        if ($member.character_name -notmatch '^[A-Z][A-Z0-9]{2,11}$') {
            throw "Fixture character name is outside the 12-character safe shape: $($member.character_name)"
        }
        if ($member.account_name -notmatch '^[A-Z][A-Z0-9]{2,15}$') {
            throw "Fixture account name is outside the safe account shape: $($member.account_name)"
        }
        if ($seenNames.ContainsKey($member.character_name)) {
            throw "Duplicate dungeon fixture character name: $($member.character_name)"
        }
        $seenNames[$member.character_name] = $true
        if ($member.faction -ne $Definition.faction) {
            throw "Faction mismatch for $($member.character_name)."
        }
        if ([int]$member.level -ne [int]$Definition.level) {
            throw "Level mismatch for $($member.character_name)."
        }
        if ($member.template_provider -ne 'PlayerbotFactory.Randomize(false)') {
            throw "Template provider mismatch for $($member.character_name)."
        }
    }

    $expected = @{
        'tank' = @('Paladin', 'tank', 'prot pve', 1)
        'healer' = @('Priest', 'healer', 'disc pve', 0)
        'dps-mage' = @('Mage', 'dps', 'fire pve', 1)
        'dps-rogue' = @('Rogue', 'dps', 'combat pve', 1)
        'dps-hunter' = @('Hunter', 'dps', 'bm pve', 0)
    }
    foreach ($member in $members) {
        $want = $expected[$member.fixture_role]
        if ($member.class_name -ne $want[0] -or $member.role -ne $want[1] -or $member.spec_label -ne $want[2] -or
            [int]$member.spec_index -ne [int]$want[3]) {
            throw "Role/spec contract mismatch for $($member.fixture_role)."
        }
    }

    $stageIds = @(Get-DungeonFixtureArray -Value $Definition.stages | Sort-Object ordinal | ForEach-Object stage_id)
    if (($stageIds -join ',') -ne ($script:DungeonFixtureStageIds -join ',')) {
        throw 'Dungeon fixture stages must be readiness, elite-pack, then ingvar.'
    }

    return $true
}

function Get-DungeonFixtureReadOnlyDbQueries {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][psobject]$Definition)

    Assert-DungeonFixtureDefinition -Definition $Definition | Out-Null
    $names = @($Definition.members | ForEach-Object { ConvertTo-DungeonFixtureSqlLiteral -Value $_.character_name })
    $nameList = $names -join ', '
    $characters = ConvertTo-DungeonFixtureSqlIdentifier -Value 'characters'
    $inventory = ConvertTo-DungeonFixtureSqlIdentifier -Value 'character_inventory'
    $skills = ConvertTo-DungeonFixtureSqlIdentifier -Value 'character_skills'
    $talents = ConvertTo-DungeonFixtureSqlIdentifier -Value 'character_talent'
    $spells = ConvertTo-DungeonFixtureSqlIdentifier -Value 'character_spell'
    $items = ConvertTo-DungeonFixtureSqlIdentifier -Value 'item_instance'
    $groups = ConvertTo-DungeonFixtureSqlIdentifier -Value 'groups'
    $groupMember = ConvertTo-DungeonFixtureSqlIdentifier -Value 'group_member'
    $playerbotsEvents = ConvertTo-DungeonFixtureSqlIdentifier -Value 'playerbots_random_bots'
    $accountTypes = ConvertTo-DungeonFixtureSqlIdentifier -Value 'playerbots_account_type'

    return @(
        [pscustomobject][ordered]@{
            query_id = 'characters-by-name'
            database = 'acore_characters'
            sql = "SELECT guid, account, name, race, class, level, online, map, instance_id FROM $characters WHERE name IN ($nameList);"
            purpose = 'resolve exact fixture ownership and current online/level/class/race state; faction is derived from race'
        },
        [pscustomobject][ordered]@{
            query_id = 'character-skills'
            database = 'acore_characters'
            sql = "SELECT c.name, cs.guid, cs.skill, cs.value, cs.max FROM $skills cs INNER JOIN $characters c ON c.guid = cs.guid WHERE c.name IN ($nameList);"
            purpose = 'prove factory-generated level-appropriate skills without writing character_skills'
        },
        [pscustomobject][ordered]@{
            query_id = 'character-talents-and-spells'
            database = 'acore_characters'
            sql = "SELECT c.name, ct.guid, ct.spell, ct.specMask FROM $talents ct INNER JOIN $characters c ON c.guid = ct.guid WHERE c.name IN ($nameList); SELECT c.name, cs.guid, cs.spell, cs.specMask FROM $spells cs INNER JOIN $characters c ON c.guid = cs.guid WHERE c.name IN ($nameList);"
            purpose = 'prove talents and learned spells came from the factory/template path'
        },
        [pscustomobject][ordered]@{
            query_id = 'character-equipment'
            database = 'acore_characters'
            sql = "SELECT c.name, ci.guid, ci.bag, ci.slot, ci.item, ii.itemEntry, ii.owner_guid, ii.enchantments, ii.randomPropertyId, ii.durability FROM $inventory ci INNER JOIN $characters c ON c.guid = ci.guid INNER JOIN $items ii ON ii.guid = ci.item WHERE c.name IN ($nameList);"
            purpose = 'prove equipment is backed by item_instance rows owned by the exact character'
        },
        [pscustomobject][ordered]@{
            query_id = 'playerbot-events'
            database = 'acore_playerbots'
            sql = "SELECT p.bot, p.event, p.value, p.data FROM $playerbotsEvents p WHERE p.bot IN (SELECT c.guid FROM acore_characters.$characters c WHERE c.name IN ($nameList)) ORDER BY p.bot, p.event;"
            purpose = 'observe factory/spec persistence markers; never use this query as a write substitute'
        },
        [pscustomobject][ordered]@{
            query_id = 'playerbot-account-type'
            database = 'acore_playerbots'
            sql = "SELECT pat.account_id, pat.account_type, pat.assignment_date FROM $accountTypes pat INNER JOIN acore_auth.account a ON a.id = pat.account_id WHERE a.username IN ($nameList);"
            purpose = 'verify AddClass ownership classification without changing it'
        },
        [pscustomobject][ordered]@{
            query_id = 'group-membership'
            database = 'acore_characters'
            sql = "SELECT gm.guid, gm.memberGuid, gm.subgroup, gm.roles, g.leaderGuid, g.groupType, g.difficulty FROM $groupMember gm INNER JOIN $groups g ON g.guid = gm.guid WHERE gm.memberGuid IN (SELECT c.guid FROM $characters c WHERE c.name IN ($nameList));"
            purpose = 'prove exact five-member group membership and dungeon difficulty'
        }
    )
}

function Get-DungeonFixtureGapReport {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][psobject]$Definition)

    Assert-DungeonFixtureDefinition -Definition $Definition | Out-Null
    return @(
        [pscustomobject][ordered]@{
            id = 'DNG-EXEC-001'
            severity = 'blocker'
            surface = 'account-and-character-provisioning'
            finding = 'The existing account provisioner creates or updates one account through a temporary worldserver and direct GM-row SQL; it does not create a five-account, fixture-owned character manifest.'
            required_change = 'Provide an owner-aware fixture adapter that creates or reuses exactly five accounts/characters and returns their GUIDs without accepting arbitrary names or overwriting unrelated rows.'
        },
        [pscustomobject][ordered]@{
            id = 'DNG-EXEC-002'
            severity = 'blocker'
            surface = 'playerbot-factory-and-spec'
            finding = 'PlayerbotFactory.Randomize(false) legitimately generates level, skills, talents, spells, and gear, but the current exposed commands choose a random spec; the exact spec-index helper is internal and not exposed through the bridge or console.'
            required_change = 'Expose a fixture-scoped factory operation that accepts class, level, quality, and exact spec index, then returns proof of the generated state.'
        },
        [pscustomobject][ordered]@{
            id = 'DNG-EXEC-003'
            severity = 'blocker'
            surface = 'server-console-and-bridge'
            finding = 'The bridge can activate, party, pause, resume, snapshot, and issue broad engage/scout/rally intents, but it cannot deterministically enter Utgarde Keep, select the first elite pack, or start Ingvar after a gate.'
            required_change = 'Add a local-only dungeon adapter with natural movement/entry, named stage targets, and a stage token that refuses the boss until the prior receipt gate is PASS.'
        },
        [pscustomobject][ordered]@{
            id = 'DNG-EXEC-004'
            severity = 'blocker'
            surface = 'acceptance-telemetry'
            finding = 'Current bridge snapshots omit gear completeness, exact spec, skills/talents, victim ownership, taunt confirmation, effective healing, death counters, and boss credit.'
            required_change = 'Emit versioned fixture evidence for roster/template state and per-stage combat events, including tank victim/taunt and healer effective-heal fields.'
        },
        [pscustomobject][ordered]@{
            id = 'DNG-EXEC-005'
            severity = 'blocker'
            surface = 'teardown'
            finding = 'No current bridge or console surface safely disbands and deletes only this fixture. Direct DELETE statements would risk orphaning characters, items, talents, skills, spells, group rows, and playerbot rows.'
            required_change = 'Provide an owner-aware teardown adapter that pauses/deactivates, removes the group, logs out bots, and deletes only the manifest-owned accounts/characters with referential cleanup.'
        },
        [pscustomobject][ordered]@{
            id = 'DNG-EXEC-006'
            severity = 'blocker'
            surface = 'current-configuration'
            finding = 'The checked-in local config has one AddClass account, random bots fixed at level 80, and non-deterministic class spec probabilities; changing existing config is outside this lane.'
            required_change = 'Use a fixture-scoped adapter/config overlay or a future disposable server profile; do not edit the existing config in this lane.'
        }
    )
}

function New-DungeonFixtureOperation {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][int]$Sequence,
        [Parameter(Mandatory = $true)][string]$StageId,
        [Parameter(Mandatory = $true)][string]$Surface,
        [Parameter(Mandatory = $true)][string]$Action,
        [Parameter(Mandatory = $true)][string]$Command,
        [Parameter(Mandatory = $true)][bool]$Mutation,
        [Parameter(Mandatory = $true)][bool]$Supported,
        [Parameter(Mandatory = $true)][string]$Scope,
        [Parameter(Mandatory = $true)][string]$Reason
    )

    return [pscustomobject][ordered]@{
        sequence = $Sequence
        stage_id = $StageId
        surface = $Surface
        action = $Action
        command = $Command
        mutation = $Mutation
        supported = $Supported
        scope = $Scope
        reason = $Reason
    }
}

function Get-DungeonFixtureProvisionPlan {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][psobject]$Definition)

    Assert-DungeonFixtureDefinition -Definition $Definition | Out-Null
    $operations = [System.Collections.Generic.List[object]]::new()
    $sequence = 1
    $scope = "fixture:$($Definition.fixture_id)"

    $operations.Add((New-DungeonFixtureOperation -Sequence ($sequence++) -StageId 'readiness' -Surface 'database-read-only' -Action 'probe' -Command 'SELECT exact fixture manifest rows; see read_only_db_queries' -Mutation $false -Supported $true -Scope $scope -Reason 'Read-only identity, skills, talents, spells, equipment, playerbot, and group probes.'))
    $operations.Add((New-DungeonFixtureOperation -Sequence ($sequence++) -StageId 'readiness' -Surface 'account-provisioner' -Action 'create-or-reuse-account' -Command 'future fixture adapter: create-or-reuse exact account <ACCOUNT_NAME> using secret from WOW_ACCOUNT_PASSWORD' -Mutation $true -Supported $false -Scope $scope -Reason 'Existing create-account.ps1 is one-account, live, and not a fixture-owned five-account adapter.'))

    foreach ($member in @($Definition.members)) {
        $operations.Add((New-DungeonFixtureOperation -Sequence ($sequence++) -StageId 'readiness' -Surface 'playerbot-session-command' -Action 'addclass' -Command ".playerbots bot addclass $($member.class_name.ToLowerInvariant())" -Mutation $true -Supported $false -Scope "$scope/member:$($member.fixture_role)" -Reason 'addclass requires an active player session and selects a cached character; it cannot bind the deterministic fixture name or prove ownership.'))
        $operations.Add((New-DungeonFixtureOperation -Sequence ($sequence++) -StageId 'readiness' -Surface 'playerbot-session-command' -Action 'factory-init' -Command ".playerbots bot init=$($Definition.gear_quality) $($member.character_name)" -Mutation $true -Supported $false -Scope "$scope/member:$($member.fixture_role)" -Reason 'This is the legitimate factory path, but the current command resolves an existing AddClass bot and chooses its spec randomly.'))
        $operations.Add((New-DungeonFixtureOperation -Sequence ($sequence++) -StageId 'readiness' -Surface 'playerbot-factory-internal' -Action 'exact-spec' -Command "PlayerbotFactory.InitTalentsBySpecNo(<GUID>, $($member.spec_index), true)" -Mutation $true -Supported $false -Scope "$scope/member:$($member.fixture_role)" -Reason 'The exact spec-index helper exists internally but has no current safe console/bridge entry point.'))
    }

    $operations.Add((New-DungeonFixtureOperation -Sequence ($sequence++) -StageId 'readiness' -Surface 'bridge' -Action 'party' -Command 'party <tank_guid> <healer_guid> <mage_guid> <rogue_guid> <hunter_guid>' -Mutation $true -Supported $true -Scope $scope -Reason 'Current bridge can create a five-member same-faction group once exact online GUIDs are resolved; this does not provision or verify the members.'))
    $operations.Add((New-DungeonFixtureOperation -Sequence ($sequence++) -StageId 'readiness' -Surface 'bridge' -Action 'snapshot' -Command 'snapshot <tank_guid> and each member GUID' -Mutation $false -Supported $true -Scope $scope -Reason 'Read-only bridge snapshot; insufficient alone for the fixture acceptance contract.'))
    $operations.Add((New-DungeonFixtureOperation -Sequence ($sequence++) -StageId 'readiness' -Surface 'bridge' -Action 'natural-rally' -Command 'natural movement to the dungeon entrance; do not use bridge rally' -Mutation $true -Supported $false -Scope $scope -Reason 'Current rally teleports members and would invalidate natural dungeon-readiness proof.'))
    $operations.Add((New-DungeonFixtureOperation -Sequence ($sequence++) -StageId 'elite-pack' -Surface 'dungeon-adapter' -Action 'enter' -Command 'enter Utgarde Keep normal (map 574, difficulty 0)' -Mutation $true -Supported $false -Scope $scope -Reason 'No current deterministic entry/instance adapter is exposed.'))
    $operations.Add((New-DungeonFixtureOperation -Sequence ($sequence++) -StageId 'elite-pack' -Surface 'dungeon-adapter' -Action 'first-elite-pack' -Command 'move naturally and engage the first selected elite pack only' -Mutation $true -Supported $false -Scope $scope -Reason 'Broad bridge engage attacks anything and cannot name or stage-lock the first pack.'))
    $operations.Add((New-DungeonFixtureOperation -Sequence ($sequence++) -StageId 'elite-pack' -Surface 'acceptance-evaluator' -Action 'gate' -Command 'evaluate elite-pack receipt; continue only on PASS' -Mutation $false -Supported $true -Scope $scope -Reason 'Offline gate evaluator is implemented locally.'))
    $operations.Add((New-DungeonFixtureOperation -Sequence ($sequence++) -StageId 'ingvar' -Surface 'dungeon-adapter' -Action 'boss' -Command 'start Ingvar only after elite-pack gate PASS' -Mutation $true -Supported $false -Scope $scope -Reason 'No current boss-stage command or stage token exists.'))
    $operations.Add((New-DungeonFixtureOperation -Sequence ($sequence++) -StageId 'ingvar' -Surface 'acceptance-evaluator' -Action 'gate' -Command 'evaluate Ingvar receipt, deaths, and five-member boss credit' -Mutation $false -Supported $true -Scope $scope -Reason 'Offline gate evaluator is implemented locally.'))

    $gaps = @(Get-DungeonFixtureGapReport -Definition $Definition)
    return [pscustomobject][ordered]@{
        schema = $script:DungeonFixturePlanSchema
        schema_version = 1
        plan_kind = 'provision-and-orchestrate'
        execute_required_switch = '-Execute'
        dry_run_default = $true
        local_only = $true
        direct_database_mutations = $false
        existing_scripts_invoked = @()
        operations = @($operations)
        read_only_db_queries = @(Get-DungeonFixtureReadOnlyDbQueries -Definition $Definition)
        gap_report = $gaps
        execute_allowed_now = (@($gaps | Where-Object severity -eq 'blocker').Count -eq 0)
    }
}

function Get-DungeonFixtureTeardownPlan {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][psobject]$Definition)

    Assert-DungeonFixtureDefinition -Definition $Definition | Out-Null
    $scope = "fixture:$($Definition.fixture_id)"
    $operations = [System.Collections.Generic.List[object]]::new()
    $sequence = 1
    $operations.Add((New-DungeonFixtureOperation -Sequence ($sequence++) -StageId 'teardown' -Surface 'bridge' -Action 'pause' -Command 'pause <each exact fixture GUID>' -Mutation $true -Supported $true -Scope $scope -Reason 'Pause before teardown and preserve the receipt.'))
    $operations.Add((New-DungeonFixtureOperation -Sequence ($sequence++) -StageId 'teardown' -Surface 'bridge' -Action 'deactivate' -Command 'deactivate <each exact fixture GUID>' -Mutation $true -Supported $true -Scope $scope -Reason 'Deactivate only exact manifest members after ownership proof.'))
    $operations.Add((New-DungeonFixtureOperation -Sequence ($sequence++) -StageId 'teardown' -Surface 'group-adapter' -Action 'disband' -Command 'future fixture adapter: disband the exact fixture group' -Mutation $true -Supported $false -Scope $scope -Reason 'Current bridge has no exact disband operation.'))
    $operations.Add((New-DungeonFixtureOperation -Sequence ($sequence++) -StageId 'teardown' -Surface 'owner-aware-adapter' -Action 'delete-owned-objects' -Command 'future owner-aware adapter: delete only manifest-owned accounts/characters and dependent rows' -Mutation $true -Supported $false -Scope $scope -Reason 'Direct SQL deletion is prohibited because item, talent, skill, spell, group, and playerbot rows require coordinated cleanup.'))
    $operations.Add((New-DungeonFixtureOperation -Sequence ($sequence++) -StageId 'teardown' -Surface 'evidence' -Action 'retain-receipts' -Command 'retain JSONL receipt and gate evidence' -Mutation $false -Supported $true -Scope $scope -Reason 'Receipts are the audit trail and are not deleted by teardown.'))

    return [pscustomobject][ordered]@{
        schema = $script:DungeonFixturePlanSchema
        schema_version = 1
        plan_kind = 'teardown'
        execute_required_switch = '-Execute'
        dry_run_default = $true
        local_only = $true
        preserve_receipts = $true
        direct_database_mutations = $false
        operations = @($operations)
        read_only_db_queries = @()
        execute_allowed_now = $false
        gap_report = @(Get-DungeonFixtureGapReport -Definition $Definition | Where-Object id -in @('DNG-EXEC-005', 'DNG-EXEC-001'))
    }
}

function Get-DungeonFixtureDefaultReceiptPath {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$FixtureId,
        [string]$OutputDirectory = (Join-Path (Split-Path -Parent $PSScriptRoot) 'logs')
    )

    Assert-DungeonFixtureId -FixtureId $FixtureId
    New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
    $stamp = (Get-Date).ToUniversalTime().ToString('yyyyMMdd-HHmmss')
    return (Join-Path $OutputDirectory ("dungeon-fixture-{0}-{1}.jsonl" -f $FixtureId.ToLowerInvariant(), $stamp))
}

function Write-DungeonFixtureReceipt {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$FixtureId,
        [Parameter(Mandatory = $true)][string]$Event,
        [AllowNull()][psobject]$Data = $null
    )

    $parent = Split-Path -Parent $Path
    if ($parent) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    $record = [ordered]@{
        schema = $script:DungeonFixtureReceiptSchema
        schema_version = 1
        utc = (Get-Date).ToUniversalTime().ToString('o')
        fixture_id = $FixtureId
        event = $Event
    }
    if ($null -ne $Data) {
        foreach ($property in $Data.PSObject.Properties) {
            if ($property.Name -notin @('password', 'secret', 'secret_value')) {
                $record[$property.Name] = $property.Value
            }
        }
    }
    $json = $record | ConvertTo-Json -Depth 40 -Compress
    $utf8NoBom = [System.Text.UTF8Encoding]::new($false)
    [System.IO.File]::AppendAllText($Path, $json + [Environment]::NewLine, $utf8NoBom)
    return $record
}

function Get-DungeonFixtureStage {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][psobject]$Evidence,
        [Parameter(Mandatory = $true)][string]$StageId
    )

    foreach ($stage in Get-DungeonFixtureArray -Value (Get-DungeonFixtureProperty -Object $Evidence -Name 'stages')) {
        if ((Get-DungeonFixtureProperty -Object $stage -Name 'stage_id' -Default '') -eq $StageId) {
            return $stage
        }
    }

    return $null
}

function Add-DungeonFixtureGateResult {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][System.Collections.Generic.List[object]]$Results,
        [Parameter(Mandatory = $true)][string]$GateId,
        [Parameter(Mandatory = $true)][string]$StageId,
        [Parameter(Mandatory = $true)][bool]$Passed,
        [Parameter(Mandatory = $true)][string]$Reason,
        [AllowNull()][object]$Observed = $null,
        [AllowNull()][object]$Required = $null,
        [ValidateSet('PASS', 'FAIL', 'BLOCKED')][string]$Status = $(if ($Passed) { 'PASS' } else { 'FAIL' })
    )

    $Results.Add([pscustomobject][ordered]@{
            gate_id = $GateId
            stage_id = $StageId
            passed = $Passed
            status = $Status
            reason = $Reason
            observed = $Observed
            required = $Required
        })
}

function Get-DungeonFixtureNumber {
    [CmdletBinding()]
    param(
        [AllowNull()][object]$Value,
        [double]$Default = 0
    )

    if ($null -eq $Value -or [string]::IsNullOrWhiteSpace([string]$Value)) {
        return $Default
    }

    try {
        return [double]$Value
    }
    catch {
        return $Default
    }
}

function Test-DungeonFixtureFactoryEvidence {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][psobject]$Member,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][System.Collections.Generic.List[object]]$Results
    )

    $template = Get-DungeonFixtureProperty -Object $Member -Name 'template_generation'
    $provider = [string](Get-DungeonFixtureProperty -Object $template -Name 'provider' -Default '')
    $factory = (Get-DungeonFixtureProperty -Object $template -Name 'factory_randomize' -Default $false) -eq $true
    $skills = (Get-DungeonFixtureProperty -Object $template -Name 'skills' -Default $false) -eq $true
    $talents = (Get-DungeonFixtureProperty -Object $template -Name 'talents' -Default $false) -eq $true
    $gear = (Get-DungeonFixtureProperty -Object $template -Name 'gear' -Default $false) -eq $true
    $providerPass = $provider -eq 'PlayerbotFactory.Randomize(false)'
    $passed = $providerPass -and $factory -and $skills -and $talents -and $gear
    $reason = if ($passed) { 'Factory provider, Randomize(false), skills, talents, and gear provenance were all observed.' } else { 'Factory provenance must explicitly identify PlayerbotFactory.Randomize(false) and cover skills, talents, and gear.' }
    Add-DungeonFixtureGateResult -Results $Results -GateId "readiness.template.$($Member.fixture_role)" -StageId 'readiness' -Passed $passed -Reason $reason -Observed ([pscustomobject]@{ provider = $provider; factory_randomize = $factory; skills = $skills; talents = $talents; gear = $gear }) -Required ([pscustomobject]@{ provider = 'PlayerbotFactory.Randomize(false)'; factory_randomize = $true; skills = $true; talents = $true; gear = $true })
}

function Test-DungeonFixtureGearEvidence {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][psobject]$Member,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][System.Collections.Generic.List[object]]$Results
    )

    $gear = Get-DungeonFixtureProperty -Object $Member -Name 'gear'
    $missing = @(Get-DungeonFixtureArray -Value (Get-DungeonFixtureProperty -Object $gear -Name 'missing_slots'))
    $invalid = @(Get-DungeonFixtureArray -Value (Get-DungeonFixtureProperty -Object $gear -Name 'invalid_slots'))
    $ratio = Get-DungeonFixtureNumber -Value (Get-DungeonFixtureProperty -Object $gear -Name 'completeness_ratio' -Default 0)
    $generated = (Get-DungeonFixtureProperty -Object $gear -Name 'generated_by_factory' -Default $false) -eq $true
    $slots = @(Get-DungeonFixtureArray -Value (Get-DungeonFixtureProperty -Object $gear -Name 'slots'))
    $slotProblems = @()
    foreach ($slot in $slots) {
        $verified = (Get-DungeonFixtureProperty -Object $slot -Name 'verified' -Default $false) -eq $true
        $occupied = (Get-DungeonFixtureProperty -Object $slot -Name 'occupied' -Default $false) -eq $true
        $legalEmpty = (Get-DungeonFixtureProperty -Object $slot -Name 'legitimate_empty' -Default $false) -eq $true
        if (-not $verified -or (-not $occupied -and -not $legalEmpty)) {
            $slotProblems += [string](Get-DungeonFixtureProperty -Object $slot -Name 'slot' -Default 'unknown')
        }
    }
    $passed = $generated -and $missing.Count -eq 0 -and $invalid.Count -eq 0 -and $slotProblems.Count -eq 0 -and $ratio -ge 1.0
    $reason = if ($passed) { 'Every reported equipment slot is complete, legal, and factory-generated.' } else { 'Gear must have completeness_ratio >= 1, no missing/invalid slots, verified occupied or legal-empty slots, and factory provenance.' }
    Add-DungeonFixtureGateResult -Results $Results -GateId "readiness.gear.$($Member.fixture_role)" -StageId 'readiness' -Passed $passed -Reason $reason -Observed ([pscustomobject]@{ completeness_ratio = $ratio; generated_by_factory = $generated; missing_slots = $missing; invalid_slots = $invalid; slot_problems = $slotProblems }) -Required ([pscustomobject]@{ completeness_ratio = 1.0; missing_slots = 0; invalid_slots = 0; slot_problems = 0 })
}

function Test-DungeonFixtureStageEvidence {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][psobject]$Stage,
        [Parameter(Mandatory = $true)][psobject]$Definition,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][System.Collections.Generic.List[object]]$Results
    )

    $stageId = [string](Get-DungeonFixtureProperty -Object $Stage -Name 'stage_id' -Default 'unknown')
    $completed = (Get-DungeonFixtureProperty -Object $Stage -Name 'completed' -Default $false) -eq $true
    $deaths = [int](Get-DungeonFixtureNumber -Value (Get-DungeonFixtureProperty -Object $Stage -Name 'deaths' -Default 0))
    $tank = Get-DungeonFixtureProperty -Object $Stage -Name 'tank'
    $healer = Get-DungeonFixtureProperty -Object $Stage -Name 'healer'
    $victimRatio = Get-DungeonFixtureNumber -Value (Get-DungeonFixtureProperty -Object $tank -Name 'victim_ownership_ratio' -Default 0)
    $tauntCasts = [int](Get-DungeonFixtureNumber -Value (Get-DungeonFixtureProperty -Object $tank -Name 'taunt_casts' -Default 0))
    $tauntConfirmed = [int](Get-DungeonFixtureNumber -Value (Get-DungeonFixtureProperty -Object $tank -Name 'taunt_confirmed' -Default 0))
    $tankPriority = Get-DungeonFixtureNumber -Value (Get-DungeonFixtureProperty -Object $healer -Name 'tank_priority_ratio' -Default 0)
    $effectiveTankHeals = Get-DungeonFixtureNumber -Value (Get-DungeonFixtureProperty -Object $healer -Name 'effective_healing_tank' -Default 0)
    $effectiveTotalHeals = Get-DungeonFixtureNumber -Value (Get-DungeonFixtureProperty -Object $healer -Name 'effective_healing_total' -Default 0)
    $tankPass = $victimRatio -ge 0.90 -and $tauntCasts -ge 1 -and $tauntConfirmed -ge $tauntCasts
    $healerPass = $tankPriority -ge 0.60 -and $effectiveTankHeals -gt 0 -and $effectiveTotalHeals -ge $effectiveTankHeals

    Add-DungeonFixtureGateResult -Results $Results -GateId "$stageId.completed" -StageId $stageId -Passed $completed -Reason $(if ($completed) { 'Stage completion was observed.' } else { 'Stage completion was not observed.' }) -Observed $completed -Required $true
    Add-DungeonFixtureGateResult -Results $Results -GateId "$stageId.deaths" -StageId $stageId -Passed ($deaths -eq 0) -Reason $(if ($deaths -eq 0) { 'No party deaths were observed in the stage.' } else { "Stage reported $deaths death(s)." }) -Observed $deaths -Required 0
    Add-DungeonFixtureGateResult -Results $Results -GateId "$stageId.tank-victim-and-taunt" -StageId $stageId -Passed $tankPass -Reason $(if ($tankPass) { 'Tank owned the victim window and every observed taunt was confirmed.' } else { 'Tank victim ownership must be at least 90%, with at least one confirmed taunt per stage.' }) -Observed ([pscustomobject]@{ victim_ownership_ratio = $victimRatio; taunt_casts = $tauntCasts; taunt_confirmed = $tauntConfirmed }) -Required ([pscustomobject]@{ victim_ownership_ratio = 0.90; taunt_casts = 1; taunt_confirmed = '>= taunt_casts' })
    Add-DungeonFixtureGateResult -Results $Results -GateId "$stageId.healer-tank-priority" -StageId $stageId -Passed $healerPass -Reason $(if ($healerPass) { 'Healer prioritized the tank and produced effective tank healing.' } else { 'Healer tank priority must be at least 60% with positive effective tank healing.' }) -Observed ([pscustomobject]@{ tank_priority_ratio = $tankPriority; effective_healing_tank = $effectiveTankHeals; effective_healing_total = $effectiveTotalHeals }) -Required ([pscustomobject]@{ tank_priority_ratio = 0.60; effective_healing_tank = '> 0'; effective_healing_total = '>= effective_healing_tank' })

    if ($stageId -eq 'ingvar') {
        $boss = Get-DungeonFixtureProperty -Object $Stage -Name 'boss_completion'
        $bossCompleted = (Get-DungeonFixtureProperty -Object $boss -Name 'completed' -Default $false) -eq $true
        $creditedRoles = Get-DungeonFixtureArray -Value (Get-DungeonFixtureProperty -Object $boss -Name 'credited_fixture_roles')
        $expectedRoles = @($Definition.members | ForEach-Object fixture_role | Sort-Object)
        $actualRoles = @($creditedRoles | ForEach-Object { [string]$_ } | Sort-Object)
        $bossName = [string](Get-DungeonFixtureProperty -Object $boss -Name 'boss_name' -Default '')
        $creditPass = $bossCompleted -and $bossName -eq 'Ingvar the Plunderer' -and $actualRoles.Count -eq 5 -and (($actualRoles -join ',') -eq ($expectedRoles -join ','))
        Add-DungeonFixtureGateResult -Results $Results -GateId 'ingvar.boss-completion' -StageId 'ingvar' -Passed $creditPass -Reason $(if ($creditPass) { 'Ingvar completion and all five fixture credits were observed.' } else { 'Ingvar must be completed and credited to all five expected fixture roles.' }) -Observed ([pscustomobject]@{ boss_name = $bossName; completed = $bossCompleted; credited_fixture_roles = $creditedRoles }) -Required ([pscustomobject]@{ boss_name = 'Ingvar the Plunderer'; completed = $true; credited_fixture_roles = $expectedRoles })
    }
}

function Get-DungeonFixtureGateResults {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][psobject]$Evidence,
        [Parameter(Mandatory = $true)][psobject]$Definition
    )

    Assert-DungeonFixtureDefinition -Definition $Definition | Out-Null
    $results = [System.Collections.Generic.List[object]]::new()
    $evidenceSchema = [string](Get-DungeonFixtureProperty -Object $Evidence -Name 'schema' -Default '')
    Add-DungeonFixtureGateResult -Results $results -GateId 'readiness.evidence-schema' -StageId 'readiness' -Passed ($evidenceSchema -eq $script:DungeonFixtureEvidenceSchema) -Reason $(if ($evidenceSchema -eq $script:DungeonFixtureEvidenceSchema) { 'Versioned dungeon fixture evidence was supplied.' } else { "Expected evidence schema $script:DungeonFixtureEvidenceSchema." }) -Observed $evidenceSchema -Required $script:DungeonFixtureEvidenceSchema
    $evidenceFixtureId = [string](Get-DungeonFixtureProperty -Object $Evidence -Name 'fixture_id' -Default '')
    Add-DungeonFixtureGateResult -Results $results -GateId 'readiness.fixture-id' -StageId 'readiness' -Passed ($evidenceFixtureId -eq [string]$Definition.fixture_id) -Reason $(if ($evidenceFixtureId -eq [string]$Definition.fixture_id) { 'Evidence is bound to the exact fixture identity.' } else { 'Evidence fixture_id does not match the requested fixture.' }) -Observed $evidenceFixtureId -Required $Definition.fixture_id

    $members = Get-DungeonFixtureArray -Value (Get-DungeonFixtureProperty -Object $Evidence -Name 'members')
    $rosterPass = $members.Count -eq 5
    Add-DungeonFixtureGateResult -Results $results -GateId 'readiness.roster-count' -StageId 'readiness' -Passed $rosterPass -Reason $(if ($rosterPass) { 'Exactly five evidence members were supplied.' } else { "Expected exactly five evidence members; found $($members.Count)." }) -Observed $members.Count -Required 5

    foreach ($expectedMember in @($Definition.members)) {
        $observedMember = @($members | Where-Object { (Get-DungeonFixtureProperty -Object $_ -Name 'fixture_role' -Default '') -eq $expectedMember.fixture_role })[0]
        $identityPass = $null -ne $observedMember -and
            (Get-DungeonFixtureProperty -Object $observedMember -Name 'character_name' -Default '') -eq $expectedMember.character_name -and
            (Get-DungeonFixtureProperty -Object $observedMember -Name 'class_name' -Default '') -eq $expectedMember.class_name -and
            [int](Get-DungeonFixtureNumber -Value (Get-DungeonFixtureProperty -Object $observedMember -Name 'class_id' -Default -1) -Default -1) -eq [int]$expectedMember.class_id -and
            (Get-DungeonFixtureProperty -Object $observedMember -Name 'faction' -Default '') -eq $expectedMember.faction -and
            (Get-DungeonFixtureProperty -Object $observedMember -Name 'role' -Default '') -eq $expectedMember.role -and
            (Get-DungeonFixtureProperty -Object $observedMember -Name 'spec_label' -Default '') -eq $expectedMember.spec_label -and
            [int](Get-DungeonFixtureNumber -Value (Get-DungeonFixtureProperty -Object $observedMember -Name 'spec_index' -Default -1) -Default -1) -eq [int]$expectedMember.spec_index -and
            [int](Get-DungeonFixtureNumber -Value (Get-DungeonFixtureProperty -Object $observedMember -Name 'level' -Default -1) -Default -1) -eq [int]$Definition.level
        Add-DungeonFixtureGateResult -Results $results -GateId "readiness.identity.$($expectedMember.fixture_role)" -StageId 'readiness' -Passed $identityPass -Reason $(if ($identityPass) { 'Class, role, exact spec, exact name, and level match the fixture definition.' } else { 'Evidence does not match the exact class/role/spec/name/level contract.' }) -Observed $observedMember -Required $expectedMember
        if ($null -ne $observedMember) {
            Test-DungeonFixtureFactoryEvidence -Member $observedMember -Results $results
            Test-DungeonFixtureGearEvidence -Member $observedMember -Results $results
        }
        else {
            Add-DungeonFixtureGateResult -Results $results -GateId "readiness.template.$($expectedMember.fixture_role)" -StageId 'readiness' -Passed $false -Reason 'No evidence member exists for this fixture role.' -Observed $null -Required $true
            Add-DungeonFixtureGateResult -Results $results -GateId "readiness.gear.$($expectedMember.fixture_role)" -StageId 'readiness' -Passed $false -Reason 'No evidence member exists for this fixture role.' -Observed $null -Required $true
        }
    }

    $provenance = Get-DungeonFixtureProperty -Object $Evidence -Name 'provenance'
    $provenanceFields = @('direct_db_mutations', 'combat_result_mutations', 'teleports', 'gm_combat_commands')
    $provenancePresent = $null -ne $provenance -and @($provenanceFields | Where-Object { $null -eq (Get-DungeonFixtureProperty -Object $provenance -Name $_) }).Count -eq 0
    $provenancePass = $provenancePresent -and
        [int](Get-DungeonFixtureNumber -Value (Get-DungeonFixtureProperty -Object $provenance -Name 'direct_db_mutations' -Default 0)) -eq 0 -and
        [int](Get-DungeonFixtureNumber -Value (Get-DungeonFixtureProperty -Object $provenance -Name 'combat_result_mutations' -Default 0)) -eq 0 -and
        [int](Get-DungeonFixtureNumber -Value (Get-DungeonFixtureProperty -Object $provenance -Name 'teleports' -Default 0)) -eq 0 -and
        [int](Get-DungeonFixtureNumber -Value (Get-DungeonFixtureProperty -Object $provenance -Name 'gm_combat_commands' -Default 0)) -eq 0
    Add-DungeonFixtureGateResult -Results $results -GateId 'readiness.provenance' -StageId 'readiness' -Passed $provenancePass -Reason $(if ($provenancePass) { 'No direct DB, combat-result, teleport, or GM-combat mutation was reported.' } else { 'Fixture proof cannot contain direct DB, combat-result, teleport, or GM-combat mutations.' }) -Observed $provenance -Required ([pscustomobject]@{ direct_db_mutations = 0; combat_result_mutations = 0; teleports = 0; gm_combat_commands = 0 })

    $readinessPass = @($results | Where-Object { $_.stage_id -eq 'readiness' -and -not $_.passed }).Count -eq 0
    $eliteStage = Get-DungeonFixtureStage -Evidence $Evidence -StageId 'elite-pack'
    if (-not $readinessPass) {
        Add-DungeonFixtureGateResult -Results $results -GateId 'elite-pack.prerequisite-readiness' -StageId 'elite-pack' -Passed $false -Status 'BLOCKED' -Reason 'Readiness gate did not PASS; the first elite pack may not start.' -Observed $readinessPass -Required $true
    }
    elseif ($null -eq $eliteStage) {
        Add-DungeonFixtureGateResult -Results $results -GateId 'elite-pack.evidence-present' -StageId 'elite-pack' -Passed $false -Reason 'No elite-pack evidence was supplied.' -Observed $false -Required $true
    }
    else {
        Test-DungeonFixtureStageEvidence -Stage $eliteStage -Definition $Definition -Results $results
    }

    $elitePass = @($results | Where-Object { $_.stage_id -eq 'elite-pack' -and -not $_.passed }).Count -eq 0
    $ingvarStage = Get-DungeonFixtureStage -Evidence $Evidence -StageId 'ingvar'
    if (-not $elitePass) {
        Add-DungeonFixtureGateResult -Results $results -GateId 'ingvar.prerequisite-elite-pack' -StageId 'ingvar' -Passed $false -Status 'BLOCKED' -Reason 'Elite-pack gate did not PASS; Ingvar must not start.' -Observed $elitePass -Required $true
    }
    elseif ($null -eq $ingvarStage) {
        Add-DungeonFixtureGateResult -Results $results -GateId 'ingvar.evidence-present' -StageId 'ingvar' -Passed $false -Reason 'No Ingvar evidence was supplied.' -Observed $false -Required $true
    }
    else {
        Test-DungeonFixtureStageEvidence -Stage $ingvarStage -Definition $Definition -Results $results
    }

    $allPass = @($results | Where-Object { -not $_.passed }).Count -eq 0
    $nextStage = 'readiness'
    if ($readinessPass) { $nextStage = 'elite-pack' }
    if ($elitePass) { $nextStage = 'ingvar' }
    if ($allPass) { $nextStage = 'completed' }

    return [pscustomobject][ordered]@{
        schema = $script:DungeonFixtureAcceptanceSchema
        schema_version = 1
        fixture_id = $Definition.fixture_id
        status = if ($allPass) { 'PASS' } else { 'FAIL' }
        all_gates_passed = $allPass
        next_stage = $nextStage
        gates = @($results)
        counts = [pscustomobject][ordered]@{
            total = $results.Count
            passed = @($results | Where-Object passed).Count
            failed = @($results | Where-Object { -not $_.passed -and $_.status -eq 'FAIL' }).Count
            blocked = @($results | Where-Object status -eq 'BLOCKED').Count
        }
    }
}

function Test-DungeonFixtureEvidence {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][psobject]$Evidence,
        [Parameter(Mandatory = $true)][psobject]$Definition
    )

    $result = Get-DungeonFixtureGateResults -Evidence $Evidence -Definition $Definition
    return [pscustomobject][ordered]@{
        passed = $result.all_gates_passed
        status = $result.status
        result = $result
    }
}

function Assert-DungeonFixtureExecutionAllowed {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][psobject]$Plan,
        [Parameter(Mandatory = $true)][string]$GapReportPath
    )

    if ($Plan.execute_allowed_now -ne $true) {
        $blockers = @($Plan.gap_report | Where-Object severity -eq 'blocker' | ForEach-Object id)
        throw "DNG-EXEC-BLOCKED: current AutoWoW surfaces cannot safely execute this fixture. Gap report: $GapReportPath. Blockers: $($blockers -join ', ')"
    }
}
