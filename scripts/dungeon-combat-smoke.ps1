[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [uint32]$TankGuid = 2,
    [uint32]$HealerGuid = 26,
    [uint32[]]$DpsGuids = @(8,45,24),
    [ValidateRange(30,900)][int]$DurationSeconds = 180,
    [ValidateRange(1,10)][int]$PollSeconds = 1,
    [string]$ReceiptPath = '',
    [switch]$Apply
)

. (Join-Path $PSScriptRoot 'QuestLogSource.ps1')

$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
$control = Join-Path $PSScriptRoot 'autowow-control.ps1'
if (-not $ReceiptPath) {
    $ReceiptPath = Join-Path $ServerRoot ('logs\phase1-{0}\uk-combat-smoke.jsonl' -f (Get-Date -Format 'yyyyMMdd'))
}
$party = @($TankGuid, $HealerGuid) + @($DpsGuids)
if ($party.Count -ne 5 -or @($party | Select-Object -Unique).Count -ne 5) { throw 'Exactly five unique fixture GUIDs are required.' }

function Write-Receipt {
    param([string]$Event, [hashtable]$Fields = @{})
    $row = [ordered]@{ timestamp_utc = (Get-Date).ToUniversalTime().ToString('o'); event = $Event }
    foreach ($key in $Fields.Keys) { $row[$key] = $Fields[$key] }
    $directory = Split-Path -Parent $ReceiptPath
    if (-not (Test-Path -LiteralPath $directory)) { New-Item -ItemType Directory -Path $directory -Force | Out-Null }
    [System.IO.File]::AppendAllText($ReceiptPath, (($row | ConvertTo-Json -Compress -Depth 15) + [Environment]::NewLine), [System.Text.UTF8Encoding]::new($false))
}

function Invoke-ControlJson {
    param([string]$Action, [uint32]$Guid = 0)
    return ((& $control -Action $Action -BotGuid $Guid | Out-String).Trim() | ConvertFrom-Json)
}

function Get-Snapshot {
    param([uint32]$Guid)
    return (Send-Bridge -Request "snapshot $Guid" | ConvertFrom-Json).bot
}

function Get-Combat {
    param([uint32]$Guid)
    $response = Send-Bridge -Request "combatlog $Guid" | ConvertFrom-Json
    if (-not $response.ok) { throw "combatlog failed for ${Guid}: $($response.error)" }
    return $response.telemetry
}

$plan = [ordered]@{
    map = 574
    dungeon = 'Utgarde Keep normal'
    tank_guid = $TankGuid
    healer_guid = $HealerGuid
    dps_guids = @($DpsGuids)
    duration_seconds = $DurationSeconds
    receipt = $ReceiptPath
    applies = [bool]$Apply
}
if (-not $Apply) { [pscustomobject]$plan | ConvertTo-Json -Depth 6; return }

$list = Invoke-ControlJson -Action list
$members = @($list.bots | Where-Object { [uint32]$_.guid -in $party })
if ($members.Count -ne 5) { throw 'All five fixture bots must be online.' }
foreach ($member in $members) {
    if ([uint32]$member.group.leader_guid -ne $TankGuid -or [int]$member.group.members -ne 5) {
        throw "GUID $($member.guid) is not in the expected five-player party."
    }
    if ([int]$member.position.map -ne 574) { throw "GUID $($member.guid) is not inside Utgarde Keep." }
}

$tankPre = Get-Combat -Guid $TankGuid
$healerPre = Get-Combat -Guid $HealerGuid
$instanceIds = @($party | ForEach-Object { [uint32](Get-Combat -Guid $_).location.instance_id } | Select-Object -Unique)
if ($instanceIds.Count -ne 1 -or $instanceIds[0] -eq 0) {
    throw "The five bots are not in one shared dungeon instance (instance ids: $($instanceIds -join ','))."
}
if (-not $tankPre.role.tank -or -not $tankPre.role.main_tank) { throw 'Configured tank is not recognized as the main tank.' }
if (-not $healerPre.role.healer) { throw 'Configured healer is not recognized as a healer.' }
$baseline = @{}
foreach ($guid in $party) { $baseline[$guid] = Get-Snapshot -Guid $guid }

