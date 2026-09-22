Set-StrictMode -Version Latest

$script:ProbeLabManifestSchema = 'autowow.probe-lab.manifest.v1'
$script:ProbeLabReceiptSchema = 'autowow.probe-lab.receipt.v1'
$script:ProbeLabSummarySchema = 'autowow.probe-lab.summary.v1'
$script:ProbeLabAllowedSizes = @(5, 10, 25, 40)
$script:ProbeLabNavigatorBlockedReasons = @(
    'already_complete', 'no_reachable_waypoint', 'party_cohesion', 'route_exhausted',
    'unreachable', 'unsafe_waypoint', 'unsupported_transition'
)

function Get-ProbeLabProperty {
    [CmdletBinding()]
    param(
        [AllowNull()][object]$Object,
        [Parameter(Mandatory)][string]$Name,
        [AllowNull()][object]$Default = $null
    )

    if ($null -eq $Object) { return $Default }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $Default }
    return $property.Value
}

function Assert-ProbeLabNoSecrets {
    [CmdletBinding()]
    param([Parameter(Mandatory)][object]$Object, [string]$Path = 'manifest')

    if ($Object -is [string] -or $Object -is [ValueType]) { return }
    if ($Object -is [System.Collections.IDictionary]) {
        foreach ($key in $Object.Keys) {
            if ([string]$key -match '(?i)(password|passwd|secret|credential|token)') {
                throw "Secret-bearing property '$Path.$key' is forbidden. Probe Lab never stores credentials."
            }
            Assert-ProbeLabNoSecrets -Object $Object[$key] -Path "$Path.$key"
        }
        return
    }
    if ($Object -is [System.Collections.IEnumerable]) {
        $index = 0
        foreach ($item in $Object) {
            Assert-ProbeLabNoSecrets -Object $item -Path "$Path[$index]"
            ++$index
        }
        return
    }
    foreach ($property in $Object.PSObject.Properties) {
        if ($property.Name -match '(?i)(password|passwd|secret|credential|token)') {
            throw "Secret-bearing property '$Path.$($property.Name)' is forbidden. Probe Lab never stores credentials."
        }
        if ($null -ne $property.Value) {
            Assert-ProbeLabNoSecrets -Object $property.Value -Path "$Path.$($property.Name)"
        }
    }
}

function Assert-ProbeLabName {
    param([Parameter(Mandatory)][string]$Value, [Parameter(Mandatory)][string]$Field)
    if ($Value -notmatch '^[A-Za-z][A-Za-z0-9._-]{1,63}$') {
        throw "$Field '$Value' must be a 2-64 character named token without whitespace or command characters."
    }
}

function Read-ProbeLabManifest {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Probe manifest does not exist: $Path" }
    try { $manifest = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json } catch {
        throw "Probe manifest is not valid JSON: $($_.Exception.Message)"
    }
    Assert-ProbeLabManifest -Manifest $manifest | Out-Null
    return $manifest
}

