param(
    [string]$ManifestPath = (Join-Path $PSScriptRoot 'cohort-manifest.json'),
    # Optional offline guid source: JSON object {"<name>": <guid>, ...}. Default: read-only SELECT on characters.
    [string]$GuidMapPath = '',
    # Print only the quoted value instead of the full config line.
    [switch]$ValueOnly
)
# Renders the cohort manifest v2 profession plan into the one-line worldserver config value
#   AutoWow.Professions.Assignments = "<guid>:<skill>[,<skill>];..."
# (format and parse rules: mod-playerbots src/AutoWow/AutoWowTrainPolicy.h). Active entries only, sorted
# by guid. Secondaries are global (AutoWow.Professions.Secondaries), not per guid. Read-only: prints the
# line; pasting it into playerbots.conf and restarting is the operator's step.
# PowerShell 5.1, ASCII only.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'cohort-lib.ps1')

$manifest = Get-CohortManifest -Path $ManifestPath
if ($manifest.schema -ne 'autowow.cohort.manifest.v2') { throw 'profession-assign needs a v2 manifest (profession_plan)' }
$entries = @(Select-CohortEntries -Manifest $manifest)

$guidByName = @{}
if ($GuidMapPath) {
    $map = Get-Content -Raw -LiteralPath $GuidMapPath | ConvertFrom-Json
    foreach ($p in $map.PSObject.Properties) { $guidByName[$p.Name] = [int64]$p.Value }
} else {
    $db = Get-CohortDb
    $names = ($entries | ForEach-Object { "'" + ([string]$_.name).Replace("'", "''") + "'" }) -join ','
    foreach ($line in @(Invoke-CohortSql -DbInfo $db -Parts $db.characters -Sql "SELECT guid,name FROM characters WHERE name IN ($names)")) {
        $a = $line -split "`t"
        $guidByName[$a[1]] = [int64]$a[0]
    }
}

$rows = New-Object System.Collections.Generic.List[object]
$missing = New-Object System.Collections.Generic.List[string]
foreach ($e in $entries) {
    $n = [string]$e.name
    if (-not $guidByName.ContainsKey($n)) { $missing.Add("$($e.id) $n"); continue }
    $rows.Add([pscustomobject]@{ guid = $guidByName[$n]; primary = (@($e.professions.primary | ForEach-Object { [int]$_ }) -join ',') })
}
if ($missing.Count) { throw ("no character guid for: " + ($missing -join ', ')) }
$dupes = @($rows | Group-Object guid | Where-Object { $_.Count -gt 1 })
if ($dupes.Count) { throw ("duplicate guid: " + (($dupes | ForEach-Object { $_.Name }) -join ', ')) }

$value = (@($rows | Sort-Object guid | ForEach-Object { "$($_.guid):$($_.primary)" })) -join ';'
if ($ValueOnly) { '"' + $value + '"' } else { 'AutoWow.Professions.Assignments = "' + $value + '"' }
