<#!
.SYNOPSIS
    Read-only terminal dashboard for persistent AutoWoW Playerbots.

.DESCRIPTION
    Snapshot mode takes one read-only sample. Watch mode repeats the same sample and never
    sends a mutating bridge action. The bridge host is deliberately fixed to loopback and the
    only delegated actions are list, combatlog, and questlog.
#>
[CmdletBinding()]
param(
    [ValidateSet('Snapshot', 'Watch')][string]$Mode = 'Snapshot',
    [string]$ProbeManifestPath = '',
    [uint32[]]$LeaderGuid = @(),
    [ValidateSet('127.0.0.1')][string]$BridgeHost = '127.0.0.1',
    [ValidateRange(1, 65535)][int]$Port = 18787,
    [ValidateRange(1000, 120000)][int]$TimeoutMs = 5000,
    [Alias('PollSeconds')][ValidateRange(1, 3600)][int]$RefreshSeconds = 5,
    [ValidateRange(0, 1000000)][int]$MaxRefreshes = 0,
    [switch]$AsJson,
    [string]$ControlScriptPath = (Join-Path $PSScriptRoot 'autowow-control.ps1')
)

$ErrorActionPreference = 'Stop'
$script:BotFleetReadActions = @('list', 'combatlog', 'questlog', 'questobjective')

function Get-BotFleetProperty {
    param(
        [AllowNull()][object]$Object,
        [Parameter(Mandatory)][string]$Name,
        [AllowNull()][object]$Default = $null
    )
    if ($null -eq $Object) { return $Default }
    if ($Object -is [System.Collections.IDictionary] -and $Object.Contains($Name)) {
        return $Object[$Name]
    }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -ne $property) { return $property.Value }
    return $Default
}

function ConvertFrom-BotFleetBridgeResponse {
    [CmdletBinding()]
    param([Parameter(Mandatory)][object[]]$Raw)

    $objects = @($Raw | Where-Object { $null -ne $_ -and $_ -isnot [string] })
    if ($objects.Count -eq 1 -and $null -ne $objects[0].PSObject.Properties['ok']) {
        return $objects[0]
    }

    $jsonLines = @($Raw | ForEach-Object { [string]$_ } | Where-Object {
        -not [string]::IsNullOrWhiteSpace($_) -and $_.TrimStart().StartsWith('{')
    })
    if ($jsonLines.Count -eq 0) { throw 'AutoWow bridge returned no JSON response.' }
    try { return ($jsonLines[-1] | ConvertFrom-Json) }
    catch { throw 'AutoWow bridge returned malformed JSON.' }
}

function Invoke-BotFleetRead {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][ValidateSet('list', 'combatlog', 'questlog', 'questobjective')][string]$Action,
        [Parameter(Mandatory)][scriptblock]$ControlInvoker,
        [uint32]$Guid = 0,
        [switch]$AllowError
    )
    if ($Action -notin $script:BotFleetReadActions) { throw "Dashboard action is not read-only: $Action" }
    try {
        $response = ConvertFrom-BotFleetBridgeResponse -Raw @(& $ControlInvoker $Action $Guid)
        $ok = Get-BotFleetProperty $response 'ok' $true
        if (-not [bool]$ok) {
            throw "Bridge $Action failed for GUID ${Guid}: $(Get-BotFleetProperty $response 'error' 'unknown_error')"
        }
        return $response
    }
    catch {
        if ($AllowError) {
            return [pscustomobject][ordered]@{
                ok = $false
                error = 'bridge_error'
                detail = $_.Exception.Message
                guid = $Guid
            }
        }
        throw
    }
}

function Read-BotFleetManifest {
    [CmdletBinding()]
    param([string]$Path = '')
    if ([string]::IsNullOrWhiteSpace($Path)) { return $null }
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Probe manifest was not found: $Path" }
    try { $manifest = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json }
    catch { throw "Probe manifest is not valid JSON: $Path" }
    $probes = @(Get-BotFleetProperty $manifest 'probes' @())
    if ($probes.Count -eq 0) { throw 'Probe manifest must contain at least one probe.' }
    foreach ($probe in $probes) {
        $members = @(Get-BotFleetProperty $probe 'members' @())
        if ($members.Count -eq 0) { throw "Probe manifest probe has no members: $(Get-BotFleetProperty $probe 'id' '<unknown>')" }
        $leader = [uint32](Get-BotFleetProperty $probe 'leader_guid' 0)
        if ($leader -eq 0) { throw "Probe manifest probe has no positive leader_guid: $(Get-BotFleetProperty $probe 'id' '<unknown>')" }
    }
    return $manifest
}

