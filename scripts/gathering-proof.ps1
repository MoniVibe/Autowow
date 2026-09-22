<#
    Exact-roster gathering proof for the local AutoWoW bridge.

    Dry-run is the default. -Apply requires a versioned fixture roster, uses
    only SELECT statements for profession/inventory evidence, and sends only
    list/activate/deploy/snapshot requests through the existing loopback bridge.
    A pass also requires a matching bridge gather_route source/request/material/
    credit receipt and compatible item_template metadata. It never starts, stops,
    restarts, or rewrites configuration for the server.
#>
[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$FixtureRosterPath = '',
    [ValidateRange(5, 3600)][int]$DurationSeconds = 600,
    [ValidateRange(1, 60)][int]$PollSeconds = 5,
    [ValidateRange(5, 300)][int]$ActivationTimeoutSeconds = 45,
    [ValidateRange(1, 300)][int]$PersistenceTimeoutSeconds = 30,
    [ValidateRange(1, 60)][int]$PersistencePollSeconds = 2,
    [ValidateRange(2, 5)][int]$PersistenceStableReads = 2,
    [ValidateSet('127.0.0.1')][string]$BridgeHost = '127.0.0.1',
    [ValidateRange(1, 65535)][int]$BridgePort = 18787,
    [ValidateRange(1000, 120000)][int]$BridgeTimeoutMs = 5000,
    [string]$ControlScriptPath = (Join-Path $PSScriptRoot 'autowow-control.ps1'),
    [string]$OutputPath = '',
    [string]$CharactersDatabaseName = 'acore_characters',
    [string]$WorldDatabaseName = 'acore_world',
    [string]$LeagueDatabaseName = 'acore_playerbots',
    [string]$WorldServerConfigPath = '',
    [string]$PlayerbotsConfigPath = '',
    [string]$MySqlPath = '',
    [switch]$LibraryOnly,
    [switch]$Apply
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:SqlQueryCount = 0
$script:BridgeCallCount = 0
$script:GatheringProofSchema = 'autowow.gathering-proof.v2'
$script:GatheringSkills = [ordered]@{
    Herbalism = [ordered]@{ skill_id = 182; tool_ids = @() }
    Mining = [ordered]@{ skill_id = 186; tool_ids = @(756, 778, 1819, 1893, 1959, 2901, 9465, 20723, 40772, 40892, 40893) }
    Skinning = [ordered]@{ skill_id = 393; tool_ids = @(7005, 40772, 40893, 12709, 19901) }
}
$script:GatheringCreditEvidence = @(
    'loot_consumed',
    'skill_credit',
    'loot_opened',
    'interaction_attempted'
)

function Get-OptionalProperty {
    param(
        [AllowNull()][object]$InputObject,
        [Parameter(Mandatory)][string]$Name,
        [AllowNull()][object]$Default = $null
    )

    if ($null -eq $InputObject) { return $Default }
    if ($InputObject -is [System.Collections.IDictionary] -and $InputObject.Contains($Name)) {
        $dictionaryValue = $InputObject[$Name]
        if ($null -eq $dictionaryValue) { return $Default }
        return $dictionaryValue
    }
    $property = $InputObject.PSObject.Properties[$Name]
    if ($null -eq $property -or $null -eq $property.Value) { return $Default }
    return $property.Value
}

function ConvertTo-Boolean {
    param([AllowNull()][object]$Value, [bool]$Default = $false)

    if ($null -eq $Value) { return $Default }
    if ($Value -is [bool]) { return [bool]$Value }
    $text = "$Value".Trim().ToLowerInvariant()
    if ($text -in @('1', 'true', 'yes', 'on')) { return $true }
    if ($text -in @('0', 'false', 'no', 'off')) { return $false }
    return $Default
}

function ConvertTo-Int64OrZero {
    param([AllowNull()][object]$Value)

    if ($null -eq $Value -or [string]::IsNullOrWhiteSpace("$Value")) { return [int64]0 }
    $number = [int64]0
    if ([int64]::TryParse("$Value", [Globalization.NumberStyles]::Integer,
            [Globalization.CultureInfo]::InvariantCulture, [ref]$number)) {
        return $number
    }
    return [int64]0
}

function Get-CanonicalProfession {
    param([AllowNull()][object]$Value)

    switch ("$Value".Trim().ToLowerInvariant()) {
        'herbalism' { return 'Herbalism' }
        'mining' { return 'Mining' }
        'skinning' { return 'Skinning' }
        default { return $null }
    }
}

function Get-MaterialTemplateCategory {
    param([AllowNull()][object]$Material)

    $itemClass = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $Material -Name 'item_class')
    $itemSubclass = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $Material -Name 'item_subclass')

    # These are authoritative item_template class/subclass facts, not name heuristics.
    # Trade-goods subclass 8 is meat and is deliberately not accepted for any gathering
    # profession; this is the exact class-7 false-positive that motivated this contract.
    if ($itemClass -eq 7 -and $itemSubclass -eq 9) { return 'herb' }
    if ($itemClass -eq 7 -and $itemSubclass -eq 7) { return 'ore_or_stone' }
    if ($itemClass -eq 7 -and $itemSubclass -eq 6) { return 'skin_hide_leather' }
    if ($itemClass -eq 7 -and $itemSubclass -eq 8) { return 'meat' }
    if ($itemClass -eq 3) { return 'gem' }
    if ($itemClass -eq 7) { return 'trade_good_other' }
    return 'unknown'
}

function Test-MaterialCategoryForProfession {
    param(
        [Parameter(Mandatory)][string]$Profession,
        [Parameter(Mandatory)][string]$Category
    )

    switch ($Profession) {
        'Herbalism' {
            if ($Category -eq 'herb') { return $true }
            if ($Category -in @('unknown', 'trade_good_other')) { return $null }
            return $false
        }
        'Mining' {
            if ($Category -in @('ore_or_stone', 'gem')) { return $true }
            if ($Category -in @('unknown', 'trade_good_other')) { return $null }
            return $false
        }
        'Skinning' {
            if ($Category -eq 'skin_hide_leather') { return $true }
            if ($Category -in @('unknown', 'trade_good_other')) { return $null }
            return $false
        }
        default { return $false }
    }
}

function Test-GatherRouteCreditEvidence {
    param([Parameter(Mandatory)][object]$Route)

    $credit = "$(Get-OptionalProperty -InputObject $Route -Name 'credit_evidence' -Default '')".Trim().ToLowerInvariant()
    if ($credit -notin $script:GatheringCreditEvidence) { return $false }
    if ($credit -in @('loot_consumed', 'skill_credit')) { return $true }

    # Open/attempted are only sufficient when the bridge also recorded a packet,
    # loot, or skill-side event. The inventory delta remains a separate persistence
    # postcondition below.
    $eventDelta = [int64]0
    foreach ($field in @(
        'loot_response_delta',
        'store_loot_execution_delta',
        'autostore_loot_packet_delta',
        'loot_release_packet_delta',
        'loot_packet_item_delta',
        'loot_allowed_owner_slot_delta'
    )) {
        $eventDelta += [Math]::Max(0, (ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $Route -Name $field)))
    }
    $eventDelta += [Math]::Max(0,
        (ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $Route -Name 'skill_after')) -
        (ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $Route -Name 'skill_before')))
    return $eventDelta -gt 0
}

