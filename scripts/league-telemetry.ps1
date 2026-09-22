[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$LeaguePath = '',
    [ValidateRange(1, 1440)][int]$DurationMinutes = 480,
    [ValidateRange(5, 60)][int]$PollSeconds = 10,
    [string]$ReceiptPath = ''
)

. (Join-Path $PSScriptRoot 'Common.ps1')

$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
Initialize-AutoWoWLayout -Root $ServerRoot
$leagueRoot = Join-Path $ServerRoot 'leagues'
if ([string]::IsNullOrWhiteSpace($LeaguePath)) { $LeaguePath = Join-Path $leagueRoot 'autowow-league.json' }
& (Join-Path $PSScriptRoot 'league.ps1') -Action validate -ServerRoot $ServerRoot -LeaguePath $LeaguePath | Out-Null
$league = Get-Content -LiteralPath $LeaguePath -Raw | ConvertFrom-Json
$control = Join-Path $PSScriptRoot 'autowow-control.ps1'
if (-not (Test-Path -LiteralPath $control)) { throw "Bridge client is missing: $control" }
if ([string]::IsNullOrWhiteSpace($ReceiptPath)) { $ReceiptPath = Join-Path $leagueRoot ('results\telemetry-{0}.jsonl' -f (Get-Date -Format 'yyyyMMdd-HHmmss')) }
New-Item -ItemType Directory -Path (Split-Path -Parent $ReceiptPath) -Force | Out-Null
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)

function Write-Telemetry {
    param([string]$Event, [hashtable]$Fields = @{})
    $record = [ordered]@{ timestamp_utc = (Get-Date).ToUniversalTime().ToString('o'); event = $Event }
    foreach ($key in $Fields.Keys) { $record[$key] = $Fields[$key] }
    $line = ([pscustomobject]$record | ConvertTo-Json -Depth 7 -Compress)
    [System.IO.File]::AppendAllText($ReceiptPath, $line + [Environment]::NewLine, $utf8NoBom)
    Write-Output $line
}

function Get-LiveBots {
    $raw = & $control -Action list
    $result = (@($raw | ForEach-Object { $_.ToString() }) | Select-Object -Last 1) | ConvertFrom-Json
    if (-not $result.ok) { throw "Bridge list failed: $($result.error)" }
    return @($result.bots)
}

function Get-WorldMetrics {
    $pidPath = Join-Path $ServerRoot 'worldserver.pid'
    if (-not (Test-Path -LiteralPath $pidPath)) { throw 'worldserver PID record is missing.' }
    $world = Get-Process -Id ([int](Get-Content -LiteralPath $pidPath -Raw).Trim()) -ErrorAction Stop
    $available = (Get-Counter '\Memory\Available MBytes').CounterSamples[0].CookedValue
    return [ordered]@{
        world_private_gib = [math]::Round($world.PrivateMemorySize64 / 1GB, 2)
        world_working_gib = [math]::Round($world.WorkingSet64 / 1GB, 2)
        world_cpu_seconds = [math]::Round($world.CPU, 2)
        available_memory_mib = [math]::Round($available, 0)
    }
}

function Get-DatabaseParts {
    param([string]$ConfigPath, [string]$Key)
    $pattern = "^\s*{0}\s*=\s*" -f [regex]::Escape($Key)
    $line = (Select-String -LiteralPath $ConfigPath -Pattern $pattern | Select-Object -First 1).Line
    if (-not $line) { throw "$Key was not found in $ConfigPath" }
    $parts = (($line -replace $pattern, '').Trim().Trim('"')) -split ';', 5
    if ($parts.Count -ne 5) { throw "$Key has an unexpected connection-string format." }
    return $parts
}

