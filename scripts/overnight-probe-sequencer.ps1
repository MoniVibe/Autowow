<#
.SYNOPSIS
    Durable, apply-gated scheduler for exact Probe Lab manifest/probe pairs.
.DESCRIPTION
    Plan is read-only. Apply serializes Probe Lab Launch calls and starts bounded,
    read-only Monitor children without waiting for them. This file never calls the
    bridge directly, writes a database, teleports, or kills generic processes.
#>
[CmdletBinding()]
param(
    [ValidateSet('Plan','Apply')][string]$Mode = 'Plan',
    [string]$QueuePath = '',
    [string]$StatePath = '',
    [string]$ReceiptPath = '',
    [ValidateRange(1,64)][int]$MaxMonitors = 4,
    [string]$ProbeLabPath = (Join-Path $PSScriptRoot 'probe-lab.ps1'),
    [string[]]$ProtectionManifestPath = @(),
    [switch]$StopRecordedMonitors
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$script:SequencerSchema = 'autowow.overnight-probe-sequencer.v1'
$script:ProtectedQuestGuids = [uint32[]](7,12,13,18,20,10,11,14,15,19)
$script:ProtectedGatherGuids = [uint32[]](3,6,49)
$script:DefaultMonitorDurationSeconds = 300
$script:DefaultMonitorPollSeconds = 5
$script:DefaultMonitorStallSeconds = 60

function Resolve-ExistingFile([string]$Path, [string]$Label) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Missing $Label`: $Path" }
    (Resolve-Path -LiteralPath $Path).Path
}

function Get-OptionalQueueInt {
    param([Parameter(Mandatory)]$Object, [Parameter(Mandatory)][string]$Name, [Parameter(Mandatory)][int]$Default, [Parameter(Mandatory)][int]$Minimum, [Parameter(Mandatory)][int]$Maximum)
    $property = $Object.PSObject.Properties[$Name]
    $value = if ($null -eq $property -or $null -eq $property.Value) { $Default } else { [int]$property.Value }
    if ($value -lt $Minimum -or $value -gt $Maximum) { throw "Queue entry field '$Name' must be between $Minimum and $Maximum." }
    $value
}

function Read-SequencerQueue {
    param([Parameter(Mandatory)][string]$Path)
    $root = Get-Content -LiteralPath (Resolve-ExistingFile $Path 'queue') -Raw | ConvertFrom-Json
    $entries = @($root.entries)
    if ($entries.Count -eq 0) { throw 'Queue has no entries.' }
    $seenIds = [Collections.Generic.HashSet[string]]::new()
    $seenGuids = [Collections.Generic.HashSet[uint32]]::new()
    $normalized = foreach ($entry in $entries) {
        $manifest = Resolve-ExistingFile ([string]$entry.manifest_path) 'manifest'
        $probeId = [string]$entry.probe_id
        if ([string]::IsNullOrWhiteSpace($probeId)) { throw 'Queue entry has an empty probe_id.' }
        if (-not $seenIds.Add("$manifest`n$probeId")) { throw "Duplicate manifest+ProbeId entry: $manifest / $probeId" }
        $m = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
        $probe = @($m.probes | Where-Object { [string]$_.id -eq $probeId })
        if ($probe.Count -ne 1) { throw "Manifest '$manifest' must contain exactly one ProbeId '$probeId'." }
        $guids = @($probe[0].members | ForEach-Object { [uint32]$_.guid })
        foreach ($guid in $guids) {
            if (-not $seenGuids.Add($guid)) { throw "Duplicate GUID $guid across queued probes." }
            if ($guid -in $script:ProtectedQuestGuids) { throw "Protected persistent quester GUID $guid is in queued probe '$probeId'." }
            if ($guid -in $script:ProtectedGatherGuids) { throw "Protected gather GUID $guid is in queued probe '$probeId'." }
        }
        $declaredProtected = @()
        if ($null -ne ($m.PSObject.Properties['protected_guids'])) { $declaredProtected += @($m.protected_guids) }
        if ($null -ne ($m.PSObject.Properties['protectedGuids'])) { $declaredProtected += @($m.protectedGuids) }
        foreach ($guid in $declaredProtected) {
            if ([uint32]$guid -in $script:ProtectedQuestGuids -or [uint32]$guid -in $script:ProtectedGatherGuids) {
                throw "Manifest '$manifest' declares a protected GUID $guid; refusing closed-world scheduling."
            }
        }
        [pscustomobject][ordered]@{
            manifest_path=$manifest; probe_id=$probeId; guids=$guids; lab_id=[string]$m.lab_id
            duration_seconds=(Get-OptionalQueueInt $entry 'duration_seconds' $script:DefaultMonitorDurationSeconds 1 86400)
            poll_seconds=(Get-OptionalQueueInt $entry 'poll_seconds' $script:DefaultMonitorPollSeconds 1 60)
            stall_seconds=(Get-OptionalQueueInt $entry 'stall_seconds' $script:DefaultMonitorStallSeconds 5 3600)
        }
    }
    [pscustomobject][ordered]@{ schema=$script:SequencerSchema; queue_path=(Resolve-Path $Path).Path; entries=@($normalized) }
}

