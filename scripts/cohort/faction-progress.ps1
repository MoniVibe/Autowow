param(
    [string]$OutPath = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'work\faction-progress.jsonl'),
    [string]$ManifestPath = (Join-Path $PSScriptRoot 'cohort-manifest.json')
)
# Read-only faction progression sample for the persistent cohort. Appends one JSON line per call:
# per faction: characters, total/avg level, total profession skill (primary + secondary), per-profession totals.
# Faction progression KPI (owner 2026-09-23) = total profession skill per faction and its trend.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'cohort-lib.ps1')

$Professions = [ordered]@{
    164 = 'blacksmithing'; 165 = 'leatherworking'; 171 = 'alchemy'; 182 = 'herbalism'; 186 = 'mining'
    197 = 'tailoring'; 202 = 'engineering'; 333 = 'enchanting'; 393 = 'skinning'; 755 = 'jewelcrafting'
    773 = 'inscription'; 129 = 'first_aid'; 185 = 'cooking'; 356 = 'fishing'
}

$db = Get-CohortDb
$entries = @((Get-Content -Raw -LiteralPath $ManifestPath | ConvertFrom-Json).entries)
$names = ($entries | ForEach-Object { "'" + ([string]$_.name).Replace("'", "''") + "'" }) -join ','
$factionByName = @{}
foreach ($e in $entries) { $factionByName[[string]$e.name] = [string]$e.faction }

$chars = @{}
foreach ($line in @(Invoke-CohortSql -DbInfo $db -Parts $db.characters -Sql "SELECT guid,name,level FROM characters WHERE name IN ($names)")) {
    $a = $line -split "`t"
    $chars[[int]$a[0]] = [pscustomobject]@{ name = $a[1]; level = [int]$a[2]; faction = $factionByName[$a[1]] }
}

$result = [ordered]@{ utc = (Get-Date).ToUniversalTime().ToString('o'); factions = [ordered]@{} }
foreach ($f in @('Alliance', 'Horde')) {
    $result.factions[$f] = [ordered]@{ characters = 0; total_level = 0; avg_level = 0; total_profession_skill = 0; professions = [ordered]@{} }
}
foreach ($c in $chars.Values) {
    if (-not $result.factions.Contains($c.faction)) { continue }
    $result.factions[$c.faction].characters++
    $result.factions[$c.faction].total_level += $c.level
}
if ($chars.Count) {
    $guids = ($chars.Keys | Sort-Object) -join ','
    $skillList = ($Professions.Keys) -join ','
    foreach ($line in @(Invoke-CohortSql -DbInfo $db -Parts $db.characters -Sql "SELECT guid,skill,value FROM character_skills WHERE guid IN ($guids) AND skill IN ($skillList)")) {
        $a = $line -split "`t"
        $c = $chars[[int]$a[0]]
        if (-not $c -or -not $result.factions.Contains($c.faction)) { continue }
        $prof = $Professions[[int]$a[1]]
        $v = [int]$a[2]
        $fx = $result.factions[$c.faction]
        $fx.total_profession_skill += $v
        if (-not $fx.professions.Contains($prof)) { $fx.professions[$prof] = 0 }
        $fx.professions[$prof] += $v
    }
}
foreach ($f in $result.factions.Keys) {
    $fx = $result.factions[$f]
    if ($fx.characters) { $fx.avg_level = [math]::Round($fx.total_level / $fx.characters, 2) }
}
$dir = Split-Path -Parent $OutPath
if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
Add-Content -Path $OutPath -Value ($result | ConvertTo-Json -Depth 5 -Compress) -Encoding ASCII
$result | ConvertTo-Json -Depth 5
