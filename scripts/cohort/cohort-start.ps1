<#
.SYNOPSIS
    Log the persistent cohort in and arm it as autonomous players. Dry-run by default.
.DESCRIPTION
    Wraps the existing bridge verbs (autowow-control.ps1, same sequence as revive-autowow.ps1):
    activate (Playerbots async login, masterless) -> wait online -> independent (solo, NewRpg,
    forced activity, no-teleport). Only offline cohort members are touched; online members keep
    their current state (re-arming would reset their AI). Idempotent: run it after every world
    start or crash to relog the cohort.

    -ControlMode
      Manifest : each entry uses its manifest control_arm (A/B split)
      Stock    : every selected entry is treated as the stock arm
      Oracle   : every selected entry is treated as the Oracle arm
    The arm is enforced against the live Oracle allowlist (AutoWow.OracleRuntime.BotGuids in the
    WSL playerbots.conf). An Oracle-arm bot missing from the list, or a Stock-arm bot present in
    it, is skipped and reported, never started in the wrong arm. Changing the list is a config
    edit + worldserver restart owned by the orchestrator.
#>
[CmdletBinding()]
param(
    [switch]$Apply,
    [ValidateSet('Manifest','Stock','Oracle')][string]$ControlMode = 'Manifest',
    [string[]]$Id = @(),
    [ValidateSet('Both','Alliance','Horde')][string]$Faction = 'Both',
    [string]$ManifestPath = (Join-Path $PSScriptRoot 'cohort-manifest.json'),
    [string]$WslDistro = 'Ubuntu-24.04',
    [ValidateRange(10,600)][int]$OnlineTimeoutSeconds = 120
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'cohort-lib.ps1')

$manifest = Get-CohortManifest -Path $ManifestPath
$entries = @(Select-CohortEntries -Manifest $manifest -Id $Id -Faction $Faction)
if ($entries.Count -eq 0) { throw 'No active manifest entries selected.' }

$db = Get-CohortDb
$inv = Get-CohortInventory -Db $db -Entries $entries
$oracle = Get-CohortOracleAllowlist -WslDistro $WslDistro
$online = @(Get-CohortOnlineGuids)

$toStart = New-Object System.Collections.Generic.List[object]
$skipped = New-Object System.Collections.Generic.List[string]
$alreadyOnline = New-Object System.Collections.Generic.List[string]
foreach ($r in $inv.rows) {
    $arm = if ($ControlMode -eq 'Manifest') { $r.control_arm } else { $ControlMode.ToLowerInvariant() }
    $listed = [bool]($r.guid -and $oracle.enabled -and ($r.guid -in $oracle.guids))
    if ($r.state -ne 'ok' -or -not $r.enrolled) { $skipped.Add("$($r.id) not provisioned ($($r.state), enrolled=$($r.enrolled)); run provision-cohort.ps1"); continue }
    if ($arm -eq 'oracle' -and -not $listed) { $skipped.Add("$($r.id) guid $($r.guid): oracle arm but not in live AutoWow.OracleRuntime.BotGuids"); continue }
    if ($arm -eq 'stock' -and $listed) { $skipped.Add("$($r.id) guid $($r.guid): stock arm but listed in the Oracle allowlist"); continue }
    if ($r.guid -in $online) { $alreadyOnline.Add("$($r.id)=$($r.guid)"); continue }
    $toStart.Add([pscustomobject]@{ id = $r.id; guid = [uint32]$r.guid; arm = $arm })
}

$report = [ordered]@{
    mode = if ($Apply) { 'apply' } else { 'dry-run' }
    control_mode = $ControlMode
    oracle_runtime_enabled = $oracle.enabled
    planned = @($toStart | ForEach-Object { "activate+independent $($_.id) guid=$($_.guid) arm=$($_.arm)" })
    already_online = @($alreadyOnline)
    skipped = @($skipped)
}
if (-not $Apply -or $toStart.Count -eq 0) { $report | ConvertTo-Json -Depth 4; return }

foreach ($b in $toStart) {
    $res = Invoke-CohortBridge -Action activate -Guid $b.guid
    if (-not $res.ok -and $res.error -ne 'bot_already_online') { throw "activate rejected for $($b.id): $($res.error)" }
    Start-Sleep -Milliseconds 250
}
$deadline = (Get-Date).AddSeconds($OnlineTimeoutSeconds)
do {
    Start-Sleep -Seconds 2
    $online = @(Get-CohortOnlineGuids)
    $missing = @($toStart | Where-Object { $_.guid -notin $online })
} while ($missing.Count -and (Get-Date) -lt $deadline)

$armed = New-Object System.Collections.Generic.List[string]
$failed = New-Object System.Collections.Generic.List[string]
foreach ($b in @($toStart | Where-Object { $_.guid -in $online })) {
    # A fresh login may already be fighting; independent defers during combat (as in revive-autowow.ps1).
    for ($attempt = 1; $attempt -le 11; $attempt++) {
        $res = Invoke-CohortBridge -Action independent -Guid $b.guid
        if ($res.ok -or $res.error -ne 'independent_deferred_combat') { break }
        Start-Sleep -Seconds 2
    }
    if ($res.ok) { $armed.Add("$($b.id)=$($b.guid)") } else { $failed.Add("$($b.id): $($res.error)") }
}
$report['armed'] = @($armed)
$report['not_online'] = @($missing | ForEach-Object { "$($_.id)=$($_.guid)" })
$report['arm_failed'] = @($failed)
$report | ConvertTo-Json -Depth 4
if ($failed.Count -or $missing.Count) { throw 'Cohort start incomplete; see not_online / arm_failed.' }
