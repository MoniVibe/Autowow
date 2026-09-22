Set-StrictMode -Version Latest

$existingFixtureLibrary = Join-Path $PSScriptRoot 'pvp-fixture-lib.ps1'
if (-not (Test-Path -LiteralPath $existingFixtureLibrary)) {
    throw "Missing existing PvP fixture helper library: $existingFixtureLibrary"
}
. $existingFixtureLibrary

$script:PvpRoleFixtureDefinitionSchema = 'autowow.pvp.role-fixture.definition.v1'
$script:PvpRoleFixturePlanSchema = 'autowow.pvp.role-fixture.plan.v1'
$script:PvpRoleFixtureManifestSchema = 'autowow.pvp.role-fixture.manifest.v1'
$script:PvpRoleFixtureId = 'awrole1'
$script:PvpRoleFixtureLevel = 80
$script:PvpRoleFixtureQuality = 3
$script:PvpRoleFixtureQualityName = 'rare'

function Get-PvpRoleFixtureDefinition {
    [CmdletBinding()]
    param()

    $roleSlots = @(
        [ordered]@{
            slot = 1; role_slot = 'flag-carrier-tank'; role_category = 'tank'; class_name = 'Druid'; class_id = 11
            spec_name = 'bear pve'; spec_index = 1; capabilities = @('durable', 'flag-carrier', 'melee', 'tank')
            alliance_race_name = 'Night Elf'; alliance_race_id = 4; horde_race_name = 'Tauren'; horde_race_id = 6
            alliance_character = 'Roleaflag'; horde_character = 'Rolehflag'
        },
        [ordered]@{
            slot = 2; role_slot = 'healer-discipline'; role_category = 'healer'; class_name = 'Priest'; class_id = 5
            spec_name = 'disc pvp'; spec_index = 3; capabilities = @('caster', 'dispel', 'healer', 'ranged')
            alliance_race_name = 'Human'; alliance_race_id = 1; horde_race_name = 'Undead'; horde_race_id = 5
            alliance_character = 'Roleapriest'; horde_character = 'Rolehpriest'
        },
        [ordered]@{
            slot = 3; role_slot = 'healer-restoration'; role_category = 'healer'; class_name = 'Shaman'; class_id = 7
            spec_name = 'resto pvp'; spec_index = 5; capabilities = @('caster', 'healer', 'interrupt', 'ranged')
            alliance_race_name = 'Draenei'; alliance_race_id = 11; horde_race_name = 'Tauren'; horde_race_id = 6
            alliance_character = 'Rolearesto'; horde_character = 'Rolehresto'
        },
        [ordered]@{
            slot = 4; role_slot = 'dps-rogue'; role_category = 'dps'; class_name = 'Rogue'; class_id = 4
            spec_name = 'subtlety pvp'; spec_index = 5; capabilities = @('dps', 'interrupt', 'melee', 'stealth')
            alliance_race_name = 'Human'; alliance_race_id = 1; horde_race_name = 'Undead'; horde_race_id = 5
            alliance_character = 'Rolearogue'; horde_character = 'Rolehrogue'
        },
        [ordered]@{
            slot = 5; role_slot = 'dps-warrior'; role_category = 'dps'; class_name = 'Warrior'; class_id = 1
            spec_name = 'arms pvp'; spec_index = 3; capabilities = @('dps', 'interrupt', 'melee', 'mortal-strike')
            alliance_race_name = 'Human'; alliance_race_id = 1; horde_race_name = 'Orc'; horde_race_id = 2
            alliance_character = 'Roleawarrior'; horde_character = 'Rolehwarrior'
        },
        [ordered]@{
            slot = 6; role_slot = 'dps-hunter'; role_category = 'dps'; class_name = 'Hunter'; class_id = 3
            spec_name = 'mm pvp'; spec_index = 4; capabilities = @('dps', 'physical-ranged', 'ranged', 'trap-control')
            alliance_race_name = 'Dwarf'; alliance_race_id = 3; horde_race_name = 'Troll'; horde_race_id = 8
            alliance_character = 'Roleahunter'; horde_character = 'Rolehhunter'
        },
        [ordered]@{
            slot = 7; role_slot = 'dps-mage'; role_category = 'dps'; class_name = 'Mage'; class_id = 8
            spec_name = 'frost pvp'; spec_index = 6; capabilities = @('caster', 'control', 'dps', 'interrupt', 'ranged')
            alliance_race_name = 'Gnome'; alliance_race_id = 7; horde_race_name = 'Blood Elf'; horde_race_id = 10
            alliance_character = 'Roleamage'; horde_character = 'Rolehmage'
        },
        [ordered]@{
            slot = 8; role_slot = 'dps-warlock'; role_category = 'dps'; class_name = 'Warlock'; class_id = 9
            spec_name = 'affli pvp'; spec_index = 3; capabilities = @('caster', 'dps', 'pressure', 'ranged')
            alliance_race_name = 'Gnome'; alliance_race_id = 7; horde_race_name = 'Orc'; horde_race_id = 2
            alliance_character = 'Roleawarlock'; horde_character = 'Rolehwarlock'
        },
        [ordered]@{
            slot = 9; role_slot = 'dps-elemental'; role_category = 'dps'; class_name = 'Shaman'; class_id = 7
            spec_name = 'ele pvp'; spec_index = 3; capabilities = @('caster', 'dps', 'interrupt', 'ranged')
            alliance_race_name = 'Draenei'; alliance_race_id = 11; horde_race_name = 'Troll'; horde_race_id = 8
            alliance_character = 'Roleaele'; horde_character = 'Rolehele'
        },
        [ordered]@{
            slot = 10; role_slot = 'dps-paladin'; role_category = 'dps'; class_name = 'Paladin'; class_id = 2
            spec_name = 'ret pvp'; spec_index = 5; capabilities = @('dps', 'melee', 'support', 'stun-control')
            alliance_race_name = 'Human'; alliance_race_id = 1; horde_race_name = 'Blood Elf'; horde_race_id = 10
            alliance_character = 'Roleapaladin'; horde_character = 'Rolehpaladin'
        }
    )

    $members = New-Object System.Collections.Generic.List[object]
    foreach ($side in @(
        [pscustomobject]@{
            key = 'A'; faction = 'Alliance'; character_property = 'alliance_character'; race_id_property = 'alliance_race_id'
            race_name_property = 'alliance_race_name'; map = 0; zone = 12; x = -8949.95; y = -132.493; z = 83.5312; o = 0.0
        },
        [pscustomobject]@{
            key = 'H'; faction = 'Horde'; character_property = 'horde_character'; race_id_property = 'horde_race_id'
            race_name_property = 'horde_race_name'; map = 1; zone = 14; x = -618.518; y = -4251.67; z = 38.718; o = 0.0
        }
    )) {
        foreach ($roleSlot in $roleSlots) {
            $members.Add([pscustomobject][ordered]@{
                ordinal = if ($side.key -eq 'A') { [int]$roleSlot.slot } else { 10 + [int]$roleSlot.slot }
                side_slot = [int]$roleSlot.slot
                role_slot = $roleSlot.role_slot
                role_category = $roleSlot.role_category
                faction = $side.faction
                account_name = ('AWROLE1{0}{1:D2}' -f $side.key, [int]$roleSlot.slot)
                character_name = [string]$roleSlot[$side.character_property]
                race_name = [string]$roleSlot[$side.race_name_property]
                race_id = [int]$roleSlot[$side.race_id_property]
                class_name = $roleSlot.class_name
                class_id = [int]$roleSlot.class_id
                spec_name = $roleSlot.spec_name
                spec_index = [int]$roleSlot.spec_index
                capabilities = @($roleSlot.capabilities)
                gender = 0
                level = $script:PvpRoleFixtureLevel
                gear_quality = $script:PvpRoleFixtureQuality
                gear_quality_name = $script:PvpRoleFixtureQualityName
                position = [pscustomobject][ordered]@{
                    map = [int]$side.map; x = [double]$side.x; y = [double]$side.y; z = [double]$side.z; o = [double]$side.o
                }
                homebind = [pscustomobject][ordered]@{
                    map = [int]$side.map; zone = [int]$side.zone; x = [double]$side.x; y = [double]$side.y; z = [double]$side.z
                }
            })
        }
    }

    $definition = [pscustomobject][ordered]@{
        schema = $script:PvpRoleFixtureDefinitionSchema
        schema_version = 1
        fixture_id = $script:PvpRoleFixtureId
        tag = $script:PvpRoleFixtureId.ToUpperInvariant()
        local_only = $true
        disposable = $true
        battleground = 'Warsong Gulch'
        bracket = '10v10'
        level = $script:PvpRoleFixtureLevel
        gear_quality = $script:PvpRoleFixtureQuality
        gear_quality_name = $script:PvpRoleFixtureQualityName
        account_password_env = 'WOW_ACCOUNT_PASSWORD'
        ordered_roster = @($members | Sort-Object ordinal)
        idempotence = [pscustomobject][ordered]@{
            identity = 'fixture_id plus exact account_name plus exact character_name'
            read_before_write = $true
            exact_existing_match_is_reused = $true
            mismatch_policy = 'fail closed without rewriting, taking ownership, or deleting'
            direct_character_bootstrap = 'named-lock protected insert-if-absent for an empty exact fixture account only'
            post_login_normalization = 'current fixture-init bridge action with per-member exact level, spec index, and quality'
        }
    }

    Assert-PvpRoleFixtureDefinition -Definition $definition | Out-Null
    return $definition
}

