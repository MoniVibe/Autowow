<#
.SYNOPSIS
    Log the persistent cohort in and arm it as autonomous players. Dry-run by default.
.DESCRIPTION
    Wraps the existing bridge verbs (autowow-control.ps1, same sequence as revive-autowow.ps1):
    activate (Playerbots async login, masterless) -> wait online -> independent (solo, NewRpg,
    forced activity, no-teleport). Offline cohort members are started and armed. Online members
    keep their current state unless their GUID is explicitly supplied through -PendingGuid after
    an earlier incomplete attempt. Idempotent: run it after every world start or crash to relog
    the cohort.

    -PendingGuid carries incomplete start/arm work into a later attempt. -AllowIncomplete returns
    the machine-readable receipt instead of throwing, so an outer bounded retry loop can consume
    pending_guids. complete means all selected actions from this invocation finished; it does not
    verify the AI state of originally-online members that were intentionally left untouched.

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
    [uint32[]]$PendingGuid = @(),
    [switch]$AllowIncomplete,
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
$inv = Get-CohortInventory -DbInfo $db -Entries $entries
$oracle = Get-CohortOracleAllowlist -WslDistro $WslDistro
$online = @(Get-CohortOnlineGuids)

$toStart = New-Object System.Collections.Generic.List[object]
$retryOnline = New-Object System.Collections.Generic.List[object]
$rejectedPending = New-Object System.Collections.Generic.List[uint32]
$skipped = New-Object System.Collections.Generic.List[string]
$alreadyOnline = New-Object System.Collections.Generic.List[string]
foreach ($r in $inv.rows) {
    $arm = if ($ControlMode -eq 'Manifest') { $r.control_arm } else { $ControlMode.ToLowerInvariant() }
    $listed = [bool]($r.guid -and $oracle.enabled -and ($r.guid -in $oracle.guids))
    $isPending = [bool]($r.guid -and ([uint32]$r.guid -in $PendingGuid))
    if ($r.state -ne 'ok' -or -not $r.enrolled) {
        $skipped.Add("$($r.id) not provisioned ($($r.state), enrolled=$($r.enrolled)); run provision-cohort.ps1")
        if ($isPending) { $rejectedPending.Add([uint32]$r.guid) }
        continue
    }
    if ($arm -eq 'oracle' -and -not $listed) {
        $skipped.Add("$($r.id) guid $($r.guid): oracle arm but not in live AutoWow.OracleRuntime.BotGuids")
        if ($isPending) { $rejectedPending.Add([uint32]$r.guid) }
        continue
    }
    if ($arm -eq 'stock' -and $listed) {
        $skipped.Add("$($r.id) guid $($r.guid): stock arm but listed in the Oracle allowlist")
        if ($isPending) { $rejectedPending.Add([uint32]$r.guid) }
        continue
    }
    $candidate = [pscustomobject]@{ id = $r.id; guid = [uint32]$r.guid; arm = $arm }
    if ($r.guid -in $online) {
        if ($isPending) { $retryOnline.Add($candidate) }
        else { $alreadyOnline.Add("$($r.id)=$($r.guid)") }
        continue
    }
    $toStart.Add($candidate)
}

$knownGuids = @($inv.rows | Where-Object { $_.guid } | ForEach-Object { [uint32]$_.guid })
foreach ($guid in @($PendingGuid | Where-Object { $_ -notin $knownGuids })) { $rejectedPending.Add([uint32]$guid) }

