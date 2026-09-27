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
    [switch]$SkipWorld
)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$sd = Join-Path $PSScriptRoot 'start-detached.ps1'
$lg = Join-Path $root 'logs\phase1-runtime'
$ctl = Join-Path $PSScriptRoot 'autowow-control.ps1'

if (-not $SkipWorld) {
if (-not (Get-Process authserver -ErrorAction SilentlyContinue)) {
    & $sd -Script (Join-Path $PSScriptRoot 'start-server.ps1') -Arguments '-AuthOnly' -Log "$lg\detached-auth.log"
    Start-Sleep -Seconds 20
}
# A fresh log name per start: reusing one log file once left the launch silently not started.
$startLog = Join-Path $lg ("detached-start-{0}.log" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
& $sd -Script (Join-Path $PSScriptRoot 'start-phase1-wsl-worldserver.ps1') -Arguments '-WorldserverBinary /root/autowow-advisor-t1-build/src/server/apps/worldserver' -Log $startLog
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

foreach ($try in 1..8) {
    try { & (Join-Path $PSScriptRoot 'cohort\cohort-start.ps1') -ControlMode Stock -Apply 2>&1 | Out-Null; "cohort ok try $try"; break }
    catch { Start-Sleep -Seconds 30 }
}
foreach ($try in 1..4) {
    try { & (Join-Path $PSScriptRoot 'revive-autowow.ps1') -Action Start -Apply -RosterGuids @(101,112,121,123,236,244) 2>&1 | Out-Null; 'scouts ok'; break }
    catch { Start-Sleep -Seconds 20 }
}
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
