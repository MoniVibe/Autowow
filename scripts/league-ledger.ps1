[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$LeaguePath = ''
)

. (Join-Path $PSScriptRoot 'Common.ps1')

$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
$leagueRoot = Join-Path $ServerRoot 'leagues'
if ([string]::IsNullOrWhiteSpace($LeaguePath)) { $LeaguePath = Join-Path $leagueRoot 'autowow-league.json' }
& (Join-Path $PSScriptRoot 'league.ps1') -Action validate -ServerRoot $ServerRoot -LeaguePath $LeaguePath | Out-Null
$league = Get-Content -LiteralPath $LeaguePath -Raw | ConvertFrom-Json

function Get-DatabaseParts {
    param([string]$ConfigPath, [string]$Key)
    $pattern = "^\s*{0}\s*=\s*" -f [regex]::Escape($Key)
    $line = (Select-String -LiteralPath $ConfigPath -Pattern $pattern | Select-Object -First 1).Line
    if (-not $line) { throw "$Key was not found in $ConfigPath" }
    $parts = (($line -replace $pattern, '').Trim().Trim('"')) -split ';', 5
    if ($parts.Count -ne 5) { throw "$Key has an unexpected connection-string format." }
    return $parts
}

$worldConfig = Join-Path $ServerRoot 'server\configs\worldserver.conf'
$auth = Get-DatabaseParts -ConfigPath $worldConfig -Key 'LoginDatabaseInfo'
$characters = Get-DatabaseParts -ConfigPath $worldConfig -Key 'CharacterDatabaseInfo'
$mysql = Get-FirstExistingPath -Candidates @((Join-Path $ServerRoot 'third_party\mysql\bin\mysql.exe'),'C:\Program Files\MySQL\MySQL Server 8.4\bin\mysql.exe','C:\Program Files\MySQL\MySQL Server 8.0\bin\mysql.exe')
if (-not $mysql) { throw 'MySQL CLI was not found.' }

$teamAccounts = @{}
$allAccounts = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
foreach ($team in @($league.teams)) {
    $accounts = @([string]$team.captainAccount) + @($team.rosterAccounts | ForEach-Object { [string]$_ })
    $teamAccounts[$team.id] = $accounts
    foreach ($account in $accounts) { $null = $allAccounts.Add($account) }
}
$quoted = @($allAccounts | Sort-Object | ForEach-Object { "'$($_.Replace("'", "''"))'" }) -join ','
$sql = @"
SELECT a.username, c.guid, c.name, c.level, c.money, c.totalHonorPoints, c.totalKills, c.totaltime, c.online
FROM $($auth[4]).account a
LEFT JOIN $($characters[4]).characters c ON c.account = a.id
WHERE a.username IN ($quoted)
ORDER BY a.username, c.guid;
"@
$oldPwd = $env:MYSQL_PWD
$env:MYSQL_PWD = $auth[3]
try {
    $raw = & $mysql --protocol=tcp "--host=$($auth[0])" "--port=$($auth[1])" "--user=$($auth[2])" --batch --skip-column-names --execute=$sql 2>&1
    $exitCode = $LASTEXITCODE
}
finally {
    if ($null -eq $oldPwd) { Remove-Item Env:MYSQL_PWD -ErrorAction SilentlyContinue } else { $env:MYSQL_PWD = $oldPwd }
}
if ($exitCode -ne 0) { throw 'League ledger query failed. Database connection details were not emitted.' }
$rows = @($raw | ForEach-Object { $_.ToString() } | Where-Object { $_ -notmatch '^mysql:' -and $_ -ne '' })
$charactersByAccount = @{}
foreach ($row in $rows) {
    $columns = $row -split "`t"
    if ($columns.Count -ne 9) { continue }
    $account = $columns[0]
    if (-not $charactersByAccount.ContainsKey($account)) { $charactersByAccount[$account] = @() }
    if ($columns[1] -ne 'NULL') {
        $charactersByAccount[$account] += [pscustomobject]@{ guid = [uint32]$columns[1]; name = $columns[2]; level = [int]$columns[3]; money = [uint64]$columns[4]; honor = [uint64]$columns[5]; kills = [uint64]$columns[6]; played_seconds = [uint64]$columns[7]; online = [int]$columns[8] }
    }
}

function Format-Copper {
    param([uint64]$Copper)
    $gold = [uint64][math]::Floor($Copper / 10000)
    $silver = [uint64][math]::Floor(($Copper % 10000) / 100)
    $copperPart = [uint64]($Copper % 100)
    return ('{0:N0}g {1:D2}s {2:D2}c' -f $gold, $silver, $copperPart)
}
function Get-CharacterPropertySum {
    param([object[]]$Characters, [string]$Property)
    if (-not $Characters -or $Characters.Count -eq 0) { return [uint64]0 }
    $measurement = $Characters | Measure-Object -Property $Property -Sum
    return [uint64]$measurement.Sum
}
$lines = [System.Collections.Generic.List[string]]::new()
$lines.Add('# AutoWow league ledger')
$lines.Add('')
$lines.Add(('Generated: {0}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')))
$lines.Add('')
foreach ($team in @($league.teams)) {
    $charactersForTeam = @()
    $presentAccounts = 0
    foreach ($account in $teamAccounts[$team.id]) {
        if ($charactersByAccount.ContainsKey($account)) { $presentAccounts++; $charactersForTeam += @($charactersByAccount[$account]) }
    }
    $money = Get-CharacterPropertySum -Characters $charactersForTeam -Property 'money'
    $honor = Get-CharacterPropertySum -Characters $charactersForTeam -Property 'honor'
    $kills = Get-CharacterPropertySum -Characters $charactersForTeam -Property 'kills'
    $online = @($charactersForTeam | Where-Object { $_.online -eq 1 }).Count
    $lines.Add(('## {0} ({1})' -f $team.displayName, $team.faction))
    $lines.Add('')
    $lines.Add('| Metric | Value |')
    $lines.Add('|---|---:|')
    $lines.Add(('| Planned accounts | {0} |' -f $teamAccounts[$team.id].Count))
    $lines.Add(('| Present accounts | {0} |' -f $presentAccounts))
    $lines.Add(('| Characters | {0} |' -f $charactersForTeam.Count))
    $lines.Add(('| Online characters | {0} |' -f $online))
    $lines.Add(('| Collective wealth | {0} |' -f (Format-Copper -Copper $money)))
    $lines.Add(('| Total honor | {0:N0} |' -f $honor))
    $lines.Add(('| Total kills | {0:N0} |' -f $kills))
    $lines.Add('')
}
$lines.Add('This ledger reads persistent character/account data. Live deaths, revives, combat, and map transitions are collected separately by league-telemetry.ps1.')
$reportPath = Join-Path $ServerRoot 'LEAGUE_LEDGER.md'
[System.IO.File]::WriteAllLines($reportPath, $lines, [System.Text.UTF8Encoding]::new($false))
Write-Output "League ledger: $reportPath"