$report = [ordered]@{
    mode = if ($Apply) { 'apply' } else { 'dry-run' }
    control_mode = $ControlMode
    oracle_runtime_enabled = $oracle.enabled
    planned = @(
        @($toStart | ForEach-Object { "activate+independent $($_.id) guid=$($_.guid) arm=$($_.arm)" }) +
        @($retryOnline | ForEach-Object { "independent-retry $($_.id) guid=$($_.guid) arm=$($_.arm)" })
    )
    already_online = @($alreadyOnline)
    skipped = @($skipped)
}
if (-not $Apply) {
    $plannedPending = @(@(
        @($toStart | ForEach-Object { [uint32]$_.guid }) +
        @($retryOnline | ForEach-Object { [uint32]$_.guid }) +
        @($rejectedPending | ForEach-Object { [uint32]$_ })
    ) | Sort-Object -Unique)
    $report['pending_guids'] = @($plannedPending)
    $report['complete'] = ($plannedPending.Count -eq 0)
    $report | ConvertTo-Json -Depth 4
    return
}

$activationFailed = New-Object System.Collections.Generic.List[uint32]
foreach ($b in $toStart) {
    try {
        $res = Invoke-CohortBridge -Action activate -Guid $b.guid
        if (-not $res.ok -and $res.error -ne 'bot_already_online') {
            $activationFailed.Add([uint32]$b.guid)
            $skipped.Add("$($b.id): activate rejected ($($res.error))")
        }
        else { Start-Sleep -Milliseconds 250 }
    }
    catch {
        $activationFailed.Add([uint32]$b.guid)
        $skipped.Add("$($b.id): activate failed ($($_.Exception.Message))")
    }
}
$missing = @()
if ($toStart.Count) {
    $deadline = (Get-Date).AddSeconds($OnlineTimeoutSeconds)
    do {
        Start-Sleep -Seconds 2
        $online = @(Get-CohortOnlineGuids)
        $missing = @($toStart | Where-Object { $_.guid -notin $online })
    } while ($missing.Count -and (Get-Date) -lt $deadline)
}

$armed = New-Object System.Collections.Generic.List[string]
$armedGuids = New-Object System.Collections.Generic.List[uint32]
$failed = New-Object System.Collections.Generic.List[string]
$failedGuids = New-Object System.Collections.Generic.List[uint32]
$armTargets = @(
    @($toStart | Where-Object { $_.guid -in $online }) +
    @($retryOnline | ForEach-Object { $_ })
)
foreach ($b in $armTargets) {
    # A fresh login may already be fighting; independent defers during combat (as in revive-autowow.ps1).
    $res = $null
    try {
        for ($attempt = 1; $attempt -le 11; $attempt++) {
            $res = Invoke-CohortBridge -Action independent -Guid $b.guid
            if ($res.ok -or $res.error -ne 'independent_deferred_combat') { break }
            Start-Sleep -Seconds 2
        }
    }
    catch {
        $res = [pscustomobject]@{ ok = $false; error = $_.Exception.Message }
    }
    if ($res -and $res.ok) {
        $armed.Add("$($b.id)=$($b.guid)")
        $armedGuids.Add([uint32]$b.guid)
    }
    else {
        $errorText = if ($res) { [string]$res.error } else { 'independent returned no result' }
        $failed.Add("$($b.id): $errorText")
        $failedGuids.Add([uint32]$b.guid)
    }
}
$pendingGuids = @(@(
    @($missing | ForEach-Object { [uint32]$_.guid }) +
    @($activationFailed | Where-Object { [uint32]$_ -notin $armedGuids } | ForEach-Object { [uint32]$_ }) +
    @($failedGuids | ForEach-Object { [uint32]$_ }) +
    @($rejectedPending | ForEach-Object { [uint32]$_ })
) | Sort-Object -Unique)
$report['skipped'] = @($skipped | ForEach-Object { [string]$_ })
$report['armed'] = @($armed)
$report['not_online'] = @($missing | ForEach-Object { "$($_.id)=$($_.guid)" })
$report['arm_failed'] = @($failed)
$report['pending_guids'] = @($pendingGuids)
$report['complete'] = ($pendingGuids.Count -eq 0)
$report | ConvertTo-Json -Depth 4
if ($pendingGuids.Count -and -not $AllowIncomplete) { throw 'Cohort start incomplete; see not_online / arm_failed / pending_guids.' }
