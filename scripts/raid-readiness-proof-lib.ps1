Set-StrictMode -Version Latest

$script:RaidReadinessSchema = 'autowow.raid-readiness.proof.v2'
$script:RaidReadinessOnyxiaMapId = [uint32]249
$script:RaidReadinessOnyxiaCreditEntry = [uint32]10184
$script:RaidReadinessOnyxiaTenPlayerEncounterId = [uint32]707
$script:RaidReadinessOnyxiaTwentyFivePlayerEncounterId = [uint32]708
$script:RaidReadinessOnyxiaEncounterAuthority = 'pinned DungeonEncounter.dbc mapping + acore_world.instance_encounters + acore_characters.instance.completedEncounters'

function Get-RaidReadinessProperty {
    [CmdletBinding()]
    param(
        [AllowNull()][object]$Object,
        [Parameter(Mandatory)][string[]]$Name,
        [AllowNull()][object]$Default = $null
    )

    if ($null -eq $Object) { return $Default }
    foreach ($candidate in $Name) {
        if ($Object -is [System.Collections.IDictionary] -and $Object.Contains($candidate)) {
            return $Object[$candidate]
        }
        $property = $Object.PSObject.Properties[$candidate]
        if ($null -ne $property) { return $property.Value }
    }
    return $Default
}

function ConvertTo-RaidReadinessLong {
    [CmdletBinding()]
    param([AllowNull()][object]$Value)

    if ($null -eq $Value -or [string]::IsNullOrWhiteSpace([string]$Value)) { return $null }
    $number = 0L
    if ([long]::TryParse([string]$Value, [ref]$number)) { return $number }
    return $null
}

function ConvertTo-RaidReadinessDouble {
    [CmdletBinding()]
    param([AllowNull()][object]$Value)

    if ($null -eq $Value -or [string]::IsNullOrWhiteSpace([string]$Value)) { return $null }
    $number = 0.0
    if ([double]::TryParse([string]$Value, [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$number)) {
        return $number
    }
    return $null
}

function Get-RaidReadinessOnyxiaExpectedDifficulty {
    [CmdletBinding()]
    param([Parameter(Mandatory)][int]$RaidSize)

    switch ($RaidSize) {
        10 { return 0 }
        25 { return 1 }
        default { return $null }
    }
}

function Assert-RaidReadinessSafeDatabaseIdentifier {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Name)

    if ($Name -notmatch '^[A-Za-z0-9_]+$') {
        throw "Unsafe database identifier: $Name"
    }
}

function Get-RaidReadinessConfigValue {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$ConfigText,
        [Parameter(Mandatory)][string[]]$Keys
    )

    foreach ($key in $Keys) {
        $pattern = '(?im)^\s*' + [regex]::Escape($key) + '\s*=\s*(?<value>[^#\r\n]+)'
        $match = [regex]::Match($ConfigText, $pattern)
        if ($match.Success) {
            return $match.Groups['value'].Value.Trim().Trim('"')
        }
    }

    return $null
}

function ConvertTo-RaidReadinessDatabaseConnection {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Info,
        [Parameter(Mandatory)][string]$DatabaseOverride
    )

    $parts = @($Info.Trim().Trim('"').Split(';'))
    if ($parts.Count -lt 5) {
        throw 'DatabaseInfo must contain host;port;user;password;database.'
    }

    $port = 0L
    if (-not [long]::TryParse($parts[1].Trim(), [ref]$port) -or $port -lt 1 -or $port -gt 65535) {
        throw "Invalid database port in DatabaseInfo: $($parts[1])"
    }
    Assert-RaidReadinessSafeDatabaseIdentifier -Name $DatabaseOverride

    return [pscustomobject][ordered]@{
        Host = $parts[0].Trim()
        Port = [int]$port
        User = $parts[2].Trim()
        Password = $parts[3]
        Database = $DatabaseOverride
    }
}

function Get-RaidReadinessDatabaseConnections {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$ServerRoot,
        [string]$WorldServerConfigPath = '',
        [string]$CharactersDatabaseName = 'acore_characters',
        [string]$WorldDatabaseName = 'acore_world'
    )

    $configPath = if ([string]::IsNullOrWhiteSpace($WorldServerConfigPath)) {
        Join-Path $ServerRoot 'server\configs\worldserver.conf'
    } else {
        $WorldServerConfigPath
    }
    if (-not (Test-Path -LiteralPath $configPath -PathType Leaf)) {
        throw "Worldserver config not found: $configPath"
    }

    $configText = Get-Content -LiteralPath $configPath -Raw
    $characterInfo = Get-RaidReadinessConfigValue -ConfigText $configText -Keys @('CharacterDatabaseInfo')
    $worldInfo = Get-RaidReadinessConfigValue -ConfigText $configText -Keys @('WorldDatabaseInfo')
    if ([string]::IsNullOrWhiteSpace($characterInfo) -or [string]::IsNullOrWhiteSpace($worldInfo)) {
        throw 'Could not find CharacterDatabaseInfo and WorldDatabaseInfo in the existing worldserver config.'
    }

    return [ordered]@{
        characters = ConvertTo-RaidReadinessDatabaseConnection -Info $characterInfo -DatabaseOverride $CharactersDatabaseName
        world = ConvertTo-RaidReadinessDatabaseConnection -Info $worldInfo -DatabaseOverride $WorldDatabaseName
    }
}

function Get-RaidReadinessMySqlPath {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$ServerRoot,
        [string]$MySqlPath = ''
    )

    if (-not [string]::IsNullOrWhiteSpace($MySqlPath)) {
        if (-not (Test-Path -LiteralPath $MySqlPath -PathType Leaf)) {
            throw "Configured mysql client not found: $MySqlPath"
        }
        return [IO.Path]::GetFullPath($MySqlPath)
    }

    foreach ($candidate in @(
        (Join-Path $ServerRoot 'third_party\mysql\bin\mysql.exe'),
        (Join-Path $ServerRoot 'server\bin\mysql.exe'),
        'C:\Program Files\MySQL\MySQL Server 8.4\bin\mysql.exe',
        'C:\Program Files\MySQL\MySQL Server 8.0\bin\mysql.exe',
        'C:\Program Files\MariaDB 11.0\bin\mysql.exe'
    )) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return [IO.Path]::GetFullPath($candidate)
        }
    }

    $command = Get-Command mysql.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $command) { return $command.Source }
    throw 'mysql.exe was not found. Pass -MySqlPath or use the existing AutoWoW installation.'
}

function Assert-RaidReadinessReadOnlySql {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Sql)

    $trimmed = $Sql.Trim()
    if ($trimmed -notmatch '^(?i:SELECT)\b') {
        throw 'Raid readiness database evidence accepts SELECT-only SQL.'
    }
    if ($trimmed.Contains(';')) {
        throw 'Raid readiness database evidence rejects SQL statement separators.'
    }
    if ($trimmed -match '(?i)\b(INSERT|UPDATE|DELETE|DROP|ALTER|CREATE|TRUNCATE|REPLACE|CALL|SET|GRANT|REVOKE|LOAD|RENAME|LOCK|UNLOCK)\b') {
        throw 'Mutating SQL keyword rejected by raid readiness database evidence.'
    }
}

function ConvertFrom-RaidReadinessTabularText {
    [CmdletBinding()]
    param(
        [AllowNull()][string]$Text,
        [Parameter(Mandatory)][string[]]$Headers
    )

    $rows = New-Object System.Collections.Generic.List[object]
    if ([string]::IsNullOrWhiteSpace($Text)) { return $rows.ToArray() }

    foreach ($line in ($Text -split "`r?`n")) {
        if ([string]::IsNullOrWhiteSpace($line) -or $line -match '^(?i:warning|mysql:)') { continue }
        $parts = $line.Split([char]9)
        $row = [ordered]@{}
        for ($index = 0; $index -lt $Headers.Count; $index++) {
            $row[$Headers[$index]] = if ($index -lt $parts.Count) { $parts[$index] } else { '' }
        }
        [void]$rows.Add([pscustomobject]$row)
    }

    return $rows.ToArray()
}

function Invoke-RaidReadinessReadOnlySql {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][object]$Connection,
        [Parameter(Mandatory)][string]$Sql,
        [Parameter(Mandatory)][string[]]$Headers,
        [Parameter(Mandatory)][string]$MysqlPath
    )

    Assert-RaidReadinessReadOnlySql -Sql $Sql
    $oldPassword = [Environment]::GetEnvironmentVariable('MYSQL_PWD', 'Process')
    $hadPassword = $null -ne $oldPassword
    try {
        $env:MYSQL_PWD = [string]$Connection.Password
        $arguments = @(
            '--protocol=tcp',
            "--host=$($Connection.Host)",
            "--port=$($Connection.Port)",
            "--user=$($Connection.User)",
            "--database=$($Connection.Database)",
            '--default-character-set=utf8mb4',
            '--batch',
            '--skip-column-names',
            "--execute=$Sql"
        )
        $output = @(& $MysqlPath @arguments 2>&1 | ForEach-Object { [string]$_ })
        $exitCode = $LASTEXITCODE
        if ($exitCode -ne 0) {
            throw "Read-only SQL failed for database $($Connection.Database) with exit code $exitCode."
        }
        return @(ConvertFrom-RaidReadinessTabularText -Text ($output -join [Environment]::NewLine) -Headers $Headers)
    }
    finally {
        if ($hadPassword) {
            $env:MYSQL_PWD = $oldPassword
        } else {
            Remove-Item Env:MYSQL_PWD -ErrorAction SilentlyContinue
        }
    }
}

function New-RaidReadinessOnyxiaReadOnlyQuerySet {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][object]$Contract,
        [string]$CharactersDatabaseName = 'acore_characters',
        [string]$WorldDatabaseName = 'acore_world'
    )

    if ([uint32]$Contract.expected_map_id -ne $script:RaidReadinessOnyxiaMapId) {
        throw 'The Onyxia authoritative query set is scoped to map 249.'
    }
    Assert-RaidReadinessSafeDatabaseIdentifier -Name $CharactersDatabaseName
    Assert-RaidReadinessSafeDatabaseIdentifier -Name $WorldDatabaseName

    $guids = @($Contract.roster_guids | ForEach-Object { [uint32]$_ } | Sort-Object -Unique)
    if ($guids.Count -eq 0) { throw 'The Onyxia authoritative query set requires a non-empty roster.' }
    $guidList = $guids -join ','
    $expectedInstanceId = [uint32]$Contract.expected_instance_id

    return [pscustomobject][ordered]@{
        read_only = $true
        database_mutations = 0
        bindings = [pscustomobject][ordered]@{
            sql = "SELECT ci.guid AS guid, ci.instance AS instance_id FROM ${CharactersDatabaseName}.character_instance ci WHERE ci.guid IN ($guidList) OR ci.instance = $expectedInstanceId ORDER BY ci.guid, ci.instance"
            headers = @('guid', 'instance_id')
            source = "${CharactersDatabaseName}.character_instance"
        }
        instances = [pscustomobject][ordered]@{
            sql = "SELECT i.id AS instance_id, i.map AS map_id, i.difficulty AS difficulty, i.completedEncounters AS completed_encounters FROM ${CharactersDatabaseName}.instance i WHERE i.id = $expectedInstanceId"
            headers = @('instance_id', 'map_id', 'difficulty', 'completed_encounters')
            source = "${CharactersDatabaseName}.instance"
        }
        encounter_bits = [pscustomobject][ordered]@{
            # dungeonencounter_dbc is an override table and is empty in an ordinary AzerothCore
            # world database. The encounter ids/map/difficulty/bit below are the pinned build's
            # DungeonEncounter.dbc mapping; instance_encounters remains the SQL authority that
            # ties those records to Onyxia's kill credit entry.
            sql = "SELECT e.entry AS encounter_id, $($script:RaidReadinessOnyxiaMapId) AS map_id, CASE e.entry WHEN $($script:RaidReadinessOnyxiaTenPlayerEncounterId) THEN 0 WHEN $($script:RaidReadinessOnyxiaTwentyFivePlayerEncounterId) THEN 1 END AS difficulty, 0 AS encounter_bit FROM ${WorldDatabaseName}.instance_encounters e WHERE e.entry IN ($($script:RaidReadinessOnyxiaTenPlayerEncounterId),$($script:RaidReadinessOnyxiaTwentyFivePlayerEncounterId)) AND e.creditType = 0 AND e.creditEntry = $($script:RaidReadinessOnyxiaCreditEntry) ORDER BY e.entry"
            headers = @('encounter_id', 'map_id', 'difficulty', 'encounter_bit')
            source = "pinned DungeonEncounter.dbc mapping + ${WorldDatabaseName}.instance_encounters"
        }
    }
}

