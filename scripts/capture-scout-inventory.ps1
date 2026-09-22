[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$manifestPath = Join-Path $PSScriptRoot 'fixtures\zone-scouts-20260922.json'
$configPath = Join-Path $projectRoot 'server\configs\worldserver.conf'
$mysqlPath = Join-Path $projectRoot 'third_party\mysql\bin\mysql.exe'
$historyDir = Join-Path $projectRoot 'work\zone-scouts-20260922\inventory-history'
$expectedGuids = @(101, 112, 121, 123, 236, 244)
$utf8 = [Text.UTF8Encoding]::new($false)

function Get-Connection([string]$config, [string]$key) {
    $match = [regex]::Match($config, '(?im)^\s*' + [regex]::Escape($key) + '\s*=\s*"(?<value>[^"\r\n]+)"')
    if (-not $match.Success) { throw "$key is unavailable." }
    $parts = $match.Groups['value'].Value.Split(';')
    if ($parts.Count -ne 5 -or $parts[0] -notin @('localhost', '127.0.0.1', '::1') -or
        $parts[1] -notmatch '^\d{1,5}$' -or [int]$parts[1] -lt 1 -or [int]$parts[1] -gt 65535 -or
        $parts[4] -notmatch '^[A-Za-z0-9_]+$') {
        throw "$key must be a valid local database connection."
    }
    return $parts
}

function Get-Location([long]$bag, [int]$slot, [hashtable]$rootSlots) {
    $rootSlot = $slot
    if ($bag -ne 0) {
        if (-not $rootSlots.ContainsKey([string]$bag)) { return 'unresolved_bag' }
        $rootSlot = [int]$rootSlots[[string]$bag]
    }
    if ($rootSlot -ge 0 -and $rootSlot -lt 19) { return 'equipped' }
    if ($rootSlot -ge 19 -and $rootSlot -lt 39) { return 'carried' }
    if ($rootSlot -ge 39 -and $rootSlot -lt 74) { return 'bank' }
    if ($rootSlot -ge 74 -and $rootSlot -lt 86) { return 'buyback' }
    if ($rootSlot -ge 86 -and $rootSlot -lt 118) { return 'keyring' }
    return 'other'
}

if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf) -or
    -not (Test-Path -LiteralPath $mysqlPath -PathType Leaf)) { throw 'Required local input is missing.' }
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json -AsHashtable
$scouts = @($manifest.scouts)
$actualGuids = @($scouts | ForEach-Object { [int]$_.guid } | Sort-Object)
if ($manifest.schema -ne 'autowow.zone-scout.manifest.v1' -or $scouts.Count -ne 6 -or
    (($actualGuids -join ',') -ne (($expectedGuids | Sort-Object) -join ','))) {
    throw 'Manifest does not match the fixed six-scout roster.'
}

$config = Get-Content -LiteralPath $configPath -Raw
$characters = Get-Connection $config 'CharacterDatabaseInfo'
$world = Get-Connection $config 'WorldDatabaseInfo'
if ($characters[0] -ne $world[0] -or $characters[1] -ne $world[1]) {
    throw 'Character and world databases must use the same local MySQL endpoint.'
}
$charactersDb = $characters[4]
$worldDb = $world[4]
$sql = "SELECT c.guid,c.money,ci.bag,ci.slot,ci.item,ii.itemEntry,ii.count,HEX(COALESCE(it.name,'')),it.Quality,it.class,it.SellPrice,it.BuyPrice,ii.owner_guid FROM $charactersDb.characters c LEFT JOIN $charactersDb.character_inventory ci ON ci.guid=c.guid LEFT JOIN $charactersDb.item_instance ii ON ii.guid=ci.item LEFT JOIN $worldDb.item_template it ON it.entry=ii.itemEntry WHERE c.guid IN (101,112,121,123,236,244) ORDER BY c.guid,ci.bag,ci.slot,ci.item"

$start = [Diagnostics.ProcessStartInfo]::new($mysqlPath)
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
foreach ($argument in @('--protocol=tcp', '--connect-timeout=5', "--host=$($characters[0])",
    "--port=$($characters[1])", "--user=$($characters[2])", "--database=$charactersDb",
    '--default-character-set=utf8mb4', '--batch', '--raw', '--skip-column-names',
    "--execute=$sql")) { [void]$start.ArgumentList.Add($argument) }
$start.Environment['MYSQL_PWD'] = $characters[3]
$process = [Diagnostics.Process]::Start($start)
try {
    $output = $process.StandardOutput.ReadToEnd()
    $null = $process.StandardError.ReadToEnd() # Do not expose diagnostics or credentials.
    $process.WaitForExit()
    if ($process.ExitCode -ne 0) { throw "Read-only inventory query failed (exit $($process.ExitCode))." }
} finally { $process.Dispose() }

