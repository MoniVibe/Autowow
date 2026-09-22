[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [uint32]$OwnerGuid = 25,
    [uint32]$HelperGuid = 36,
    [ValidateRange(2,30)][int]$DurationMinutes = 18,
    [ValidateRange(1,30)][int]$PollSeconds = 3,
    [string]$ReceiptPath = '',
    [switch]$Apply
)

. (Join-Path $PSScriptRoot 'Common.ps1')
. (Join-Path $PSScriptRoot 'QuestLogSource.ps1')

$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
$control = Join-Path $PSScriptRoot 'autowow-control.ps1'
$worldConfig = Join-Path $ServerRoot 'server\configs\worldserver.conf'
$playerbotsConfig = Join-Path $ServerRoot 'server\configs\modules\playerbots.conf'
if (-not $ReceiptPath) {
    $ReceiptPath = Join-Path $ServerRoot ('logs\phase1-{0}\q435-escort-live.jsonl' -f (Get-Date -Format 'yyyyMMdd'))
}

foreach ($required in @($control, $worldConfig, $playerbotsConfig)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Required file is missing: $required" }
}
if ($OwnerGuid -eq $HelperGuid) { throw 'OwnerGuid and HelperGuid must be different.' }

function Get-DatabaseParts {
    param([string]$ConfigPath, [string]$Key)
    $pattern = '^\s*{0}\s*=\s*' -f [regex]::Escape($Key)
    $line = (Select-String -LiteralPath $ConfigPath -Pattern $pattern | Select-Object -First 1).Line
    if (-not $line) { throw "$Key was not found in $ConfigPath" }
    $parts = (($line -replace $pattern, '').Trim().Trim('"')) -split ';', 5
    if ($parts.Count -ne 5) { throw "$Key has an unexpected connection-string format." }
    return $parts
}

$characterDb = Get-DatabaseParts -ConfigPath $worldConfig -Key 'CharacterDatabaseInfo'
$playerbotsDb = Get-DatabaseParts -ConfigPath $playerbotsConfig -Key 'PlayerbotsDatabaseInfo'
$mysql = Get-FirstExistingPath -Candidates @(
    (Join-Path $ServerRoot 'third_party\mysql\bin\mysql.exe'),
    'C:\Program Files\MySQL\MySQL Server 8.4\bin\mysql.exe',
    'C:\Program Files\MySQL\MySQL Server 8.0\bin\mysql.exe'
)
if (-not $mysql) { throw 'MySQL CLI was not found.' }

