# Shared helpers for the persistent bot cohort (dot-sourced by provision/start/park).
# PowerShell 5.1 compatible, ASCII only. Database passwords come from the existing server
# config connection strings (same pattern as oracle-race-campaign.ps1 / quest-telemetry.ps1)
# and are passed to mysql.exe only through a scoped MYSQL_PWD; they are never printed.

Set-StrictMode -Version Latest

$script:CohortServerRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
$script:CohortBridge = Join-Path $script:CohortServerRoot 'scripts\autowow-control.ps1'
$script:CohortWorldConfig = Join-Path $script:CohortServerRoot 'server\configs\worldserver.conf'
$script:CohortPlayerbotsConfig = Join-Path $script:CohortServerRoot 'server\configs\modules\playerbots.conf'

# Valid 3.3.5 race -> class ids, Death Knight (6) deliberately excluded.
$script:CohortValidClasses = @{
    1  = @(1,2,4,5,8,9)      # Human
    2  = @(1,3,4,7,9)        # Orc
    3  = @(1,2,3,4,5)        # Dwarf
    4  = @(1,3,4,5,11)       # Night Elf
    5  = @(1,4,5,8,9)        # Undead
    6  = @(1,3,7,11)         # Tauren
    7  = @(1,4,8,9)          # Gnome
    8  = @(1,3,4,5,7,8)      # Troll
    10 = @(2,3,4,5,8,9)      # Blood Elf
    11 = @(1,2,3,5,7,8)      # Draenei
}
$script:CohortAllianceRaces = @(1,3,4,7,11)

function Get-CohortManifest {
    param([string]$Path = (Join-Path $PSScriptRoot 'cohort-manifest.json'))
    if (-not (Test-Path -LiteralPath $Path)) { throw "Cohort manifest not found: $Path" }
    $manifest = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    $errors = @(Test-CohortManifest -Manifest $manifest)
    if ($errors.Count) { throw ("Cohort manifest invalid:`n - " + ($errors -join "`n - ")) }
    return $manifest
}

function Test-CohortManifest {
    param([Parameter(Mandatory = $true)][object]$Manifest)
    $errors = New-Object System.Collections.Generic.List[string]
    $v2 = ($Manifest.schema -eq 'autowow.cohort.manifest.v2')
    if (-not $v2 -and $Manifest.schema -ne 'autowow.cohort.manifest.v1') { $errors.Add('schema must be autowow.cohort.manifest.v1 or v2') }
    if ($v2) { foreach ($x in @(Test-CohortProfessionPlan -Manifest $Manifest)) { $errors.Add($x) } }
    $seqs = @{}; $ids = @{}; $names = @{}; $perAccount = @{}; $accountFaction = @{}
    $teamIds = @($Manifest.teams | ForEach-Object { [string]$_.team_id })
    foreach ($e in @($Manifest.entries)) {
        $tag = "entry $($e.id)"
        if ($seqs.ContainsKey([int]$e.seq)) { $errors.Add("$tag duplicate seq $($e.seq)") } else { $seqs[[int]$e.seq] = 1 }
        if ([string]$e.id -notmatch '^[AH]-[A-Z]{2}-\d{2,3}$') { $errors.Add("$tag id format") }
        if ($ids.ContainsKey([string]$e.id)) { $errors.Add("$tag duplicate id") } else { $ids[[string]$e.id] = 1 }
        $n = [string]$e.name
        # AzerothCore name rules: 2..12 letters; we also require Capitalized and no triple letters.
        if ($n -cnotmatch '^[A-Z][a-z]{1,11}$') { $errors.Add("$tag name '$n' must be 2-12 ASCII letters, Capitalized") }
        if ($n -match '(.)\1\1') { $errors.Add("$tag name '$n' has a triple letter") }
        if ($names.ContainsKey($n.ToLowerInvariant())) { $errors.Add("$tag duplicate name $n") } else { $names[$n.ToLowerInvariant()] = 1 }
        $race = [int]$e.race_id; $class = [int]$e.class_id
        if (-not $script:CohortValidClasses.ContainsKey($race)) { $errors.Add("$tag unknown race_id $race") }
        elseif ($class -notin $script:CohortValidClasses[$race]) { $errors.Add("$tag invalid race/class $race/$class (DK excluded)") }
        $faction = if ($race -in $script:CohortAllianceRaces) { 'Alliance' } else { 'Horde' }
        if ([string]$e.faction -ne $faction) { $errors.Add("$tag faction must be $faction") }
        if ([int]$e.gender -notin @(0,1)) { $errors.Add("$tag gender must be 0 or 1") }
        $acct = [string]$e.account
        if ($acct -cnotmatch '^[A-Z0-9_]{3,16}$') { $errors.Add("$tag account '$acct' must be 3-16 of A-Z 0-9 _") }
        if ($acct -match '^(?i)rndbot') { $errors.Add("$tag account must not use the rndbot prefix (random pool)") }
        if ($accountFaction.ContainsKey($acct) -and $accountFaction[$acct] -ne $faction) { $errors.Add("$tag account $acct mixes factions") }
        $accountFaction[$acct] = $faction
        if ([string]$e.status -eq 'active') { $perAccount[$acct] = 1 + $(if ($perAccount.ContainsKey($acct)) { $perAccount[$acct] } else { 0 }) }
        if ([string]$e.status -notin @('active','retired')) { $errors.Add("$tag status must be active or retired") }
        if ([string]$e.control_arm -notin @('stock','oracle')) { $errors.Add("$tag control_arm must be stock or oracle") }
        if ((Get-CohortTeamId -Faction $faction) -notin $teamIds) { $errors.Add("$tag team for $faction missing from teams") }
    }
    # CharactersPerRealm = 10 (live worldserver.conf) caps each account.
    foreach ($k in $perAccount.Keys) { if ($perAccount[$k] -gt 10) { $errors.Add("account $k has $($perAccount[$k]) characters (max 10 per realm)") } }
    return $errors
}

