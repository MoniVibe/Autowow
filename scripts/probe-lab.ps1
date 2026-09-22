<#
.SYNOPSIS
    Safe manifest-driven launcher and monitor for concurrent dungeon or raid probes.
.DESCRIPTION
    Plan is the default and performs no writes or bridge calls. Launch requires -Apply because it
    activates and initializes exact GUIDs, forms groups, and issues named exterior movement.
    Monitor is read-only and samples list, combatlog, and bounded leader encounterlog evidence. This script never handles passwords,
    accesses a database, controls the server process, or uses an interior teleport operation.
#>
[CmdletBinding()]
param(
    [ValidateSet('Plan', 'Launch', 'Monitor')][string]$Mode = 'Plan',
    [Parameter(Mandatory)][string]$ManifestPath,
    [switch]$Apply,
    [ValidateSet('127.0.0.1')][string]$BridgeHost = '127.0.0.1',
    [ValidateRange(1, 65535)][int]$Port = 18787,
    [ValidateRange(1000, 120000)][int]$TimeoutMs = 5000,
    [ValidateRange(5, 600)][int]$OnlineTimeoutSeconds = 90,
    [ValidateRange(5, 1800)][int]$AdmissionTimeoutSeconds = 180,
    [ValidateRange(5, 600)][int]$ResetTimeoutSeconds = 90,
    [ValidateRange(1, 86400)][int]$DurationSeconds = 300,
    [ValidateRange(1, 60)][int]$PollSeconds = 5,
    [ValidateRange(0, 10)][int]$MonitorTransportMaxRetries = 2,
    [ValidateRange(0, 60000)][int]$MonitorTransportRetryBackoffMs = 500,
    [ValidateRange(0, 60000)][int]$MonitorTransportRetryMaxBackoffMs = 5000,
    [ValidateRange(5, 3600)][int]$StallSeconds = 60,
    [ValidateRange(1.0, 500.0)][double]$CohesionRadius = 60,
    [ValidateRange(0.1, 100.0)][double]$ProgressEpsilon = 1,
    [string[]]$ProbeId = @(),
    [string]$PlayerbotsLogPath = '',
    [string]$ReceiptPath = '',
    [string]$SummaryJsonPath = '',
    [string]$SummaryMarkdownPath = '',
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$ControlScriptPath = (Join-Path $PSScriptRoot 'autowow-control.ps1')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$script:ProbeLabEncounterMemberEvidenceCap = 40
$script:ProbeLabEncounterUnitEvidenceCap = 12
$libraryPath = Join-Path $PSScriptRoot 'probe-lab-lib.ps1'
if (-not (Test-Path -LiteralPath $libraryPath)) { throw "Missing Probe Lab library: $libraryPath" }
. $libraryPath

function Add-ProbeLabEncounterPlanMetadata {
    [CmdletBinding()]
    param([Parameter(Mandatory)][psobject]$Plan)

    $allowedActions = @($Plan.safety.allowed_bridge_actions) + @('encounterlog')
    $Plan.safety.allowed_bridge_actions = @($allowedActions | Select-Object -Unique)
    if ($Plan.safety -is [System.Collections.IDictionary]) {
        $Plan.safety['read_only_bridge_actions'] = @('list', 'combatlog', 'encounterlog')
    } else {
        $Plan.safety | Add-Member -Force -NotePropertyName read_only_bridge_actions -NotePropertyValue @('list', 'combatlog', 'encounterlog')
    }

    $Plan | Add-Member -Force -NotePropertyName evidence -NotePropertyValue ([ordered]@{
        read_only = $true
        bridge_actions = @('list', 'combatlog', 'encounterlog')
        per_probe = [ordered]@{
            list = 'exact manifest members'
            combatlog = 'one request per exact manifest member'
            encounterlog = 'one request per probe leader'
        }
        encounter_projection = [ordered]@{
            fields = @('phase', 'roster', 'member_context', 'encounter_units')
            max_member_context = $script:ProbeLabEncounterMemberEvidenceCap
            max_encounter_units = $script:ProbeLabEncounterUnitEvidenceCap
            raw_payload_stored = $false
        }
    })

    $monitorOperation = @($Plan.operations | Where-Object { $_.action -eq 'monitor-list-and-combatlog' })[0]
    if ($null -ne $monitorOperation) {
        $monitorOperation.action = 'monitor-list-combatlog-encounterlog'
        $monitorOperation | Add-Member -Force -NotePropertyName evidence -NotePropertyValue @('list', 'combatlog', 'encounterlog')
        $monitorOperation | Add-Member -Force -NotePropertyName read_only -NotePropertyValue $true
        $monitorOperation | Add-Member -Force -NotePropertyName boundary -NotePropertyValue 'per launched probe leader; compact bounded projection; no mutations'
    }
    return $Plan
}

$manifest = Read-ProbeLabManifest -Path $ManifestPath
$resolvedManifestPath = (Resolve-Path -LiteralPath $ManifestPath).Path
if ($ProbeId.Count -gt 0) {
    $requestedIds = @($ProbeId | Sort-Object -Unique)
    $knownIds = @($manifest.probes | ForEach-Object { [string]$_.id })
    $unknownIds = @($requestedIds | Where-Object { $_ -notin $knownIds })
    if ($unknownIds.Count -gt 0) { throw "Unknown ProbeId: $($unknownIds -join ', ')" }
    $manifest.probes = @($manifest.probes | Where-Object { [string]$_.id -in $requestedIds })
}
$plan = Add-ProbeLabEncounterPlanMetadata -Plan (New-ProbeLabPlan -Manifest $manifest -ManifestPath $resolvedManifestPath)
if ($Mode -eq 'Plan' -or ($Mode -eq 'Launch' -and -not $Apply)) {
    if ($Mode -eq 'Launch') {
        $plan.mode = 'Launch'
        $plan | Add-Member -NotePropertyName note -NotePropertyValue 'Dry-run only. Supply -Apply to authorize launch mutations.'
    }
    $plan | ConvertTo-Json -Depth 30
    return
}
if ($Mode -eq 'Monitor' -and $Apply) { throw '-Apply is not valid in read-only Monitor mode.' }
if (-not (Test-Path -LiteralPath $ControlScriptPath -PathType Leaf)) { throw "Missing AutoWoW control script: $ControlScriptPath" }

$resolvedRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
$runId = "$($manifest.lab_id)-$(Get-Date -Format 'yyyyMMdd-HHmmss')-$([guid]::NewGuid().ToString('N').Substring(0, 8))"
$ReceiptPath = Resolve-ProbeLabOutputPath -ServerRoot $resolvedRoot -Path $ReceiptPath -DefaultName "probe-lab-$runId.jsonl"
$SummaryJsonPath = Resolve-ProbeLabOutputPath -ServerRoot $resolvedRoot -Path $SummaryJsonPath -DefaultName "probe-lab-$runId-summary.json"
$SummaryMarkdownPath = Resolve-ProbeLabOutputPath -ServerRoot $resolvedRoot -Path $SummaryMarkdownPath -DefaultName "probe-lab-$runId-summary.md"
foreach ($path in @($ReceiptPath, $SummaryJsonPath, $SummaryMarkdownPath)) {
    if (Test-Path -LiteralPath $path) { throw "Refusing to overwrite existing Probe Lab output: $path" }
}
$resolvedPlayerbotsLog = Resolve-ProbeLabLogPath -ServerRoot $resolvedRoot -Path $PlayerbotsLogPath

function Write-LabReceipt {
    param([Parameter(Mandatory)][string]$Event, [System.Collections.IDictionary]$Fields = @{})
    $row = [ordered]@{ schema = $script:ProbeLabReceiptSchema; timestamp_utc = [datetime]::UtcNow.ToString('o'); run_id = $runId; lab_id = $manifest.lab_id; mode = $Mode; event = $Event }
    foreach ($key in $Fields.Keys) { $row[$key] = $Fields[$key] }
    Write-ProbeLabJsonLine -Path $ReceiptPath -Row $row
}

function Invoke-LabControl {
    param(
        [Parameter(Mandatory)][ValidateSet('list', 'activate', 'probe-reset', 'fixture-init', 'party', 'raid-create', 'route', 'advance', 'deploy', 'combatlog', 'encounterlog')][string]$Action,
        [uint32]$Guid = 0, [uint32[]]$MemberGuid = @(), [int]$Level = 1, [int]$SpecIndex = 0,
        [int]$Quality = 2, [int]$RaidDifficulty = 0, [string]$Destination = '',
        [uint32]$ExpectedMapId = 0, [uint32]$ExpectedDifficulty = 0, [switch]$AllowError
    )
    try {
        $parameters = @{ Action = $Action; BridgeHost = $BridgeHost; Port = $Port; TimeoutMs = $TimeoutMs }
        if ($Guid -ne 0) { $parameters.BotGuid = $Guid }
        if ($MemberGuid.Count -gt 0) { $parameters.MemberGuid = [uint32[]]$MemberGuid }
        if ($Action -eq 'fixture-init') { $parameters.Level = $Level; $parameters.SpecIndex = $SpecIndex; $parameters.Quality = $Quality }
        if ($Action -eq 'raid-create') { $parameters.RaidDifficulty = $RaidDifficulty }
        if ($Action -eq 'probe-reset') { $parameters.ExpectedMapId = $ExpectedMapId; $parameters.ExpectedDifficulty = $ExpectedDifficulty }
        if ($Action -in @('route', 'advance')) { $parameters.Destination = $Destination }
        if ($Action -eq 'probe-reset') { $parameters.Destination = $Destination }
        return ConvertFrom-ProbeLabControlJson -Raw @(& $ControlScriptPath @parameters) -Action $Action -Guid $Guid -AllowRefusal:($Action -eq 'probe-reset')
    } catch {
        if ($AllowError) { return [pscustomobject]@{ ok = $false; error = 'bridge_error'; detail = $_.Exception.Message; guid = $Guid } }
        throw
    }
}

function Get-LabErrorDetail {
    [CmdletBinding()]
    param([Parameter(Mandatory)][AllowNull()][object]$ErrorRecord)

    $messages = [System.Collections.Generic.List[string]]::new()
    $exception = if ($ErrorRecord -is [System.Management.Automation.ErrorRecord]) {
        $ErrorRecord.Exception
    } elseif ($ErrorRecord -is [System.Exception]) {
        $ErrorRecord
    } else {
        $null
    }
    while ($null -ne $exception) {
        if (-not [string]::IsNullOrWhiteSpace([string]$exception.Message)) { $messages.Add([string]$exception.Message) }
        $exception = $exception.InnerException
    }
    if ($messages.Count -eq 0) { return [string]$ErrorRecord }
    return ($messages -join ' | ')
}

function Test-LabTransientBridgeTimeout {
    [CmdletBinding()]
    param([Parameter(Mandatory)][AllowNull()][object]$ErrorRecord)

    $detail = Get-LabErrorDetail -ErrorRecord $ErrorRecord
    if ($detail -match '(?i)could not connect to the autowow bridge') { return $true }
    if ($detail -notmatch '(?i)timed?\s*out|timeout|time-out') { return $false }
    return $true
}

function ConvertTo-ProbeLabEncounterUnitEvidence {
    [CmdletBinding()]
    param([AllowNull()][object]$Unit)

    if ($null -eq $Unit) { return $null }
    $rank = Get-ProbeLabProperty $Unit rank $null
    $health = Get-ProbeLabProperty $Unit health $null
    $healthPct = if ($null -eq $health) { $null } else { [double](Get-ProbeLabProperty $health pct $null) }
    return [pscustomobject][ordered]@{
        guid = [uint64](Get-ProbeLabProperty $Unit guid 0)
        entry = [uint32](Get-ProbeLabProperty $Unit entry 0)
        name = [string](Get-ProbeLabProperty $Unit name '')
        boss = [bool](Get-ProbeLabProperty $rank boss $false)
        rank = [string](Get-ProbeLabProperty $rank name '')
        alive = [bool](Get-ProbeLabProperty $Unit alive $false)
        health_pct = $healthPct
        distance = if ($null -ne (Get-ProbeLabProperty $Unit position $null)) {
            [double](Get-ProbeLabProperty (Get-ProbeLabProperty $Unit position $null) distance 0)
        } else { $null }
        in_combat = [bool](Get-ProbeLabProperty $Unit in_combat $false)
        engaged_by_group = [bool](Get-ProbeLabProperty $Unit engaged_by_group $false)
        victim = ConvertTo-ProbeLabEncounterUnitEvidence -Unit (Get-ProbeLabProperty $Unit victim $null)
        active_spell_id = [uint32](Get-ProbeLabProperty $Unit active_spell_id 0)
        group_links = [ordered]@{
            threatening_members = [uint32](Get-ProbeLabProperty (Get-ProbeLabProperty $Unit group_links $null) threatening_members 0)
            targeting_members = [uint32](Get-ProbeLabProperty (Get-ProbeLabProperty $Unit group_links $null) targeting_members 0)
            threatening_or_targeting_members = [uint32](Get-ProbeLabProperty (Get-ProbeLabProperty $Unit group_links $null) threatening_or_targeting_members 0)
            victim_links = [uint32](Get-ProbeLabProperty (Get-ProbeLabProperty $Unit group_links $null) victim_links 0)
            combat_links = [uint32](Get-ProbeLabProperty (Get-ProbeLabProperty $Unit group_links $null) combat_links 0)
            involved_members = [uint32](Get-ProbeLabProperty (Get-ProbeLabProperty $Unit group_links $null) involved_members 0)
        }
    }
}

function ConvertTo-ProbeLabEncounterMemberEvidence {
    [CmdletBinding()]
    param([Parameter(Mandatory)][psobject]$Member)

    $health = Get-ProbeLabProperty $Member health $null
    $mana = Get-ProbeLabProperty $Member mana $null
    $power = Get-ProbeLabProperty $Member power $null
    $target = Get-ProbeLabProperty $Member target $null
    $threat = Get-ProbeLabProperty $Member threat $null
    return [pscustomobject][ordered]@{
        guid = [uint64](Get-ProbeLabProperty $Member guid 0)
        name = [string](Get-ProbeLabProperty $Member name '')
        alive = [bool](Get-ProbeLabProperty $Member alive $false)
        death_state = [string](Get-ProbeLabProperty $Member death_state 'unknown')
        released = [bool](Get-ProbeLabProperty $Member released $false)
        in_combat = [bool](Get-ProbeLabProperty $Member in_combat $false)
        roles = [ordered]@{
            tank = [bool](Get-ProbeLabProperty (Get-ProbeLabProperty $Member roles $null) tank $false)
            healer = [bool](Get-ProbeLabProperty (Get-ProbeLabProperty $Member roles $null) healer $false)
            dps = [bool](Get-ProbeLabProperty (Get-ProbeLabProperty $Member roles $null) dps $false)
            ranged = [bool](Get-ProbeLabProperty (Get-ProbeLabProperty $Member roles $null) ranged $false)
            main_tank = [bool](Get-ProbeLabProperty (Get-ProbeLabProperty $Member roles $null) main_tank $false)
            assist_tank = [bool](Get-ProbeLabProperty (Get-ProbeLabProperty $Member roles $null) assist_tank $false)
        }
        health_pct = if ($null -eq $health) { $null } else { [double](Get-ProbeLabProperty $health pct $null) }
        mana_pct = if ($null -eq $mana) { $null } else { [double](Get-ProbeLabProperty $mana pct $null) }
        power = [ordered]@{
            type = [uint32](Get-ProbeLabProperty $power type 0)
            current = [uint32](Get-ProbeLabProperty $power current 0)
            max = [uint32](Get-ProbeLabProperty $power max 0)
        }
        target = [ordered]@{
            source = [string](Get-ProbeLabProperty $target source 'none')
            selected_guid = [uint64](Get-ProbeLabProperty $target selected_guid 0)
            unit = ConvertTo-ProbeLabEncounterUnitEvidence -Unit (Get-ProbeLabProperty $target unit $null)
        }
        victim = ConvertTo-ProbeLabEncounterUnitEvidence -Unit (Get-ProbeLabProperty $Member victim $null)
        active_spell_id = [uint32](Get-ProbeLabProperty $Member active_spell_id 0)
        threat = [ordered]@{
            owned_creatures = [uint32](Get-ProbeLabProperty $threat owned_creatures 0)
            owned_creatures_targeting_member = [uint32](Get-ProbeLabProperty $threat owned_creatures_targeting_member 0)
            highest_owned_creature_guid = [uint64](Get-ProbeLabProperty $threat highest_owned_creature_guid 0)
            highest_owned_threat = [double](Get-ProbeLabProperty $threat highest_owned_threat 0)
        }
    }
}

function ConvertTo-ProbeLabEncounterEvidence {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][uint32]$LeaderGuid,
        [AllowNull()][object]$Response,
        [uint32[]]$ExpectedRoster = @()
    )

    $ok = Get-ProbeLabProperty $Response ok $null
    if ($null -eq $Response -or ($null -ne $ok -and -not [bool]$ok)) {
        $errorCode = [string](Get-ProbeLabProperty $Response error 'encounterlog_unavailable')
        $detail = [string](Get-ProbeLabProperty $Response detail $errorCode)
        return [pscustomobject][ordered]@{
            ok = $false
            transport = 'error'
            leader_guid = $LeaderGuid
            error = $errorCode
            detail = $detail
        }
    }

    $roster = Get-ProbeLabProperty $Response roster $null
    $rawRosterGuids = @(Get-ProbeLabProperty $roster guids @())
    $observedRosterGuids = @($rawRosterGuids | Select-Object -First $script:ProbeLabEncounterMemberEvidenceCap | ForEach-Object { [uint64]$_ })
    $expectedRosterGuids = @($ExpectedRoster | ForEach-Object { [uint64]$_ } | Sort-Object)
    $rosterTruncated = [bool](Get-ProbeLabProperty $roster guids_truncated $false) -or $rawRosterGuids.Count -gt $script:ProbeLabEncounterMemberEvidenceCap
    $sortedObservedRosterGuids = @($observedRosterGuids | Sort-Object)
    $exactRoster = -not $rosterTruncated -and $sortedObservedRosterGuids.Count -eq $expectedRosterGuids.Count -and
        (($sortedObservedRosterGuids -join ',') -eq ($expectedRosterGuids -join ','))

    $rawMembers = @(Get-ProbeLabProperty $roster members @())
    $memberContextTruncated = [bool](Get-ProbeLabProperty $Response members_truncated $false) -or $rawMembers.Count -gt $script:ProbeLabEncounterMemberEvidenceCap
    $memberContext = @($rawMembers | Select-Object -First $script:ProbeLabEncounterMemberEvidenceCap | ForEach-Object {
        ConvertTo-ProbeLabEncounterMemberEvidence -Member $_
    })

    $encounterUnits = Get-ProbeLabProperty $Response encounter_units $null
    $rawUnits = @(Get-ProbeLabProperty $encounterUnits units @())
    $capturedUnits = @($rawUnits | Select-Object -First $script:ProbeLabEncounterUnitEvidenceCap | ForEach-Object {
        ConvertTo-ProbeLabEncounterUnitEvidence -Unit $_
    })
    $unitTruncated = [bool](Get-ProbeLabProperty $encounterUnits truncated $false) -or $rawUnits.Count -gt $script:ProbeLabEncounterUnitEvidenceCap
    $phaseValue = Get-ProbeLabProperty $Response phase $null
    if ($null -eq $phaseValue) { $phaseValue = Get-ProbeLabProperty $Response encounter_phase $null }

    return [pscustomobject][ordered]@{
        ok = $true
        transport = 'ok'
        error = $null
        schema = [string](Get-ProbeLabProperty $Response schema '')
        version = [uint32](Get-ProbeLabProperty $Response version 0)
        leader_guid = [uint32](Get-ProbeLabProperty $Response leader_guid $LeaderGuid)
        phase = if ($null -eq $phaseValue) { $null } else { [string]$phaseValue }
        map_id = [uint32](Get-ProbeLabProperty $Response map_id 0)
        instance_id = [uint32](Get-ProbeLabProperty $Response instance_id 0)
        difficulty = [uint32](Get-ProbeLabProperty $Response difficulty 0)
        roster = [ordered]@{
            total = [uint32](Get-ProbeLabProperty $roster total $rawRosterGuids.Count)
            expected_count = $expectedRosterGuids.Count
            exact_match = $exactRoster
            guids_truncated = $rosterTruncated
            guids = @($observedRosterGuids)
            expected_guids = @($expectedRosterGuids)
            online_playerbots = [uint32](Get-ProbeLabProperty $roster online_playerbots $rawMembers.Count)
            members_truncated = $memberContextTruncated
        }
        member_context = @($memberContext)
        encounter_units = [ordered]@{
            eligible_before_cap = [uint32](Get-ProbeLabProperty $encounterUnits eligible_before_cap $rawUnits.Count)
            source_count = [uint32](Get-ProbeLabProperty $encounterUnits count $rawUnits.Count)
            source_cap = [uint32](Get-ProbeLabProperty $encounterUnits cap 0)
            captured_count = $capturedUnits.Count
            cap = $script:ProbeLabEncounterUnitEvidenceCap
            truncated = $unitTruncated
            units = @($capturedUnits)
        }
    }
}