function Assert-PvpRoleFixtureDefinition {
    [CmdletBinding()]
    param([Parameter(Mandatory)][psobject]$Definition)

    if ($Definition.schema -ne $script:PvpRoleFixtureDefinitionSchema) { throw 'Unexpected PvP role fixture schema.' }
    if ($Definition.fixture_id -ne $script:PvpRoleFixtureId -or $Definition.local_only -ne $true -or $Definition.disposable -ne $true) {
        throw 'The PvP role fixture must use the fixed local-only disposable identity.'
    }
    if ([int]$Definition.level -ne 80 -or [int]$Definition.gear_quality -ne 3 -or $Definition.gear_quality_name -ne 'rare') {
        throw 'The PvP role fixture must use exact level 80 rare loadouts.'
    }

    $members = @($Definition.ordered_roster)
    if ($members.Count -ne 20) { throw "The PvP role fixture must contain exactly 20 members; found $($members.Count)." }
    if (@($members | Where-Object faction -eq 'Alliance').Count -ne 10 -or @($members | Where-Object faction -eq 'Horde').Count -ne 10) {
        throw 'The PvP role fixture must contain exactly ten members per faction.'
    }
    if ((@($members | Sort-Object ordinal | ForEach-Object ordinal) -join ',') -ne ((1..20) -join ',')) {
        throw 'The PvP role fixture must have a deterministic Alliance-then-Horde ordinal order.'
    }

    $validRaces = @{
        Alliance = @(1, 3, 4, 7, 11)
        Horde = @(2, 5, 6, 8, 10)
    }
    $validClassesByRace = @{
        1 = @(1, 2, 4, 5, 8, 9); 2 = @(1, 3, 4, 7, 9); 3 = @(1, 2, 3, 4, 5)
        4 = @(1, 3, 4, 5, 11); 5 = @(1, 4, 5, 8, 9); 6 = @(1, 3, 7, 11)
        7 = @(1, 4, 8, 9); 8 = @(1, 3, 4, 5, 7, 8); 10 = @(2, 3, 4, 5, 8, 9)
        11 = @(1, 2, 3, 5, 7, 8)
    }
    $seenAccounts = @{}
    $seenCharacters = @{}
    foreach ($member in $members) {
        if ($member.account_name -notmatch '^AWROLE1[AH](0[1-9]|10)$') { throw "Unsafe or unscoped account name: $($member.account_name)" }
        if ($member.character_name -notmatch '^[A-Z][a-z]{2,11}$') { throw "Unsafe 3.3.5 character name: $($member.character_name)" }
        if ($seenAccounts.ContainsKey($member.account_name) -or $seenCharacters.ContainsKey($member.character_name)) {
            throw "Duplicate PvP role fixture identity near $($member.character_name)."
        }
        $seenAccounts[$member.account_name] = $true
        $seenCharacters[$member.character_name] = $true
        if ([int]$member.race_id -notin $validRaces[$member.faction]) {
            throw "Faction-invalid race $($member.race_name) for $($member.character_name)."
        }
        if ([int]$member.class_id -notin $validClassesByRace[[int]$member.race_id]) {
            throw "Wrath-invalid race/class pair $($member.race_name)/$($member.class_name) for $($member.character_name)."
        }
        if ([int]$member.level -ne 80 -or [int]$member.gear_quality -ne 3 -or $member.gear_quality_name -ne 'rare') {
            throw "Loadout target mismatch for $($member.character_name)."
        }
    }

    foreach ($roleSlot in @($members.role_slot | Sort-Object -Unique)) {
        $pair = @($members | Where-Object role_slot -eq $roleSlot | Sort-Object faction)
        if ($pair.Count -ne 2 -or ($pair.faction -join ',') -ne 'Alliance,Horde') { throw "Role slot $roleSlot is not mirrored once per faction." }
        foreach ($propertyName in @('side_slot', 'role_category', 'class_name', 'class_id', 'spec_name', 'spec_index')) {
            if ([string]$pair[0].$propertyName -ne [string]$pair[1].$propertyName) {
                throw "Role slot $roleSlot is not mirrored for $propertyName."
            }
        }
        if ((@($pair[0].capabilities | Sort-Object) -join ',') -ne (@($pair[1].capabilities | Sort-Object) -join ',')) {
            throw "Role slot $roleSlot is not mirrored for capability coverage."
        }
    }

    foreach ($faction in @('Alliance', 'Horde')) {
        $side = @($members | Where-Object faction -eq $faction)
        if (@($side | Where-Object role_category -eq 'tank').Count -ne 1 -or
            @($side | Where-Object role_category -eq 'healer').Count -ne 2 -or
            @($side | Where-Object role_category -eq 'dps').Count -ne 7) {
            throw "$faction does not have the exact 1 tank, 2 healer, 7 DPS role split."
        }
        $dpsCapabilities = @($side | Where-Object role_category -eq 'dps' | ForEach-Object capabilities | Sort-Object -Unique)
        foreach ($requiredCoverage in @('melee', 'physical-ranged', 'caster', 'interrupt')) {
            if ($requiredCoverage -notin $dpsCapabilities) { throw "$faction DPS lacks $requiredCoverage coverage." }
        }
    }

    return $true
}

