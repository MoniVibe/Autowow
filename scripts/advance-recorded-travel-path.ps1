[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [Parameter(Mandatory)][uint32]$LeaderGuid,
    [Parameter(Mandatory)][uint32]$FromNodeId,
    [Parameter(Mandatory)][uint32]$ToNodeId,
    [ValidateRange(0, 10000)][int]$StartNr = 1,
    [ValidateRange(0, 10000)][int]$EndNr = 10000,
    [ValidateRange(250, 10000)][int]$PollIntervalMs = 250,
    [ValidateRange(2, 60)][int]$StepTimeoutSeconds = 12,
    [string]$ReceiptPath = '',
    [switch]$Reverse,
    [switch]$ContinueOnCombat,
    [switch]$Apply
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$controlPath = Join-Path $ServerRoot 'scripts\autowow-control.ps1'
$pathSql = Join-Path $ServerRoot '_phase1_worktree\mod-playerbots\data\sql\playerbots\base\playerbots_travelnode_path.sql'
foreach ($required in @($controlPath, $pathSql)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required file not found: $required"
    }
}

if ([string]::IsNullOrWhiteSpace($ReceiptPath)) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $ReceiptPath = Join-Path $ServerRoot "logs\phase1-20260715\recorded-route-${FromNodeId}-${ToNodeId}-${stamp}.jsonl"
}
$ReceiptPath = [System.IO.Path]::GetFullPath($ReceiptPath)

function Invoke-ControlJson {
    param([Parameter(Mandatory)][hashtable]$Arguments)

    $raw = & $controlPath @Arguments
    return $raw | ConvertFrom-Json -Depth 30
}

function Write-Receipt {
    param([Parameter(Mandatory)][object]$Record)

    $directory = Split-Path -Parent $ReceiptPath
    if (-not (Test-Path -LiteralPath $directory)) {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
    }
    $line = ($Record | ConvertTo-Json -Depth 20 -Compress) + [Environment]::NewLine
    [System.IO.File]::AppendAllText($ReceiptPath, $line, [System.Text.UTF8Encoding]::new($false))
}

$escapedFrom = [regex]::Escape([string]$FromNodeId)
$escapedTo = [regex]::Escape([string]$ToNodeId)
$pattern = "^\(${escapedFrom}, ${escapedTo}, (?<nr>\d+), (?<map>\d+), (?<x>-?[0-9.]+), (?<y>-?[0-9.]+), (?<z>-?[0-9.]+)\)"
$allPoints = @(
    Select-String -LiteralPath $pathSql -Pattern $pattern | ForEach-Object {
        if ($_.Line -match $pattern) {
            [pscustomobject][ordered]@{
                nr = [int]$Matches.nr
                map = [uint32]$Matches.map
                x = [float]::Parse($Matches.x, [Globalization.CultureInfo]::InvariantCulture)
                y = [float]::Parse($Matches.y, [Globalization.CultureInfo]::InvariantCulture)
                z = [float]::Parse($Matches.z, [Globalization.CultureInfo]::InvariantCulture)
            }
        }
    } | Sort-Object nr
)
$selectedPoints = @($allPoints | Where-Object { $_.nr -ge $StartNr -and $_.nr -le $EndNr })
$points = if ($Reverse) { @($selectedPoints | Sort-Object nr -Descending) } else { $selectedPoints }
if ($points.Count -eq 0) {
    throw "No recorded path points matched ${FromNodeId}->${ToNodeId} in nr range ${StartNr}..${EndNr}."
}
if (@($points.map | Sort-Object -Unique).Count -ne 1) {
    throw 'Recorded path crosses maps; coordinate advance supports one live map at a time.'
}

$raid = Invoke-ControlJson -Arguments @{ Action = 'raid-status'; BotGuid = $LeaderGuid; TimeoutMs = 120000 }
if (-not $raid.ok -or -not $raid.proof.exact_roster -or -not $raid.proof.all_online_playerbots -or
    -not $raid.proof.same_map -or -not $raid.proof.same_instance) {
    throw 'The exact online raid is not assembled on one map and instance.'
}
$roster = @([uint32[]]$raid.expected_roster)
if ($raid.map -ne $points[0].map) {
    throw "Raid is on map $($raid.map), but recorded path is on map $($points[0].map)."
}

$plan = [pscustomobject][ordered]@{
    schema = 'autowow.recorded-travel-route.v1'
    apply = [bool]$Apply
    leader_guid = $LeaderGuid
    roster = $roster
    from_node_id = $FromNodeId
    to_node_id = $ToNodeId
    map = $points[0].map
    start_nr = $points[0].nr
    end_nr = $points[-1].nr
    point_count = $points.Count
    reverse = [bool]$Reverse
    receipt_path = $ReceiptPath
}
if (-not $Apply) {
    return $plan
}

if (Test-Path -LiteralPath $ReceiptPath) {
    throw "Receipt already exists; refusing to append a second run: $ReceiptPath"
}

