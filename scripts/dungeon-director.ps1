[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [uint32]$LeaderGuid = 2,
    [uint32[]]$PartyGuids = @(2,26,8,45,24),
    [string[]]$Waypoint = @(),
    [string]$StartAt = '',
    [ValidateRange(5,60)][double]$ArrivalRadius = 15,
    # Ranged formation spacing can settle near 40 yards; larger persistent gaps trigger backtracking.
    [ValidateRange(15,60)][double]$CohesionRadius = 45,
    [ValidateRange(1,10)][int]$PollSeconds = 3,
    [ValidateRange(1,180)][int]$MaxMinutes = 30,
    [ValidateRange(5000,120000)][int]$ControlTimeoutMs = 30000,
    [string]$ReceiptPath = '',
    [switch]$Apply
)

$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
$control = Join-Path $PSScriptRoot 'autowow-control.ps1'
if (-not $ReceiptPath) {
    $ReceiptPath = Join-Path $ServerRoot ('logs\phase1-{0}\dungeon-director-{1}.jsonl' -f `
        (Get-Date -Format 'yyyyMMdd'), (Get-Date -Format 'yyyyMMdd-HHmmss'))
}

$route = [ordered]@{
    # Map 574 contains stacked floors with nearly identical X/Y coordinates. Keep Z in the
    # director contract so reaching the floor below cannot be mistaken for reaching a stair landing.
    'uk-forge-end'  = @(390.0, -3.0, 22.8)
    'uk-drake-room' = @(385.0, 145.0, 30.8)
    # The Keleseth approach crosses stacked floor components. The direct drake-room -> Keleseth
    # named goal can strand the party on an upper landing with a SHORTCUT|NOPATH final segment.
    # These observed mmap-safe checkpoints keep the route on connected components.
    'uk-keleseth-lower-loop' = @(300.0, 210.0, 31.0)
    'uk-keleseth-ramp-base'  = @(285.0, 210.0, 32.0)
    'uk-keleseth-platform'   = @(255.0, 235.0, 42.8)
    'uk-keleseth'   = @(220.0, 200.0, 40.9)
    'uk-tunnel'     = @(105.0, 240.0, 43.0)
    # The nominal lower-hall point at (96,186,49.4) is not reachable directly
    # from all tunnel-side follower positions on the deployed mmap component.
    'uk-lower-hall-descent' = @(100.0, 230.0, 47.0)
    'uk-lower-hall-mid' = @(90.0, 210.0, 49.4)
    # This nearby point then bridges safely back to the nominal lower hall.
    'uk-lower-hall-approach' = @(90.0, 186.0, 49.4)
    'uk-lower-hall' = @(96.0, 186.0, 49.4)
    'uk-stairs-ramp' = @(95.0, 140.0, 65.8)
    'uk-stairs-1'   = @(105.0, 92.0, 65.8)
    'uk-stairs-2'   = @(89.0, 98.0, 87.2)
    'uk-stairs-3'   = @(108.0, 58.0, 109.1)
    'uk-skarvald'   = @(112.0, -30.0, 118.9)
    'uk-upper-ramp' = @(238.0, 13.0, 135.3)
    'uk-upper-hall' = @(230.0, -6.0, 178.6)
    'uk-worg-hall'  = @(266.0, -102.0, 190.5)
    'uk-worg-turn'  = @(281.0, -148.0, 190.5)
    'uk-gauntlet-east' = @(210.0, -176.0, 190.0)
    # These are centroids from one connected NAV_GROUND component in the deployed
    # 5743132 mmap tile. The old (154,-178,180.6) shortcut overlapped another floor.
    'uk-gauntlet-portcullis' = @(167.133, -202.933, 180.566)
    'uk-west-corridor' = @(181.556, -223.644, 180.677)
    'uk-corridor-bend' = @(195.533, -242.400, 180.633)
    'uk-southwest-corridor' = @(207.680, -268.480, 180.500)
    'uk-south-corridor' = @(226.187, -274.507, 180.500)
    'uk-ingvar-north' = @(239.533, -275.667, 180.500)
    'uk-inner-approach' = @(245.556, -287.378, 180.500)
    'uk-final-approach' = @(250.222, -308.356, 180.500)
    'uk-ingvar-platform' = @(245.822, -331.733, 180.500)
}

$strictUpperFloorWaypoints = @(
    'uk-keleseth-platform',
    'uk-gauntlet-portcullis', 'uk-west-corridor', 'uk-corridor-bend',
    'uk-southwest-corridor', 'uk-south-corridor', 'uk-ingvar-north',
    'uk-inner-approach', 'uk-final-approach', 'uk-ingvar-platform'
)
$coordinateWaypoints = @(
    'uk-keleseth-lower-loop',
    'uk-keleseth-ramp-base',
    'uk-keleseth-platform',
    'uk-lower-hall-descent',
    'uk-lower-hall-mid',
    'uk-lower-hall-approach',
    'uk-stairs-ramp'
)
$allRouteNames = [string[]]$route.Keys

if ($Waypoint.Count -eq 0) { $Waypoint = @($route.Keys) }
foreach ($name in $Waypoint) {
    if (-not $route.Contains($name)) { throw "Unknown dungeon waypoint: $name" }
}
if ($StartAt) {
    $index = [Array]::IndexOf([string[]]$Waypoint, $StartAt)
    if ($index -lt 0) { throw "StartAt waypoint is not in the selected route: $StartAt" }
    $Waypoint = @($Waypoint[$index..($Waypoint.Count - 1)])
}
if ($PartyGuids.Count -ne 5 -or @($PartyGuids | Select-Object -Unique).Count -ne 5) {
    throw 'Dungeon Director V0 requires exactly five unique party GUIDs.'
}
if ($LeaderGuid -notin $PartyGuids) { throw 'LeaderGuid must be one of PartyGuids.' }

function Write-Receipt {
    param([string]$Event, [hashtable]$Fields = @{})
    $row = [ordered]@{ timestamp_utc = (Get-Date).ToUniversalTime().ToString('o'); event = $Event }
    foreach ($key in $Fields.Keys) { $row[$key] = $Fields[$key] }
    $directory = Split-Path -Parent $ReceiptPath
    if (-not (Test-Path -LiteralPath $directory)) { New-Item -ItemType Directory -Path $directory -Force | Out-Null }
    [IO.File]::AppendAllText($ReceiptPath, (($row | ConvertTo-Json -Compress -Depth 15) + [Environment]::NewLine),
        [Text.UTF8Encoding]::new($false))
}

function Invoke-ControlJson {
    param(
        [string]$Action,
        [uint32]$Guid = 0,
        [string]$Destination = '',
        [double[]]$Coordinate = @()
    )
    $args = @{ Action = $Action; TimeoutMs = $ControlTimeoutMs }
    if ($Guid) { $args.BotGuid = $Guid }
    if ($Destination) { $args.Destination = $Destination }
    if ($Coordinate.Count) {
        if ($Coordinate.Count -ne 3) { throw 'Coordinate requires exactly x, y, z.' }
        $args.CoordinateX = [float]$Coordinate[0]
        $args.CoordinateY = [float]$Coordinate[1]
        $args.CoordinateZ = [float]$Coordinate[2]
    }
    return ((& $control @args | Out-String).Trim() | ConvertFrom-Json)
}

function Invoke-AdvanceWaypoint {
    param([string]$Name, [double[]]$Target)
    if ($Name -in $coordinateWaypoints) {
        return Invoke-ControlJson -Action 'advance-point' -Guid $LeaderGuid -Coordinate $Target
    }
    return Invoke-ControlJson -Action advance -Guid $LeaderGuid -Destination $Name
}

function Invoke-TankPickup {
    param([object[]]$Bots, [string]$Waypoint)
    $combatStragglers = @($Bots | Where-Object { [uint32]$_.guid -ne $LeaderGuid -and [bool]$_.combat })
    if (-not $combatStragglers.Count) { return $null }

    $pickup = @($combatStragglers | Sort-Object @{ Expression = { [double]$_.health.pct }; Ascending = $true })[0]
    $moveResponse = Invoke-ControlJson -Action 'advance-point' -Guid $LeaderGuid -Coordinate @(
        [double]$pickup.position.x,
        [double]$pickup.position.y,
        [double]$pickup.position.z
    )
    $engageResponse = Invoke-ControlJson -Action engage -Guid $LeaderGuid
    Write-Receipt -Event 'tank_pickup_reposition_requested' -Fields @{
        waypoint = $Waypoint
        straggler_guid = [uint32]$pickup.guid
        straggler_health_pct = [Math]::Round([double]$pickup.health.pct, 2)
        straggler_position = $pickup.position
        move_response = $moveResponse
        engage_response = $engageResponse
    }
    return $moveResponse
}

function Get-PartyState {
    $lastError = $null
    for ($attempt = 1; $attempt -le 5; ++$attempt) {
        try {
            $list = Invoke-ControlJson -Action list
            $bots = @($list.bots | Where-Object { [uint32]$_.guid -in $PartyGuids })
            if ($bots.Count -eq 5) { return $bots }
            $missing = @($PartyGuids | Where-Object { [uint32]$_ -notin @($bots.guid) })
            $lastError = "temporarily missing GUIDs: $($missing -join ',')"
        }
        catch { $lastError = $_.Exception.Message }
        if ($attempt -lt 5) { Start-Sleep -Seconds 2 }
    }
    throw "All five dungeon bots must be online after transition retries ($lastError)."
}

function Get-Distance3d {
    param([object]$Bot, [double[]]$Target)
    return [Math]::Sqrt([Math]::Pow([double]$Bot.position.x - $Target[0], 2) +
        [Math]::Pow([double]$Bot.position.y - $Target[1], 2) +
        [Math]::Pow([double]$Bot.position.z - $Target[2], 2))
}

$plan = [ordered]@{
    schema = 'autowow.dungeon-director.v0'
    apply = [bool]$Apply
    leader_guid = $LeaderGuid
    party_guids = @($PartyGuids)
    waypoints = @($Waypoint)
    arrival_radius = $ArrivalRadius
    cohesion_radius = $CohesionRadius
    max_minutes = $MaxMinutes
    receipt = $ReceiptPath
    movement = 'named goals through Playerbots normal mmap pathfinding; no dungeon teleports'
}
if (-not $Apply) { [pscustomobject]$plan | ConvertTo-Json -Depth 8; return }

$initial = Get-PartyState
foreach ($bot in $initial) {
    if (-not [bool]$bot.alive) { throw "GUID $($bot.guid) is dead." }
    if ([uint32]$bot.group.leader_guid -ne $LeaderGuid -or [int]$bot.group.members -ne 5) {
        throw "GUID $($bot.guid) is not in the expected five-player party."
    }
    if ([int]$bot.position.map -ne 574) { throw "GUID $($bot.guid) is not in Utgarde Keep." }
}

$instances = @($PartyGuids | ForEach-Object {
    [uint32](Invoke-ControlJson -Action combatlog -Guid $_).telemetry.location.instance_id
} | Select-Object -Unique)
if ($instances.Count -ne 1 -or $instances[0] -eq 0) { throw 'The party is not in one shared dungeon instance.' }

Write-Receipt -Event 'director_started' -Fields @{ plan = $plan; instance_id = $instances[0] }
$deadline = (Get-Date).AddMinutes($MaxMinutes)
$orderCount = 0
$combatPolls = 0
$leaderHeld = $false

try {
foreach ($name in $Waypoint) {
    $target = [double[]]$route[$name]
    $lastDistance = [double]::PositiveInfinity
    $acceptedAdvance = $false
    $stalledOrders = 0
    $wrongFloorPolls = 0
    $cohesionHoldPolls = 0
    while ((Get-Date) -lt $deadline) {
        $bots = Get-PartyState
        $leader = $bots | Where-Object { [uint32]$_.guid -eq $LeaderGuid }
        $dead = @($bots | Where-Object { -not [bool]$_.alive })
        if ($dead.Count) {
            if ($leaderHeld) { [void](Invoke-ControlJson -Action resume -Guid $LeaderGuid) }
            Write-Receipt -Event 'director_failed' -Fields @{ reason = 'party_death'; waypoint = $name; dead_guids = @($dead.guid) }
            throw "Party death while advancing to $name."
        }

        $leaderPosition = [double[]]@($leader.position.x, $leader.position.y, $leader.position.z)
        $maximumSeparation = [double](($bots | Where-Object { [uint32]$_.guid -ne $LeaderGuid } | ForEach-Object {
            Get-Distance3d -Bot $_ -Target $leaderPosition
        } | Measure-Object -Maximum).Maximum)
        $busy = @($bots | Where-Object { [bool]$_.combat }).Count

        # A moving tank can outrun ranged followers on stairs and stacked dungeon floors. Hold the
        # leader at ordinary world position while the party catches up; this is AI control only,
        # never a teleport or coordinate correction.
        if (-not $busy -and $maximumSeparation -gt $CohesionRadius) {
            if (-not $leaderHeld) {
                $holdResponse = Invoke-ControlJson -Action pause -Guid $LeaderGuid
                if (-not [bool]$holdResponse.ok) { throw "Leader hold failed at ${name}: $($holdResponse.error)" }
                $leaderHeld = $true
            }
            ++$cohesionHoldPolls
            # A follower can be stranded behind a boss-room threshold while the tank waits inside.
            # Repeating +follow cannot cross a closed encounter door. After a persistent hold, walk
            # the tank back to the previous safe waypoint through ordinary mmap pathfinding, allow
            # the party to reform, then retry this waypoint. No position is corrected or teleported.
            if ($cohesionHoldPolls -ge 10) {
                $routeIndex = [Array]::IndexOf($allRouteNames, $name)
                if ($routeIndex -gt 0) {
                    $previousName = $allRouteNames[$routeIndex - 1]
                    $resumeResponse = Invoke-ControlJson -Action resume -Guid $LeaderGuid
                    if (-not [bool]$resumeResponse.ok) { throw "Cohesion recovery resume failed at ${name}: $($resumeResponse.error)" }
                    $leaderHeld = $false
                    $previousTarget = [double[]]$route[$previousName]
                    $backtrackResponse = Invoke-AdvanceWaypoint -Name $previousName -Target $previousTarget
                    if (-not [bool]$backtrackResponse.ok) { throw "Cohesion backtrack failed at ${name}: $($backtrackResponse.error)" }
                    ++$orderCount
                    Write-Receipt -Event 'cohesion_backtrack' -Fields @{
                        waypoint = $name
                        previous_waypoint = $previousName
                        maximum_separation = [Math]::Round($maximumSeparation, 2)
                        response = $backtrackResponse
                    }
                    $cohesionHoldPolls = 0
                    Start-Sleep -Seconds 20
                    continue
                }
            }
            Write-Receipt -Event 'cohesion_hold' -Fields @{
                waypoint = $name
                maximum_separation = [Math]::Round($maximumSeparation, 2)
            }
            Start-Sleep -Seconds $PollSeconds
            continue
        }
        $cohesionHoldPolls = 0
        if ($leaderHeld) {
            $resumeResponse = Invoke-ControlJson -Action resume -Guid $LeaderGuid
            if (-not [bool]$resumeResponse.ok) { throw "Leader resume failed at ${name}: $($resumeResponse.error)" }
            $leaderHeld = $false
            Write-Receipt -Event 'cohesion_resumed' -Fields @{ waypoint = $name; maximum_separation = [Math]::Round($maximumSeparation, 2) }
        }

        $distance = Get-Distance3d -Bot $leader -Target $target
        $arrivalRadiusForWaypoint = if ($name -in $coordinateWaypoints) { [Math]::Min([double]$ArrivalRadius, 3.0) } else { [double]$ArrivalRadius }
        if ($distance -le $arrivalRadiusForWaypoint) {
            if ($name -in $strictUpperFloorWaypoints) {
                $wrongFloor = @($bots | Where-Object {
                    [Math]::Abs([double]$_.position.z - $target[2]) -gt 1.5
                })
                if ($wrongFloor.Count) {
                    ++$wrongFloorPolls
                    Write-Receipt -Event 'wrong_floor_rejected' -Fields @{
                        waypoint = $name
                        expected_z = $target[2]
                        bot_positions = @($wrongFloor | ForEach-Object {
                            [ordered]@{ guid = [uint32]$_.guid; x = $_.position.x; y = $_.position.y; z = $_.position.z }
                        })
                    }
                    if ($wrongFloorPolls -ge 10) {
                        Write-Receipt -Event 'director_failed' -Fields @{
                            reason = 'wrong_floor_persistent'
                            waypoint = $name
                            expected_z = $target[2]
                            wrong_floor_guids = @($wrongFloor.guid)
                        }
                        throw "Party remained on the wrong stacked floor at $name."
                    }
                    # Do not accept a stacked-floor endpoint merely because X/Y is close.
                    # Reassert the same ordinary mmap movement order and fail closed if the
                    # requested upper component cannot be reached within ten polls.
                    $floorResponse = Invoke-AdvanceWaypoint -Name $name -Target $target
                    if (-not [bool]$floorResponse.ok -and [string]$floorResponse.error -ne 'advance_deferred_combat') {
                        throw "Upper-floor correction failed at ${name}: $($floorResponse.error)"
                    }
                    if ([bool]$floorResponse.ok) { ++$orderCount }
                    Start-Sleep -Seconds $PollSeconds
                    continue
                }
                $wrongFloorPolls = 0
            }
            $stragglers = @($bots | Where-Object {
                [uint32]$_.guid -ne $LeaderGuid -and (
                    (Get-Distance3d -Bot $_ -Target $leaderPosition) -gt $CohesionRadius -or
                    ($name -in $strictUpperFloorWaypoints -and (Get-Distance3d -Bot $_ -Target $target) -gt $CohesionRadius)
                )
            })
            if ($stragglers.Count) {
                $combatStragglers = @($stragglers | Where-Object { [bool]$_.combat })
                if ($combatStragglers.Count -and -not [bool]$leader.combat) {
                    [void](Invoke-TankPickup -Bots $bots -Waypoint $name)
                    Start-Sleep -Seconds $PollSeconds
                    continue
                }
                Write-Receipt -Event 'regroup_wait' -Fields @{
                    waypoint = $name
                    straggler_guids = @($stragglers.guid)
                    maximum_separation = [Math]::Round([double](($stragglers | ForEach-Object {
                        Get-Distance3d -Bot $_ -Target $leaderPosition
                    } | Measure-Object -Maximum).Maximum), 2)
                }
                # Reassert leader/follower strategies at the safe landmark. The server still
                # refuses this during genuine combat and clears only inert stale combat flags.
                $regroupResponse = Invoke-AdvanceWaypoint -Name $name -Target $target
                if ([bool]$regroupResponse.ok) { ++$orderCount }
                elseif ([string]$regroupResponse.error -ne 'advance_deferred_combat') {
                    throw "Regroup order failed at ${name}: $($regroupResponse.error)"
                }
                Start-Sleep -Seconds $PollSeconds
                continue
            }
            Write-Receipt -Event 'waypoint_arrived' -Fields @{ waypoint = $name; distance = [Math]::Round($distance, 2); position = $leader.position }
            break
        }

        if ($busy) {
            ++$combatPolls
            if (-not [bool]$leader.combat) {
                $pickupResponse = Invoke-TankPickup -Bots $bots -Waypoint $name
                if (-not $pickupResponse) {
                    $engageResponse = Invoke-ControlJson -Action engage -Guid $LeaderGuid
                    Write-Receipt -Event 'tank_pickup_requested' -Fields @{ waypoint = $name; response = $engageResponse }
                }
                Start-Sleep -Seconds $PollSeconds
                continue
            }
            # Ask the server to distinguish genuine combat from stale combat flags. Advance is
            # authoritative here: it clears only players with no victim and no attackers, and
            # returns advance_deferred_combat without moving anyone when real combat remains.
            $combatResponse = Invoke-AdvanceWaypoint -Name $name -Target $target
            if ([bool]$combatResponse.ok) {
                ++$orderCount
                Write-Receipt -Event 'advance_order' -Fields @{ waypoint = $name; distance = [Math]::Round($distance, 2); response = $combatResponse }
            }
            elseif ([string]$combatResponse.error -ne 'advance_deferred_combat') {
                throw "Advance combat-state check failed at ${name}: $($combatResponse.error)"
            }
            Start-Sleep -Seconds $PollSeconds
            continue
        }

        $response = Invoke-AdvanceWaypoint -Name $name -Target $target
        ++$orderCount
        Write-Receipt -Event 'advance_order' -Fields @{ waypoint = $name; distance = [Math]::Round($distance, 2); response = $response }
        if (-not [bool]$response.ok) {
            # Re-probing from the middle of an already accepted spline can temporarily report no
            # complete path across a door/turn even though the original grounded route is still
            # advancing. Preserve that live movement only while distance continues to fall; three
            # consecutive no-progress rejections still fail closed as a real path stall.
            if ([string]$response.error -eq 'advance_path_rejected' -and $acceptedAdvance) {
                if ([double]::IsPositiveInfinity($lastDistance) -or ($lastDistance - $distance) -ge 2.0) {
                    $stalledOrders = 0
                } else {
                    ++$stalledOrders
                }
                Write-Receipt -Event 'advance_probe_deferred' -Fields @{
                    waypoint = $name
                    distance = [Math]::Round($distance, 2)
                    previous_distance = if ([double]::IsPositiveInfinity($lastDistance)) { $null } else { [Math]::Round($lastDistance, 2) }
                    stalled_rejections = $stalledOrders
                    response = $response
                }
                if ($stalledOrders -ge 3) {
                    Write-Receipt -Event 'director_failed' -Fields @{ reason = 'path_probe_stall'; waypoint = $name; distance = $distance }
                    throw "Path probe stalled at $name."
                }
                $lastDistance = [Math]::Min($lastDistance, $distance)
                Start-Sleep -Seconds ([Math]::Max(5, $PollSeconds))
                continue
            }
            throw "Advance order failed at ${name}: $($response.error)"
        }
        $acceptedAdvance = $true

        if ([Math]::Abs($lastDistance - $distance) -lt 2.0 -and -not [bool]$response.started) { ++$stalledOrders }
        else { $stalledOrders = 0 }
        if ($stalledOrders -ge 3) {
            Write-Receipt -Event 'director_failed' -Fields @{ reason = 'path_stall'; waypoint = $name; distance = $distance }
            throw "Pathfinding stalled at $name."
        }
        $lastDistance = $distance
        Start-Sleep -Seconds ([Math]::Max(5, $PollSeconds))
    }

    if ((Get-Date) -ge $deadline) {
        if ($leaderHeld) { [void](Invoke-ControlJson -Action resume -Guid $LeaderGuid); $leaderHeld = $false }
        Write-Receipt -Event 'director_failed' -Fields @{ reason = 'deadline'; waypoint = $name }
        throw "Dungeon director deadline reached at $name."
    }
}
}
finally {
    if ($leaderHeld) {
        try { [void](Invoke-ControlJson -Action resume -Guid $LeaderGuid) } catch {}
        $leaderHeld = $false
    }
}

$result = [ordered]@{
    passed = $true
    waypoints_completed = $Waypoint.Count
    orders_issued = $orderCount
    combat_polls = $combatPolls
    alive = 5
    instance_id = $instances[0]
    receipt = $ReceiptPath
}
Write-Receipt -Event 'director_completed' -Fields $result
[pscustomobject]$result | ConvertTo-Json -Depth 6