function New-RaidReadinessBlockedCompletionCreditGate {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][object]$Contract,
        [Parameter(Mandatory)][string[]]$Reasons,
        [string]$Source = 'read_only_authority_unresolved',
        [string]$Detail = ''
    )

    $errorValue = if ([string]::IsNullOrWhiteSpace($Detail)) { $null } else { $Detail }
    $expectedRoster = @($Contract.roster_guids | ForEach-Object { [uint32]$_ })
    $sharedInstance = [pscustomobject][ordered]@{
        status = 'UNPROVEN'
        expected_instance_id = [uint32]$Contract.expected_instance_id
        resolved_instance_id = $null
        map_id = $null
        difficulty = $null
        completed_encounters = $null
        source = $Source
    }

    return [pscustomobject][ordered]@{
        schema = $script:RaidReadinessSchema
        status = 'BLOCKED'
        required_for_pass = $true
        applicable = [uint32]$Contract.expected_map_id -eq $script:RaidReadinessOnyxiaMapId
        authority = $script:RaidReadinessOnyxiaEncounterAuthority
        read_only = $true
        database_mutations = 0
        target_damage_authoritative = $false
        exact_target_death_authoritative = $false
        expected_map_id = [uint32]$Contract.expected_map_id
        expected_instance_id = [uint32]$Contract.expected_instance_id
        resolved_instance_id = $null
        reasons = @($Reasons)
        error = $errorValue
        shared_instance = $sharedInstance
        encounter_done = [pscustomobject][ordered]@{
            status = 'UNPROVEN'
            done = $null
            encounter_bit = $null
            encounter_mask = $null
            completed_encounters = $null
            source = $Source
        }
        exact_roster_credit = [pscustomobject][ordered]@{
            status = 'UNPROVEN'
            exact_match = $null
            expected_roster_guids = $expectedRoster
            credited_roster_guids = $null
            source = 'acore_characters.character_instance'
        }
    }
}

function Test-RaidReadinessOnyxiaCompletionCredit {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][object]$Contract,
        [AllowNull()][object[]]$BindingRows = @(),
        [AllowNull()][object[]]$InstanceRows = @(),
        [AllowNull()][object[]]$EncounterRows = @(),
        [AllowNull()][object]$EncounterBit = $null
    )

    if ([uint32]$Contract.expected_map_id -ne $script:RaidReadinessOnyxiaMapId) {
        return New-RaidReadinessBlockedCompletionCreditGate -Contract $Contract `
            -Reasons @('authoritative_onyxia_gate_not_applicable_to_expected_map') `
            -Source 'map_249_only'
    }

    $expectedRoster = @($Contract.roster_guids | ForEach-Object { [uint32]$_ } | Sort-Object -Unique)
    $expectedInstanceId = [uint32]$Contract.expected_instance_id
    $expectedDifficulty = Get-RaidReadinessOnyxiaExpectedDifficulty -RaidSize ([int]$Contract.raid_size)
    $reasons = New-Object System.Collections.Generic.List[string]

    $normalizedBindings = New-Object System.Collections.Generic.List[object]
    foreach ($row in @($BindingRows)) {
        $guid = ConvertTo-RaidReadinessLong (Get-RaidReadinessProperty -Object $row -Name @('guid', 'character_guid'))
        $instanceId = ConvertTo-RaidReadinessLong (Get-RaidReadinessProperty -Object $row -Name @('instance_id', 'instance'))
        if ($null -ne $guid -and $guid -gt 0) {
            $normalizedBindings.Add([pscustomobject][ordered]@{
                    guid = [uint32]$guid
                    instance_id = $instanceId
                })
        }
    }

    $expectedBindingRows = @($normalizedBindings | Where-Object { $null -ne $_.instance_id -and [long]$_.instance_id -eq [long]$expectedInstanceId })
    $creditedRosterGuids = @($expectedBindingRows | ForEach-Object { [uint32]$_.guid } | Sort-Object -Unique)
    $missingRosterGuids = @($expectedRoster | Where-Object { $creditedRosterGuids -notcontains [uint32]$_ })
    $unexpectedSharedGuids = @($creditedRosterGuids | Where-Object { $expectedRoster -notcontains [uint32]$_ })
    $rosterObservedInstanceIds = @(
        $normalizedBindings |
            Where-Object { $expectedRoster -contains [uint32]$_.guid -and $null -ne $_.instance_id -and [long]$_.instance_id -gt 0 } |
            ForEach-Object { [long]$_.instance_id } |
            Sort-Object -Unique
    )

    $bindingStatus = 'UNPROVEN'
    if ($missingRosterGuids.Count -eq 0) {
        if ($unexpectedSharedGuids.Count -gt 0) {
            $bindingStatus = 'FAIL'
            foreach ($guid in $unexpectedSharedGuids) { $reasons.Add("unexpected_guid_bound_to_expected_instance=$guid") }
        } else {
            $bindingStatus = 'PASS'
        }
    } else {
        foreach ($guid in $missingRosterGuids) { $reasons.Add("guid_${guid}_missing_expected_instance_binding") }
        if ($normalizedBindings.Count -eq 0) {
            $reasons.Add('exact_roster_instance_binding_not_resolved')
        } elseif ($expectedBindingRows.Count -eq 0 -and $rosterObservedInstanceIds.Count -gt 0) {
            $reasons.Add("roster_bound_to_other_instance=$($rosterObservedInstanceIds -join ',')")
        } else {
            $reasons.Add('exact_roster_instance_binding_incomplete')
        }
    }

    $normalizedInstances = New-Object System.Collections.Generic.List[object]
    foreach ($row in @($InstanceRows)) {
        $instanceId = ConvertTo-RaidReadinessLong (Get-RaidReadinessProperty -Object $row -Name @('instance_id', 'id', 'instance'))
        if ($null -eq $instanceId -or $instanceId -le 0) { continue }
        $normalizedInstances.Add([pscustomobject][ordered]@{
                instance_id = [long]$instanceId
                map_id = ConvertTo-RaidReadinessLong (Get-RaidReadinessProperty -Object $row -Name @('map_id', 'map'))
                difficulty = ConvertTo-RaidReadinessLong (Get-RaidReadinessProperty -Object $row -Name @('difficulty', 'mode'))
                completed_encounters = ConvertTo-RaidReadinessLong (Get-RaidReadinessProperty -Object $row -Name @('completed_encounters', 'completedEncounters', 'encounter_mask'))
            })
    }

    $expectedInstanceRows = @($normalizedInstances | Where-Object { [long]$_.instance_id -eq [long]$expectedInstanceId })
    $resolvedInstanceId = $null
    $observedMapId = $null
    $observedDifficulty = $null
    $completedEncounters = $null
    $instanceStatus = 'UNPROVEN'
    if ($expectedInstanceRows.Count -eq 0) {
        if ($normalizedInstances.Count -eq 0) {
            $reasons.Add('shared_instance_row_not_resolved')
        } else {
            $observedIds = @($normalizedInstances | ForEach-Object { [long]$_.instance_id } | Sort-Object -Unique)
            $reasons.Add("expected_instance_row_not_found_observed=$($observedIds -join ',')")
        }
    } elseif ($expectedInstanceRows.Count -gt 1) {
        $reasons.Add('shared_instance_row_ambiguous')
    } else {
        $instance = $expectedInstanceRows[0]
        $resolvedInstanceId = [long]$instance.instance_id
        $observedMapId = $instance.map_id
        $observedDifficulty = $instance.difficulty
        $completedEncounters = $instance.completed_encounters
        if ($null -eq $observedMapId) {
            $reasons.Add('shared_instance_map_not_resolved')
        } elseif ([long]$observedMapId -ne [long]$script:RaidReadinessOnyxiaMapId) {
            $instanceStatus = 'FAIL'
            $reasons.Add("shared_instance_map_mismatch=$observedMapId")
        } else {
            if ($null -ne $expectedDifficulty -and $null -ne $observedDifficulty -and [long]$observedDifficulty -ne [long]$expectedDifficulty) {
                $instanceStatus = 'FAIL'
                $reasons.Add("shared_instance_difficulty_mismatch=$observedDifficulty")
            } else {
                $instanceStatus = 'PASS'
            }
        }
    }

    $rawEncounterRows = @($EncounterRows)
    $normalizedEncounterRows = New-Object System.Collections.Generic.List[object]
    if ($null -ne $EncounterBit) {
        $directBit = ConvertTo-RaidReadinessLong $EncounterBit
        if ($null -ne $directBit) {
            $normalizedEncounterRows.Add([pscustomobject][ordered]@{
                    map_id = [long]$script:RaidReadinessOnyxiaMapId
                    difficulty = $expectedDifficulty
                    encounter_bit = [long]$directBit
                    source = 'explicit_read_only_encounter_bit'
                })
        }
    } else {
        foreach ($row in $rawEncounterRows) {
            $normalizedEncounterRows.Add([pscustomobject][ordered]@{
                    map_id = ConvertTo-RaidReadinessLong (Get-RaidReadinessProperty -Object $row -Name @('map_id', 'map', 'MapID'))
                    difficulty = ConvertTo-RaidReadinessLong (Get-RaidReadinessProperty -Object $row -Name @('difficulty', 'mode', 'Difficulty'))
                    encounter_bit = ConvertTo-RaidReadinessLong (Get-RaidReadinessProperty -Object $row -Name @('encounter_bit', 'bit', 'Bit'))
                    source = 'pinned DungeonEncounter.dbc mapping + acore_world.instance_encounters'
                })
        }
    }

    $mapMatchedEncounterRows = @($normalizedEncounterRows | Where-Object { $null -ne $_.map_id -and [long]$_.map_id -eq [long]$script:RaidReadinessOnyxiaMapId })
    $difficultyMatchedEncounterRows = if ($null -eq $expectedDifficulty) {
        @($mapMatchedEncounterRows)
    } else {
        @($mapMatchedEncounterRows | Where-Object { $null -eq $_.difficulty -or [long]$_.difficulty -eq [long]$expectedDifficulty })
    }

    $encounterBitValue = $null
    $encounterMask = $null
    $encounterDone = $null
    $encounterDoneStatus = 'UNPROVEN'
    if ($mapMatchedEncounterRows.Count -eq 0) {
        if ($normalizedEncounterRows.Count -gt 0) {
            $reasons.Add('onyxia_encounter_bit_map_mismatch')
        } else {
            $reasons.Add('onyxia_encounter_bit_not_resolved')
        }
    } elseif ($difficultyMatchedEncounterRows.Count -eq 0) {
        $reasons.Add('onyxia_encounter_bit_difficulty_not_resolved')
    } else {
        $bitValues = @($difficultyMatchedEncounterRows | ForEach-Object { $_.encounter_bit } | Where-Object { $null -ne $_ } | ForEach-Object { [long]$_ } | Sort-Object -Unique)
        if ($bitValues.Count -eq 0) {
            $reasons.Add('onyxia_encounter_bit_not_resolved')
        } elseif ($bitValues.Count -ne 1) {
            $reasons.Add("onyxia_encounter_bit_ambiguous=$($bitValues -join ',')")
        } elseif ($bitValues[0] -lt 0 -or $bitValues[0] -gt 31) {
            $reasons.Add("onyxia_encounter_bit_out_of_range=$($bitValues[0])")
        } else {
            $encounterBitValue = [long]$bitValues[0]
            $encounterMask = [uint64]1 -shl [int]$encounterBitValue
            if ($null -eq $completedEncounters) {
                $reasons.Add('completed_encounters_mask_not_resolved')
            } else {
                $encounterDone = (([uint64][long]$completedEncounters -band [uint64]$encounterMask) -ne 0)
                if ($encounterDone) {
                    $encounterDoneStatus = 'PASS'
                } else {
                    $encounterDoneStatus = 'FAIL'
                    $reasons.Add("onyxia_encounter_bit_not_set=$encounterBitValue")
                }
            }
        }
    }

    $exactMatch = if ($bindingStatus -eq 'PASS' -and $instanceStatus -eq 'PASS') {
        $true
    } elseif ($bindingStatus -eq 'FAIL') {
        $false
    } else {
        $null
    }
    $exactCreditStatus = if ($exactMatch -eq $true) { 'PASS' } elseif ($exactMatch -eq $false) { 'FAIL' } else { 'UNPROVEN' }
    $subStatuses = @($instanceStatus, $exactCreditStatus, $encounterDoneStatus)
    $overall = if (@($subStatuses | Where-Object { $_ -eq 'FAIL' }).Count -gt 0) {
        'FAIL'
    } elseif (@($subStatuses | Where-Object { $_ -eq 'UNPROVEN' }).Count -gt 0) {
        'BLOCKED'
    } else {
        'PASS'
    }

    return [pscustomobject][ordered]@{
        schema = $script:RaidReadinessSchema
        status = $overall
        required_for_pass = $true
        applicable = $true
        authority = $script:RaidReadinessOnyxiaEncounterAuthority
        read_only = $true
        database_mutations = 0
        target_damage_authoritative = $false
        exact_target_death_authoritative = ($overall -eq 'PASS' -and $encounterDone -eq $true -and $exactMatch -eq $true)
        expected_map_id = [uint32]$Contract.expected_map_id
        expected_instance_id = $expectedInstanceId
        resolved_instance_id = $resolvedInstanceId
        reasons = @($reasons)
        shared_instance = [pscustomobject][ordered]@{
            status = $instanceStatus
            expected_instance_id = $expectedInstanceId
            resolved_instance_id = $resolvedInstanceId
            map_id = $observedMapId
            difficulty = $observedDifficulty
            expected_difficulty = $expectedDifficulty
            completed_encounters = $completedEncounters
            source = 'acore_characters.instance + acore_characters.character_instance'
        }
        encounter_done = [pscustomobject][ordered]@{
            status = $encounterDoneStatus
            done = $encounterDone
            encounter_bit = $encounterBitValue
            encounter_mask = $encounterMask
            completed_encounters = $completedEncounters
            source = 'pinned DungeonEncounter.dbc bit + acore_world.instance_encounters + acore_characters.instance.completedEncounters'
        }
        exact_roster_credit = [pscustomobject][ordered]@{
            status = $exactCreditStatus
            exact_match = $exactMatch
            expected_roster_guids = $expectedRoster
            credited_roster_guids = if ($creditedRosterGuids.Count -gt 0) { $creditedRosterGuids } else { $null }
            source = 'acore_characters.character_instance'
        }
    }
}