function Read-FixtureRoster {
    if ([string]::IsNullOrWhiteSpace($FixtureRosterPath)) {
        return [pscustomobject][ordered]@{
            supplied = $false
            path = $null
            allow_campaign_characters = $false
            members = @()
        }
    }

    if (-not (Test-Path -LiteralPath $FixtureRosterPath -PathType Leaf)) {
        throw "Fixture roster not found: $FixtureRosterPath"
    }

    $resolvedPath = [IO.Path]::GetFullPath($FixtureRosterPath)
    try {
        $json = Get-Content -LiteralPath $resolvedPath -Raw | ConvertFrom-Json
    } catch {
        throw "Fixture roster is not valid JSON: $($_.Exception.Message)"
    }

    $schema = "$(Get-OptionalProperty -InputObject $json -Name 'schema')"
    if ($schema -ne 'autowow.gathering-proof.fixture-roster.v1') {
        throw "Fixture roster schema must be 'autowow.gathering-proof.fixture-roster.v1'."
    }
    $purpose = "$(Get-OptionalProperty -InputObject $json -Name 'purpose')"
    if ($purpose -ne 'gathering-proof') {
        throw "Fixture roster purpose must be 'gathering-proof'."
    }

    $rawMembers = @(Get-OptionalProperty -InputObject $json -Name 'members' -Default @())
    if ($rawMembers.Count -lt 1 -or $rawMembers.Count -gt 10) {
        throw 'Fixture roster must contain between one and ten members.'
    }

    $seen = [System.Collections.Generic.HashSet[uint32]]::new()
    $members = [System.Collections.Generic.List[object]]::new()
    foreach ($member in $rawMembers) {
        $guidValue = Get-OptionalProperty -InputObject $member -Name 'guid' -Default (
            Get-OptionalProperty -InputObject $member -Name 'character_guid')
        $parsedGuid = [uint64]0
        if (-not [uint64]::TryParse("$guidValue", [Globalization.NumberStyles]::Integer,
                [Globalization.CultureInfo]::InvariantCulture, [ref]$parsedGuid) -or
            $parsedGuid -eq 0 -or $parsedGuid -gt [uint32]::MaxValue) {
            throw "Fixture roster member has an invalid positive uint32 guid: $guidValue"
        }
        $guid = [uint32]$parsedGuid
        if (-not $seen.Add($guid)) {
            throw "Fixture roster repeats guid $guid."
        }

        $profession = Get-CanonicalProfession (
            Get-OptionalProperty -InputObject $member -Name 'profession' -Default (
                Get-OptionalProperty -InputObject $member -Name 'gathering_profession'))
        if ($null -eq $profession) {
            throw "Fixture roster guid $guid must name Herbalism, Mining, or Skinning as profession."
        }

        [void]$members.Add([pscustomobject][ordered]@{
            guid = $guid
            expected_name = "$(Get-OptionalProperty -InputObject $member -Name 'name' -Default '')".Trim()
            profession = $profession
            skill_id = [int]$script:GatheringSkills[$profession].skill_id
        })
    }

    return [pscustomobject][ordered]@{
        supplied = $true
        path = $resolvedPath
        allow_campaign_characters = ConvertTo-Boolean (
            Get-OptionalProperty -InputObject $json -Name 'allow_campaign_characters')
        members = @($members)
    }
}

function Assert-SafeDatabaseIdentifier {
    param([Parameter(Mandatory)][string]$Name)

    if ($Name -notmatch '^[A-Za-z0-9_]+$') {
        throw "Unsafe database identifier: $Name"
    }
}

function Assert-ReadOnlySql {
    param([Parameter(Mandatory)][string]$Sql)

    $trimmed = $Sql.Trim()
    if ($trimmed -notmatch '^(?i:SELECT)\b') {
        throw 'Gathering proof accepts SELECT-only SQL.'
    }
    if ($trimmed.Contains(';')) {
        throw 'Gathering proof rejects SQL statement separators.'
    }
    if ($trimmed -match '(?i)\b(INSERT|UPDATE|DELETE|DROP|ALTER|CREATE|TRUNCATE|REPLACE|CALL|SET|GRANT|REVOKE|LOAD|LOCK|UNLOCK)\b') {
        throw 'Mutating SQL keyword rejected by gathering proof.'
    }
}

function Get-ConfigValue {
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

function ConvertTo-ConnectionInfo {
    param(
        [Parameter(Mandatory)][string]$Info,
        [Parameter(Mandatory)][string]$DatabaseOverride
    )

    $parts = @($Info.Trim().Trim('"').Split(';'))
    if ($parts.Count -lt 5) {
        throw 'DatabaseInfo must contain host;port;user;password;database.'
    }
    $port = ConvertTo-Int64OrZero $parts[1].Trim()
    if ($port -lt 1 -or $port -gt 65535) {
        throw "Invalid database port in DatabaseInfo: $($parts[1])"
    }
    Assert-SafeDatabaseIdentifier -Name $DatabaseOverride

    return [pscustomobject]@{
        Host = $parts[0].Trim()
        Port = [int]$port
        User = $parts[2].Trim()
        Password = $parts[3]
        Database = $DatabaseOverride
    }
}

function Get-MySqlClientPath {
    if (-not [string]::IsNullOrWhiteSpace($MySqlPath)) {
        if (-not (Test-Path -LiteralPath $MySqlPath -PathType Leaf)) {
            throw "Configured mysql client not found: $MySqlPath"
        }
        return [IO.Path]::GetFullPath($MySqlPath)
    }

    foreach ($candidate in @(
        (Join-Path $ServerRoot 'third_party\mysql\bin\mysql.exe'),
        (Join-Path $ServerRoot 'server\bin\mysql.exe'),
        'C:\Program Files\MySQL\MySQL Server 8.0\bin\mysql.exe',
        'C:\Program Files\MariaDB 11.0\bin\mysql.exe'
    )) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return [IO.Path]::GetFullPath($candidate)
        }
    }

    $command = Get-Command mysql.exe -ErrorAction SilentlyContinue
    if ($null -ne $command) { return $command.Source }
    throw 'mysql.exe was not found. Pass -MySqlPath or use the existing AutoWoW installation.'
}

function Get-DatabaseConnections {
    $worldConfig = if ([string]::IsNullOrWhiteSpace($WorldServerConfigPath)) {
        Join-Path $ServerRoot 'server\configs\worldserver.conf'
    } else { $WorldServerConfigPath }
    $playerbotsConfig = if ([string]::IsNullOrWhiteSpace($PlayerbotsConfigPath)) {
        Join-Path $ServerRoot 'server\configs\modules\playerbots.conf'
    } else { $PlayerbotsConfigPath }

    if (-not (Test-Path -LiteralPath $worldConfig -PathType Leaf)) {
        throw "Worldserver config not found: $worldConfig"
    }
    $worldText = Get-Content -LiteralPath $worldConfig -Raw
    $playerbotsText = if (Test-Path -LiteralPath $playerbotsConfig -PathType Leaf) {
        Get-Content -LiteralPath $playerbotsConfig -Raw
    } else { '' }

    $characterInfo = Get-ConfigValue -ConfigText $worldText -Keys @('CharacterDatabaseInfo')
    $worldInfo = Get-ConfigValue -ConfigText $worldText -Keys @('WorldDatabaseInfo')
    $leagueInfo = Get-ConfigValue -ConfigText $playerbotsText -Keys @(
        'PlayerbotsDatabaseInfo', 'PlayerbotDatabaseInfo', 'AiPlayerbot.DatabaseInfo', 'PlayerbotDatabase')
    if ([string]::IsNullOrWhiteSpace($leagueInfo)) { $leagueInfo = $worldInfo }
    if ([string]::IsNullOrWhiteSpace($characterInfo) -or
        [string]::IsNullOrWhiteSpace($worldInfo) -or
        [string]::IsNullOrWhiteSpace($leagueInfo)) {
        throw 'Could not resolve character, world, and Playerbots database connections from existing configs.'
    }

    return [ordered]@{
        characters = ConvertTo-ConnectionInfo -Info $characterInfo -DatabaseOverride $CharactersDatabaseName
        world = ConvertTo-ConnectionInfo -Info $worldInfo -DatabaseOverride $WorldDatabaseName
        league = ConvertTo-ConnectionInfo -Info $leagueInfo -DatabaseOverride $LeagueDatabaseName
    }
}

function ConvertFrom-TabularText {
    param([AllowNull()][string]$Text, [Parameter(Mandatory)][string[]]$Headers)

    $rows = [System.Collections.Generic.List[object]]::new()
    if ([string]::IsNullOrWhiteSpace($Text)) { return @($rows) }
    foreach ($line in ($Text -split "`r?`n")) {
        if ([string]::IsNullOrWhiteSpace($line) -or $line -match '^(?i:warning|mysql:)') { continue }
        $parts = $line.Split([char]9)
        $row = [ordered]@{}
        for ($index = 0; $index -lt $Headers.Count; $index++) {
            $row[$Headers[$index]] = if ($index -lt $parts.Count) { $parts[$index] } else { '' }
        }
        [void]$rows.Add([pscustomobject]$row)
    }
    return @($rows)
}

