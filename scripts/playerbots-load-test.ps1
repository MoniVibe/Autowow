[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [ValidateRange(1, 80)][int]$BotCount = 40,
    [ValidateSet('World','WSG','AV')][string]$Scenario = 'WSG',
    [ValidateRange(2, 20)][int]$WarmupMinutes = 10,
    [ValidateRange(1, 120)][int]$ObservationMinutes = 20,
    [ValidateRange(2048, 16384)][int]$MinAvailableMemoryMiB = 4096,
    [ValidateRange(6, 48)][int]$MaxWorldPrivateGiB = 12,
    [switch]$KeepTier
)

. (Join-Path $PSScriptRoot 'Common.ps1')

$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
Initialize-AutoWoWLayout -Root $ServerRoot
$playerbotsConfig = Join-Path $ServerRoot 'server\configs\modules\playerbots.conf'
$worldPidPath = Join-Path $ServerRoot 'worldserver.pid'
$control = Join-Path $PSScriptRoot 'autowow-control.ps1'
foreach ($required in @($playerbotsConfig, $control)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Load-test prerequisite is missing: $required" }
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$receiptPath = Join-Path $ServerRoot "logs\playerbots-load-test-$stamp.jsonl"
$backupPath = Join-Path $ServerRoot "logs\playerbots-load-test-$stamp-before.conf"
$reportPath = Join-Path $ServerRoot 'PLAYERBOTS_LOAD_TEST_REPORT.md'
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
Copy-Item -LiteralPath $playerbotsConfig -Destination $backupPath -Force

function Write-LoadReceipt {
    param([string]$Event, [hashtable]$Fields = @{})
    $entry = [ordered]@{ timestamp = (Get-Date).ToUniversalTime().ToString('o'); event = $Event }
    foreach ($key in $Fields.Keys) { $entry[$key] = $Fields[$key] }
    $line = ([pscustomobject]$entry | ConvertTo-Json -Compress -Depth 8)
    [System.IO.File]::AppendAllText($receiptPath, $line + [Environment]::NewLine, $utf8NoBom)
    Write-Output $line
}

function Get-WorldProcess {
    if (-not (Test-Path -LiteralPath $worldPidPath)) { return $null }
    try {
        $processId = [int](Get-Content -LiteralPath $worldPidPath -Raw).Trim()
        return Get-Process -Id $processId -ErrorAction SilentlyContinue
    }
    catch { return $null }
}

function Get-BridgeBots {
    $raw = & $control -Action list
    $json = (@($raw | ForEach-Object { $_.ToString() }) | Select-Object -Last 1) | ConvertFrom-Json
    if (-not $json.ok) { throw "Bridge list failed: $($json.error)" }
    return @($json.bots)
}

function Get-LoadSample {
    $world = Get-WorldProcess
    if (-not $world) { throw 'worldserver process is not running.' }
    $available = (Get-Counter '\Memory\Available MBytes').CounterSamples[0].CookedValue
    $bots = Get-BridgeBots
    $maps = @($bots | Group-Object { $_.position.map } | ForEach-Object { "$($_.Name):$($_.Count)" }) -join ','
    return [ordered]@{
        online_bots = @($bots).Count
        alive_bots = @($bots | Where-Object alive).Count
        combat_bots = @($bots | Where-Object combat).Count
        maps = $maps
        available_memory_mib = [math]::Round($available, 0)
        world_private_gib = [math]::Round($world.PrivateMemorySize64 / 1GB, 2)
        world_working_gib = [math]::Round($world.WorkingSet64 / 1GB, 2)
        world_cpu_seconds = [math]::Round($world.CPU, 2)
    }
}

function Apply-LoadConfiguration {
    param([int]$Count, [string]$Mode)

    $autoJoinBg = if ($Mode -eq 'World') { '0' } else { '1' }
    $wsgCount = if ($Mode -eq 'WSG') { [math]::Max(1, [math]::Floor($Count / 20)).ToString() } else { '0' }
    $avCount = if ($Mode -eq 'AV') { [math]::Max(1, [math]::Floor($Count / 80)).ToString() } else { '0' }

    Set-ConfigValue -Path $playerbotsConfig -Key 'AiPlayerbot.RandomBotAutologin' -Value '1'
    Set-ConfigValue -Path $playerbotsConfig -Key 'AiPlayerbot.MinRandomBots' -Value "$Count"
    Set-ConfigValue -Path $playerbotsConfig -Key 'AiPlayerbot.MaxRandomBots' -Value "$Count"
    Set-ConfigValue -Path $playerbotsConfig -Key 'AiPlayerbot.RandomBotAccountCount' -Value '0'
    Set-ConfigValue -Path $playerbotsConfig -Key 'AiPlayerbot.RandomBotMinLevel' -Value '80'
    Set-ConfigValue -Path $playerbotsConfig -Key 'AiPlayerbot.RandomBotMaxLevel' -Value '80'
    Set-ConfigValue -Path $playerbotsConfig -Key 'AiPlayerbot.RandomBotMinLevelChance' -Value '1.0'
    Set-ConfigValue -Path $playerbotsConfig -Key 'AiPlayerbot.RandomBotMaxLevelChance' -Value '1.0'
    Set-ConfigValue -Path $playerbotsConfig -Key 'AiPlayerbot.RandomBotFixedLevel' -Value '1'
    Set-ConfigValue -Path $playerbotsConfig -Key 'AiPlayerbot.RandomBotsPerInterval' -Value '80'
    Set-ConfigValue -Path $playerbotsConfig -Key 'AiPlayerbot.MinRandomBotInWorldTime' -Value '43200'
    Set-ConfigValue -Path $playerbotsConfig -Key 'AiPlayerbot.MaxRandomBotInWorldTime' -Value '43200'
    Set-ConfigValue -Path $playerbotsConfig -Key 'AiPlayerbot.RandomBotJoinBG' -Value '1'
    Set-ConfigValue -Path $playerbotsConfig -Key 'AiPlayerbot.RandomBotAutoJoinBG' -Value $autoJoinBg
    Set-ConfigValue -Path $playerbotsConfig -Key 'AiPlayerbot.RandomBotAutoJoinBGWSCount' -Value $wsgCount
    Set-ConfigValue -Path $playerbotsConfig -Key 'AiPlayerbot.RandomBotAutoJoinBGABCount' -Value '0'
    Set-ConfigValue -Path $playerbotsConfig -Key 'AiPlayerbot.RandomBotAutoJoinBGAVCount' -Value $avCount
    Set-ConfigValue -Path $playerbotsConfig -Key 'AiPlayerbot.RandomBotAutoJoinBGEYCount' -Value '0'
    Set-ConfigValue -Path $playerbotsConfig -Key 'AiPlayerbot.RandomBotAutoJoinBGICCount' -Value '0'
}

$agentWasRunning = Test-Path -LiteralPath (Join-Path $ServerRoot 'autowow-agent.pid.json')
$passed = $false
$failure = $null
$latestSample = $null

Write-LoadReceipt -Event 'load_test_started' -Fields @{
    bot_count = $BotCount
    scenario = $Scenario
    warmup_minutes = $WarmupMinutes
    observation_minutes = $ObservationMinutes
    config_backup = $backupPath
    keep_tier = [bool]$KeepTier
}

try {
    $availableAtStart = (Get-Counter '\Memory\Available MBytes').CounterSamples[0].CookedValue
    if ($availableAtStart -lt $MinAvailableMemoryMiB) {
        throw "Refusing the load test: only $([math]::Round($availableAtStart)) MiB is currently available; minimum is $MinAvailableMemoryMiB MiB."
    }

    if ($agentWasRunning) { & (Join-Path $PSScriptRoot 'stop-autowow-agent.ps1') -ServerRoot $ServerRoot }
    & (Join-Path $PSScriptRoot 'stop-server.ps1') -ServerRoot $ServerRoot
    Apply-LoadConfiguration -Count $BotCount -Mode $Scenario
    Write-LoadReceipt -Event 'configuration_applied' -Fields @{ bot_count = $BotCount; scenario = $Scenario }
    & (Join-Path $PSScriptRoot 'start-server.ps1') -ServerRoot $ServerRoot

    $readyDeadline = (Get-Date).AddMinutes($WarmupMinutes)
    $consecutiveBridgeErrors = 0
    while ((Get-Date) -lt $readyDeadline) {
        try {
            $latestSample = Get-LoadSample
            Write-LoadReceipt -Event 'warmup_sample' -Fields $latestSample
            if ($latestSample.available_memory_mib -lt $MinAvailableMemoryMiB) { throw "Available memory dropped below $MinAvailableMemoryMiB MiB." }
            if ($latestSample.world_private_gib -gt $MaxWorldPrivateGiB) { throw "worldserver private memory exceeded $MaxWorldPrivateGiB GiB." }
            if ($latestSample.online_bots -ge $BotCount) { break }
            $consecutiveBridgeErrors = 0
        }
        catch {
            $consecutiveBridgeErrors++
            Write-LoadReceipt -Event 'warmup_error' -Fields @{ consecutive_errors = $consecutiveBridgeErrors; error = $_.Exception.Message }
            # A Playerbots startup can legitimately take more than a minute to
            # construct accounts, characters, caches, and the bridge. Keep
            # retrying while the worldserver process itself remains healthy.
            if (-not (Get-WorldProcess)) { throw 'worldserver exited during warmup.' }
            if ($consecutiveBridgeErrors -ge 20) { throw "Bridge remained unavailable for $consecutiveBridgeErrors warmup probes." }
        }
        Start-Sleep -Seconds 15
    }
    if (-not $latestSample -or $latestSample.online_bots -lt $BotCount) {
        throw "Timed out waiting for $BotCount online random bots. Last observed count: $($latestSample.online_bots)."
    }
    Write-LoadReceipt -Event 'target_population_reached' -Fields $latestSample

    $observeDeadline = (Get-Date).AddMinutes($ObservationMinutes)
    while ((Get-Date) -lt $observeDeadline) {
        $latestSample = Get-LoadSample
        Write-LoadReceipt -Event 'observation_sample' -Fields $latestSample
        if ($latestSample.available_memory_mib -lt $MinAvailableMemoryMiB) { throw "Available memory dropped below $MinAvailableMemoryMiB MiB." }
        if ($latestSample.world_private_gib -gt $MaxWorldPrivateGiB) { throw "worldserver private memory exceeded $MaxWorldPrivateGiB GiB." }
        if ($latestSample.online_bots -lt [math]::Max(1, $BotCount - 2)) { throw "Online bot population fell below the tolerated floor ($($BotCount - 2))." }
        Start-Sleep -Seconds 20
    }
    $passed = $true
    Write-LoadReceipt -Event 'load_test_passed' -Fields $latestSample
}
catch {
    $failure = $_.Exception.Message
    Write-LoadReceipt -Event 'load_test_failed' -Fields @{ error = $failure }
}
finally {
    if (-not $passed -or -not $KeepTier) {
        try {
            if (Get-WorldProcess) { & (Join-Path $PSScriptRoot 'stop-server.ps1') -ServerRoot $ServerRoot }
            Copy-Item -LiteralPath $backupPath -Destination $playerbotsConfig -Force
            & (Join-Path $PSScriptRoot 'start-server.ps1') -ServerRoot $ServerRoot
            Write-LoadReceipt -Event 'configuration_restored' -Fields @{ restored_from = $backupPath }
        }
        catch {
            Write-LoadReceipt -Event 'restore_failed' -Fields @{ error = $_.Exception.Message; restored_from = $backupPath }
            if (-not $failure) { $failure = "Restore failed: $($_.Exception.Message)" }
        }
    }

    if ($agentWasRunning) {
        try {
            & (Join-Path $PSScriptRoot 'start-autowow-agent.ps1') -ServerRoot $ServerRoot -DurationMinutes 480 -PollSeconds 20 -TravelEveryMinutes 20
            Write-LoadReceipt -Event 'overnight_agent_restarted' -Fields @{}
        }
        catch {
            Write-LoadReceipt -Event 'overnight_agent_restart_failed' -Fields @{ error = $_.Exception.Message }
            if (-not $failure) { $failure = "Could not restart the overnight agent: $($_.Exception.Message)" }
        }
    }

    $result = if ($passed) { 'PASS' } else { 'FAIL' }
    $lastOnlineBots = if ($latestSample) { $latestSample['online_bots'] } else { 'n/a' }
    $lastAvailableMemory = if ($latestSample) { $latestSample['available_memory_mib'] } else { 'n/a' }
    $lastWorldPrivate = if ($latestSample) { $latestSample['world_private_gib'] } else { 'n/a' }
    $lastMaps = if ($latestSample) { $latestSample['maps'] } else { 'n/a' }
    $report = @"
# Playerbots load-test report

Generated: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')

| Field | Value |
|---|---|
| Result | $result |
| Requested concurrent bots | $BotCount |
| Scenario | $Scenario |
| Keep tier after test | $([bool]$KeepTier) |
| Config backup | $backupPath |
| Receipts | $receiptPath |
| Last online bots | $lastOnlineBots |
| Last available memory (MiB) | $lastAvailableMemory |
| Last world private memory (GiB) | $lastWorldPrivate |
| Last map distribution | $lastMaps |

Failure detail: $(if ($failure) { $failure } else { 'none' })

The WSG scenario asks Playerbots to target one 20-player match per configured instance. The AV scenario targets an 80-player match. A PASS proves concurrent bot population and server health; it does not by itself prove a completed battleground or player-led raid encounter.
"@
    [System.IO.File]::WriteAllText($reportPath, $report, $utf8NoBom)
}

if (-not $passed) { throw "Playerbots load test failed. See $reportPath and $receiptPath. $failure" }
Write-Output "Playerbots load test passed. Report: $reportPath"
