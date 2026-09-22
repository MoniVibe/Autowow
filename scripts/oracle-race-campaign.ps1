[CmdletBinding()]
param(
    [ValidateSet('install','seed','start','run','status','snapshot','report','stop')][string]$Action = 'status',
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [ValidateRange(0,1440)][int]$DurationMinutes = 15,
    [ValidateRange(5,300)][int]$PollSeconds = 30,
    [ValidateRange(1,120)][int]$GatherEveryMinutes = 30,
    [ValidateRange(1,30)][int]$GatherWindowMinutes = 3,
    [string]$RunId = '',
    [string]$WslDistro = 'Ubuntu-24.04',
    [string]$WorldserverBinary = '/root/autowow-quest-giver-core/build-tests/src/server/apps/worldserver',
    [ValidateRange(30,300)][int]$StartupTimeoutSeconds = 180,
    [switch]$EnableGatherRotation,
    [switch]$RestartIfNeeded,
    [switch]$ReleaseLiveMutations,
    [switch]$AsJson
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'Common.ps1')
. (Join-Path $PSScriptRoot 'oracle-race-campaign-lib.ps1')
. (Join-Path $PSScriptRoot 'oracle-controlled-agent-lib.ps1')

$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
$oracleContractPath = Join-Path $ServerRoot 'contracts\oracle-agent-contracts.v1.json'
$configRoot = Join-Path $ServerRoot 'server\configs'
$worldConfig = Join-Path $configRoot 'worldserver.conf'
$playerbotsConfig = Join-Path $configRoot 'modules\playerbots.conf'
$wslPlayerbotsConfig = "\\wsl$\$WslDistro\usr\local\etc\modules\playerbots.conf"
$bridge = Join-Path $PSScriptRoot 'autowow-control.ps1'
$campaignLogRoot = Join-Path $ServerRoot 'logs\oracle-race-campaign'
$activeModuleRoot = Join-Path $ServerRoot 'work\oracle-integration-r13-r6-r5'
$fallbackModuleRoot = Join-Path $ServerRoot 'azerothcore-wotlk\modules\mod-playerbots'
$migration = Get-FirstExistingPath -Candidates @(
    (Join-Path $activeModuleRoot 'data\sql\playerbots\custom\2026_07_19_00_autowow_oracle_race_campaign_v1.sql'),
    (Join-Path $fallbackModuleRoot 'data\sql\playerbots\custom\2026_07_19_00_autowow_oracle_race_campaign_v1.sql')
)
$leagueMigration = Get-FirstExistingPath -Candidates @(
    (Join-Path $activeModuleRoot 'data\sql\playerbots\custom\2026_07_12_00_autowow_league_v0.sql'),
    (Join-Path $fallbackModuleRoot 'data\sql\playerbots\custom\2026_07_12_00_autowow_league_v0.sql')
)
$mysql = Get-FirstExistingPath -Candidates @(
    (Join-Path $ServerRoot 'third_party\mysql\bin\mysql.exe'),
    'C:\Program Files\MySQL\MySQL Server 8.4\bin\mysql.exe',
    'C:\Program Files\MySQL\MySQL Server 8.0\bin\mysql.exe'
)

foreach ($required in @($oracleContractPath, $worldConfig, $playerbotsConfig, $bridge, $migration, $leagueMigration, $mysql)) {
    if (-not $required -or -not (Test-Path -LiteralPath $required)) {
        throw "Required Oracle race campaign file is missing: $required"
    }
}
$oracleControlledAgentValidation = Test-OracleControlledAgentCatalog -Catalog (Get-OracleControlledAgentCatalog -Path $oracleContractPath)
if (-not $oracleControlledAgentValidation.valid) {
    throw ('Oracle controlled-agent catalog is invalid: ' + (@($oracleControlledAgentValidation.errors) -join '; '))
}

function Get-DatabaseParts {
    param(
        [Parameter(Mandatory = $true)][string]$ConfigPath,
        [Parameter(Mandatory = $true)][string]$Key
    )

    $pattern = "^\s*{0}\s*=\s*" -f [regex]::Escape($Key)
    $match = Select-String -LiteralPath $ConfigPath -Pattern $pattern | Select-Object -First 1
    if (-not $match) { throw "$Key was not found in $ConfigPath" }
    $parts = (($match.Line -replace $pattern, '').Trim().Trim('"')) -split ';', 5
    if ($parts.Count -ne 5) { throw "$Key has an unexpected connection-string format." }
    return $parts
}

$characterDb = @(Get-DatabaseParts -ConfigPath $worldConfig -Key 'CharacterDatabaseInfo')
$playerbotsDb = @(Get-DatabaseParts -ConfigPath $playerbotsConfig -Key 'PlayerbotsDatabaseInfo')

function ConvertTo-SqlLiteral {
    param([AllowNull()][object]$Value)
    if ($null -eq $Value) { return 'NULL' }
    return "'" + ([string]$Value).Replace("'", "''") + "'"
}

function Invoke-OracleSql {
    param(
        [Parameter(Mandatory = $true)][string]$Sql,
        [Parameter(Mandatory = $true)][string[]]$DatabaseParts,
        [switch]$Silent
    )

    $priorPassword = $env:MYSQL_PWD
    $env:MYSQL_PWD = $DatabaseParts[3]
    try {
        $raw = & $mysql --protocol=tcp "--host=$($DatabaseParts[0])" "--port=$($DatabaseParts[1])" `
            "--user=$($DatabaseParts[2])" "--database=$($DatabaseParts[4])" `
            --default-character-set=utf8mb4 --batch --raw --skip-column-names --execute=$Sql 2>&1
        $exitCode = $LASTEXITCODE
    }
    finally {
        if ($null -eq $priorPassword) { Remove-Item Env:MYSQL_PWD -ErrorAction SilentlyContinue }
        else { $env:MYSQL_PWD = $priorPassword }
    }

    $lines = @($raw | ForEach-Object { $_.ToString() } | Where-Object {
        $_ -and $_ -notmatch '^mysql: Unknown OS character set' -and
        $_ -notmatch '^mysql: Switching to the default character set'
    })
    if ($exitCode -ne 0) {
        $safeError = @($lines | Where-Object {
            $_ -notmatch '(?i)password|MYSQL_PWD|;[^;]+;[^;]+;[^;]+;'
        }) -join ' '
        if ([string]::IsNullOrWhiteSpace($safeError)) { $safeError = 'no database error text returned' }
        throw "Oracle race campaign database command failed: $safeError"
    }
    if (-not $Silent) { return $lines }
}

function Invoke-OracleBridgeList {
    $output = & $bridge -Action list
    $raw = @($output | ForEach-Object { $_.ToString() } | Select-Object -Last 1)
    if ($raw.Count -eq 0 -or [string]::IsNullOrWhiteSpace([string]$raw[0])) {
        throw 'AutoWow bridge returned no list response.'
    }
    $parsed = $raw[0] | ConvertFrom-Json
    if (-not $parsed.ok) { throw 'AutoWow bridge list response was not ok.' }
    return $parsed
}

function Invoke-OracleBridgeQuestLog {
    param([Parameter(Mandatory = $true)][uint32]$Guid)

    try {
        $output = & $bridge -Action questlog -BotGuid $Guid
        $raw = @($output | ForEach-Object { $_.ToString() } | Select-Object -Last 1)
        if ($raw.Count -eq 0 -or [string]::IsNullOrWhiteSpace([string]$raw[0])) { return $null }
        $parsed = $raw[0] | ConvertFrom-Json
        if (-not $parsed.ok) { return $null }
        return $parsed
    }
    catch {
        return $null
    }
}

