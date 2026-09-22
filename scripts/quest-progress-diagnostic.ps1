<#!
.SYNOPSIS
    Summarize a live AutoWoW quest run without issuing control orders.

.DESCRIPTION
    Reads the newest quest telemetry JSONL, league-director JSONL, and Playerbots.log.
    It reports XP/movement/objective deltas, quest IDs observed, director decisions,
    no-teleport holds, and quest-abandonment signatures. It does not connect to MySQL,
    open the bridge, or modify the running server.
#>
[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$TelemetryPath = '',
    [string]$DirectorPath = '',
    [string]$ReportPath = ''
)

$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path

function Get-NewestFile {
    param(
        [Parameter(Mandatory = $true)][string]$Directory,
        [Parameter(Mandatory = $true)][string]$Filter
    )
    Get-ChildItem -LiteralPath $Directory -Filter $Filter -File -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
}

function Read-JsonLines {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return @() }

    $records = foreach ($line in (Get-Content -LiteralPath $Path)) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        try { $line | ConvertFrom-Json }
        catch { Write-Warning "Ignoring malformed JSONL line in $Path" }
    }
    return @($records)
}

function Get-Int64OrZero {
    param([object]$Value)
    if ($null -eq $Value) { return [int64]0 }
    return [int64]$Value
}

$telemetryFile = if ([string]::IsNullOrWhiteSpace($TelemetryPath)) {
    Get-NewestFile -Directory (Join-Path $ServerRoot 'logs') -Filter 'quest-telemetry-overnight-*.jsonl'
} else {
    Get-Item -LiteralPath $TelemetryPath
}
$directorFile = if ([string]::IsNullOrWhiteSpace($DirectorPath)) {
    Get-NewestFile -Directory (Join-Path $ServerRoot 'leagues\results') -Filter 'league-v0-director-*.jsonl'
} else {
    Get-Item -LiteralPath $DirectorPath
}

if ($null -eq $telemetryFile) { throw 'No quest telemetry JSONL was found.' }
if ($null -eq $directorFile) { throw 'No league director JSONL was found.' }

$telemetry = @(Read-JsonLines -Path $telemetryFile.FullName)
$director = @(Read-JsonLines -Path $directorFile.FullName)
$samples = @($telemetry | Where-Object { $_.event -eq 'quest_sample' })
$directorEvents = @($director | Where-Object { $_.event })

$logPath = Join-Path $ServerRoot 'server\logs\Playerbots.log'
$logLines = if (Test-Path -LiteralPath $logPath) { @(Get-Content -LiteralPath $logPath) } else { @() }
$holdLines = @($logLines | Where-Object { $_ -match 'AutoWow no-teleport hold' })
$abandonLines = @($logLines | Where-Object { $_ -match 'marked as abandoned quest' })
$bridgeEofLines = @($logLines | Where-Object { $_ -match 'AutoWow bridge client error: read_until: End of file' })

$botSummaries = foreach ($guid in @($samples | ForEach-Object { [uint32]$_.bot_guid } | Sort-Object -Unique)) {
    $botSamples = @($samples | Where-Object { [uint32]$_.bot_guid -eq $guid })
    if ($botSamples.Count -eq 0) { continue }

    $first = $botSamples | Select-Object -First 1
    $last = $botSamples | Select-Object -Last 1
    $questIds = @(
        $botSamples | ForEach-Object { @($_.active_quests) } | ForEach-Object { if ($_.id) { [uint32]$_.id } } |
            Sort-Object -Unique
    )
    [pscustomobject]@{
        guid = $guid
        name = [string]$last.name
        samples = $botSamples.Count
        level_start = [int]$first.level
        level_end = [int]$last.level
        xp_start = [int64]$first.xp
        xp_end = [int64]$last.xp
        xp_delta = (Get-Int64OrZero $last.xp) - (Get-Int64OrZero $first.xp)
        objective_delta_sum = [int64](($botSamples | Measure-Object -Property objective_delta -Sum).Sum)
        movement_sum_yards = [math]::Round([double](($botSamples | Measure-Object -Property moved_yards -Sum).Sum), 1)
        quest_ids = if ($questIds.Count) { $questIds -join ', ' } else { '(none)' }
        no_progress_samples = @($botSamples | Where-Object { $_.flat_cycles -gt 0 }).Count
    }
}

$eventCounts = $directorEvents | Group-Object event | Sort-Object Name
$reportLines = [System.Collections.Generic.List[string]]::new()
$reportLines.Add('# AutoWoW quest progress diagnostic')
$reportLines.Add('')
$reportLines.Add("Generated: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')")
$reportLines.Add('')
$reportLines.Add('This report is read-only. It does not connect to MySQL, issue bridge orders, or modify the server.')
$reportLines.Add('')
$reportLines.Add("- Telemetry: ``$($telemetryFile.FullName)``")
$reportLines.Add("- Director: ``$($directorFile.FullName)``")
$reportLines.Add("- Playerbots log: ``$logPath``")
$reportLines.Add('')
$reportLines.Add('## Bot summaries')
$reportLines.Add('')
$reportLines.Add('| GUID | Name | Samples | Level | XP delta | Objective delta | Movement yards | Quest IDs | Flat samples |')
$reportLines.Add('|---:|---|---:|---:|---:|---:|---:|---|---:|')
foreach ($summary in $botSummaries) {
    $reportLines.Add("| $($summary.guid) | $($summary.name) | $($summary.samples) | $($summary.level_start) -> $($summary.level_end) | $($summary.xp_delta) | $($summary.objective_delta_sum) | $($summary.movement_sum_yards) | $($summary.quest_ids) | $($summary.no_progress_samples) |")
}
$reportLines.Add('')
$reportLines.Add('## Director events')
$reportLines.Add('')
foreach ($group in $eventCounts) { $reportLines.Add("- ``$($group.Name)``: $($group.Count)") }
$reportLines.Add('')
$reportLines.Add('## Server signatures')
$reportLines.Add('')
$reportLines.Add("- No-teleport hold lines: $($holdLines.Count)")
$reportLines.Add("- Quest-abandonment lines: $($abandonLines.Count)")
$reportLines.Add("- Bridge EOF lines: $($bridgeEofLines.Count)")
if ($abandonLines.Count) {
    $reportLines.Add('')
    $reportLines.Add('Recent quest-abandonment lines:')
    foreach ($line in ($abandonLines | Select-Object -Last 10)) { $reportLines.Add("- ``$line``") }
}

if ([string]::IsNullOrWhiteSpace($ReportPath)) {
    $ReportPath = Join-Path $ServerRoot ('logs\quest-progress-diagnostic-{0}.md' -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
$reportDirectory = Split-Path -Parent $ReportPath
New-Item -ItemType Directory -Path $reportDirectory -Force | Out-Null
[System.IO.File]::WriteAllText($ReportPath, ($reportLines -join [Environment]::NewLine) + [Environment]::NewLine, [System.Text.UTF8Encoding]::new($false))
Write-Output "Quest progress diagnostic written: $ReportPath"
