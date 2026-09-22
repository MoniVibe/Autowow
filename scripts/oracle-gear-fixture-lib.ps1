Set-StrictMode -Version Latest

$script:OracleGearFixtureManifestSchema = 'autowow.oracle.gear-fixture.manifest.v1'
$script:OracleGearFixtureAblationSchema = 'autowow.oracle.gear-ablation-matrix.v1'
$script:OracleGearFixtureSizes = @(5, 10, 25, 40)

# The class/spec values are intentionally supplied by the exact manifest. These minimums describe
# the comparison shape, while the existing fixture-init/factory remains the only loadout writer.
$script:OracleGearFixtureContracts = [ordered]@{
    '5'  = [ordered]@{ tank = 1; healer = 1; dps = 3; melee = 1; ranged = 1 }
    '10' = [ordered]@{ tank = 1; healer = 2; dps = 7; melee = 2; ranged = 3 }
    '25' = [ordered]@{ tank = 2; healer = 5; dps = 18; melee = 5; ranged = 8 }
    '40' = [ordered]@{ tank = 3; healer = 8; dps = 29; melee = 8; ranged = 12 }
}

function Get-OracleProperty {
    [CmdletBinding()]
    param(
        [AllowNull()][object]$Object,
        [Parameter(Mandatory)][string]$Name
    )

    if ($null -eq $Object) { return $null }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}

function Get-OracleArray {
    [CmdletBinding()]
    param([AllowNull()][object]$Value)

    if ($null -eq $Value) { return @() }
    if ($Value -is [string]) { return @($Value) }
    return @($Value)
}

function Get-OracleGearFixtureContracts {
    [CmdletBinding()]
    param()

    return $script:OracleGearFixtureContracts
}

function ConvertFrom-OracleRoleFixtureMembers {
    [CmdletBinding()]
    param([Parameter(Mandatory)][object[]]$Members)

    # Adapter for the existing pvp-role-fixture-world.ps1 resolved_members manifest. It is pure
    # data conversion: it does not dot-source the deployment script, query a database, or mutate
    # a character. The caller still has to place the returned exact rows in a complete Oracle
    # manifest and pass Assert-OracleGearFixtureManifest before any future apply.
    $classNames = @{
        1 = 'Warrior'; 2 = 'Paladin'; 3 = 'Hunter'; 4 = 'Rogue'; 5 = 'Priest'; 6 = 'Death Knight'
        7 = 'Shaman'; 8 = 'Mage'; 9 = 'Warlock'; 10 = 'Unknown'; 11 = 'Druid'
    }
    $rows = New-Object System.Collections.Generic.List[object]
    foreach ($member in @($Members | Sort-Object ordinal)) {
        $role = [string](Get-OracleProperty $member 'role_category')
        $classId = [int](Get-OracleProperty $member 'class_id')
        $damageKind = if ($role -eq 'healer') { 'ranged' }
        elseif ($role -eq 'tank') { 'melee' }
        elseif ($classId -eq 3) { 'physical-ranged' }
        else { 'caster' }
        if ($role -eq 'dps' -and $classId -in @(1, 2, 4, 11)) { $damageKind = 'melee' }
        $rows.Add([pscustomobject][ordered]@{
            ordinal = [int](Get-OracleProperty $member 'ordinal')
            guid = [uint32](Get-OracleProperty $member 'character_guid')
            account_id = [uint32](Get-OracleProperty $member 'account_id')
            name = [string](Get-OracleProperty $member 'character_name')
            faction = [string](Get-OracleProperty $member 'faction')
            class_id = $classId
            class_name = [string]$(if ([string]::IsNullOrWhiteSpace([string](Get-OracleProperty $member 'class_name'))) { $classNames[$classId] } else { Get-OracleProperty $member 'class_name' })
            role_category = $role
            damage_kind = $damageKind
            spec_index = [int](Get-OracleProperty $member 'spec_index')
            spec_name = [string](Get-OracleProperty $member 'spec_name')
        })
    }
    return $rows.ToArray()
}

