[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$manifestPath = Join-Path $PSScriptRoot 'fixtures\zone-scouts-20260922.json'
$controlPath = Join-Path $PSScriptRoot 'autowow-control.ps1'
$outputDir = Join-Path $projectRoot 'work\zone-scouts-20260922'
$baselinePath = Join-Path $outputDir 'overnight-progress-baseline.json'
$currentPath = Join-Path $outputDir 'overnight-progress-current.json'
$historyDir = Join-Path $outputDir 'overnight-progress-history'
$configPath = Join-Path $projectRoot 'server\configs\worldserver.conf'
$mysqlPath = Join-Path $projectRoot 'third_party\mysql\bin\mysql.exe'
$utf8 = [Text.UTF8Encoding]::new($false)
$expectedGuids = @(101, 112, 121, 123, 236, 244)

function Read-Bridge([string]$action, [uint32]$guid = 0) {
    $arguments = @{ Action = $action; TimeoutMs = 5000 }
    if ($guid) { $arguments.BotGuid = $guid }
    $raw = (& $controlPath @arguments | Out-String).Trim()
    if ([string]::IsNullOrWhiteSpace($raw)) { throw "Bridge $action returned no data." }
    $value = $raw | ConvertFrom-Json -AsHashtable
    if (-not $value.ok) { throw "Bridge $action returned a non-OK result." }
    return $value
}

function Read-RewardedQuestIds {
    $config = Get-Content -LiteralPath $configPath -Raw
    $match = [regex]::Match($config, '(?im)^\s*CharacterDatabaseInfo\s*=\s*"(?<value>[^"\r\n]+)"')
    if (-not $match.Success) { throw 'CharacterDatabaseInfo is unavailable.' }
    $parts = $match.Groups['value'].Value.Split(';')
    if ($parts.Count -ne 5 -or $parts[1] -notmatch '^\d{1,5}$' -or
        [int]$parts[1] -lt 1 -or [int]$parts[1] -gt 65535 -or
        $parts[4] -notmatch '^[A-Za-z0-9_]+$') {
        throw 'CharacterDatabaseInfo has an invalid connection shape.'
    }

    $sql = 'SELECT guid,quest FROM character_queststatus_rewarded WHERE guid IN (101,112,121,123,236,244) ORDER BY guid,quest'
    $start = [Diagnostics.ProcessStartInfo]::new($mysqlPath)
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    foreach ($argument in @('--protocol=tcp', '--connect-timeout=5', "--host=$($parts[0])",
        "--port=$($parts[1])", "--user=$($parts[2])", "--database=$($parts[4])",
        '--default-character-set=utf8mb4', '--batch', '--raw', '--skip-column-names',
        "--execute=$sql")) { [void]$start.ArgumentList.Add($argument) }
    $start.Environment['MYSQL_PWD'] = $parts[3]
    $process = [Diagnostics.Process]::Start($start)
    try {
        $output = $process.StandardOutput.ReadToEnd()
        $null = $process.StandardError.ReadToEnd() # Never echo MySQL diagnostics or config values.
        $process.WaitForExit()
        if ($process.ExitCode -ne 0) { throw "Read-only reward query failed (exit $($process.ExitCode))." }
    } finally { $process.Dispose() }

    $rewarded = @{}
    foreach ($guid in $expectedGuids) { $rewarded[[string]$guid] = [Collections.Generic.List[int]]::new() }
    foreach ($line in ($output -split "`r?`n")) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        $columns = $line.Split([char]9)
        if ($columns.Count -ne 2 -or $columns[0] -notmatch '^\d+$' -or $columns[1] -notmatch '^\d+$' -or
            $columns[0] -notin @($expectedGuids | ForEach-Object { [string]$_ })) {
            throw 'Reward query returned an unexpected row.'
        }
        $rewarded[$columns[0]].Add([int]$columns[1])
    }
    return $rewarded
}

function Write-NewJson([string]$path, $value) {
    $json = ConvertTo-Json -InputObject $value -Depth 15
    $bytes = $utf8.GetBytes($json + "`n")
    $stream = [IO.FileStream]::new($path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try { $stream.Write($bytes, 0, $bytes.Length) } finally { $stream.Dispose() }
}

if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf) -or
    -not (Test-Path -LiteralPath $controlPath -PathType Leaf) -or
    -not (Test-Path -LiteralPath $mysqlPath -PathType Leaf)) { throw 'Required local input is missing.' }
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json -AsHashtable
$scouts = @($manifest.scouts)
$manifestGuids = @($scouts | ForEach-Object { [int]$_.guid } | Sort-Object)
if ($manifest.schema -ne 'autowow.zone-scout.manifest.v1' -or $scouts.Count -ne 6 -or
    (($manifestGuids -join ',') -ne (($expectedGuids | Sort-Object) -join ','))) {
    throw 'Manifest does not match the fixed six-scout roster.'
}