$byGuid = @{}
foreach ($scout in $scouts) {
    $byGuid[[string]$scout.guid] = [ordered]@{
        guid = [int]$scout.guid; name = [string]$scout.name
        saved_money_copper = $null; item_rows = @(); root_slots = @{}
    }
}
foreach ($line in ($output -split "`r?`n")) {
    if ([string]::IsNullOrWhiteSpace($line)) { continue }
    $fields = $line.Split([char]9)
    if ($fields.Count -ne 13 -or $fields[0] -notin @($expectedGuids | ForEach-Object { [string]$_ }) -or
        $fields[1] -notmatch '^\d+$') { throw 'Inventory query returned an unexpected row.' }
    $record = $byGuid[$fields[0]]
    $record.saved_money_copper = [long]$fields[1]
    if ($fields[4] -eq 'NULL') { continue } # Character with no saved inventory rows.
    if ($fields[2] -notmatch '^\d+$' -or $fields[3] -notmatch '^\d+$' -or
        $fields[4] -notmatch '^\d+$') { throw 'Inventory query returned malformed item placement.' }
    $name = if ($fields[7] -ne 'NULL' -and $fields[7].Length -gt 0) {
        [Text.Encoding]::UTF8.GetString([Convert]::FromHexString($fields[7]))
    } else { $null }
    $item = [ordered]@{
        item_guid = [long]$fields[4]; bag_guid = [long]$fields[2]; slot = [int]$fields[3]
        location = $null
        entry = if ($fields[5] -eq 'NULL') { $null } else { [int]$fields[5] }
        stack_count = if ($fields[6] -eq 'NULL') { $null } else { [int]$fields[6] }
        name = $name
        quality = if ($fields[8] -eq 'NULL') { $null } else { [int]$fields[8] }
        item_class = if ($fields[9] -eq 'NULL') { $null } else { [int]$fields[9] }
        vendor_sell_price_each_copper = if ($fields[10] -eq 'NULL') { $null } else { [long]$fields[10] }
        vendor_buy_price_each_copper = if ($fields[11] -eq 'NULL') { $null } else { [long]$fields[11] }
        saved_owner_guid = if ($fields[12] -eq 'NULL') { $null } else { [long]$fields[12] }
    }
    $record.item_rows += $item
    if ($item.bag_guid -eq 0) { $record.root_slots[[string]$item.item_guid] = $item.slot }
}

$totalRows = 0
$poorCarriedRows = 0
$missingMetadataRows = 0
$snapshotScouts = @()
foreach ($scout in $scouts) {
    $record = $byGuid[[string]$scout.guid]
    if ($null -eq $record.saved_money_copper) { throw "No character row for scout $($scout.guid)." }
    foreach ($item in $record.item_rows) {
        $item.location = Get-Location $item.bag_guid $item.slot $record.root_slots
        $totalRows++
        if ($item.location -eq 'carried' -and $item.quality -eq 0) { $poorCarriedRows++ }
        if ($null -eq $item.entry -or $null -eq $item.quality) { $missingMetadataRows++ }
    }
    $snapshotScouts += [ordered]@{
        guid = $record.guid; name = $record.name
        saved_money_copper = $record.saved_money_copper
        item_row_count = @($record.item_rows).Count
        items = @($record.item_rows)
    }
}
$capturedUtc = [datetime]::UtcNow
$snapshot = [ordered]@{
    schema = 'autowow.zone-scout.inventory.v1'
    captured_utc = $capturedUtc.ToString('o')
    manifest_path = $manifestPath
    source = 'Read-only local character_inventory, item_instance, characters and world item_template SQL.'
    location_rule = 'Bag GUID is resolved to its root slot. Root slots 0-18 equipped, 19-38 carried, 39-73 bank, 74-85 buyback, 86-117 keyring; raw bag GUID and slot retained.'
    limits = @(
        'Saved character DB inventory and copper can lag live online state.',
        'Poor quality and vendor price do not prove an item is safe to sell; quest/profession/use classification requires live policy evidence.',
        'One snapshot is state evidence, not a vendor transaction journal. Compare item GUID/count and copper across snapshots.'
    )
    summary = [ordered]@{
        scout_count = 6; item_rows = $totalRows
        poor_quality_carried_rows = $poorCarriedRows; missing_metadata_rows = $missingMetadataRows
    }
    scouts = $snapshotScouts
}
New-Item -ItemType Directory -Path $historyDir -Force | Out-Null
$snapshotPath = Join-Path $historyDir ("inventory-{0}-{1}.json" -f $capturedUtc.ToString('yyyyMMddTHHmmssfffZ'), [guid]::NewGuid().ToString('N').Substring(0, 8))
$json = ConvertTo-Json -InputObject $snapshot -Depth 15
$bytes = $utf8.GetBytes($json + "`n")
$stream = [IO.FileStream]::new($snapshotPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
try { $stream.Write($bytes, 0, $bytes.Length) } finally { $stream.Dispose() }
[ordered]@{
    ok = $true; captured_utc = $snapshot.captured_utc; snapshot_path = $snapshotPath
    scout_count = 6; item_rows = $totalRows; poor_quality_carried_rows = $poorCarriedRows
    missing_metadata_rows = $missingMetadataRows
    data_source_limit = 'Saved character DB inventory and copper may lag live online state.'
} | ConvertTo-Json -Compress -Depth 3