function Get-OracleGearAblationMatrix {
    [CmdletBinding()]
    param()

    # This is a planning interface only. No entry contains a live mutation command, and the
    # apply script rejects ablation execution until a separate disposable-copy implementation is
    # deliberately added.
    return [pscustomobject][ordered]@{
        schema = $script:OracleGearFixtureAblationSchema
        schema_version = 1
        mutation_enabled = $false
        execution = 'plan_only_until_disposable_copy_surface_exists'
        quality = @(
            [pscustomobject][ordered]@{ id = 'rare-baseline'; quality = 3; quality_name = 'rare'; reduction = 0 },
            [pscustomobject][ordered]@{ id = 'uncommon'; quality = 2; quality_name = 'uncommon'; reduction = 1 },
            [pscustomobject][ordered]@{ id = 'common'; quality = 1; quality_name = 'normal'; reduction = 2 }
        )
        slots = @(
            [pscustomobject][ordered]@{ id = 'full-kit'; removed_slots = @(); destructive = $false },
            [pscustomobject][ordered]@{ id = 'no-trinkets'; removed_slots = @('trinket1', 'trinket2'); destructive = $true },
            [pscustomobject][ordered]@{ id = 'weapons-only'; removed_slots = @('head', 'shoulders', 'chest', 'waist', 'legs', 'feet', 'wrists', 'hands', 'back', 'neck', 'finger1', 'finger2', 'trinket1', 'trinket2'); destructive = $true }
        )
        talents = @(
            [pscustomobject][ordered]@{ id = 'full-spec'; reduction_percent = 0; destructive = $false },
            [pscustomobject][ordered]@{ id = 'minus-25-percent'; reduction_percent = 25; destructive = $true },
            [pscustomobject][ordered]@{ id = 'minus-50-percent'; reduction_percent = 50; destructive = $true },
            [pscustomobject][ordered]@{ id = 'no-talents'; reduction_percent = 100; destructive = $true }
        )
    }
}

function Assert-OracleGearAblationMatrix {
    [CmdletBinding()]
    param([Parameter(Mandatory)][psobject]$Matrix)

    if ([string](Get-OracleProperty $Matrix 'schema') -ne $script:OracleGearFixtureAblationSchema) {
        throw 'Unexpected Oracle gear-ablation matrix schema.'
    }
    if ([int](Get-OracleProperty $Matrix 'schema_version') -ne 1) {
        throw 'Unsupported Oracle gear-ablation matrix version.'
    }
    if ([bool](Get-OracleProperty $Matrix 'mutation_enabled')) {
        throw 'Oracle gear-ablation mutation is intentionally disabled in this lane.'
    }
    foreach ($section in @('quality', 'slots', 'talents')) {
        if (@(Get-OracleArray (Get-OracleProperty $Matrix $section)).Count -eq 0) {
            throw "Oracle gear-ablation matrix section '$section' is empty."
        }
    }
    return $true
}