function Invoke-ReadOnlySql {
    param(
        [Parameter(Mandatory)][pscustomobject]$Connection,
        [Parameter(Mandatory)][string]$Sql,
        [Parameter(Mandatory)][string[]]$Headers,
        [Parameter(Mandatory)][string]$ClientPath
    )

    Assert-ReadOnlySql -Sql $Sql
    $script:SqlQueryCount++
    $oldPassword = [Environment]::GetEnvironmentVariable('MYSQL_PWD', 'Process')
    $hadPassword = $null -ne $oldPassword
    try {
        $env:MYSQL_PWD = $Connection.Password
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
        $output = & $ClientPath @arguments 2>&1 | ForEach-Object { "$($_)" } | Out-String
        if ($LASTEXITCODE -ne 0) {
            throw "Read-only SQL failed for database $($Connection.Database) with exit code $LASTEXITCODE."
        }
        return @(ConvertFrom-TabularText -Text $output -Headers $Headers)
    } finally {
        if ($hadPassword) { $env:MYSQL_PWD = $oldPassword }
        else { Remove-Item Env:MYSQL_PWD -ErrorAction SilentlyContinue }
    }
}

function Get-GuidList {
    param([Parameter(Mandatory)][object[]]$Members)
    return (@($Members | ForEach-Object { [uint32]$_.guid } | Sort-Object -Unique) -join ',')
}

function Get-TargetState {
    param(
        [Parameter(Mandatory)][object[]]$Members,
        [Parameter(Mandatory)][object]$Connections,
        [Parameter(Mandatory)][string]$ClientPath
    )

    Assert-SafeDatabaseIdentifier $CharactersDatabaseName
    Assert-SafeDatabaseIdentifier $LeagueDatabaseName
    $guids = Get-GuidList -Members $Members
    $sql = "SELECT c.guid, COALESCE(c.name,''), COALESCE(c.online,0), COALESCE(m.team_id,''), COALESCE(m.affiliation,''), COALESCE(m.role,''), CASE WHEN m.character_guid IS NOT NULL AND m.retired_at IS NULL THEN 1 ELSE 0 END, CASE WHEN COALESCE(m.team_id,'') IN ('northstar','ember','wayfarers') OR (COALESCE(m.team_id,'')='' AND COALESCE(m.affiliation,'')='wayfarer') THEN 1 ELSE 0 END FROM $CharactersDatabaseName.characters c LEFT JOIN $LeagueDatabaseName.autowow_league_member m ON m.character_guid=c.guid WHERE c.guid IN ($guids) ORDER BY c.guid"
    $rows = @(Invoke-ReadOnlySql -Connection $Connections.characters -Sql $sql -ClientPath $ClientPath -Headers @(
        'guid', 'name', 'online', 'team', 'affiliation', 'role', 'bridge_enrolled', 'campaign_character'))

    return @($rows | ForEach-Object {
        [pscustomobject][ordered]@{
            guid = [uint32](ConvertTo-Int64OrZero $_.guid)
            name = "$($_.name)"
            online = (ConvertTo-Int64OrZero $_.online) -eq 1
            team = "$($_.team)"
            affiliation = "$($_.affiliation)"
            role = "$($_.role)"
            bridge_enrolled = (ConvertTo-Int64OrZero $_.bridge_enrolled) -eq 1
            campaign_character = (ConvertTo-Int64OrZero $_.campaign_character) -eq 1
        }
    })
}