function Assert-ProbeLabManifest {
    [CmdletBinding()]
    param([Parameter(Mandatory)][psobject]$Manifest)

    Assert-ProbeLabNoSecrets -Object $Manifest
    if ((Get-ProbeLabProperty $Manifest schema '') -ne $script:ProbeLabManifestSchema) {
        throw "Manifest schema must be '$script:ProbeLabManifestSchema'."
    }
    $labId = [string](Get-ProbeLabProperty $Manifest lab_id '')
    Assert-ProbeLabName -Value $labId -Field 'lab_id'
    $probes = @(Get-ProbeLabProperty $Manifest probes @())
    if ($probes.Count -eq 0) { throw 'Manifest must contain at least one probe.' }

    $allGuids = [System.Collections.Generic.HashSet[uint32]]::new()
    $leaders = [System.Collections.Generic.HashSet[uint32]]::new()
    $probeIds = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($probe in $probes) {
        $id = [string](Get-ProbeLabProperty $probe id '')
        Assert-ProbeLabName -Value $id -Field 'probe.id'
        if (-not $probeIds.Add($id)) { throw "Duplicate probe id: $id" }
        $kind = [string](Get-ProbeLabProperty $probe kind '')
        if ($kind -notin @('party', 'raid')) { throw "Probe '$id' kind must be party or raid." }
        $size = [int](Get-ProbeLabProperty $probe size 0)
        if ($kind -eq 'party' -and $size -ne 5) { throw "Party probe '$id' must have size 5." }
        if ($kind -eq 'raid' -and $size -notin @(10, 25, 40)) { throw "Raid probe '$id' must have size 10, 25, or 40." }
        $members = @(Get-ProbeLabProperty $probe members @())
        if ($members.Count -ne $size) { throw "Probe '$id' declares size $size but has $($members.Count) members." }
        $leader = [uint32](Get-ProbeLabProperty $probe leader_guid 0)
        if ($leader -eq 0) { throw "Probe '$id' leader_guid must be positive." }
        if (-not $leaders.Add($leader)) { throw "Leader GUID $leader is reused across probes." }

        $memberGuids = [System.Collections.Generic.HashSet[uint32]]::new()
        foreach ($member in $members) {
            $guid = [uint32](Get-ProbeLabProperty $member guid 0)
            if ($guid -eq 0) { throw "Probe '$id' contains a zero member GUID." }
            if (-not $memberGuids.Add($guid)) { throw "Probe '$id' repeats member GUID $guid." }
            if (-not $allGuids.Add($guid)) { throw "Member GUID $guid is reused across probes; all GUIDs must be globally unique." }
            $level = [int](Get-ProbeLabProperty $member level 0)
            $specIndex = [int](Get-ProbeLabProperty $member spec_index -1)
            $quality = [int](Get-ProbeLabProperty $member quality -1)
            if ($level -lt 1 -or $level -gt 80) { throw "Member GUID $guid level must be 1-80." }
            if ($specIndex -lt 0 -or $specIndex -gt 19) { throw "Member GUID $guid spec_index must be 0-19." }
            if ($quality -lt 0 -or $quality -gt 5) { throw "Member GUID $guid quality must be 0-5." }
            $name = [string](Get-ProbeLabProperty $member name '')
            if (-not [string]::IsNullOrWhiteSpace($name) -and $name -notmatch '^[A-Za-z][A-Za-z0-9]{1,31}$') {
                throw "Member GUID $guid name is not a safe character token."
            }
        }
        if (-not $memberGuids.Contains($leader)) { throw "Probe '$id' leader GUID $leader is not in its member roster." }
        $expectedMapId = [uint32](Get-ProbeLabProperty $probe expected_map_id 0)
        if ($expectedMapId -eq 0) { throw "Probe '$id' expected_map_id must be positive." }
        Assert-ProbeLabName -Value ([string](Get-ProbeLabProperty $probe exterior_route '')) -Field "probe '$id' exterior_route"
        if ([string](Get-ProbeLabProperty $probe exterior_route '') -match '(?i)(interior|inside|teleport)') {
            throw "Probe '$id' exterior_route must name exterior staging and may not describe an interior or teleport route."
        }
        $waypoint = [string](Get-ProbeLabProperty $probe advance_waypoint '')
        if (-not [string]::IsNullOrWhiteSpace($waypoint)) {
            Assert-ProbeLabName -Value $waypoint -Field "probe '$id' advance_waypoint"
        }
        $difficulty = [int](Get-ProbeLabProperty $probe raid_difficulty 0)
        if ($difficulty -notin @(0, 1)) { throw "Probe '$id' raid_difficulty must be 0 or 1." }
        $failurePolicy = Get-ProbeLabProperty $probe failure_policy $null
        if ($null -ne $failurePolicy) {
            $failOnAnyDeath = Get-ProbeLabProperty $failurePolicy fail_on_any_death $false
            if ($failOnAnyDeath -isnot [bool]) { throw "Probe '$id' failure_policy.fail_on_any_death must be boolean." }
        }
    }
    return $true
}