function Assert-OracleGearFixtureManifest {
    [CmdletBinding()]
    param([Parameter(Mandatory)][psobject]$Manifest)

    if ([string](Get-OracleProperty $Manifest 'schema') -ne $script:OracleGearFixtureManifestSchema) {
        throw 'Unexpected Oracle gear fixture manifest schema.'
    }
    if ([int](Get-OracleProperty $Manifest 'schema_version') -ne 1) {
        throw 'Unsupported Oracle gear fixture manifest version.'
    }
    $fixtureId = [string](Get-OracleProperty $Manifest 'fixture_id')
    if ($fixtureId -notmatch '^[a-z][a-z0-9-]{3,47}$') {
        throw "Unsafe Oracle fixture id: $fixtureId"
    }
    if (-not [bool](Get-OracleProperty $Manifest 'local_only') -or
        -not [bool](Get-OracleProperty $Manifest 'disposable')) {
        throw 'Oracle gear fixtures must be explicitly local-only and disposable.'
    }

    $normalization = Get-OracleProperty $Manifest 'normalization'
    if ($null -eq $normalization) { throw 'Oracle fixture normalization contract is missing.' }
    if ([int](Get-OracleProperty $normalization 'level') -ne 80 -or
        [int](Get-OracleProperty $normalization 'quality') -ne 3 -or
        [string](Get-OracleProperty $normalization 'quality_name') -ne 'rare') {
        throw 'Oracle baseline must be level 80 with quality 3 (rare / blue-equivalent) gear.'
    }
    if ([int](Get-OracleProperty $normalization 'min_free_slots') -lt 8) {
        throw 'Oracle baseline must reserve at least eight carried loot slots.'
    }
    if ([int](Get-OracleProperty $normalization 'min_equipped_slots') -lt 1) {
        throw 'Oracle baseline must require at least one equipped slot.'
    }

    $idempotence = Get-OracleProperty $Manifest 'idempotence'
    if ($null -eq $idempotence -or -not [bool](Get-OracleProperty $idempotence 'read_before_write') -or
        -not [bool](Get-OracleProperty $idempotence 'exact_guid_scope')) {
        throw 'Oracle fixture idempotence must require read-before-write and exact GUID scope.'
    }
    if ([string](Get-OracleProperty $idempotence 'mismatch_policy') -notmatch '(?i)fail closed') {
        throw 'Oracle fixture mismatch policy must fail closed.'
    }
    if ([bool](Get-OracleProperty $idempotence 'direct_sql_mutation')) {
        throw 'Oracle fixture lane does not permit direct SQL mutation.'
    }

    $roster = @(Get-OracleArray (Get-OracleProperty $Manifest 'roster'))
    if ($roster.Count -ne 40) { throw "Oracle fixture manifest must contain exactly 40 roster members; found $($roster.Count)." }

    $seenGuids = @{}
    $seenOrdinals = @{}
    foreach ($member in $roster) {
        $guid = [uint64](Get-OracleProperty $member 'guid')
        $ordinal = [int](Get-OracleProperty $member 'ordinal')
        $name = [string](Get-OracleProperty $member 'name')
        $accountId = [uint64](Get-OracleProperty $member 'account_id')
        $faction = [string](Get-OracleProperty $member 'faction')
        $role = [string](Get-OracleProperty $member 'role_category')
        $damageKind = [string](Get-OracleProperty $member 'damage_kind')
        $classId = [int](Get-OracleProperty $member 'class_id')
        $className = [string](Get-OracleProperty $member 'class_name')
        $specIndex = [int](Get-OracleProperty $member 'spec_index')
        $specName = [string](Get-OracleProperty $member 'spec_name')

        if ($guid -le 0 -or $guid -gt [uint32]::MaxValue) { throw "Invalid exact GUID for $name." }
        if ($seenGuids.ContainsKey([string]$guid)) { throw "Duplicate exact GUID: $guid." }
        if ($seenOrdinals.ContainsKey([string]$ordinal)) { throw "Duplicate roster ordinal: $ordinal." }
        $seenGuids[[string]$guid] = $true
        $seenOrdinals[[string]$ordinal] = $true
        if ($ordinal -lt 1 -or $ordinal -gt 40) { throw "Roster ordinal outside 1..40: $ordinal." }
        if ($name -notmatch '^[A-Za-z][A-Za-z0-9]{2,11}$') { throw "Unsafe exact character name: $name." }
        if ($accountId -le 0 -or $accountId -gt [uint32]::MaxValue) { throw "Invalid exact account id for $name." }
        if ($faction -notin @('Alliance', 'Horde')) { throw "Invalid faction for ${name}: $faction." }
        if ($classId -lt 1 -or $classId -gt 11 -or [string]::IsNullOrWhiteSpace($className)) { throw "Invalid class contract for $name." }
        if ($specIndex -lt 0 -or $specIndex -gt 19 -or [string]::IsNullOrWhiteSpace($specName)) { throw "Invalid spec contract for $name." }
        if ($role -notin @('tank', 'healer', 'dps')) { throw "Invalid role category for ${name}: $role." }
        if ($role -eq 'dps' -and $damageKind -notin @('melee', 'physical-ranged', 'caster')) {
            throw "DPS member $name must declare melee, physical-ranged, or caster damage kind."
        }
        if ($role -ne 'dps' -and $damageKind -notin @('melee', 'ranged')) {
            throw "Non-DPS member $name has an invalid damage kind: $damageKind."
        }
    }
    if (@(1..40 | Where-Object { -not $seenOrdinals.ContainsKey([string]$_) }).Count -ne 0) {
        throw 'Oracle roster ordinals must be the complete deterministic range 1..40.'
    }

    $profiles = Get-OracleProperty $Manifest 'profiles'
    $contracts = Get-OracleGearFixtureContracts
    foreach ($size in $script:OracleGearFixtureSizes) {
        $profile = Get-OracleProperty $profiles ([string]$size)
        if ($null -eq $profile) { throw "Oracle profile $size is missing." }
        $ordinals = @(Get-OracleArray (Get-OracleProperty $profile 'member_ordinals') | ForEach-Object { [int]$_ })
        if ($ordinals.Count -ne $size -or (($ordinals | Sort-Object) -join ',') -ne ((1..$size) -join ',')) {
            throw "Oracle profile $size must select deterministic ordinals 1..$size."
        }
        $selected = @($roster | Where-Object { [int]$_.ordinal -in $ordinals })
        $factions = @($selected | ForEach-Object { [string]$_.faction } | Sort-Object -Unique)
        if ($factions.Count -ne 1) { throw "Oracle profile $size must be single-faction; found $($factions -join ',')." }
        $contract = $contracts[[string]$size]
        $tankCount = @($selected | Where-Object role_category -eq 'tank').Count
        $healerCount = @($selected | Where-Object role_category -eq 'healer').Count
        $dpsCount = @($selected | Where-Object role_category -eq 'dps').Count
        $meleeCount = @($selected | Where-Object damage_kind -eq 'melee').Count
        $rangedCount = @($selected | Where-Object { $_.damage_kind -in @('physical-ranged', 'caster', 'ranged') }).Count
        if ($tankCount -lt $contract.tank -or $healerCount -lt $contract.healer -or $dpsCount -lt $contract.dps -or
            $meleeCount -lt $contract.melee -or $rangedCount -lt $contract.ranged) {
            throw "Oracle profile $size does not meet its tank/healer/melee/ranged contract."
        }
    }

    $matrix = Get-OracleProperty $Manifest 'ablation_matrix'
    if ($null -ne $matrix) { Assert-OracleGearAblationMatrix -Matrix $matrix | Out-Null }
    return $true
}