$completed = 0
$terminal = 'complete'
foreach ($point in $points) {
    $previous = if ($Reverse) {
        @($allPoints | Where-Object { $_.nr -gt $point.nr } | Select-Object -First 1)
    } else {
        @($allPoints | Where-Object { $_.nr -lt $point.nr } | Select-Object -Last 1)
    }
    if ($previous.Count -ne 1) {
        throw "Recorded path point $($point.nr) has no preceding tangent sample."
    }
    $orientation = [Math]::Atan2(
        [double]$point.y - [double]$previous[0].y,
        [double]$point.x - [double]$previous[0].x)
    $issuedUtc = (Get-Date).ToUniversalTime().ToString('o')
    $response = Invoke-ControlJson -Arguments @{
        Action = 'advance-point'
        BotGuid = $LeaderGuid
        CoordinateX = $point.x
        CoordinateY = $point.y
        CoordinateZ = $point.z
        UseCoordinateOrientation = $true
        CoordinateOrientation = $orientation
        TimeoutMs = 120000
    }

    $deadline = (Get-Date).AddSeconds($StepTimeoutSeconds)
    $bots = @()
    $leaderDistance = [double]::PositiveInfinity
    do {
        Start-Sleep -Milliseconds $PollIntervalMs
        $list = Invoke-ControlJson -Arguments @{ Action = 'list'; TimeoutMs = 120000 }
        $bots = @($list.bots | Where-Object { [uint32]$_.guid -in $roster })
        $leader = @($bots | Where-Object { [uint32]$_.guid -eq $LeaderGuid } | Select-Object -First 1)
        if ($leader.Count -eq 1) {
            $dx = [double]$leader[0].position.x - [double]$point.x
            $dy = [double]$leader[0].position.y - [double]$point.y
            $dz = [double]$leader[0].position.z - [double]$point.z
            $leaderDistance = [Math]::Sqrt($dx * $dx + $dy * $dy + $dz * $dz)
        }
        $combatCount = @($bots | Where-Object { $_.combat }).Count
    } while ($response.ok -and $leaderDistance -gt 1.5 -and $combatCount -eq 0 -and (Get-Date) -lt $deadline)

    $aliveCount = @($bots | Where-Object { $_.alive }).Count
    $maps = @($bots | ForEach-Object { "$($_.position.map)" } | Sort-Object -Unique)
    $healthPcts = @($bots | ForEach-Object { [double]$_.health.pct })
    $minimumHealth = if ($healthPcts.Count) { ($healthPcts | Measure-Object -Minimum).Minimum } else { 0.0 }
    $issues = [System.Collections.Generic.List[string]]::new()
    if (-not $response.ok) { $issues.Add("advance_rejected:$($response.error)") }
    if ($response.ok -and [int]$response.started_members -ne $roster.Count) { $issues.Add('not_all_members_started') }
    if ($bots.Count -ne $roster.Count) { $issues.Add('roster_offline_or_missing') }
    if ($aliveCount -ne $roster.Count) { $issues.Add('raid_death') }
    if ($maps.Count -ne 1 -or [uint32]$maps[0] -ne [uint32]$point.map) { $issues.Add('raid_map_split') }
    if ($leaderDistance -gt 1.5 -and $combatCount -eq 0) { $issues.Add('leader_did_not_reach_point') }
    if (-not $ContinueOnCombat -and $combatCount -ne 0) { $issues.Add('combat_started') }

    $record = [pscustomobject][ordered]@{
        schema = 'autowow.recorded-travel-route.step.v1'
        issued_utc = $issuedUtc
        observed_utc = (Get-Date).ToUniversalTime().ToString('o')
        from_node_id = $FromNodeId
        to_node_id = $ToNodeId
        nr = $point.nr
        destination = $point
        response = $response
        observation = [pscustomobject][ordered]@{
            roster_count = $bots.Count
            alive_count = $aliveCount
            combat_count = $combatCount
            minimum_health_pct = $minimumHealth
            leader_distance = $leaderDistance
            map_ids = $maps
            players = @($bots | ForEach-Object {
                [pscustomobject][ordered]@{
                    guid = [uint32]$_.guid
                    alive = [bool]$_.alive
                    combat = [bool]$_.combat
                    health_pct = [double]$_.health.pct
                    map = [uint32]$_.position.map
                    x = [double]$_.position.x
                    y = [double]$_.position.y
                    z = [double]$_.position.z
                    target_guid = [uint64]$_.target.guid
                }
            })
        }
        issues = @($issues)
    }
    Write-Receipt -Record $record

    if ($issues.Count -ne 0) {
        $terminal = $issues[0]
        break
    }
    $completed++
}

[pscustomobject][ordered]@{
    schema = 'autowow.recorded-travel-route.summary.v1'
    status = if ($completed -eq $points.Count) { 'PASS' } else { 'STOPPED' }
    terminal = $terminal
    completed_points = $completed
    requested_points = $points.Count
    last_completed_nr = if ($completed) { $points[$completed - 1].nr } else { $null }
    receipt_path = $ReceiptPath
}