function Get-RaidReadinessOnyxiaCompletionCreditFromDatabase {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][object]$Contract,
        [Parameter(Mandatory)][string]$ServerRoot,
        [string]$WorldServerConfigPath = '',
        [string]$MySqlPath = '',
        [string]$CharactersDatabaseName = 'acore_characters',
        [string]$WorldDatabaseName = 'acore_world'
    )

    if ([uint32]$Contract.expected_map_id -ne $script:RaidReadinessOnyxiaMapId) {
        return New-RaidReadinessBlockedCompletionCreditGate -Contract $Contract `
            -Reasons @('authoritative_onyxia_gate_not_applicable_to_expected_map') `
            -Source 'map_249_only'
    }

    try {
        $connections = Get-RaidReadinessDatabaseConnections -ServerRoot $ServerRoot `
            -WorldServerConfigPath $WorldServerConfigPath `
            -CharactersDatabaseName $CharactersDatabaseName -WorldDatabaseName $WorldDatabaseName
        $mysql = Get-RaidReadinessMySqlPath -ServerRoot $ServerRoot -MySqlPath $MySqlPath
        $queries = New-RaidReadinessOnyxiaReadOnlyQuerySet -Contract $Contract `
            -CharactersDatabaseName $CharactersDatabaseName -WorldDatabaseName $WorldDatabaseName

        $bindings = @(Invoke-RaidReadinessReadOnlySql -Connection $connections.characters `
            -Sql $queries.bindings.sql -Headers $queries.bindings.headers -MysqlPath $mysql)
        $instances = @(Invoke-RaidReadinessReadOnlySql -Connection $connections.characters `
            -Sql $queries.instances.sql -Headers $queries.instances.headers -MysqlPath $mysql)
        $encounterBits = @(Invoke-RaidReadinessReadOnlySql -Connection $connections.world `
            -Sql $queries.encounter_bits.sql -Headers $queries.encounter_bits.headers -MysqlPath $mysql)

        $gate = Test-RaidReadinessOnyxiaCompletionCredit -Contract $Contract `
            -BindingRows $bindings -InstanceRows $instances -EncounterRows $encounterBits
        Add-Member -InputObject $gate -MemberType NoteProperty -Name 'database_query_contract' -Value $queries -Force
        return $gate
    } catch {
        return New-RaidReadinessBlockedCompletionCreditGate -Contract $Contract `
            -Reasons @('authoritative_read_only_database_evidence_failed') `
            -Source 'read_only_database_query' -Detail $_.Exception.Message
    }
}

function New-RaidReadinessPinnedEncounterReadOnlyQuerySet {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][object]$Contract,
        [Parameter(Mandatory)][uint32]$EncounterId,
        [Parameter(Mandatory)][uint32]$CreditEntry,
        [Parameter(Mandatory)][ValidateRange(0, 31)][int]$EncounterBit,
        [Parameter(Mandatory)][ValidateRange(0, 1)][int]$ExpectedDifficulty,
        [string]$CharactersDatabaseName = 'acore_characters',
        [string]$WorldDatabaseName = 'acore_world'
    )

    Assert-RaidReadinessSafeDatabaseIdentifier -Name $CharactersDatabaseName
    Assert-RaidReadinessSafeDatabaseIdentifier -Name $WorldDatabaseName
    $guids = @($Contract.roster_guids | ForEach-Object { [uint32]$_ } | Sort-Object -Unique)
    if ($guids.Count -eq 0) { throw 'The pinned encounter query set requires a non-empty roster.' }
    $guidList = $guids -join ','
    $expectedInstanceId = [uint32]$Contract.expected_instance_id
    $expectedMapId = [uint32]$Contract.expected_map_id

    return [pscustomobject][ordered]@{
        read_only = $true
        database_mutations = 0
        bindings = [pscustomobject][ordered]@{
            sql = "SELECT ci.guid AS guid, ci.instance AS instance_id FROM ${CharactersDatabaseName}.character_instance ci WHERE ci.guid IN ($guidList) OR ci.instance = $expectedInstanceId ORDER BY ci.guid, ci.instance"
            headers = @('guid', 'instance_id')
            source = "${CharactersDatabaseName}.character_instance"
        }
        instances = [pscustomobject][ordered]@{
            sql = "SELECT i.id AS instance_id, i.map AS map_id, i.difficulty AS difficulty, i.completedEncounters AS completed_encounters FROM ${CharactersDatabaseName}.instance i WHERE i.id = $expectedInstanceId"
            headers = @('instance_id', 'map_id', 'difficulty', 'completed_encounters')
            source = "${CharactersDatabaseName}.instance"
        }
        encounter_bits = [pscustomobject][ordered]@{
            sql = "SELECT e.entry AS encounter_id, $expectedMapId AS map_id, $ExpectedDifficulty AS difficulty, $EncounterBit AS encounter_bit FROM ${WorldDatabaseName}.instance_encounters e WHERE e.entry = $EncounterId AND e.creditType = 0 AND e.creditEntry = $CreditEntry"
            headers = @('encounter_id', 'map_id', 'difficulty', 'encounter_bit')
            source = "pinned DungeonEncounter.dbc mapping + ${WorldDatabaseName}.instance_encounters"
        }
    }
}

function Test-RaidReadinessPinnedEncounterCompletionCredit {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][object]$Contract,
        [Parameter(Mandatory)][uint32]$EncounterId,
        [Parameter(Mandatory)][ValidateRange(0, 31)][int]$EncounterBit,
        [Parameter(Mandatory)][ValidateRange(0, 1)][int]$ExpectedDifficulty,
        [AllowNull()][object[]]$BindingRows = @(),
        [AllowNull()][object[]]$InstanceRows = @(),
        [AllowNull()][object[]]$EncounterRows = @()
    )

    $expectedRoster = @($Contract.roster_guids | ForEach-Object { [uint32]$_ } | Sort-Object -Unique)
    $expectedInstanceId = [uint32]$Contract.expected_instance_id
    $expectedMapId = [uint32]$Contract.expected_map_id
    $reasons = [System.Collections.Generic.List[string]]::new()

    $bindings = @($BindingRows | ForEach-Object {
        $guid = ConvertTo-RaidReadinessLong (Get-RaidReadinessProperty -Object $_ -Name @('guid', 'character_guid'))
        $instance = ConvertTo-RaidReadinessLong (Get-RaidReadinessProperty -Object $_ -Name @('instance_id', 'instance'))
        if ($null -ne $guid -and $null -ne $instance) { [pscustomobject]@{ guid = [uint32]$guid; instance_id = [long]$instance } }
    })
    $credited = @($bindings | Where-Object instance_id -eq $expectedInstanceId | ForEach-Object guid | Sort-Object -Unique)
    $missing = @($expectedRoster | Where-Object { $credited -notcontains $_ })
    $unexpected = @($credited | Where-Object { $expectedRoster -notcontains $_ })
    $bindingStatus = if ($bindings.Count -eq 0) { 'UNPROVEN' } elseif ($missing.Count -eq 0 -and $unexpected.Count -eq 0) { 'PASS' } else { 'FAIL' }
    foreach ($guid in $missing) { $reasons.Add("guid_${guid}_missing_expected_instance_binding") }
    foreach ($guid in $unexpected) { $reasons.Add("unexpected_guid_bound_to_expected_instance=$guid") }
    if ($bindings.Count -eq 0) { $reasons.Add('exact_roster_instance_binding_not_resolved') }

    $instances = @($InstanceRows | ForEach-Object {
        [pscustomobject]@{
            instance_id = ConvertTo-RaidReadinessLong (Get-RaidReadinessProperty -Object $_ -Name @('instance_id', 'id', 'instance'))
            map_id = ConvertTo-RaidReadinessLong (Get-RaidReadinessProperty -Object $_ -Name @('map_id', 'map'))
            difficulty = ConvertTo-RaidReadinessLong (Get-RaidReadinessProperty -Object $_ -Name @('difficulty', 'mode'))
            completed_encounters = ConvertTo-RaidReadinessLong (Get-RaidReadinessProperty -Object $_ -Name @('completed_encounters', 'completedEncounters', 'encounter_mask'))
        }
    })
    $instanceMatches = @($instances | Where-Object instance_id -eq $expectedInstanceId)
    $instanceStatus = 'UNPROVEN'
    $resolvedInstance = $null
    if ($instanceMatches.Count -eq 0) {
        $reasons.Add('shared_instance_row_not_resolved')
    } elseif ($instanceMatches.Count -ne 1) {
        $instanceStatus = 'FAIL'; $reasons.Add('shared_instance_row_ambiguous')
    } else {
        $resolvedInstance = $instanceMatches[0]
        if ([long]$resolvedInstance.map_id -ne $expectedMapId) {
            $instanceStatus = 'FAIL'; $reasons.Add("shared_instance_map_mismatch=$($resolvedInstance.map_id)")
        } elseif ([long]$resolvedInstance.difficulty -ne $ExpectedDifficulty) {
            $instanceStatus = 'FAIL'; $reasons.Add("shared_instance_difficulty_mismatch=$($resolvedInstance.difficulty)")
        } else { $instanceStatus = 'PASS' }
    }

    $encounters = @($EncounterRows | Where-Object {
        (ConvertTo-RaidReadinessLong (Get-RaidReadinessProperty -Object $_ -Name @('encounter_id', 'entry'))) -eq $EncounterId
    })
    $mappingStatus = if ($encounters.Count -eq 0) { 'UNPROVEN' } elseif ($encounters.Count -eq 1) { 'PASS' } else { 'FAIL' }
    if ($encounters.Count -eq 0) { $reasons.Add("encounter_mapping_not_resolved=$EncounterId") }
    if ($encounters.Count -gt 1) { $reasons.Add("encounter_mapping_ambiguous=$EncounterId") }

    $encounterMask = [uint64]1 -shl $EncounterBit
    $done = $null
    $doneStatus = 'UNPROVEN'
    if ($instanceStatus -eq 'PASS' -and $mappingStatus -eq 'PASS' -and $null -ne $resolvedInstance.completed_encounters) {
        $done = (([uint64][long]$resolvedInstance.completed_encounters -band $encounterMask) -ne 0)
        $doneStatus = if ($done) { 'PASS' } else { 'FAIL' }
        if (-not $done) { $reasons.Add("encounter_bit_not_set=$EncounterBit") }
    } elseif ($null -eq $resolvedInstance -or $null -eq $resolvedInstance.completed_encounters) {
        $reasons.Add('completed_encounters_mask_not_resolved')
    }

    $statuses = @($bindingStatus, $instanceStatus, $mappingStatus, $doneStatus)
    $overall = if ($statuses -contains 'FAIL') { 'FAIL' } elseif ($statuses -contains 'UNPROVEN') { 'BLOCKED' } else { 'PASS' }
    return [pscustomobject][ordered]@{
        schema = $script:RaidReadinessSchema
        status = $overall
        required_for_pass = $true
        applicable = $true
        authority = 'pinned DungeonEncounter.dbc mapping + acore_world.instance_encounters + acore_characters.instance.completedEncounters'
        read_only = $true
        database_mutations = 0
        target_damage_authoritative = $false
        exact_target_death_authoritative = ($overall -eq 'PASS' -and $done -eq $true -and $bindingStatus -eq 'PASS')
        expected_map_id = $expectedMapId
        expected_instance_id = $expectedInstanceId
        resolved_instance_id = if ($resolvedInstance) { $resolvedInstance.instance_id } else { $null }
        reasons = @($reasons)
        shared_instance = [pscustomobject][ordered]@{
            status = $instanceStatus; expected_instance_id = $expectedInstanceId
            resolved_instance_id = if ($resolvedInstance) { $resolvedInstance.instance_id } else { $null }
            map_id = if ($resolvedInstance) { $resolvedInstance.map_id } else { $null }
            difficulty = if ($resolvedInstance) { $resolvedInstance.difficulty } else { $null }
            expected_difficulty = $ExpectedDifficulty
            completed_encounters = if ($resolvedInstance) { $resolvedInstance.completed_encounters } else { $null }
            source = 'acore_characters.instance + acore_characters.character_instance'
        }
        encounter_done = [pscustomobject][ordered]@{
            status = $doneStatus; done = $done; encounter_id = $EncounterId; encounter_bit = $EncounterBit
            encounter_mask = $encounterMask
            completed_encounters = if ($resolvedInstance) { $resolvedInstance.completed_encounters } else { $null }
            mapping_status = $mappingStatus
            source = 'pinned DungeonEncounter.dbc bit + acore_world.instance_encounters + acore_characters.instance.completedEncounters'
        }
        exact_roster_credit = [pscustomobject][ordered]@{
            status = $bindingStatus; exact_match = if ($bindingStatus -eq 'PASS') { $true } elseif ($bindingStatus -eq 'FAIL') { $false } else { $null }
            expected_roster_guids = $expectedRoster; credited_roster_guids = if ($credited.Count) { $credited } else { $null }
            source = 'acore_characters.character_instance'
        }
    }
}

function Get-RaidReadinessPinnedEncounterCompletionCreditFromDatabase {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][object]$Contract,
        [Parameter(Mandatory)][string]$ServerRoot,
        [Parameter(Mandatory)][uint32]$EncounterId,
        [Parameter(Mandatory)][uint32]$CreditEntry,
        [Parameter(Mandatory)][ValidateRange(0, 31)][int]$EncounterBit,
        [Parameter(Mandatory)][ValidateRange(0, 1)][int]$ExpectedDifficulty,
        [string]$WorldServerConfigPath = '', [string]$MySqlPath = '',
        [string]$CharactersDatabaseName = 'acore_characters', [string]$WorldDatabaseName = 'acore_world'
    )

    try {
        $connections = Get-RaidReadinessDatabaseConnections -ServerRoot $ServerRoot -WorldServerConfigPath $WorldServerConfigPath `
            -CharactersDatabaseName $CharactersDatabaseName -WorldDatabaseName $WorldDatabaseName
        $mysql = Get-RaidReadinessMySqlPath -ServerRoot $ServerRoot -MySqlPath $MySqlPath
        $queries = New-RaidReadinessPinnedEncounterReadOnlyQuerySet -Contract $Contract -EncounterId $EncounterId `
            -CreditEntry $CreditEntry -EncounterBit $EncounterBit -ExpectedDifficulty $ExpectedDifficulty `
            -CharactersDatabaseName $CharactersDatabaseName -WorldDatabaseName $WorldDatabaseName
        $bindings = @(Invoke-RaidReadinessReadOnlySql -Connection $connections.characters -Sql $queries.bindings.sql -Headers $queries.bindings.headers -MysqlPath $mysql)
        $instances = @(Invoke-RaidReadinessReadOnlySql -Connection $connections.characters -Sql $queries.instances.sql -Headers $queries.instances.headers -MysqlPath $mysql)
        $encounters = @(Invoke-RaidReadinessReadOnlySql -Connection $connections.world -Sql $queries.encounter_bits.sql -Headers $queries.encounter_bits.headers -MysqlPath $mysql)
        $gate = Test-RaidReadinessPinnedEncounterCompletionCredit -Contract $Contract -EncounterId $EncounterId `
            -EncounterBit $EncounterBit -ExpectedDifficulty $ExpectedDifficulty -BindingRows $bindings `
            -InstanceRows $instances -EncounterRows $encounters
        Add-Member -InputObject $gate -MemberType NoteProperty -Name database_query_contract -Value $queries -Force
        return $gate
    } catch {
        return New-RaidReadinessBlockedCompletionCreditGate -Contract $Contract `
            -Reasons @('authoritative_read_only_database_evidence_failed') -Source 'read_only_database_query' -Detail $_.Exception.Message
    }
}

function Get-RaidReadinessPrimaryRole {
    [CmdletBinding()]
    param([AllowNull()][object]$Role)

    if ([bool](Get-RaidReadinessProperty -Object $Role -Name @('tank') -Default $false)) { return 'tank' }
    if ([bool](Get-RaidReadinessProperty -Object $Role -Name @('healer') -Default $false)) { return 'healer' }
    if ([bool](Get-RaidReadinessProperty -Object $Role -Name @('dps') -Default $false)) { return 'dps' }
    return 'unknown'
}

function New-RaidReadinessContract {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][ValidateSet(10, 25, 40)][int]$RaidSize,
        [Parameter(Mandatory)][uint32[]]$RosterGuid,
        [Parameter(Mandatory)][uint32[]]$TankGuid,
        [Parameter(Mandatory)][uint32[]]$HealerGuid,
        [uint32]$LeaderGuid = 0,
        [Parameter(Mandatory)][uint32]$TargetGuid,
        [Parameter(Mandatory)][uint32]$ExpectedMapId,
        [Parameter(Mandatory)][uint32]$ExpectedInstanceId,
        [string]$TargetName = 'raid-target'
    )

    $roster = @($RosterGuid | ForEach-Object { [uint32]$_ })
    $tanks = @($TankGuid | ForEach-Object { [uint32]$_ })
    $healers = @($HealerGuid | ForEach-Object { [uint32]$_ })
    if ($roster.Count -ne $RaidSize) { throw "RosterGuid must contain exactly $RaidSize GUIDs." }
    if (@($roster | Sort-Object -Unique).Count -ne $RaidSize -or @($roster | Where-Object { $_ -eq 0 }).Count -ne 0) {
        throw 'RosterGuid must contain unique positive GUIDs.'
    }
    if ($tanks.Count -eq 0 -or @($tanks | Sort-Object -Unique).Count -ne $tanks.Count -or @($tanks | Where-Object { $_ -eq 0 }).Count -ne 0) {
        throw 'TankGuid must contain at least one unique positive GUID.'
    }
    if ($healers.Count -eq 0 -or @($healers | Sort-Object -Unique).Count -ne $healers.Count -or @($healers | Where-Object { $_ -eq 0 }).Count -ne 0) {
        throw 'HealerGuid must contain at least one unique positive GUID.'
    }
    if (@($tanks | Where-Object { $roster -notcontains $_ }).Count -ne 0) { throw 'Every TankGuid must belong to RosterGuid.' }
    if (@($healers | Where-Object { $roster -notcontains $_ }).Count -ne 0) { throw 'Every HealerGuid must belong to RosterGuid.' }
    if (@($tanks | Where-Object { $healers -contains $_ }).Count -ne 0) { throw 'TankGuid and HealerGuid must be disjoint.' }
    if ($TargetGuid -eq 0 -or $ExpectedMapId -eq 0 -or $ExpectedInstanceId -eq 0) {
        throw 'TargetGuid, ExpectedMapId, and ExpectedInstanceId must all be positive.'
    }
    if ($LeaderGuid -eq 0) { $LeaderGuid = $tanks[0] }
    if ($roster -notcontains $LeaderGuid) { throw 'LeaderGuid must belong to RosterGuid.' }

    $members = foreach ($guid in $roster) {
        $role = if ($tanks -contains $guid) { 'tank' } elseif ($healers -contains $guid) { 'healer' } else { 'dps' }
        [pscustomobject][ordered]@{ guid = [uint32]$guid; role = $role }
    }

    return [pscustomobject][ordered]@{
        raid_size = $RaidSize
        roster_guids = @($roster)
        tank_guids = @($tanks)
        healer_guids = @($healers)
        dps_guids = @($roster | Where-Object { $tanks -notcontains $_ -and $healers -notcontains $_ })
        leader_guid = [uint32]$LeaderGuid
        target_guid = [uint32]$TargetGuid
        target_name = $TargetName
        expected_map_id = [uint32]$ExpectedMapId
        expected_instance_id = [uint32]$ExpectedInstanceId
        members = @($members)
    }
}

function Get-RaidReadinessDictionaryValue {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][System.Collections.IDictionary]$Dictionary,
        [Parameter(Mandatory)][uint32]$Guid
    )

    foreach ($key in @([string]$Guid, $Guid, [long]$Guid)) {
        if ($Dictionary.Contains($key)) { return $Dictionary[$key] }
    }
    return $null
}

function Get-RaidReadinessTargetObjects {
    [CmdletBinding()]
    param(
        [AllowNull()][object]$Combat,
        [Parameter(Mandatory)][uint32]$TargetGuid
    )

    $objects = New-Object System.Collections.Generic.List[object]
    foreach ($candidateName in @('victim', 'ai_target', 'target')) {
        $candidate = Get-RaidReadinessProperty -Object $Combat -Name @($candidateName)
        $guid = ConvertTo-RaidReadinessLong (Get-RaidReadinessProperty -Object $candidate -Name @('guid', 'unit_guid'))
        if ($null -ne $guid -and [uint32]$guid -eq $TargetGuid) { $objects.Add($candidate) }
    }
    $threat = Get-RaidReadinessProperty -Object $Combat -Name @('threat')
    foreach ($link in @(Get-RaidReadinessProperty -Object $threat -Name @('links') -Default @())) {
        $source = Get-RaidReadinessProperty -Object $link -Name @('source')
        $guid = ConvertTo-RaidReadinessLong (Get-RaidReadinessProperty -Object $source -Name @('guid', 'unit_guid'))
        if ($null -ne $guid -and [uint32]$guid -eq $TargetGuid) { $objects.Add($source) }
    }
    return $objects.ToArray()
}

function Resolve-RaidReadinessCombatResponses {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][System.Collections.IDictionary]$CombatResponseByGuid,
        [Parameter(Mandatory)][object]$Contract,
        [uint32[]]$ObservedDeadGuid = @()
    )

    $observedDead = [System.Collections.Generic.HashSet[uint32]]::new()
    foreach ($guid in @($ObservedDeadGuid)) { [void]$observedDead.Add([uint32]$guid) }
    $combat = [ordered]@{}
    $offlineDead = New-Object System.Collections.Generic.List[uint32]

    foreach ($guidValue in @($Contract.roster_guids)) {
        $guid = [uint32]$guidValue
        $response = Get-RaidReadinessDictionaryValue -Dictionary $CombatResponseByGuid -Guid $guid
        if ($null -eq $response) { throw "Bridge action combatlog returned no response for GUID ${guid}." }

        $ok = Get-RaidReadinessProperty -Object $response -Name @('ok')
        if ($null -ne $ok -and -not [bool]$ok) {
            $errorCode = [string](Get-RaidReadinessProperty -Object $response -Name @('error') -Default 'unknown_error')
            if ($errorCode -eq 'bot_not_online' -and $observedDead.Contains($guid)) {
                $offlineDead.Add($guid)
                continue
            }
            throw "Bridge action combatlog failed for GUID ${guid}: $errorCode"
        }

        $combat[[string]$guid] = Get-RaidReadinessProperty -Object $response -Name @('telemetry')
    }

    return [pscustomobject][ordered]@{
        combat_by_guid = $combat
        offline_dead_guids = @($offlineDead.ToArray() | Sort-Object -Unique)
    }
}

function ConvertTo-RaidReadinessCanonicalSample {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][object]$ListResponse,
        [Parameter(Mandatory)][System.Collections.IDictionary]$CombatByGuid,
        [Parameter(Mandatory)][object]$Contract,
        [uint32[]]$OfflineDeadGuid = @(),
        [datetime]$ObservedAtUtc = [datetime]::UtcNow
    )

    $onlineBots = @(Get-RaidReadinessProperty -Object $ListResponse -Name @('bots') -Default @())
    $members = New-Object System.Collections.Generic.List[object]
    $targetCandidates = New-Object System.Collections.Generic.List[object]
    $targetOwnerGuids = New-Object System.Collections.Generic.List[uint32]
    $hostileOwners = @{}
    $anyLinksTruncated = $false

    foreach ($expected in @($Contract.members)) {
        $guid = [uint32]$expected.guid
        $offlineDeadAttrition = $OfflineDeadGuid -contains $guid
        $botMatches = @($onlineBots | Where-Object { [uint32](Get-RaidReadinessProperty -Object $_ -Name @('guid') -Default 0) -eq $guid })
        $bot = if ($botMatches.Count -eq 1) { $botMatches[0] } else { $null }
        $combat = Get-RaidReadinessDictionaryValue -Dictionary $CombatByGuid -Guid $guid
        $position = Get-RaidReadinessProperty -Object $bot -Name @('position')
        $location = Get-RaidReadinessProperty -Object $combat -Name @('location')
        $group = Get-RaidReadinessProperty -Object $combat -Name @('group')
        if ($null -eq $group) { $group = Get-RaidReadinessProperty -Object $bot -Name @('group') }
        $role = Get-RaidReadinessProperty -Object $combat -Name @('role')
        $health = Get-RaidReadinessProperty -Object $combat -Name @('health')
        if ($null -eq $health) { $health = Get-RaidReadinessProperty -Object $bot -Name @('health') }
        $healer = Get-RaidReadinessProperty -Object $combat -Name @('healer')
        $healerTarget = Get-RaidReadinessProperty -Object $healer -Name @('target')
        $healerTargetHealth = Get-RaidReadinessProperty -Object $healerTarget -Name @('health')
        $threat = Get-RaidReadinessProperty -Object $combat -Name @('threat')
        $combatTarget = Get-RaidReadinessProperty -Object $combat -Name @('ai_target')
        if ($null -eq $combatTarget) { $combatTarget = Get-RaidReadinessProperty -Object $combat -Name @('victim') }
        $mana = Get-RaidReadinessProperty -Object $combat -Name @('mana')
        $linksTruncated = [bool](Get-RaidReadinessProperty -Object $threat -Name @('links_truncated') -Default $false)
        if ($linksTruncated) { $anyLinksTruncated = $true }

        foreach ($link in @(Get-RaidReadinessProperty -Object $threat -Name @('links') -Default @())) {
            $source = Get-RaidReadinessProperty -Object $link -Name @('source')
            $victim = Get-RaidReadinessProperty -Object $link -Name @('victim')
            $sourceGuid = ConvertTo-RaidReadinessLong (Get-RaidReadinessProperty -Object $source -Name @('guid'))
            $victimGuid = ConvertTo-RaidReadinessLong (Get-RaidReadinessProperty -Object $victim -Name @('guid'))
            if ($null -ne $sourceGuid -and $sourceGuid -gt 0 -and $null -ne $victimGuid -and $victimGuid -gt 0) {
                $hostileOwners[[string]$sourceGuid] = [uint32]$victimGuid
                if ([uint32]$sourceGuid -eq [uint32]$Contract.target_guid -and -not $targetOwnerGuids.Contains([uint32]$victimGuid)) {
                    $targetOwnerGuids.Add([uint32]$victimGuid)
                }
            }
        }

        foreach ($candidate in @(Get-RaidReadinessTargetObjects -Combat $combat -TargetGuid ([uint32]$Contract.target_guid))) {
            $targetCandidates.Add($candidate)
        }

        $recent = Get-RaidReadinessProperty -Object $combat -Name @('recent')
        $counters = Get-RaidReadinessProperty -Object $recent -Name @('counters')
        $interruptCount = $null
        foreach ($counterName in @('interrupts', 'interrupt_count', 'spell_interrupts')) {
            $candidate = ConvertTo-RaidReadinessLong (Get-RaidReadinessProperty -Object $counters -Name @($counterName))
            if ($null -ne $candidate) { $interruptCount = $candidate; break }
        }

        $combatMap = ConvertTo-RaidReadinessLong (Get-RaidReadinessProperty -Object $location -Name @('map_id', 'map'))
        $listMap = ConvertTo-RaidReadinessLong (Get-RaidReadinessProperty -Object $position -Name @('map', 'map_id'))
        $alive = if ($offlineDeadAttrition) {
            $false
        } else {
            [bool](Get-RaidReadinessProperty -Object $combat -Name @('alive') -Default (Get-RaidReadinessProperty -Object $bot -Name @('alive') -Default $false))
        }
        $member = [pscustomobject][ordered]@{
            guid = $guid
            name = [string](Get-RaidReadinessProperty -Object $combat -Name @('name') -Default (Get-RaidReadinessProperty -Object $bot -Name @('name') -Default ''))
            expected_role = [string]$expected.role
            observed_role = Get-RaidReadinessPrimaryRole -Role $role
            online_match_count = $botMatches.Count
            combat_telemetry_present = $null -ne $combat
            alive = $alive
            offline_dead_attrition = $offlineDeadAttrition
            attrition_observation = if ($offlineDeadAttrition) { 'combatlog_bot_not_online_after_observed_death' } elseif ($null -ne $combat -and -not $alive) { 'observed_dead' } else { $null }
            in_combat = [bool](Get-RaidReadinessProperty -Object $combat -Name @('in_combat', 'combat') -Default (Get-RaidReadinessProperty -Object $bot -Name @('combat') -Default $false))
            map_id = $combatMap
            list_map_id = $listMap
            instance_id = ConvertTo-RaidReadinessLong (Get-RaidReadinessProperty -Object $location -Name @('instance_id'))
            group_members = ConvertTo-RaidReadinessLong (Get-RaidReadinessProperty -Object $group -Name @('members', 'member_count'))
            leader_guid = ConvertTo-RaidReadinessLong (Get-RaidReadinessProperty -Object $group -Name @('leader_guid'))
            health_pct = ConvertTo-RaidReadinessDouble (Get-RaidReadinessProperty -Object $health -Name @('pct', 'health_pct'))
            position = [pscustomobject][ordered]@{
                x = ConvertTo-RaidReadinessDouble (Get-RaidReadinessProperty -Object $position -Name @('x'))
                y = ConvertTo-RaidReadinessDouble (Get-RaidReadinessProperty -Object $position -Name @('y'))
                z = ConvertTo-RaidReadinessDouble (Get-RaidReadinessProperty -Object $position -Name @('z'))
            }
            healer_target_guid = ConvertTo-RaidReadinessLong (Get-RaidReadinessProperty -Object $healerTarget -Name @('guid'))
            healer_target_health_pct = ConvertTo-RaidReadinessDouble (Get-RaidReadinessProperty -Object $healerTargetHealth -Name @('pct', 'health_pct'))
            mana_pct = ConvertTo-RaidReadinessDouble (Get-RaidReadinessProperty -Object $mana -Name @('pct', 'mana_pct'))
            current_target_guid = ConvertTo-RaidReadinessLong (Get-RaidReadinessProperty -Object $combatTarget -Name @('guid'))
            current_target_entry = ConvertTo-RaidReadinessLong (Get-RaidReadinessProperty -Object $combatTarget -Name @('entry'))
            current_target_name = [string](Get-RaidReadinessProperty -Object $combatTarget -Name @('name') -Default '')
            active_spell_id = ConvertTo-RaidReadinessLong (Get-RaidReadinessProperty -Object $recent -Name @('active_spell_id'))
            last_spell_id = ConvertTo-RaidReadinessLong (Get-RaidReadinessProperty -Object (Get-RaidReadinessProperty -Object $recent -Name @('last_spell')) -Name @('id'))
            threat_links_truncated = $linksTruncated
            interrupt_count = $interruptCount
        }
        $members.Add($member)
    }

    $healthCurrent = @()
    $healthMax = @()
    $healthPct = @()
    $targetAlive = @()
    foreach ($candidate in $targetCandidates) {
        $health = Get-RaidReadinessProperty -Object $candidate -Name @('health')
        $current = ConvertTo-RaidReadinessLong (Get-RaidReadinessProperty -Object $health -Name @('current'))
        $maximum = ConvertTo-RaidReadinessLong (Get-RaidReadinessProperty -Object $health -Name @('max'))
        $pct = ConvertTo-RaidReadinessDouble (Get-RaidReadinessProperty -Object $health -Name @('pct', 'health_pct'))
        if ($null -ne $current) { $healthCurrent += $current }
        if ($null -ne $maximum) { $healthMax += $maximum }
        if ($null -ne $pct) { $healthPct += $pct }
        $aliveValue = Get-RaidReadinessProperty -Object $candidate -Name @('alive')
        if ($null -ne $aliveValue) { $targetAlive += [bool]$aliveValue }
    }
    $targetOwnerUnique = @($targetOwnerGuids.ToArray() | Sort-Object -Unique)
    $hostileOwnerValues = @($hostileOwners.Values | ForEach-Object { [uint32]$_ })

    return [pscustomobject][ordered]@{
        observed_at_utc = $ObservedAtUtc.ToUniversalTime().ToString('o')
        members = $members.ToArray()
        target = [pscustomobject][ordered]@{
            observed = $targetCandidates.Count -gt 0
            guid = [uint32]$Contract.target_guid
            health_current = if ($healthCurrent.Count -gt 0) { [long]($healthCurrent | Measure-Object -Minimum).Minimum } else { $null }
            health_max = if ($healthMax.Count -gt 0) { [long]($healthMax | Measure-Object -Maximum).Maximum } else { $null }
            health_pct = if ($healthPct.Count -gt 0) { [double]($healthPct | Measure-Object -Minimum).Minimum } else { $null }
            alive = if ($targetAlive.Count -gt 0) { -not (@($targetAlive | Where-Object { -not $_ }).Count -gt 0) } else { $null }
        }
        target_threat_available = $targetOwnerUnique.Count -eq 1
        target_owner_guid = if ($targetOwnerUnique.Count -eq 1) { [uint32]$targetOwnerUnique[0] } else { $null }
        target_owner_candidates = @($targetOwnerUnique)
        hostile_owner_events = $hostileOwnerValues.Count
        tank_hostile_owner_events = @($hostileOwnerValues | Where-Object { $Contract.tank_guids -contains $_ }).Count
        threat_links_truncated = $anyLinksTruncated
    }
}

function ConvertTo-RaidReadinessCanonicalBridgeSample {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][object]$ListResponse,
        [Parameter(Mandatory)][System.Collections.IDictionary]$CombatResponseByGuid,
        [Parameter(Mandatory)][object]$Contract,
        [uint32[]]$ObservedDeadGuid = @(),
        [datetime]$ObservedAtUtc = [datetime]::UtcNow
    )

    $resolved = Resolve-RaidReadinessCombatResponses -CombatResponseByGuid $CombatResponseByGuid `
        -Contract $Contract -ObservedDeadGuid $ObservedDeadGuid
    return ConvertTo-RaidReadinessCanonicalSample -ListResponse $ListResponse `
        -CombatByGuid $resolved.combat_by_guid -Contract $Contract `
        -OfflineDeadGuid $resolved.offline_dead_guids -ObservedAtUtc $ObservedAtUtc
}