function Assert-TargetSafety {
    param(
        [Parameter(Mandatory)][object]$Roster,
        [Parameter(Mandatory)][object[]]$TargetState
    )

    $validated = [System.Collections.Generic.List[object]]::new()
    foreach ($member in $Roster.members) {
        $matches = @($TargetState | Where-Object { [uint32]$_.guid -eq [uint32]$member.guid })
        if ($matches.Count -ne 1) {
            throw "Fixture roster guid $($member.guid) did not resolve to exactly one character."
        }
        $state = $matches[0]
        if (-not [string]::IsNullOrWhiteSpace($member.expected_name) -and
            -not [string]::Equals($member.expected_name, $state.name, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Fixture roster guid $($member.guid) expected '$($member.expected_name)' but database name is '$($state.name)'."
        }
        if (-not $state.bridge_enrolled) {
            throw "Fixture roster guid $($member.guid) is not an unretired autowow_league_member; the current bridge cannot deploy it."
        }
        if ($state.campaign_character -and -not $Roster.allow_campaign_characters) {
            throw "Fixture roster guid $($member.guid) is a campaign character. Set allow_campaign_characters=true in the explicit fixture roster to authorize that exact GUID."
        }
        [void]$validated.Add([pscustomobject][ordered]@{
            guid = $member.guid
            expected_name = $member.expected_name
            name = $state.name
            profession = $member.profession
            skill_id = $member.skill_id
            team = $state.team
            affiliation = $state.affiliation
            role = $state.role
            campaign_character = $state.campaign_character
        })
    }
    return @($validated)
}

function New-ReadSnapshot {
    param(
        [Parameter(Mandatory)][object[]]$Members,
        [Parameter(Mandatory)][object]$Connections,
        [Parameter(Mandatory)][string]$ClientPath
    )

    Assert-SafeDatabaseIdentifier $CharactersDatabaseName
    Assert-SafeDatabaseIdentifier $WorldDatabaseName
    $guids = Get-GuidList -Members $Members
    $skillIds = @($script:GatheringSkills.Values | ForEach-Object { [int]$_.skill_id }) -join ','
    $toolIds = @($script:GatheringSkills.Values | ForEach-Object { @($_.tool_ids) } | Sort-Object -Unique) -join ','

    $skillSql = "SELECT guid, skill, value, max FROM $CharactersDatabaseName.character_skills WHERE guid IN ($guids) AND skill IN ($skillIds) ORDER BY guid, skill"
    $materialSql = "SELECT ci.guid, ii.itemEntry, COALESCE(it.name,''), COALESCE(it.class,0), COALESCE(it.subclass,0), COALESCE(SUM(ii.count),0) FROM $CharactersDatabaseName.character_inventory ci JOIN $CharactersDatabaseName.item_instance ii ON ii.guid=ci.item JOIN $WorldDatabaseName.item_template it ON it.entry=ii.itemEntry WHERE ci.guid IN ($guids) AND it.class IN (3,7) GROUP BY ci.guid, ii.itemEntry, it.name, it.class, it.subclass ORDER BY ci.guid, ii.itemEntry"
    $toolSql = "SELECT ci.guid, ii.itemEntry, COALESCE(SUM(ii.count),0) FROM $CharactersDatabaseName.character_inventory ci JOIN $CharactersDatabaseName.item_instance ii ON ii.guid=ci.item LEFT JOIN $CharactersDatabaseName.character_inventory carrier ON carrier.guid=ci.guid AND carrier.item=ci.bag WHERE ci.guid IN ($guids) AND ii.itemEntry IN ($toolIds) AND ((ci.bag=0 AND ci.slot<39) OR (ci.bag<>0 AND carrier.bag=0 AND carrier.slot>=19 AND carrier.slot<23)) GROUP BY ci.guid, ii.itemEntry ORDER BY ci.guid, ii.itemEntry"

    $skillRows = @(Invoke-ReadOnlySql -Connection $Connections.characters -Sql $skillSql -ClientPath $ClientPath -Headers @('guid', 'skill_id', 'value', 'max'))
    $materialRows = @(Invoke-ReadOnlySql -Connection $Connections.characters -Sql $materialSql -ClientPath $ClientPath -Headers @('guid', 'item_entry', 'item_name', 'item_class', 'item_subclass', 'count'))
    $toolRows = @(Invoke-ReadOnlySql -Connection $Connections.characters -Sql $toolSql -ClientPath $ClientPath -Headers @('guid', 'item_entry', 'count'))

    return [pscustomobject][ordered]@{
        collected_utc = [DateTimeOffset]::UtcNow.ToString('o')
        read_only = $true
        skills = @($skillRows | ForEach-Object {
            $skillId = [int](ConvertTo-Int64OrZero $_.skill_id)
            $name = @($script:GatheringSkills.Keys | Where-Object { [int]$script:GatheringSkills[$_].skill_id -eq $skillId } | Select-Object -First 1)
            [pscustomobject][ordered]@{
                guid = [uint32](ConvertTo-Int64OrZero $_.guid)
                skill_id = $skillId
                name = if ($name.Count -eq 1) { $name[0] } else { 'Unknown' }
                value = ConvertTo-Int64OrZero $_.value
                max = ConvertTo-Int64OrZero $_.max
            }
        })
        materials = @($materialRows | ForEach-Object {
            [pscustomobject][ordered]@{
                guid = [uint32](ConvertTo-Int64OrZero $_.guid)
                item_entry = ConvertTo-Int64OrZero $_.item_entry
                item_name = "$($_.item_name)"
                item_class = ConvertTo-Int64OrZero $_.item_class
                item_subclass = ConvertTo-Int64OrZero $_.item_subclass
                count = ConvertTo-Int64OrZero $_.count
                classification = 'item_template_class_subclass'
                material_category = Get-MaterialTemplateCategory -Material $_
                metadata_source = 'world.item_template'
            }
        })
        tools = @($toolRows | ForEach-Object {
            [pscustomobject][ordered]@{
                guid = [uint32](ConvertTo-Int64OrZero $_.guid)
                item_entry = ConvertTo-Int64OrZero $_.item_entry
                count = ConvertTo-Int64OrZero $_.count
            }
        })
    }
}

function Get-ToolReadiness {
    param([Parameter(Mandatory)][object]$Member, [Parameter(Mandatory)][object]$Snapshot)

    $requiredIds = @($script:GatheringSkills[$Member.profession].tool_ids)
    $found = @($Snapshot.tools | Where-Object {
        [uint32]$_.guid -eq [uint32]$Member.guid -and [int64]$_.count -gt 0 -and
        [int64]$_.item_entry -in $requiredIds
    } | ForEach-Object { [int64]$_.item_entry } | Sort-Object -Unique)
    return [pscustomobject][ordered]@{
        required = $requiredIds.Count -gt 0
        required_item_entries = $requiredIds
        found_item_entries = $found
        ready = ($requiredIds.Count -eq 0 -or $found.Count -gt 0)
    }
}

function Assert-BaselineReadiness {
    param([Parameter(Mandatory)][object[]]$Members, [Parameter(Mandatory)][object]$Snapshot)

    foreach ($member in $Members) {
        $skill = @($Snapshot.skills | Where-Object {
            [uint32]$_.guid -eq [uint32]$member.guid -and [int]$_.skill_id -eq [int]$member.skill_id
        })
        if ($skill.Count -ne 1 -or [int64]$skill[0].value -le 0) {
            throw "Fixture guid $($member.guid) does not have positive $($member.profession) skill in the baseline snapshot."
        }
        $tool = Get-ToolReadiness -Member $member -Snapshot $Snapshot
        if (-not $tool.ready) {
            throw "Fixture guid $($member.guid) lacks a required $($member.profession) tool. Expected one of: $($tool.required_item_entries -join ',')."
        }
    }
}

function Invoke-ControlJson {
    param(
        [Parameter(Mandatory)][ValidateSet('list', 'activate', 'deactivate', 'deploy', 'snapshot')][string]$Action,
        [uint32]$BotGuid = 0
    )

    if (-not (Test-Path -LiteralPath $ControlScriptPath -PathType Leaf)) {
        throw "AutoWoW control script not found: $ControlScriptPath"
    }
    $arguments = @{
        Action = $Action
        BridgeHost = $BridgeHost
        Port = $BridgePort
        TimeoutMs = $BridgeTimeoutMs
    }
    if ($Action -ne 'list') { $arguments.BotGuid = $BotGuid }
    $script:BridgeCallCount++
    $raw = & $ControlScriptPath @arguments 2>&1 | Out-String
    try {
        $response = $raw | ConvertFrom-Json
    } catch {
        throw "Bridge action '$Action' did not return JSON: $($_.Exception.Message)"
    }
    if (-not (ConvertTo-Boolean (Get-OptionalProperty -InputObject $response -Name 'ok'))) {
        throw "Bridge action '$Action' failed: $($response | ConvertTo-Json -Compress -Depth 20)"
    }
    return $response
}

function Add-GatherRouteObservation {
    param(
        [Parameter(Mandatory)][AllowEmptyCollection()][System.Collections.Generic.List[object]]$Observations,
        [Parameter(Mandatory)][uint32]$Guid,
        [Parameter(Mandatory)][object]$SnapshotResponse,
        [Parameter(Mandatory)][string]$Phase
    )

    $bot = Get-OptionalProperty -InputObject $SnapshotResponse -Name 'bot'
    $route = Get-OptionalProperty -InputObject $bot -Name 'gather_route'
    if ($null -eq $route) { return }
    [void]$Observations.Add([ordered]@{
        guid = $Guid
        observed_utc = [DateTimeOffset]::UtcNow.ToString('o')
        phase = $Phase
        route = $route
    })
}

function Find-ListedBot {
    param([Parameter(Mandatory)][object]$ListResult, [Parameter(Mandatory)][uint32]$Guid)
    return @(@(Get-OptionalProperty -InputObject $ListResult -Name 'bots' -Default @()) |
        Where-Object { [uint32](Get-OptionalProperty -InputObject $_ -Name 'guid' -Default 0) -eq $Guid } |
        Select-Object -First 1)
}

function Get-BridgeTargetState {
    param([Parameter(Mandatory)][object]$ListResult, [Parameter(Mandatory)][object[]]$Members)

    return @($Members | ForEach-Object {
        $matches = @(Find-ListedBot -ListResult $ListResult -Guid ([uint32]$_.guid))
        if ($matches.Count -eq 0) {
            [pscustomobject][ordered]@{ guid = $_.guid; listed = $false; paused = $null; group_members = $null }
        } else {
            $bot = $matches[0]
            $group = Get-OptionalProperty -InputObject $bot -Name 'group'
            [pscustomobject][ordered]@{
                guid = $_.guid
                listed = $true
                paused = ConvertTo-Boolean (Get-OptionalProperty -InputObject $bot -Name 'paused')
                group_members = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $group -Name 'members')
            }
        }
    })
}

function Wait-ForRosterOnline {
    param([Parameter(Mandatory)][object[]]$Members)

    $deadline = [DateTimeOffset]::UtcNow.AddSeconds($ActivationTimeoutSeconds)
    do {
        $list = Invoke-ControlJson -Action list
        $states = @(Get-BridgeTargetState -ListResult $list -Members $Members)
        if (@($states | Where-Object { -not $_.listed }).Count -eq 0) { return $list }
        if ([DateTimeOffset]::UtcNow -ge $deadline) { break }
        Start-Sleep -Seconds 1
    } while ($true)

    $missing = @($states | Where-Object { -not $_.listed } | ForEach-Object { $_.guid })
    throw "Fixture bots did not become available before activation timeout: $($missing -join ',')."
}

function Wait-ForRosterOffline {
    param([Parameter(Mandatory)][object[]]$Members)

    $deadline = [DateTimeOffset]::UtcNow.AddSeconds($ActivationTimeoutSeconds)
    do {
        $list = Invoke-ControlJson -Action list
        $states = @(Get-BridgeTargetState -ListResult $list -Members $Members)
        if (@($states | Where-Object { $_.listed }).Count -eq 0) { return }
        if ([DateTimeOffset]::UtcNow -ge $deadline) { break }
        Start-Sleep -Seconds 1
    } while ($true)

    $online = @($states | Where-Object { $_.listed } | ForEach-Object { $_.guid })
    throw "Fixture bots did not log out before persistence timeout: $($online -join ',')."
}

function Get-SnapshotFingerprint {
    param([Parameter(Mandatory)][object]$Snapshot)

    $stableView = [ordered]@{
        skills = @($Snapshot.skills | ForEach-Object {
            [ordered]@{
                guid = [uint32]$_.guid
                skill_id = [int]$_.skill_id
                value = [int64]$_.value
                max = [int64]$_.max
            }
        })
        materials = @($Snapshot.materials | ForEach-Object {
            [ordered]@{
                guid = [uint32]$_.guid
                item_entry = [int64]$_.item_entry
                item_class = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $_ -Name 'item_class')
                item_subclass = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $_ -Name 'item_subclass')
                count = [int64]$_.count
            }
        })
        tools = @($Snapshot.tools | ForEach-Object {
            [ordered]@{
                guid = [uint32]$_.guid
                item_entry = [int64]$_.item_entry
                count = [int64]$_.count
            }
        })
    }
    return $stableView | ConvertTo-Json -Depth 20 -Compress
}

