[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [ValidateRange(1,1440)][int]$DurationMinutes = 480,
    [ValidateRange(5,300)][int]$PollSeconds = 20,
    [string]$ReceiptPath = ''
)

. (Join-Path $PSScriptRoot 'Common.ps1')

$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
$simulation = Join-Path $PSScriptRoot 'league-simulation.ps1'
$control = Join-Path $PSScriptRoot 'autowow-control.ps1'
if ([string]::IsNullOrWhiteSpace($ReceiptPath)) {
    $ReceiptPath = Join-Path $ServerRoot ('leagues\results\league-v0-monitor-{0}.jsonl' -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
foreach ($required in @($simulation, $control)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Required AutoWow file is missing: $required" }
}
New-Item -ItemType Directory -Path (Split-Path -Parent $ReceiptPath) -Force | Out-Null
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)

function Write-Receipt {
    param([string]$Event, [hashtable]$Fields = @{})
    $record = [ordered]@{ timestamp_utc = (Get-Date).ToUniversalTime().ToString('o'); event = $Event }
    foreach ($key in $Fields.Keys) { $record[$key] = $Fields[$key] }
    [System.IO.File]::AppendAllText($ReceiptPath, (($record | ConvertTo-Json -Compress -Depth 8) + [Environment]::NewLine), $utf8NoBom)
}

$deadline = (Get-Date).AddMinutes($DurationMinutes)
$failures = 0
Write-Receipt -Event 'monitor_started' -Fields @{ duration_minutes = $DurationMinutes; poll_seconds = $PollSeconds }
try {
    while ((Get-Date) -lt $deadline) {
        try {
            $rosterRaw = (& $simulation -Action roster -ServerRoot $ServerRoot -AsJson | Out-String).Trim()
            if ([string]::IsNullOrWhiteSpace($rosterRaw)) { throw 'League roster returned empty output.' }
            $parsedRoster = $rosterRaw | ConvertFrom-Json
            $roster = [System.Collections.Generic.List[object]]::new()
            foreach ($entry in $parsedRoster) { $roster.Add($entry) }
            $teamByGuid = @{}
            foreach ($member in $roster) {
                $teamByGuid[[uint32]$member.guid] = if ([string]::IsNullOrWhiteSpace([string]$member.team)) { 'wayfarers' } else { [string]$member.team }
            }
            $bridgeRaw = (& $control -Action list | Out-String).Trim()
            if ([string]::IsNullOrWhiteSpace($bridgeRaw)) { throw 'AutoWow bridge returned empty output.' }
            $bridge = $bridgeRaw | ConvertFrom-Json
            $bots = @($bridge.bots | ForEach-Object {
                [ordered]@{
                    guid = [uint32]$_.guid
                    team = $teamByGuid[[uint32]$_.guid]
                    name = $_.name
                    alive = [bool]$_.alive
                    combat = [bool]$_.combat
                    level = [int]$_.progress.level
                    xp = [uint32]$_.progress.xp
                    money_copper = [uint32]$_.progress.money_copper
                    map = [int]$_.position.map
                    x = [math]::Round([double]$_.position.x, 2)
                    y = [math]::Round([double]$_.position.y, 2)
                    group_members = [int]$_.group.members
                    leader_guid = [uint32]$_.group.leader_guid
                    target = $_.target.name
                }
            })
            $teams = @{}
            foreach ($team in @('northstar','ember','wayfarers')) {
                $teamBots = @($bots | Where-Object { $_.team -eq $team })
                $alive = 0
                $combat = 0
                [uint64]$totalXp = 0
                [uint64]$totalMoney = 0
                $maxLevel = 0
                foreach ($teamBot in $teamBots) {
                    if ([bool]$teamBot['alive']) { $alive++ }
                    if ([bool]$teamBot['combat']) { $combat++ }
                    $totalXp += [uint64]$teamBot['xp']
                    $totalMoney += [uint64]$teamBot['money_copper']
                    $maxLevel = [Math]::Max($maxLevel, [int]$teamBot['level'])
                }
                $teams[$team] = [ordered]@{
                    online = $teamBots.Count
                    alive = $alive
                    combat = $combat
                    dead = $teamBots.Count - $alive
                    total_xp = $totalXp
                    total_money_copper = $totalMoney
                    max_level = $maxLevel
                }
            }
            Write-Receipt -Event 'sample' -Fields @{ online_bots = $bots.Count; teams = $teams; bots = $bots }
            $failures = 0
        }
        catch {
            $failures++
            Write-Receipt -Event 'sample_error' -Fields @{ consecutive_failures = $failures; error = $_.Exception.Message }
            if ($failures -ge 3) { throw "Monitor stopped after $failures consecutive failures: $($_.Exception.Message)" }
        }
        Start-Sleep -Seconds $PollSeconds
    }
    Write-Receipt -Event 'monitor_completed'
}
finally {
    Write-Receipt -Event 'monitor_stopped'
}

Write-Output "League v0 monitor receipt: $ReceiptPath"
