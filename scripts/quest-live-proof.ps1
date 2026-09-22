<#
    Live, engine-authoritative proof harness for one explicit Playerbots quest.

    Dry-run is the default. -Execute sends exactly one `quest <guid> <id>`
    directive, then uses only read-only bridge commands. The harness never
    writes a character, quest, XP, or item table.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][uint32]$BotGuid,
    [Parameter(Mandatory = $true)][uint32]$QuestId,
    [ValidateRange(5, 3600)][int]$DurationSeconds = 600,
    [ValidateRange(200, 10000)][int]$PollMilliseconds = 1000,
    [ValidateSet('127.0.0.1')][string]$BridgeHost = '127.0.0.1',
    [ValidateRange(1, 65535)][int]$BridgePort = 18787,
    [string]$OutputPath = '',
    [switch]$Execute,
    [switch]$StopAfterObjective
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $OutputPath = Join-Path (Split-Path -Parent $PSScriptRoot) "logs\quest-live-proof-q${QuestId}-g${BotGuid}-${stamp}.jsonl"
}
$OutputPath = [System.IO.Path]::GetFullPath($OutputPath)

function Invoke-AutoWowBridge {
    param([Parameter(Mandatory = $true)][string]$Request)

    $client = [System.Net.Sockets.TcpClient]::new()
    try {
        $connect = $client.ConnectAsync($BridgeHost, $BridgePort)
        if (-not $connect.Wait(5000) -or -not $client.Connected) {
            throw "Could not connect to AutoWow bridge at ${BridgeHost}:$BridgePort."
        }
        $stream = $client.GetStream()
        $stream.ReadTimeout = 5000
        $writer = [System.IO.StreamWriter]::new($stream)
        $writer.NewLine = "`n"
        $writer.WriteLine($Request)
        $writer.Flush()
        $reader = [System.IO.StreamReader]::new($stream)
        $line = $reader.ReadLine()
        if ([string]::IsNullOrWhiteSpace($line)) {
            throw "Bridge returned an empty response for '$Request'."
        }
        return ($line | ConvertFrom-Json -Depth 100)
    }
    finally {
        $client.Dispose()
    }
}

function Get-Int64Property {
    param([AllowNull()][object]$Object, [string]$Name)
    if ($null -eq $Object) { return [int64]0 }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property -or $null -eq $property.Value) { return [int64]0 }
    return [int64]$property.Value
}

$directory = Split-Path -Parent $OutputPath
if (-not (Test-Path -LiteralPath $directory)) {
    [void](New-Item -ItemType Directory -Path $directory -Force)
}

$plan = [pscustomobject][ordered]@{
    execute = [bool]$Execute
    control_request = "quest $BotGuid $QuestId"
    read_requests = @("questobjective $BotGuid", "acceptance $BotGuid $QuestId", "snapshot $BotGuid")
    output = $OutputPath
    writes_database = $false
}
if (-not $Execute) {
    $plan | ConvertTo-Json -Depth 10
    return
}

$baseline = Invoke-AutoWowBridge -Request "acceptance $BotGuid $QuestId"
$order = Invoke-AutoWowBridge -Request "quest $BotGuid $QuestId"
if (-not $order.ok) {
    throw "Quest directive failed: $($order | ConvertTo-Json -Compress -Depth 20)"
}

$started = [DateTimeOffset]::UtcNow
$deadline = $started.AddSeconds($DurationSeconds)
$sample = 0
$firstCount = $null
$maxCount = [int64]0
$requiredCount = [int64]0
$maxItemUsePackets = Get-Int64Property -Object $baseline.objective -Name 'quest_item_use_packet_count'
$sawSupportedLock = $false
$sawCombat = $false
$sawNamedTarget = $false
$rewarded = $false

