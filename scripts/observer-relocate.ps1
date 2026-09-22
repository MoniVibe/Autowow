<#
.SYNOPSIS
    Observer Camera v0 relocation loop — periodically relocates one protected observer to a
    watched bot leader (Northstar/Ember). DRY-RUN by default.

.DESCRIPTION
    Reads the watched leader's live position through the read-only bridge `snapshot` order and,
    when -Execute is supplied, issues `observe relocate` so the SERVER teleports the observer to
    the leader. It never groups the observer with bots, never issues quest/pause/travel/recover
    orders, never edits any database, and never launches or automates the WoW client. Screenshot
    and video capture are performed by the human-run client (see OBSERVER_CAMERA_V0_PLAN.md).

    Safety model:
      - Default is DRY-RUN: it only logs the relocation it *would* perform. Pass -Execute to act.
      - Before executing it verifies via `observe status` that the observer is GM-protected
        (is_gm && !gm_visible) and not grouped. If not, it refuses to execute and stays in dry-run.
      - All server-side guards (allow-list, protection, no-group, observer-not-a-bot) are enforced
        again on every relocate by ObserverControl.cpp; this script cannot bypass them.

.EXAMPLE
    # Safe to run before the bridge rebuild: observes only, issues no relocation.
    .\scripts\observer-relocate.ps1 -ObserverGuid 5 -LeaderGuid 10 -DurationMinutes 30

.EXAMPLE
    # After the ObserverControl hook is built and the observer is logged in and `.gm on`:
    .\scripts\observer-relocate.ps1 -ObserverGuid 5 -LeaderGuid 10 -Execute
#>
[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [Parameter(Mandatory = $true)][uint32]$ObserverGuid,
    [Parameter(Mandatory = $true)][uint32]$LeaderGuid,
    [ValidateRange(5, 300)][int]$IntervalSeconds = 15,
    [ValidateRange(1, 1440)][int]$DurationMinutes = 60,
    [switch]$Execute,
    [string]$ReceiptPath = ''
)

$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
if ($ObserverGuid -eq 0 -or $LeaderGuid -eq 0) { throw 'ObserverGuid and LeaderGuid are required positive GUIDs.' }

$observerControl = Join-Path $PSScriptRoot 'observer-control.ps1'
$botControl = Join-Path $PSScriptRoot 'autowow-control.ps1'
foreach ($p in @($observerControl, $botControl)) { if (-not (Test-Path -LiteralPath $p)) { throw "Missing dependency: $p" } }

