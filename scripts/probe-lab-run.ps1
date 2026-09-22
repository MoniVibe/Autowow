<#
.SYNOPSIS
    One-command Probe Lab entry point for planning, monitoring, or launch-then-monitor.
.DESCRIPTION
    This wrapper composes the existing probe-lab.ps1 contract. Plan is the default. The
    combined mode is intentionally apply-gated and delegates every operation to the existing
    launcher and read-only monitor.
#>
[CmdletBinding()]
param(
    [ValidateSet('Plan', 'LaunchThenMonitor', 'Monitor')][string]$Mode = 'Plan',
    [Parameter(Mandatory)][string]$ManifestPath,
    [switch]$Apply,
    [ValidateSet('127.0.0.1')][string]$BridgeHost = '127.0.0.1',
    [ValidateRange(1, 65535)][int]$Port = 18787,
    [ValidateRange(1000, 120000)][int]$TimeoutMs = 5000,
    [ValidateRange(5, 600)][int]$OnlineTimeoutSeconds = 90,
    [ValidateRange(5, 1800)][int]$AdmissionTimeoutSeconds = 180,
    [ValidateRange(1, 86400)][int]$DurationSeconds = 300,
    [ValidateRange(1, 60)][int]$PollSeconds = 5,
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
$probeLabPath = Join-Path $PSScriptRoot 'probe-lab.ps1'
if (-not (Test-Path -LiteralPath $probeLabPath -PathType Leaf)) { throw "Missing Probe Lab script: $probeLabPath" }
if ($Mode -eq 'LaunchThenMonitor' -and -not $Apply) { throw 'LaunchThenMonitor requires -Apply.' }
if ($Mode -ne 'LaunchThenMonitor' -and $Apply) { throw "-Apply is only valid with -Mode LaunchThenMonitor." }

$common = @{
    ManifestPath = $ManifestPath; BridgeHost = $BridgeHost; Port = $Port; TimeoutMs = $TimeoutMs
    OnlineTimeoutSeconds = $OnlineTimeoutSeconds; AdmissionTimeoutSeconds = $AdmissionTimeoutSeconds
    DurationSeconds = $DurationSeconds; PollSeconds = $PollSeconds; StallSeconds = $StallSeconds
    CohesionRadius = $CohesionRadius; ProgressEpsilon = $ProgressEpsilon; PlayerbotsLogPath = $PlayerbotsLogPath
    ServerRoot = $ServerRoot; ControlScriptPath = $ControlScriptPath
}
if ($ProbeId.Count -gt 0) { $common.ProbeId = @($ProbeId) }

if ($Mode -eq 'Plan') {
    & $probeLabPath -Mode Plan @common
    return
}

$monitorArguments = @{} + $common
$monitorArguments.Mode = 'Monitor'
if (-not [string]::IsNullOrWhiteSpace($ReceiptPath)) { $monitorArguments.ReceiptPath = $ReceiptPath }
if (-not [string]::IsNullOrWhiteSpace($SummaryJsonPath)) { $monitorArguments.SummaryJsonPath = $SummaryJsonPath }
if (-not [string]::IsNullOrWhiteSpace($SummaryMarkdownPath)) { $monitorArguments.SummaryMarkdownPath = $SummaryMarkdownPath }

if ($Mode -eq 'Monitor') {
    & $probeLabPath @monitorArguments
    return
}

# Launch gets its own generated receipt; supplied output paths belong to the monitor phase.
$launchArguments = @{} + $common
$launchArguments.Mode = 'Launch'
$launchArguments.Apply = $true
$launchText = & $probeLabPath @launchArguments | Out-String
$launch = $launchText | ConvertFrom-Json
$launchedProbeIds = @($launch.probes | Where-Object status -eq 'LAUNCHED' | ForEach-Object { [string]$_.probe_id })
if ($launchedProbeIds.Count -gt 0) {
    # A partial launch must never monitor idle or stale state for probes which failed to launch.
    $monitorArguments.ProbeId = $launchedProbeIds
    $monitorText = & $probeLabPath @monitorArguments | Out-String
    $monitor = $monitorText | ConvertFrom-Json
} else {
    $monitor = [pscustomobject][ordered]@{
        schema = 'autowow.probe-lab.summary.v1'
        mode = 'Monitor'
        status = 'NOT_RUN'
        reason = 'no_probes_launched'
        probes = @()
    }
}
[pscustomobject][ordered]@{
    schema = 'autowow.probe-lab.wrapper.v1'
    mode = 'LaunchThenMonitor'
    dry_run = $false
    launch = $launch
    monitor = $monitor
} | ConvertTo-Json -Depth 30