function Get-ProbeLabEncounterEvidence {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][uint32]$LeaderGuid,
        [uint32[]]$ExpectedRoster = @()
    )

    try {
        $response = Invoke-LabControl -Action encounterlog -Guid $LeaderGuid -AllowError
        return ConvertTo-ProbeLabEncounterEvidence -LeaderGuid $LeaderGuid -Response $response -ExpectedRoster $ExpectedRoster
    } catch {
        return ConvertTo-ProbeLabEncounterEvidence -LeaderGuid $LeaderGuid `
            -Response ([pscustomobject]@{ ok = $false; error = 'bridge_error'; detail = Get-LabErrorDetail -ErrorRecord $_ }) `
            -ExpectedRoster $ExpectedRoster
    }
}

function Invoke-ProbeLabMonitorSampleWithRetry {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][scriptblock]$Invoke,
        [ValidateRange(0, 10)][int]$MaxRetries = 2,
        [ValidateRange(0, 60000)][int]$InitialBackoffMs = 500,
        [ValidateRange(0, 60000)][int]$MaxBackoffMs = 5000,
        [scriptblock]$Sleep = { param([int]$Milliseconds) Start-Sleep -Milliseconds $Milliseconds },
        [scriptblock]$OnEvent = { param([psobject]$Event) }
    )

    $maxAttempts = $MaxRetries + 1
    $attempt = 0
    while ($attempt -lt $maxAttempts) {
        ++$attempt
        try {
            $sample = & $Invoke
            if ($null -eq $sample) { throw 'Monitor sample returned no data.' }
            if ($attempt -gt 1) {
                $null = & $OnEvent ([pscustomobject][ordered]@{
                    event = 'monitor_transport_retry_success'
                    operation = 'sample'
                    attempts = $attempt
                    retries = $attempt - 1
                    max_retries = $MaxRetries
                    max_attempts = $maxAttempts
                })
            }
            return [pscustomobject][ordered]@{
                sample = $sample
                attempts = $attempt
                retries = $attempt - 1
            }
        } catch {
            $errorRecord = $_
            $detail = Get-LabErrorDetail -ErrorRecord $errorRecord
            $transient = Test-LabTransientBridgeTimeout -ErrorRecord $errorRecord
            if (-not $transient) { throw }

            if ($attempt -ge $maxAttempts) {
                $null = & $OnEvent ([pscustomobject][ordered]@{
                    event = 'monitor_transport_retry_exhausted'
                    operation = 'sample'
                    attempts = $attempt
                    retries = $attempt - 1
                    max_retries = $MaxRetries
                    max_attempts = $maxAttempts
                    detail = $detail
                })
                throw "Monitor bridge transport retry budget exhausted after $attempt attempt(s): $detail"
            }

            $retryNumber = $attempt
            $backoffMs = [int][Math]::Min(
                [double]$MaxBackoffMs,
                [double]$InitialBackoffMs * [Math]::Pow(2, $retryNumber - 1)
            )
            $null = & $OnEvent ([pscustomobject][ordered]@{
                event = 'monitor_transport_retry'
                operation = 'sample'
                attempt = $attempt
                retry_number = $retryNumber
                retries = $retryNumber
                max_retries = $MaxRetries
                max_attempts = $maxAttempts
                backoff_ms = $backoffMs
                detail = $detail
            })
            if ($backoffMs -gt 0) { $null = & $Sleep $backoffMs }
        }
    }

    throw "Monitor bridge transport retry budget exhausted after $maxAttempts attempt(s)."
}