function New-BotFleetGroups {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][object[]]$Bots,
        [AllowNull()][psobject]$Manifest,
        [uint32[]]$LeaderGuid = @()
    )

    $leaderFilter = @($LeaderGuid | Where-Object { $_ -ne 0 } | Select-Object -Unique)
    $groups = [System.Collections.Generic.List[object]]::new()
    if ($null -ne $Manifest) {
        foreach ($probe in @(Get-BotFleetProperty $Manifest 'probes' @())) {
            $leader = [uint32](Get-BotFleetProperty $probe 'leader_guid' 0)
            $members = foreach ($member in @(Get-BotFleetProperty $probe 'members' @())) {
                [pscustomobject][ordered]@{
                    guid = [uint32](Get-BotFleetProperty $member 'guid' 0)
                    expected_name = [string](Get-BotFleetProperty $member 'name' '')
                }
            }
            $groups.Add([pscustomobject][ordered]@{
                key = "probe:$([string](Get-BotFleetProperty $probe 'id' $leader))"
                label = [string](Get-BotFleetProperty $probe 'id' $leader)
                kind = [string](Get-BotFleetProperty $probe 'kind' 'roster')
                leader_guid = $leader
                members = @($members)
            })
        }
        if ($leaderFilter.Count -eq 0) { return @($groups) }

        # A manifest is an exact-roster source of truth. Explicit leader GUIDs are additive in
        # manifest mode: retain every manifest probe, then append discovered groups for requested
        # leaders that the manifest did not already represent.
        $manifestLeaders = @($groups | ForEach-Object { [uint32]$_.leader_guid })
        foreach ($bot in $Bots) {
            $guid = [uint32](Get-BotFleetProperty $bot 'guid' 0)
            if ($guid -eq 0) { continue }
            $group = Get-BotFleetProperty $bot 'group' $null
            $leader = [uint32](Get-BotFleetProperty $group 'leader_guid' 0)
            if ($leader -eq 0) { $leader = $guid }
            if ($leader -notin $leaderFilter -or $leader -in $manifestLeaders) { continue }
            $key = "leader:$leader"
            $existing = @($groups | Where-Object { $_.key -eq $key }) | Select-Object -First 1
            if ($null -eq $existing) {
                $existing = [pscustomobject][ordered]@{
                    key = $key; label = "leader:$leader"; kind = 'group'; leader_guid = $leader
                    members = [System.Collections.Generic.List[object]]::new()
                }
                $groups.Add($existing)
            }
            $existing.members.Add([pscustomobject][ordered]@{ guid = $guid; expected_name = [string](Get-BotFleetProperty $bot 'name' '') })
        }
        return @($groups)
    }

    foreach ($bot in $Bots) {
        $guid = [uint32](Get-BotFleetProperty $bot 'guid' 0)
        if ($guid -eq 0) { continue }
        $group = Get-BotFleetProperty $bot 'group' $null
        $leader = [uint32](Get-BotFleetProperty $group 'leader_guid' 0)
        if ($leader -eq 0) { $leader = $guid }
        if ($leaderFilter.Count -gt 0 -and $leader -notin $leaderFilter) { continue }
        $key = "leader:$leader"
        $existing = @($groups | Where-Object { $_.key -eq $key }) | Select-Object -First 1
        if ($null -eq $existing) {
            $existing = [pscustomobject][ordered]@{
                key = $key; label = "leader:$leader"; kind = 'group'; leader_guid = $leader
                members = [System.Collections.Generic.List[object]]::new()
            }
            $groups.Add($existing)
        }
        $existing.members.Add([pscustomobject][ordered]@{ guid = $guid; expected_name = [string](Get-BotFleetProperty $bot 'name' '') })
    }
    return @($groups | Sort-Object leader_guid)
}