function Get-PvpRoleFixturePlan {
    [CmdletBinding()]
    param([Parameter(Mandatory)][psobject]$Definition)

    Assert-PvpRoleFixtureDefinition -Definition $Definition | Out-Null
    return [pscustomobject][ordered]@{
        schema = $script:PvpRoleFixturePlanSchema
        schema_version = 1
        fixture_id = $Definition.fixture_id
        dry_run = $true
        apply = $false
        local_only = $true
        roster_count = 20
        alliance_count = 10
        horde_count = 10
        role_split_per_side = [pscustomobject][ordered]@{ flag_carrier_tank = 1; healers = 2; dps = 7 }
        account_password = [pscustomobject][ordered]@{
            source = 'WOW_ACCOUNT_PASSWORD environment variable only'
            accepted_length = '3-16 characters'
            emitted_or_persisted = $false
        }
        account_console_commands = @($Definition.ordered_roster | ForEach-Object {
            [pscustomobject][ordered]@{ account_name = $_.account_name; command = ".account create $($_.account_name) <WOW_ACCOUNT_PASSWORD>" }
        })
        manifest_resolution = [pscustomobject][ordered]@{
            idempotent = $true
            read_before_write = $true
            account_scope = @($Definition.ordered_roster.account_name)
            character_scope = @($Definition.ordered_roster.character_name)
            collision_policy = 'fail closed on ownership, name, race, class, or extra-character mismatch'
        }
        character_bootstrap = @($Definition.ordered_roster | ForEach-Object {
            [pscustomobject][ordered]@{
                account_name = $_.account_name
                character_name = $_.character_name
                race = "$($_.race_name) ($($_.race_id))"
                class = "$($_.class_name) ($($_.class_id))"
                mutation = 'named-lock protected exact insert-if-absent only after an empty exact account is proven'
            }
        })
        post_login_fixture_init = @($Definition.ordered_roster | ForEach-Object {
            [pscustomobject][ordered]@{
                character_name = $_.character_name
                level = [int]$_.level
                spec_index = [int]$_.spec_index
                spec_name = $_.spec_name
                quality = [int]$_.gear_quality
                quality_name = $_.gear_quality_name
            }
        })
        wsg_order = @($Definition.ordered_roster.character_name)
        preconditions = @(
            'local MySQL and bridge endpoints only',
            'normal worldserver stopped before any account or direct character bootstrap',
            'all resolved GUIDs already present in AutoWow.FixtureGuids before fixture-init',
            'normal worldserver online only for activation and post-login fixture-init'
        )
        forbidden_actions = @(
            'server restart', 'server build', 'configuration write', 'battleground_template mutation',
            'league/gathering table mutation', 'unrelated account or character mutation', 'WSG queue dispatch'
        )
    }
}