function Invoke-OracleBridgeAction {
    param(
        [Parameter(Mandatory = $true)][ValidateSet('activate','deactivate','independent','deploy')][string]$BridgeAction,
        [Parameter(Mandatory = $true)][uint32]$Guid
    )

    $output = & $bridge -Action $BridgeAction -BotGuid $Guid
    $raw = @($output | ForEach-Object { $_.ToString() } | Select-Object -Last 1)
    if ($raw.Count -eq 0 -or [string]::IsNullOrWhiteSpace([string]$raw[0])) {
        throw "Bridge '$BridgeAction' returned no response for $Guid."
    }
    $parsed = $raw[0] | ConvertFrom-Json
    if (-not $parsed.ok) {
        $message = if ($parsed.error) { [string]$parsed.error } else { 'unknown bridge error' }
        throw "Bridge '$BridgeAction' failed for $Guid`: $message"
    }
    return $parsed
}

function Invoke-OracleBridgeProfessionEconomy {
    param([Parameter(Mandatory = $true)][uint32]$Guid)

    $output = & $bridge -Action professioneconomy -BotGuid $Guid
    $raw = @($output | ForEach-Object { $_.ToString() } | Select-Object -Last 1)
    if ($raw.Count -eq 0 -or [string]::IsNullOrWhiteSpace([string]$raw[0])) {
        throw "Bridge 'professioneconomy' returned no response for $Guid."
    }
    $parsed = $raw[0] | ConvertFrom-Json
    if (-not $parsed.ok) {
        $message = if ($parsed.error) { [string]$parsed.error } else { 'unknown bridge error' }
        throw "Bridge 'professioneconomy' failed for $Guid`: $message"
    }
    return $parsed
}

function ConvertTo-Int64OrZero {
    param([AllowNull()][object]$Value)
    if ($null -eq $Value -or [string]::IsNullOrWhiteSpace([string]$Value)) { return [int64]0 }
    try { return [int64]$Value } catch { return [int64]0 }
}

function Get-OptionalProperty {
    param(
        [AllowNull()][object]$InputObject,
        [Parameter(Mandatory = $true)][string]$Name,
        [AllowNull()][object]$Default = $null
    )
    if ($null -eq $InputObject) { return $Default }
    $property = $InputObject.PSObject.Properties[$Name]
    if ($null -eq $property) { return $Default }
    if ($null -eq $property.Value) { return $Default }
    return $property.Value
}

function Get-BridgeBotMap {
    param([Parameter(Mandatory = $true)][object]$ListResponse)
    $map = @{}
    foreach ($bot in @($ListResponse.bots)) {
        if ($null -eq $bot) { continue }
        $guid = [uint32](ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $bot -Name 'guid' -Default 0))
        if ($guid -ne 0) { $map[$guid] = $bot }
    }
    return $map
}

function ConvertTo-OracleRows {
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][string[]]$Lines,
        [Parameter(Mandatory = $true)][int]$Width
    )
    $rows = [System.Collections.Generic.List[object]]::new()
    foreach ($line in $Lines) {
        $parts = $line -split "`t"
        if ($parts.Count -lt $Width) { continue }
        [void]$rows.Add($parts)
    }
    return @($rows)
}

function Get-CharacterRows {
    param([Parameter(Mandatory = $true)][uint32[]]$Guids)
    $idList = (@($Guids | Sort-Object -Unique) -join ',')
    $sql = "SELECT guid,name,race,class,level,xp,money,online,map,zone FROM $($characterDb[4]).characters WHERE guid IN ($idList) ORDER BY guid;"
    $rows = @{}
    foreach ($parts in @(ConvertTo-OracleRows -Lines @(Invoke-OracleSql -DatabaseParts $characterDb -Sql $sql) -Width 10)) {
        $guid = [uint32](ConvertTo-Int64OrZero $parts[0])
        $rows[$guid] = [pscustomobject][ordered]@{
            guid = $guid; name = [string]$parts[1]; race = [int](ConvertTo-Int64OrZero $parts[2]);
            class = [int](ConvertTo-Int64OrZero $parts[3]); level = [int](ConvertTo-Int64OrZero $parts[4]);
            xp = [uint64](ConvertTo-Int64OrZero $parts[5]); money = [uint64](ConvertTo-Int64OrZero $parts[6]);
            online = ([int](ConvertTo-Int64OrZero $parts[7]) -ne 0); map = [int](ConvertTo-Int64OrZero $parts[8]);
            zone = [int](ConvertTo-Int64OrZero $parts[9])
        }
    }
    return $rows
}

function Get-CountMap {
    param(
        [Parameter(Mandatory = $true)][string]$Table,
        [Parameter(Mandatory = $true)][string]$ValueExpression,
        [Parameter(Mandatory = $true)][uint32[]]$Guids
    )
    $idList = (@($Guids | Sort-Object -Unique) -join ',')
    $sql = "SELECT guid,$ValueExpression FROM $($characterDb[4]).$Table WHERE guid IN ($idList) GROUP BY guid;"
    $map = @{}
    foreach ($parts in @(ConvertTo-OracleRows -Lines @(Invoke-OracleSql -DatabaseParts $characterDb -Sql $sql) -Width 2)) {
        $map[[uint32](ConvertTo-Int64OrZero $parts[0])] = [uint64](ConvertTo-Int64OrZero $parts[1])
    }
    return $map
}

function Get-ProfessionRows {
    param([Parameter(Mandatory = $true)][uint32[]]$Guids)
    $idList = (@($Guids | Sort-Object -Unique) -join ',')
    $skillIds = @((Get-OracleProfessionSkillIds).Values) -join ','
    $sql = "SELECT guid,skill,value,max FROM $($characterDb[4]).character_skills WHERE guid IN ($idList) AND skill IN ($skillIds) ORDER BY guid,skill;"
    $rows = @{}
    foreach ($parts in @(ConvertTo-OracleRows -Lines @(Invoke-OracleSql -DatabaseParts $characterDb -Sql $sql) -Width 4)) {
        $guid = [uint32](ConvertTo-Int64OrZero $parts[0])
        if (-not $rows.ContainsKey($guid)) { $rows[$guid] = [System.Collections.Generic.List[object]]::new() }
        $skillId = [int](ConvertTo-Int64OrZero $parts[1])
        $skillName = 'unknown'
        foreach ($entry in (Get-OracleProfessionSkillIds).GetEnumerator()) {
            if ([int]$entry.Value -eq $skillId) { $skillName = [string]$entry.Key; break }
        }
        [void]$rows[$guid].Add([pscustomobject][ordered]@{
            skill_id = $skillId; name = $skillName; kind = 'primary';
            value = [int](ConvertTo-Int64OrZero $parts[2]); max = [int](ConvertTo-Int64OrZero $parts[3])
        })
    }
    return $rows
}

function Get-SeedMemberRows {
    param([Parameter(Mandatory = $true)][uint32[]]$Guids)
    $idList = (@($Guids | Sort-Object -Unique) -join ',')
    $sql = "SELECT character_guid,COALESCE(team_id,''),affiliation,COALESCE(retired_at,'') FROM $($playerbotsDb[4]).autowow_league_member WHERE character_guid IN ($idList);"
    $rows = @{}
    foreach ($parts in @(ConvertTo-OracleRows -Lines @(Invoke-OracleSql -DatabaseParts $playerbotsDb -Sql $sql) -Width 4)) {
        $guid = [uint32](ConvertTo-Int64OrZero $parts[0])
        $rows[$guid] = [pscustomobject][ordered]@{ guid = $guid; team = [string]$parts[1]; affiliation = [string]$parts[2]; retired_at = [string]$parts[3] }
    }
    return $rows
}