$list = Read-Bridge 'list'
$online = @{}
foreach ($bot in @($list.bots)) { $online[[string]$bot.guid] = $bot }
foreach ($guid in $expectedGuids) {
    if (-not $online.ContainsKey([string]$guid)) { throw "Scout $guid is absent from the live bridge list." }
}
$rewarded = Read-RewardedQuestIds
$now = [datetime]::UtcNow
$snapshot = [ordered]@{
    schema = 'autowow.zone-scout.progress.v1'
    captured_utc = $now.ToString('o')
    manifest_path = $manifestPath
    sources = [ordered]@{
        live = 'AutoWow bridge list + questlog; live level, XP, copper, zone, active quest counters'
        durable = 'Read-only character_queststatus_rewarded rows; saved DB may lag live state'
    }
    scouts = @()
}
foreach ($scout in $scouts) {
    $guid = [int]$scout.guid
    $bot = $online[[string]$guid]
    $questlog = Read-Bridge 'questlog' $guid
    if ([int]$questlog.guid -ne $guid) { throw "Questlog GUID mismatch for scout $guid." }
    $quests = @()
    foreach ($quest in @($questlog.quests)) {
        if ($null -eq $quest) { continue }
        $objectives = @()
        foreach ($objective in @($quest.objectives)) {
            if ($null -eq $objective) { continue }
            $objectives += [ordered]@{
                kind = $objective.kind; entry = $objective.entry
                current = $objective.current; required = $objective.required; done = $objective.done
            }
        }
        $quests += [ordered]@{
            id = [int]$quest.id; title = $quest.title; status = $quest.status
            is_complete = $quest.is_complete; objectives_all_done = $quest.objectives_all_done
            objectives = $objectives
        }
    }
    $ids = @($rewarded[[string]$guid] | Sort-Object -Unique)
    $snapshot.scouts += [ordered]@{
        guid = $guid; name = [string]$bot.name; home_zone_label = [string]$scout.home_zone
        live = [ordered]@{
            level = $bot.progress.level; xp = $bot.progress.xp; money_copper = $bot.progress.money_copper
            map = $bot.position.map; zone = $bot.position.zone; area = $bot.position.area
            active_quest_count = $quests.Count; active_quests = $quests
        }
        saved_db = [ordered]@{ rewarded_quest_count = $ids.Count; rewarded_quest_ids = $ids }
    }
}

New-Item -ItemType Directory -Path $outputDir, $historyDir -Force | Out-Null
if (-not (Test-Path -LiteralPath $baselinePath -PathType Leaf)) {
    try { Write-NewJson $baselinePath $snapshot }
    catch [IO.IOException] {
        if (-not (Test-Path -LiteralPath $baselinePath -PathType Leaf)) { throw }
    }
}
$baseline = Get-Content -LiteralPath $baselinePath -Raw | ConvertFrom-Json -AsHashtable -DateKind String
if ($baseline.schema -ne 'autowow.zone-scout.progress.v1' -or @($baseline.scouts).Count -ne 6 -or
    (($baseline.scouts | ForEach-Object { [int]$_.guid } | Sort-Object) -join ',') -ne (($expectedGuids | Sort-Object) -join ',')) {
    throw 'Existing baseline has an incompatible schema or roster; it was not replaced.'
}
$baselineByGuid = @{}
foreach ($scout in $baseline.scouts) { $baselineByGuid[[string]$scout.guid] = $scout }
$baselineUtc = [DateTimeOffset]::Parse([string]$baseline.captured_utc,
    [Globalization.CultureInfo]::InvariantCulture).UtcDateTime
$elapsed = [math]::Max(0, [math]::Round(($now - $baselineUtc).TotalSeconds, 1))
$newRewardedTotal = 0
foreach ($scout in $snapshot.scouts) {
    $previous = $baselineByGuid[[string]$scout.guid]
    $baselineIds = [Collections.Generic.HashSet[int]]::new()
    foreach ($id in @($previous.saved_db.rewarded_quest_ids)) { [void]$baselineIds.Add([int]$id) }
    $newIds = @($scout.saved_db.rewarded_quest_ids | Where-Object { -not $baselineIds.Contains([int]$_) })
    $newRewardedTotal += $newIds.Count
    $scout.delta_from_baseline = [ordered]@{
        level = [int]$scout.live.level - [int]$previous.live.level
        money_copper = [int]$scout.live.money_copper - [int]$previous.live.money_copper
        newly_rewarded_quest_ids = $newIds
        newly_rewarded_count = $newIds.Count
    }
}
$snapshot.baseline_utc = $baseline.captured_utc
$snapshot.elapsed_seconds = $elapsed
$snapshot.summary = [ordered]@{ scout_count = 6; newly_rewarded_quest_count = $newRewardedTotal }
$snapshot.limits = @(
    'Rewarded quest IDs are from saved character DB rows, which may lag live bridge state.',
    'XP is within the current level; level and XP must be read together.',
    'Home zone is a manifest label; observed zone ID is from the live bridge.'
)
$historyPath = Join-Path $historyDir ("progress-{0}-{1}.json" -f $now.ToString('yyyyMMddTHHmmssfffZ'), [guid]::NewGuid().ToString('N').Substring(0, 8))
Write-NewJson $historyPath $snapshot
$tempPath = "$currentPath.$([guid]::NewGuid().ToString('N')).tmp"
try {
    [IO.File]::WriteAllText($tempPath, (ConvertTo-Json -InputObject $snapshot -Depth 15) + "`n", $utf8)
    Move-Item -LiteralPath $tempPath -Destination $currentPath -Force
} finally { if (Test-Path -LiteralPath $tempPath) { Remove-Item -LiteralPath $tempPath -Force } }

[ordered]@{
    ok = $true; captured_utc = $snapshot.captured_utc; baseline_utc = $snapshot.baseline_utc
    elapsed_seconds = $elapsed; scout_count = 6; newly_rewarded_quest_count = $newRewardedTotal
    baseline_path = $baselinePath; current_path = $currentPath; history_path = $historyPath
    data_source_limit = 'Saved rewarded quest rows may lag live bridge progress.'
} | ConvertTo-Json -Compress -Depth 4