function Resolve-PvpRoleFixtureManifest {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][psobject]$Definition,
        [AllowEmptyCollection()][object[]]$AccountRows = @(),
        [AllowEmptyCollection()][object[]]$CharacterRows = @()
    )

    Assert-PvpRoleFixtureDefinition -Definition $Definition | Out-Null
    $expectedByAccount = @{}
    $expectedByCharacter = @{}
    foreach ($member in @($Definition.ordered_roster)) {
        $expectedByAccount[$member.account_name] = $member
        $expectedByCharacter[$member.character_name] = $member
    }

    $accountByName = @{}
    $accountNameById = @{}
    foreach ($row in @($AccountRows)) {
        $username = [string]$row.username
        if (-not $expectedByAccount.ContainsKey($username)) { throw "Manifest query returned out-of-scope account $username." }
        if ($row.id -notmatch '^\d+$' -or [int64]$row.id -le 0) { throw "Invalid account id for $username." }
        if ($accountByName.ContainsKey($username) -or $accountNameById.ContainsKey([string]$row.id)) { throw "Duplicate account identity returned for $username." }
        $accountByName[$username] = $row
        $accountNameById[[string]$row.id] = $username
    }

    $charactersByAccount = @{}
    $characterByName = @{}
    $seenGuids = @{}
    foreach ($row in @($CharacterRows)) {
        $accountId = [string]$row.account
        $name = [string]$row.name
        if ($row.guid -notmatch '^\d+$' -or [int64]$row.guid -le 0) { throw "Invalid character guid returned for $name." }
        if ($seenGuids.ContainsKey([string]$row.guid)) { throw "Duplicate character guid returned: $($row.guid)." }
        $seenGuids[[string]$row.guid] = $true
        if (-not $charactersByAccount.ContainsKey($accountId)) { $charactersByAccount[$accountId] = @() }
        $charactersByAccount[$accountId] += $row

        if ($expectedByCharacter.ContainsKey($name)) {
            if ($characterByName.ContainsKey($name)) { throw "Duplicate fixture character name returned: $name." }
            $characterByName[$name] = $row
            $expected = $expectedByCharacter[$name]
            if (-not $accountByName.ContainsKey($expected.account_name)) {
                throw "Fixture character collision: $name exists while account $($expected.account_name) is absent."
            }
            if ([int64]$row.account -ne [int64]$accountByName[$expected.account_name].id) {
                throw "Fixture character collision: $name belongs to another account."
            }
            if ([int]$row.race -ne [int]$expected.race_id -or [int]$row.class -ne [int]$expected.class_id) {
                throw "Fixture character $name has an unexpected race/class and will not be rewritten."
            }
        }
    }

    $missingAccounts = New-Object System.Collections.Generic.List[object]
    $missingCharacters = New-Object System.Collections.Generic.List[object]
    $resolvedMembers = New-Object System.Collections.Generic.List[object]
    foreach ($member in @($Definition.ordered_roster | Sort-Object ordinal)) {
        if (-not $accountByName.ContainsKey($member.account_name)) {
            $missingAccounts.Add($member)
            $missingCharacters.Add($member)
            continue
        }
        $accountId = [string]$accountByName[$member.account_name].id
        $ownedCharacters = @(if ($charactersByAccount.ContainsKey($accountId)) { $charactersByAccount[$accountId] })
        if ($ownedCharacters.Count -gt 1) {
            throw "Disposable account $($member.account_name) owns more than one character and will not be reused."
        }
        $exact = @($ownedCharacters | Where-Object name -eq $member.character_name)
        if ($exact.Count -eq 0) {
            if ($ownedCharacters.Count -eq 1) {
                throw "Disposable account $($member.account_name) owns non-fixture character $($ownedCharacters[0].name)."
            }
            $missingCharacters.Add($member)
            continue
        }
        if ($exact.Count -ne 1) { throw "Could not resolve one exact character for $($member.character_name)." }
        $resolvedMembers.Add([pscustomobject][ordered]@{
            ordinal = [int]$member.ordinal
            side_slot = [int]$member.side_slot
            role_slot = $member.role_slot
            role_category = $member.role_category
            faction = $member.faction
            account_name = $member.account_name
            account_id = [int64]$accountByName[$member.account_name].id
            character_name = $member.character_name
            character_guid = [uint32]$exact[0].guid
            race_id = [int]$exact[0].race
            class_id = [int]$exact[0].class
            observed_level = [int]$exact[0].level
            spec_index = [int]$member.spec_index
            spec_name = $member.spec_name
            gear_quality = [int]$member.gear_quality
            gear_quality_name = $member.gear_quality_name
            needs_fixture_init = ([int]$exact[0].level -ne [int]$member.level)
        })
    }

    return [pscustomobject][ordered]@{
        schema = $script:PvpRoleFixtureManifestSchema
        schema_version = 1
        fixture_id = $Definition.fixture_id
        complete = ($missingAccounts.Count -eq 0 -and $missingCharacters.Count -eq 0 -and $resolvedMembers.Count -eq 20)
        missing_accounts = $missingAccounts.ToArray()
        missing_characters = $missingCharacters.ToArray()
        resolved_members = @($resolvedMembers | Sort-Object ordinal)
        ordered_wsg_guids = @($resolvedMembers | Sort-Object ordinal | ForEach-Object { [uint32]$_.character_guid })
    }
}

