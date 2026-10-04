<#
.SYNOPSIS
    Start the full AutoWoW soak detached: authserver (if down), WSL worldserver, then arm the cohort, scouts,
    reps, artisans and squad, and start the snapshot + metrics samplers. The soak config must already be
    applied (soak-config.sh apply). MySQL is the owner's (needs MYSQL_ROOT_PASSWORD).
.EXAMPLE
    .\start-soak-full.ps1 -RunId soak-s48-full-r1 -SnapshotName cohort-s48.jsonl
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$RunId,
    [Parameter(Mandatory = $true)][string]$SnapshotName,
    [switch]$SkipWorld,
    # Cohort GUIDs known not to arm (62961 Hedd: scripted one-HP fight); left pending without failing the start.
    [uint32[]]$TolerateUnarmedGuids = @(62961),
    [string]$WorldserverBinary = '/root/autowow-upstream-s83-build/src/server/apps/worldserver',
    [string]$AuthserverBinary = '/root/autowow-upstream-s83-build/src/server/apps/authserver',
    [string]$AuthserverConfig = '/root/p1runtime/authserver-s83.conf'
)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$sd = Join-Path $PSScriptRoot 'start-detached.ps1'
$lg = Join-Path $root 'logs\phase1-runtime'
$ctl = Join-Path $PSScriptRoot 'autowow-control.ps1'

function Get-FullSoakServerLaunchPlan {
    param(
        [Parameter(Mandatory = $true)][string]$ServerRoot,
        [string]$WorldserverBinary = '/root/autowow-upstream-s83-build/src/server/apps/worldserver',
        [string]$AuthserverBinary = '/root/autowow-upstream-s83-build/src/server/apps/authserver',
        [string]$AuthserverConfig = '/root/p1runtime/authserver-s83.conf'
    )

    $authBinarySpecified = -not [string]::IsNullOrWhiteSpace($AuthserverBinary)
    $authConfigSpecified = -not [string]::IsNullOrWhiteSpace($AuthserverConfig)
    if ($authBinarySpecified -xor $authConfigSpecified) {
        throw 'AuthserverBinary and AuthserverConfig must be supplied together.'
    }

    foreach ($commandLineValue in @($ServerRoot, $WorldserverBinary, $AuthserverBinary, $AuthserverConfig)) {
        if ($commandLineValue.IndexOf('"') -ge 0 -or $commandLineValue.IndexOf('%') -ge 0 -or
            $commandLineValue.IndexOfAny([char[]]@("`r", "`n")) -ge 0) {
            throw 'Server launch paths cannot contain double quotes, percent signs, or line breaks.'
        }
    }

    $quotedRoot = '"' + $ServerRoot + '"'
    $quotedWorld = '"' + $WorldserverBinary + '"'
    $quotedAuth = '"' + $AuthserverBinary + '"'
    $quotedAuthConfig = '"' + $AuthserverConfig + '"'
    [pscustomobject]@{
        auth_mode = if ($authBinarySpecified) { 'wsl' } else { 'windows' }
        start_windows_auth = -not $authBinarySpecified
        arguments = "-ServerRoot $quotedRoot -WorldserverBinary $quotedWorld " +
            "-AuthserverBinary $quotedAuth -AuthserverConfig $quotedAuthConfig"
    }
}