function Get-BotFleetNumber {
    param([AllowNull()][object]$Value, [double]$Default = 0)
    if ($null -eq $Value -or [string]::IsNullOrWhiteSpace([string]$Value)) { return $Default }
    try { return [double]$Value } catch { return $Default }
}

function Get-BotFleetQuestView {
    [CmdletBinding()]
    param(
        [AllowNull()][psobject]$Response,
        [AllowNull()][psobject]$ObjectiveResponse
    )
    $questLogOk = $null -ne $Response -and [bool](Get-BotFleetProperty $Response 'ok' $false)
    $objectiveOk = $null -ne $ObjectiveResponse -and [bool](Get-BotFleetProperty $ObjectiveResponse 'ok' $false)
    if (-not $questLogOk -and -not $objectiveOk) { return $null }

    $active = if ($questLogOk) { Get-BotFleetProperty $Response 'active_quest' $null } else { $null }
    if ($null -eq $active -and $questLogOk) { $active = Get-BotFleetProperty $Response 'current_quest' $null }
    if ($null -eq $active -and $questLogOk) {
        $quests = @(Get-BotFleetProperty $Response 'quests' @())
        $active = @($quests | Where-Object { -not [bool](Get-BotFleetProperty $_ 'is_complete' $false) } | Select-Object -First 1)
        if ($active.Count -eq 0) { $active = @($quests | Select-Object -First 1) }
        if ($active.Count -gt 0) { $active = $active[0] } else { $active = $null }
    }
    $objectiveView = Get-BotFleetProperty $ObjectiveResponse 'objective' $null
    $objectiveQuestId = [uint32](Get-BotFleetProperty $objectiveView 'quest_id' 0)
    $objectiveAvailable = $objectiveOk -and $objectiveQuestId -gt 0
    if ($null -eq $active -and -not $objectiveAvailable) { return $null }
    if ($null -eq $active) {
        $active = [pscustomobject][ordered]@{ id = $objectiveQuestId; title = ''; is_complete = $false }
    }

    $objectives = @(Get-BotFleetProperty $active 'objectives' @())
    $objective = @($objectives | Where-Object { -not [bool](Get-BotFleetProperty $_ 'done' $false) } | Select-Object -First 1)
    if ($objective.Count -eq 0) { $objective = @($objectives | Select-Object -First 1) }
    if ($objective.Count -gt 0) { $objective = $objective[0] } else { $objective = $null }
    $current = if ($objectiveAvailable) { Get-BotFleetProperty $objectiveView 'current_count' $null } else { Get-BotFleetProperty $objective 'current' $null }
    $required = if ($objectiveAvailable) { Get-BotFleetProperty $objectiveView 'required_count' $null } else { Get-BotFleetProperty $objective 'required' $null }
    $progress = if ($null -ne $current -and $null -ne $required) { "${current}/${required}" } else { '' }
    $phase = if ($objectiveAvailable) { Get-BotFleetProperty $objectiveView 'phase' $null } else { Get-BotFleetProperty $active 'phase' $null }
    if ($null -eq $phase -and $questLogOk) { $phase = Get-BotFleetProperty $Response 'phase' $null }
    if ($null -eq $phase -and $questLogOk) { $phase = Get-BotFleetProperty $Response 'active_phase' $null }
    $objectiveKind = if ($objectiveAvailable) { Get-BotFleetProperty $objectiveView 'objective_kind' (Get-BotFleetProperty $objective 'kind' '') } else { Get-BotFleetProperty $objective 'kind' '' }
    $objectiveEntry = if ($objectiveAvailable) {
        Get-BotFleetProperty $objectiveView 'required_npc_or_go_entry' (Get-BotFleetProperty $objectiveView 'required_item_id' $null)
    } else { Get-BotFleetProperty $objective 'entry' $null }

    return [pscustomobject][ordered]@{
        quest_id = [uint32](Get-BotFleetProperty $active 'id' (Get-BotFleetProperty $active 'quest_id' $objectiveQuestId))
        title = [string](Get-BotFleetProperty $active 'title' '')
        phase = if ($null -eq $phase) { '' } else { [string]$phase }
        objective_kind = [string]$objectiveKind
        objective_entry = $objectiveEntry
        objective_progress = $progress
        objective_done = if ($objectiveAvailable) { [bool](Get-BotFleetProperty $objectiveView 'current_count' 0) -ge [int](Get-BotFleetProperty $objectiveView 'required_count' 0) } elseif ($null -eq $objective) { $null } else { [bool](Get-BotFleetProperty $objective 'done' $false) }
        is_complete = [bool](Get-BotFleetProperty $active 'is_complete' $false)
    }
}