function New-ProbeLabPlan {
    [CmdletBinding()]
    param([Parameter(Mandatory)][psobject]$Manifest, [string]$ManifestPath = '')

    Assert-ProbeLabManifest -Manifest $Manifest | Out-Null
    $operations = [System.Collections.Generic.List[object]]::new()
    $sequence = 1
    foreach ($probe in @($Manifest.probes)) {
        foreach ($member in @($probe.members)) {
            $operations.Add([pscustomobject][ordered]@{ sequence = $sequence++; probe_id = $probe.id; action = 'activate'; guid = [uint32]$member.guid; mutation = $true })
        }
    }
    $operations.Add([pscustomobject][ordered]@{ sequence = $sequence++; probe_id = '*'; action = 'wait-online'; mutation = $false; exact_guids = @($Manifest.probes.members.guid) })
    foreach ($probe in @($Manifest.probes)) {
        $operations.Add([pscustomobject][ordered]@{ sequence = $sequence++; probe_id = $probe.id; action = 'probe-reset-until-ready'; leader_guid = [uint32]$probe.leader_guid; member_guids = @($probe.members | Where-Object { [uint32]$_.guid -ne [uint32]$probe.leader_guid } | ForEach-Object { [uint32]$_.guid }); destination = [string]$probe.exterior_route; expected_map_id = [uint32]$probe.expected_map_id; expected_difficulty = [uint32](Get-ProbeLabProperty $probe raid_difficulty 0); mutation = $true; boundary = 'exact roster and allowlisted exterior route only' })
        foreach ($member in @($probe.members)) {
            $operations.Add([pscustomobject][ordered]@{ sequence = $sequence++; probe_id = $probe.id; action = 'fixture-init'; guid = [uint32]$member.guid; level = [int]$member.level; spec_index = [int]$member.spec_index; quality = [int]$member.quality; mutation = $true })
        }
        $operations.Add([pscustomobject][ordered]@{ sequence = $sequence++; probe_id = $probe.id; action = if ($probe.kind -eq 'party') { 'party' } else { 'raid-create' }; leader_guid = [uint32]$probe.leader_guid; member_guids = @($probe.members | Where-Object { [uint32]$_.guid -ne [uint32]$probe.leader_guid } | ForEach-Object { [uint32]$_.guid }); mutation = $true })
        $operations.Add([pscustomobject][ordered]@{ sequence = $sequence++; probe_id = $probe.id; action = 'route'; leader_guid = [uint32]$probe.leader_guid; destination = [string]$probe.exterior_route; expected_map_id = [uint32]$probe.expected_map_id; mutation = $true; boundary = 'named exterior route only' })
        $waypoint = [string](Get-ProbeLabProperty $probe advance_waypoint '')
        if (-not [string]::IsNullOrWhiteSpace($waypoint)) {
            $operations.Add([pscustomobject][ordered]@{ sequence = $sequence++; probe_id = $probe.id; action = 'advance-repeat-until-admitted'; leader_guid = [uint32]$probe.leader_guid; destination = $waypoint; mutation = $true; boundary = 'ordinary movement through authoritative admission; never teleport' })
        }
        $operations.Add([pscustomobject][ordered]@{ sequence = $sequence++; probe_id = $probe.id; action = 'deploy'; leader_guid = [uint32]$probe.leader_guid; mutation = $true; boundary = 'standard grouped follow and combat profile' })
    }
    $operations.Add([pscustomobject][ordered]@{ sequence = $sequence; probe_id = '*'; action = 'monitor-list-and-combatlog'; mutation = $false })

    return [pscustomobject][ordered]@{
        schema = 'autowow.probe-lab.plan.v1'
        schema_version = 1
        lab_id = $Manifest.lab_id
        manifest_path = $ManifestPath
        mode = 'Plan'
        dry_run = $true
        local_only = $true
        bridge_host = '127.0.0.1'
        launch_failure_scope = 'probe'
        operations = @($operations)
        safety = [ordered]@{
            apply_required_for_mutations = $true
            passwords_stored = $false
            database_access = $false
            server_restart = $false
            interior_teleport = $false
            allowed_bridge_actions = @('list', 'activate', 'probe-reset', 'fixture-init', 'party', 'raid-create', 'route', 'advance', 'deploy', 'combatlog')
        }
    }
}

function New-ProbeLabLaunchFailure {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$ProbeId,
        [Parameter(Mandatory)][string]$Operation,
        [Parameter(Mandatory)][string]$Detail,
        [string]$Code = '',
        [AllowNull()][Nullable[uint32]]$Guid = $null
    )

    if ([string]::IsNullOrWhiteSpace($Code)) {
        $Code = if ($Detail -match '^Bridge action ') { 'bridge_error' } else { 'orchestration_error' }
    }
    return [pscustomobject][ordered]@{
        probe_id = $ProbeId
        code = $Code
        operation = $Operation
        detail = $Detail
        guid = $Guid
        observed_at_utc = [datetime]::UtcNow.ToString('o')
    }
}