function New-PvpRoleFixtureCharacterInsertSql {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$CharactersDatabaseName,
        [Parameter(Mandatory)][ValidateRange(1, [int]::MaxValue)][int]$AccountId,
        [Parameter(Mandatory)][psobject]$Member
    )

    $database = ConvertTo-PvpFixtureSqlIdentifier -Value $CharactersDatabaseName
    $name = ConvertTo-PvpFixtureSqlLiteral -Value $Member.character_name
    $lockName = ConvertTo-PvpFixtureSqlLiteral -Value "autowow.pvp.role-fixture.$script:PvpRoleFixtureId"
    $map = [int]$Member.position.map
    $x = [double]$Member.position.x
    $y = [double]$Member.position.y
    $z = [double]$Member.position.z
    $o = [double]$Member.position.o
    $homeMap = [int]$Member.homebind.map
    $homeZone = [int]$Member.homebind.zone
    $homeX = [double]$Member.homebind.x
    $homeY = [double]$Member.homebind.y
    $homeZ = [double]$Member.homebind.z

    return @"
SELECT GET_LOCK($lockName, 30) INTO @pvp_role_fixture_lock;
START TRANSACTION;
SELECT COALESCE(MAX(guid), 0) + 1 INTO @pvp_role_fixture_guid FROM $database.characters;
INSERT INTO $database.characters
    (guid, account, name, race, class, gender, level, map, position_x, position_y, position_z, orientation, taximask, cinematic, health, innTriggerId, at_login)
