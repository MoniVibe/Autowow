[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [ValidateRange(1,1440)][int]$DurationMinutes = 480,
    [ValidateRange(60,1800)][int]$PollSeconds = 300,
    [string]$ReceiptPath = '',
    [switch]$LibraryOnly
)

$script:LeagueProgressionCampaignRoster = @(
    [pscustomobject][ordered]@{ guid = [uint32]7;  team = 'ember' },
    [pscustomobject][ordered]@{ guid = [uint32]12; team = 'ember' },
    [pscustomobject][ordered]@{ guid = [uint32]13; team = 'ember' },
    [pscustomobject][ordered]@{ guid = [uint32]18; team = 'ember' },
    [pscustomobject][ordered]@{ guid = [uint32]20; team = 'ember' },
    [pscustomobject][ordered]@{ guid = [uint32]10; team = 'northstar' },
    [pscustomobject][ordered]@{ guid = [uint32]11; team = 'northstar' },
    [pscustomobject][ordered]@{ guid = [uint32]14; team = 'northstar' },
    [pscustomobject][ordered]@{ guid = [uint32]15; team = 'northstar' },
    [pscustomobject][ordered]@{ guid = [uint32]19; team = 'northstar' },
    [pscustomobject][ordered]@{ guid = [uint32]3;  team = 'wayfarers' },
    [pscustomobject][ordered]@{ guid = [uint32]6;  team = 'wayfarers' },
    [pscustomobject][ordered]@{ guid = [uint32]49; team = 'wayfarers' }
)

function Get-LeagueProgressionCampaignRoster {
    @($script:LeagueProgressionCampaignRoster | ForEach-Object {
        [pscustomobject][ordered]@{ guid = [uint32]$_.guid; team = [string]$_.team }
    })
}

function Select-LeagueProgressionCampaignBots {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [object[]]$BridgeBots
    )

    $bridgeByGuid = @{}
    foreach ($bot in @($BridgeBots)) {
        $guid = [uint32]$bot.guid
        if (-not $bridgeByGuid.ContainsKey($guid)) { $bridgeByGuid[$guid] = $bot }
    }

    $included = @(
        foreach ($member in @(Get-LeagueProgressionCampaignRoster)) {
            $guid = [uint32]$member.guid
            if (-not $bridgeByGuid.ContainsKey($guid)) { continue }
            $bot = $bridgeByGuid[$guid]
            [pscustomobject][ordered]@{
                guid = $guid
                team = [string]$member.team
                name = [string]$bot.name
                level = [int]$bot.progress.level
                xp = [uint32]$bot.progress.xp
                money_copper = [uint32]$bot.progress.money_copper
                alive = [bool]$bot.alive
                combat = [bool]$bot.combat
            }
        }
    )

    [pscustomobject][ordered]@{
        observed_count = @($BridgeBots).Count
        included_count = $included.Count
        excluded_count = [Math]::Max(0, @($BridgeBots).Count - $included.Count)
        bots = $included
    }
}

function Get-LeagueProgressionDeltas {
    param(
        [Parameter(Mandatory = $true)][hashtable]$Baseline,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]]$Bots
    )

    @($Bots | ForEach-Object {
        $initial = $Baseline[[uint32]$_.guid]
        if (-not $initial) { throw "No baseline exists for bot GUID $($_.guid)." }
        [pscustomobject][ordered]@{
            guid = $_.guid
            team = $_.team
            name = $_.name
            level = $_.level
            level_delta = $_.level - $initial.level
            xp = $_.xp
            xp_delta_same_level = if ($_.level -eq $initial.level) { [int64]$_.xp - [int64]$initial.xp } else { $null }
            money_copper = $_.money_copper
            money_delta_copper = [int64]$_.money_copper - [int64]$initial.money_copper
            alive = $_.alive
            combat = $_.combat
        }
    })
}