function Wait-ForPersistedSnapshot {
    param(
        [Parameter(Mandatory)][object[]]$Members,
        [Parameter(Mandatory)][object]$Connections,
        [Parameter(Mandatory)][string]$ClientPath,
        [ValidateRange(1, 300)][int]$TimeoutSeconds = $PersistenceTimeoutSeconds,
        [ValidateRange(1, 60)][int]$PollSeconds = $PersistencePollSeconds,
        [ValidateRange(2, 5)][int]$StableReadsRequired = $PersistenceStableReads
    )

    $started = [DateTimeOffset]::UtcNow
    $deadline = $started.AddSeconds($TimeoutSeconds)
    $previousFingerprint = $null
    $stableReads = 0
    $attempts = 0
    $lastSnapshot = $null

    do {
        $attempts++
        $lastSnapshot = New-ReadSnapshot -Members $Members -Connections $Connections -ClientPath $ClientPath
        $fingerprint = Get-SnapshotFingerprint -Snapshot $lastSnapshot
        if ($null -ne $previousFingerprint -and $fingerprint -eq $previousFingerprint) {
            $stableReads++
        } else {
            $stableReads = 1
        }
        $previousFingerprint = $fingerprint

        if ($stableReads -ge $StableReadsRequired) {
            $ended = [DateTimeOffset]::UtcNow
            return [pscustomobject][ordered]@{
                snapshot = $lastSnapshot
                stable = $true
                reason = 'stable_read_set_reached'
                read_only = $true
                attempts = $attempts
                stable_reads_observed = $stableReads
                stable_reads_required = $StableReadsRequired
                timeout_seconds = $TimeoutSeconds
                poll_seconds = $PollSeconds
                started_utc = $started.ToString('o')
                ended_utc = $ended.ToString('o')
                elapsed_seconds = [Math]::Round(($ended - $started).TotalSeconds, 3)
            }
        }

        $remainingMilliseconds = [Math]::Max(0, [Math]::Ceiling(($deadline - [DateTimeOffset]::UtcNow).TotalMilliseconds))
        if ($remainingMilliseconds -le 0) { break }
        $sleepMilliseconds = [int][Math]::Min($PollSeconds * 1000, $remainingMilliseconds)
        if ($sleepMilliseconds -gt 0) { Start-Sleep -Milliseconds $sleepMilliseconds }
    } while ([DateTimeOffset]::UtcNow -lt $deadline)

    $ended = [DateTimeOffset]::UtcNow
    return [pscustomobject][ordered]@{
        snapshot = $lastSnapshot
        stable = $false
        reason = 'persistence_not_stable_before_timeout'
        read_only = $true
        attempts = $attempts
        stable_reads_observed = $stableReads
        stable_reads_required = $StableReadsRequired
        timeout_seconds = $TimeoutSeconds
        poll_seconds = $PollSeconds
        started_utc = $started.ToString('o')
        ended_utc = $ended.ToString('o')
        elapsed_seconds = [Math]::Round(($ended - $started).TotalSeconds, 3)
    }
}

function ConvertTo-PersistenceReceipt {
    param([AllowNull()][object]$Observation)

    if ($null -eq $Observation) { return $null }
    return [pscustomobject][ordered]@{
        read_only = [bool]$Observation.read_only
        stable = [bool]$Observation.stable
        reason = "$($Observation.reason)"
        attempts = [int]$Observation.attempts
        stable_reads_observed = [int]$Observation.stable_reads_observed
        stable_reads_required = [int]$Observation.stable_reads_required
        timeout_seconds = [int]$Observation.timeout_seconds
        poll_seconds = [int]$Observation.poll_seconds
        started_utc = $Observation.started_utc
        ended_utc = $Observation.ended_utc
        elapsed_seconds = $Observation.elapsed_seconds
    }
}

function Get-SkillObservation {
    param(
        [Parameter(Mandatory)][uint32]$Guid,
        [Parameter(Mandatory)][string]$Profession,
        [Parameter(Mandatory)][object]$Baseline,
        [Parameter(Mandatory)][object]$Current
    )

    $skillId = [int]$script:GatheringSkills[$Profession].skill_id
    $before = @($Baseline.skills | Where-Object { [uint32]$_.guid -eq $Guid -and [int]$_.skill_id -eq $skillId })
    $after = @($Current.skills | Where-Object { [uint32]$_.guid -eq $Guid -and [int]$_.skill_id -eq $skillId })
    $beforeValue = if ($before.Count -eq 1) { [int64]$before[0].value } else { [int64]0 }
    $afterValue = if ($after.Count -eq 1) { [int64]$after[0].value } else { [int64]0 }
    $beforeMax = if ($before.Count -eq 1) { [int64]$before[0].max } else { [int64]0 }
    $afterMax = if ($after.Count -eq 1) { [int64]$after[0].max } else { [int64]0 }
    return [pscustomobject][ordered]@{
        profession = $Profession
        skill_id = $skillId
        present_before = $before.Count -eq 1
        present_after = $after.Count -eq 1
        before_value = $beforeValue
        after_value = $afterValue
        value_delta = $afterValue - $beforeValue
        before_max = $beforeMax
        after_max = $afterMax
        max_delta = $afterMax - $beforeMax
    }
}

function Get-MaterialDeltas {
    param(
        [Parameter(Mandatory)][uint32]$Guid,
        [Parameter(Mandatory)][object]$Baseline,
        [Parameter(Mandatory)][object]$Current
    )

    $beforeMap = @{}
    $afterMap = @{}
    $keys = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($row in @($Baseline.materials | Where-Object { [uint32]$_.guid -eq $Guid })) {
        $key = "$($row.item_entry)"
        $beforeMap[$key] = $row
        [void]$keys.Add($key)
    }
    foreach ($row in @($Current.materials | Where-Object { [uint32]$_.guid -eq $Guid })) {
        $key = "$($row.item_entry)"
        $afterMap[$key] = $row
        [void]$keys.Add($key)
    }

    $deltas = [System.Collections.Generic.List[object]]::new()
    foreach ($key in @($keys | Sort-Object { [int64]$_ })) {
        $before = if ($beforeMap.ContainsKey($key)) { $beforeMap[$key] } else { $null }
        $after = if ($afterMap.ContainsKey($key)) { $afterMap[$key] } else { $null }
        $beforeCount = if ($null -eq $before) { [int64]0 } else { [int64]$before.count }
        $afterCount = if ($null -eq $after) { [int64]0 } else { [int64]$after.count }
        $delta = $afterCount - $beforeCount
        if ($delta -eq 0) { continue }
        $source = if ($null -ne $after) { $after } else { $before }
        [void]$deltas.Add([pscustomobject][ordered]@{
            item_entry = [int64]$key
            item_name = "$(Get-OptionalProperty -InputObject $source -Name 'item_name' -Default '')"
            item_class = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $source -Name 'item_class')
            item_subclass = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $source -Name 'item_subclass')
            before_count = $beforeCount
            after_count = $afterCount
            delta = $delta
            direction = if ($delta -gt 0) { 'gained' } else { 'spent_or_removed' }
            classification = 'item_template_class_subclass'
            material_category = Get-MaterialTemplateCategory -Material $source
            metadata_source = if ($null -ne (Get-OptionalProperty -InputObject $source -Name 'metadata_source')) {
                "$(Get-OptionalProperty -InputObject $source -Name 'metadata_source')"
            } else { 'world.item_template' }
        })
    }
    return @($deltas)
}