function Test-MonitorIdentity {
    param([Parameter(Mandatory)]$Process, [Parameter(Mandatory)][string]$ScriptPath,
          [Parameter(Mandatory)][string]$ManifestPath, [Parameter(Mandatory)][string]$ProbeId,
          [int]$DurationSeconds = 300, [int]$PollSeconds = 5, [int]$StallSeconds = 60)
    $command = [string]$Process.CommandLine
    if ([string]::IsNullOrWhiteSpace($command)) { return $false }
    if ($command -notmatch '(?i)(^|\s|["''])-Mode\s+Monitor(\s|$)') { return $false }
    if ($command -notmatch [regex]::Escape((Resolve-Path $ScriptPath).Path)) { return $false }
    if ($command -notmatch [regex]::Escape((Resolve-Path $ManifestPath).Path)) { return $false }
    $getArgument = {
        param([string]$Name)
        $match = [regex]::Match($command, '(?i)(^|\s|["''])-' + $Name + '\s+(?:"(?<quoted>[^"]+)"|(?<bare>[^\s]+))(?:\s|$)')
        if (-not $match.Success) { return $null }
        if ($match.Groups['quoted'].Success) { return $match.Groups['quoted'].Value }
        $match.Groups['bare'].Value
    }
    ([string](& $getArgument 'ProbeId')) -ceq $ProbeId -and
        [int](& $getArgument 'DurationSeconds') -eq $DurationSeconds -and
        [int](& $getArgument 'PollSeconds') -eq $PollSeconds -and
        [int](& $getArgument 'StallSeconds') -eq $StallSeconds
}

function Ensure-SequencerProperty {
    param([Parameter(Mandatory)]$Object, [Parameter(Mandatory)][string]$Name, $DefaultValue)
    if ($null -eq $Object.PSObject.Properties[$Name]) {
        $Object | Add-Member -MemberType NoteProperty -Name $Name -Value $DefaultValue
    }
}

function ConvertTo-QuotedProcessArgument([string]$Value) {
    '"' + $Value.Replace('"', '\"') + '"'
}

function New-MonitorArgumentList {
    param([Parameter(Mandatory)]$Entry)
    @('-NoProfile', '-File', (ConvertTo-QuotedProcessArgument $script:ProbeLabPath), '-Mode', 'Monitor',
      '-ManifestPath', (ConvertTo-QuotedProcessArgument $Entry.manifest_path), '-ProbeId', (ConvertTo-QuotedProcessArgument $Entry.probe_id),
      '-DurationSeconds', [string]$Entry.duration_seconds, '-PollSeconds', [string]$Entry.poll_seconds, '-StallSeconds', [string]$Entry.stall_seconds)
}