function Get-LabSample {
    param([switch]$MonitorSampling)

    $list = Invoke-LabControl -Action list
    $combat = [ordered]@{}
    foreach ($guid in @($manifest.probes.members.guid | ForEach-Object { [uint32]$_ })) {
        $response = Invoke-LabControl -Action combatlog -Guid $guid -AllowError
        if ($MonitorSampling -and -not [bool](Get-ProbeLabProperty $response ok $true) -and
            (Test-LabTransientBridgeTimeout -ErrorRecord (Get-ProbeLabProperty $response detail (Get-ProbeLabProperty $response error 'bridge_error')))) {
            $detail = [string](Get-ProbeLabProperty $response detail (Get-ProbeLabProperty $response error 'bridge_error'))
            throw "Monitor bridge transport timeout during combatlog for GUID ${guid}: $detail"
        }
        $combat[[string]$guid] = $response
    }
    $sample = ConvertTo-ProbeLabSample -Manifest $manifest -ListResponse $list -CombatByGuid $combat
    if (-not $MonitorSampling) { return $sample }

    foreach ($probeSample in @($sample.probes)) {
        $definition = @($manifest.probes | Where-Object id -eq $probeSample.probe_id)[0]
        $expectedRoster = @($definition.members | ForEach-Object { [uint32]$_.guid })
        $encounterEvidence = Get-ProbeLabEncounterEvidence -LeaderGuid ([uint32]$definition.leader_guid) -ExpectedRoster $expectedRoster
        $probeSample | Add-Member -Force -NotePropertyName encounterlog -NotePropertyValue $encounterEvidence
    }
    return $sample
}

