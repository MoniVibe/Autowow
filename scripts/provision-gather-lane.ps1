<#
    Idempotently enrolls exactly three neutral RandomPlayerbots as economy workers.
    No character, account, skill, inventory, or world rows are modified.
#>
[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$PlayerbotsConfigPath = '',
    [switch]$Apply
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$expected = @(
    [ordered]@{ guid = 3; name = 'Emealar'; profession = 'Herbalism'; skill = 182; mining_tool = $false },
    [ordered]@{ guid = 6; name = 'Kurl'; profession = 'Mining'; skill = 186; mining_tool = $true },
    [ordered]@{ guid = 49; name = 'Pikli'; profession = 'Herbalism'; skill = 182; mining_tool = $false }
)
$ownerTag = 'economy-gather-v1'
if ([string]::IsNullOrWhiteSpace($PlayerbotsConfigPath)) {
    $PlayerbotsConfigPath = Join-Path $ServerRoot 'server\configs\modules\playerbots.conf'
}
$mysqlPath = Join-Path $ServerRoot 'third_party\mysql\bin\mysql.exe'
$logRoot = Join-Path $ServerRoot 'logs\gather-lane'
[void](New-Item -ItemType Directory -Path $logRoot -Force)
$receipt = Join-Path $logRoot ('provision-' + [DateTimeOffset]::UtcNow.ToString('yyyyMMdd-HHmmssfff') + '.json')

function Get-ConnectionParts {
    $match = @(Select-String -LiteralPath $PlayerbotsConfigPath -Pattern '^\s*PlayerbotsDatabaseInfo\s*=' | Select-Object -First 1)
    if ($match.Count -eq 0) { throw 'PlayerbotsDatabaseInfo was not found.' }
    $line = $match[0].Line
    $value = ($line -replace '^\s*PlayerbotsDatabaseInfo\s*=\s*', '').Trim().Trim('"')
    $parts = $value -split ';', 5
    if ($parts.Count -ne 5) { throw 'PlayerbotsDatabaseInfo has an unexpected format.' }
    return $parts
}

function Invoke-MySql {
    param([Parameter(Mandatory)][string]$Sql)
    if ($Sql -match '(?i)\b(DROP|DELETE|TRUNCATE|ALTER|CREATE|REPLACE)\b') {
        throw 'Forbidden SQL verb in gather provisioner.'
    }
    $parts = Get-ConnectionParts
    $oldPassword = $env:MYSQL_PWD
    $env:MYSQL_PWD = $parts[3]
    try {
        $output = & $mysqlPath '--protocol=tcp' "--host=$($parts[0])" "--port=$($parts[1])" "--user=$($parts[2])" '--default-character-set=utf8mb4' '--batch' '--raw' '--skip-column-names' "--database=$($parts[4])" "--execute=$Sql" 2>&1
        if ($LASTEXITCODE -ne 0) { throw 'MySQL command failed; credentials and connection details were not emitted.' }
        return @($output | Where-Object { $_ -notmatch '^mysql:' } | ForEach-Object { $_.ToString() })
    }
    finally {
        if ($null -eq $oldPassword) { Remove-Item Env:MYSQL_PWD -ErrorAction SilentlyContinue }
        else { $env:MYSQL_PWD = $oldPassword }
    }
}

if (-not (Test-Path -LiteralPath $mysqlPath -PathType Leaf)) { throw "MySQL client not found: $mysqlPath" }
$guidList = ($expected.guid -join ',')
$readinessSql = @"
SELECT c.guid,c.name,pat.account_type,
 COALESCE(MAX(CASE WHEN cs.skill=182 THEN cs.value END),0),
 COALESCE(MAX(CASE WHEN cs.skill=186 THEN cs.value END),0),
 COALESCE((SELECT SUM(ii.count) FROM acore_characters.character_inventory ci JOIN acore_characters.item_instance ii ON ii.guid=ci.item WHERE ci.guid=c.guid AND ii.itemEntry IN (756,778,1819,1893,1959,2901,9465,20723,40772,40892,40893)),0)
FROM acore_characters.characters c
JOIN acore_playerbots.playerbots_account_type pat ON pat.account_id=c.account
LEFT JOIN acore_characters.character_skills cs ON cs.guid=c.guid AND cs.skill IN (182,186)
WHERE c.guid IN ($guidList)
GROUP BY c.guid,c.name,pat.account_type
ORDER BY c.guid;
"@
$rows = @(Invoke-MySql -Sql $readinessSql)
if ($rows.Count -ne $expected.Count) { throw "Expected $($expected.Count) readiness rows; got $($rows.Count)." }
foreach ($item in $expected) {
    $columns = @($rows | Where-Object { ($_ -split "`t")[0] -eq [string]$item.guid } | Select-Object -First 1) -split "`t"
    if ($columns.Count -ne 6 -or $columns[1] -ne $item.name -or [int]$columns[2] -ne 0) {
        throw "GUID $($item.guid) is not the expected account-type-0 RandomPlayerbot."
    }
    $skillValue = if ($item.skill -eq 182) { [int]$columns[3] } else { [int]$columns[4] }
    if ($skillValue -le 0) { throw "GUID $($item.guid) lacks $($item.profession)." }
    if ($item.mining_tool -and [int]$columns[5] -le 0) { throw "GUID $($item.guid) lacks a mining tool." }
}

$existing = @(Invoke-MySql -Sql "SELECT character_guid,COALESCE(team_id,''),affiliation,role,class_plan,profession_one,profession_two,active,IF(retired_at IS NULL,0,1) FROM autowow_league_member WHERE character_guid IN ($guidList) ORDER BY character_guid;")
foreach ($line in $existing) {
    $columns = $line -split "`t"
    if ($columns.Count -ne 9 -or $columns[1] -ne '' -or $columns[2] -ne 'wayfarer' -or $columns[3] -ne 'worker' -or $columns[4] -ne $ownerTag) {
        throw "GUID $($columns[0]) already has non-gather-lane league ownership."
    }
}

if ($Apply) {
    $values = @($expected | ForEach-Object {
        "($($_.guid),NULL,'wayfarer','worker','$ownerTag','$($_.profession)','',1,NULL)"
    }) -join ','
    $sql = "INSERT INTO autowow_league_member (character_guid,team_id,affiliation,role,class_plan,profession_one,profession_two,active,retired_at) VALUES $values ON DUPLICATE KEY UPDATE active=VALUES(active),retired_at=NULL;"
    [void](Invoke-MySql -Sql $sql)
}

$verification = @(Invoke-MySql -Sql "SELECT character_guid,COALESCE(team_id,''),affiliation,role,class_plan,profession_one,active,IF(retired_at IS NULL,0,1) FROM autowow_league_member WHERE character_guid IN ($guidList) ORDER BY character_guid;")
$result = [ordered]@{
    schema = 'autowow.gather-lane.provision.v1'
    mode = if ($Apply) { 'apply' } else { 'preflight' }
    utc = [DateTimeOffset]::UtcNow.ToString('o')
    owner_tag = $ownerTag
    expected_guids = @($expected.guid)
    readiness = @($rows)
    verification = @($verification)
    mutations = if ($Apply) { 1 } else { 0 }
    character_account_skill_inventory_world_writes = 0
}
$result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $receipt -Encoding utf8
$result | ConvertTo-Json -Depth 8
Write-Output "Provision receipt: $receipt"