function Invoke-CohortStartWithRetry {
    param(
        [Parameter(Mandatory = $true)][string]$CohortStartPath,
        [ValidateRange(1,100)][int]$Attempts = 8,
        [ValidateRange(0,600)][int]$DelaySeconds = 30,
        [uint32[]]$Tolerate = @()
    )
    $preflightRaw = @(& $CohortStartPath -ControlMode Stock)
    $preflight = (($preflightRaw | Out-String) | ConvertFrom-Json)
    $pending = @($preflight.pending_guids | ForEach-Object { [uint32]$_ })
    $lastError = $null
    for ($try = 1; $try -le $Attempts; $try++) {
        try {
            $raw = @(& $CohortStartPath -ControlMode Stock -Apply -PendingGuid $pending -AllowIncomplete)
            $status = (($raw | Out-String) | ConvertFrom-Json)
            $pending = @($status.pending_guids | ForEach-Object { [uint32]$_ })
            if ([bool]$status.complete -and $pending.Count -eq 0) {
                "cohort ok try $try"
                return $status
            }
            $blocking = @($pending | Where-Object { $Tolerate -notcontains $_ })
            if ($pending.Count -gt 0 -and $blocking.Count -eq 0) {
                "cohort ok try $try (tolerated unarmed: $($pending -join ','))"
                return $status
            }
            $lastError = "pending GUIDs: $($pending -join ',')"
        }
        catch {
            $lastError = $_.Exception.Message
        }
        if ($try -lt $Attempts -and $DelaySeconds -gt 0) { Start-Sleep -Seconds $DelaySeconds }
    }
    throw "Cohort start incomplete after $Attempts attempts ($lastError)."
}

function ConvertFrom-FullSoakControlResult {
    param([Parameter(Mandatory = $true)]$Value)
    $items = @($Value)
    if (-not $items.Count) { throw 'AutoWoW control returned no result.' }
    $last = $items[-1]
    if ($last -is [string]) { return ([string]$last | ConvertFrom-Json) }
    return $last
}

function Invoke-FullSoakWslScoutStart {
    param(
        [Parameter(Mandatory = $true)][string]$ControlPath,
        [scriptblock]$ControlInvoker,
        [ValidateRange(1,120)][int]$OnlineAttempts = 45,
        [ValidateRange(1,30)][int]$ArmAttempts = 11,
        [ValidateRange(0,30)][int]$DelaySeconds = 2
    )

    $scoutGuids = @([uint32]101, [uint32]112, [uint32]121, [uint32]123, [uint32]236, [uint32]244)
    if (-not $ControlInvoker) {
        $ControlInvoker = {
            param($Action, $Guid)
            if ($Action -eq 'list') { return @(& $ControlPath -Action list) }
            return @(& $ControlPath -Action $Action -BotGuid ([uint32]$Guid))
        }
    }

    $invokeControl = {
        param($Action, $Guid = 0)
        ConvertFrom-FullSoakControlResult -Value (& $ControlInvoker $Action ([uint32]$Guid))
    }
    $list = & $invokeControl 'list'
    if (-not $list.ok) { throw "AutoWoW list was rejected: $($list.error)" }
    $onlineByGuid = @{}
    foreach ($bot in @($list.bots)) { $onlineByGuid[[uint32]$bot.guid] = $bot }

    $activationErrors = @{}
    foreach ($guid in @($scoutGuids | Where-Object { -not $onlineByGuid.ContainsKey($_) })) {
        try {
            $activation = & $invokeControl 'activate' $guid
            if (-not $activation.ok -and $activation.error -notin @('bot_already_online', 'login_already_queued')) {
                $activationErrors[$guid] = [string]$activation.error
            }
        }
        catch { $activationErrors[$guid] = $_.Exception.Message }
    }

    $notOnline = @($scoutGuids | Where-Object { -not $onlineByGuid.ContainsKey($_) })
    for ($attempt = 1; $notOnline.Count -and $attempt -le $OnlineAttempts; $attempt++) {
        if ($DelaySeconds -gt 0) { Start-Sleep -Seconds $DelaySeconds }
        $list = & $invokeControl 'list'
        if (-not $list.ok) { throw "AutoWoW list was rejected while waiting for scouts: $($list.error)" }
        foreach ($bot in @($list.bots)) { $onlineByGuid[[uint32]$bot.guid] = $bot }
        $notOnline = @($scoutGuids | Where-Object { -not $onlineByGuid.ContainsKey($_) })
    }

    $armed = New-Object System.Collections.Generic.List[uint32]
    $grouped = New-Object System.Collections.Generic.List[uint32]
    $armFailures = @{}
    foreach ($guid in @($scoutGuids | Where-Object { $onlineByGuid.ContainsKey($_) })) {
        $result = $null
        try {
            for ($attempt = 1; $attempt -le $ArmAttempts; $attempt++) {
                $result = & $invokeControl 'independent' $guid
                if ($result.ok -or $result.error -ne 'independent_deferred_combat') { break }
                if ($attempt -lt $ArmAttempts -and $DelaySeconds -gt 0) { Start-Sleep -Seconds $DelaySeconds }
            }
        }
        catch { $result = [pscustomobject]@{ ok = $false; error = $_.Exception.Message } }

        if ($result -and $result.ok) {
            $armed.Add($guid)
            continue
        }
        $bot = $onlineByGuid[$guid]
        $isGrouped = $bot.group -and
            (([int]$bot.group.members -gt 0) -or ([uint64]$bot.group.leader_guid -gt 0))
        if ($result -and $result.error -eq 'independent_requires_solo' -and $isGrouped) {
            $grouped.Add($guid)
            continue
        }
        $armFailures[$guid] = if ($result) { [string]$result.error } else { 'independent returned no result' }
    }

    if ($notOnline.Count -or $armFailures.Count) {
        $parts = New-Object System.Collections.Generic.List[string]
        if ($notOnline.Count) {
            $details = @($notOnline | ForEach-Object {
                if ($activationErrors.ContainsKey($_)) { "$_ ($($activationErrors[$_]))" } else { [string]$_ }
            })
            $parts.Add("not_online=$($details -join ',')")
        }
        if ($armFailures.Count) {
            $details = @($armFailures.Keys | Sort-Object | ForEach-Object { "$_ ($($armFailures[$_]))" })
            $parts.Add("arm_failed=$($details -join ',')")
        }
        throw "WSL scout arming incomplete: $($parts -join '; ')."
    }

    [pscustomobject]@{
        complete = $true
        scout_guids = @($scoutGuids)
        armed_guids = @($armed)
        grouped_guids = @($grouped)
    }
}