function Assert-LabGroupsSafeToForm {
    param([Parameter(Mandatory)][psobject]$Sample, [Parameter(Mandatory)][string]$ProbeId)
    foreach ($probe in @($Sample.probes | Where-Object probe_id -eq $ProbeId)) {
        $members = @($probe.members)
        $exactGroup = $members.Count -eq $probe.size -and @($members | Where-Object { $_.group_members -eq $probe.size -and [uint32]$_.leader_guid -eq [uint32]$probe.leader_guid }).Count -eq $probe.size
        if ($exactGroup) { continue }
        if (@($members | Where-Object { $_.group_members -ne 0 -or $_.leader_guid -ne 0 }).Count -gt 0) {
            throw "Probe '$($probe.probe_id)' has a partial or foreign group; refusing to regroup exact members."
        }
    }
}

function Invoke-LabLaunch {
    Write-LabReceipt -Event 'launch_started' -Fields ([ordered]@{ manifest_path = $resolvedManifestPath; plan = $plan })
    $initialList = Invoke-LabControl -Action list
    $online = @(Get-ProbeLabProperty $initialList bots @() | ForEach-Object { [uint32]$_.guid })
    $probeResults = foreach ($probe in @($manifest.probes)) {
        $operation = 'activate'
        [Nullable[uint32]]$operationGuid = $null
        try {
            foreach ($member in @($probe.members)) {
                $operationGuid = [uint32]$member.guid
                if ([uint32]$member.guid -notin $online) {
                    $response = Invoke-LabControl -Action activate -Guid ([uint32]$member.guid)
                    Write-LabReceipt -Event 'bridge_mutation' -Fields ([ordered]@{ probe_id = $probe.id; action = 'activate'; guid = [uint32]$member.guid; response = $response })
                }
            }

            $operation = 'wait-online'
            $operationGuid = $null
            $deadline = (Get-Date).AddSeconds($OnlineTimeoutSeconds)
            do {
                $list = Invoke-LabControl -Action list
                $online = @(Get-ProbeLabProperty $list bots @() | ForEach-Object { [uint32]$_.guid })
                $missing = @($probe.members.guid | ForEach-Object { [uint32]$_ } | Where-Object { $_ -notin $online })
                if ($missing.Count -eq 0) { break }
                Start-Sleep -Seconds ([Math]::Min(2, $PollSeconds))
            } while ((Get-Date) -lt $deadline)
            if ($missing.Count -gt 0) { throw "Timed out waiting for probe '$($probe.id)' GUIDs to come online: $($missing -join ',')" }
            Write-LabReceipt -Event 'probe_members_online' -Fields ([ordered]@{ probe_id = $probe.id; guids = @($probe.members.guid | ForEach-Object { [uint32]$_ }) })

            $operation = 'probe-reset'
            $operationGuid = [uint32]$probe.leader_guid
            $otherGuids = @($probe.members | Where-Object { [uint32]$_.guid -ne [uint32]$probe.leader_guid } | ForEach-Object { [uint32]$_.guid })
            $resetResponse = Wait-ProbeLabReset -TimeoutSeconds $ResetTimeoutSeconds `
                -PollSeconds ([Math]::Min(2, $PollSeconds)) -Invoke {
                $response = Invoke-LabControl -Action probe-reset -Guid ([uint32]$probe.leader_guid) `
                    -MemberGuid $otherGuids -Destination ([string]$probe.exterior_route) `
                    -ExpectedMapId ([uint32]$probe.expected_map_id) `
                    -ExpectedDifficulty ([uint32](Get-ProbeLabProperty $probe raid_difficulty 0))
                Write-LabReceipt -Event 'probe_reset' -Fields ([ordered]@{ probe_id = $probe.id; response = $response })
                $response
            }

            $operation = 'fixture-init'
            foreach ($member in @($probe.members)) {
                $operationGuid = [uint32]$member.guid
                $response = Invoke-LabControl -Action fixture-init -Guid ([uint32]$member.guid) -Level ([int]$member.level) -SpecIndex ([int]$member.spec_index) -Quality ([int]$member.quality)
                Write-LabReceipt -Event 'bridge_mutation' -Fields ([ordered]@{ probe_id = $probe.id; action = 'fixture-init'; guid = [uint32]$member.guid; level = [int]$member.level; spec_index = [int]$member.spec_index; quality = [int]$member.quality; response = $response })
            }

            $operation = 'group-safety'
            $operationGuid = [uint32]$probe.leader_guid
            $preGroup = Get-LabSample
            Assert-LabGroupsSafeToForm -Sample $preGroup -ProbeId ([string]$probe.id)
            $sampleProbe = @($preGroup.probes | Where-Object probe_id -eq $probe.id)[0]
            $alreadyGrouped = @($sampleProbe.members | Where-Object { $_.group_members -eq [int]$probe.size -and [uint32]$_.leader_guid -eq [uint32]$probe.leader_guid }).Count -eq [int]$probe.size
            $otherGuids = @($probe.members | Where-Object { [uint32]$_.guid -ne [uint32]$probe.leader_guid } | ForEach-Object { [uint32]$_.guid })
            if (-not $alreadyGrouped) {
                $operation = if ($probe.kind -eq 'party') { 'party' } else { 'raid-create' }
                $response = if ($probe.kind -eq 'party') {
                    Invoke-LabControl -Action party -Guid ([uint32]$probe.leader_guid) -MemberGuid $otherGuids
                } else {
                    Invoke-LabControl -Action raid-create -Guid ([uint32]$probe.leader_guid) -MemberGuid $otherGuids -RaidDifficulty ([int](Get-ProbeLabProperty $probe raid_difficulty 0))
                }
                Write-LabReceipt -Event 'bridge_mutation' -Fields ([ordered]@{ probe_id = $probe.id; action = $operation; leader_guid = [uint32]$probe.leader_guid; member_guids = $otherGuids; response = $response })
            }

            $operation = 'route'
            $routeResponse = Invoke-LabControl -Action route -Guid ([uint32]$probe.leader_guid) -Destination ([string]$probe.exterior_route)
            Write-LabReceipt -Event 'bridge_mutation' -Fields ([ordered]@{ probe_id = $probe.id; action = 'route'; leader_guid = [uint32]$probe.leader_guid; destination = [string]$probe.exterior_route; exterior_only = $true; response = $routeResponse })

            $waypoint = [string](Get-ProbeLabProperty $probe advance_waypoint '')
            if (-not [string]::IsNullOrWhiteSpace($waypoint)) {
                $operation = 'advance'
                $admissionDeadline = (Get-Date).AddSeconds($AdmissionTimeoutSeconds)
                $admitted = $false
                do {
                    $response = Invoke-LabControl -Action advance -Guid ([uint32]$probe.leader_guid) -Destination $waypoint
                    Write-LabReceipt -Event 'bridge_mutation' -Fields ([ordered]@{ probe_id = $probe.id; action = 'advance'; leader_guid = [uint32]$probe.leader_guid; destination = $waypoint; serial_admission = $true; response = $response })
                    Start-Sleep -Seconds $PollSeconds
                    $admissionSample = Get-LabSample
                    $probeSample = @($admissionSample.probes | Where-Object probe_id -eq $probe.id)[0]
                    $admitted = Test-ProbeLabAdmission -ProbeSample $probeSample
                    Write-LabReceipt -Event 'admission_sample' -Fields ([ordered]@{ probe_id = $probe.id; admitted = $admitted; members = $probeSample.members })
                } while (-not $admitted -and (Get-Date) -lt $admissionDeadline)
                if (-not $admitted) {
                    $operation = 'admission'
                    throw "Probe '$($probe.id)' admission timed out."
                }
            }

            $operation = 'deploy'
            $operationGuid = [uint32]$probe.leader_guid
            $deployResponse = Invoke-LabControl -Action deploy -Guid ([uint32]$probe.leader_guid)
            Write-LabReceipt -Event 'bridge_mutation' -Fields ([ordered]@{ probe_id = $probe.id; action = 'deploy'; leader_guid = [uint32]$probe.leader_guid; response = $deployResponse })

            [pscustomobject][ordered]@{ probe_id = $probe.id; status = 'LAUNCHED'; first_failure = $null }
        } catch {
            $code = if ($operation -eq 'wait-online') { 'offline' } elseif ($operation -eq 'admission') { 'admission_timeout' } else { '' }
            $failure = New-ProbeLabLaunchFailure -ProbeId ([string]$probe.id) -Operation $operation -Detail $_.Exception.Message -Code $code -Guid $operationGuid
            Write-LabReceipt -Event 'probe_launch_failed' -Fields ([ordered]@{ probe_id = $probe.id; failure = $failure })
            [pscustomobject][ordered]@{ probe_id = $probe.id; status = 'FAIL'; first_failure = $failure }
        }
    }

    $failedCount = @($probeResults | Where-Object status -eq 'FAIL').Count
    $launchedCount = @($probeResults | Where-Object status -eq 'LAUNCHED').Count
    $status = if ($failedCount -eq 0) { 'LAUNCHED' } elseif ($launchedCount -eq 0) { 'FAILED' } else { 'PARTIAL' }
    Write-LabReceipt -Event 'launch_complete' -Fields ([ordered]@{ status = $status; launched_count = $launchedCount; failed_count = $failedCount; probes = @($probeResults); next_action = 'Run Monitor mode without -Apply for launched probes.' })
    return [pscustomobject][ordered]@{ schema = $script:ProbeLabSummarySchema; run_id = $runId; lab_id = $manifest.lab_id; mode = 'Launch'; status = $status; receipt_path = $ReceiptPath; probes = @($probeResults) }
}