if ([string]::IsNullOrWhiteSpace($ReceiptPath)) {
    $ReceiptPath = Join-Path $ServerRoot ('logs\observer-relocate-{0}.jsonl' -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
New-Item -ItemType Directory -Path (Split-Path -Parent $ReceiptPath) -Force | Out-Null
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)

function Write-Receipt {
    param([Parameter(Mandatory = $true)][string]$Event, [hashtable]$Fields = @{})
    $record = [ordered]@{ timestamp_utc = (Get-Date).ToUniversalTime().ToString('o'); event = $Event }
    foreach ($key in $Fields.Keys) { $record[$key] = $Fields[$key] }
    $line = ([pscustomobject]$record | ConvertTo-Json -Depth 8 -Compress)
    [System.IO.File]::AppendAllText($ReceiptPath, $line + [Environment]::NewLine, $utf8NoBom)
    Write-Output $line
}

function Invoke-Json {
    param([Parameter(Mandatory = $true)][scriptblock]$Call)
    $raw = & $Call
    $line = (@($raw | ForEach-Object { $_.ToString() }) | Select-Object -Last 1)
    if ([string]::IsNullOrWhiteSpace($line)) { throw 'Bridge returned no response.' }
    return $line | ConvertFrom-Json
}

# ConvertFrom-Json is used only to read; nothing here writes character data.
$deadline = (Get-Date).AddMinutes($DurationMinutes)
$executing = [bool]$Execute
$consecutiveErrors = 0
$initialMode = if ($executing) { 'execute' } else { 'dry_run' }

Write-Receipt -Event 'observer_relocate_started' -Fields @{
    observer_guid = $ObserverGuid; leader_guid = $LeaderGuid
    interval_seconds = $IntervalSeconds; duration_minutes = $DurationMinutes
    mode = $initialMode; bridge = '127.0.0.1:18787'
}

# Preflight: verify protection + set the watched leader server-side (only when executing).
if ($executing) {
    try {
        $status = Invoke-Json { & $observerControl -Action status -ObserverGuid $ObserverGuid }
        if (-not $status.ok) {
            $executing = $false
            $note = if ($status.error -match 'unknown command') { 'observe_endpoint_not_built' } else { $status.error }
            Write-Receipt -Event 'execute_disabled' -Fields @{ reason = $note; hint = 'rebuild bridge (ObserverControl) and/or enroll guid in AutoWow.ObserverGuids' }
        }
        elseif (-not $status.protected) {
            $executing = $false
            Write-Receipt -Event 'execute_disabled' -Fields @{ reason = 'observer_not_protected'; is_gm = [bool]$status.is_gm; gm_visible = [bool]$status.gm_visible; in_group = [bool]$status.in_group; hint = 'run: observe protect (or in client: .gm on then .gm visible off), and leave all bot groups' }
        }
        else {
            $watch = Invoke-Json { & $observerControl -Action watch -ObserverGuid $ObserverGuid -LeaderGuid $LeaderGuid }
            Write-Receipt -Event 'watch_set' -Fields @{ ok = [bool]$watch.ok; leader = $watch.leader; leader_name = $watch.leader_name; error = $watch.error }
            if (-not $watch.ok) { $executing = $false }
        }
    }
    catch {
        $executing = $false
        Write-Receipt -Event 'execute_disabled' -Fields @{ reason = 'preflight_failed'; error = $_.Exception.Message }
    }
}

try {
    while ((Get-Date) -lt $deadline) {
        try {
            # Read-only: the leader is a Playerbots bot, so the standard snapshot order returns its live pose.
            $leaderSnap = Invoke-Json { & $botControl -Action snapshot -BotGuid $LeaderGuid }
            $bot = $leaderSnap.bot
            $pos = $bot.position
            Write-Receipt -Event 'leader_observed' -Fields @{
                leader_guid = $LeaderGuid; name = $bot.name; alive = [bool]$bot.alive; combat = [bool]$bot.combat
                map = $pos.map; x = $pos.x; y = $pos.y; z = $pos.z
            }

            if ($executing) {
                $reloc = Invoke-Json { & $observerControl -Action relocate -ObserverGuid $ObserverGuid }
                if ($reloc.ok) {
                    $relocationResult = if ($reloc.result) { $reloc.result } else { 'teleported' }
                    Write-Receipt -Event 'relocated' -Fields @{ observer_guid = $ObserverGuid; leader_guid = $LeaderGuid; result = $relocationResult; map = $reloc.map }
                }
                elseif ($reloc.error -in @('observer_floor_unavailable', 'observer_floor_context_invalid')) {
                    # A safe camera offset may be unavailable on a stair or ledge. Retry at the
                    # next normal interval; the server rechecks every relocation guard each time.
                    Write-Receipt -Event 'relocate_deferred' -Fields @{ observer_guid = $ObserverGuid; error = $reloc.error; mode = 'execute' }
                }
                else {
                    # A guard tripped (protection lost, grouped, endpoint missing, ...). Stop executing to
                    # avoid a hot error loop; continue observing in dry-run and surface the reason.
                    $executing = $false
                    Write-Receipt -Event 'relocate_refused' -Fields @{ observer_guid = $ObserverGuid; error = $reloc.error; mode = 'reverting_to_dry_run' }
                }
            }
            else {
                Write-Receipt -Event 'relocate_dry_run' -Fields @{ observer_guid = $ObserverGuid; leader_guid = $LeaderGuid; would_move_to = @{ map = $pos.map; x = $pos.x; y = $pos.y; z = $pos.z } }
            }
            $consecutiveErrors = 0
        }
        catch {
            $consecutiveErrors++
            Write-Receipt -Event 'loop_error' -Fields @{ consecutive_errors = $consecutiveErrors; error = $_.Exception.Message }
            if ($consecutiveErrors -ge 5) { throw "Observer relocate loop stopped after $consecutiveErrors consecutive errors." }
        }
        Start-Sleep -Seconds $IntervalSeconds
    }
    Write-Receipt -Event 'observer_relocate_completed' -Fields @{ observer_guid = $ObserverGuid; leader_guid = $LeaderGuid }
}
finally {
    Write-Receipt -Event 'observer_relocate_stopped' -Fields @{ observer_guid = $ObserverGuid; receipt = $ReceiptPath }
}