function ConvertFrom-ProbeLabControlJson {
    [CmdletBinding()]
    param([Parameter(Mandatory)][object[]]$Raw, [Parameter(Mandatory)][string]$Action, [uint32]$Guid = 0, [switch]$AllowRefusal)
    $lines = @($Raw | ForEach-Object { [string]$_ } | Where-Object { $_ -match '^\s*\{' })
    if ($lines.Count -eq 0) { throw "Bridge action $Action returned no JSON response." }
    try { $response = $lines[-1] | ConvertFrom-Json } catch { throw "Bridge action $Action returned malformed JSON." }
    $ok = Get-ProbeLabProperty $response ok $null
    if ($null -ne $ok -and -not [bool]$ok -and -not $AllowRefusal) {
        throw "Bridge action $Action failed for GUID ${Guid}: $(Get-ProbeLabProperty $response error 'unknown_error')"
    }
    return $response
}

function Get-ProbeLabResetDisposition {
    [CmdletBinding()]
    param([Parameter(Mandatory)][psobject]$Response)

    $status = [string](Get-ProbeLabProperty $Response status '')
    $code = [string](Get-ProbeLabProperty $Response code '')
    if ($status -notin @('READY', 'PENDING', 'REFUSED') -or [string]::IsNullOrWhiteSpace($code)) {
        throw 'probe-reset returned an invalid status envelope.'
    }
    if ($status -eq 'REFUSED' -and [bool](Get-ProbeLabProperty $Response ok $true)) {
        throw 'probe-reset REFUSED must set ok=false.'
    }
    if ($status -ne 'REFUSED' -and -not [bool](Get-ProbeLabProperty $Response ok $false)) {
        throw "probe-reset $status must set ok=true."
    }
    return [pscustomobject][ordered]@{ status = $status; code = $code; terminal = $status -ne 'PENDING' }
}

function Wait-ProbeLabReset {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][scriptblock]$Invoke,
        [ValidateRange(1, 600)][int]$TimeoutSeconds = 90,
        [ValidateRange(1, 60)][int]$PollSeconds = 2,
        [scriptblock]$Sleep = { param([int]$Seconds) Start-Sleep -Seconds $Seconds }
    )

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    do {
        $response = & $Invoke
        $disposition = Get-ProbeLabResetDisposition -Response $response
        if ($disposition.status -eq 'REFUSED') {
            throw "probe-reset refused: $($disposition.code)"
        }
        if ($disposition.status -eq 'READY') { return $response }
        & $Sleep $PollSeconds
    } while ((Get-Date) -lt $deadline)
    throw "probe-reset timed out while PENDING: $($disposition.code)"
}

function Get-ProbeLabBotByGuid {
    param([Parameter(Mandatory)][psobject]$ListResponse)
    $result = @{}
    foreach ($bot in @(Get-ProbeLabProperty $ListResponse bots @())) { $result[[string][uint32]$bot.guid] = $bot }
    return $result
}