function Install-OracleSchema {
    $leagueSql = "USE ``$($playerbotsDb[4])``;`n" + (Get-Content -LiteralPath $leagueMigration -Raw)
    $oracleSql = "USE ``$($playerbotsDb[4])``;`n" + (Get-Content -LiteralPath $migration -Raw)
    Invoke-OracleSql -DatabaseParts $playerbotsDb -Sql $leagueSql -Silent
    Invoke-OracleSql -DatabaseParts $playerbotsDb -Sql $oracleSql -Silent
    $tables = @(Invoke-OracleSql -DatabaseParts $playerbotsDb -Sql "SELECT table_name FROM information_schema.tables WHERE table_schema=$(ConvertTo-SqlLiteral $playerbotsDb[4]) AND table_name IN ('autowow_league_member','autowow_oracle_race_seed','autowow_oracle_progress','autowow_oracle_failure') ORDER BY table_name;")
    if ($tables.Count -ne 4) { throw "Oracle race campaign schema verification failed; found $($tables.Count) of four required tables." }
    return $tables
}

function Assert-RaceSeedCharacters {
    $definitions = @(Get-OracleRaceSeedDefinitions)
    $guids = @($definitions | ForEach-Object { [uint32]$_.guid })
    $characters = Get-CharacterRows -Guids $guids
    foreach ($definition in $definitions) {
        $guid = [uint32]$definition.guid
        if (-not $characters.ContainsKey($guid)) { throw "Race seed character $guid ($($definition.race_name)) does not exist." }
        $character = $characters[$guid]
        if ([int]$character.race -ne [int]$definition.race_id) {
            throw "Character $guid is race $($character.race), expected $($definition.race_id) ($($definition.race_name))."
        }
        if ([int]$character.class -ne [int]$definition.class_id) {
            throw "Character $guid is class $($character.class), expected $($definition.class_id) ($($definition.class_name))."
        }
        if ([int]$character.level -lt 1) { throw "Character $guid has an invalid level $($character.level)." }
    }
    return $characters
}

function Seed-OracleRaceCampaign {
    Install-OracleSchema | Out-Null
    $characters = Assert-RaceSeedCharacters
    $definitions = @(Get-OracleRaceSeedDefinitions)
    $guids = @($definitions | ForEach-Object { [uint32]$_.guid })
    $members = Get-SeedMemberRows -Guids $guids

    foreach ($definition in $definitions) {
        $guid = [uint32]$definition.guid
        if ($members.ContainsKey($guid)) {
            $member = $members[$guid]
            if ([string]$member.team -ne 'race-seeds' -and [string]::IsNullOrWhiteSpace([string]$member.retired_at)) {
                throw "Refusing to claim character ${guid}: it is already enrolled in team '$($member.team)'."
            }
        }
    }

    $raceList = (@($definitions | ForEach-Object { [int]$_.race_id }) -join ',')
    $existingRaceRows = @(Invoke-OracleSql -DatabaseParts $playerbotsDb -Sql "SELECT character_guid,race_id FROM $($playerbotsDb[4]).autowow_oracle_race_seed WHERE race_id IN ($raceList);")
    foreach ($parts in @(ConvertTo-OracleRows -Lines $existingRaceRows -Width 2)) {
        $existingGuid = [uint32](ConvertTo-Int64OrZero $parts[0])
        $existingRace = [int](ConvertTo-Int64OrZero $parts[1])
        $expected = @($definitions | Where-Object { [int]$_.race_id -eq $existingRace } | Select-Object -First 1)
        if ($expected.Count -eq 1 -and $existingGuid -ne [uint32]$expected[0].guid) {
            throw "Race $existingRace is already assigned to character $existingGuid; refusing a second seed."
        }
    }

    $sql = [System.Text.StringBuilder]::new()
    [void]$sql.AppendLine('START TRANSACTION;')
    [void]$sql.AppendLine("INSERT INTO $($playerbotsDb[4]).autowow_league_team (team_id,display_name,faction,roster_cap,worker_cap,treasury_copper) VALUES ('race-seeds','Oracle Race Seeds','Neutral',10,10,0) ON DUPLICATE KEY UPDATE display_name=VALUES(display_name),faction=VALUES(faction),roster_cap=VALUES(roster_cap),worker_cap=VALUES(worker_cap);")
    foreach ($definition in $definitions) {
        $guid = [uint32]$definition.guid
        $character = $characters[$guid]
        $classPlan = ConvertTo-SqlLiteral $definition.class_plan
        $professionOne = ConvertTo-SqlLiteral $definition.profession_one
        $professionTwo = ConvertTo-SqlLiteral $definition.profession_two
        [void]$sql.AppendLine("INSERT INTO $($playerbotsDb[4]).autowow_league_member (character_guid,team_id,affiliation,role,class_plan,profession_one,profession_two,active,retired_at) VALUES ($guid,'race-seeds','wayfarer','adventurer',$classPlan,$professionOne,$professionTwo,1,NULL) ON DUPLICATE KEY UPDATE team_id='race-seeds',affiliation='wayfarer',role='adventurer',class_plan=$classPlan,profession_one=$professionOne,profession_two=$professionTwo,active=1,retired_at=NULL;")
        [void]$sql.AppendLine("INSERT INTO $($playerbotsDb[4]).autowow_oracle_race_seed (character_guid,race_id,race_name,faction,class_id,class_plan,profession_one,profession_two,cohort,oracle_enabled,active,last_level,last_xp) VALUES ($guid,$([int]$definition.race_id),$(ConvertTo-SqlLiteral $definition.race_name),$(ConvertTo-SqlLiteral $definition.faction),$([int]$definition.class_id),$classPlan,$professionOne,$professionTwo,'oracle-race-seeds',1,1,$([int]$character.level),$([uint64]$character.xp)) ON DUPLICATE KEY UPDATE race_id=VALUES(race_id),race_name=VALUES(race_name),faction=VALUES(faction),class_id=VALUES(class_id),class_plan=VALUES(class_plan),profession_one=VALUES(profession_one),profession_two=VALUES(profession_two),oracle_enabled=1,active=1;")
    }
    [void]$sql.AppendLine('COMMIT;')
    Invoke-OracleSql -DatabaseParts $playerbotsDb -Sql $sql.ToString() -Silent
    return [ordered]@{ seeded = $definitions.Count; guids = @($guids | Sort-Object); characters = $characters }
}

function Get-ConfigOracleGuids {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return @() }
    $match = Select-String -LiteralPath $Path -Pattern '^\s*AutoWow\.OracleRuntime\.BotGuids\s*=\s*' | Select-Object -First 1
    if (-not $match) { return @() }
    $value = ($match.Line -replace '^\s*AutoWow\.OracleRuntime\.BotGuids\s*=\s*', '').Trim().Trim('"')
    if ([string]::IsNullOrWhiteSpace($value)) { return @() }
    $guids = [System.Collections.Generic.List[uint32]]::new()
    foreach ($token in ($value -split '[,\s]+')) {
        if ([string]::IsNullOrWhiteSpace($token)) { continue }
        [uint32]$parsed = 0
        if ([uint32]::TryParse($token, [ref]$parsed) -and $parsed -ne 0) { [void]$guids.Add($parsed) }
    }
    return @($guids)
}