function Invoke-FullSoakScoutDispatch {
    param(
        [Parameter(Mandatory = $true)][ValidateSet('wsl','windows')][string]$AuthMode,
        [Parameter(Mandatory = $true)][string]$ServerRoot,
        [Parameter(Mandatory = $true)][string]$WorldserverBinary,
        [Parameter(Mandatory = $true)][string]$ControlPath,
        [Parameter(Mandatory = $true)][string]$RevivePath,
        [scriptblock]$ControlInvoker,
        [scriptblock]$LegacyInvoker,
        [ValidateRange(1,10)][int]$LegacyAttempts = 4,
        [ValidateRange(0,120)][int]$LegacyDelaySeconds = 20,
        [ValidateRange(1,120)][int]$OnlineAttempts = 45,
        [ValidateRange(1,30)][int]$ArmAttempts = 11,
        [ValidateRange(0,30)][int]$ControlDelaySeconds = 2
    )

    if ($AuthMode -eq 'wsl') {
        return Invoke-FullSoakWslScoutStart -ControlPath $ControlPath -ControlInvoker $ControlInvoker `
            -OnlineAttempts $OnlineAttempts -ArmAttempts $ArmAttempts -DelaySeconds $ControlDelaySeconds
    }

    if (-not $LegacyInvoker) {
        $LegacyInvoker = {
            param($Path, $Root, $Binary, $Guids)
            & $Path -Action Start -Apply -ServerRoot $Root -WorldserverBinary $Binary -RosterGuids $Guids
        }
    }
    $scoutGuids = @([uint32]101, [uint32]112, [uint32]121, [uint32]123, [uint32]236, [uint32]244)
    $lastError = $null
    for ($attempt = 1; $attempt -le $LegacyAttempts; $attempt++) {
        try {
            $null = & $LegacyInvoker $RevivePath $ServerRoot $WorldserverBinary $scoutGuids
            return [pscustomobject]@{ complete = $true; scout_guids = $scoutGuids; armed_guids = $scoutGuids; grouped_guids = @() }
        }
        catch { $lastError = $_.Exception.Message }
        if ($attempt -lt $LegacyAttempts -and $LegacyDelaySeconds -gt 0) { Start-Sleep -Seconds $LegacyDelaySeconds }
    }
    throw "Legacy scout startup failed after $LegacyAttempts attempts: $lastError"
}

if (-not $SkipWorld) {
    $launchPlan = Get-FullSoakServerLaunchPlan -ServerRoot $root -WorldserverBinary $WorldserverBinary `
        -AuthserverBinary $AuthserverBinary -AuthserverConfig $AuthserverConfig
    if ($launchPlan.start_windows_auth -and -not (Get-Process authserver -ErrorAction SilentlyContinue)) {
        & $sd -Script (Join-Path $PSScriptRoot 'start-server.ps1') -Arguments '-AuthOnly' -Log "$lg\detached-auth.log"
        Start-Sleep -Seconds 20
    }
    # A fresh log name per start: reusing one log file once left the launch silently not started.
    $startLog = Join-Path $lg ("detached-start-{0}.log" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
    & $sd -Script (Join-Path $PSScriptRoot 'start-phase1-wsl-worldserver.ps1') `
        -Arguments $launchPlan.arguments -Log $startLog
}
$ready = $false
foreach ($i in 1..120) {
    Start-Sleep -Seconds 5
    $tcp = New-Object System.Net.Sockets.TcpClient
    try { $ready = $tcp.ConnectAsync('127.0.0.1', 18787).Wait(1000) -and $tcp.Connected } catch {} finally { $tcp.Dispose() }
    if ($ready) { break }
}
"bridge ready=$ready"
if (-not $ready) { return }
Start-Sleep -Seconds 30

$null = Invoke-CohortStartWithRetry -CohortStartPath (Join-Path $PSScriptRoot 'cohort\cohort-start.ps1') -Tolerate $TolerateUnarmedGuids
$scoutAuthMode = if (-not [string]::IsNullOrWhiteSpace($AuthserverBinary) -and
    -not [string]::IsNullOrWhiteSpace($AuthserverConfig)) { 'wsl' } else { 'windows' }
$scoutStatus = Invoke-FullSoakScoutDispatch -AuthMode $scoutAuthMode -ServerRoot $root `
    -WorldserverBinary $WorldserverBinary -ControlPath $ctl `
    -RevivePath (Join-Path $PSScriptRoot 'revive-autowow.ps1')
"scouts armed/grouped $(@($scoutStatus.armed_guids).Count + @($scoutStatus.grouped_guids).Count)/$(@($scoutStatus.scout_guids).Count)"
# reps 69665-69672, Brewers artisans 70577/70582, Tanners artisans 72755/72756, Tinkers reps 73297/73299 + artisans 73298/73300, squad (grouped squads refuse 'independent' once formed: fine)
$special = @(69665..69672) + @(70577, 70582, 72755, 72756, 73297, 73298, 73299, 73300) + @(70573, 70574, 70575, 70576, 70763, 70578, 70579, 70580, 70581, 70764)
$ok = 0
foreach ($g in $special) {
    $null = @(& $ctl -Action activate -BotGuid $g)
    Start-Sleep -Seconds 3
    $r = ''
    foreach ($t in 1..6) {
        $r = [string]@(& $ctl -Action independent -BotGuid $g)[-1]
        if ($r -notmatch 'deferred_combat') { break }
        Start-Sleep -Seconds 10
    }
    if ($r -match '"ok":true|independent_requires_solo') { $ok++ }
}
"special bots armed/grouped $ok/$($special.Count)"
& $sd -Script (Join-Path $PSScriptRoot 'soak-snapshot-loop.ps1') -Arguments "-OutPath $root\work\soak-20260923\$SnapshotName" -Log "$lg\snapshot-$RunId.log"
& wsl.exe -d Ubuntu-24.04 -u root -e bash -c "setsid nohup bash /mnt/d/Games/wowstuff/AutoWoW/scripts/soak-metrics.sh $RunId 30 86400 >/dev/null 2>&1 < /dev/null & disown; sleep 2"
'samplers started'