function ConvertTo-ProbeLabSample {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][psobject]$Manifest,
        [Parameter(Mandatory)][psobject]$ListResponse,
        [Parameter(Mandatory)][System.Collections.IDictionary]$CombatByGuid,
        [datetime]$ObservedAtUtc = [datetime]::UtcNow
    )

    $bots = Get-ProbeLabBotByGuid -ListResponse $ListResponse
    $probeSamples = foreach ($probe in @($Manifest.probes)) {
        $members = foreach ($expected in @($probe.members)) {
            $key = [string][uint32]$expected.guid
            $bot = if ($bots.ContainsKey($key)) { $bots[$key] } else { $null }
            $combatEnvelope = if ($CombatByGuid.Contains($key)) { $CombatByGuid[$key] } else { $null }
            $combatOk = $null -ne $combatEnvelope -and [bool](Get-ProbeLabProperty $combatEnvelope ok $true)
            $combat = if ($combatOk) { Get-ProbeLabProperty $combatEnvelope telemetry $combatEnvelope } else { $null }
            $location = if ($combatOk) { Get-ProbeLabProperty $combat location $null } else { $null }
            $position = if ($null -ne $bot) { Get-ProbeLabProperty $bot position $null } else { $null }
            $group = if ($combatOk) { Get-ProbeLabProperty $combat group $null } elseif ($null -ne $bot) { Get-ProbeLabProperty $bot group $null } else { $null }
            [pscustomobject][ordered]@{
                guid = [uint32]$expected.guid
                name = if ($null -ne $bot) { [string](Get-ProbeLabProperty $bot name (Get-ProbeLabProperty $expected name '')) } else { [string](Get-ProbeLabProperty $expected name '') }
                online = ($null -ne $bot)
                combat_telemetry = $combatOk
                bridge_error = if ($null -ne $combatEnvelope -and -not $combatOk) { [string](Get-ProbeLabProperty $combatEnvelope error 'bridge_error') } else { $null }
                alive = if ($combatOk) { [bool](Get-ProbeLabProperty $combat alive $false) } elseif ($null -ne $bot) { [bool](Get-ProbeLabProperty $bot alive $false) } else { $false }
                death_state = if ($combatOk) {
                    $defaultDeathState = if ([bool](Get-ProbeLabProperty $combat alive $false)) { 'alive' } else { 'dead' }
                    [string](Get-ProbeLabProperty $combat death_state $defaultDeathState)
                } elseif ($null -ne $bot -and [bool](Get-ProbeLabProperty $bot alive $false)) { 'alive' } else { 'unknown' }
                in_combat = if ($combatOk) { [bool](Get-ProbeLabProperty $combat in_combat $false) } elseif ($null -ne $bot) { [bool](Get-ProbeLabProperty $bot combat $false) } else { $false }
                map_id = if ($null -ne $location) { [uint32](Get-ProbeLabProperty $location map_id 0) } elseif ($null -ne $position) { [uint32](Get-ProbeLabProperty $position map 0) } else { 0 }
                instance_id = if ($null -ne $location) { [uint32](Get-ProbeLabProperty $location instance_id 0) } else { 0 }
                x = if ($null -ne $position) { [double](Get-ProbeLabProperty $position x 0) } else { $null }
                y = if ($null -ne $position) { [double](Get-ProbeLabProperty $position y 0) } else { $null }
                z = if ($null -ne $position) { [double](Get-ProbeLabProperty $position z 0) } else { $null }
                group_members = if ($null -ne $group) { [int](Get-ProbeLabProperty $group members 0) } else { 0 }
                leader_guid = if ($null -ne $group) { [uint32](Get-ProbeLabProperty $group leader_guid 0) } else { 0 }
            }
        }
        foreach ($member in @($members)) {
            $member | Add-Member -NotePropertyName released_corpse -NotePropertyValue (
                -not [bool]$member.alive -and [string]$member.death_state -in @('corpse', 'ghost', 'released') -and
                ([uint32]$member.map_id -ne [uint32]$probe.expected_map_id -or [uint32]$member.instance_id -eq 0)
            )
        }
        $failurePolicy = Get-ProbeLabProperty $probe failure_policy $null
        [pscustomobject][ordered]@{
            probe_id = [string]$probe.id; kind = [string]$probe.kind; size = [int]$probe.size
            leader_guid = [uint32]$probe.leader_guid; expected_map_id = [uint32]$probe.expected_map_id
            fail_on_any_death = [bool](Get-ProbeLabProperty $failurePolicy fail_on_any_death $false)
            members = @($members)
        }
    }
    return [pscustomobject][ordered]@{ observed_at_utc = $ObservedAtUtc.ToString('o'); probes = @($probeSamples) }
}

function Get-ProbeLabDistance3d {
    param([Parameter(Mandatory)][psobject]$A, [Parameter(Mandatory)][psobject]$B)
    if ($null -eq $A.x -or $null -eq $B.x) { return [double]::PositiveInfinity }
    return [Math]::Sqrt([Math]::Pow([double]$A.x - [double]$B.x, 2) + [Math]::Pow([double]$A.y - [double]$B.y, 2) + [Math]::Pow([double]$A.z - [double]$B.z, 2))
}

function Test-ProbeLabAdmission {
    [CmdletBinding()]
    param([Parameter(Mandatory)][psobject]$ProbeSample)
    $members = @($ProbeSample.members)
    if (@($members | Where-Object { -not $_.online }).Count -gt 0) { return $false }
    if (@($members | Where-Object { [uint32]$_.map_id -ne [uint32]$ProbeSample.expected_map_id }).Count -gt 0) { return $false }
    $instances = @($members.instance_id | Where-Object { [uint32]$_ -ne 0 } | Sort-Object -Unique)
    return $instances.Count -eq 1 -and @($members | Where-Object { [uint32]$_.instance_id -eq 0 }).Count -eq 0
}