function Get-TeamByBotGuid {
    $worldConfig = Join-Path $ServerRoot 'server\configs\worldserver.conf'
    $auth = Get-DatabaseParts -ConfigPath $worldConfig -Key 'LoginDatabaseInfo'
    $characters = Get-DatabaseParts -ConfigPath $worldConfig -Key 'CharacterDatabaseInfo'
    $mysql = Get-FirstExistingPath -Candidates @((Join-Path $ServerRoot 'third_party\mysql\bin\mysql.exe'),'C:\Program Files\MySQL\MySQL Server 8.4\bin\mysql.exe','C:\Program Files\MySQL\MySQL Server 8.0\bin\mysql.exe')
    if (-not $mysql) { throw 'MySQL CLI was not found for team mapping.' }
    $accountToTeam = @{}
    foreach ($team in @($league.teams)) {
        $accountToTeam[[string]$team.captainAccount] = [string]$team.id
        foreach ($account in @($team.rosterAccounts)) { $accountToTeam[[string]$account] = [string]$team.id }
    }
    $quoted = @($accountToTeam.Keys | Sort-Object | ForEach-Object { "'$($_.Replace("'", "''"))'" }) -join ','
    $sql = "SELECT c.guid, a.username FROM $($auth[4]).account a INNER JOIN $($characters[4]).characters c ON c.account = a.id WHERE a.username IN ($quoted);"
    $oldPwd = $env:MYSQL_PWD
    $env:MYSQL_PWD = $auth[3]
    try {
        $raw = & $mysql --protocol=tcp "--host=$($auth[0])" "--port=$($auth[1])" "--user=$($auth[2])" --batch --skip-column-names --execute=$sql 2>&1
        $exitCode = $LASTEXITCODE
    }
    finally {
        if ($null -eq $oldPwd) { Remove-Item Env:MYSQL_PWD -ErrorAction SilentlyContinue } else { $env:MYSQL_PWD = $oldPwd }
    }
    if ($exitCode -ne 0) { throw 'Team mapping query failed. Database connection details were not emitted.' }
    $mapping = @{}
    foreach ($line in @($raw | ForEach-Object { $_.ToString() } | Where-Object { $_ -notmatch '^mysql:' -and $_ -ne '' })) {
        $columns = $line -split "`t"
        if ($columns.Count -eq 2 -and $accountToTeam.ContainsKey($columns[1])) { $mapping[[string]$columns[0]] = $accountToTeam[$columns[1]] }
    }
    return $mapping
}

$previous = @{}
$deathCount = 0
$reviveCount = 0
$mapChangeCount = 0
$consecutiveErrors = 0
$deadline = (Get-Date).AddMinutes($DurationMinutes)
$teamByBotGuid = Get-TeamByBotGuid
Write-Telemetry -Event 'telemetry_started' -Fields @{ league = $league.leagueName; duration_minutes = $DurationMinutes; poll_seconds = $PollSeconds }
Write-Telemetry -Event 'team_mapping_loaded' -Fields @{ roster_characters_mapped = $teamByBotGuid.Count }

try {
    while ((Get-Date) -lt $deadline) {
        try {
            $bots = Get-LiveBots
            $maps = [ordered]@{}
            foreach ($group in ($bots | Group-Object { $_.position.map } | Sort-Object Name)) { $maps[[string]$group.Name] = $group.Count }
            foreach ($bot in $bots) {
                $guid = [string]$bot.guid
                $botTeam = if ($teamByBotGuid.ContainsKey($guid)) { $teamByBotGuid[$guid] } else { 'unassigned' }
                $current = [ordered]@{ name = $bot.name; alive = [bool]$bot.alive; map = [int]$bot.position.map; combat = [bool]$bot.combat }
                if ($previous.ContainsKey($guid)) {
                    $old = $previous[$guid]
                    if ($old.alive -and -not $current.alive) {
                        $deathCount++
                        Write-Telemetry -Event 'bot_died' -Fields @{ bot_guid = [uint32]$bot.guid; name = $bot.name; team = $botTeam; map = $current.map; observed_deaths = $deathCount }
                    }
                    elseif (-not $old.alive -and $current.alive) {
                        $reviveCount++
                        Write-Telemetry -Event 'bot_revived' -Fields @{ bot_guid = [uint32]$bot.guid; name = $bot.name; team = $botTeam; map = $current.map; observed_revives = $reviveCount }
                    }
                    if ($old.map -ne $current.map) {
                        $mapChangeCount++
                        Write-Telemetry -Event 'bot_map_changed' -Fields @{ bot_guid = [uint32]$bot.guid; name = $bot.name; team = $botTeam; from_map = $old.map; to_map = $current.map; observed_map_changes = $mapChangeCount }
                    }
                }
                $previous[$guid] = $current
            }
            $metrics = Get-WorldMetrics
            $teamPopulation = [ordered]@{}
            foreach ($team in @($league.teams)) {
                $teamBots = @($bots | Where-Object { $teamByBotGuid.ContainsKey([string]$_.guid) -and $teamByBotGuid[[string]$_.guid] -eq $team.id })
                $teamPopulation[[string]$team.id] = [ordered]@{ online = $teamBots.Count; alive = @($teamBots | Where-Object alive).Count; dead = @($teamBots | Where-Object { -not $_.alive }).Count; combat = @($teamBots | Where-Object combat).Count }
            }
            $sample = [ordered]@{
                online_bots = $bots.Count
                alive_bots = @($bots | Where-Object alive).Count
                dead_bots = @($bots | Where-Object { -not $_.alive }).Count
                combat_bots = @($bots | Where-Object combat).Count
                map_population = $maps
                observed_deaths = $deathCount
                observed_revives = $reviveCount
                observed_map_changes = $mapChangeCount
                team_population = $teamPopulation
            }
            foreach ($key in $metrics.Keys) { $sample[$key] = $metrics[$key] }
            Write-Telemetry -Event 'sample' -Fields $sample
            $consecutiveErrors = 0
        }
        catch {
            $consecutiveErrors++
            Write-Telemetry -Event 'collector_error' -Fields @{ consecutive_errors = $consecutiveErrors; error = $_.Exception.Message }
            if ($consecutiveErrors -ge 6) { throw "Telemetry collector stopped after $consecutiveErrors consecutive errors." }
        }
        Start-Sleep -Seconds $PollSeconds
    }
    Write-Telemetry -Event 'telemetry_completed' -Fields @{ observed_deaths = $deathCount; observed_revives = $reviveCount; observed_map_changes = $mapChangeCount }
}
finally {
    Write-Telemetry -Event 'telemetry_stopped' -Fields @{ observed_deaths = $deathCount; observed_revives = $reviveCount; observed_map_changes = $mapChangeCount }
}