# v2 profession plan (docs/PROFESSIONS_PLAN.md section 2). Mirrors the server-side parse rules in
# mod-playerbots src/AutoWow/AutoWowTrainPolicy.h: 1-2 distinct primaries, secondaries from 129/185/356.
$script:CohortPrimarySkills = @(164,165,171,182,186,197,202,333,393,755,773)
$script:CohortSecondarySkills = @(129,185,356)

function Test-CohortProfessionPlan {
    param([Parameter(Mandatory = $true)][object]$Manifest)
    $errors = New-Object System.Collections.Generic.List[string]
    $plan = $Manifest.profession_plan
    if ($null -eq $plan) { $errors.Add('v2 manifest needs profession_plan'); return $errors }
    $pairs = @{}
    foreach ($p in $plan.pairs.PSObject.Properties) { $pairs[$p.Name] = @($p.Value | ForEach-Object { [int]$_ }) }
    foreach ($k in $pairs.Keys) {
        $skills = $pairs[$k]
        if ($skills.Count -lt 1 -or $skills.Count -gt 2 -or ($skills | Select-Object -Unique).Count -ne $skills.Count) { $errors.Add("pair $k must be 1-2 distinct skills") }
        foreach ($s in $skills) { if ($s -notin $script:CohortPrimarySkills) { $errors.Add("pair $k skill $s is not a primary profession") } }
    }
    foreach ($e in @($Manifest.entries)) {
        $tag = "entry $($e.id)"
        $pr = $e.professions
        if ($null -eq $pr) { $errors.Add("$tag missing professions"); continue }
        if (-not $pairs.ContainsKey([string]$pr.pair)) { $errors.Add("$tag unknown pair '$($pr.pair)'"); continue }
        $primary = @($pr.primary | ForEach-Object { [int]$_ })
        if (($primary -join ',') -ne ($pairs[[string]$pr.pair] -join ',')) { $errors.Add("$tag primary does not match pair $($pr.pair)") }
        foreach ($s in @($pr.secondary)) { if ([int]$s -notin $script:CohortSecondarySkills) { $errors.Add("$tag secondary $s is not 129/185/356") } }
        if ([int]$pr.plan_version -ne [int]$plan.plan_version) { $errors.Add("$tag plan_version $($pr.plan_version) != $($plan.plan_version)") }
    }
    return $errors
}

function Get-CohortTeamId {
    param([Parameter(Mandatory = $true)][string]$Faction)
    if ($Faction -eq 'Alliance') { 'cohort-alliance' } else { 'cohort-horde' }
}

function Select-CohortEntries {
    param([object]$Manifest, [string[]]$Id = @(), [string]$Faction = 'Both')
    @($Manifest.entries | Where-Object {
        $_.status -eq 'active' -and
        ($Id.Count -eq 0 -or [string]$_.id -in $Id) -and
        ($Faction -eq 'Both' -or [string]$_.faction -eq $Faction)
    } | Sort-Object { [int]$_.seq })
}