function Get-ProbeLabAdmissionState {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][psobject]$ProbeSample,
        [AllowEmptyString()][string]$AdvanceWaypoint = '',
        [bool]$PreviouslyConverged = $false,
        [Parameter(Mandatory)][datetime]$StartedAt,
        [datetime]$Now = [datetime]::UtcNow,
        [ValidateRange(1, 1800)][int]$TimeoutSeconds = 180
    )

    $admittedNow = Test-ProbeLabAdmission -ProbeSample $ProbeSample
    # Admission is a historical gate. Once the exact roster has converged in the instance,
    # later deaths move released ghosts to a graveyard map and must be classified as attrition,
    # not retroactively as an admission failure.
    $converged = $PreviouslyConverged -or $admittedNow
    $timedOut = -not [string]::IsNullOrWhiteSpace($AdvanceWaypoint) -and
        -not $converged -and
        ($Now.ToUniversalTime() - $StartedAt.ToUniversalTime()).TotalSeconds -ge $TimeoutSeconds

    return [pscustomobject][ordered]@{
        admitted_now = $admittedNow
        converged = $converged
        timed_out = $timedOut
    }
}

function Get-ProbeLabNavigatorEvents {
    [CmdletBinding()]
    param([string[]]$Lines, [string[]]$MemberNames = @())
    $events = [System.Collections.Generic.List[object]]::new()
    foreach ($line in @($Lines)) {
        if ($line -notmatch '\[DungeonNavigator\]\s+bot=([^\s]+).*?\bblocked=([^\s]+)') { continue }
        $botName = $Matches[1]
        $reason = $Matches[2].TrimEnd('.', ',', ';')
        if ($MemberNames.Count -gt 0 -and $botName -notin $MemberNames) { continue }
        $events.Add([pscustomobject][ordered]@{
            bot = $botName
            reason = $reason
            known_reason = ($reason -in $script:ProbeLabNavigatorBlockedReasons)
            line = $line
        })
    }
    return @($events)
}

function Read-ProbeLabLogDelta {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][long]$Offset)
    $stream = [System.IO.FileStream]::new($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    try {
        if ($Offset -lt 0 -or $Offset -gt $stream.Length) { $Offset = 0 }
        $null = $stream.Seek($Offset, [System.IO.SeekOrigin]::Begin)
        $reader = [System.IO.StreamReader]::new($stream, [System.Text.UTF8Encoding]::new($false), $true, 4096, $true)
        try { $text = $reader.ReadToEnd() } finally { $reader.Dispose() }
        return [pscustomobject][ordered]@{ next_offset = $stream.Position; lines = @($text -split '\r?\n' | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }) }
    } finally { $stream.Dispose() }
}