function Ensure-OracleRuntimeConfig {
    $seedGuids = @(Get-OracleRaceSeedGuidList)
    $existing = @((Get-ConfigOracleGuids -Path $playerbotsConfig) + (Get-ConfigOracleGuids -Path $wslPlayerbotsConfig))
    $allowlist = @($existing + $seedGuids | Sort-Object -Unique)
    if ($allowlist.Count -gt 256) { throw "Oracle runtime allowlist exceeds the compiled capacity: $($allowlist.Count)." }
    $guidValue = '"' + ($allowlist -join ',') + '"'
    $desired = [ordered]@{
        'AutoWow.OracleRuntime.Enabled' = '1'
        'AutoWow.OracleRuntime.BotGuids' = $guidValue
        'AutoWow.OracleRuntime.CadenceMs' = '1000'
        'AutoWow.OracleRuntime.MaxBots' = [string]$allowlist.Count
        'AutoWow.OracleRuntime.LeaseTtlTicks' = '3'
    }

    New-Item -ItemType Directory -Path (Join-Path $campaignLogRoot 'config-backups') -Force | Out-Null
    $backups = [System.Collections.Generic.List[string]]::new()
    $changedPaths = [System.Collections.Generic.List[string]]::new()
    $configPaths = [System.Collections.Generic.List[string]]::new()
    [void]$configPaths.Add($playerbotsConfig)
    if (Test-Path -LiteralPath $wslPlayerbotsConfig) { [void]$configPaths.Add($wslPlayerbotsConfig) }
    foreach ($path in @($configPaths)) {
        $before = (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash
        $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
        $pathLabel = if ([string]$path -eq [string]$playerbotsConfig) { 'windows' } else { 'wsl' }
        $backupStem = $pathLabel + '-' + (Split-Path -Leaf $path) + '.' + $stamp
        $temporaryBackup = Join-Path (Join-Path $campaignLogRoot 'config-backups') ($backupStem + '.tmp')
        Copy-Item -LiteralPath $path -Destination $temporaryBackup -Force
        foreach ($entry in $desired.GetEnumerator()) {
            Set-ConfigValue -Path $path -Key $entry.Key -Value $entry.Value
        }
        $after = (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash
        if ($before -ne $after) {
            $backup = Join-Path (Join-Path $campaignLogRoot 'config-backups') ($backupStem + '.bak')
            Move-Item -LiteralPath $temporaryBackup -Destination $backup -Force
            [void]$backups.Add($backup)
            [void]$changedPaths.Add($path)
        }
        else {
            Remove-Item -LiteralPath $temporaryBackup -Force
        }
    }
    return [ordered]@{
        allowlist = @($allowlist)
        max_bots = $allowlist.Count
        changed = ($changedPaths.Count -gt 0)
        changed_paths = @($changedPaths)
        backups = @($backups)
        windows_config = $playerbotsConfig
        wsl_config = $wslPlayerbotsConfig
    }
}

function Test-WorldserverRunning {
    $output = @(& wsl.exe -d $WslDistro -u root -- pgrep -x worldserver 2>$null)
    return ($LASTEXITCODE -eq 0 -and $output.Count -gt 0)
}

function Restart-OracleWorldserver {
    if (Test-WorldserverRunning) {
        & (Join-Path $PSScriptRoot 'stop-phase1-wsl-worldserver.ps1') -ServerRoot $ServerRoot -WslDistro $WslDistro | Out-Host
    }
    & (Join-Path $PSScriptRoot 'start-phase1-wsl-worldserver.ps1') -ServerRoot $ServerRoot -WslDistro $WslDistro `
        -WorldserverBinary $WorldserverBinary -StartupTimeoutSeconds $StartupTimeoutSeconds | Out-Host
}

function Wait-OracleSeedsOnline {
    param([Parameter(Mandatory = $true)][uint32[]]$Guids, [int]$TimeoutSeconds = 180)
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    do {
        $list = Invoke-OracleBridgeList
        $online = @((Get-BridgeBotMap -ListResponse $list).Keys | ForEach-Object { [uint32]$_ })
        if (@($Guids | Where-Object { $_ -notin $online }).Count -eq 0) { return $list }
        Start-Sleep -Seconds 2
    } while ((Get-Date) -lt $deadline)
    $missing = @($Guids | Where-Object { $_ -notin $online }) -join ','
    throw "Timed out waiting for Oracle race seeds to log in: $missing"
}

function Activate-OracleSeeds {
    param([Parameter(Mandatory = $true)][uint32[]]$Guids)
    $errors = [System.Collections.Generic.List[object]]::new()
    $list = Invoke-OracleBridgeList
    $map = Get-BridgeBotMap -ListResponse $list
    foreach ($guid in @($Guids | Sort-Object)) {
        if ($map.ContainsKey([uint32]$guid)) { continue }
        try {
            Invoke-OracleBridgeAction -BridgeAction activate -Guid $guid | Out-Null
        }
        catch {
            [void]$errors.Add([pscustomobject]@{ guid = [uint32]$guid; error = $_.Exception.Message })
        }
        Start-Sleep -Milliseconds 250
    }
    if ($errors.Count -gt 0) { return [ordered]@{ activated = $Guids.Count - $errors.Count; errors = @($errors) } }
    return [ordered]@{ activated = $Guids.Count; errors = @() }
}

function Arm-OracleSeeds {
    param([Parameter(Mandatory = $true)][uint32[]]$Guids)
    $armed = [System.Collections.Generic.List[uint32]]::new()
    $errors = [System.Collections.Generic.List[object]]::new()
    $professionChecks = [System.Collections.Generic.List[object]]::new()
    foreach ($guid in @($Guids | Sort-Object)) {
        try {
            Invoke-OracleBridgeAction -BridgeAction independent -Guid $guid | Out-Null
            [void]$armed.Add([uint32]$guid)

            # `independent` acknowledges an AI mode change, not profession realization. Capture a
            # read-only profession snapshot immediately so a successful bridge response cannot be
            # mistaken for proof that the declared campaign pair was learned.
            $definition = @(Get-OracleRaceSeedByGuid -Guid $guid | Select-Object -First 1)
            $telemetry = Invoke-OracleBridgeProfessionEconomy -Guid $guid
            $assessment = if ($definition.Count -eq 1) {
                Get-OracleProfessionPlanAssessment -Definition $definition[0] -Telemetry $telemetry
            }
            else {
                [ordered]@{ status = 'unavailable'; source = 'seed_definition_missing'; desired = @(); observed = @(); missing = @() }
            }
            [void]$professionChecks.Add([pscustomobject][ordered]@{
                guid = [uint32]$guid
                status = [string]$assessment.status
                source = [string]$assessment.source
                desired = @($assessment.desired)
                observed = @($assessment.observed)
                missing = @($assessment.missing)
            })
        }
        catch {
            [void]$errors.Add([pscustomobject]@{ guid = [uint32]$guid; error = $_.Exception.Message })
        }
        Start-Sleep -Milliseconds 150
    }
    return [ordered]@{ armed = @($armed); errors = @($errors); profession_checks = @($professionChecks) }
}

function Get-CurrentOracleFailureRows {
    $sql = "SELECT character_guid,domain,failure_code,status,occurrences,last_level,last_seen_at FROM $($playerbotsDb[4]).autowow_oracle_failure ORDER BY character_guid,domain,failure_code;"
    $result = [System.Collections.Generic.List[object]]::new()
    foreach ($parts in @(ConvertTo-OracleRows -Lines @(Invoke-OracleSql -DatabaseParts $playerbotsDb -Sql $sql) -Width 7)) {
        [void]$result.Add([pscustomobject][ordered]@{
            guid = [uint32](ConvertTo-Int64OrZero $parts[0]); domain = [string]$parts[1]; code = [string]$parts[2];
            status = [string]$parts[3]; occurrences = [uint64](ConvertTo-Int64OrZero $parts[4]);
            level = [int](ConvertTo-Int64OrZero $parts[5]); last_seen_at = [string]$parts[6]
        })
    }
    return @($result)
}

function Get-OracleFailureObject {
    param(
        [Parameter(Mandatory = $true)][uint32]$Guid,
        [Parameter(Mandatory = $true)][string]$Domain,
        [Parameter(Mandatory = $true)][string]$Code,
        [Parameter(Mandatory = $true)][int]$Level,
        [Parameter(Mandatory = $true)][hashtable]$Payload
    )
    return [pscustomobject][ordered]@{
        guid = $Guid; domain = $Domain; code = (ConvertTo-OracleFailureCode -Domain $Domain -Code $Code);
        level = $Level; payload = $Payload
    }
}

function Add-OracleFailure {
    param([Parameter(Mandatory = $true)][object]$Failure)
    $json = $Failure.payload | ConvertTo-Json -Compress -Depth 12
    $sql = "INSERT INTO $($playerbotsDb[4]).autowow_oracle_failure (character_guid,domain,failure_code,status,occurrences,last_level,latest_payload_json) VALUES ($([uint32]$Failure.guid),$(ConvertTo-SqlLiteral $Failure.domain),$(ConvertTo-SqlLiteral $Failure.code),'open',1,$([int]$Failure.level),$(ConvertTo-SqlLiteral $json)) ON DUPLICATE KEY UPDATE status='open',occurrences=occurrences+1,last_seen_at=UTC_TIMESTAMP(),last_level=VALUES(last_level),latest_payload_json=VALUES(latest_payload_json);"
    Invoke-OracleSql -DatabaseParts $playerbotsDb -Sql $sql -Silent
}

function Get-OracleFailuresForState {
    param(
        [Parameter(Mandatory = $true)][object]$Definition,
        [AllowNull()][object]$Character,
        [AllowNull()][object]$Bot,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]]$ProfessionRows,
        [Parameter(Mandatory = $true)][uint64]$QuestCount,
        [Parameter(Mandatory = $true)][bool]$QuestLogAvailable,
        [Parameter(Mandatory = $true)][uint64]$TalentCount,
        [Parameter(Mandatory = $true)][hashtable]$History
    )

    $guid = [uint32]$Definition.guid
    $level = if ($null -eq $Character) { 1 } else { [int]$Character.level }
    $failures = [System.Collections.Generic.List[object]]::new()
    if ($null -eq $Character) {
        [void]$failures.Add((Get-OracleFailureObject -Guid $guid -Domain 'identity' -Code 'character_missing' -Level 1 -Payload @{ race = $Definition.race_name }))
        return @($failures)
    }

    if ($null -eq $Bot) {
        [void]$failures.Add((Get-OracleFailureObject -Guid $guid -Domain 'activation' -Code 'offline' -Level $level -Payload @{ expected = 'online'; character_online = [bool]$Character.online }))
        return @($failures)
    }
    $liveProgress = Get-OptionalProperty -InputObject $Bot -Name 'progress' -Default $null
    $level = [int](ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $liveProgress -Name 'level' -Default $level))
    $liveXp = [uint64](ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $liveProgress -Name 'xp' -Default $Character.xp))
    $liveMoney = [uint64](ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $liveProgress -Name 'money_copper' -Default $Character.money))
    if ([bool](Get-OptionalProperty -InputObject $Bot -Name 'paused' -Default $false)) {
        [void]$failures.Add((Get-OracleFailureObject -Guid $guid -Domain 'runtime' -Code 'paused' -Level $level -Payload @{ action = Get-OptionalProperty -InputObject $Bot -Name 'action' -Default '' }))
    }
    if (-not [bool](Get-OptionalProperty -InputObject $Bot -Name 'alive' -Default $true)) {
        [void]$failures.Add((Get-OracleFailureObject -Guid $guid -Domain 'survival' -Code 'dead' -Level $level -Payload @{ state = Get-OptionalProperty -InputObject $Bot -Name 'state' -Default '' }))
    }
    if (-not $QuestLogAvailable) {
        [void]$failures.Add((Get-OracleFailureObject -Guid $guid -Domain 'quest' -Code 'telemetry_unavailable' -Level $level -Payload @{ source = 'bridge.questlog'; fallback = 'character_queststatus' }))
    }

    $nonCombat = @((Get-OptionalProperty -InputObject (Get-OptionalProperty -InputObject $Bot -Name 'strategies' -Default $null) -Name 'non_combat' -Default @())) | ForEach-Object { [string]$_ }
    if ($nonCombat -notcontains 'new rpg') {
        [void]$failures.Add((Get-OracleFailureObject -Guid $guid -Domain 'ai' -Code 'native_strategy_missing' -Level $level -Payload @{ strategies = $nonCombat }))
    }

    $signature = '{0}|{1}|{2}|{3}|{4}|{5}|{6}' -f $level, $liveXp, $liveMoney, $QuestCount, $TalentCount, (Get-OptionalProperty -InputObject $Bot -Name 'state' -Default ''), (Get-OptionalProperty -InputObject $Bot -Name 'action' -Default '')
    $now = Get-Date
    if (-not $History.ContainsKey('signature') -or [string]$History.signature -ne $signature) {
        $History.signature = $signature
        $History.last_progress = $now
    }
    $noProgressSeconds = ($now - [datetime]$History.last_progress).TotalSeconds
    $activity = Get-OptionalProperty -InputObject $Bot -Name 'activity' -Default $null
    if ($noProgressSeconds -ge 120 -and $null -ne $activity -and [bool](Get-OptionalProperty -InputObject $activity -Name 'allowed' -Default $false)) {
        [void]$failures.Add((Get-OracleFailureObject -Guid $guid -Domain 'progression' -Code 'no_progress' -Level $level -Payload @{ seconds = [int]$noProgressSeconds; signature = $signature }))
    }

    if ($level -ge 5) {
        $skillIds = Get-OracleProfessionSkillIds
        $actualNames = @($ProfessionRows | ForEach-Object { [string]$_.name })
        $desired = @([string]$Definition.profession_one, [string]$Definition.profession_two)
        $assessment = Get-OracleProfessionPlanAssessment -Definition $Definition -Telemetry ([pscustomobject]@{ professions = @($ProfessionRows) }) -Source 'character_skills.read_only'
        if ($assessment.status -ne 'realized') {
            [void]$failures.Add((Get-OracleFailureObject -Guid $guid -Domain 'profession' -Code 'plan_not_realized' -Level $level -Payload @{ desired = @($Definition.profession_one, $Definition.profession_two); observed = $actualNames; missing = @($assessment.missing); status = [string]$assessment.status; source = [string]$assessment.source; skill_ids = $skillIds }))
        }
    }
    if ($level -ge 10 -and $TalentCount -eq 0) {
        [void]$failures.Add((Get-OracleFailureObject -Guid $guid -Domain 'talent' -Code 'no_talents_observed' -Level $level -Payload @{ level = $level }))
    }

    $gatherRoute = Get-OptionalProperty -InputObject $Bot -Name 'gather_route' -Default $null
    if ($null -ne $gatherRoute -and [bool](Get-OptionalProperty -InputObject $gatherRoute -Name 'explicit_worker' -Default $false)) {
        $idleReason = [string](Get-OptionalProperty -InputObject $gatherRoute -Name 'idle_reason' -Default 'none')
        if (-not [string]::IsNullOrWhiteSpace($idleReason) -and $idleReason -ne 'none') {
            [void]$failures.Add((Get-OracleFailureObject -Guid $guid -Domain 'gather' -Code $idleReason -Level $level -Payload @{ route = $gatherRoute }))
        }
    }

    if ($level -ge 5 -and -not $History.ContainsKey('craft_gap_recorded')) {
        $History.craft_gap_recorded = $true
        [void]$failures.Add((Get-OracleFailureObject -Guid $guid -Domain 'craft' -Code 'evidence_unavailable' -Level $level -Payload @{ reason = 'No deployed per-bot craft receipt or execution verb is exposed yet.'; next_patch = 'wire bounded OracleCraftExecutor to a bridge/runtime action' }))
    }
    return @($failures)
}

function Capture-OracleProgress {
    param([Parameter(Mandatory = $true)][string]$EffectiveRunId)
    $definitions = @(Get-OracleRaceSeedDefinitions)
    $guids = @($definitions | ForEach-Object { [uint32]$_.guid })
    $characters = Get-CharacterRows -Guids $guids
    $questCounts = Get-CountMap -Table 'character_queststatus' -ValueExpression 'COUNT(*)' -Guids $guids
    $talentCounts = Get-CountMap -Table 'character_talent' -ValueExpression 'COUNT(*)' -Guids $guids
    $spellCounts = Get-CountMap -Table 'character_spell' -ValueExpression 'COUNT(*)' -Guids $guids
    $professionRows = Get-ProfessionRows -Guids $guids
    $bridgeList = Invoke-OracleBridgeList
    $bridgeMap = Get-BridgeBotMap -ListResponse $bridgeList
    $snapshots = [System.Collections.Generic.List[object]]::new()
    $allFailures = [System.Collections.Generic.List[object]]::new()

    foreach ($definition in $definitions) {
        $guid = [uint32]$definition.guid
        $character = if ($characters.ContainsKey($guid)) { $characters[$guid] } else { $null }
        $bot = if ($bridgeMap.ContainsKey($guid)) { $bridgeMap[$guid] } else { $null }
        $questCount = if ($questCounts.ContainsKey($guid)) { [uint64]$questCounts[$guid] } else { [uint64]0 }
        $talentCount = if ($talentCounts.ContainsKey($guid)) { [uint64]$talentCounts[$guid] } else { [uint64]0 }
        $spellCount = if ($spellCounts.ContainsKey($guid)) { [uint64]$spellCounts[$guid] } else { [uint64]0 }
        $profession = @()
        if ($professionRows.ContainsKey($guid)) { $profession = @($professionRows[$guid]) }
        $liveQuestLog = if ($null -ne $bot) { Invoke-OracleBridgeQuestLog -Guid $guid } else { $null }
        $liveQuestRows = if ($null -eq $liveQuestLog) { @() } else { @($liveQuestLog.quests) }
        $questLogAvailable = $null -ne $liveQuestLog
        $questCount = if ($questLogAvailable) { [uint64](@($liveQuestRows).Count) } elseif ($questCounts.ContainsKey($guid)) { [uint64]$questCounts[$guid] } else { [uint64]0 }
        $completedQuestCount = if ($questLogAvailable) { [uint64](@(@($liveQuestRows) | Where-Object { [int]$_.status -eq 1 }).Count) } else { [uint64]0 }
        if (-not $script:OracleRaceCampaignHistory.ContainsKey($guid)) { $script:OracleRaceCampaignHistory[$guid] = @{} }
        $history = $script:OracleRaceCampaignHistory[$guid]
        $failures = @(Get-OracleFailuresForState -Definition $definition -Character $character -Bot $bot -ProfessionRows $profession -QuestCount $questCount -QuestLogAvailable $questLogAvailable -TalentCount $talentCount -History $history)
        foreach ($failure in $failures) { [void]$allFailures.Add($failure) }

        $level = if ($null -eq $character) { 0 } else { [int]$character.level }
        $xp = if ($null -eq $character) { 0 } else { [uint64]$character.xp }
        $money = if ($null -eq $character) { 0 } else { [uint64]$character.money }
        $map = if ($null -eq $character) { 0 } else { [int]$character.map }
        $zone = if ($null -eq $character) { 0 } else { [int]$character.zone }
        if ($null -ne $bot) {
            $progress = Get-OptionalProperty -InputObject $bot -Name 'progress' -Default $null
            $level = [int](ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $progress -Name 'level' -Default $level))
            $xp = [uint64](ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $progress -Name 'xp' -Default $xp))
            $money = [uint64](ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $progress -Name 'money_copper' -Default $money))
            $position = Get-OptionalProperty -InputObject $bot -Name 'position' -Default $null
            $map = [int](ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $position -Name 'map' -Default $map))
        }

        $professionJson = if (@($profession).Count -eq 0) { '[]' } else { @($profession) | ConvertTo-Json -Compress -Depth 8 }
        $aiJson = if ($null -eq $bot) {
            '{}'
        }
        else {
            $strategies = Get-OptionalProperty -InputObject $bot -Name 'strategies' -Default $null
            [ordered]@{
                state = Get-OptionalProperty -InputObject $bot -Name 'state' -Default ''
                action = Get-OptionalProperty -InputObject $bot -Name 'action' -Default ''
                alive = [bool](Get-OptionalProperty -InputObject $bot -Name 'alive' -Default $true)
                combat = [bool](Get-OptionalProperty -InputObject $bot -Name 'combat' -Default $false)
                position = Get-OptionalProperty -InputObject $bot -Name 'position' -Default $null
                progress = Get-OptionalProperty -InputObject $bot -Name 'progress' -Default $null
                target = Get-OptionalProperty -InputObject $bot -Name 'target' -Default $null
                activity = Get-OptionalProperty -InputObject $bot -Name 'activity' -Default $null
                travel = Get-OptionalProperty -InputObject $bot -Name 'travel' -Default $null
                non_combat_strategies = Get-OptionalProperty -InputObject $strategies -Name 'non_combat' -Default @()
                quest_log = @($liveQuestRows)
                quest_log_source = if ($questLogAvailable) { 'bridge_live' } else { 'database_fallback' }
            } | ConvertTo-Json -Compress -Depth 12
        }
        $snapshot = [pscustomobject][ordered]@{
            run_id = $EffectiveRunId; guid = $guid; race_id = [int]$definition.race_id; race_name = [string]$definition.race_name;
            name = if ($null -eq $character) { [string]$definition.name } else { [string]$character.name };
            level = $level; xp = $xp; money_copper = $money; online = ($null -ne $bot); map_id = $map; zone_id = $zone;
            quest_count = $questCount; completed_quest_count = $completedQuestCount; talent_count = $talentCount; spell_count = $spellCount;
            profession = @($profession); profession_json = $professionJson; ai_json = $aiJson;
            failure_codes = @($failures | ForEach-Object { [string]$_.code }); failures = @($failures);
            captured_at = (Get-Date).ToUniversalTime().ToString('o')
        }
        [void]$snapshots.Add($snapshot)
    }

    $sql = [System.Text.StringBuilder]::new()
    [void]$sql.AppendLine('START TRANSACTION;')
    foreach ($snapshot in $snapshots) {
        $failureCodeText = ($snapshot.failure_codes -join ',')
        $onlineValue = if ([bool]$snapshot.online) { 1 } else { 0 }
        $currentFailureCodes = @($snapshot.failure_codes | Sort-Object -Unique)
        if (@($currentFailureCodes).Count -eq 0) {
            [void]$sql.AppendLine("UPDATE $($playerbotsDb[4]).autowow_oracle_failure SET status='resolved' WHERE character_guid=$([uint32]$snapshot.guid) AND status='open';")
        }
        else {
            $failureCodeList = ($currentFailureCodes | ForEach-Object { ConvertTo-SqlLiteral $_ }) -join ','
            [void]$sql.AppendLine("UPDATE $($playerbotsDb[4]).autowow_oracle_failure SET status='resolved' WHERE character_guid=$([uint32]$snapshot.guid) AND status='open' AND failure_code NOT IN ($failureCodeList);")
        }
        [void]$sql.AppendLine("INSERT INTO $($playerbotsDb[4]).autowow_oracle_progress (run_id,character_guid,race_id,race_name,level,xp,money_copper,online,map_id,zone_id,quest_count,completed_quest_count,talent_count,spell_count,profession_json,ai_json,failure_codes) VALUES ($(ConvertTo-SqlLiteral $snapshot.run_id),$([uint32]$snapshot.guid),$([int]$snapshot.race_id),$(ConvertTo-SqlLiteral $snapshot.race_name),$([int]$snapshot.level),$([uint64]$snapshot.xp),$([uint64]$snapshot.money_copper),$onlineValue,$([int]$snapshot.map_id),$([int]$snapshot.zone_id),$([uint64]$snapshot.quest_count),$([uint64]$snapshot.completed_quest_count),$([uint64]$snapshot.talent_count),$([uint64]$snapshot.spell_count),$(ConvertTo-SqlLiteral $snapshot.profession_json),$(ConvertTo-SqlLiteral $snapshot.ai_json),$(ConvertTo-SqlLiteral $failureCodeText));")
        [void]$sql.AppendLine("UPDATE $($playerbotsDb[4]).autowow_oracle_race_seed SET last_level=$([int]$snapshot.level),last_xp=$([uint64]$snapshot.xp),last_snapshot_at=UTC_TIMESTAMP(),active=1 WHERE character_guid=$([uint32]$snapshot.guid);")
        foreach ($failure in @($snapshot.failures)) {
            $failureJson = $failure.payload | ConvertTo-Json -Compress -Depth 12
            [void]$sql.AppendLine("INSERT INTO $($playerbotsDb[4]).autowow_oracle_failure (character_guid,domain,failure_code,status,occurrences,last_level,latest_payload_json) VALUES ($([uint32]$failure.guid),$(ConvertTo-SqlLiteral $failure.domain),$(ConvertTo-SqlLiteral $failure.code),'open',1,$([int]$failure.level),$(ConvertTo-SqlLiteral $failureJson)) ON DUPLICATE KEY UPDATE status='open',occurrences=occurrences+1,last_seen_at=UTC_TIMESTAMP(),last_level=VALUES(last_level),latest_payload_json=VALUES(latest_payload_json);")
        }
    }
    [void]$sql.AppendLine('COMMIT;')
    Invoke-OracleSql -DatabaseParts $playerbotsDb -Sql $sql.ToString() -Silent
    return [ordered]@{ run_id = $EffectiveRunId; snapshots = @($snapshots); failures = @($allFailures); bridge_bots = $bridgeMap.Count }
}

function Write-OracleRunReceipt {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][object]$Receipt
    )
    $line = $Receipt | ConvertTo-Json -Compress -Depth 16
    Add-Content -LiteralPath $Path -Value $line -Encoding utf8
}

function Get-OracleStatus {
    $definitions = @(Get-OracleRaceSeedDefinitions)
    $guids = @($definitions | ForEach-Object { [uint32]$_.guid })
    $characters = Get-CharacterRows -Guids $guids
    $bridgeError = $null
    $bridgeMap = @{}
    try { $bridgeMap = Get-BridgeBotMap -ListResponse (Invoke-OracleBridgeList) } catch { $bridgeError = $_.Exception.Message }
    $failures = @(Get-CurrentOracleFailureRows)
    $latest = @{}
    $latestSql = "SELECT p.character_guid,p.level,p.xp,p.online,p.quest_count,p.talent_count,p.spell_count,p.captured_at FROM $($playerbotsDb[4]).autowow_oracle_progress p INNER JOIN (SELECT character_guid,MAX(snapshot_id) snapshot_id FROM $($playerbotsDb[4]).autowow_oracle_progress GROUP BY character_guid) latest ON latest.snapshot_id=p.snapshot_id ORDER BY p.character_guid;"
    foreach ($parts in @(ConvertTo-OracleRows -Lines @(Invoke-OracleSql -DatabaseParts $playerbotsDb -Sql $latestSql) -Width 8)) {
        $latest[[uint32](ConvertTo-Int64OrZero $parts[0])] = [ordered]@{ level = [int](ConvertTo-Int64OrZero $parts[1]); xp = [uint64](ConvertTo-Int64OrZero $parts[2]); online = ([int](ConvertTo-Int64OrZero $parts[3]) -ne 0); quest_count = [uint64](ConvertTo-Int64OrZero $parts[4]); talent_count = [uint64](ConvertTo-Int64OrZero $parts[5]); spell_count = [uint64](ConvertTo-Int64OrZero $parts[6]); captured_at = [string]$parts[7] }
    }
    $seeds = foreach ($definition in $definitions) {
        $guid = [uint32]$definition.guid
        $character = if ($characters.ContainsKey($guid)) { $characters[$guid] } else { $null }
        [ordered]@{
            guid = $guid; name = if ($null -eq $character) { $definition.name } else { $character.name }; race = $definition.race_name; faction = $definition.faction; class = $definition.class_name; plan = $definition.class_plan; professions = @($definition.profession_one, $definition.profession_two); level = if ($null -eq $character) { 0 } else { $character.level }; online = $bridgeMap.ContainsKey($guid); live = $bridgeMap.ContainsKey($guid); latest = if ($latest.ContainsKey($guid)) { $latest[$guid] } else { $null }
        }
    }
    return [ordered]@{
        schema = 'autowow.oracle-race-campaign.v1'; generated_utc = (Get-Date).ToUniversalTime().ToString('o'); seeds = @($seeds); online_seed_count = @($seeds | Where-Object online).Count; bridge_error = $bridgeError; open_failures = @($failures | Where-Object status -eq 'open'); failure_count = $failures.Count; runtime_allowlist = @((Get-ConfigOracleGuids -Path $wslPlayerbotsConfig)); controlled_agent_catalog = $oracleControlledAgentValidation;
    }
}

function Start-OracleRaceCampaign {
    $seedResult = Seed-OracleRaceCampaign
    $configResult = Ensure-OracleRuntimeConfig
    $running = Test-WorldserverRunning
    $restartRequired = [bool]$configResult.changed
    if ($restartRequired -and -not $RestartIfNeeded) {
        throw "Oracle runtime config changed; rerun start with -RestartIfNeeded to apply it safely. Backups: $($configResult.backups -join ', ')"
    }
    if (-not $running -or $restartRequired) {
        if (-not $RestartIfNeeded -and -not $running) { throw 'Worldserver is not running; rerun start with -RestartIfNeeded.' }
        Restart-OracleWorldserver
    }
    $seedGuids = @(Get-OracleRaceSeedGuidList)
    $activation = Activate-OracleSeeds -Guids $seedGuids
    $list = Wait-OracleSeedsOnline -Guids $seedGuids
    $arming = Arm-OracleSeeds -Guids $seedGuids
    $list = Invoke-OracleBridgeList
    $receipt = [ordered]@{ action = 'start'; schema = 'autowow.oracle-race-campaign.v1'; seeded = $seedResult.seeded; config = $configResult; activation = $activation; arming = $arming; online = @($list.bots | Where-Object { [uint32]$_.guid -in $seedGuids }).Count }
    if ($AsJson) { $receipt | ConvertTo-Json -Depth 12 } else { $receipt | ConvertTo-Json -Depth 12 }
}

function Run-OracleRaceCampaign {
    $effectiveRunId = if ([string]::IsNullOrWhiteSpace($RunId)) { 'race-' + (Get-Date -Format 'yyyyMMdd-HHmmss') } else { $RunId }
    if ($effectiveRunId -notmatch '^[A-Za-z0-9._-]{1,64}$') { throw 'RunId may contain only letters, numbers, dot, underscore, and dash.' }
    New-Item -ItemType Directory -Path $campaignLogRoot -Force | Out-Null
    $receiptPath = Join-Path $campaignLogRoot ("run-{0}.jsonl" -f $effectiveRunId)
    $deadline = (Get-Date).AddMinutes($DurationMinutes)
    $iteration = 0
    $gatherGuid = [uint32]0
    $gatherEnds = $null
    $nextGatherAt = Get-Date
    do {
        $now = Get-Date
        if ($EnableGatherRotation -and $null -eq $gatherEnds -and $now -ge $nextGatherAt) {
            $seedGuids = @(Get-OracleRaceSeedGuidList)
            $gatherGuid = $seedGuids[$iteration % $seedGuids.Count]
            try {
                Invoke-OracleBridgeAction -BridgeAction deploy -Guid $gatherGuid | Out-Null
                $gatherEnds = $now.AddMinutes($GatherWindowMinutes)
                $nextGatherAt = $gatherEnds.AddMinutes($GatherEveryMinutes)
            }
            catch {
                Add-OracleFailure -Failure (Get-OracleFailureObject -Guid $gatherGuid -Domain 'gather' -Code 'activation_failed' -Level 1 -Payload @{ error = $_.Exception.Message })
                $gatherEnds = $null
                $nextGatherAt = $now.AddMinutes($GatherEveryMinutes)
            }
        }
        if ($null -ne $gatherEnds -and $now -ge $gatherEnds) {
            try {
                Invoke-OracleBridgeAction -BridgeAction deactivate -Guid $gatherGuid | Out-Null
                Start-Sleep -Seconds 1
                Invoke-OracleBridgeAction -BridgeAction activate -Guid $gatherGuid | Out-Null
                Start-Sleep -Milliseconds 500
                Invoke-OracleBridgeAction -BridgeAction independent -Guid $gatherGuid | Out-Null
            }
            catch {
                Add-OracleFailure -Failure (Get-OracleFailureObject -Guid $gatherGuid -Domain 'gather' -Code 'restore_native_ai_failed' -Level 1 -Payload @{ error = $_.Exception.Message })
            }
            $gatherEnds = $null
            $gatherGuid = 0
        }

        $result = Capture-OracleProgress -EffectiveRunId $effectiveRunId
        $iteration++
        $receipt = [ordered]@{ action = 'tick'; iteration = $iteration; run_id = $effectiveRunId; captured_utc = (Get-Date).ToUniversalTime().ToString('o'); online = $result.bridge_bots; seed_online = @($result.snapshots | Where-Object online).Count; levels = @($result.snapshots | ForEach-Object { [ordered]@{ guid = $_.guid; race = $_.race_name; level = $_.level; xp = $_.xp; quest_count = $_.quest_count; talents = $_.talent_count; professions = @($_.profession | ForEach-Object name); failures = @($_.failure_codes) } }); failure_count = @($result.failures).Count; gather_guid = $gatherGuid }
        Write-OracleRunReceipt -Path $receiptPath -Receipt $receipt
        if (-not $AsJson) { Write-Host ("{0} tick {1}: {2}/{3} race seeds online, {4} current failure observations." -f (Get-Date -Format 'HH:mm:ss'), $iteration, @($result.snapshots | Where-Object online).Count, @($result.snapshots).Count, @($result.failures).Count) }
        else { $receipt | ConvertTo-Json -Depth 12 }
        if ($DurationMinutes -eq 0) { break }
        if ((Get-Date) -ge $deadline) { break }
        Start-Sleep -Seconds $PollSeconds
    } while ((Get-Date) -lt $deadline)
    if ($null -ne $gatherEnds -and $ReleaseLiveMutations) {
        try {
            Invoke-OracleBridgeAction -BridgeAction deactivate -Guid $gatherGuid | Out-Null
            Start-Sleep -Seconds 1
            Invoke-OracleBridgeAction -BridgeAction activate -Guid $gatherGuid | Out-Null
            Start-Sleep -Milliseconds 500
            Invoke-OracleBridgeAction -BridgeAction independent -Guid $gatherGuid | Out-Null
        } catch { }
    }
    if (-not $AsJson) { Write-Host "Oracle race campaign run complete. Receipts: $receiptPath" }
}

function Stop-OracleRaceCampaign {
    $guids = @(Get-OracleRaceSeedGuidList)
    if (-not $ReleaseLiveMutations) {
        $result = [ordered]@{ action = 'stop'; mutated = $false; reason = 'default_safe_stop_requires_-ReleaseLiveMutations'; guids = $guids }
        $result | ConvertTo-Json -Depth 8
        return
    }
    foreach ($guid in $guids) {
        try { Invoke-OracleBridgeAction -BridgeAction deactivate -Guid $guid | Out-Null } catch { }
    }
    Invoke-OracleSql -DatabaseParts $playerbotsDb -Sql "UPDATE $($playerbotsDb[4]).autowow_oracle_race_seed SET active=0 WHERE character_guid IN ($($guids -join ',')); UPDATE $($playerbotsDb[4]).autowow_league_member SET active=0 WHERE team_id='race-seeds' AND character_guid IN ($($guids -join ','));" -Silent
    [ordered]@{ action = 'stop'; mutated = $true; guids = $guids } | ConvertTo-Json -Depth 8
}

$script:OracleRaceCampaignHistory = @{}
switch ($Action) {
    'install' {
        $tables = @(Install-OracleSchema)
        [ordered]@{ action = 'install'; tables = $tables } | ConvertTo-Json -Depth 8
    }
    'seed' {
        $result = Seed-OracleRaceCampaign
        [ordered]@{ action = 'seed'; seeded = $result.seeded; guids = $result.guids } | ConvertTo-Json -Depth 8
    }
    'start' { Start-OracleRaceCampaign }
    'run' { Run-OracleRaceCampaign }
    'snapshot' {
        $effectiveRunId = if ([string]::IsNullOrWhiteSpace($RunId)) { 'snapshot-' + (Get-Date -Format 'yyyyMMdd-HHmmss') } else { $RunId }
        $result = Capture-OracleProgress -EffectiveRunId $effectiveRunId
        if ($AsJson) { $result | ConvertTo-Json -Depth 16 } else { $result.snapshots | Select-Object guid,name,race_name,level,xp,online,quest_count,talent_count,spell_count,failure_codes | Format-Table -AutoSize }
    }
    'status' {
        $result = Get-OracleStatus
        $result | ConvertTo-Json -Depth 16
    }
    'report' {
        $result = Get-OracleStatus
        $reportPath = Join-Path $ServerRoot ("ORACLE_RACE_CAMPAIGN_REPORT_{0}.md" -f (Get-Date -Format 'yyyyMMdd'))
        $lines = [System.Collections.Generic.List[string]]::new()
        [void]$lines.Add('# AutoWow Oracle Race Campaign')
        [void]$lines.Add('')
        [void]$lines.Add("Generated: $($result.generated_utc)")
        [void]$lines.Add('')
        [void]$lines.Add('| Race | Character | Level | Online | Planned class | Planned professions |')
        [void]$lines.Add('|---|---|---:|:---:|---|---|')
        foreach ($seed in @($result.seeds)) { [void]$lines.Add("| $($seed.race) | $($seed.name) ($($seed.guid)) | $($seed.level) | $($seed.online) | $($seed.plan) | $($seed.professions -join ', ') |") }
        [void]$lines.Add('')
        [void]$lines.Add("Online seeds: $($result.online_seed_count)/10")
        [void]$lines.Add("Failure rows: $($result.failure_count)")
        [void]$lines.Add('')
        [void]$lines.Add('## Honest capability notes')
        [void]$lines.Add('')
        [void]$lines.Add('- Native New RPG, quest objective, loot, trainer learning, equipment refresh, and talent-picking are measured from the live bot and database state.')
        [void]$lines.Add('- Gathering is exercised only during an explicit bounded worker rotation; it is never inferred from the ordinary quest loop.')
        [void]$lines.Add('- Crafting has a deployed bounded one-cast verb and exact inventory-delta status surface, but remains contract-planned until a real reagent-backed live receipt proves one output and the matching reagent decrements.')
        [void]$lines.Add('')
        [void]$lines.Add('## Open failures')
        [void]$lines.Add('')
        [void]$lines.Add('| GUID | Domain | Code | Status | Occurrences | Last level | Last seen |')
        [void]$lines.Add('|---:|---|---|---|---:|---:|---|')
        foreach ($failure in @($result.open_failures)) { [void]$lines.Add("| $($failure.guid) | $($failure.domain) | $($failure.code) | $($failure.status) | $($failure.occurrences) | $($failure.level) | $($failure.last_seen_at) |") }
        [System.IO.File]::WriteAllLines($reportPath, $lines, [System.Text.UTF8Encoding]::new($false))
        [ordered]@{ action = 'report'; path = $reportPath; status = $result } | ConvertTo-Json -Depth 16
    }
    'stop' { Stop-OracleRaceCampaign }
}
