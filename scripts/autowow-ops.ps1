<#
.SYNOPSIS
    One guarded operator surface for persistent AutoWoW raiders and questers.
.DESCRIPTION
    Plan is the default and is non-mutating. Runtime-changing actions require -Apply. This script
    composes the existing specialist tools; it does not duplicate bridge, build, or director logic.
#>
[CmdletBinding()]
param(
    [ValidateSet('Plan','Status','Dashboard','Raiders','QuestersStart','QuestersStop','QuestersStatus','DevCycle','FailureBundle')]
    [string]$Action = 'Plan',
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$ProbeManifestPath = (Join-Path $PSScriptRoot 'fixtures\probe-lab-live-three-lane.json'),
    [string]$ProbeSummaryPath = '',
    [uint32[]]$LeaderGuid = @(),
    [ValidateRange(1,1440)][int]$DurationMinutes = 480,
    [ValidateRange(10,300)][int]$QuestPollSeconds = 20,
    [ValidateRange(30,900)][int]$NoProgressSeconds = 120,
    [ValidateRange(1,5)][int]$MaxRecoveriesPerQuest = 2,
    [ValidateRange(30,86400)][int]$RaidMonitorSeconds = 600,
    [ValidateRange(1,60)][int]$DashboardPollSeconds = 5,
    [switch]$Watch,
    [switch]$Apply
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
$stateRoot = Join-Path $ServerRoot 'work\autowow-ops'
$questStatePath = Join-Path $stateRoot 'quest-director.json'
$scripts = [ordered]@{
    dashboard = Join-Path $PSScriptRoot 'bot-fleet-dashboard.ps1'
    raiders = Join-Path $PSScriptRoot 'probe-lab-run.ps1'
    questers = Join-Path $PSScriptRoot 'league-simulation-director.ps1'
    league = Join-Path $PSScriptRoot 'league-simulation.ps1'
    dev_cycle = Join-Path $PSScriptRoot 'phase1-dev-cycle.ps1'
    failure_bundle = Join-Path $PSScriptRoot 'probe-failure-bundle.ps1'
}

function Assert-OpsScript([string]$Path, [string]$Name) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "$Name is not installed: $Path" }
}

function Get-QuestDirectorState {
    if (-not (Test-Path -LiteralPath $questStatePath -PathType Leaf)) {
        return [pscustomobject][ordered]@{ status = 'STOPPED'; process_id = $null; state_path = $questStatePath }
    }
    try { $state = Get-Content -LiteralPath $questStatePath -Raw | ConvertFrom-Json }
    catch { return [pscustomobject][ordered]@{ status = 'STALE'; process_id = $null; state_path = $questStatePath; reason = 'invalid_state_file' } }
    $process = Get-Process -Id ([int]$state.process_id) -ErrorAction SilentlyContinue
    return [pscustomobject][ordered]@{
        status = if ($process) { 'RUNNING' } else { 'STALE' }
        process_id = [int]$state.process_id
        started_utc = $state.started_utc
        leaders = @($state.leaders)
        receipt_path = $state.receipt_path
        stdout = $state.stdout
        stderr = $state.stderr
        state_path = $questStatePath
    }
}

function Get-OpsPlan {
    [pscustomobject][ordered]@{
        schema = 'autowow.ops.plan.v1'
        default_safe = $true
        server_root = $ServerRoot
        persistent_questers = [ordered]@{
            leaders = @($LeaderGuid)
            duration_minutes = $DurationMinutes
            poll_seconds = $QuestPollSeconds
            no_progress_seconds = $NoProgressSeconds
            playerbots_log = (Join-Path $ServerRoot 'logs\phase1-runtime\Playerbots.log')
            apply_required = $true
        }
        raiders = [ordered]@{
            manifest = $ProbeManifestPath
            monitor_seconds = $RaidMonitorSeconds
            apply_required = $true
        }
        read_only = @('Status','Dashboard','QuestersStatus')
        guarded_mutations = @('Raiders','QuestersStart','QuestersStop','DevCycle')
        specialist_tools = $scripts
    }
}

if ($Action -eq 'Plan') { Get-OpsPlan | ConvertTo-Json -Depth 8; return }
if ($Action -in @('Raiders','QuestersStart','QuestersStop','DevCycle') -and -not $Apply) {
    throw "$Action requires -Apply. Run -Action Plan first for the non-mutating plan."
}
if ($Apply -and $Action -notin @('Raiders','QuestersStart','QuestersStop','DevCycle')) {
    throw "-Apply is not valid for read-only action $Action."
}