function Get-LeagueProgressionTeamTotals {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [object[]]$Deltas
    )

    $teams = [ordered]@{}
    foreach ($team in @('northstar', 'ember', 'wayfarers')) {
        $teamDeltas = @($Deltas | Where-Object { $_.team -eq $team })
        [int64]$levelGains = 0
        [int64]$xpDeltaSameLevel = 0
        [int64]$moneyDeltaCopper = 0
        $inCombat = 0
        $dead = 0
        foreach ($teamDelta in $teamDeltas) {
            $levelGains += [int64]$teamDelta.level_delta
            if ($null -ne $teamDelta.xp_delta_same_level) { $xpDeltaSameLevel += [int64]$teamDelta.xp_delta_same_level }
            $moneyDeltaCopper += [int64]$teamDelta.money_delta_copper
            if ([bool]$teamDelta.combat) { $inCombat++ }
            if (-not [bool]$teamDelta.alive) { $dead++ }
        }
        $teams[$team] = [ordered]@{
            online = $teamDeltas.Count
            level_gains = $levelGains
            xp_delta_same_level = $xpDeltaSameLevel
            money_delta_copper = $moneyDeltaCopper
            in_combat = $inCombat
            dead = $dead
        }
    }
    return $teams
}

if ($LibraryOnly) { return }

. (Join-Path $PSScriptRoot 'Common.ps1')

$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
$control = Join-Path $PSScriptRoot 'autowow-control.ps1'
if ([string]::IsNullOrWhiteSpace($ReceiptPath)) {
    $ReceiptPath = Join-Path $ServerRoot ('leagues\results\league-v0-progression-{0}.jsonl' -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
foreach ($required in @($control)) {
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

function Get-ProgressSample {
    $bridgeRaw = (& $control -Action list | Out-String).Trim()
    if ([string]::IsNullOrWhiteSpace($bridgeRaw)) { throw 'AutoWow bridge returned empty output.' }
    $bridge = $bridgeRaw | ConvertFrom-Json
    if (-not $bridge.ok) { throw 'AutoWow bridge returned an unsuccessful response.' }
    return Select-LeagueProgressionCampaignBots -BridgeBots @($bridge.bots)
}

$deadline = (Get-Date).AddMinutes($DurationMinutes)
$baseline = @{}
$failures = 0
Write-Receipt -Event 'progression_observer_started' -Fields @{ duration_minutes = $DurationMinutes; poll_seconds = $PollSeconds }
try {
    while ((Get-Date) -lt $deadline) {
        try {
            $observation = Get-ProgressSample
            $sample = @($observation.bots)
            if ($baseline.Count -eq 0) {
                foreach ($bot in $sample) { $baseline[[uint32]$bot.guid] = $bot }
                Write-Receipt -Event 'baseline' -Fields @{
                    bots = $sample
                    observed_bot_count = $observation.observed_count
                    included_bot_count = $observation.included_count
                    excluded_bot_count = $observation.excluded_count
                }
            }
            else {
                $deltas = @(Get-LeagueProgressionDeltas -Baseline $baseline -Bots $sample)
                $teams = Get-LeagueProgressionTeamTotals -Deltas $deltas
                Write-Receipt -Event 'progress_delta' -Fields @{
                    teams = $teams
                    bots = $deltas
                    observed_bot_count = $observation.observed_count
                    included_bot_count = $observation.included_count
                    excluded_bot_count = $observation.excluded_count
                }
            }
            $failures = 0
        }
        catch {
            $failures++
            Write-Receipt -Event 'progression_observer_error' -Fields @{ consecutive_failures = $failures; error = $_.Exception.Message; stack = $_.ScriptStackTrace }
            if ($failures -ge 3) { throw "Progression observer stopped after $failures consecutive failures: $($_.Exception.Message)" }
        }
        Start-Sleep -Seconds $PollSeconds
    }
    Write-Receipt -Event 'progression_observer_completed'
}
finally {
    Write-Receipt -Event 'progression_observer_stopped'
}

Write-Output "League progression receipt: $ReceiptPath"
