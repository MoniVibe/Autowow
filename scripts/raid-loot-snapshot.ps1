[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [Alias('Roster')]
    [string[]]$RosterGuid,

    [string]$WorldServerConfigPath = (Join-Path $PSScriptRoot '..\server\configs\worldserver.conf'),
    [string]$BaselinePath,
    [string]$OutputPath,
    [string]$MySqlPath,

    # Test seam: param($QueryName, $Sql, $Headers, $Connection) -> fixture rows.
    [scriptblock]$QueryExecutor
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:SchemaVersion = 'raid-loot-snapshot-v1'
$script:SlotNames = @(
    'head', 'neck', 'shoulders', 'shirt', 'chest', 'waist', 'legs', 'feet', 'wrists',
    'hands', 'finger_1', 'finger_2', 'trinket_1', 'trinket_2', 'back', 'main_hand',
    'off_hand', 'ranged', 'tabard'
)

function ConvertTo-ExactRoster {
    param([Parameter(Mandatory)][string[]]$Values)

    $result = [System.Collections.Generic.List[uint32]]::new()
    $seen = [System.Collections.Generic.HashSet[uint32]]::new()
    foreach ($rawValue in $Values) {
        foreach ($value in @("$rawValue" -split ',')) {
            if ($value -notmatch '^\d+$') {
                throw "Roster GUID '$value' is not an exact uint32 value."
            }
            [uint64]$wide = 0
            if (-not [uint64]::TryParse($value, [ref]$wide) -or $wide -gt [uint32]::MaxValue -or $wide -eq 0) {
                throw "Roster GUID '$value' is outside the valid character uint32 range 1..4294967295."
            }
            $guid = [uint32]$wide
            if (-not $seen.Add($guid)) {
                throw "Roster GUID '$guid' was supplied more than once."
            }
            [void]$result.Add($guid)
        }
    }
    if ($result.Count -eq 0) {
        throw 'At least one roster GUID is required.'
    }
    return @($result | Sort-Object)
}

function Get-ConfigDatabaseConnection {
    param(
        [Parameter(Mandatory)][string]$ConfigText,
        [Parameter(Mandatory)][string]$Key
    )

    $escapedKey = [regex]::Escape($Key)
    $pattern = '(?m)^\s*{0}\s*=\s*"([^"]+)"\s*(?:#.*)?$' -f $escapedKey
    $matches = [regex]::Matches($ConfigText, $pattern)
    if ($matches.Count -ne 1) {
        throw "Expected exactly one active $Key value in worldserver.conf."
    }
    $parts = $matches[0].Groups[1].Value -split ';', 5
    if ($parts.Count -ne 5 -or @($parts | Where-Object { [string]::IsNullOrWhiteSpace($_) }).Count -ne 0) {
        throw "$Key must contain host;port;user;password;database."
    }
    [uint16]$port = 0
    if (-not [uint16]::TryParse($parts[1], [ref]$port) -or $port -eq 0) {
        throw "$Key contains an invalid TCP port."
    }
    if ($parts[4] -notmatch '^[A-Za-z0-9_]+$') {
        throw "$Key contains an unsafe database identifier."
    }
    return [pscustomobject][ordered]@{
        Host = $parts[0]
        Port = $port
        User = $parts[2]
        Password = $parts[3]
        Database = $parts[4]
    }
}

function Assert-SelectOnlySql {
    param([Parameter(Mandatory)][string]$Sql)

    if ($Sql -notmatch '^\s*SELECT\b') {
        throw 'Only SELECT SQL is permitted.'
    }
    if ($Sql -match '(?i)(?:;\s*\S|--|/\*|\*/|#|\bREPLACE\s+INTO\b|\b(?:INSERT|UPDATE|DELETE|DROP|ALTER|CREATE|TRUNCATE|CALL|SET|GRANT|REVOKE|LOAD|LOCK|UNLOCK)\b)') {
        throw 'The SQL failed the SELECT-only safety check.'
    }
}

function Resolve-MySqlClient {
    param([string]$RequestedPath)

    if (-not [string]::IsNullOrWhiteSpace($RequestedPath)) {
        if (-not (Test-Path -LiteralPath $RequestedPath -PathType Leaf)) {
            throw "MySQL client not found: $RequestedPath"
        }
        return (Resolve-Path -LiteralPath $RequestedPath).Path
    }
    $candidates = @(
        (Join-Path $PSScriptRoot '..\third_party\mysql\bin\mysql.exe'),
        (Join-Path $PSScriptRoot '..\server\bin\mysql.exe')
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    $command = Get-Command mysql.exe -ErrorAction SilentlyContinue
    if ($null -ne $command) { return $command.Source }
    throw 'mysql.exe was not found. Pass -MySqlPath or install a PATH-visible client.'
}

function ConvertFrom-TabularText {
    param(
        [AllowNull()][string]$Text,
        [Parameter(Mandatory)][string[]]$Headers
    )

    $rows = [System.Collections.Generic.List[object]]::new()
    foreach ($line in @($Text -split "`r?`n")) {
        if ([string]::IsNullOrWhiteSpace($line) -or $line -match '^(?i:warning|mysql:)') { continue }
        $parts = $line.Split([char]9)
        if ($parts.Count -ne $Headers.Count) {
            throw "Read-only query returned $($parts.Count) columns; expected $($Headers.Count)."
        }
        $row = [ordered]@{}
        for ($index = 0; $index -lt $Headers.Count; $index++) {
            $row[$Headers[$index]] = $parts[$index]
        }
        [void]$rows.Add([pscustomobject]$row)
    }
    return @($rows)
}

function Invoke-LiveSelectQuery {
    param(
        [Parameter(Mandatory)][string]$Sql,
        [Parameter(Mandatory)][string[]]$Headers,
        [Parameter(Mandatory)][pscustomobject]$Connection,
        [Parameter(Mandatory)][string]$ClientPath
    )

    Assert-SelectOnlySql -Sql $Sql
    $previousPassword = [Environment]::GetEnvironmentVariable('MYSQL_PWD', 'Process')
    $hadPreviousPassword = $null -ne $previousPassword
    try {
        $env:MYSQL_PWD = $Connection.Password
        $arguments = @(
            '--protocol=tcp', "--host=$($Connection.Host)", "--port=$($Connection.Port)",
            "--user=$($Connection.User)", "--database=$($Connection.Database)",
            '--default-character-set=utf8mb4', '--batch', '--skip-column-names', "--execute=$Sql"
        )
        $text = & $ClientPath @arguments 2>&1 | ForEach-Object { "$_" } | Out-String
        if ($LASTEXITCODE -ne 0) {
            throw "Read-only raid loot query failed with exit code $LASTEXITCODE."
        }
        return @(ConvertFrom-TabularText -Text $text -Headers $Headers)
    } finally {
        if ($hadPreviousPassword) { $env:MYSQL_PWD = $previousPassword }
        else { Remove-Item Env:MYSQL_PWD -ErrorAction SilentlyContinue }
    }
}

function Get-PropertyValue {
    param([Parameter(Mandatory)][object]$InputObject, [Parameter(Mandatory)][string]$Name)
    $property = $InputObject.PSObject.Properties[$Name]
    if ($null -eq $property) { throw "Query row is missing required field '$Name'." }
    return $property.Value
}

function ConvertTo-UInt32Field {
    param([AllowNull()][object]$Value, [Parameter(Mandatory)][string]$Name)
    [uint32]$parsed = 0
    if ($null -eq $Value -or -not [uint32]::TryParse("$Value", [ref]$parsed)) {
        throw "Query field '$Name' is not a uint32 value."
    }
    return $parsed
}

function ConvertTo-Int32Field {
    param([AllowNull()][object]$Value, [Parameter(Mandatory)][string]$Name)
    [int]$parsed = 0
    if ($null -eq $Value -or -not [int]::TryParse("$Value", [ref]$parsed)) {
        throw "Query field '$Name' is not an int32 value."
    }
    return $parsed
}

function New-ItemRecord {
    param([Parameter(Mandatory)][object]$Row)
    return [ordered]@{
        item_guid = ConvertTo-UInt32Field (Get-PropertyValue $Row 'item_guid') 'item_guid'
        entry = ConvertTo-UInt32Field (Get-PropertyValue $Row 'entry') 'entry'
        name = "$(Get-PropertyValue $Row 'name')"
        quality = ConvertTo-Int32Field (Get-PropertyValue $Row 'quality') 'quality'
        item_level = ConvertTo-Int32Field (Get-PropertyValue $Row 'item_level') 'item_level'
        durability = ConvertTo-Int32Field (Get-PropertyValue $Row 'durability') 'durability'
        count = ConvertTo-UInt32Field (Get-PropertyValue $Row 'count') 'count'
        bag = ConvertTo-UInt32Field (Get-PropertyValue $Row 'bag') 'bag'
        slot = ConvertTo-UInt32Field (Get-PropertyValue $Row 'slot') 'slot'
    }
}

function New-Aggregates {
    param([object[]]$Items, [object[]]$EquippedItems, [object[]]$CarriedItems)

    [uint64]$stackCount = 0
    [int64]$itemLevelSum = 0
    [int64]$equippedItemLevelSum = 0
    foreach ($item in @($Items)) {
        $stackCount += [uint64]$item.count
        $itemLevelSum += [int64]$item.item_level
    }
    foreach ($item in @($EquippedItems)) {
        $equippedItemLevelSum += [int64]$item.item_level
    }
    $average = if ($EquippedItems.Count -eq 0) { 0.0 } else { [Math]::Round($equippedItemLevelSum / [double]$EquippedItems.Count, 3) }
    return [ordered]@{
        item_guid_count = [int]$Items.Count
        stack_count = $stackCount
        equipped_item_count = [int]$EquippedItems.Count
        carried_item_count = [int]$CarriedItems.Count
        item_level_sum = $itemLevelSum
        equipped_item_level_sum = $equippedItemLevelSum
        equipped_average_item_level = $average
    }
}

function Get-AllSnapshotItems {
    param([Parameter(Mandatory)][object]$Snapshot)
    $items = [System.Collections.Generic.List[object]]::new()
    foreach ($character in @($Snapshot.characters)) {
        foreach ($slot in @($character.equipped_slots)) {
            if ($null -ne $slot.item_guid) {
                [void]$items.Add([pscustomobject][ordered]@{
                    character_guid = [uint32]$character.guid; location = 'equipped'; slot = [int]$slot.slot
                    item_guid = [uint32]$slot.item_guid; entry = [uint32]$slot.entry; name = "$($slot.name)"
                    quality = [int]$slot.quality; item_level = [int]$slot.item_level; count = [uint32]$slot.count
                })
            }
        }
        foreach ($item in @($character.carried_items)) {
            [void]$items.Add([pscustomobject][ordered]@{
                character_guid = [uint32]$character.guid; location = 'carried'; slot = [int]$item.slot
                item_guid = [uint32]$item.item_guid; entry = [uint32]$item.entry; name = "$($item.name)"
                quality = [int]$item.quality; item_level = [int]$item.item_level; count = [uint32]$item.count
            })
        }
    }
    return @($items)
}

function New-BaselineComparison {
    param([Parameter(Mandatory)][object]$Baseline, [Parameter(Mandatory)][object]$Current)

    if ("$($Baseline.schema_version)" -ne $script:SchemaVersion) {
        throw "Baseline schema_version must be $script:SchemaVersion."
    }
    $baselineRoster = @($Baseline.roster_guids | ForEach-Object { [uint32]$_ } | Sort-Object)
    $currentRoster = @($Current.roster_guids | ForEach-Object { [uint32]$_ } | Sort-Object)
    if (($baselineRoster -join ',') -ne ($currentRoster -join ',')) {
        throw 'Baseline roster must exactly match the current uint32 roster.'
    }

    $beforeItems = @(Get-AllSnapshotItems -Snapshot $Baseline)
    $afterItems = @(Get-AllSnapshotItems -Snapshot $Current)
    $beforeByGuid = @{}; foreach ($item in $beforeItems) { $beforeByGuid["$($item.item_guid)"] = $item }
    $afterByGuid = @{}; foreach ($item in $afterItems) { $afterByGuid["$($item.item_guid)"] = $item }
    $newItems = @($afterItems | Where-Object { -not $beforeByGuid.ContainsKey("$($_.item_guid)") } | Sort-Object character_guid, item_guid)
    $removedItems = @($beforeItems | Where-Object { -not $afterByGuid.ContainsKey("$($_.item_guid)") } | Sort-Object character_guid, item_guid)

    $baselineCharacters = @{}; foreach ($character in @($Baseline.characters)) { $baselineCharacters["$($character.guid)"] = $character }
    $replacements = [System.Collections.Generic.List[object]]::new()
    $ilvlDeltas = [System.Collections.Generic.List[object]]::new()
    foreach ($character in @($Current.characters | Sort-Object guid)) {
        $beforeCharacter = $baselineCharacters["$($character.guid)"]
        for ($slotIndex = 0; $slotIndex -le 18; $slotIndex++) {
            $before = @($beforeCharacter.equipped_slots | Where-Object { [int]$_.slot -eq $slotIndex })[0]
            $after = @($character.equipped_slots | Where-Object { [int]$_.slot -eq $slotIndex })[0]
            if ($null -ne $before.item_guid -and $null -ne $after.item_guid -and [uint32]$before.item_guid -ne [uint32]$after.item_guid) {
                [void]$replacements.Add([ordered]@{
                    character_guid = [uint32]$character.guid; slot = $slotIndex; slot_name = $script:SlotNames[$slotIndex]
                    old_item_guid = [uint32]$before.item_guid; old_entry = [uint32]$before.entry; old_name = "$($before.name)"; old_item_level = [int]$before.item_level
                    new_item_guid = [uint32]$after.item_guid; new_entry = [uint32]$after.entry; new_name = "$($after.name)"; new_item_level = [int]$after.item_level
                    item_level_delta = [int]$after.item_level - [int]$before.item_level
                })
            }
        }
        [void]$ilvlDeltas.Add([ordered]@{
            character_guid = [uint32]$character.guid
            baseline_equipped_item_level_sum = [int64]$beforeCharacter.aggregates.equipped_item_level_sum
            current_equipped_item_level_sum = [int64]$character.aggregates.equipped_item_level_sum
            equipped_item_level_sum_delta = [int64]$character.aggregates.equipped_item_level_sum - [int64]$beforeCharacter.aggregates.equipped_item_level_sum
            baseline_equipped_average_item_level = [double]$beforeCharacter.aggregates.equipped_average_item_level
            current_equipped_average_item_level = [double]$character.aggregates.equipped_average_item_level
            equipped_average_item_level_delta = [Math]::Round([double]$character.aggregates.equipped_average_item_level - [double]$beforeCharacter.aggregates.equipped_average_item_level, 3)
        })
    }
    return [ordered]@{
        baseline_captured_utc = "$($Baseline.captured_utc)"
        new_item_guids = @($newItems)
        removed_item_guids = @($removedItems)
        equipped_slot_replacements = @($replacements)
        character_item_level_deltas = @($ilvlDeltas)
    }
}

$roster = @(ConvertTo-ExactRoster -Values $RosterGuid)
if (-not (Test-Path -LiteralPath $WorldServerConfigPath -PathType Leaf)) {
    throw "worldserver.conf not found: $WorldServerConfigPath"
}
$configText = Get-Content -LiteralPath $WorldServerConfigPath -Raw
$charactersConnection = Get-ConfigDatabaseConnection -ConfigText $configText -Key 'CharacterDatabaseInfo'
$worldConnection = Get-ConfigDatabaseConnection -ConfigText $configText -Key 'WorldDatabaseInfo'
$guidList = $roster -join ','
$headers = @('character_guid', 'bag', 'slot', 'item_guid', 'entry', 'name', 'quality', 'item_level', 'durability', 'count')
$sql = "SELECT ci.guid AS character_guid, ci.bag, ci.slot, ci.item AS item_guid, ii.itemEntry AS entry, REPLACE(REPLACE(COALESCE(it.name,''), CHAR(9), ' '), CHAR(10), ' ') AS name, COALESCE(it.Quality,0) AS quality, COALESCE(it.ItemLevel,0) AS item_level, COALESCE(ii.durability,0) AS durability, COALESCE(ii.count,1) AS count FROM $($charactersConnection.Database).character_inventory ci JOIN $($charactersConnection.Database).item_instance ii ON ii.guid=ci.item LEFT JOIN $($worldConnection.Database).item_template it ON it.entry=ii.itemEntry WHERE ci.guid IN ($guidList) ORDER BY ci.guid, ci.bag, ci.slot, ci.item"
Assert-SelectOnlySql -Sql $sql

if ($null -ne $QueryExecutor) {
    $rows = @(& $QueryExecutor 'inventory' $sql $headers $charactersConnection)
} else {
    $clientPath = Resolve-MySqlClient -RequestedPath $MySqlPath
    $rows = @(Invoke-LiveSelectQuery -Sql $sql -Headers $headers -Connection $charactersConnection -ClientPath $clientPath)
}

$rosterSet = [System.Collections.Generic.HashSet[uint32]]::new()
foreach ($guid in $roster) { [void]$rosterSet.Add($guid) }
$itemsByCharacter = @{}
foreach ($guid in $roster) { $itemsByCharacter["$guid"] = [System.Collections.Generic.List[object]]::new() }
foreach ($row in $rows) {
    $characterGuid = ConvertTo-UInt32Field (Get-PropertyValue $row 'character_guid') 'character_guid'
    if (-not $rosterSet.Contains($characterGuid)) {
        throw "Query returned character GUID $characterGuid outside the exact roster."
    }
    $item = New-ItemRecord -Row $row
    [void]$itemsByCharacter["$characterGuid"].Add([pscustomobject]$item)
}

$characters = [System.Collections.Generic.List[object]]::new()
$allItems = [System.Collections.Generic.List[object]]::new()
foreach ($guid in $roster) {
    $items = @($itemsByCharacter["$guid"] | Sort-Object bag, slot, item_guid)
    foreach ($item in $items) { [void]$allItems.Add($item) }
    $equipped = @($items | Where-Object { $_.bag -eq 0 -and $_.slot -le 18 })
    $carried = @($items | Where-Object { -not ($_.bag -eq 0 -and $_.slot -le 18) })
    $equippedBySlot = @{}; foreach ($item in $equipped) {
        if ($equippedBySlot.ContainsKey([int]$item.slot)) { throw "Character $guid has duplicate equipped slot $($item.slot)." }
        $equippedBySlot[[int]$item.slot] = $item
    }
    $slots = [System.Collections.Generic.List[object]]::new()
    for ($slot = 0; $slot -le 18; $slot++) {
        $item = $equippedBySlot[$slot]
        [void]$slots.Add([ordered]@{
            slot = $slot; slot_name = $script:SlotNames[$slot]
            item_guid = if ($null -eq $item) { $null } else { [uint32]$item.item_guid }
            entry = if ($null -eq $item) { $null } else { [uint32]$item.entry }
            name = if ($null -eq $item) { $null } else { "$($item.name)" }
            quality = if ($null -eq $item) { $null } else { [int]$item.quality }
            item_level = if ($null -eq $item) { $null } else { [int]$item.item_level }
            durability = if ($null -eq $item) { $null } else { [int]$item.durability }
            count = if ($null -eq $item) { $null } else { [uint32]$item.count }
        })
    }
    [void]$characters.Add([ordered]@{
        guid = [uint32]$guid
        equipped_slots = @($slots)
        carried_items = @($carried | ForEach-Object { [ordered]@{
            item_guid = [uint32]$_.item_guid; entry = [uint32]$_.entry; name = "$($_.name)"
            quality = [int]$_.quality; item_level = [int]$_.item_level; durability = [int]$_.durability
            count = [uint32]$_.count; bag = [uint32]$_.bag; slot = [uint32]$_.slot
        } })
        aggregates = New-Aggregates -Items $items -EquippedItems $equipped -CarriedItems $carried
    })
}

$allItemArray = @($allItems)
$allEquipped = @($allItemArray | Where-Object { $_.bag -eq 0 -and $_.slot -le 18 })
$allCarried = @($allItemArray | Where-Object { -not ($_.bag -eq 0 -and $_.slot -le 18) })
$snapshot = [ordered]@{
    schema_version = $script:SchemaVersion
    captured_utc = [DateTime]::UtcNow.ToString('o')
    roster_guids = @($roster)
    source = [ordered]@{
        config_path = (Resolve-Path -LiteralPath $WorldServerConfigPath).Path
        character_database = $charactersConnection.Database
        world_database = $worldConnection.Database
        tables = @('character_inventory', 'item_instance', 'item_template')
        query_count = 1
        read_only = $true
    }
    characters = @($characters)
    aggregates = New-Aggregates -Items $allItemArray -EquippedItems $allEquipped -CarriedItems $allCarried
    comparison = $null
}

if (-not [string]::IsNullOrWhiteSpace($BaselinePath)) {
    if (-not (Test-Path -LiteralPath $BaselinePath -PathType Leaf)) { throw "Baseline JSON not found: $BaselinePath" }
    $baseline = Get-Content -LiteralPath $BaselinePath -Raw | ConvertFrom-Json
    $snapshot.comparison = New-BaselineComparison -Baseline $baseline -Current ([pscustomobject]$snapshot)
}

$json = $snapshot | ConvertTo-Json -Depth 12
if (-not [string]::IsNullOrWhiteSpace($OutputPath)) {
    $parent = Split-Path -Parent $OutputPath
    if (-not [string]::IsNullOrWhiteSpace($parent) -and -not (Test-Path -LiteralPath $parent -PathType Container)) {
        throw "Output directory does not exist: $parent"
    }
    Set-Content -LiteralPath $OutputPath -Value $json -Encoding utf8
} else {
    Write-Output $json
}