function Get-BotFleetMaxSeparation {
    [CmdletBinding()]
    param([Parameter(Mandatory)][object[]]$Members)
    $located = @($Members | Where-Object { $null -ne $_.x -and $null -ne $_.y -and $null -ne $_.z })
    if ($located.Count -lt 2) { return $null }
    $max = 0.0
    for ($i = 0; $i -lt $located.Count; $i++) {
        for ($j = $i + 1; $j -lt $located.Count; $j++) {
            $dx = [double]$located[$i].x - [double]$located[$j].x
            $dy = [double]$located[$i].y - [double]$located[$j].y
            $dz = [double]$located[$i].z - [double]$located[$j].z
            $distance = [Math]::Sqrt(($dx * $dx) + ($dy * $dy) + ($dz * $dz))
            if ($distance -gt $max) { $max = $distance }
        }
    }
    return [Math]::Round($max, 1)
}

function ConvertTo-BotFleetMemberState {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][psobject]$RosterMember,
        [AllowNull()][psobject]$Bot,
        [AllowNull()][psobject]$CombatResponse
    )
    $telemetry = Get-BotFleetProperty $CombatResponse 'telemetry' $null
    $location = Get-BotFleetProperty $telemetry 'location' $null
    $position = Get-BotFleetProperty $Bot 'position' $null
    $group = Get-BotFleetProperty $telemetry 'group' (Get-BotFleetProperty $Bot 'group' $null)
    $combatOk = $null -ne $CombatResponse -and [bool](Get-BotFleetProperty $CombatResponse 'ok' $false)
    $alive = if ($combatOk) { [bool](Get-BotFleetProperty $telemetry 'alive' $false) } else { [bool](Get-BotFleetProperty $Bot 'alive' $false) }
    $map = if ($null -ne $location) { [uint32](Get-BotFleetProperty $location 'map_id' 0) } else { [uint32](Get-BotFleetProperty $position 'map' (Get-BotFleetProperty $position 'map_id' 0)) }
    $instance = if ($null -ne $location) { [uint32](Get-BotFleetProperty $location 'instance_id' 0) } else { [uint32](Get-BotFleetProperty $position 'instance_id' 0) }
    [pscustomobject][ordered]@{
        guid = [uint32]$RosterMember.guid
        name = if ($null -ne $Bot) { [string](Get-BotFleetProperty $Bot 'name' $RosterMember.expected_name) } else { [string]$RosterMember.expected_name }
        online = ($null -ne $Bot)
        alive = $alive
        death_state = if ($combatOk) { [string](Get-BotFleetProperty $telemetry 'death_state' $(if ($alive) { 'alive' } else { 'unknown' })) } else { 'unknown' }
        in_combat = if ($combatOk) { [bool](Get-BotFleetProperty $telemetry 'in_combat' $false) } else { [bool](Get-BotFleetProperty $Bot 'combat' $false) }
        map_id = $map
        instance_id = $instance
        x = Get-BotFleetProperty $position 'x' $null
        y = Get-BotFleetProperty $position 'y' $null
        z = Get-BotFleetProperty $position 'z' $null
        group_members = [int](Get-BotFleetProperty $group 'members' 0)
        observed_leader_guid = [uint32](Get-BotFleetProperty $group 'leader_guid' 0)
        bridge_error = if ($null -ne $CombatResponse -and -not $combatOk) { [string](Get-BotFleetProperty $CombatResponse 'error' 'bridge_error') } else { $null }
    }
}