function Invoke-LabMonitor {
    Write-LabReceipt -Event 'monitor_started' -Fields ([ordered]@{
        manifest_path = $resolvedManifestPath
        duration_seconds = $DurationSeconds
        poll_seconds = $PollSeconds
        playerbots_log_path = $resolvedPlayerbotsLog
        encounter_unit_evidence_cap = $script:ProbeLabEncounterUnitEvidenceCap
        transport_max_retries = $MonitorTransportMaxRetries
        transport_max_attempts = $MonitorTransportMaxRetries + 1
        transport_retry_backoff_ms = $MonitorTransportRetryBackoffMs
        transport_retry_max_backoff_ms = $MonitorTransportRetryMaxBackoffMs
    })
    $firstFailures = @{}
    $previousByProbe = @{}
    $lastProgressAt = @{}
    $lastLeaderByProbe = @{}
    $admissionStartedAt = @{}
    $admissionConvergedByProbe = @{}
    $knownDeadByProbe = @{}
    $latestByProbe = @{}
    $sampleCount = 0
    $logByteCursor = if ([string]::IsNullOrWhiteSpace($resolvedPlayerbotsLog)) { 0L } else { (Get-Item -LiteralPath $resolvedPlayerbotsLog).Length }
    $deadline = (Get-Date).AddSeconds($DurationSeconds)
    do {
        $sampleAttempt = Invoke-ProbeLabMonitorSampleWithRetry -Invoke { Get-LabSample -MonitorSampling } `
            -MaxRetries $MonitorTransportMaxRetries `
            -InitialBackoffMs $MonitorTransportRetryBackoffMs `
            -MaxBackoffMs $MonitorTransportRetryMaxBackoffMs `
            -OnEvent {
                param([psobject]$transportEvent)
                $fields = [ordered]@{
                    operation = [string](Get-ProbeLabProperty $transportEvent operation 'sample')
                    attempts = [int](Get-ProbeLabProperty $transportEvent attempts (Get-ProbeLabProperty $transportEvent attempt 0))
                    retries = [int](Get-ProbeLabProperty $transportEvent retries 0)
                    max_retries = [int](Get-ProbeLabProperty $transportEvent max_retries 0)
                    max_attempts = [int](Get-ProbeLabProperty $transportEvent max_attempts 0)
                }
                foreach ($name in @('attempt', 'retry_number', 'backoff_ms', 'detail')) {
                    $value = Get-ProbeLabProperty $transportEvent $name $null
                    if ($null -ne $value) { $fields[$name] = $value }
                }
                Write-LabReceipt -Event ([string]$transportEvent.event) -Fields $fields
            }
        $sample = $sampleAttempt.sample
        ++$sampleCount
        $newLogLines = @()
        if (-not [string]::IsNullOrWhiteSpace($resolvedPlayerbotsLog)) {
            $logDelta = Read-ProbeLabLogDelta -Path $resolvedPlayerbotsLog -Offset $logByteCursor
            $newLogLines = @($logDelta.lines)
            $logByteCursor = [long]$logDelta.next_offset
        }
        foreach ($probeSample in @($sample.probes)) {
            $id = [string]$probeSample.probe_id
            $definition = @($manifest.probes | Where-Object id -eq $id)[0]
            if (-not $knownDeadByProbe.ContainsKey($id)) { $knownDeadByProbe[$id] = [System.Collections.Generic.HashSet[uint32]]::new() }
            foreach ($member in @($probeSample.members | Where-Object { $_.combat_telemetry -and -not $_.alive -and $_.death_state -in @('dead', 'corpse', 'ghost', 'released') })) {
                $null = $knownDeadByProbe[$id].Add([uint32]$member.guid)
            }
            $leader = @($probeSample.members | Where-Object { [uint32]$_.guid -eq [uint32]$probeSample.leader_guid })[0]
            if (-not $lastProgressAt.ContainsKey($id)) { $lastProgressAt[$id] = [datetime]::UtcNow; $lastLeaderByProbe[$id] = $leader }
            else {
                $movement = Get-ProbeLabDistance3d -A $leader -B $lastLeaderByProbe[$id]
                if ($movement -ge $ProgressEpsilon -or @($probeSample.members | Where-Object { $_.in_combat }).Count -gt 0) {
                    $lastProgressAt[$id] = [datetime]::UtcNow
                    $lastLeaderByProbe[$id] = $leader
                }
            }
            $names = @($probeSample.members.name | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
            $navigatorEvents = @(Get-ProbeLabNavigatorEvents -Lines $newLogLines -MemberNames $names)
            $progressTimedOut = ([datetime]::UtcNow - [datetime]$lastProgressAt[$id]).TotalSeconds -ge $StallSeconds
            $waypoint = [string](Get-ProbeLabProperty $definition advance_waypoint '')
            if (-not $admissionStartedAt.ContainsKey($id)) { $admissionStartedAt[$id] = [datetime]::UtcNow }
            if (-not $admissionConvergedByProbe.ContainsKey($id)) { $admissionConvergedByProbe[$id] = $false }
            $admissionState = Get-ProbeLabAdmissionState -ProbeSample $probeSample -AdvanceWaypoint $waypoint `
                -PreviouslyConverged ([bool]$admissionConvergedByProbe[$id]) `
                -StartedAt ([datetime]$admissionStartedAt[$id]) -Now ([datetime]::UtcNow) `
                -TimeoutSeconds $AdmissionTimeoutSeconds
            $admissionConvergedByProbe[$id] = [bool]$admissionState.converged
            $admissionTimedOut = [bool]$admissionState.timed_out
            $failure = Get-ProbeLabFirstFailure -ProbeSample $probeSample -PreviousProbeSample $previousByProbe[$id] -NavigatorEvents $navigatorEvents -CohesionRadius $CohesionRadius -ProgressEpsilon $ProgressEpsilon -KnownDeadGuid @($knownDeadByProbe[$id]) -AdmissionTimedOut:$admissionTimedOut -ProgressTimedOut:$progressTimedOut
            if ($null -ne $failure -and -not $firstFailures.ContainsKey($id)) {
                $firstFailures[$id] = [pscustomobject][ordered]@{ code = $failure.code; detail = $failure.detail; navigator_reason = Get-ProbeLabProperty $failure navigator_reason $null; observed_at_utc = $sample.observed_at_utc }
                Write-LabReceipt -Event 'first_failure' -Fields ([ordered]@{ probe_id = $id; failure = $firstFailures[$id] })
            }
            Write-LabReceipt -Event 'monitor_sample' -Fields ([ordered]@{ probe_id = $id; observed_at_utc = $sample.observed_at_utc; members = $probeSample.members; encounterlog = Get-ProbeLabProperty $probeSample encounterlog $null; navigator_events = $navigatorEvents; admission_converged = [bool]$admissionState.converged; admission_timed_out = $admissionTimedOut; progress_timed_out = $progressTimedOut; transport_attempts = [int]$sampleAttempt.attempts; transport_retries = [int]$sampleAttempt.retries })
            $previousByProbe[$id] = $probeSample
            $latestByProbe[$id] = $probeSample
        }
        if ((Get-Date) -lt $deadline) { Start-Sleep -Seconds $PollSeconds }
    } while ((Get-Date) -lt $deadline)

    $probeSummaries = foreach ($probe in @($manifest.probes)) {
        $failure = if ($firstFailures.ContainsKey([string]$probe.id)) { $firstFailures[[string]$probe.id] } else { $null }
        $latest = $latestByProbe[[string]$probe.id]
        $deadMembers = @($latest.members | Where-Object { -not $_.alive })
        $currentHealth = Get-ProbeLabCurrentHealth -ProbeSample $latest -FirstFailure $failure
        [pscustomobject][ordered]@{
            probe_id = $probe.id; kind = $probe.kind; size = [int]$probe.size
            expected_map_id = [uint32]$probe.expected_map_id
            status = $currentHealth.status; reason = $currentHealth.reason; current_health = $currentHealth; first_failure = $failure
            observations = [ordered]@{
                dead_member_guids = @($deadMembers | ForEach-Object { [uint32]$_.guid })
                released_corpse_guids = @($deadMembers | Where-Object released_corpse | ForEach-Object { [uint32]$_.guid })
                active_map_ids = @($latest.members | Where-Object alive | ForEach-Object { [uint32]$_.map_id } | Sort-Object -Unique)
                active_instance_ids = @($latest.members | Where-Object alive | ForEach-Object { [uint32]$_.instance_id } | Sort-Object -Unique)
                encounterlog = Get-ProbeLabProperty $latest encounterlog $null
            }
        }
    }
    $failedCount = @($probeSummaries | Where-Object status -eq 'FAIL').Count
    $degradedCount = @($probeSummaries | Where-Object status -eq 'DEGRADED').Count
    $aggregateStatus = Get-ProbeLabWorstStatus -ProbeSummaries @($probeSummaries)
    $worstProbes = @($probeSummaries | Where-Object status -eq $aggregateStatus)
    $aggregateReason = if ($aggregateStatus -eq 'PASS') {
        'all_probes_healthy'
    } elseif ($worstProbes.Count -eq 1) {
        "$($worstProbes[0].probe_id):$($worstProbes[0].reason)"
    } else {
        "$($worstProbes.Count) probes at $aggregateStatus"
    }
    $summary = [pscustomobject][ordered]@{
        schema = $script:ProbeLabSummarySchema; schema_version = 1; run_id = $runId; lab_id = $manifest.lab_id
        mode = 'Monitor'; status = $aggregateStatus; reason = $aggregateReason
        status_counts = [ordered]@{ PASS = @($probeSummaries | Where-Object status -eq 'PASS').Count; DEGRADED = $degradedCount; FAIL = $failedCount }
        started_from_manifest = $resolvedManifestPath; completed_at_utc = [datetime]::UtcNow.ToString('o'); sample_count = $sampleCount
        receipt_path = $ReceiptPath; playerbots_log_path = $resolvedPlayerbotsLog; probes = @($probeSummaries)
        safety = [ordered]@{ bridge_actions = @('list', 'combatlog', 'encounterlog'); mutations = 0; database_access = $false; server_restart = $false; interior_teleport = $false }
    }
    $summary | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $SummaryJsonPath -Encoding utf8NoBOM
    ConvertTo-ProbeLabMarkdownSummary -Summary $summary | Set-Content -LiteralPath $SummaryMarkdownPath -Encoding utf8NoBOM
    Write-LabReceipt -Event 'monitor_complete' -Fields ([ordered]@{ status = $summary.status; summary_json_path = $SummaryJsonPath; summary_markdown_path = $SummaryMarkdownPath })
    return $summary
}

try {
    $result = if ($Mode -eq 'Launch') { Invoke-LabLaunch } else { Invoke-LabMonitor }
    $result | ConvertTo-Json -Depth 30
} catch {
    $errorCode = if ($_.Exception.Message -match '^(Bridge action |Monitor bridge transport)') { 'bridge_error' } else { 'orchestration_error' }
    Write-LabReceipt -Event 'run_failed' -Fields ([ordered]@{ code = $errorCode; detail = $_.Exception.Message })
    throw
}