function Get-ProbeLabFirstFailure {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][psobject]$ProbeSample,
        [psobject]$PreviousProbeSample,
        [object[]]$NavigatorEvents = @(),
        [double]$CohesionRadius = 60,
        [double]$ProgressEpsilon = 1,
        [uint32[]]$KnownDeadGuid = @(),
        [switch]$AdmissionTimedOut,
        [switch]$ProgressTimedOut
    )
    $members = @($ProbeSample.members)
    $offline = @($members | Where-Object { -not $_.online -and [uint32]$_.guid -notin $KnownDeadGuid })
    if ($offline.Count -gt 0) { return [pscustomobject]@{ code = 'offline'; detail = "offline_guids=$($offline.guid -join ',')" } }
    $bridgeErrors = @($members | Where-Object { $null -ne $_.bridge_error -and [uint32]$_.guid -notin $KnownDeadGuid })
    if ($bridgeErrors.Count -gt 0) { return [pscustomobject]@{ code = 'bridge_error'; detail = "guids=$($bridgeErrors.guid -join ',')" } }
    $alive = @($members | Where-Object { $_.alive })
    if ($alive.Count -eq 0) { return [pscustomobject]@{ code = if ($ProbeSample.kind -eq 'party') { 'dead_party' } else { 'wipe' }; detail = 'all exact members are dead' } }
    $dead = @($members | Where-Object { -not $_.alive })
    if ($dead.Count -gt 0 -and [bool]$ProbeSample.fail_on_any_death) {
        return [pscustomobject]@{ code = 'member_death'; detail = "dead_guids=$($dead.guid -join ',');death_states=$($dead.death_state -join ',')" }
    }
    $activeMembers = @($members | Where-Object { $_.alive })
    $instances = @($activeMembers.instance_id | Where-Object { [uint32]$_ -ne 0 } | Sort-Object -Unique)
    $maps = @($activeMembers | Where-Object { [uint32]$_.instance_id -ne 0 } | ForEach-Object { [uint32]$_.map_id } | Where-Object { $_ -ne 0 } | Sort-Object -Unique)
    if ($instances.Count -gt 1 -or $maps.Count -gt 1) {
        $released = @($dead | Where-Object released_corpse | ForEach-Object { [uint32]$_.guid })
        return [pscustomobject]@{ code = 'split_instance'; detail = "active_map_ids=$($maps -join ',');active_instance_ids=$($instances -join ',');released_corpse_guids=$($released -join ',')" }
    }
    $hardBlocked = @($NavigatorEvents | Where-Object { $_.reason -ne 'party_cohesion' } | Select-Object -First 1)
    if ($hardBlocked.Count -gt 0) {
        return [pscustomobject]@{ code = 'navigator_blocked'; detail = "DungeonNavigator blocked=$($hardBlocked[0].reason) bot=$($hardBlocked[0].bot)"; navigator_reason = $hardBlocked[0].reason }
    }
    $cohesionBlocked = @($NavigatorEvents | Where-Object { $_.reason -eq 'party_cohesion' } | Select-Object -First 1)
    if ($ProgressTimedOut -and $cohesionBlocked.Count -gt 0) {
        return [pscustomobject]@{ code = 'cohesion_stall'; detail = "persistent DungeonNavigator blocked=party_cohesion bot=$($cohesionBlocked[0].bot)"; navigator_reason = 'party_cohesion' }
    }
    $leader = @($members | Where-Object { [uint32]$_.guid -eq [uint32]$ProbeSample.leader_guid })[0]
    $maxSeparation = 0.0
    foreach ($member in $members) { $maxSeparation = [Math]::Max($maxSeparation, (Get-ProbeLabDistance3d -A $leader -B $member)) }
    if ($ProgressTimedOut -and $maxSeparation -gt $CohesionRadius) { return [pscustomobject]@{ code = 'cohesion_stall'; detail = "maximum_separation=$([Math]::Round($maxSeparation, 2))" } }
    if ($AdmissionTimedOut) { return [pscustomobject]@{ code = 'admission_timeout'; detail = 'serial admission did not converge before its deadline' } }
    if ($ProgressTimedOut -and $null -ne $PreviousProbeSample) {
        $previousLeader = @($PreviousProbeSample.members | Where-Object { [uint32]$_.guid -eq [uint32]$ProbeSample.leader_guid })[0]
        $movement = Get-ProbeLabDistance3d -A $leader -B $previousLeader
        if ($movement -lt $ProgressEpsilon -and @($members | Where-Object { $_.in_combat }).Count -eq 0) {
            return [pscustomobject]@{ code = 'no_progress'; detail = "leader_movement=$([Math]::Round($movement, 2))" }
        }
    }
    return $null
}

function Get-ProbeLabCurrentHealth {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][psobject]$ProbeSample,
        [AllowNull()][psobject]$FirstFailure = $null
    )

    $members = @($ProbeSample.members)
    $aliveCount = @($members | Where-Object { [bool]$_.alive }).Count
    $deadMembers = @($members | Where-Object { -not [bool]$_.alive })
    $deadCount = $deadMembers.Count
    $releasedCount = @($deadMembers | Where-Object { [bool](Get-ProbeLabProperty $_ released_corpse $false) }).Count
    $counts = [ordered]@{
        members = $members.Count
        alive = $aliveCount
        dead = $deadCount
        released = $releasedCount
    }
    $countSummary = "$deadCount dead / $aliveCount alive"
    if ($releasedCount -gt 0) { $countSummary += "; $releasedCount released" }

    $status = if ($null -ne $FirstFailure) {
        'FAIL'
    } elseif ($deadCount -eq 0) {
        'PASS'
    } elseif ($deadCount -ge $aliveCount) {
        'FAIL'
    } else {
        'DEGRADED'
    }
    $reason = if ($null -ne $FirstFailure) {
        "first_failure:$([string](Get-ProbeLabProperty $FirstFailure code 'unknown'))"
    } elseif ($deadCount -eq 0) {
        'all_members_alive'
    } elseif ($deadCount -ge $aliveCount) {
        'lost_combat_capacity'
    } else {
        'minority_member_deaths'
    }

    return [pscustomobject][ordered]@{
        status = $status
        reason = $reason
        count_summary = $countSummary
        counts = $counts
    }
}

