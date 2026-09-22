[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$PlanPath = '',
    [string]$OutputPath = '',
    [string]$BridgeScriptPath = '',
    [switch]$Apply,
    [switch]$ConfirmLiveBridge,
    [switch]$AsJson
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-OptionalProperty {
    param(
        [AllowNull()]
        [object]$InputObject,
        [Parameter(Mandatory)]
        [string]$Name,
        [AllowNull()]
        [object]$Default = $null
    )

    if ($null -eq $InputObject) { return $Default }
    $property = $InputObject.PSObject.Properties[$Name]
    if ($null -eq $property -or $null -eq $property.Value) { return $Default }
    return $property.Value
}

function ConvertTo-Boolean {
    param([AllowNull()][object]$Value, [bool]$Default = $false)
    if ($null -eq $Value) { return $Default }
    if ($Value -is [bool]) { return [bool]$Value }
    return "$Value".Trim().ToLowerInvariant() -in @('1', 'true', 'yes', 'on', 'ready')
}

function ConvertTo-Int64OrZero {
    param([AllowNull()][object]$Value)
    if ($null -eq $Value -or [string]::IsNullOrWhiteSpace("$Value")) { return [int64]0 }
    $number = [int64]0
    if ([int64]::TryParse("$Value", [Globalization.NumberStyles]::Integer, [Globalization.CultureInfo]::InvariantCulture, [ref]$number)) { return $number }
    return [int64]0
}

function Read-Plan {
    if (-not [string]::IsNullOrWhiteSpace($PlanPath)) {
        if (-not (Test-Path -LiteralPath $PlanPath -PathType Leaf)) {
            throw "Gathering plan not found: $PlanPath"
        }
        return Get-Content -LiteralPath $PlanPath -Raw | ConvertFrom-Json
    }

    $plannerPath = Join-Path $PSScriptRoot 'gathering-planner.ps1'
    if (-not (Test-Path -LiteralPath $plannerPath -PathType Leaf)) {
        throw "Gathering planner not found: $plannerPath"
    }

    $raw = & $plannerPath -ServerRoot $ServerRoot -AsJson 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
        throw "Gathering planner failed with exit code $LASTEXITCODE."
    }
    try {
        return $raw | ConvertFrom-Json
    } catch {
        throw "Gathering planner did not return JSON: $($_.Exception.Message)"
    }
}

function Get-PlanAssignments {
    param([Parameter(Mandatory)][object]$Plan)

    $teams = @(Get-OptionalProperty -InputObject $Plan -Name 'teams' -Default @())
    $assignments = [System.Collections.Generic.List[object]]::new()
    foreach ($team in $teams) {
        $teamId = Get-OptionalProperty -InputObject $team -Name 'team' -Default ''
        $teamName = Get-OptionalProperty -InputObject $team -Name 'name' -Default $teamId
        foreach ($assignment in @(Get-OptionalProperty -InputObject $team -Name 'assignments' -Default @())) {
            [void]$assignments.Add([pscustomobject]@{
                team = "$teamId"
                team_name = "$teamName"
                slot = ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $assignment -Name 'slot')
                slot_key = "$((Get-OptionalProperty -InputObject $assignment -Name 'slot_key' -Default ''))"
                guid = if ($null -eq (Get-OptionalProperty -InputObject $assignment -Name 'guid')) { $null } else { ConvertTo-Int64OrZero (Get-OptionalProperty -InputObject $assignment -Name 'guid') }
                name = "$((Get-OptionalProperty -InputObject $assignment -Name 'name' -Default ''))"
                role = "$((Get-OptionalProperty -InputObject $assignment -Name 'role' -Default ''))".ToLowerInvariant()
                candidate_type = "$((Get-OptionalProperty -InputObject $assignment -Name 'candidate_type' -Default 'unbound'))"
                planned_profession_one = "$((Get-OptionalProperty -InputObject $assignment -Name 'planned_profession_one' -Default ''))"
                planned_profession_two = "$((Get-OptionalProperty -InputObject $assignment -Name 'planned_profession_two' -Default ''))"
                planned_pair_valid = ConvertTo-Boolean (Get-OptionalProperty -InputObject $assignment -Name 'planned_pair_valid')
                live_ready = ConvertTo-Boolean (Get-OptionalProperty -InputObject $assignment -Name 'live_ready')
                status = "$((Get-OptionalProperty -InputObject $assignment -Name 'status' -Default 'unknown'))"
                blocking_reasons = @((Get-OptionalProperty -InputObject $assignment -Name 'blocking_reasons' -Default @()))
            })
        }
    }
    return @($assignments)
}