function Invoke-BotFleetDashboardSnapshot {
    [CmdletBinding()]
    param(
        [ValidateSet('Snapshot', 'Watch')][string]$Mode = 'Snapshot',
        [string]$ProbeManifestPath = '',
        [uint32[]]$LeaderGuid = @(),
        [Parameter(Mandatory)][scriptblock]$ControlInvoker,
        [datetime]$ObservedAtUtc = [datetime]::UtcNow
    )
    $manifest = Read-BotFleetManifest -Path $ProbeManifestPath
    $list = Invoke-BotFleetRead -Action list -ControlInvoker $ControlInvoker
    $bots = @(Get-BotFleetProperty $list 'bots' @())
    $botByGuid = @{}
    foreach ($bot in $bots) {
        $guid = [uint32](Get-BotFleetProperty $bot 'guid' 0)
        if ($guid -ne 0) { $botByGuid[[string]$guid] = $bot }
    }
    $groups = @(New-BotFleetGroups -Bots $bots -Manifest $manifest -LeaderGuid $LeaderGuid)
    $leaderFilter = @($LeaderGuid | Where-Object { $_ -ne 0 } | Select-Object -Unique)
    if ($groups.Count -eq 0) {
        return [pscustomobject][ordered]@{
            schema = 'autowow.bot-fleet-dashboard.v1'; mode = $Mode; observed_at_utc = $ObservedAtUtc.ToString('o')
            bridge_host = '127.0.0.1'; read_only = $true; bridge_actions = @('list', 'combatlog', 'questlog', 'questobjective')
            manifest_path = if ([string]::IsNullOrWhiteSpace($ProbeManifestPath)) { $null } else { (Resolve-Path -LiteralPath $ProbeManifestPath).Path }
            leader_guid_filter = if ($leaderFilter.Count -eq 0) { $null } else { @($leaderFilter) }; groups = @(); table = @()
        }
    }

    $questByLeader = @{}
    foreach ($group in $groups) {
        $leader = [uint32]$group.leader_guid
        if (-not $questByLeader.ContainsKey([string]$leader)) {
            $questLog = Invoke-BotFleetRead -Action questlog -Guid $leader -ControlInvoker $ControlInvoker -AllowError
            $questObjective = Invoke-BotFleetRead -Action questobjective -Guid $leader -ControlInvoker $ControlInvoker -AllowError
            $questByLeader[[string]$leader] = Get-BotFleetQuestView -Response $questLog -ObjectiveResponse $questObjective
        }
    }

    $groupViews = foreach ($group in $groups) {
        $memberStates = foreach ($rosterMember in @($group.members)) {
            $guidKey = [string][uint32]$rosterMember.guid
            $bot = if ($botByGuid.ContainsKey($guidKey)) { $botByGuid[$guidKey] } else { $null }
            $combat = Invoke-BotFleetRead -Action combatlog -Guid ([uint32]$rosterMember.guid) -ControlInvoker $ControlInvoker -AllowError
            ConvertTo-BotFleetMemberState -RosterMember $rosterMember -Bot $bot -CombatResponse $combat
        }
        $maps = @($memberStates | ForEach-Object { [uint32]$_.map_id } | Where-Object { $_ -ne 0 } | Sort-Object -Unique)
        $instances = @($memberStates | ForEach-Object { [uint32]$_.instance_id } | Sort-Object -Unique)
        $quest = $questByLeader[[string][uint32]$group.leader_guid]
        $groupView = [pscustomobject][ordered]@{
            key = $group.key; label = $group.label; kind = $group.kind; leader_guid = [uint32]$group.leader_guid
            roster_count = @($memberStates).Count
            alive_count = @($memberStates | Where-Object alive).Count
            dead_count = @($memberStates | Where-Object { -not $_.alive }).Count
            offline_count = @($memberStates | Where-Object { -not $_.online }).Count
            combat_count = @($memberStates | Where-Object in_combat).Count
            map_ids = @($maps); instance_ids = @($instances)
            max_separation = Get-BotFleetMaxSeparation -Members @($memberStates)
            quest = $quest
            members = @($memberStates)
        }
        $groupView
    }
    $table = foreach ($group in @($groupViews)) {
        [pscustomobject][ordered]@{
            Group = $group.label
            Leader = $group.leader_guid
            Roster = $group.roster_count
            Alive = $group.alive_count
            Dead = $group.dead_count
            Combat = $group.combat_count
            Maps = if ($group.map_ids.Count -eq 0) { '-' } else { $group.map_ids -join ',' }
            Instances = if ($group.instance_ids.Count -eq 0) { '-' } else { $group.instance_ids -join ',' }
            MaxSep = if ($null -eq $group.max_separation) { '-' } else { $group.max_separation }
            Quest = if ($null -eq $group.quest) { '-' } else { "id=$($group.quest.quest_id) $($group.quest.objective_progress) phase=$($group.quest.phase)" }
        }
    }
    [pscustomobject][ordered]@{
        schema = 'autowow.bot-fleet-dashboard.v1'; mode = $Mode; observed_at_utc = $ObservedAtUtc.ToString('o')
        bridge_host = '127.0.0.1'; read_only = $true; bridge_actions = @('list', 'combatlog', 'questlog', 'questobjective')
        manifest_path = if ([string]::IsNullOrWhiteSpace($ProbeManifestPath)) { $null } else { (Resolve-Path -LiteralPath $ProbeManifestPath).Path }
        leader_guid_filter = if ($leaderFilter.Count -eq 0) { $null } else { @($leaderFilter) }
        groups = @($groupViews); table = @($table)
    }
}