while ([DateTimeOffset]::UtcNow -lt $deadline) {
    $sample++
    $objectiveResponse = Invoke-AutoWowBridge -Request "questobjective $BotGuid"
    $acceptance = Invoke-AutoWowBridge -Request "acceptance $BotGuid $QuestId"
    $snapshot = Invoke-AutoWowBridge -Request "snapshot $BotGuid"
    $snapshotBot = if ($null -ne $snapshot.PSObject.Properties['bot']) { $snapshot.bot } else { $snapshot }

    $objective = $objectiveResponse.objective
    if ($null -ne $objective -and [uint32]$objective.quest_id -eq $QuestId) {
        $count = [int64]$objective.current_count
        if ($null -eq $firstCount) { $firstCount = $count }
        $maxCount = [Math]::Max($maxCount, $count)
        $requiredCount = [Math]::Max($requiredCount, [int64]$objective.required_count)
        $maxItemUsePackets = [Math]::Max(
            $maxItemUsePackets,
            (Get-Int64Property -Object $objective -Name 'quest_item_use_packet_count'))
        $sawSupportedLock = $sawSupportedLock -or ([bool]$objective.supported -and [bool]$objective.has_lock)
    }

    $sawCombat = $sawCombat -or [bool]$snapshotBot.combat
    $sawNamedTarget = $sawNamedTarget -or (-not [string]::IsNullOrWhiteSpace([string]$snapshotBot.target.name))
    $rewarded = [bool]$acceptance.reward.reward_status

    $record = [pscustomobject][ordered]@{
        ts = [DateTimeOffset]::UtcNow.ToString('o')
        sample = $sample
        objective = $objectiveResponse
        acceptance = $acceptance
        snapshot = $snapshot
    }
    Add-Content -LiteralPath $OutputPath -Encoding utf8 -Value ($record | ConvertTo-Json -Compress -Depth 100)

    if ($rewarded) { break }
    if ($StopAfterObjective -and $null -ne $objective -and
        [int64]$objective.required_count -gt 0 -and
        [int64]$objective.current_count -ge [int64]$objective.required_count) { break }
    Start-Sleep -Milliseconds $PollMilliseconds
}

$final = Invoke-AutoWowBridge -Request "acceptance $BotGuid $QuestId"
$baselinePackets = Get-Int64Property -Object $baseline.objective -Name 'quest_item_use_packet_count'
$finalPackets = [Math]::Max(
    $maxItemUsePackets,
    (Get-Int64Property -Object $final.objective -Name 'quest_item_use_packet_count'))
$first = if ($null -eq $firstCount) { [int64]0 } else { [int64]$firstCount }

$summary = [pscustomobject][ordered]@{
    schema = 'autowow.quest.live-proof.v1'
    bot_guid = $BotGuid
    quest_id = $QuestId
    elapsed_seconds = [Math]::Round(([DateTimeOffset]::UtcNow - $started).TotalSeconds, 3)
    samples = $sample
    first_count = $first
    required_count = $requiredCount
    max_count = $maxCount
    objective_delta = $maxCount - $first
    objective_completed = ([bool]$final.reward.reward_status -or
        ($requiredCount -gt 0 -and $maxCount -ge $requiredCount))
    completion_basis = if ([bool]$final.reward.reward_status) { 'authoritative_reward_status' }
        elseif ($requiredCount -gt 0 -and $maxCount -ge $requiredCount) { 'observed_objective_counter' }
        else { 'not_completed' }
    quest_item_use_packet_delta = $finalPackets - $baselinePackets
    saw_supported_lock = $sawSupportedLock
    saw_combat = $sawCombat
    saw_named_gameplay_target = $sawNamedTarget
    reward_status = [bool]$final.reward.reward_status
    invariant_deltas = [pscustomobject][ordered]@{
        unrelated_offensive_target_count =
            (Get-Int64Property $final.invariants 'unrelated_offensive_target_count') -
            (Get-Int64Property $baseline.invariants 'unrelated_offensive_target_count')
        random_grind_fallback_count =
            (Get-Int64Property $final.invariants 'random_grind_fallback_count') -
            (Get-Int64Property $baseline.invariants 'random_grind_fallback_count')
        teleport_count =
            (Get-Int64Property $final.invariants 'teleport_count') -
            (Get-Int64Property $baseline.invariants 'teleport_count')
        direct_quest_db_mutation_count =
            (Get-Int64Property $final.invariants 'direct_quest_db_mutation_count') -
            (Get-Int64Property $baseline.invariants 'direct_quest_db_mutation_count')
    }
    evidence_path = $OutputPath
}

Add-Content -LiteralPath $OutputPath -Encoding utf8 -Value (
    ([pscustomobject][ordered]@{ ts = [DateTimeOffset]::UtcNow.ToString('o'); event = 'summary'; result = $summary }) |
        ConvertTo-Json -Compress -Depth 100)
$summary | ConvertTo-Json -Depth 20
