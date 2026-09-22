[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$ReceiptPath = '',
    [string]$ReportPath = ''
)

. (Join-Path $PSScriptRoot 'Common.ps1')

$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
$simulation = Join-Path $PSScriptRoot 'league-simulation.ps1'
$control = Join-Path $PSScriptRoot 'autowow-control.ps1'
$config = Join-Path $ServerRoot 'server\configs\worldserver.conf'
foreach ($required in @($simulation, $control, $config)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Required AutoWow file is missing: $required" }
}
if ([string]::IsNullOrWhiteSpace($ReceiptPath)) {
    $ReceiptPath = Join-Path $ServerRoot ('leagues\results\league-v0-sanity-{0}.jsonl' -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
if ([string]::IsNullOrWhiteSpace($ReportPath)) { $ReportPath = Join-Path $ServerRoot 'LEAGUE_SANITY_STATUS.md' }
New-Item -ItemType Directory -Path (Split-Path -Parent $ReceiptPath) -Force | Out-Null
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)

function Get-DatabaseParts {
    param([string]$ConfigPath, [string]$Key)

    $pattern = "^\s*{0}\s*=\s*" -f [regex]::Escape($Key)
    $line = (Select-String -LiteralPath $ConfigPath -Pattern $pattern | Select-Object -First 1).Line
    if (-not $line) { throw "$Key was not found in $ConfigPath" }
    $parts = (($line -replace $pattern, '').Trim().Trim('"')) -split ';', 5
    if ($parts.Count -ne 5) { throw "$Key has an unexpected connection-string format." }
    return $parts
}

function Get-MySqlPath {
    $candidates = @(
        (Join-Path $ServerRoot 'third_party\mysql\bin\mysql.exe'),
        'C:\Program Files\MySQL\MySQL Server 8.4\bin\mysql.exe',
        'C:\Program Files\MySQL\MySQL Server 8.0\bin\mysql.exe'
    )
    $path = @($candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1)[0]
    if (-not $path) { throw 'MySQL CLI was not found.' }
    return $path
}

function Invoke-ReadOnlySql {
    param([string]$Sql)

    $priorPassword = $env:MYSQL_PWD
    $env:MYSQL_PWD = $worldDb[3]
    try {
        $raw = & $mysql --protocol=tcp "--host=$($worldDb[0])" "--port=$($worldDb[1])" "--user=$($worldDb[2])" --default-character-set=utf8mb4 --batch --skip-column-names --execute=$Sql 2>&1
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
        throw "Read-only league database query failed: $safeError"
    }
    return $lines
}

function Convert-TabRows {
    param([string[]]$Lines, [int]$ColumnCount)
    $rows = @()
    foreach ($line in $Lines) {
        $parts = $line -split "`t", $ColumnCount
        if ($parts.Count -eq $ColumnCount) { $rows += ,$parts }
    }
    return $rows
}

function Get-LocationLabel {
    param([string]$Team, [int]$Map)
    if ($Team -eq 'northstar' -and $Map -eq 0) { return 'Northshire expedition' }
    if ($Team -eq 'ember' -and $Map -eq 1) { return 'Valley of Trials expedition' }
    if ($Team -eq 'wayfarers' -and $Map -eq 1) { return 'Kalimdor roaming route' }
    return "map $Map"
}

function Get-DbcString {
    param([byte[]]$Bytes, [int]$StringBlockOffset, [uint32]$Offset)

    if ($Offset -eq 0) { return '' }
    $start = $StringBlockOffset + [int]$Offset
    if ($start -lt $StringBlockOffset -or $start -ge $Bytes.Length) { return '' }
    $end = $start
    while ($end -lt $Bytes.Length -and $Bytes[$end] -ne 0) { $end++ }
    return [System.Text.Encoding]::UTF8.GetString($Bytes, $start, $end - $start)
}

function Get-TalentSpellMetadata {
    $talentPath = Join-Path $ServerRoot 'server\data\dbc\Talent.dbc'
    $tabPath = Join-Path $ServerRoot 'server\data\dbc\TalentTab.dbc'
    foreach ($path in @($talentPath, $tabPath)) { if (-not (Test-Path -LiteralPath $path)) { throw "Required talent DBC is missing: $path" } }
    $talentBytes = [System.IO.File]::ReadAllBytes($talentPath)
    $tabBytes = [System.IO.File]::ReadAllBytes($tabPath)
    foreach ($entry in @(
        [ordered]@{ bytes = $talentBytes; path = $talentPath; fields = 23; size = 92 },
        [ordered]@{ bytes = $tabBytes; path = $tabPath; fields = 24; size = 96 }
    )) {
        if ($entry.bytes.Length -lt 20 -or [System.Text.Encoding]::ASCII.GetString($entry.bytes, 0, 4) -ne 'WDBC') { throw "Invalid DBC header: $($entry.path)" }
        if ([BitConverter]::ToUInt32($entry.bytes, 8) -ne $entry.fields -or [BitConverter]::ToUInt32($entry.bytes, 12) -ne $entry.size) { throw "Unexpected DBC record shape: $($entry.path)" }
    }
    $tabRecords = [BitConverter]::ToUInt32($tabBytes, 4)
    $tabSize = [BitConverter]::ToUInt32($tabBytes, 12)
    $tabStrings = 20 + ($tabRecords * $tabSize)
    $tabNames = @{}
    for ($index = 0; $index -lt $tabRecords; $index++) {
        $offset = 20 + ($index * $tabSize)
        $tabId = [BitConverter]::ToUInt32($tabBytes, $offset)
        $nameOffset = [BitConverter]::ToUInt32($tabBytes, $offset + 4)
        $tabNames[$tabId] = Get-DbcString -Bytes $tabBytes -StringBlockOffset $tabStrings -Offset $nameOffset
    }
    $talentRecords = [BitConverter]::ToUInt32($talentBytes, 4)
    $talentSize = [BitConverter]::ToUInt32($talentBytes, 12)
    $metadata = @{}
    for ($index = 0; $index -lt $talentRecords; $index++) {
        $offset = 20 + ($index * $talentSize)
        $tabId = [BitConverter]::ToUInt32($talentBytes, $offset + 4)
        $tree = if ($tabNames.ContainsKey($tabId) -and -not [string]::IsNullOrWhiteSpace($tabNames[$tabId])) { $tabNames[$tabId] } else { "TalentTab:$tabId" }
        for ($rank = 0; $rank -lt 5; $rank++) {
            $spellId = [BitConverter]::ToUInt32($talentBytes, $offset + 16 + ($rank * 4))
            if ($spellId -ne 0) { $metadata[$spellId] = [ordered]@{ tree = $tree; points = $rank + 1 } }
        }
    }
    return $metadata
}

$primaryProfessions = @{
    164 = 'Blacksmithing'; 165 = 'Leatherworking'; 171 = 'Alchemy'; 182 = 'Herbalism'; 186 = 'Mining';
    197 = 'Tailoring'; 202 = 'Engineering'; 333 = 'Enchanting'; 393 = 'Skinning'; 755 = 'Jewelcrafting'; 773 = 'Inscription'
}
$slotNames = @{
    0 = 'Head'; 1 = 'Neck'; 2 = 'Shoulders'; 3 = 'Shirt'; 4 = 'Chest'; 5 = 'Waist'; 6 = 'Legs';
    7 = 'Feet'; 8 = 'Wrists'; 9 = 'Hands'; 10 = 'Finger 1'; 11 = 'Finger 2'; 12 = 'Trinket 1'; 13 = 'Trinket 2';
    14 = 'Back'; 15 = 'Main hand'; 16 = 'Off hand'; 17 = 'Ranged'; 18 = 'Tabard'
}

$characterDb = Get-DatabaseParts -ConfigPath $config -Key 'CharacterDatabaseInfo'
$worldDb = Get-DatabaseParts -ConfigPath $config -Key 'WorldDatabaseInfo'
$mysql = Get-MySqlPath
$talentSpellMetadata = Get-TalentSpellMetadata

$rosterRaw = (& $simulation -Action roster -ServerRoot $ServerRoot -AsJson | Out-String).Trim()
if ([string]::IsNullOrWhiteSpace($rosterRaw)) { throw 'League roster returned empty output.' }
$roster = $rosterRaw | ConvertFrom-Json
$members = @($roster | ForEach-Object {
    [ordered]@{
        guid = [uint32]$_.guid
        team = if ([string]::IsNullOrWhiteSpace([string]$_.team)) { 'wayfarers' } else { [string]$_.team }
        affiliation = [string]$_.affiliation
        role = [string]$_.role
        class_plan = [string]$_.class_plan
        name = [string]$_.name
    }
})
if ($members.Count -eq 0) { throw 'No league members were returned.' }
$guids = $members.guid -join ','

$bridgeRaw = (& $control -Action list | Out-String).Trim()
if ([string]::IsNullOrWhiteSpace($bridgeRaw)) { throw 'AutoWow bridge returned empty output.' }
$bridge = $bridgeRaw | ConvertFrom-Json
if (-not $bridge.ok) { throw 'AutoWow bridge returned an unsuccessful response.' }
$liveByGuid = @{}
foreach ($bot in @($bridge.bots)) { $liveByGuid[[uint32]$bot.guid] = $bot }

$talentsByGuid = @{}
$talentSql = "SELECT c.guid, COALESCE(GROUP_CONCAT(DISTINCT ct.specMask ORDER BY ct.specMask SEPARATOR ','),''), COALESCE(GROUP_CONCAT(ct.spell ORDER BY ct.spell SEPARATOR ','),'') FROM $($characterDb[4]).characters c LEFT JOIN $($characterDb[4]).character_talent ct ON ct.guid=c.guid WHERE c.guid IN ($guids) GROUP BY c.guid;"
foreach ($row in Convert-TabRows -Lines (Invoke-ReadOnlySql -Sql $talentSql) -ColumnCount 3) {
    $talentsByGuid[[uint32]$row[0]] = [ordered]@{ spec_masks = [string]$row[1]; spell_ids = [string]$row[2]; count = if ([string]::IsNullOrWhiteSpace([string]$row[2])) { 0 } else { @($row[2] -split ',').Count } }
}

$professionsByGuid = @{}
$professionSql = "SELECT guid, skill, value, max FROM $($characterDb[4]).character_skills WHERE guid IN ($guids) AND skill IN ($($primaryProfessions.Keys -join ',')) ORDER BY guid, skill;"
foreach ($row in Convert-TabRows -Lines (Invoke-ReadOnlySql -Sql $professionSql) -ColumnCount 4) {
    $guid = [uint32]$row[0]
    if (-not $professionsByGuid.ContainsKey($guid)) { $professionsByGuid[$guid] = [System.Collections.Generic.List[object]]::new() }
    $skillId = [int]$row[1]
    $professionsByGuid[$guid].Add([ordered]@{ skill = $skillId; name = $primaryProfessions[$skillId]; value = [int]$row[2]; max = [int]$row[3] })
}

$gearByGuid = @{}
$gearSql = "SELECT ci.guid, ci.slot, ii.itemEntry, REPLACE(COALESCE(it.name,''), CHAR(9), ' '), ii.durability, COALESCE(it.ItemLevel,0), COALESCE(it.Quality,0) FROM $($characterDb[4]).character_inventory ci JOIN $($characterDb[4]).item_instance ii ON ii.guid=ci.item LEFT JOIN $($worldDb[4]).item_template it ON it.entry=ii.itemEntry WHERE ci.guid IN ($guids) AND ci.bag=0 AND ci.slot BETWEEN 0 AND 18 ORDER BY ci.guid, ci.slot;"
foreach ($row in Convert-TabRows -Lines (Invoke-ReadOnlySql -Sql $gearSql) -ColumnCount 7) {
    $guid = [uint32]$row[0]
    if (-not $gearByGuid.ContainsKey($guid)) { $gearByGuid[$guid] = [System.Collections.Generic.List[object]]::new() }
    $gearByGuid[$guid].Add([ordered]@{ slot = [int]$row[1]; slot_name = $slotNames[[int]$row[1]]; item_entry = [uint32]$row[2]; name = [string]$row[3]; durability = [int]$row[4]; item_level = [int]$row[5]; quality = [int]$row[6] })
}

$partyNames = @{}
foreach ($member in $members) {
    $live = $liveByGuid[$member.guid]
    if (-not $live -or [int]$live.group.members -lt 2) { continue }
    $leaderGuid = [uint32]$live.group.leader_guid
    if (-not $partyNames.ContainsKey($leaderGuid)) { $partyNames[$leaderGuid] = [System.Collections.Generic.List[string]]::new() }
    $partyNames[$leaderGuid].Add($member.name)
}

$records = @()
$warnings = @()
foreach ($member in $members) {
    $live = $liveByGuid[$member.guid]
    $memberWarnings = @()
    if (-not $live) {
        $memberWarnings += 'offline or missing from live bridge'
        $record = [ordered]@{ guid = $member.guid; team = $member.team; name = $member.name; class_plan = $member.class_plan; online = $false; warnings = $memberWarnings }
        $records += $record
        $warnings += "$($member.name): $($memberWarnings -join '; ')"
        continue
    }
    $level = [int]$live.progress.level
    $talent = if ($talentsByGuid.ContainsKey($member.guid)) { $talentsByGuid[$member.guid] } else { [ordered]@{ spec_masks = ''; spell_ids = ''; count = 0 } }
    $expectedTalentPoints = [Math]::Max(0, $level - 9)
    $talentSpellIds = if ([string]::IsNullOrWhiteSpace([string]$talent.spell_ids)) { @() } else { @($talent.spell_ids -split ',' | ForEach-Object { [uint32]$_ }) }
    [int]$allocatedTalentPoints = 0
    $talentTrees = @{}
    foreach ($spellId in $talentSpellIds) {
        $metadata = if ($talentSpellMetadata.ContainsKey($spellId)) { $talentSpellMetadata[$spellId] } else { [ordered]@{ tree = 'unresolved talent spell'; points = 1 } }
        $allocatedTalentPoints += [int]$metadata.points
        if (-not $talentTrees.ContainsKey($metadata.tree)) { $talentTrees[$metadata.tree] = 0 }
        $talentTrees[$metadata.tree] += [int]$metadata.points
    }
    $treeSummary = if ($talentTrees.Count) { (@($talentTrees.GetEnumerator() | Sort-Object Name | ForEach-Object { "$($_.Key) $($_.Value)" }) -join '; ') } else { 'none' }
    if ($level -lt 10 -and $allocatedTalentPoints -ne 0) { $memberWarnings += 'talents present before level 10' }
    if ($level -ge 10 -and $allocatedTalentPoints -lt $expectedTalentPoints) { $memberWarnings += "talents behind expected allocation ($allocatedTalentPoints/$expectedTalentPoints)" }
    $professions = [System.Collections.Generic.List[object]]::new()
    if ($professionsByGuid.ContainsKey($member.guid)) { foreach ($profession in $professionsByGuid[$member.guid]) { $professions.Add($profession) } }
    if ($professions.Count -gt 2) { $memberWarnings += "more than two primary professions ($($professions.Count))" }
    if ($member.role -eq 'worker' -and $professions.Count -lt 1) { $memberWarnings += 'worker has no primary profession; gathering output must not be credited' }
    if ($level -ge 20 -and $professions.Count -lt 2) { $memberWarnings += "fewer than two primary professions at level $level" }
    $gear = [System.Collections.Generic.List[object]]::new()
    if ($gearByGuid.ContainsKey($member.guid)) { foreach ($gearItem in $gearByGuid[$member.guid]) { $gear.Add($gearItem) } }
    if (@($gear | Where-Object { $_.slot -in 15,16,17 }).Count -eq 0) { $memberWarnings += 'no equipped weapon slot' }
    $leaderGuid = [uint32]$live.group.leader_guid
    $companions = [System.Collections.Generic.List[string]]::new()
    if ($partyNames.ContainsKey($leaderGuid)) { foreach ($companion in $partyNames[$leaderGuid]) { $companions.Add($companion) } }
    if ($companions.Count -eq 0) { $companions.Add($member.name) }
    $record = [ordered]@{
        guid = $member.guid
        team = $member.team
        name = $member.name
        role = $member.role
        class_plan = $member.class_plan
        online = $true
        level = $level
        xp = [uint32]$live.progress.xp
        money_copper = [uint32]$live.progress.money_copper
        position = [ordered]@{ map = [int]$live.position.map; x = [math]::Round([double]$live.position.x,2); y = [math]::Round([double]$live.position.y,2); label = Get-LocationLabel -Team $member.team -Map ([int]$live.position.map) }
        party = [ordered]@{ members = [int]$live.group.members; leader_guid = $leaderGuid; companions = $companions }
        talents = [ordered]@{ expected_points = $expectedTalentPoints; allocated_points = $allocatedTalentPoints; recorded_spells = $talentSpellIds; spec_masks = $talent.spec_masks; tree_summary = $treeSummary; state = if ($level -lt 10) { 'not applicable before level 10' } elseif ($allocatedTalentPoints -ge $expectedTalentPoints) { 'allocated' } else { 'allocation gap' } }
        professions = $professions
        gear = $gear
        warnings = $memberWarnings
    }
    $records += $record
    foreach ($warning in $memberWarnings) { $warnings += "$($member.name): $warning" }
}

$summary = [ordered]@{
    timestamp_utc = (Get-Date).ToUniversalTime().ToString('o')
    event = 'sanity_snapshot'
    expected_online = $members.Count
    online = @($records | Where-Object online).Count
    warnings = $warnings
    characters = $records
}
[System.IO.File]::AppendAllText($ReceiptPath, (($summary | ConvertTo-Json -Compress -Depth 10) + [Environment]::NewLine), $utf8NoBom)

$lines = [System.Collections.Generic.List[string]]::new()
$lines.Add('# AutoWow league sanity status')
$lines.Add('')
$lines.Add("Updated: $($summary.timestamp_utc) UTC")
$lines.Add('')
$lines.Add("Live campaign characters: $($summary.online)/$($summary.expected_online)")
$lines.Add("Warnings: $($summary.warnings.Count)")
$lines.Add('')
foreach ($record in $records | Sort-Object team, guid) {
    if (-not $record.online) {
        $lines.Add("## $($record.name) ($($record.team))")
        $lines.Add('')
        $lines.Add('- Offline; live talents, professions, equipment, position, and party state were not evaluated.')
        if (@($record.warnings).Count) { $lines.Add("- Sanity flags: $($record.warnings -join '; ').") }
        $lines.Add('')
        continue
    }
    $professionText = if (@($record.professions).Count) { (@($record.professions | ForEach-Object { "$($_.name) $($_.value)/$($_.max)" }) -join '; ') } else { 'none yet' }
    $gearText = if (@($record.gear).Count) { (@($record.gear | ForEach-Object { "$($_.slot_name): $($_.name) (ilvl $($_.item_level))" }) -join '; ') } else { 'none found' }
    $partyText = if ($record.party.members -gt 1) { "leader $($record.party.leader_guid); with $($record.party.companions -join ', ')" } else { 'solo' }
    $lines.Add("## $($record.name) ($($record.team))")
    $lines.Add('')
    $lines.Add("- Level $($record.level), XP $($record.xp), $($record.position.label); $partyText.")
    $lines.Add("- Plan: $($record.class_plan). Talents: $($record.talents.state) ($($record.talents.allocated_points)/$($record.talents.expected_points) points; $($record.talents.tree_summary)).")
    $lines.Add("- Professions: $professionText.")
    $lines.Add("- Equipped: $gearText.")
    if (@($record.warnings).Count) { $lines.Add("- Sanity flags: $($record.warnings -join '; ').") }
    $lines.Add('')
}
if ($warnings.Count) {
    $lines.Add('## Flags')
    $lines.Add('')
    foreach ($warning in $warnings) { $lines.Add("- $warning") }
    $lines.Add('')
}
[System.IO.File]::WriteAllText($ReportPath, ($lines -join [Environment]::NewLine), $utf8NoBom)
Write-Output "League sanity snapshot: $ReportPath"
