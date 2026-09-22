<#
    Read-only status surface for the persistent economy gatherers.

    This script never activates, deploys, deactivates, moves, recovers, or
    otherwise mutates a bot. It probes the loopback endpoint first and only
    sends the existing read-only list/snapshot requests when the endpoint is
    reachable. It also folds in the latest local gather receipt, proof, and
    read-only telemetry so an offline bridge still leaves useful evidence.
#>
[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$ManifestPath = (Join-Path $PSScriptRoot 'gather-lane-manifest.json'),
    [string]$ControlPath = (Join-Path $PSScriptRoot 'autowow-control.ps1'),
    [ValidateSet('127.0.0.1')][string]$BridgeHost = '127.0.0.1',
    [ValidateRange(1, 65535)][int]$BridgePort = 18787,
    [ValidateRange(100, 120000)][int]$BridgeTimeoutMs = 1000,
    [ValidateRange(0, 1440)][int]$DurationMinutes = 0,
    [ValidateRange(1, 900)][int]$PollSeconds = 60,
    [string]$ReceiptPath = '',
    [string]$StatePath = '',
    [switch]$LibraryOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-OptionalProperty {
    param(
        [AllowNull()][object]$InputObject,
        [Parameter(Mandatory)][string]$Name,
        [AllowNull()][object]$Default = $null
    )

    if ($null -eq $InputObject) { return $Default }
    $property = $InputObject.PSObject.Properties[$Name]
    if ($null -eq $property -or $null -eq $property.Value) { return $Default }
    return $property.Value
}

function Get-FullPathOrDefault {
    param([Parameter(Mandatory)][string]$Path)

    return [IO.Path]::GetFullPath($Path)
}

function Get-PersistentGathererStatusPaths {
    param([Parameter(Mandatory)][string]$Root)

    $statusWork = Join-Path $Root 'work\persistent-gatherer-status'
    $defaultReceipt = Join-Path $Root 'logs\persistent-gatherer-status.jsonl'
    $defaultState = Join-Path $statusWork 'state.json'
    return [ordered]@{
        receipt_path = if ([string]::IsNullOrWhiteSpace($ReceiptPath)) { $defaultReceipt } else { Get-FullPathOrDefault $ReceiptPath }
        state_path = if ([string]::IsNullOrWhiteSpace($StatePath)) { $defaultState } else { Get-FullPathOrDefault $StatePath }
        work_root = $statusWork
        gather_log_root = Join-Path $Root 'logs\gather-lane'
    }
}

function Read-GathererManifest {
    param([Parameter(Mandatory)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Gatherer manifest not found: $Path"
    }
    $manifest = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    if ($manifest.schema -ne 'autowow.gathering-proof.fixture-roster.v1' -or
        $manifest.purpose -ne 'gathering-proof') {
        throw 'Gatherer manifest schema or purpose is invalid.'
    }

    $seen = [System.Collections.Generic.HashSet[uint32]]::new()
    $members = [System.Collections.Generic.List[object]]::new()
    foreach ($member in @($manifest.members)) {
        $guid = [uint64]0
        if (-not [uint64]::TryParse("$(Get-OptionalProperty $member 'guid')", [Globalization.NumberStyles]::Integer,
                [Globalization.CultureInfo]::InvariantCulture, [ref]$guid) -or
            $guid -eq 0 -or $guid -gt [uint32]::MaxValue) {
            throw 'Gatherer manifest contains an invalid GUID.'
        }
        if (-not $seen.Add([uint32]$guid)) { throw "Gatherer manifest repeats GUID $guid." }
        $name = "$(Get-OptionalProperty $member 'name' '')".Trim()
        $profession = "$(Get-OptionalProperty $member 'profession' '')".Trim()
        if ([string]::IsNullOrWhiteSpace($name) -or [string]::IsNullOrWhiteSpace($profession)) {
            throw "Gatherer manifest GUID $guid must have a name and profession."
        }
        [void]$members.Add([ordered]@{
            guid = [uint32]$guid
            name = $name
            profession = $profession
        })
    }
    if ($members.Count -eq 0) { throw 'Gatherer manifest contains no members.' }
    return @($members)
}

function Read-JsonLines {
    param([Parameter(Mandatory)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return @() }
    $records = [System.Collections.Generic.List[object]]::new()
    foreach ($line in Get-Content -LiteralPath $Path) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        try { [void]$records.Add(($line | ConvertFrom-Json)) } catch { }
    }
    return @($records)
}

function Get-LatestGatherReceipt {
    param([Parameter(Mandatory)][string]$LogRoot)

    if (-not (Test-Path -LiteralPath $LogRoot -PathType Container)) { return $null }
    $candidates = [System.Collections.Generic.List[object]]::new()
    foreach ($file in Get-ChildItem -LiteralPath $LogRoot -Filter '*.jsonl' -File) {
        foreach ($record in Read-JsonLines -Path $file.FullName) {
            if ($record.event -ne 'status') { continue }
            $stamp = [DateTimeOffset]$file.LastWriteTimeUtc
            try { $stamp = [DateTimeOffset]::Parse("$($record.utc)") } catch { }
            [void]$candidates.Add([pscustomobject]@{ stamp = $stamp; record = $record; path = $file.FullName })
        }
    }
    return ($candidates | Sort-Object stamp -Descending | Select-Object -First 1)
}

function Get-LatestJsonDocument {
    param(
        [Parameter(Mandatory)][string]$LogRoot,
        [Parameter(Mandatory)][string]$Filter,
        [string]$RequiredProperty = ''
    )

    if (-not (Test-Path -LiteralPath $LogRoot -PathType Container)) { return $null }
    foreach ($file in Get-ChildItem -LiteralPath $LogRoot -Filter $Filter -File | Sort-Object LastWriteTimeUtc -Descending) {
        try {
            $document = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json
            if (-not [string]::IsNullOrWhiteSpace($RequiredProperty) -and
                $null -eq $document.PSObject.Properties[$RequiredProperty]) { continue }
            return [pscustomobject]@{ path = $file.FullName; document = $document }
        } catch {
            continue
        }
    }
    return $null
}

function Test-GathererBridgeEndpoint {
    $client = [System.Net.Sockets.TcpClient]::new()
    try {
        $connect = $client.ConnectAsync($BridgeHost, $BridgePort)
        $reachable = $connect.Wait([Math]::Min($BridgeTimeoutMs, 10000)) -and $client.Connected
        return [ordered]@{
            host = $BridgeHost
            port = $BridgePort
            reachable = [bool]$reachable
            checked_utc = [DateTimeOffset]::UtcNow.ToString('o')
            error = if ($reachable) { $null } else { 'endpoint_unreachable' }
        }
    } catch {
        return [ordered]@{
            host = $BridgeHost
            port = $BridgePort
            reachable = $false
            checked_utc = [DateTimeOffset]::UtcNow.ToString('o')
            error = 'endpoint_probe_failed'
        }
    } finally {
        $client.Dispose()
    }
}

function Invoke-ReadOnlyBridgeJson {
    param(
        [Parameter(Mandatory)][ValidateSet('list', 'snapshot')][string]$Action,
        [uint32]$Guid = 0
    )

    if (-not (Test-Path -LiteralPath $ControlPath -PathType Leaf)) {
        throw 'Read-only bridge control script is missing.'
    }
    $controlArgs = @{
        Action = $Action
        BridgeHost = $BridgeHost
        Port = $BridgePort
        TimeoutMs = $BridgeTimeoutMs
    }
    if ($Action -eq 'snapshot') { $controlArgs.BotGuid = $Guid }
    $raw = @(& $ControlPath @controlArgs)
    if (-not $?) { throw 'Read-only bridge query failed.' }
    $value = ($raw -join "`n") | ConvertFrom-Json
    if (-not (Get-OptionalProperty $value 'ok' $false)) { throw 'Read-only bridge query returned ok=false.' }
    return $value
}

function Get-SafeBridgeStatus {
    param([Parameter(Mandatory)][object[]]$Members)

    $probe = Test-GathererBridgeEndpoint
    $result = [ordered]@{
        endpoint = $probe
        query_status = if ($probe.reachable) { 'not_attempted' } else { 'offline' }
        members = @()
        other_online_guids = @()
        error = $null
    }
    if (-not $probe.reachable) { return $result }

    try {
        $list = Invoke-ReadOnlyBridgeJson -Action list
        $bots = @($list.bots)
        $registered = @($Members.guid)
        $result.query_status = 'read_only_ok'
        $result.other_online_guids = @($bots | Where-Object { [uint32]$_.guid -notin $registered } | ForEach-Object { [uint32]$_.guid })
        $rows = [System.Collections.Generic.List[object]]::new()
        foreach ($member in $Members) {
            $bot = @($bots | Where-Object { [uint32]$_.guid -eq $member.guid } | Select-Object -First 1)
            if ($bot.Count -eq 0) {
                [void]$rows.Add([ordered]@{ guid = $member.guid; name = $member.name; online = $false })
                continue
            }
            $snapshot = Invoke-ReadOnlyBridgeJson -Action snapshot -Guid ([uint32]$member.guid)
            $state = Get-OptionalProperty $snapshot 'bot' $null
            $position = Get-OptionalProperty $state 'position' $null
            [void]$rows.Add([ordered]@{
                guid = $member.guid
                name = $member.name
                online = $true
                paused = [bool](Get-OptionalProperty $bot 'paused' $false)
                group_members = Get-OptionalProperty (Get-OptionalProperty $bot 'group' $null) 'members' 0
                alive = Get-OptionalProperty $state 'alive' $null
                state = Get-OptionalProperty $state 'state' $null
                map = Get-OptionalProperty $position 'map' $null
                gather_route = Get-OptionalProperty $state 'gather_route' $null
            })
        }
        $result.members = @($rows)
    } catch {
        $result.query_status = 'read_only_query_failed'
        $result.error = 'read_only_bridge_query_failed'
    }
    return $result
}

function Get-BridgeTransition {
    param(
        [AllowNull()][string]$PreviousStatus,
        [Parameter(Mandatory)][string]$CurrentStatus
    )

    if ([string]::IsNullOrWhiteSpace($PreviousStatus)) { return 'bridge_status' }
    if ($PreviousStatus -eq 'offline' -and $CurrentStatus -eq 'online') { return 'bridge_reconnected' }
    if ($PreviousStatus -eq 'online' -and $CurrentStatus -eq 'offline') { return 'bridge_disconnected' }
    return 'bridge_status'
}

function Get-UnregisteredWorkers {
    param(
        [AllowNull()][object]$Telemetry,
        [Parameter(Mandatory)][object[]]$Members
    )

    if ($null -eq $Telemetry) { return @() }
    $registered = @($Members.guid)
    return @($Telemetry.members | Where-Object {
        "$($_.role)" -eq 'worker' -and [uint32]$_.guid -notin $registered
    } | ForEach-Object {
        [ordered]@{
            guid = [uint32]$_.guid
            name = "$(Get-OptionalProperty $_ 'name' '')"
            role = "$($_.role)"
            affiliation = "$(Get-OptionalProperty $_ 'affiliation' '')"
            declared_professions = @((Get-OptionalProperty $_ 'declared_professions' @()))
        }
    })
}

function Get-GathererStatusSummary {
    param([Parameter(Mandatory)][hashtable]$Paths)

    $members = Read-GathererManifest -Path $ManifestPath
    $latestReceipt = Get-LatestGatherReceipt -LogRoot $Paths.gather_log_root
    $latestProof = Get-LatestJsonDocument -LogRoot $Paths.gather_log_root -Filter '*-proof.json'
    $latestTelemetry = Get-LatestJsonDocument -LogRoot (Join-Path $ServerRoot 'logs') -Filter 'gathering-telemetry*.json' -RequiredProperty 'members'
    $bridge = Get-SafeBridgeStatus -Members $members
    $bridgeStatus = if ($bridge.endpoint.reachable) { 'online' } else { 'offline' }

    $prior = $null
    if (Test-Path -LiteralPath $Paths.state_path -PathType Leaf) {
        try { $prior = Get-Content -LiteralPath $Paths.state_path -Raw | ConvertFrom-Json } catch { $prior = $null }
    }

    $telemetry = if ($null -eq $latestTelemetry) { $null } else { $latestTelemetry.document }
    $receiptRecord = if ($null -eq $latestReceipt) { $null } else { $latestReceipt.record }
    $proof = if ($null -eq $latestProof) { $null } else { $latestProof.document }
    $rows = [System.Collections.Generic.List[object]]::new()
    foreach ($member in $members) {
        $receiptRow = if ($null -eq $receiptRecord) { $null } else { ($receiptRecord.details.cohort | Where-Object { [uint32]$_.guid -eq $member.guid } | Select-Object -First 1) }
        $proofRow = if ($null -eq $proof) { $null } else { ($proof.member_proofs | Where-Object { [uint32]$_.guid -eq $member.guid } | Select-Object -First 1) }
        $telemetryMember = if ($null -eq $telemetry) { $null } else { ($telemetry.members | Where-Object { [uint32]$_.guid -eq $member.guid } | Select-Object -First 1) }
        $liveRow = ($bridge.members | Where-Object { [uint32]$_.guid -eq $member.guid } | Select-Object -First 1)
        $skills = if ($null -eq $telemetry) { @() } else { @($telemetry.skills | Where-Object { [uint32]$_.guid -eq $member.guid -and "$($_.kind)" -eq 'primary' } | ForEach-Object { [ordered]@{ name = $_.name; value = $_.value; max = $_.max } }) }
        $receiptRoute = if ($null -eq $receiptRow) { $null } else { Get-OptionalProperty $receiptRow 'gather_route' $null }
        $receiptCandidate = if ($null -eq $receiptRoute) { $null } else { Get-OptionalProperty $receiptRoute 'candidate' $null }
        [void]$rows.Add([ordered]@{
            guid = $member.guid
            name = $member.name
            registered_profession = $member.profession
            telemetry_role = if ($null -eq $telemetryMember) { $null } else { $telemetryMember.role }
            telemetry_online = if ($null -eq $telemetryMember) { $null } else { $telemetryMember.online }
            declared_professions = if ($null -eq $telemetryMember) { @() } else { @($telemetryMember.declared_professions) }
            primary_skills = $skills
            latest_receipt = if ($null -eq $receiptRow) { $null } else { [ordered]@{ online = $receiptRow.online; map = $receiptRow.map; alive = $receiptRow.alive; route_status = Get-OptionalProperty $receiptRoute 'status' $null; route_event = Get-OptionalProperty $receiptRoute 'event' $null; candidate = Get-OptionalProperty $receiptCandidate 'name' $null } }
            latest_proof = if ($null -eq $proofRow) { $null } else { [ordered]@{ status = $proofRow.status; skill_delta = $proofRow.expected_profession_skill.value_delta; positive_material_units = $proofRow.positive_material_units; negative_material_units = $proofRow.negative_material_units; reasons = @($proofRow.reasons) } }
            live_bridge = $liveRow
        })
    }

    $event = Get-BridgeTransition -PreviousStatus (Get-OptionalProperty $prior 'bridge_status' '') -CurrentStatus $bridgeStatus
    $summary = [ordered]@{
        schema = 'autowow.persistent-gatherer-status.v1'
        schema_version = 1
        sampled_utc = [DateTimeOffset]::UtcNow.ToString('o')
        event = $event
        bridge_status = $bridgeStatus
        bridge = $bridge
        registered_gatherers = @($rows)
        unregistered_worker_rows = @(Get-UnregisteredWorkers -Telemetry $telemetry -Members $members)
        latest_evidence = [ordered]@{
            receipt_path = if ($null -eq $latestReceipt) { $null } else { $latestReceipt.path }
            proof_path = if ($null -eq $latestProof) { $null } else { $latestProof.path }
            proof_status = if ($null -eq $proof) { 'unavailable' } else { $proof.status }
            telemetry_path = if ($null -eq $latestTelemetry) { $null } else { $latestTelemetry.path }
        }
        safety = [ordered]@{
            read_only = $true
            db_writes = 0
            bridge_mutations = 0
            bridge_read_only_queries = if ($bridge.query_status -eq 'read_only_ok') { 1 + @($bridge.members).Count } else { 0 }
            server_start_stop_restart_actions = 0
            chat_commands_sent = 0
            secrets_recorded = $false
        }
    }
    return $summary
}

function Write-GathererStatusSample {
    param([Parameter(Mandatory)][hashtable]$Paths)

    $summary = Get-GathererStatusSummary -Paths $Paths
    [void](New-Item -ItemType Directory -Path $Paths.work_root -Force)
    [void](New-Item -ItemType Directory -Path (Split-Path -Parent $Paths.receipt_path) -Force)
    $summary | ConvertTo-Json -Depth 20 -Compress | Add-Content -LiteralPath $Paths.receipt_path -Encoding utf8
    [ordered]@{ bridge_status = $summary.bridge_status; event = $summary.event; receipt_path = $Paths.receipt_path; state_path = $Paths.state_path } |
        ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $Paths.state_path -Encoding utf8
    return $summary
}

if ($LibraryOnly) { return }

$resolvedRoot = Get-FullPathOrDefault $ServerRoot
$ServerRoot = $resolvedRoot
$Paths = Get-PersistentGathererStatusPaths -Root $resolvedRoot
$sample = Write-GathererStatusSample -Paths $Paths
$deadline = [DateTimeOffset]::UtcNow.AddMinutes($DurationMinutes)
while ($DurationMinutes -gt 0 -and [DateTimeOffset]::UtcNow -lt $deadline) {
    Start-Sleep -Seconds $PollSeconds
    $sample = Write-GathererStatusSample -Paths $Paths
}
$sample | ConvertTo-Json -Depth 20 -Compress