function Get-GatherRouteEvidence {
    param(
        [Parameter(Mandatory)][object]$Member,
        [AllowNull()][object[]]$RouteObservations = @()
    )

    $evidence = [System.Collections.Generic.List[object]]::new()
    foreach ($observation in @($RouteObservations)) {
        $observationGuid = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $observation -Name 'guid')
        if ($observationGuid -ne [uint32]$Member.guid) { continue }

        $route = Get-OptionalProperty -InputObject $observation -Name 'route'
        if ($null -eq $route) {
            $route = Get-OptionalProperty -InputObject $observation -Name 'gather_route'
        }
        if ($null -eq $route) { $route = $observation }

        $candidate = Get-OptionalProperty -InputObject $route -Name 'candidate'
        $oracleSource = Get-OptionalProperty -InputObject $route -Name 'oracle_source'
        $candidateProfession = Get-CanonicalProfession (
            Get-OptionalProperty -InputObject $candidate -Name 'profession' -Default (
                Get-OptionalProperty -InputObject $route -Name 'profession' -Default (
                    Get-OptionalProperty -InputObject $route -Name 'gather_goal' -Default (
                        Get-OptionalProperty -InputObject $oracleSource -Name 'profession'))))
        $sourceGuid = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $route -Name 'source_guid')
        $oracleValid = ConvertTo-Boolean (Get-OptionalProperty -InputObject $oracleSource -Name 'valid')
        $sourceSpawnId = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $oracleSource -Name 'spawn_id' -Default (
            Get-OptionalProperty -InputObject $candidate -Name 'spawn_id'))
        $sourceEntry = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $oracleSource -Name 'entry' -Default (
            Get-OptionalProperty -InputObject $candidate -Name 'entry'))
        $requestedItemEntry = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $route -Name 'requested_material_item_id' -Default (
            Get-OptionalProperty -InputObject $oracleSource -Name 'material_item_id'))
        $sourceYields = ConvertTo-Boolean (Get-OptionalProperty -InputObject $route -Name 'source_yields_requested_material')
        $credit = "$(Get-OptionalProperty -InputObject $route -Name 'credit_evidence' -Default '')".Trim().ToLowerInvariant()
        $exactSource = $sourceGuid -gt 0 -or ($oracleValid -and ($sourceSpawnId -gt 0 -or $sourceEntry -gt 0))
        $creditValid = Test-GatherRouteCreditEvidence -Route $route
        $professionMatch = $candidateProfession -eq $Member.profession
        $complete = $exactSource -and $requestedItemEntry -gt 0 -and $sourceYields -and
            $creditValid -and $professionMatch

        $missing = [System.Collections.Generic.List[string]]::new()
        if (-not $exactSource) { [void]$missing.Add('exact_source_missing') }
        if ($requestedItemEntry -le 0) { [void]$missing.Add('requested_material_missing') }
        if (-not $sourceYields) { [void]$missing.Add('source_material_link_missing') }
        if (-not $creditValid) { [void]$missing.Add('credit_evidence_missing_or_unconfirmed') }
        if (-not $professionMatch) { [void]$missing.Add('source_profession_mismatch_or_missing') }

        [void]$evidence.Add([pscustomobject][ordered]@{
            guid = [uint32]$Member.guid
            observed_utc = Get-OptionalProperty -InputObject $observation -Name 'observed_utc'
            phase = "$(Get-OptionalProperty -InputObject $observation -Name 'phase' -Default '')"
            source_guid = $sourceGuid
            source_spawn_id = $sourceSpawnId
            source_entry = $sourceEntry
            source_name = "$(Get-OptionalProperty -InputObject $candidate -Name 'name' -Default '')"
            source_profession = $candidateProfession
            requested_material_item_id = $requestedItemEntry
            credit_evidence = $credit
            source_yields_requested_material = $sourceYields
            exact_source = $exactSource
            credit_confirmed = $creditValid
            profession_match = $professionMatch
            complete = $complete
            missing = @($missing)
            route = $route
        })
    }
    return @($evidence)
}

function Get-GatheringAttribution {
    param(
        [Parameter(Mandatory)][object]$Member,
        [Parameter(Mandatory)][AllowEmptyCollection()][object[]]$MaterialDeltas,
        [AllowNull()][object[]]$RouteObservations = @()
    )

    $routeEvidence = @(Get-GatherRouteEvidence -Member $Member -RouteObservations $RouteObservations)
    $positiveDeltas = @($MaterialDeltas | Where-Object { [int64]$_.delta -gt 0 })
    $attributedDeltas = [System.Collections.Generic.List[object]]::new()
    $unattributedDeltas = [System.Collections.Generic.List[object]]::new()
    [int64]$observedPositiveUnits = 0
    [int64]$attributedPositiveUnits = 0

    foreach ($delta in $positiveDeltas) {
        $units = [int64]$delta.delta
        $observedPositiveUnits += $units
        $category = "$(Get-OptionalProperty -InputObject $delta -Name 'material_category' -Default (
            Get-MaterialTemplateCategory -Material $delta))"
        $templateCompatibility = Test-MaterialCategoryForProfession -Profession $Member.profession -Category $category
        $matchingEvidence = @($routeEvidence | Where-Object {
            [bool]$_.complete -and [int64]$_.requested_material_item_id -eq [int64]$delta.item_entry
        } | Select-Object -First 1)
        $attributed = $matchingEvidence.Count -gt 0 -and $templateCompatibility -ne $false
        $row = [pscustomobject][ordered]@{
            item_entry = [int64]$delta.item_entry
            item_name = "$(Get-OptionalProperty -InputObject $delta -Name 'item_name' -Default '')"
            item_class = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $delta -Name 'item_class')
            item_subclass = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $delta -Name 'item_subclass')
            material_category = $category
            template_compatible = $templateCompatibility
            delta = $units
            attributed = $attributed
            attribution_evidence = if ($matchingEvidence.Count -gt 0) { $matchingEvidence[0] } else { $null }
            rejection_reason = if ($attributed) {
                $null
            } elseif ($templateCompatibility -eq $false) {
                'item_template_category_incompatible_with_expected_profession'
            } elseif ($matchingEvidence.Count -eq 0) {
                'no_complete_gather_route_receipt_for_item'
            } else {
                'gather_route_receipt_not_compatible_with_item_template'
            }
        }
        if ($attributed) {
            $attributedPositiveUnits += $units
            [void]$attributedDeltas.Add($row)
        } else {
            [void]$unattributedDeltas.Add($row)
        }
    }

    $status = if ($attributedPositiveUnits -gt 0) {
        'proven'
    } elseif ($observedPositiveUnits -gt 0) {
        'inconclusive'
    } else {
        'none'
    }
    $reason = if ($status -eq 'proven' -and $unattributedDeltas.Count -gt 0) {
        'one_or_more_positive_deltas_attributed_and_other_deltas_left_unattributed'
    } elseif ($status -eq 'proven') {
        'positive_delta_matched_exact_source_requested_material_and_credit_evidence'
    } elseif ($status -eq 'inconclusive' -and $routeEvidence.Count -eq 0) {
        'gather_route_attribution_absent'
    } elseif ($status -eq 'inconclusive') {
        'positive_inventory_delta_not_attributed_to_expected_profession'
    } else {
        'no_positive_inventory_delta_observed'
    }

    return [pscustomobject][ordered]@{
        status = $status
        reason = $reason
        observed_positive_material_units = $observedPositiveUnits
        attributed_positive_material_units = $attributedPositiveUnits
        unattributed_positive_material_units = $observedPositiveUnits - $attributedPositiveUnits
        route_evidence = @($routeEvidence)
        attributed_deltas = @($attributedDeltas)
        unattributed_deltas = @($unattributedDeltas)
    }
}