function Read-OracleGearFixtureManifest {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Path)

    $resolved = (Resolve-Path -LiteralPath $Path -ErrorAction Stop).Path
    $manifest = Get-Content -LiteralPath $resolved -Raw | ConvertFrom-Json
    Assert-OracleGearFixtureManifest -Manifest $manifest | Out-Null
    return $manifest
}

function Get-OracleGearProfileMembers {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][psobject]$Manifest,
        [Parameter(Mandatory)][ValidateSet(5, 10, 25, 40)][int]$Size
    )

    Assert-OracleGearFixtureManifest -Manifest $Manifest | Out-Null
    $profiles = Get-OracleProperty $Manifest 'profiles'
    $profile = Get-OracleProperty $profiles ([string]$Size)
    $ordinals = @(Get-OracleArray (Get-OracleProperty $profile 'member_ordinals'))
    $byOrdinal = @{}
    foreach ($member in @(Get-OracleArray (Get-OracleProperty $Manifest 'roster'))) {
        $byOrdinal[[int](Get-OracleProperty $member 'ordinal')] = $member
    }
    return @($ordinals | ForEach-Object { $byOrdinal[[int]$_] })
}

function Get-OracleFixtureInitRequest {
    [CmdletBinding()]
    param([Parameter(Mandatory)][psobject]$Member, [Parameter(Mandatory)][psobject]$Manifest)

    $normalization = Get-OracleProperty $Manifest 'normalization'
    return "fixture init $([uint32](Get-OracleProperty $Member 'guid')) $([int](Get-OracleProperty $normalization 'level')) $([int](Get-OracleProperty $Member 'spec_index')) $([int](Get-OracleProperty $normalization 'quality'))"
}

function Test-OracleFixtureIdentity {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][psobject]$Member,
        [Parameter(Mandatory)][psobject]$Status
    )

    $reasons = New-Object System.Collections.Generic.List[string]
    if (-not [bool](Get-OracleProperty $Status 'ok')) { $reasons.Add("status_not_ok:$([string](Get-OracleProperty $Status 'error'))") }
    if ([uint32](Get-OracleProperty $Status 'guid') -ne [uint32](Get-OracleProperty $Member 'guid')) { $reasons.Add('guid_mismatch') }
    $identity = Get-OracleProperty $Status 'identity'
    $class = Get-OracleProperty $Status 'class'
    if ($null -eq $identity -or $null -eq $class) { $reasons.Add('identity_evidence_missing'); return [pscustomobject][ordered]@{ status = 'FAIL'; reasons = $reasons.ToArray() } }
    if ([string](Get-OracleProperty $identity 'name') -ne [string](Get-OracleProperty $Member 'name')) { $reasons.Add('name_mismatch') }
    if ([uint32](Get-OracleProperty $identity 'account_id') -ne [uint32](Get-OracleProperty $Member 'account_id')) { $reasons.Add('account_ownership_mismatch') }
    if ([string](Get-OracleProperty $identity 'faction') -ne [string](Get-OracleProperty $Member 'faction')) { $reasons.Add('faction_mismatch') }
    if ([int](Get-OracleProperty $class 'id') -ne [int](Get-OracleProperty $Member 'class_id')) { $reasons.Add('class_mismatch') }
    return [pscustomobject][ordered]@{ status = if ($reasons.Count -eq 0) { 'PASS' } else { 'FAIL' }; reasons = $reasons.ToArray() }
}