function ConvertTo-CohortSqlLiteral {
    param([AllowNull()][object]$Value)
    if ($null -eq $Value) { return 'NULL' }
    return "'" + ([string]$Value).Replace("\", "\\").Replace("'", "''") + "'"
}

function Get-CohortDbParts {
    param([Parameter(Mandatory = $true)][string]$ConfigPath, [Parameter(Mandatory = $true)][string]$Key)
    $pattern = "^\s*{0}\s*=\s*" -f [regex]::Escape($Key)
    $match = Select-String -LiteralPath $ConfigPath -Pattern $pattern | Select-Object -First 1
    if (-not $match) { throw "$Key was not found in $ConfigPath" }
    $parts = (($match.Line -replace $pattern, '').Trim().Trim('"')) -split ';', 5
    if ($parts.Count -ne 5) { throw "$Key has an unexpected connection-string format." }
    return ,$parts
}

function Get-CohortDb {
    $mysql = @(
        (Join-Path $script:CohortServerRoot 'third_party\mysql\bin\mysql.exe'),
        'C:\Program Files\MySQL\MySQL Server 8.4\bin\mysql.exe',
        'C:\Program Files\MySQL\MySQL Server 8.0\bin\mysql.exe'
    ) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (-not $mysql) { throw 'MySQL CLI was not found.' }
    [pscustomobject]@{
        mysql = $mysql
        auth = Get-CohortDbParts -ConfigPath $script:CohortWorldConfig -Key 'LoginDatabaseInfo'
        characters = Get-CohortDbParts -ConfigPath $script:CohortWorldConfig -Key 'CharacterDatabaseInfo'
        playerbots = Get-CohortDbParts -ConfigPath $script:CohortPlayerbotsConfig -Key 'PlayerbotsDatabaseInfo'
    }
}

function Invoke-CohortSql {
    param(
        [Parameter(Mandatory = $true)][object]$DbInfo,
        [Parameter(Mandatory = $true)][string[]]$Parts,
        [Parameter(Mandatory = $true)][string]$Sql,
        [switch]$Write
    )
    if (-not $Write) {
        if ($Sql.TrimStart() -notmatch '^(?i)SELECT\b' -or $Sql -match ';\s*\S' -or
            $Sql -match '(?i)\b(INSERT|UPDATE|DELETE|REPLACE|DROP|ALTER|CREATE|TRUNCATE|GRANT|SET|CALL|LOAD|RENAME|LOCK)\b') {
            throw 'Refusing non read-only SQL without -Write.'
        }
    }
    $prior = $env:MYSQL_PWD
    $env:MYSQL_PWD = $Parts[3]
    try {
        $raw = & $DbInfo.mysql --protocol=tcp "--host=$($Parts[0])" "--port=$($Parts[1])" "--user=$($Parts[2])" `
            "--database=$($Parts[4])" --default-character-set=utf8mb4 --batch --raw --skip-column-names "--execute=$Sql" 2>&1
        $exit = $LASTEXITCODE
    }
    finally {
        if ($null -eq $prior) { Remove-Item Env:MYSQL_PWD -ErrorAction SilentlyContinue } else { $env:MYSQL_PWD = $prior }
    }
    $lines = @($raw | ForEach-Object { $_.ToString() } | Where-Object { $_ -and $_ -notmatch '^mysql: ' })
    if ($exit -ne 0) {
        $safe = @($lines | Where-Object { $_ -notmatch '(?i)password|MYSQL_PWD' }) -join ' '
        throw "Cohort database command failed: $safe"
    }
    return $lines
}

# Read-only snapshot of everything the cohort depends on, keyed for the manifest.
function Get-CohortInventory {
    param([Parameter(Mandatory = $true)][object]$DbInfo, [Parameter(Mandatory = $true)][object[]]$Entries)
    $accountList = (@($Entries | ForEach-Object { [string]$_.account } | Sort-Object -Unique) | ForEach-Object { ConvertTo-CohortSqlLiteral $_ }) -join ','
    $nameList = (@($Entries | ForEach-Object { ConvertTo-CohortSqlLiteral ([string]$_.name) })) -join ','
    $accounts = @{}
    foreach ($line in @(Invoke-CohortSql -DbInfo $DbInfo -Parts $DbInfo.auth -Sql "SELECT id,username FROM account WHERE username IN ($accountList)")) {
        $c = $line -split "`t"; $accounts[$c[1].ToUpperInvariant()] = [uint32]$c[0]
    }
    $characters = @{}
    foreach ($line in @(Invoke-CohortSql -DbInfo $DbInfo -Parts $DbInfo.characters -Sql "SELECT guid,account,name,race,class,gender,level FROM characters WHERE name IN ($nameList)")) {
        $c = $line -split "`t"
        $characters[$c[2].ToLowerInvariant()] = [pscustomobject]@{
            guid = [uint32]$c[0]; account = [uint32]$c[1]; name = $c[2]; race = [int]$c[3]; class = [int]$c[4]; gender = [int]$c[5]; level = [int]$c[6]
        }
    }
    $members = @{}
    $guids = @($characters.Values | ForEach-Object { $_.guid })
    if ($guids.Count) {
        foreach ($line in @(Invoke-CohortSql -DbInfo $DbInfo -Parts $DbInfo.playerbots -Sql "SELECT character_guid,COALESCE(team_id,''),IF(retired_at IS NULL,0,1) FROM autowow_league_member WHERE character_guid IN ($($guids -join ','))")) {
            $c = $line -split "`t"; $members[[uint32]$c[0]] = [pscustomobject]@{ team_id = $c[1]; retired = ($c[2] -eq '1') }
        }
    }
    $teams = @{}
    foreach ($line in @(Invoke-CohortSql -DbInfo $DbInfo -Parts $DbInfo.playerbots -Sql "SELECT team_id,faction FROM autowow_league_team WHERE team_id IN ('cohort-alliance','cohort-horde')")) {
        $c = $line -split "`t"; $teams[$c[0]] = $c[1]
    }
    $rows = foreach ($e in $Entries) {
        $acctId = if ($accounts.ContainsKey(([string]$e.account).ToUpperInvariant())) { $accounts[([string]$e.account).ToUpperInvariant()] } else { $null }
        $ch = if ($characters.ContainsKey(([string]$e.name).ToLowerInvariant())) { $characters[([string]$e.name).ToLowerInvariant()] } else { $null }
        $state = 'ok'; $detail = ''
        if ($ch) {
            if ($null -eq $acctId -or $ch.account -ne $acctId) { $state = 'conflict'; $detail = "name taken by account id $($ch.account)" }
            elseif ($ch.race -ne [int]$e.race_id -or $ch.class -ne [int]$e.class_id) { $state = 'conflict'; $detail = "existing race/class $($ch.race)/$($ch.class)" }
        }
        elseif ($null -eq $acctId) { $state = 'missing_account' }
        else { $state = 'missing_character' }
        $member = if ($ch -and $members.ContainsKey($ch.guid)) { $members[$ch.guid] } else { $null }
        $team = Get-CohortTeamId -Faction ([string]$e.faction)
        $enrolled = [bool]($member -and $member.team_id -eq $team -and -not $member.retired)
        if ($state -eq 'ok' -and $member -and $member.team_id -ne $team) { $state = 'conflict'; $detail = "enrolled in team '$($member.team_id)'" }
        [pscustomobject][ordered]@{
            id = [string]$e.id; name = [string]$e.name; account = [string]$e.account; account_id = $acctId
            guid = if ($ch) { $ch.guid } else { $null }; level = if ($ch) { $ch.level } else { $null }
            state = $state; detail = $detail; enrolled = $enrolled; team_id = $team; control_arm = [string]$e.control_arm
        }
    }
    [pscustomobject]@{ rows = @($rows); teams = $teams }
}

# Live Oracle allowlist as the WSL worldserver reads it (read-only file grep).
function Get-CohortOracleAllowlist {
    param([string]$WslDistro = 'Ubuntu-24.04', [string]$ConfigPath = '/usr/local/etc/modules/playerbots.conf')
    $lines = @(& wsl.exe -d $WslDistro -u root -e grep -E '^\s*AutoWow\.OracleRuntime\.(Enabled|BotGuids)\s*=' $ConfigPath 2>$null)
    $enabled = $false; $guids = @()
    foreach ($l in $lines) {
        $v = (([string]$l) -replace '^[^=]*=\s*', '').Trim().Trim('"')
        if ([string]$l -match 'Enabled') { $enabled = ($v -eq '1') }
        else { $guids = @($v -split '[,\s]+' | Where-Object { $_ -match '^\d+$' } | ForEach-Object { [uint32]$_ }) }
    }
    [pscustomobject]@{ enabled = $enabled; guids = $guids; source = $ConfigPath }
}

function Invoke-CohortBridge {
    param([Parameter(Mandatory = $true)][string]$Action, [uint32]$Guid = 0)
    $lines = @(if ($Guid) { & $script:CohortBridge -Action $Action -BotGuid $Guid } else { & $script:CohortBridge -Action $Action })
    if ($lines.Count -eq 0) { throw "Bridge $Action returned nothing." }
    return (([string]$lines[-1]) | ConvertFrom-Json)
}

function Get-CohortOnlineGuids {
    $list = Invoke-CohortBridge -Action list
    if (-not $list.ok) { throw 'Bridge list returned a non-ok response.' }
    return @($list.bots | ForEach-Object { [uint32]$_.guid })
}
