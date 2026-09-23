<#
.SYNOPSIS
    Explicitly log the cohort out through the bridge (deactivate). Dry-run by default.
.DESCRIPTION
    Parking is the ONLY way a cohort member leaves the world besides a server stop: cohort
    characters are not in the random pool, so no rotation logs them out. Characters, gear,
    quests and levels are untouched; cohort-start.ps1 brings them back.
#>
[CmdletBinding()]
param(
    [switch]$Apply,
    [string[]]$Id = @(),
    [ValidateSet('Both','Alliance','Horde')][string]$Faction = 'Both',
    [string]$ManifestPath = (Join-Path $PSScriptRoot 'cohort-manifest.json')
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'cohort-lib.ps1')

$manifest = Get-CohortManifest -Path $ManifestPath
$entries = @(Select-CohortEntries -Manifest $manifest -Id $Id -Faction $Faction)
$inv = Get-CohortInventory -Db (Get-CohortDb) -Entries $entries
$online = @(Get-CohortOnlineGuids)
$targets = @($inv.rows | Where-Object { $_.state -eq 'ok' -and $_.enrolled -and $_.guid -in $online })

$report = [ordered]@{
    mode = if ($Apply) { 'apply' } else { 'dry-run' }
    planned = @($targets | ForEach-Object { "deactivate $($_.id) guid=$($_.guid)" })
}
if ($Apply) {
    $failed = New-Object System.Collections.Generic.List[string]
    foreach ($t in $targets) {
        $res = Invoke-CohortBridge -Action deactivate -Guid ([uint32]$t.guid)
        if (-not $res.ok -and $res.error -ne 'bot_not_online') { $failed.Add("$($t.id): $($res.error)") }
    }
    $report['failed'] = @($failed)
}
$report | ConvertTo-Json -Depth 4