function Test-RaidReadinessRosterGuard {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][object]$Sample,
        [Parameter(Mandatory)][object]$Contract,
        [switch]$AllowReleasedDead
    )

    $reasons = New-Object System.Collections.Generic.List[string]
    $attritionGuids = New-Object System.Collections.Generic.List[uint32]
    $offlineDeadGuids = New-Object System.Collections.Generic.List[uint32]
    $members = @($Sample.members)
    if ($members.Count -ne [int]$Contract.raid_size) { $reasons.Add("roster_count=$($members.Count)") }
    $observedGuids = @($members | ForEach-Object { [uint32]$_.guid } | Sort-Object)
    if (($observedGuids -join ',') -ne (@($Contract.roster_guids | Sort-Object) -join ',')) { $reasons.Add('roster_guid_set_mismatch') }
    foreach ($member in $members) {
        $offlineDeadAttrition = -not [bool]$member.alive -and [bool](Get-RaidReadinessProperty -Object $member -Name @('offline_dead_attrition') -Default $false)
        $releasedDead = [bool]$AllowReleasedDead -and -not [bool]$member.alive -and `
            ([bool]$member.combat_telemetry_present -or $offlineDeadAttrition)
        $releasedOfflineDead = $releasedDead -and $offlineDeadAttrition
        if ($releasedDead) { $attritionGuids.Add([uint32]$member.guid) }
        if ($releasedOfflineDead) { $offlineDeadGuids.Add([uint32]$member.guid) }
        if (-not $releasedOfflineDead -and [int]$member.online_match_count -ne 1) { $reasons.Add("guid_$($member.guid)_online_match_count=$($member.online_match_count)") }
        if (-not $releasedOfflineDead -and -not [bool]$member.combat_telemetry_present) { $reasons.Add("guid_$($member.guid)_combat_telemetry_missing") }
        if (-not $releasedOfflineDead -and [string]$member.observed_role -ne [string]$member.expected_role) { $reasons.Add("guid_$($member.guid)_role=$($member.observed_role)_expected=$($member.expected_role)") }
        if (-not $releasedDead -and ($null -eq $member.map_id -or [long]$member.map_id -ne [long]$Contract.expected_map_id)) { $reasons.Add("guid_$($member.guid)_map=$($member.map_id)") }
        if (-not $releasedDead -and ($null -eq $member.list_map_id -or [long]$member.list_map_id -ne [long]$Contract.expected_map_id)) { $reasons.Add("guid_$($member.guid)_list_map=$($member.list_map_id)") }
        if (-not $releasedDead -and ($null -eq $member.instance_id -or [long]$member.instance_id -ne [long]$Contract.expected_instance_id)) { $reasons.Add("guid_$($member.guid)_instance=$($member.instance_id)") }
        if (-not $releasedOfflineDead -and ($null -eq $member.group_members -or [long]$member.group_members -ne [long]$Contract.raid_size)) { $reasons.Add("guid_$($member.guid)_group_members=$($member.group_members)") }
        if (-not $releasedOfflineDead -and ($null -eq $member.leader_guid -or [long]$member.leader_guid -ne [long]$Contract.leader_guid)) { $reasons.Add("guid_$($member.guid)_leader=$($member.leader_guid)") }
    }
    return [pscustomobject][ordered]@{
        status = if ($reasons.Count -eq 0) { 'PASS' } else { 'FAIL_CLOSED' }
        passed = $reasons.Count -eq 0
        reasons = @($reasons)
        expected_roster_count = [int]$Contract.raid_size
        observed_roster_count = $members.Count
        attrition_guids = @($attritionGuids.ToArray() | Sort-Object -Unique)
        offline_dead_guids = @($offlineDeadGuids.ToArray() | Sort-Object -Unique)
    }
}

function Get-RaidReadinessDistance {
    [CmdletBinding()]
    param([Parameter(Mandatory)][object]$Left, [Parameter(Mandatory)][object]$Right)

    foreach ($axis in @('x', 'y', 'z')) {
        if ($null -eq $Left.$axis -or $null -eq $Right.$axis) { return $null }
    }
    return [Math]::Sqrt(
        [Math]::Pow([double]$Left.x - [double]$Right.x, 2) +
        [Math]::Pow([double]$Left.y - [double]$Right.y, 2) +
        [Math]::Pow([double]$Left.z - [double]$Right.z, 2))
}

function Measure-RaidReadinessSamples {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][object[]]$Samples,
        [Parameter(Mandatory)][object]$Contract,
        [ValidateRange(0.0, 1.0)][double]$MinimumTankThreatRatio = 0.90,
        [ValidateRange(0.0, 1.0)][double]$MinimumHealerTankPriorityRatio = 0.50,
        [ValidateRange(0.0, 1.0)][double]$MinimumNonTankTriageRatio = 0.50,
        [ValidateRange(1.0, 100.0)][double]$TriageHealthPct = 65.0,
        [ValidateRange(1.0, 500.0)][double]$CohesionRadius = 60.0,
        [ValidateRange(0.0, 1.0)][double]$MinimumCohesionRatio = 0.80,
        [ValidateRange(1.0, 5000.0)][double]$TeleportStepThreshold = 100.0,
        [ValidateRange(5.0, 500.0)][double]$MaximumSuspiciousVerticalDrop = 25.0,
        [ValidateRange(0, 40)][int]$MaximumAllowedDeaths = 0,
        [AllowNull()][object]$CompletionCreditGate = $null,
        [bool]$RosterGuardMaintained = $true,
        [string[]]$RosterGuardReasons = @()
    )

    $orderedSamples = @($Samples | Sort-Object { [datetime]$_.observed_at_utc })
    $targetOwnerSamples = 0
    $tankTargetOwnerSamples = 0
    $hostileOwnerEvents = 0
    $tankHostileOwnerEvents = 0
    $healerSelections = 0
    $healerTankSelections = 0
    $triageOpportunities = 0
    $triageHits = 0
    $deathTransitions = 0
    $deadGuids = [System.Collections.Generic.HashSet[uint32]]::new()
    $wipeSamples = 0
    $cohesionSamples = 0
    $cohesionWithin = 0
    $maxCohesionRadius = 0.0
    $movementByGuid = @{}
    $maxStep = 0.0
    $teleportEvents = New-Object System.Collections.Generic.List[object]
    $maximumEncounterZByGuid = @{}
    $verticalDropEventGuids = [System.Collections.Generic.HashSet[uint32]]::new()
    $verticalDropEvents = New-Object System.Collections.Generic.List[object]
    $verticalSafetySamples = 0
    $verticalAnomalySamples = 0
    $previousAlive = @{}
    $previousPosition = @{}
    $interruptFirst = @{}
    $interruptLast = @{}
    $interruptMembers = [System.Collections.Generic.HashSet[uint32]]::new()
    $targetHealthSamples = New-Object System.Collections.Generic.List[object]
    $encounterSamples = New-Object System.Collections.Generic.List[object]
    $exactTargetObservationSamples = 0
    $mismatchedTargetGuidSamples = 0
    $targetAliveEvidenceSamples = 0
    $targetDeathConfirmed = $false

    foreach ($sample in $orderedSamples) {
        $members = @($sample.members)
        $sampleHasVerticalAnomaly = $false
        $anyCombat = @($members | Where-Object { [bool]$_.in_combat }).Count -gt 0
        $targetObserved = [bool](Get-RaidReadinessProperty -Object $sample.target -Name @('observed') -Default $false)
        $sampleTargetGuid = ConvertTo-RaidReadinessLong (Get-RaidReadinessProperty -Object $sample.target -Name @('guid'))
        $exactTargetObserved = $targetObserved -and $null -ne $sampleTargetGuid -and [uint32]$sampleTargetGuid -eq [uint32]$Contract.target_guid
        if ($anyCombat -or $targetObserved) { $encounterSamples.Add($sample) }
        if ($targetObserved -and -not $exactTargetObserved) { $mismatchedTargetGuidSamples++ }
        if ($exactTargetObserved) {
            $exactTargetObservationSamples++
            $targetAlive = Get-RaidReadinessProperty -Object $sample.target -Name @('alive')
            if ($null -ne $targetAlive) {
                $targetAliveEvidenceSamples++
                if (-not [bool]$targetAlive) { $targetDeathConfirmed = $true }
            }
        }
        if ($exactTargetObserved -and $null -ne $sample.target.health_current) {
            $targetHealthSamples.Add([pscustomobject]@{ observed_at_utc = $sample.observed_at_utc; current = [long]$sample.target.health_current; max = $sample.target.health_max; pct = $sample.target.health_pct; alive = $targetAlive })
        }
        if ([bool]$sample.target_threat_available -and $null -ne $sample.target_owner_guid) {
            $targetOwnerSamples++
            if ($Contract.tank_guids -contains [uint32]$sample.target_owner_guid) { $tankTargetOwnerSamples++ }
        }
        $hostileOwnerEvents += [int](Get-RaidReadinessProperty -Object $sample -Name @('hostile_owner_events') -Default 0)
        $tankHostileOwnerEvents += [int](Get-RaidReadinessProperty -Object $sample -Name @('tank_hostile_owner_events') -Default 0)

        $aliveMembers = @($members | Where-Object { [bool]$_.alive })
        if ($members.Count -eq [int]$Contract.raid_size -and $aliveMembers.Count -eq 0) { $wipeSamples++ }

        foreach ($member in $members) {
            $guid = [uint32]$member.guid
            $alive = [bool]$member.alive
            if ($previousAlive.ContainsKey($guid) -and [bool]$previousAlive[$guid] -and -not $alive) { $deathTransitions++ }
            if (-not $alive) { [void]$deadGuids.Add($guid) }
            $previousAlive[$guid] = $alive

            if ([string]$member.expected_role -eq 'healer' -and [bool]$member.in_combat -and $null -ne $member.healer_target_guid) {
                $healerSelections++
                if ($Contract.tank_guids -contains [uint32]$member.healer_target_guid) { $healerTankSelections++ }
            }

            $position = $member.position
            if ($alive -and $null -ne $position -and $null -ne $position.x -and $null -ne $position.y -and $null -ne $position.z) {
                if ($previousPosition.ContainsKey($guid)) {
                    $step = Get-RaidReadinessDistance -Left $previousPosition[$guid] -Right $position
                    if ($null -ne $step) {
                        if (-not $movementByGuid.ContainsKey($guid)) { $movementByGuid[$guid] = 0.0 }
                        $movementByGuid[$guid] = [double]$movementByGuid[$guid] + $step
                        if ($step -gt $maxStep) { $maxStep = $step }
                        if ($step -gt $TeleportStepThreshold) {
                            $teleportEvents.Add([pscustomobject]@{ guid = $guid; observed_at_utc = $sample.observed_at_utc; step_distance = [Math]::Round($step, 3) })
                        }
                    }
                }
                $previousPosition[$guid] = [pscustomobject]@{ x = [double]$position.x; y = [double]$position.y; z = [double]$position.z }

                $memberMapId = ConvertTo-RaidReadinessLong (Get-RaidReadinessProperty -Object $member -Name @('map_id'))
                $memberInstanceId = ConvertTo-RaidReadinessLong (Get-RaidReadinessProperty -Object $member -Name @('instance_id'))
                if ([bool]$member.in_combat -and $null -ne $memberMapId -and $null -ne $memberInstanceId -and
                    [uint32]$memberMapId -eq [uint32]$Contract.expected_map_id -and
                    [uint32]$memberInstanceId -eq [uint32]$Contract.expected_instance_id) {
                    $verticalSafetySamples++
                    $currentZ = [double]$position.z
                    if (-not $maximumEncounterZByGuid.ContainsKey($guid)) {
                        $maximumEncounterZByGuid[$guid] = $currentZ
                    } else {
                        $maximumZ = [double]$maximumEncounterZByGuid[$guid]
                        $verticalDrop = $maximumZ - $currentZ
                        if ($verticalDrop -gt $MaximumSuspiciousVerticalDrop) {
                            $sampleHasVerticalAnomaly = $true
                            if ($verticalDropEventGuids.Add($guid)) {
                                $verticalDropEvents.Add([pscustomobject]@{
                                    guid = $guid
                                    name = [string]$member.name
                                    observed_at_utc = ([datetime]$sample.observed_at_utc).ToUniversalTime().ToString('o')
                                    x = [Math]::Round([double]$position.x, 3)
                                    y = [Math]::Round([double]$position.y, 3)
                                    z = [Math]::Round($currentZ, 3)
                                    maximum_prior_z = [Math]::Round($maximumZ, 3)
                                    cumulative_vertical_drop = [Math]::Round($verticalDrop, 3)
                                })
                            }
                        }
                        if ($currentZ -gt $maximumZ) { $maximumEncounterZByGuid[$guid] = $currentZ }
                    }
                }
            }

            if ($null -ne $member.interrupt_count) {
                [void]$interruptMembers.Add($guid)
                if (-not $interruptFirst.ContainsKey($guid)) { $interruptFirst[$guid] = [long]$member.interrupt_count }
                $interruptLast[$guid] = [long]$member.interrupt_count
            }
        }
        if ($sampleHasVerticalAnomaly) { $verticalAnomalySamples++ }

        $activeHealers = @($members | Where-Object {
            [string]$_.expected_role -eq 'healer' -and [bool]$_.alive -and [bool]$_.in_combat
        })
        $lowNonTanks = @($members | Where-Object {
            $candidate = $_
            if ([string]$candidate.expected_role -eq 'tank' -or -not [bool]$candidate.alive -or
                $null -eq $candidate.health_pct -or [double]$candidate.health_pct -ge $TriageHealthPct) {
                return $false
            }
            return @($activeHealers | Where-Object {
                $distance = Get-RaidReadinessDistance -Left $_.position -Right $candidate.position
                $null -ne $distance -and $distance -lt 77.0
            }).Count -gt 0
        })
        if ($lowNonTanks.Count -gt 0) {
            $triageOpportunities++
            if (@($activeHealers | Where-Object {
                $healer = $_
                $null -ne $healer.healer_target_guid -and
                @($lowNonTanks | Where-Object {
                    [uint32]$_.guid -eq [uint32]$healer.healer_target_guid
                }).Count -gt 0
            }).Count -gt 0) {
                $triageHits++
            }
        }

        $positioned = @($aliveMembers | Where-Object { $null -ne $_.position -and $null -ne $_.position.x -and $null -ne $_.position.y -and $null -ne $_.position.z })
        if ($positioned.Count -eq $aliveMembers.Count -and $positioned.Count -gt 0) {
            $centroid = [pscustomobject]@{
                x = [double](($positioned.position.x | Measure-Object -Average).Average)
                y = [double](($positioned.position.y | Measure-Object -Average).Average)
                z = [double](($positioned.position.z | Measure-Object -Average).Average)
            }
            $radius = 0.0
            foreach ($member in $positioned) {
                $distance = Get-RaidReadinessDistance -Left $member.position -Right $centroid
                if ($null -ne $distance -and $distance -gt $radius) { $radius = $distance }
            }
            $cohesionSamples++
            if ($radius -le $CohesionRadius) { $cohesionWithin++ }
            if ($radius -gt $maxCohesionRadius) { $maxCohesionRadius = $radius }
        }
    }

    $tankThreatRatio = if ($targetOwnerSamples -gt 0) { [double]$tankTargetOwnerSamples / $targetOwnerSamples } else { 0.0 }
    $allHostileTankRatio = if ($hostileOwnerEvents -gt 0) { [double]$tankHostileOwnerEvents / $hostileOwnerEvents } else { 0.0 }
    $healerTankRatio = if ($healerSelections -gt 0) { [double]$healerTankSelections / $healerSelections } else { 0.0 }
    $triageRatio = if ($triageOpportunities -gt 0) { [double]$triageHits / $triageOpportunities } else { 0.0 }
    $cohesionRatio = if ($cohesionSamples -gt 0) { [double]$cohesionWithin / $cohesionSamples } else { 0.0 }

    $targetStart = $null
    $targetLowest = $null
    if ($targetHealthSamples.Count -gt 0) {
        $targetStart = $targetHealthSamples[0]
        $targetLowest = $targetHealthSamples[0]
        foreach ($targetHealthSample in $targetHealthSamples) {
            if ([long]$targetHealthSample.current -lt [long]$targetLowest.current) { $targetLowest = $targetHealthSample }
        }
    }
    $targetDelta = if ($null -ne $targetStart -and $null -ne $targetLowest) { [Math]::Max(0L, [long]$targetStart.current - [long]$targetLowest.current) } else { 0L }
    $targetDeltaPct = if ($null -ne $targetStart -and [long]$targetStart.current -gt 0) { 100.0 * $targetDelta / [long]$targetStart.current } else { 0.0 }
    $targetStartingHealth = $null
    $targetLowestHealth = $null
    if ($null -ne $targetStart) { $targetStartingHealth = [long]$targetStart.current }
    if ($null -ne $targetLowest) { $targetLowestHealth = [long]$targetLowest.current }
    $interruptDelta = 0L
    foreach ($guid in @($interruptMembers)) {
        $interruptDelta += [Math]::Max(0L, [long]$interruptLast[$guid] - [long]$interruptFirst[$guid])
    }
    $interruptStatus = if ($interruptMembers.Count -eq 0) { 'UNSUPPORTED' } elseif ($interruptMembers.Count -eq [int]$Contract.raid_size) { 'MEASURED' } else { 'PARTIAL' }

    $durationSeconds = 0.0
    $encounterFirstUtc = $null
    $encounterLastUtc = $null
    if ($encounterSamples.Count -gt 1) {
        $durationSeconds = ([datetime]$encounterSamples[$encounterSamples.Count - 1].observed_at_utc - [datetime]$encounterSamples[0].observed_at_utc).TotalSeconds
    }
    if ($encounterSamples.Count -gt 0) {
        $encounterFirstUtc = [string]$encounterSamples[0].observed_at_utc
        $encounterLastUtc = [string]$encounterSamples[$encounterSamples.Count - 1].observed_at_utc
    }
    $threatStatus = if ($targetOwnerSamples -eq 0) { 'UNSUPPORTED' } elseif ($tankThreatRatio -ge $MinimumTankThreatRatio) { 'PASS' } else { 'FAIL' }
    $healerStatus = if ($healerSelections -eq 0) { 'NOT_OBSERVED' } elseif ($healerTankRatio -ge $MinimumHealerTankPriorityRatio) { 'PASS' } else { 'FAIL' }
    $triageStatus = if ($triageOpportunities -eq 0) { 'NOT_OBSERVED' } elseif ($triageRatio -ge $MinimumNonTankTriageRatio) { 'PASS' } else { 'FAIL' }
    $cohesionStatus = if ($cohesionSamples -eq 0) { 'UNSUPPORTED' } elseif ($cohesionRatio -ge $MinimumCohesionRatio -and $teleportEvents.Count -eq 0) { 'PASS' } else { 'FAIL' }
    $verticalSafetyStatus = if ($verticalSafetySamples -eq 0) { 'UNSUPPORTED' } elseif ($verticalDropEvents.Count -eq 0) { 'PASS' } else { 'FAIL' }
    $survivalStatus = if ($deadGuids.Count -le $MaximumAllowedDeaths -and $wipeSamples -eq 0) { 'PASS' } else { 'FAIL' }
    $completionCreditEvidence = if ($null -eq $CompletionCreditGate) {
        [pscustomobject][ordered]@{
            status = 'BLOCKED'
            required_for_pass = $true
            reasons = @(
                'encounter_done_not_exposed_by_read_only_telemetry',
                'exact_roster_credit_not_exposed_by_read_only_telemetry'
            )
            encounter_done = [pscustomobject]@{
                status = 'UNPROVEN'
                done = $null
                source = 'not_exposed_by_list_or_combatlog'
            }
            exact_roster_credit = [pscustomobject]@{
                status = 'UNPROVEN'
                exact_match = $null
                expected_roster_guids = @($Contract.roster_guids | ForEach-Object { [uint32]$_ })
                credited_roster_guids = $null
                source = 'not_exposed_by_list_or_combatlog'
            }
        }
    } else {
        $CompletionCreditGate
    }
    $completionCreditGateStatus = [string](Get-RaidReadinessProperty -Object $completionCreditEvidence -Name @('status') -Default 'BLOCKED')
    if ([string]::IsNullOrWhiteSpace($completionCreditGateStatus)) { $completionCreditGateStatus = 'BLOCKED' }
    $completionCreditReasons = @(
        Get-RaidReadinessProperty -Object $completionCreditEvidence -Name @('reasons') -Default @()
    )
    $authoritativeTargetDeath = $completionCreditGateStatus -eq 'PASS' -and
        [bool](Get-RaidReadinessProperty -Object $completionCreditEvidence -Name @('exact_target_death_authoritative') -Default $false)
    $effectiveTargetDeathConfirmed = $targetDeathConfirmed -or $authoritativeTargetDeath
    $targetStatus = if ($effectiveTargetDeathConfirmed) { 'PASS' } elseif ($targetAliveEvidenceSamples -gt 0) { 'FAIL' } else { 'UNPROVEN' }
    $requiredStatuses = @($threatStatus, $healerStatus, $triageStatus, $targetStatus, $cohesionStatus, $verticalSafetyStatus, $survivalStatus)
    $overall = if (-not $RosterGuardMaintained) {
        'FAIL_CLOSED'
    } elseif (@($requiredStatuses | Where-Object { $_ -eq 'FAIL' }).Count -gt 0) {
        'FAIL'
    } elseif ($completionCreditGateStatus -eq 'FAIL') {
        'FAIL'
    } elseif ($completionCreditGateStatus -ne 'PASS') {
        'BLOCKED'
    } elseif (@($requiredStatuses | Where-Object { $_ -in @('UNSUPPORTED', 'NOT_OBSERVED', 'UNPROVEN', 'PARTIAL') }).Count -gt 0) {
        'PARTIAL'
    } else {
        'PASS'
    }

    $movementRows = foreach ($guid in $Contract.roster_guids) {
        $distanceValue = 0.0
        if ($movementByGuid.ContainsKey([uint32]$guid)) { $distanceValue = [double]$movementByGuid[[uint32]$guid] }
        [pscustomobject]@{ guid = [uint32]$guid; distance = [Math]::Round($distanceValue, 3) }
    }
    $deadGuidRows = @($deadGuids | ForEach-Object { [uint32]$_ } | Sort-Object)
    $teleportRows = $teleportEvents.ToArray()

    return [pscustomobject][ordered]@{
        schema = $script:RaidReadinessSchema
        overall_status = $overall
        sample_count = $orderedSamples.Count
        roster_guard = [pscustomobject]@{ status = if ($RosterGuardMaintained) { 'PASS' } else { 'FAIL_CLOSED' }; maintained = $RosterGuardMaintained; reasons = @($RosterGuardReasons) }
        tank_threat = [pscustomobject]@{
            status = $threatStatus
            target_owner_samples = $targetOwnerSamples
            tank_owner_samples = $tankTargetOwnerSamples
            tank_ownership_ratio = [Math]::Round($tankThreatRatio, 4)
            required_ratio = $MinimumTankThreatRatio
            all_hostile_owner_events = $hostileOwnerEvents
            tank_all_hostile_owner_events = $tankHostileOwnerEvents
            tank_all_hostile_ratio = [Math]::Round($allHostileTankRatio, 4)
        }
        healer = [pscustomobject]@{
            tank_priority_status = $healerStatus
            selection_samples = $healerSelections
            tank_selections = $healerTankSelections
            tank_priority_ratio = [Math]::Round($healerTankRatio, 4)
            required_tank_priority_ratio = $MinimumHealerTankPriorityRatio
            non_tank_triage_status = $triageStatus
            non_tank_triage_opportunities = $triageOpportunities
            non_tank_triage_hits = $triageHits
            non_tank_triage_ratio = [Math]::Round($triageRatio, 4)
            required_non_tank_triage_ratio = $MinimumNonTankTriageRatio
            triage_health_pct = $TriageHealthPct
        }
        survival = [pscustomobject]@{
            status = $survivalStatus
            death_transitions = $deathTransitions
            dead_guids = $deadGuidRows
            unique_deaths = $deadGuids.Count
            maximum_allowed_deaths = $MaximumAllowedDeaths
            wipe_samples = $wipeSamples
        }
        encounter = [pscustomobject]@{
            status = if ($encounterSamples.Count -gt 0) { 'MEASURED' } else { 'NOT_OBSERVED' }
            duration_seconds = [Math]::Round($durationSeconds, 3)
            encounter_sample_count = $encounterSamples.Count
            first_observed_at_utc = $encounterFirstUtc
            last_observed_at_utc = $encounterLastUtc
        }
        encounter_completion_credit = $completionCreditEvidence
        target_health = [pscustomobject]@{
            status = $targetStatus
            target_guid = [uint32]$Contract.target_guid
            exact_target_observation_samples = $exactTargetObservationSamples
            mismatched_target_guid_samples = $mismatchedTargetGuidSamples
            observed_samples = $targetHealthSamples.Count
            starting_health = $targetStartingHealth
            lowest_observed_health = $targetLowestHealth
            health_delta = $targetDelta
            health_delta_pct_of_start = [Math]::Round($targetDeltaPct, 4)
            exact_target_death_required = $true
            alive_evidence_samples = $targetAliveEvidenceSamples
            death_confirmed = $effectiveTargetDeathConfirmed
            observed_death_confirmed = $targetDeathConfirmed
            authoritative_death_confirmed = $authoritativeTargetDeath
            death_evidence = if ($targetDeathConfirmed) {
                'exact_target_guid_observed_alive_false'
            } elseif ($authoritativeTargetDeath) {
                'exact_encounter_credit_entry_completed_for_exact_roster'
            } else {
                $null
            }
        }
        movement_cohesion = [pscustomobject]@{
            status = $cohesionStatus
            cohesion_samples = $cohesionSamples
            samples_within_radius = $cohesionWithin
            cohesion_ratio = [Math]::Round($cohesionRatio, 4)
            required_ratio = $MinimumCohesionRatio
            radius_threshold = $CohesionRadius
            maximum_observed_radius = [Math]::Round($maxCohesionRadius, 3)
            maximum_member_step = [Math]::Round($maxStep, 3)
            teleport_step_threshold = $TeleportStepThreshold
            potential_teleport_events = $teleportRows
            member_distance = @($movementRows)
        }
        vertical_safety = [pscustomobject]@{
            status = $verticalSafetyStatus
            observed_member_samples = $verticalSafetySamples
            anomaly_samples = $verticalAnomalySamples
            maximum_suspicious_vertical_drop = $MaximumSuspiciousVerticalDrop
            suspicious_vertical_drop_events = @($verticalDropEvents.ToArray())
            note = 'Flags a living in-combat member that descends farther than the configured threshold below its highest observed Z in the expected encounter instance.'
        }
        interrupts = [pscustomobject]@{
            status = $interruptStatus
            exposed_member_count = $interruptMembers.Count
            total_counter_delta = $interruptDelta
            note = if ($interruptStatus -eq 'UNSUPPORTED') { 'No interrupt counter is exposed; last_spell ids are not treated as interrupt proof.' } else { 'Delta of exposed cumulative interrupt counters.' }
        }
    }
}