function New-MemberProof {
    param(
        [Parameter(Mandatory)][object]$Member,
        [Parameter(Mandatory)][object]$Baseline,
        [Parameter(Mandatory)][object]$Current,
        [bool]$PersistenceStable = $true,
        [AllowNull()][object[]]$RouteObservations = @()
    )

    $skillObservations = @($script:GatheringSkills.Keys | ForEach-Object {
        Get-SkillObservation -Guid ([uint32]$Member.guid) -Profession $_ -Baseline $Baseline -Current $Current
    })
    $expectedSkill = @($skillObservations | Where-Object { $_.profession -eq $Member.profession })[0]
    $materials = @(Get-MaterialDeltas -Guid ([uint32]$Member.guid) -Baseline $Baseline -Current $Current)
    $attribution = Get-GatheringAttribution -Member $Member -MaterialDeltas $materials -RouteObservations $RouteObservations
    [int64]$positiveUnits = [int64]$attribution.attributed_positive_material_units
    [int64]$observedPositiveUnits = [int64]$attribution.observed_positive_material_units
    [int64]$unattributedPositiveUnits = [int64]$attribution.unattributed_positive_material_units
    [int64]$negativeUnits = 0
    foreach ($material in $materials) {
        if ([int64]$material.delta -lt 0) { $negativeUnits += -1 * [int64]$material.delta }
    }
    $tool = Get-ToolReadiness -Member $Member -Snapshot $Baseline
    $reasons = [System.Collections.Generic.List[string]]::new()
    $status = 'inconclusive'

    if (-not $expectedSkill.present_before -or -not $expectedSkill.present_after -or $expectedSkill.after_value -le 0) {
        $status = 'fail'
        [void]$reasons.Add('expected_profession_skill_missing')
    }
    if ($expectedSkill.value_delta -lt 0 -or $expectedSkill.max_delta -lt 0) {
        $status = 'fail'
        [void]$reasons.Add('profession_skill_regressed')
    }
    if (-not $tool.ready) {
        $status = 'fail'
        [void]$reasons.Add('required_gathering_tool_missing_at_baseline')
    }
    if ($negativeUnits -gt 0) {
        $status = 'fail'
        [void]$reasons.Add('negative_material_delta_confounds_gathering_proof')
    }
    if ($status -ne 'fail') {
        if (-not $PersistenceStable) {
            $status = 'inconclusive'
            [void]$reasons.Add('persistence_not_stable_before_timeout')
            if ($positiveUnits -gt 0) {
                [void]$reasons.Add('positive_profession_attributed_inventory_delta_not_yet_durable')
            } elseif ($observedPositiveUnits -gt 0) {
                [void]$reasons.Add('positive_inventory_delta_not_yet_attributed_or_durable')
            } else {
                [void]$reasons.Add('no_positive_inventory_delta_observed')
            }
        } elseif ($positiveUnits -gt 0) {
            $status = 'pass'
            [void]$reasons.Add('positive_profession_attributed_inventory_delta')
            if ($unattributedPositiveUnits -gt 0) {
                [void]$reasons.Add('other_positive_inventory_delta_left_unattributed')
            }
        } else {
            [void]$reasons.Add($attribution.reason)
        }
    }

    return [pscustomobject][ordered]@{
        guid = $Member.guid
        name = $Member.name
        campaign_character = $Member.campaign_character
        expected_profession = $Member.profession
        expected_profession_skill = $expectedSkill
        all_gathering_skill_deltas = $skillObservations
        baseline_tool_readiness = $tool
        material_inventory_deltas = $materials
        attribution = $attribution
        observed_positive_material_units = $observedPositiveUnits
        positive_material_units = $positiveUnits
        unattributed_positive_material_units = $unattributedPositiveUnits
        negative_material_units = $negativeUnits
        persistence_stable = $PersistenceStable
        status = $status
        reasons = @($reasons)
    }
}

function Get-ProofAggregate {
    param([Parameter(Mandatory)][object[]]$MemberProofs)

    $members = @($MemberProofs)
    $totalCount = $members.Count
    $passMembers = @($members | Where-Object { $_.status -eq 'pass' })
    $inconclusiveMembers = @($members | Where-Object { $_.status -eq 'inconclusive' })
    $failMembers = @($members | Where-Object { $_.status -eq 'fail' })
    $durablePositiveMembers = @($passMembers | Where-Object { [int64]$_.positive_material_units -gt 0 })
    [int64]$durablePositiveUnits = 0
    foreach ($member in $durablePositiveMembers) {
        $durablePositiveUnits += [int64]$member.positive_material_units
    }

    $status = if ($failMembers.Count -gt 0) {
        'fail'
    } elseif ($totalCount -gt 0 -and $passMembers.Count -eq $totalCount) {
        'pass'
    } elseif ($durablePositiveMembers.Count -gt 0) {
        'partial'
    } else {
        'inconclusive'
    }

    $statusReason = switch ($status) {
        'fail' { 'one_or_more_members_failed' }
        'pass' { 'all_members_have_durable_profession_attributed_material_delta' }
        'partial' { 'at_least_one_member_has_durable_profession_attributed_material_delta_but_full_roster_is_not_green' }
        default { 'no_member_has_durable_profession_attributed_material_delta' }
    }

    return [pscustomobject][ordered]@{
        status = $status
        status_reason = $statusReason
        contract = 'all_members_required_for_pass'
        full_pass_requires_all_members = $true
        total_member_count = $totalCount
        pass_member_count = $passMembers.Count
        inconclusive_member_count = $inconclusiveMembers.Count
        fail_member_count = $failMembers.Count
        non_pass_member_count = $totalCount - $passMembers.Count
        durable_positive_material_member_count = $durablePositiveMembers.Count
        durable_positive_material_member_guids = @($durablePositiveMembers | ForEach-Object { [uint32]$_.guid })
        durable_positive_material_units = $durablePositiveUnits
        member_status_counts = [ordered]@{
            pass = $passMembers.Count
            inconclusive = $inconclusiveMembers.Count
            fail = $failMembers.Count
        }
    }
}

function Write-ProofResult {
    param([Parameter(Mandatory)][object]$Result)

    $json = $Result | ConvertTo-Json -Depth 30
    if (-not [string]::IsNullOrWhiteSpace($OutputPath)) {
        $fullOutputPath = [IO.Path]::GetFullPath($OutputPath)
        $parent = Split-Path -Parent $fullOutputPath
        if (-not (Test-Path -LiteralPath $parent)) {
            [void](New-Item -ItemType Directory -Path $parent -Force)
        }
        $json | Set-Content -LiteralPath $fullOutputPath -Encoding utf8
    }
    Write-Output $json
}

if ($LibraryOnly) { return }

$roster = Read-FixtureRoster

if (-not $Apply) {
    $dryRun = [ordered]@{
        schema = $script:GatheringProofSchema
        mode = 'dry_run'
        apply = $false
        fixture_roster = [ordered]@{
            supplied = $roster.supplied
            path = $roster.path
            allow_campaign_characters = $roster.allow_campaign_characters
            members = @($roster.members)
            campaign_membership = 'not queried during dry-run'
        }
        planned_sequence = @(
            'validate explicit versioned fixture roster',
            'SELECT exact GUID character/campaign enrollment state',
            'SELECT exact GUID gathering skills, class-3/7 inventory with item_template class/subclass metadata, and gathering tools for baseline',
            'loopback bridge list; activate only missing exact GUIDs; reject grouped or paused bots',
            'loopback bridge deploy and require worker_gather/configured=1',
            "observe normal world behavior for $DurationSeconds seconds while polling read-only gather_route snapshots",
            'require exact source, requested material, source-yield, credit, and expected-profession evidence for any attributed delta',
            'loopback bridge snapshot exact GUIDs for final live state',
            'loopback bridge deactivate only the exact fixture GUIDs and wait for normal logout persistence',
            'poll read-only SELECT snapshots after logout until stable or the bounded persistence timeout, then calculate deltas'
        )
        apply_requirements = @(
            '-FixtureRosterPath is mandatory',
            'roster schema and purpose must match exactly',
            'campaign GUIDs additionally require allow_campaign_characters=true in that roster',
            'every GUID must exist, be unretired bridge-enrolled, have the named gathering skill, and have any required tool',
            'every bot must be ungrouped and unpaused before deploy',
            'known incompatible item_template categories (for example meat for Herbalism) never prove the expected profession'
        )
        safety = [ordered]@{
            direct_db_writes = 0
            sql_queries = 0
            bridge_calls = 0
            server_start_stop_restart_actions = 0
            server_config_writes = 0
            chat_commands_sent = 0
            allowed_apply_bridge_actions = @('list', 'activate', 'deactivate', 'deploy', 'snapshot')
            persistence = [ordered]@{
                read_only = $true
                timeout_seconds = $PersistenceTimeoutSeconds
                poll_seconds = $PersistencePollSeconds
                stable_reads_required = $PersistenceStableReads
            }
        }
    }
    Write-ProofResult -Result $dryRun
    return
}

if (-not $roster.supplied) {
    throw '-Apply requires -FixtureRosterPath; inferred/default campaign rosters are refused.'
}

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $OutputPath = Join-Path $ServerRoot "logs\gathering-proof-$stamp.json"
}

$stage = 'database_preflight'
$targetState = @()
$validatedMembers = @()
$baseline = $null
$current = $null
$persistence = $null
$controlReceipts = [System.Collections.Generic.List[object]]::new()
$routeObservations = [System.Collections.Generic.List[object]]::new()
$observationStarted = $null
$observationEnded = $null