function Invoke-FixtureSql {
    param([string[]]$Connection, [string]$Sql, [switch]$Mutation)
    if ($Mutation) {
        if ($Sql -match '(?i)character_quest|quest_template|queststatus|rewarded') {
            throw 'The fixture refuses every quest-state mutation.'
        }
        if ($Sql -notmatch '(?i)^\s*(START TRANSACTION|INSERT INTO\s+[^;]+autowow_league_member)') {
            throw 'The fixture mutation surface is limited to AutoWow league enrollment.'
        }
    }
    elseif ($Sql.TrimStart() -notmatch '^(?i)SELECT\b') {
        throw 'Read-only fixture queries must start with SELECT.'
    }

    $previous = $env:MYSQL_PWD
    $env:MYSQL_PWD = $Connection[3]
    try {
        $raw = & $mysql --protocol=tcp "--host=$($Connection[0])" "--port=$($Connection[1])" `
            "--user=$($Connection[2])" --default-character-set=utf8mb4 --batch --skip-column-names --execute=$Sql 2>&1
        $exitCode = $LASTEXITCODE
    }
    finally {
        if ($null -eq $previous) { Remove-Item Env:MYSQL_PWD -ErrorAction SilentlyContinue }
        else { $env:MYSQL_PWD = $previous }
    }
    $lines = @($raw | ForEach-Object { $_.ToString() } | Where-Object {
        $_ -and $_ -notmatch '^mysql: Unknown OS character set' -and $_ -notmatch '^mysql: Switching to the default character set'
    })
    if ($exitCode -ne 0) { throw 'Fixture database command failed; credentials were not emitted.' }
    return $lines
}

function Write-Receipt {
    param([string]$Event, [hashtable]$Fields = @{})
    $row = [ordered]@{ timestamp_utc = (Get-Date).ToUniversalTime().ToString('o'); event = $Event }
    foreach ($key in $Fields.Keys) { $row[$key] = $Fields[$key] }
    $directory = Split-Path -Parent $ReceiptPath
    if (-not (Test-Path -LiteralPath $directory)) { New-Item -ItemType Directory -Path $directory -Force | Out-Null }
    [System.IO.File]::AppendAllText($ReceiptPath, (($row | ConvertTo-Json -Compress -Depth 12) + [Environment]::NewLine), [System.Text.UTF8Encoding]::new($false))
}

function Invoke-ControlJson {
    param([string]$Action, [uint32]$Guid = 0, [uint32[]]$Members = @(), [string]$Destination = '',
          [uint32]$Level = 1, [uint32]$SpecIndex = 0, [uint32]$Quality = 2)
    $parameters = @{ Action = $Action; BotGuid = $Guid }
    if ($Members.Count) { $parameters.MemberGuid = $Members }
    if ($Destination) { $parameters.Destination = $Destination }
    if ($Action -eq 'fixture-init') { $parameters.Level = $Level; $parameters.SpecIndex = $SpecIndex; $parameters.Quality = $Quality }
    return ((& $control @parameters | Out-String).Trim() | ConvertFrom-Json)
}

function Get-Quest435 {
    param([uint32]$Guid)
    $log = Send-Bridge -Request "questlog $Guid" | ConvertFrom-Json
    if (-not $log.ok) { throw "questlog failed for ${Guid}: $($log.error)" }
    return @($log.quests | Where-Object { [uint32]$_.id -eq 435 })
}

function Get-Rewarded435 {
    param([uint32]$Guid)
    $rows = @(Invoke-FixtureSql -Connection $characterDb -Sql "SELECT active FROM $($characterDb[4]).character_queststatus_rewarded WHERE guid=$Guid AND quest=435")
    return $rows.Count -gt 0 -and [int]([string]$rows[0]) -eq 1
}

# Planning is deliberately independent of mutable live fixture state. Runtime readiness (characters
# exist and the quest has not already been rewarded) is validated only for an applied run.
$plan = [ordered]@{
    quest = 435
    title = 'Escorting Erland'
    owner_guid = $OwnerGuid
    helper_guid = $HelperGuid
    owner_level = 10
    helper_level = 9
    stage = 'q435-erland'
    duration_minutes = $DurationMinutes
    receipt = $ReceiptPath
    quest_state_writes = 0
    applies = [bool]$Apply
}
if (-not $Apply) {
    [pscustomobject]$plan | ConvertTo-Json -Depth 6
    return
}

$characterRows = @(Invoke-FixtureSql -Connection $characterDb -Sql @"
SELECT guid,name,race,class,level,online FROM $($characterDb[4]).characters
WHERE guid IN ($OwnerGuid,$HelperGuid) ORDER BY guid
"@)
if ($characterRows.Count -ne 2) { throw 'Both fixture characters must already exist.' }
if ((Get-Rewarded435 -Guid $OwnerGuid) -or (Get-Rewarded435 -Guid $HelperGuid)) {
    throw 'q435 is already rewarded on a selected fixture character; choose clean GUIDs.'
}

$existingMembership = @(Invoke-FixtureSql -Connection $playerbotsDb -Sql @"
SELECT character_guid,COALESCE(team_id,''),affiliation,role FROM $($playerbotsDb[4]).autowow_league_member
WHERE character_guid IN ($OwnerGuid,$HelperGuid) ORDER BY character_guid
"@)
foreach ($row in $existingMembership) {
    $columns = $row -split "`t"
    if ($columns.Count -ge 4 -and ($columns[1] -ne 'wayfarers' -or $columns[2] -ne 'wayfarer')) {
        throw "GUID $($columns[0]) already belongs to a non-fixture league roster."
    }
}

$enrollSql = @"
START TRANSACTION;
INSERT INTO $($playerbotsDb[4]).autowow_league_member
  (character_guid,team_id,affiliation,role,class_plan,profession_one,profession_two,active,retired_at)
VALUES
  ($OwnerGuid,'wayfarers','wayfarer','adventurer','escort-owner','','',1,NULL),
  ($HelperGuid,'wayfarers','wayfarer','adventurer','escort-helper','','',1,NULL)
ON DUPLICATE KEY UPDATE team_id='wayfarers',affiliation='wayfarer',role='adventurer',active=1,retired_at=NULL;
COMMIT;
"@
Invoke-FixtureSql -Connection $playerbotsDb -Sql $enrollSql -Mutation | Out-Null
Write-Receipt -Event 'fixture_enrolled' -Fields @{ owner_guid = $OwnerGuid; helper_guid = $HelperGuid }

$online = @((Invoke-ControlJson -Action list).bots | ForEach-Object { [uint32]$_.guid })
foreach ($guid in @($OwnerGuid, $HelperGuid)) {
    if ($guid -notin $online) { [void](Invoke-ControlJson -Action activate -Guid $guid) }
}
$deadline = (Get-Date).AddSeconds(90)
do {
    Start-Sleep -Seconds 2
    $online = @((Invoke-ControlJson -Action list).bots | ForEach-Object { [uint32]$_.guid })
} while (@(@($OwnerGuid, $HelperGuid) | Where-Object { $_ -notin $online }).Count -gt 0 -and (Get-Date) -lt $deadline)
if (@(@($OwnerGuid, $HelperGuid) | Where-Object { $_ -notin $online }).Count -gt 0) { throw 'Fixture bots did not log in.' }

$ownerInit = Invoke-ControlJson -Action fixture-init -Guid $OwnerGuid -Level 10 -SpecIndex 0 -Quality 2
$helperInit = Invoke-ControlJson -Action fixture-init -Guid $HelperGuid -Level 9 -SpecIndex 0 -Quality 2
if (-not $ownerInit.ok -or -not $helperInit.ok) { throw "Fixture initialization failed: $($ownerInit.error) $($helperInit.error)" }
Write-Receipt -Event 'fixture_initialized' -Fields @{ owner = $ownerInit; helper = $helperInit }

$states = @((Invoke-ControlJson -Action list).bots | Where-Object { [uint32]$_.guid -in @($OwnerGuid,$HelperGuid) })
if (@($states | Where-Object { [int]$_.group.members -ne 0 }).Count) { throw 'A fixture bot is already grouped; refusing to disturb a foreign party.' }
[void](Invoke-ControlJson -Action party -Guid $OwnerGuid -Members @($HelperGuid))
[void](Invoke-ControlJson -Action route -Guid $OwnerGuid -Destination 'q435-erland')
Write-Receipt -Event 'fixture_staged' -Fields @{ route = 'q435-erland'; synthetic_credit = 0 }

$accepted = $false
for ($attempt = 1; $attempt -le 4 -and -not $accepted; $attempt++) {
    [void](Invoke-ControlJson -Action quest -Guid $OwnerGuid)
    Start-Sleep -Seconds 3
    $accepted = @(Get-Quest435 -Guid $OwnerGuid).Count -eq 1
}
if (-not $accepted) { throw 'The owner did not accept q435 through normal quest-giver interaction.' }
if (@(Get-Quest435 -Guid $HelperGuid).Count -ne 0) { throw 'The under-level helper unexpectedly accepted q435.' }
Write-Receipt -Event 'quest_accepted' -Fields @{ owner_guid = $OwnerGuid; helper_has_quest = $false; normal_npc_interaction = $true }

# The explicit quest form is sent directly because the shared wrapper reserves QuestId for its quest action.
$explicit = Send-Bridge -Request "quest $OwnerGuid 435" | ConvertFrom-Json
if (-not $explicit.ok) { throw "Explicit q435 directive failed: $($explicit.error)" }

$firstSnapshot = Invoke-ControlJson -Action snapshot -Guid $OwnerGuid
$startX = [double]$firstSnapshot.bot.position.x
$startY = [double]$firstSnapshot.bot.position.y
$sawEscortPhase = $false
$sawComplete = $false
$rewardDirectiveSent = $false
$deadline = (Get-Date).AddMinutes($DurationMinutes)

while ((Get-Date) -lt $deadline) {
    $snapshot = Invoke-ControlJson -Action snapshot -Guid $OwnerGuid
    $objective = Send-Bridge -Request "questobjective $OwnerGuid" | ConvertFrom-Json
    $quest = @(Get-Quest435 -Guid $OwnerGuid)
    if ($objective.ok -and $objective.objective.phase -eq 'escort_event') { $sawEscortPhase = $true }
    if ($quest.Count -eq 1 -and [bool]$quest[0].is_complete) {
        $sawComplete = $true
        if (-not $rewardDirectiveSent) {
            $turnin = Send-Bridge -Request "quest $OwnerGuid 435" | ConvertFrom-Json
            $rewardDirectiveSent = [bool]$turnin.ok
        }
    }
    $rewarded = Get-Rewarded435 -Guid $OwnerGuid
    Write-Receipt -Event 'escort_sample' -Fields @{
        owner_guid = $OwnerGuid
        position = $snapshot.bot.position
        alive = $snapshot.bot.alive
        quest_present = ($quest.Count -eq 1)
        quest_complete = ($quest.Count -eq 1 -and [bool]$quest[0].is_complete)
        rewarded = $rewarded
        objective = $objective
    }
    if ($rewarded) { break }
    Start-Sleep -Seconds $PollSeconds
}

$finalSnapshot = Invoke-ControlJson -Action snapshot -Guid $OwnerGuid
$dx = [double]$finalSnapshot.bot.position.x - $startX
$dy = [double]$finalSnapshot.bot.position.y - $startY
$distance = [Math]::Sqrt(($dx * $dx) + ($dy * $dy))
$ownerRewarded = Get-Rewarded435 -Guid $OwnerGuid
$helperRewarded = Get-Rewarded435 -Guid $HelperGuid
$passed = $ownerRewarded -and -not $helperRewarded -and $sawEscortPhase -and $sawComplete -and $distance -ge 50.0
Write-Receipt -Event 'escort_verdict' -Fields @{
    passed = $passed
    owner_rewarded = $ownerRewarded
    helper_rewarded = $helperRewarded
    saw_escort_phase = $sawEscortPhase
    saw_native_completion = $sawComplete
    owner_distance_yards = [Math]::Round($distance, 2)
    direct_quest_state_writes = 0
    receipt = $ReceiptPath
}

[pscustomobject]@{
    passed = $passed
    owner_guid = $OwnerGuid
    helper_guid = $HelperGuid
    owner_rewarded = $ownerRewarded
    helper_rewarded = $helperRewarded
    saw_escort_phase = $sawEscortPhase
    saw_native_completion = $sawComplete
    owner_distance_yards = [Math]::Round($distance, 2)
    direct_quest_state_writes = 0
    receipt = $ReceiptPath
} | ConvertTo-Json -Depth 6

if (-not $passed) { exit 2 }