SELECT @pvp_role_fixture_guid, $AccountId, $name, $([int]$Member.race_id), $([int]$Member.class_id), $([int]$Member.gender),
       $([int]$Member.level), $map, $x, $y, $z, $o, '', 1, 1, 0, 0
WHERE @pvp_role_fixture_lock = 1
  AND NOT EXISTS (
      SELECT 1 FROM $database.characters
      WHERE name = $name OR account = $AccountId OR guid = @pvp_role_fixture_guid
  );
SET @pvp_role_fixture_inserted = ROW_COUNT();
INSERT INTO $database.character_homebind (guid, mapId, zoneId, posX, posY, posZ)
SELECT @pvp_role_fixture_guid, $homeMap, $homeZone, $homeX, $homeY, $homeZ
WHERE @pvp_role_fixture_lock = 1
  AND @pvp_role_fixture_inserted = 1
  AND EXISTS (
      SELECT 1 FROM $database.characters
      WHERE guid = @pvp_role_fixture_guid AND account = $AccountId AND name = $name
  )
  AND NOT EXISTS (SELECT 1 FROM $database.character_homebind WHERE guid = @pvp_role_fixture_guid);
SELECT @pvp_role_fixture_guid AS allocated_guid,
       @pvp_role_fixture_inserted AS inserted_rows,
       @pvp_role_fixture_lock AS lock_acquired;
