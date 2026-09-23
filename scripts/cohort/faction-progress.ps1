param(
    [string]$OutPath = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'work\faction-progress.jsonl'),
    [string]$ManifestPath = (Join-Path $PSScriptRoot 'cohort-manifest.json'),
    # Real-time KPI: autowow.ledger log(s) with `skill_up` events (AutoWow.Ledger.Enable=1 + AutoWow.Ledger.SkillUp=1).
    [string[]]$LedgerPath = @(),
    # Only fold ledger lines of this run label (AutoWow.Ledger.RunId). Empty = all runs.
    [string]$RunId = ''
)
# Read-only faction progression sample for the persistent cohort. Appends one JSON line per call:
# per faction: characters, total/avg level, total profession skill (primary + secondary), per-profession totals.
# Faction progression KPI (owner 2026-09-23) = total profession skill per faction and its trend.
# With -LedgerPath each faction also gets a `ledger` block folded from `skill_up` events (real time,
# no save-interval lag): total = per (bot, skill) the last ledger `new` value, else the DB value;
# delta = sum(new - old); trend = delta per game hour (ledger ms / 3600000). The DB read stays the
# cross-check: db_mismatch counts (bot, skill) pairs whose saved DB value differs from the ledger's
# last value (expected to shrink to 0 after a save; it is reported, never smoothed).
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
if ($LedgerPath.Count) {
    # DB values per (guid, skill), re-read so the fold does not depend on the aggregate above.
    $dbValue = @{}
    if ($chars.Count) {
        foreach ($line in @(Invoke-CohortSql -DbInfo $db -Parts $db.characters -Sql "SELECT guid,skill,value FROM character_skills WHERE guid IN ($guids) AND skill IN ($skillList)")) {
            $a = $line -split "`t"
            $dbValue["$([int]$a[0]):$([int]$a[1])"] = [int]$a[2]
        }
    }
    $last = @{}; $events = 0; $lines = 0
    foreach ($f in $result.factions.Keys) { $result.factions[$f]['ledger'] = [ordered]@{ events = 0; delta = 0; learned = 0; total_profession_skill = 0; db_mismatch = 0; trend = [ordered]@{} } }
    foreach ($m in @(Select-String -LiteralPath $LedgerPath -SimpleMatch '"ev":"skill_up"')) {
        $lines++
        $i = $m.Line.IndexOf('{"v":')
        if ($i -lt 0) { continue }
        $ev = $m.Line.Substring($i) | ConvertFrom-Json
        if ($RunId -and [string]$ev.run -ne $RunId) { continue }
        $c = $chars[[int]$ev.bot]
        if (-not $c -or -not $result.factions.Contains($c.faction) -or -not $Professions.Contains([int]$ev.skill)) { continue }
        $lx = $result.factions[$c.faction].ledger
        $d = [int]$ev.new - [int]$ev.old
        $lx.events++; $lx.delta += $d
        if ([string]$ev.cause -eq 'learn') { $lx.learned++ }
        $hour = 'h' + [string][math]::Floor([double]$ev.ms / 3600000)
        if (-not $lx.trend.Contains($hour)) { $lx.trend[$hour] = 0 }
        $lx.trend[$hour] += $d
        # Log order is emission order per bot (one world thread per map); later lines win.
        $last["$([int]$ev.bot):$([int]$ev.skill)"] = [int]$ev.new
        $events++
    }
    $keys = @{}
    foreach ($k in $dbValue.Keys) { $keys[$k] = 1 }
    foreach ($k in $last.Keys) { $keys[$k] = 1 }
    foreach ($k in $keys.Keys) {
        $c = $chars[[int]($k.Split(':')[0])]
        if (-not $c -or -not $result.factions.Contains($c.faction)) { continue }
        $lx = $result.factions[$c.faction].ledger
        $dbv = if ($dbValue.ContainsKey($k)) { $dbValue[$k] } else { 0 }
        if ($last.ContainsKey($k)) {
            $lx.total_profession_skill += $last[$k]
            if ($last[$k] -ne $dbv) { $lx.db_mismatch++ }
        } else { $lx.total_profession_skill += $dbv }
    }
    $result['ledger'] = [ordered]@{ paths = @($LedgerPath); run = $RunId; matched_lines = $lines; cohort_events = $events }
}
$dir = Split-Path -Parent $OutPath
if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
Add-Content -Path $OutPath -Value ($result | ConvertTo-Json -Depth 5 -Compress) -Encoding ASCII
$result | ConvertTo-Json -Depth 5