function Write-BotFleetDashboardText {
    [CmdletBinding()]
    param([Parameter(Mandatory)][psobject]$Snapshot)
    Write-Output ("AutoWoW fleet {0}  observed={1}  read-only" -f $Snapshot.mode, $Snapshot.observed_at_utc)
    if (@($Snapshot.table).Count -eq 0) {
        Write-Output 'No matching online roster/group was found.'
        return
    }
    Write-Output ((@($Snapshot.table) | Format-Table -AutoSize | Out-String -Width 240).TrimEnd())
    Write-Output ''
    Write-Output 'Alive/dead/combat are member counts; Maps and Instances are distinct observed values; MaxSep is 3D yards when positions are available.'
}

function Invoke-BotFleetDashboard {
    [CmdletBinding()]
    param(
        [ValidateSet('Snapshot', 'Watch')][string]$Mode = 'Snapshot',
        [string]$ProbeManifestPath = '', [uint32[]]$LeaderGuid = @(),
        [ValidateSet('127.0.0.1')][string]$BridgeHost = '127.0.0.1',
        [ValidateRange(1, 65535)][int]$Port = 18787, [ValidateRange(1000, 120000)][int]$TimeoutMs = 5000,
        [Alias('PollSeconds')][ValidateRange(1, 3600)][int]$RefreshSeconds = 5, [ValidateRange(0, 1000000)][int]$MaxRefreshes = 0,
        [switch]$AsJson, [string]$ControlScriptPath = (Join-Path $PSScriptRoot 'autowow-control.ps1')
    )
    if (-not (Test-Path -LiteralPath $ControlScriptPath -PathType Leaf)) { throw "AutoWow control script was not found: $ControlScriptPath" }
    $controlInvoker = {
        param([string]$Action, [uint32]$Guid)
        $parameters = @{ Action = $Action; BridgeHost = $BridgeHost; Port = $Port; TimeoutMs = $TimeoutMs }
        if ($Guid -ne 0) { $parameters.BotGuid = $Guid }
        & $ControlScriptPath @parameters
    }.GetNewClosure()

    $count = 0
    do {
        $snapshot = Invoke-BotFleetDashboardSnapshot -Mode $Mode -ProbeManifestPath $ProbeManifestPath -LeaderGuid $LeaderGuid -ControlInvoker $controlInvoker
        if ($AsJson) {
            Write-Output ($snapshot | ConvertTo-Json -Depth 30 -Compress)
        } else {
            if ($Mode -eq 'Watch') { Clear-Host }
            Write-BotFleetDashboardText -Snapshot $snapshot
        }
        $count++
        if ($Mode -eq 'Snapshot' -or ($MaxRefreshes -gt 0 -and $count -ge $MaxRefreshes)) { break }
        Start-Sleep -Seconds $RefreshSeconds
    } while ($true)
}

if ($MyInvocation.InvocationName -ne '.') {
    Invoke-BotFleetDashboard -Mode $Mode -ProbeManifestPath $ProbeManifestPath -LeaderGuid $LeaderGuid `
        -BridgeHost $BridgeHost -Port $Port -TimeoutMs $TimeoutMs -RefreshSeconds $RefreshSeconds `
        -MaxRefreshes $MaxRefreshes -AsJson:$AsJson -ControlScriptPath $ControlScriptPath
}
