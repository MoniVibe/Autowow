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
    $line = (Select-String -LiteralPath $ConfigPath -Pattern ("^\s*{0}\s*=\s*" -f [regex]::Escape($Key)) | Select-Object -First 1).Line
    if (-not $line) { throw "$Key was not found in $ConfigPath" }
    $value = ($line -replace ("^\s*{0}\s*=\s*" -f [regex]::Escape($Key)), '').Trim().Trim('"')
    $parts = $value -split ';', 5
    if ($parts.Count -ne 5) { throw "$Key has an unexpected connection-string format." }
    return $parts
}

$worldConfig = Join-Path $ServerRoot 'server\configs\worldserver.conf'
$auth = Get-DatabaseParts -ConfigPath $worldConfig -Key 'LoginDatabaseInfo'
$characters = Get-DatabaseParts -ConfigPath $worldConfig -Key 'CharacterDatabaseInfo'
$mysql = Get-FirstExistingPath -Candidates @(
    (Join-Path $ServerRoot 'third_party\mysql\bin\mysql.exe'),
    'C:\Program Files\MySQL\MySQL Server 8.4\bin\mysql.exe',
    'C:\Program Files\MySQL\MySQL Server 8.0\bin\mysql.exe'
)
if (-not $mysql) { throw 'MySQL CLI was not found.' }

$accountNames = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
foreach ($team in @($league.teams)) {
    $null = $accountNames.Add([string]$team.captainAccount)
    foreach ($rosterAccount in @($team.rosterAccounts)) { $null = $accountNames.Add([string]$rosterAccount) }
}
$quotedNames = @($accountNames | Sort-Object | ForEach-Object { "'$($_.Replace("'", "''"))'" }) -join ','
$authDb = $auth[4]
$characterDb = $characters[4]
$sql = @"
SELECT a.username, a.id, COUNT(c.guid) AS character_count
FROM $authDb.account a
LEFT JOIN $characterDb.characters c ON c.account = a.id
WHERE a.username IN ($quotedNames)
GROUP BY a.username, a.id
ORDER BY a.username;
SELECT owner.username, linked.username
FROM acore_playerbots.playerbots_account_links l
INNER JOIN $authDb.account owner ON owner.id = l.account_id
INNER JOIN $authDb.account linked ON linked.id = l.linked_account_id
WHERE owner.username IN ($quotedNames) AND linked.username IN ($quotedNames)
ORDER BY owner.username, linked.username;
"@

$previousPwd = $env:MYSQL_PWD
$env:MYSQL_PWD = $auth[3]
try {
    $raw = & $mysql --protocol=tcp "--host=$($auth[0])" "--port=$($auth[1])" "--user=$($auth[2])" --batch --skip-column-names --execute=$sql 2>&1
    $exitCode = $LASTEXITCODE
}
finally {
    if ($null -eq $previousPwd) { Remove-Item Env:MYSQL_PWD -ErrorAction SilentlyContinue } else { $env:MYSQL_PWD = $previousPwd }
}
$lines = @($raw | ForEach-Object { $_.ToString() } | Where-Object { $_ -notmatch '^mysql:' })
if ($exitCode -ne 0) { throw 'League account audit query failed. Database connection details were not emitted.' }

$records = @{}
$links = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
$inLinkSection = $false
foreach ($line in $lines) {
    if ([string]::IsNullOrWhiteSpace($line)) { $inLinkSection = $true; continue }
    $columns = $line -split "`t"
    if (-not $inLinkSection -and $columns.Count -eq 3) {
        $records[$columns[0]] = [pscustomobject]@{ id = $columns[1]; character_count = [int]$columns[2] }
    }
    elseif ($columns.Count -eq 2) {
        $inLinkSection = $true
        $null = $links.Add("$($columns[0])>$($columns[1])")
    }
}

$markdown = [System.Collections.Generic.List[string]]::new()
$markdown.Add('# AutoWow league account audit')
$markdown.Add('')
$markdown.Add(('Generated: {0}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')))
$markdown.Add('')
$markdown.Add('This is read-only. It neither creates accounts nor links them.')
foreach ($team in @($league.teams)) {
    $captainSlots = if ($league.humanCaptainsIncluded) { 1 } else { 0 }
    $botSlots = [int]$league.teamSize - $captainSlots
    $markdown.Add('')
    $markdown.Add(('## {0} ({1})' -f $team.displayName, $team.faction))
    $markdown.Add('')
    $markdown.Add('| Role | Account | Present | Characters | Captain link |')
    $markdown.Add('|---|---|---:|---:|---:|')
    $captain = [string]$team.captainAccount
    $captainRecord = $records[$captain]
    $markdown.Add(('| Captain | {0} | {1} | {2} | n/a |' -f $captain, [bool]$captainRecord, $(if ($captainRecord) { $captainRecord.character_count } else { 0 })))
    foreach ($account in @($team.rosterAccounts)) {
        $record = $records[[string]$account]
        $linked = $links.Contains("$captain>$account") -and $links.Contains("$account>$captain")
        $markdown.Add(('| Roster | {0} | {1} | {2} | {3} |' -f $account, [bool]$record, $(if ($record) { $record.character_count } else { 0 }), $linked))
    }
    $markdown.Add('')
    $markdown.Add(('- Target: one captain plus {0} bot characters; roster-account capacity: {1}.' -f $botSlots, (@($team.rosterAccounts).Count * 10)))
}
$markdown.Add('')
$markdown.Add('Next manual/provisioning gate: create any missing roster accounts, create characters in their allowed slots, then establish two-way Playerbots account links using the security-key workflow.')
$reportPath = Join-Path $ServerRoot 'LEAGUE_ACCOUNT_AUDIT.md'
[System.IO.File]::WriteAllLines($reportPath, $markdown, [System.Text.UTF8Encoding]::new($false))
Write-Output "League account audit: $reportPath"