function Test-OracleFixtureEvidence {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][psobject]$Member,
        [Parameter(Mandatory)][psobject]$Manifest,
        [Parameter(Mandatory)][psobject]$Status,
        [Parameter(Mandatory)][psobject]$Snapshot,
        [Parameter(Mandatory)][psobject]$Combat
    )

    $reasons = New-Object System.Collections.Generic.List[string]
    $identity = Test-OracleFixtureIdentity -Member $Member -Status $Status
    foreach ($reason in @($identity.reasons)) { $reasons.Add([string]$reason) }
    $normalization = Get-OracleProperty $Manifest 'normalization'
    $spec = Get-OracleProperty $Status 'spec'
    $role = Get-OracleProperty $Status 'role'
    $gear = Get-OracleProperty $Status 'gear'
    $inventory = Get-OracleProperty $Status 'inventory'
    $talents = Get-OracleProperty $Status 'talents'
    $spells = Get-OracleProperty $Status 'spells'
    $weapons = Get-OracleProperty $Status 'weapons'
    if ([int](Get-OracleProperty $Status 'level') -ne [int](Get-OracleProperty $normalization 'level')) { $reasons.Add('level_mismatch') }
    if ([int](Get-OracleProperty $spec 'stored_index') -ne [int](Get-OracleProperty $Member 'spec_index')) { $reasons.Add('spec_mismatch') }
    $combatRole = [string](Get-OracleProperty $role 'combat')
    $expectedRole = [string](Get-OracleProperty $Member 'role_category')
    if ($combatRole -notmatch "(^|\+)$([regex]::Escape($expectedRole))(\+|$)") { $reasons.Add('combat_role_mismatch') }
    $qualityCounts = Get-OracleProperty $gear 'quality_counts'
    $qualityName = [string](Get-OracleProperty $normalization 'quality_name')
    if ([int](Get-OracleProperty $gear 'equipped_slots') -lt [int](Get-OracleProperty $normalization 'min_equipped_slots')) { $reasons.Add('insufficient_equipped_slots') }
    if ([int](Get-OracleProperty $qualityCounts $qualityName) -lt [int](Get-OracleProperty $normalization 'min_quality_slots')) { $reasons.Add('insufficient_normalized_quality_slots') }
    if ([int](Get-OracleProperty $gear 'other_quality_slots') -gt [int](Get-OracleProperty $normalization 'max_other_quality_slots')) { $reasons.Add('unexpected_quality_slot') }
    if ([int](Get-OracleProperty $inventory 'free_slots') -lt [int](Get-OracleProperty $normalization 'min_free_slots') -or
        -not [bool](Get-OracleProperty $inventory 'loot_slot_reserve_met')) { $reasons.Add('loot_capacity_not_ready') }
    if (-not [bool](Get-OracleProperty $weapons 'ready')) { $reasons.Add('weapon_not_ready') }
    if ([int](Get-OracleProperty $talents 'allocated') -lt [int](Get-OracleProperty $normalization 'min_allocated_talents')) { $reasons.Add('talents_not_allocated') }
    if ([int](Get-OracleProperty $spells 'known') -lt [int](Get-OracleProperty $normalization 'min_known_spells') -or
        -not [bool](Get-OracleProperty $spells 'role_critical_ready')) { $reasons.Add('role_critical_spells_not_ready') }
    if (-not [bool](Get-OracleProperty $Snapshot 'name') -or
        [string](Get-OracleProperty $Snapshot 'name') -ne [string](Get-OracleProperty $Member 'name')) { $reasons.Add('snapshot_identity_mismatch') }
    $snapshotProgress = Get-OracleProperty $Snapshot 'progress'
    if ([int](Get-OracleProperty $snapshotProgress 'level') -ne [int](Get-OracleProperty $normalization 'level')) { $reasons.Add('snapshot_level_mismatch') }
    $counters = Get-OracleProperty $Combat 'counters'
    if (-not [bool](Get-OracleProperty $counters 'available')) { $reasons.Add('combat_counters_unavailable') }
    return [pscustomobject][ordered]@{
        status = if ($reasons.Count -eq 0) { 'PASS' } else { 'FAIL' }
        guid = [uint32](Get-OracleProperty $Member 'guid')
        name = [string](Get-OracleProperty $Member 'name')
        role = $expectedRole
        reasons = $reasons.ToArray()
    }
}