function Get-ReadOnlyChatSurface {
    param([Parameter(Mandatory)][object]$Assignment)

    if ($null -eq $Assignment.guid -or $Assignment.candidate_type -eq 'unbound') {
        return $null
    }

    return [ordered]@{
        team = $Assignment.team
        guid = $Assignment.guid
        name = $Assignment.name
        inspection_commands = @('craft ?', 'mail ?', 'bank ?')
        guild_bank_inspection = 'No read-only guild-bank listing action is exposed by the current Playerbots chat action; do not send gb for telemetry.'
        proposed_activation_command = 'nc +gather,+grind,+move random,-follow'
        activation_command_is_proposal = $true
    }
}

function New-ActionProposal {
    param([Parameter(Mandatory)][object]$Assignment)

    $ready = $Assignment.live_ready -and $Assignment.role -eq 'worker' -and $Assignment.planned_pair_valid
    $blockedReasons = [System.Collections.Generic.List[string]]::new()
    foreach ($reason in @($Assignment.blocking_reasons)) {
        if (-not [string]::IsNullOrWhiteSpace("$reason")) { [void]$blockedReasons.Add("$reason") }
    }
    if (-not $Assignment.planned_pair_valid) { [void]$blockedReasons.Add('planned_pair_invalid') }
    if ($Assignment.role -ne 'worker') { [void]$blockedReasons.Add('role_is_not_worker') }
    if (-not $Assignment.live_ready) { [void]$blockedReasons.Add('assignment_not_live_ready') }

    return [ordered]@{
        team = $Assignment.team
        guid = $Assignment.guid
        name = $Assignment.name
        slot = $Assignment.slot
        profession_pair = @($Assignment.planned_profession_one, $Assignment.planned_profession_two)
        ready_for_bridge = $ready
        bridge_steps = if ($ready) {
            @(
                [ordered]@{ order = 1; action = 'activate'; only_if_not_listed = $true; bot_guid = $Assignment.guid }
                [ordered]@{ order = 2; action = 'deploy'; bot_guid = $Assignment.guid; expected_mode = 'worker_gather' }
                [ordered]@{ order = 3; action = 'snapshot'; bot_guid = $Assignment.guid }
            )
        } else { @() }
        blocked_reasons = @($blockedReasons | Select-Object -Unique)
        db_writes = 0
    }
}

function Invoke-BridgeCommand {
    param(
        [Parameter(Mandatory)][ValidateSet('list', 'activate', 'deploy', 'snapshot')][string]$Action,
        [AllowNull()][object]$BotGuid
    )

    if ([string]::IsNullOrWhiteSpace($BridgeScriptPath) -or -not (Test-Path -LiteralPath $BridgeScriptPath -PathType Leaf)) {
        throw "Bridge script not found: $BridgeScriptPath"
    }

    if ($Action -eq 'list') {
        $raw = & $BridgeScriptPath -Action list 2>&1 | Out-String
    } else {
        $raw = & $BridgeScriptPath -Action $Action -BotGuid (ConvertTo-Int64OrZero $BotGuid) 2>&1 | Out-String
    }
    if ($LASTEXITCODE -ne 0) {
        throw "Bridge command '$Action' failed with exit code $LASTEXITCODE."
    }
    try {
        return $raw | ConvertFrom-Json
    } catch {
        throw "Bridge command '$Action' did not return JSON: $($_.Exception.Message)"
    }
}