COMMIT;
SELECT RELEASE_LOCK($lockName) AS released;
"@
}

function Test-PvpRoleFixtureLoadoutEvidence {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][psobject]$Definition,
        [AllowEmptyCollection()][object[]]$EvidenceRows = @()
    )

    Assert-PvpRoleFixtureDefinition -Definition $Definition | Out-Null
    $reasons = New-Object System.Collections.Generic.List[string]
    $expectedByName = @{}
    foreach ($member in @($Definition.ordered_roster)) { $expectedByName[$member.character_name] = $member }
    if (@($EvidenceRows).Count -ne 20) { $reasons.Add("evidence_count:$(@($EvidenceRows).Count)") }
    if (@($EvidenceRows.character_name | Sort-Object -Unique).Count -ne @($EvidenceRows).Count) { $reasons.Add('duplicate_character_evidence') }
    if (@($EvidenceRows.character_guid | Sort-Object -Unique).Count -ne @($EvidenceRows).Count) { $reasons.Add('duplicate_guid_evidence') }

    foreach ($row in @($EvidenceRows)) {
        $name = [string]$row.character_name
        if (-not $expectedByName.ContainsKey($name)) { $reasons.Add("unexpected_character:$name"); continue }
        $expected = $expectedByName[$name]
        if ([int]$row.class_id -ne [int]$expected.class_id) { $reasons.Add("class_mismatch:$name") }
        if ([int]$row.level -ne [int]$expected.level) { $reasons.Add("level_mismatch:$name") }
        if ([int]$row.spec_index -ne [int]$expected.spec_index -or [string]$row.spec_name -ne [string]$expected.spec_name) {
            $reasons.Add("spec_mismatch:$name")
        }
        if ([string]$row.combat_role -notmatch "(^|\+)$([regex]::Escape([string]$expected.role_category))(\+|$)") {
            $reasons.Add("combat_role_mismatch:$name")
        }
        # PlayerbotFactory may legitimately fall back one quality tier for a class/slot when no
        # compatible item exists at the requested tier. Require all slots occupied, no unknown
        # quality, and at most one such fallback rather than falsely rejecting a 15/16 rare kit.
        $equippedSlots = [int]$row.equipped_slots
        $expectedQualitySlots = [int]$row.expected_quality_slots
        if ($equippedSlots -le 0 -or $expectedQualitySlots -lt ($equippedSlots - 1) -or
            $expectedQualitySlots -gt $equippedSlots -or [int]$row.other_quality_slots -ne 0) {
            $reasons.Add("gear_quality_mismatch:$name")
        }
    }

    return [pscustomobject][ordered]@{
        status = if ($reasons.Count -eq 0) { 'PASS' } else { 'FAIL' }
        expected_level = [int]$Definition.level
        expected_quality = [int]$Definition.gear_quality
        expected_quality_name = $Definition.gear_quality_name
        evidence_count = @($EvidenceRows).Count
        reasons = $reasons.ToArray()
        members = @($EvidenceRows)
    }
}