function Get-ExactMonitorProcesses {
    param([Parameter(Mandatory)]$Entry, [scriptblock]$ProcessProvider = { Get-CimInstance Win32_Process -Filter "Name='pwsh.exe' OR Name='powershell.exe'" })
    @(& $ProcessProvider | Where-Object { Test-MonitorIdentity $_ $script:ProbeLabPath $Entry.manifest_path $Entry.probe_id $Entry.duration_seconds $Entry.poll_seconds $Entry.stall_seconds })
}

function Write-SequencerReceipt {
    param([string]$Path, [string]$Event, [hashtable]$Fields = @{})
    $row = [ordered]@{ schema=$script:SequencerSchema; timestamp_utc=[datetime]::UtcNow.ToString('o'); event=$Event }
    foreach ($key in $Fields.Keys) { $row[$key] = $Fields[$key] }
    Add-Content -LiteralPath $Path -Value ($row | ConvertTo-Json -Compress -Depth 30) -Encoding utf8NoBOM
}

function New-SequencerPlan {
    param([Parameter(Mandatory)]$Queue, [int]$MaxMonitors)
    [pscustomobject][ordered]@{ schema=$script:SequencerSchema; mode='Plan'; dry_run=$true; max_monitors=$MaxMonitors; entries=@($Queue.entries | ForEach-Object {
        [ordered]@{ manifest_path=$_.manifest_path; probe_id=$_.probe_id; guids=@($_.guids); duration_seconds=(Get-OptionalQueueInt $_ 'duration_seconds' $script:DefaultMonitorDurationSeconds 1 86400); poll_seconds=(Get-OptionalQueueInt $_ 'poll_seconds' $script:DefaultMonitorPollSeconds 1 60); stall_seconds=(Get-OptionalQueueInt $_ 'stall_seconds' $script:DefaultMonitorStallSeconds 5 3600); launch='probe-lab.ps1 -Mode Launch -Apply'; monitor='probe-lab.ps1 -Mode Monitor'; mutations_in_plan=0 }
    }) }
}