try {
    $connections = Get-DatabaseConnections
    $clientPath = Get-MySqlClientPath
    $targetState = @(Get-TargetState -Members $roster.members -Connections $connections -ClientPath $clientPath)
    $validatedMembers = @(Assert-TargetSafety -Roster $roster -TargetState $targetState)

    $stage = 'baseline_snapshot'
    $baseline = New-ReadSnapshot -Members $validatedMembers -Connections $connections -ClientPath $clientPath
    Assert-BaselineReadiness -Members $validatedMembers -Snapshot $baseline

    $stage = 'bridge_activation'
    $list = Invoke-ControlJson -Action list
    $initialStates = @(Get-BridgeTargetState -ListResult $list -Members $validatedMembers)
    [void]$controlReceipts.Add([ordered]@{ action = 'list'; target_states = $initialStates })
    foreach ($state in @($initialStates | Where-Object { -not $_.listed })) {
        $receipt = Invoke-ControlJson -Action activate -BotGuid ([uint32]$state.guid)
        [void]$controlReceipts.Add([ordered]@{ action = 'activate'; guid = $state.guid; result = $receipt })
    }
    if (@($initialStates | Where-Object { -not $_.listed }).Count -gt 0) {
        $list = Wait-ForRosterOnline -Members $validatedMembers
    }

    $readyStates = @(Get-BridgeTargetState -ListResult $list -Members $validatedMembers)
    foreach ($state in $readyStates) {
        if (-not $state.listed) { throw "Fixture guid $($state.guid) is not listed by the bridge." }
        if ($state.paused) { throw "Fixture guid $($state.guid) is paused; refusing to alter pause state." }
        if ([int64]$state.group_members -gt 0) { throw "Fixture guid $($state.guid) is grouped; deploy would select party_grind instead of worker_gather." }
    }

    $stage = 'bridge_deploy'
    foreach ($member in $validatedMembers) {
        $deploy = Invoke-ControlJson -Action deploy -BotGuid ([uint32]$member.guid)
        if ("$(Get-OptionalProperty -InputObject $deploy -Name 'mode')" -ne 'worker_gather' -or
            (ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $deploy -Name 'configured')) -ne 1) {
            throw "Fixture guid $($member.guid) deploy did not return worker_gather/configured=1."
        }
        [void]$controlReceipts.Add([ordered]@{ action = 'deploy'; guid = $member.guid; result = $deploy })
        $snapshot = Invoke-ControlJson -Action snapshot -BotGuid ([uint32]$member.guid)
        [void]$controlReceipts.Add([ordered]@{ action = 'snapshot_after_deploy'; guid = $member.guid; result = $snapshot })
        Add-GatherRouteObservation -Observations $routeObservations -Guid ([uint32]$member.guid) -SnapshotResponse $snapshot -Phase 'after_deploy'
    }

    $stage = 'observation_window'
    $observationStarted = [DateTimeOffset]::UtcNow
    $deadline = $observationStarted.AddSeconds($DurationSeconds)
    while ([DateTimeOffset]::UtcNow -lt $deadline) {
        foreach ($member in $validatedMembers) {
            $snapshot = Invoke-ControlJson -Action snapshot -BotGuid ([uint32]$member.guid)
            [void]$controlReceipts.Add([ordered]@{ action = 'snapshot_observation'; guid = $member.guid; result = $snapshot })
            Add-GatherRouteObservation -Observations $routeObservations -Guid ([uint32]$member.guid) -SnapshotResponse $snapshot -Phase 'observation'
        }
        $remaining = [Math]::Ceiling(($deadline - [DateTimeOffset]::UtcNow).TotalSeconds)
        $sleepFor = [Math]::Min($PollSeconds, [Math]::Max(0, $remaining))
        if ($sleepFor -gt 0) { Start-Sleep -Seconds $sleepFor }
    }
    $observationEnded = [DateTimeOffset]::UtcNow

    $stage = 'final_live_snapshot'
    foreach ($member in $validatedMembers) {
        $snapshot = Invoke-ControlJson -Action snapshot -BotGuid ([uint32]$member.guid)
        [void]$controlReceipts.Add([ordered]@{ action = 'snapshot_final'; guid = $member.guid; result = $snapshot })
        Add-GatherRouteObservation -Observations $routeObservations -Guid ([uint32]$member.guid) -SnapshotResponse $snapshot -Phase 'final_live'
    }

    # Character inventory is persisted by the normal Playerbots logout path. Reading SQL while the
    # bot remains online can report the previous save and misclassify a real gather as inconclusive.
    $stage = 'persistence_logout'
    foreach ($member in $validatedMembers) {
        $deactivate = Invoke-ControlJson -Action deactivate -BotGuid ([uint32]$member.guid)
        [void]$controlReceipts.Add([ordered]@{ action = 'deactivate'; guid = $member.guid; result = $deactivate })
    }
    Wait-ForRosterOffline -Members $validatedMembers

    $stage = 'current_snapshot'
    $persistence = Wait-ForPersistedSnapshot -Members $validatedMembers -Connections $connections -ClientPath $clientPath
    $current = $persistence.snapshot

    $stage = 'delta_evaluation'
    $memberProofs = @($validatedMembers | ForEach-Object {
        New-MemberProof -Member $_ -Baseline $baseline -Current $current -PersistenceStable ([bool]$persistence.stable) -RouteObservations @($routeObservations)
    })
    $aggregate = Get-ProofAggregate -MemberProofs $memberProofs
    $overallStatus = $aggregate.status

    $result = [ordered]@{
        schema = $script:GatheringProofSchema
        mode = 'apply'
        status = $overallStatus
        reason = $aggregate.status_reason
        fixture_roster_path = $roster.path
        explicit_campaign_authorization = $roster.allow_campaign_characters
        observation = [ordered]@{
            requested_seconds = $DurationSeconds
            started_utc = $observationStarted.ToString('o')
            ended_utc = $observationEnded.ToString('o')
            elapsed_seconds = [Math]::Round(($observationEnded - $observationStarted).TotalSeconds, 3)
        }
        targets = $validatedMembers
        baseline = $baseline
        current = $current
        member_proofs = $memberProofs
        aggregate = $aggregate
        persistence = ConvertTo-PersistenceReceipt -Observation $persistence
        control_receipts = @($controlReceipts)
        gather_route_observations = @($routeObservations)
        attribution = 'A pass requires a persisted exact-GUID inventory delta matched to bridge gather_route exact source, requested material, source-yield, credit, expected source profession, and compatible item_template metadata.'
        safety = [ordered]@{
            direct_db_writes = 0
            sql_queries = $script:SqlQueryCount
            sql_policy = 'single SELECT statement only'
            bridge_calls = $script:BridgeCallCount
            server_start_stop_restart_actions = 0
            server_config_writes = 0
            chat_commands_sent = 0
            profession_training_actions = 0
        }
        limitations = @(
            'Class-7 or class-3 inventory deltas are temporal observations only; they never prove gathering without matching bridge gather_route attribution.',
            'The bridge gather_route exact-source/requested-material/credit receipt is required for a pass; missing attribution is inconclusive.',
            'world.item_template class/subclass metadata rejects known incompatible yields such as meat for Herbalism.',
            'The harness logs out the exact fixture workers after the final live snapshot so normal persistence can be measured.',
            'The harness does not train professions, provision characters, move items, or clean up fixture enrollment.'
        )
        evidence_path = [IO.Path]::GetFullPath($OutputPath)
    }
    Write-ProofResult -Result $result
} catch {
    $message = $_.Exception.Message
    $failure = [ordered]@{
        schema = $script:GatheringProofSchema
        mode = 'apply'
        status = 'error'
        failed_stage = $stage
        error = $message
        fixture_roster_path = $roster.path
        target_state = $targetState
        validated_targets = $validatedMembers
        baseline = $baseline
        current = $current
        persistence = ConvertTo-PersistenceReceipt -Observation $persistence
        control_receipts = @($controlReceipts)
        gather_route_observations = @($routeObservations)
        safety = [ordered]@{
            direct_db_writes = 0
            sql_queries = $script:SqlQueryCount
            sql_policy = 'single SELECT statement only'
            bridge_calls = $script:BridgeCallCount
            server_start_stop_restart_actions = 0
            server_config_writes = 0
            chat_commands_sent = 0
        }
        evidence_path = [IO.Path]::GetFullPath($OutputPath)
    }
    Write-ProofResult -Result $failure
    throw "Gathering proof failed at stage '$stage': $message Evidence: $([IO.Path]::GetFullPath($OutputPath))"
}