Write-Receipt -Event 'dungeon_smoke_started' -Fields @{ plan = $plan; instance_id = $instanceIds[0]; tank_role = $tankPre.role; healer_role = $healerPre.role }
$deploy = Invoke-ControlJson -Action deploy -Guid $TankGuid
Start-Sleep -Seconds 2
$engage = Invoke-ControlJson -Action engage -Guid $TankGuid
Write-Receipt -Event 'dungeon_orders_issued' -Fields @{ deploy = $deploy; engage = $engage; staging_teleport_count = 5 }

$totalOwnerEvents = 0
$tankOwnerEvents = 0
$healerSelectionSamples = 0
$healerTankSelections = 0
$combatSamples = 0
$dead = [System.Collections.Generic.HashSet[uint32]]::new()
$tankSpells = @{}
$healerSpells = @{}
$deadline = (Get-Date).AddSeconds($DurationSeconds)

while ((Get-Date) -lt $deadline) {
    $telemetry = [ordered]@{}
    foreach ($guid in $party) { $telemetry[[string]$guid] = Get-Combat -Guid $guid }

    $anyCombat = @($telemetry.Values | Where-Object { [bool]$_.in_combat }).Count -gt 0
    if ($anyCombat) { ++$combatSamples }
    foreach ($guid in $party) {
        $sample = $telemetry[[string]$guid]
        if (-not [bool]$sample.alive) { [void]$dead.Add($guid) }
        $owned = [int]$sample.threat.owners_targeting_bot
        $totalOwnerEvents += $owned
        if ($guid -eq $TankGuid) { $tankOwnerEvents += $owned }
    }

    $healer = $telemetry[[string]$HealerGuid]
    if ([bool]$healer.in_combat -and $null -ne $healer.healer.target) {
        ++$healerSelectionSamples
        if ([uint32]$healer.healer.target.guid -eq $TankGuid) { ++$healerTankSelections }
    }

    $tankSpell = [uint32]$telemetry[[string]$TankGuid].recent.last_spell.id
    $healerSpell = [uint32]$healer.recent.last_spell.id
    if ($tankSpell) { $tankSpells[[string]$tankSpell] = 1 + [int]$tankSpells[[string]$tankSpell] }
    if ($healerSpell) { $healerSpells[[string]$healerSpell] = 1 + [int]$healerSpells[[string]$healerSpell] }

    Write-Receipt -Event 'combat_sample' -Fields @{ any_combat = $anyCombat; telemetry = $telemetry }
    Start-Sleep -Seconds $PollSeconds
}

$xpDelta = 0
$aliveAtEnd = 0
foreach ($guid in $party) {
    $final = Get-Snapshot -Guid $guid
    $xpDelta += [int64]$final.progress.xp - [int64]$baseline[$guid].progress.xp
    if ([bool]$final.alive) { ++$aliveAtEnd }
}
$tankRatio = if ($totalOwnerEvents) { [double]$tankOwnerEvents / $totalOwnerEvents } else { 0.0 }
$healerRatio = if ($healerSelectionSamples) { [double]$healerTankSelections / $healerSelectionSamples } else { 0.0 }
$passed = $combatSamples -gt 0 -and $totalOwnerEvents -ge 5 -and $tankRatio -ge 0.9 -and `
    $healerSelectionSamples -gt 0 -and $healerRatio -ge 0.6 -and $dead.Count -eq 0 -and $aliveAtEnd -eq 5 -and $xpDelta -gt 0

$verdict = [ordered]@{
    passed = $passed
    combat_samples = $combatSamples
    hostile_owner_events = $totalOwnerEvents
    tank_owner_events = $tankOwnerEvents
    tank_ownership_ratio = [Math]::Round($tankRatio, 4)
    healer_selection_samples = $healerSelectionSamples
    healer_tank_selections = $healerTankSelections
    healer_tank_priority_ratio = [Math]::Round($healerRatio, 4)
    dead_guids = @($dead)
    alive_at_end = $aliveAtEnd
    party_xp_delta = $xpDelta
    observed_tank_spells = $tankSpells
    observed_healer_spells = $healerSpells
    recent_event_counters_available = $false
    staging_teleport_count = 5
    receipt = $ReceiptPath
}
Write-Receipt -Event 'dungeon_smoke_verdict' -Fields $verdict
[pscustomobject]$verdict | ConvertTo-Json -Depth 8
if (-not $passed) { exit 2 }