function Invoke-OvernightProbeSequencer {
    param([Parameter(Mandatory)]$Queue, [Parameter(Mandatory)][string]$StatePath,
          [Parameter(Mandatory)][string]$ReceiptPath, [int]$MaxMonitors,
          [scriptblock]$LaunchInvoker, [scriptblock]$ProcessProvider,
          [scriptblock]$MonitorStarter)
    $state = if (Test-Path -LiteralPath $StatePath) { Get-Content $StatePath -Raw | ConvertFrom-Json } else { [pscustomobject][ordered]@{ schema=$script:SequencerSchema; entries=@(); monitors=@(); updated_utc=$null } }
    Ensure-SequencerProperty $state 'schema' $script:SequencerSchema
    Ensure-SequencerProperty $state 'entries' @()
    Ensure-SequencerProperty $state 'monitors' @()
    Ensure-SequencerProperty $state 'updated_utc' $null
    if (-not $LaunchInvoker) { $LaunchInvoker = { param($e) & $script:ProbeLabPath -Mode Launch -Apply -ManifestPath $e.manifest_path -ProbeId $e.probe_id | Out-String | ConvertFrom-Json } }
    if (-not $ProcessProvider) { $ProcessProvider = { Get-CimInstance Win32_Process -Filter "Name='pwsh.exe' OR Name='powershell.exe'" } }
    if (-not $MonitorStarter) { $MonitorStarter = { param($e) Start-Process -WindowStyle Hidden -PassThru -FilePath (Get-Command pwsh -ErrorAction SilentlyContinue).Source -ArgumentList (New-MonitorArgumentList $e) } }
    $active = @($state.monitors | Where-Object { $null -ne $_.PSObject.Properties['pid'] -and $_.pid -and (Get-Process -Id ([int]$_.pid) -ErrorAction SilentlyContinue) }).Count
    foreach ($entry in @($Queue.entries)) {
        $old = @($state.entries | Where-Object { $_.manifest_path -eq $entry.manifest_path -and $_.probe_id -eq $entry.probe_id }) | Select-Object -First 1
        if (-not $old) { $old = [pscustomobject][ordered]@{ manifest_path=$entry.manifest_path; probe_id=$entry.probe_id; launch_status='PENDING'; launch_result=$null; monitor_pid=$null; monitor_adopted=$false }; $state.entries += $old }
        Ensure-SequencerProperty $old 'launch_status' 'PENDING'
        Ensure-SequencerProperty $old 'launch_result' $null
        Ensure-SequencerProperty $old 'monitor_pid' $null
        Ensure-SequencerProperty $old 'monitor_adopted' $false
        Ensure-SequencerProperty $old 'duration_seconds' $entry.duration_seconds
        Ensure-SequencerProperty $old 'poll_seconds' $entry.poll_seconds
        Ensure-SequencerProperty $old 'stall_seconds' $entry.stall_seconds
        $old.duration_seconds = $entry.duration_seconds
        $old.poll_seconds = $entry.poll_seconds
        $old.stall_seconds = $entry.stall_seconds
        if ($old.launch_status -ne 'LAUNCHED') {
            $old.launch_status = 'LAUNCHING'
            $state | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $StatePath -Encoding utf8NoBOM
            $result = & $LaunchInvoker $entry
            $old.launch_status = if (@($result.probes | Where-Object status -eq 'LAUNCHED').Count -gt 0) { 'LAUNCHED' } else { 'FAILED' }
            $old.launch_result = $result
            Write-SequencerReceipt $ReceiptPath 'launch_complete' @{ manifest_path=$entry.manifest_path; probe_id=$entry.probe_id; status=$old.launch_status }
        }
        if ($old.launch_status -ne 'LAUNCHED') { continue }
        $existing = @(Get-ExactMonitorProcesses $entry -ProcessProvider $ProcessProvider)
        if ($existing.Count -gt 0) { $old.monitor_pid = [int]$existing[0].ProcessId; $old.monitor_adopted = $true; continue }
        if ($active -ge $MaxMonitors) { continue }
        $child = & $MonitorStarter $entry
        $old.monitor_pid = [int]$child.Id; $old.monitor_adopted = $false; ++$active
        $state.monitors += [pscustomobject][ordered]@{ pid=[int]$child.Id; manifest_path=$entry.manifest_path; probe_id=$entry.probe_id; duration_seconds=$entry.duration_seconds; poll_seconds=$entry.poll_seconds; stall_seconds=$entry.stall_seconds; started_utc=[datetime]::UtcNow.ToString('o'); adopted=$false }
        Write-SequencerReceipt $ReceiptPath 'monitor_started' @{ pid=[int]$child.Id; manifest_path=$entry.manifest_path; probe_id=$entry.probe_id }
        $state | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $StatePath -Encoding utf8NoBOM
    }
    $state.updated_utc = [datetime]::UtcNow.ToString('o')
    $state | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $StatePath -Encoding utf8NoBOM
    $state
}

if ($env:AUTOWOW_OVERNIGHT_PROBE_SEQUENCER_TESTS -ne '1') {
    if ([string]::IsNullOrWhiteSpace($QueuePath)) { throw 'QueuePath is required.' }
    $script:ProbeLabPath = Resolve-ExistingFile $ProbeLabPath 'Probe Lab script'
    $queue = Read-SequencerQueue $QueuePath
    if ($Mode -eq 'Plan') { New-SequencerPlan $queue $MaxMonitors | ConvertTo-Json -Depth 30; return }
    if ($StopRecordedMonitors) { throw 'Stop is intentionally not implemented; no process is killed by this wrapper.' }
    if ([string]::IsNullOrWhiteSpace($StatePath)) { $StatePath = Join-Path (Split-Path $script:ProbeLabPath) '..\logs\overnight-probe-sequencer-state.json' }
    if ([string]::IsNullOrWhiteSpace($ReceiptPath)) { $ReceiptPath = Join-Path (Split-Path $StatePath) 'overnight-probe-sequencer.jsonl' }
    $null = New-Item -ItemType Directory -Force -Path (Split-Path $StatePath)
    Invoke-OvernightProbeSequencer $queue $StatePath $ReceiptPath $MaxMonitors | ConvertTo-Json -Depth 30
}