function Get-ProbeLabWorstStatus {
    [CmdletBinding()]
    param([Parameter(Mandatory)][object[]]$ProbeSummaries)

    if (@($ProbeSummaries | Where-Object status -eq 'FAIL').Count -gt 0) { return 'FAIL' }
    if (@($ProbeSummaries | Where-Object status -eq 'DEGRADED').Count -gt 0) { return 'DEGRADED' }
    return 'PASS'
}

function Resolve-ProbeLabLogPath {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$ServerRoot, [string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path)) { return '' }
    $resolved = if ([System.IO.Path]::IsPathRooted($Path)) { [System.IO.Path]::GetFullPath($Path) } else { [System.IO.Path]::GetFullPath((Join-Path $ServerRoot $Path)) }
    if (-not (Test-Path -LiteralPath $resolved -PathType Leaf)) { throw "Playerbots log does not exist: $resolved" }
    return $resolved
}

function Resolve-ProbeLabOutputPath {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$ServerRoot,
        [Parameter(Mandatory)][AllowEmptyString()][string]$Path,
        [Parameter(Mandatory)][string]$DefaultName
    )
    $logsRoot = [System.IO.Path]::GetFullPath((Join-Path $ServerRoot 'logs'))
    $candidate = if ([string]::IsNullOrWhiteSpace($Path)) { Join-Path $logsRoot $DefaultName } elseif ([System.IO.Path]::IsPathRooted($Path)) { $Path } else { Join-Path $logsRoot $Path }
    $full = [System.IO.Path]::GetFullPath($candidate)
    $prefix = $logsRoot.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    if (-not $full.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) { throw "Output path must remain under $logsRoot" }
    return $full
}

function Write-ProbeLabJsonLine {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][System.Collections.IDictionary]$Row)
    $parent = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    [System.IO.File]::AppendAllText($Path, (($Row | ConvertTo-Json -Compress -Depth 30) + [Environment]::NewLine), [System.Text.UTF8Encoding]::new($false))
}

function ConvertTo-ProbeLabMarkdownSummary {
    [CmdletBinding()]
    param([Parameter(Mandatory)][psobject]$Summary)
    $lines = [System.Collections.Generic.List[string]]::new()
    $lines.Add("# Probe Lab: $($Summary.status) - $($Summary.lab_id)")
    $lines.Add('')
    $lines.Add("- Mode: $($Summary.mode)")
    $lines.Add("- Status: $($Summary.status)")
    $lines.Add("- Reason: $($Summary.reason)")
    $lines.Add("- Samples: $($Summary.sample_count)")
    $lines.Add("- Receipt: $($Summary.receipt_path)")
    $lines.Add('')
    $lines.Add('- Health policy: PASS = all alive; DEGRADED = minority deaths/release with combat capacity; FAIL = first failure or dead members at least living members.')
    $lines.Add('')
    $lines.Add('| Probe | Kind/size | Status | Current health | Counts | First failure | Detail |')
    $lines.Add('| --- | ---: | --- | --- | --- | --- | --- |')
    foreach ($probe in @($Summary.probes)) {
        $failure = Get-ProbeLabProperty $probe first_failure $null
        $code = if ($null -eq $failure) { '' } else { [string]$failure.code }
        $detail = if ($null -eq $failure) { '' } else { ([string]$failure.detail -replace '\|', '\|') }
        $health = Get-ProbeLabProperty $probe current_health $null
        $reason = if ($null -eq $health) { [string](Get-ProbeLabProperty $probe reason '') } else { [string](Get-ProbeLabProperty $health reason '') }
        $counts = if ($null -eq $health) { '' } else { [string](Get-ProbeLabProperty $health count_summary '') }
        $lines.Add("| $($probe.probe_id) | $($probe.kind)/$($probe.size) | $($probe.status) | $($reason -replace '\|', '\|') | $($counts -replace '\|', '\|') | $code | $detail |")
    }
    return ($lines -join [Environment]::NewLine) + [Environment]::NewLine
}