function Get-ListedGuids {
    param([AllowNull()][object]$ListResult)

    $guids = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($bot in @(Get-OptionalProperty -InputObject $ListResult -Name 'bots' -Default @())) {
        $guid = Get-OptionalProperty -InputObject $bot -Name 'guid' -Default (Get-OptionalProperty -InputObject $bot -Name 'bot_guid')
        if ($null -ne $guid) { [void]$guids.Add("$guid") }
    }
    return ,$guids
}

$plan = Read-Plan
$assignments = @(Get-PlanAssignments -Plan $plan)
$actionProposals = @($assignments | ForEach-Object { New-ActionProposal -Assignment $_ })
$blockedActions = @($actionProposals | Where-Object { -not $_.ready_for_bridge })
$readOnlyChat = @($assignments | ForEach-Object { Get-ReadOnlyChatSurface -Assignment $_ } | Where-Object { $null -ne $_ })
$executions = [System.Collections.Generic.List[object]]::new()
$bridgeCallCount = 0

if ($Apply -and -not $ConfirmLiveBridge) {
    throw '-Apply is guarded. Add -ConfirmLiveBridge to authorize explicit bridge activation.'
}

if ($Apply) {
    $readyActions = @($actionProposals | Where-Object { $_.ready_for_bridge })
    if ($readyActions.Count -gt 0) {
        $listResult = Invoke-BridgeCommand -Action list -BotGuid $null
        $bridgeCallCount++
        $listedGuids = Get-ListedGuids -ListResult $listResult
        foreach ($action in $readyActions) {
            $guidKey = "$($action.guid)"
            if (-not $listedGuids.Contains($guidKey)) {
                $activateResult = Invoke-BridgeCommand -Action activate -BotGuid $action.guid
                $bridgeCallCount++
                [void]$executions.Add([ordered]@{ team = $action.team; guid = $action.guid; action = 'activate'; result = $activateResult })
            }
            $deployResult = Invoke-BridgeCommand -Action deploy -BotGuid $action.guid
            $bridgeCallCount++
            [void]$executions.Add([ordered]@{ team = $action.team; guid = $action.guid; action = 'deploy'; result = $deployResult })
            $snapshotResult = Invoke-BridgeCommand -Action snapshot -BotGuid $action.guid
            $bridgeCallCount++
            [void]$executions.Add([ordered]@{ team = $action.team; guid = $action.guid; action = 'snapshot'; result = $snapshotResult })
        }
    }
}

$result = [ordered]@{
    schema_version = 'gathering-orchestration-v0'
    mode = if ($Apply) { 'bridge_apply_explicit' } else { 'dry_run' }
    generated_utc = [DateTime]::UtcNow.ToString('o')
    dry_run = -not [bool]$Apply
    explicit_live_bridge_confirmation = [bool]$ConfirmLiveBridge
    plan_source = if ([string]::IsNullOrWhiteSpace($PlanPath)) { 'gathering-planner.ps1' } else { $PlanPath }
    planned_actions = @($actionProposals)
    blocked_actions = @($blockedActions)
    read_only_chat_surfaces = @($readOnlyChat)
    executions = @($executions)
    safety = [ordered]@{
        db_writes = 0
        character_state_writes = 0
        bridge_calls = $bridgeCallCount
        live_activation = [bool]$Apply
        chat_commands_sent = 0
        note = if ($Apply) { 'Only explicit bridge activate/deploy/snapshot calls were allowed; no SQL or chat command was sent.' } else { 'No bridge or chat surface was called.' }
    }
}

if (-not [string]::IsNullOrWhiteSpace($OutputPath)) {
    $parent = Split-Path -Parent $OutputPath
    if (-not [string]::IsNullOrWhiteSpace($parent)) {
        [void](New-Item -ItemType Directory -Path $parent -Force)
    }
    $result | ConvertTo-Json -Depth 15 | Set-Content -LiteralPath $OutputPath -Encoding utf8
}

if ($AsJson) {
    $result | ConvertTo-Json -Depth 15 -Compress
    exit 0
}

Write-Output ("Gathering orchestration {0}: {1} planned actions, {2} blocked, {3} bridge executions." -f $result.mode, @($actionProposals).Count, @($blockedActions).Count, @($executions).Count)