switch ($Action) {
    'Status' {
        $worldPid = (& wsl.exe -d Ubuntu-24.04 -u root -- pgrep -x worldserver 2>$null | Select-Object -First 1)
        $quest = Get-QuestDirectorState
        $bridge = $null
        try {
            Assert-OpsScript (Join-Path $PSScriptRoot 'autowow-control.ps1') 'Bridge client'
            $bridge = (& (Join-Path $PSScriptRoot 'autowow-control.ps1') -Action list | ConvertFrom-Json)
        } catch { $bridge = [pscustomobject]@{ ok = $false; error = $_.Exception.Message } }
        [pscustomobject][ordered]@{
            schema = 'autowow.ops.status.v1'
            worldserver = if ($worldPid) { 'RUNNING' } else { 'STOPPED' }
            worldserver_pid = if ($worldPid) { [int]$worldPid } else { $null }
            bridge_ok = [bool]$bridge.ok
            online_bots = if ($bridge.ok) { @($bridge.bots).Count } else { 0 }
            quest_director = $quest
        } | ConvertTo-Json -Depth 8
    }
    'Dashboard' {
        Assert-OpsScript $scripts.dashboard 'Fleet dashboard'
        $args = @{ Mode = if ($Watch) { 'Watch' } else { 'Snapshot' }; ProbeManifestPath = $ProbeManifestPath; PollSeconds = $DashboardPollSeconds }
        if ($LeaderGuid.Count) { $args.LeaderGuid = [uint32[]]$LeaderGuid }
        & $scripts.dashboard @args
    }
    'Raiders' {
        Assert-OpsScript $scripts.raiders 'Probe Lab runner'
        & $scripts.raiders -Mode LaunchThenMonitor -Apply -ManifestPath $ProbeManifestPath `
            -DurationSeconds $RaidMonitorSeconds -PlayerbotsLogPath (Join-Path $ServerRoot 'logs\phase1-runtime\Playerbots.log')
    }
    'QuestersStart' {
        Assert-OpsScript $scripts.questers 'Quest director'
        Assert-OpsScript $scripts.league 'League simulation control'
        $prior = Get-QuestDirectorState
        if ($prior.status -eq 'RUNNING') { throw "Quest director is already running with PID $($prior.process_id)." }
        # Activate/form only the two persistent quest parties while preserving any concurrent
        # dungeon/raid lab roster. Existing quest-state rows prevent starter-zone restaging.
        & $scripts.league -Action launch -ServerRoot $ServerRoot -PreserveOtherFleets | Out-Null
        New-Item -ItemType Directory -Path $stateRoot,(Join-Path $ServerRoot 'logs') -Force | Out-Null
        if (Test-Path -LiteralPath $questStatePath) { Remove-Item -LiteralPath $questStatePath -Force }
        $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
        $receipt = Join-Path $ServerRoot "leagues\results\quest-director-ops-$stamp.jsonl"
        $stdout = Join-Path $ServerRoot "logs\quest-director-ops-$stamp-stdout.log"
        $stderr = Join-Path $ServerRoot "logs\quest-director-ops-$stamp-stderr.log"
        $arguments = @('-NoProfile','-File',$scripts.questers,'-ServerRoot',$ServerRoot,
            '-DurationMinutes',$DurationMinutes,'-ScoutEverySeconds',$QuestPollSeconds,
            '-NoProgressSeconds',$NoProgressSeconds,'-MaxRecoveriesPerQuest',$MaxRecoveriesPerQuest,
            '-PlayerbotsLogPath',(Join-Path $ServerRoot 'logs\phase1-runtime\Playerbots.log'),'-ReceiptPath',$receipt)
        if ($LeaderGuid.Count) { $arguments += '-LeaderCsv'; $arguments += ($LeaderGuid -join ',') }
        $process = Start-Process -FilePath (Get-Command pwsh.exe -ErrorAction Stop).Source -ArgumentList $arguments `
            -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
        $state = [ordered]@{ schema='autowow.ops.quest-director-state.v1'; process_id=$process.Id; started_utc=[datetime]::UtcNow.ToString('o'); leaders=@($LeaderGuid); receipt_path=$receipt; stdout=$stdout; stderr=$stderr }
        New-Item -ItemType Directory -Path (Split-Path -Parent $receipt) -Force | Out-Null
        [System.IO.File]::WriteAllText($questStatePath, ($state | ConvertTo-Json -Depth 5), [System.Text.UTF8Encoding]::new($false))
        [pscustomobject]$state | ConvertTo-Json -Depth 5
    }
    'QuestersStop' {
        $state = Get-QuestDirectorState
        if ($state.status -eq 'RUNNING') { Stop-Process -Id $state.process_id -Force }
        if (Test-Path -LiteralPath $questStatePath) { Remove-Item -LiteralPath $questStatePath -Force }
        [pscustomobject][ordered]@{ status='STOPPED'; previous_status=$state.status; process_id=$state.process_id } | ConvertTo-Json
    }
    'QuestersStatus' { Get-QuestDirectorState | ConvertTo-Json -Depth 6 }
    'DevCycle' {
        Assert-OpsScript $scripts.dev_cycle 'Phase 1 development cycle'
        & $scripts.dev_cycle -Action Deploy -Apply -ServerRoot $ServerRoot
    }
    'FailureBundle' {
        Assert-OpsScript $scripts.failure_bundle 'Failure bundler'
        if ([string]::IsNullOrWhiteSpace($ProbeSummaryPath)) { throw 'FailureBundle requires -ProbeSummaryPath.' }
        & $scripts.failure_bundle -InputPath $ProbeSummaryPath
    }
}
