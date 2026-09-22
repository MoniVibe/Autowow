[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [uint32]$BotGuid = 0,
    [ValidateRange(1, 1440)][int]$DurationMinutes = 480,
    [ValidateRange(5, 60)][int]$PollSeconds = 20,
    [ValidateRange(5, 240)][int]$TravelEveryMinutes = 20,
    [switch]$NoTravel,
    [string]$ReceiptPath = ''
)

. (Join-Path $PSScriptRoot 'Common.ps1')

$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
Initialize-AutoWoWLayout -Root $ServerRoot
$control = Join-Path $PSScriptRoot 'autowow-control.ps1'
if (-not (Test-Path -LiteralPath $control)) { throw "AutoWow bridge client is missing: $control" }

if ([string]::IsNullOrWhiteSpace($ReceiptPath)) {
    $ReceiptPath = Join-Path $ServerRoot ('logs\\autowow-agent-{0}.jsonl' -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
$receiptDirectory = Split-Path -Parent $ReceiptPath
if ($receiptDirectory) { New-Item -ItemType Directory -Path $receiptDirectory -Force | Out-Null }
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)

function Write-AgentReceipt {
    param(
        [Parameter(Mandatory = $true)][string]$Event,
        [hashtable]$Fields = @{}
    )

    $entry = [ordered]@{
        timestamp = (Get-Date).ToUniversalTime().ToString('o')
        event = $Event
    }
    foreach ($key in $Fields.Keys) { $entry[$key] = $Fields[$key] }
    $line = ([pscustomobject]$entry | ConvertTo-Json -Compress -Depth 8)
    [System.IO.File]::AppendAllText($ReceiptPath, $line + [Environment]::NewLine, $utf8NoBom)
    Write-Output $line
}

function Invoke-Bridge {
    param(
        [Parameter(Mandatory = $true)][ValidateSet('list','snapshot','pause','resume','travel')][string]$Action,
        [uint32]$Guid = 0,
        [string]$Destination = ''
    )

    $response = switch ($Action) {
        'list' { & $control -Action list }
        'snapshot' { & $control -Action snapshot -BotGuid $Guid }
        'pause' { & $control -Action pause -BotGuid $Guid }
        'resume' { & $control -Action resume -BotGuid $Guid }
        'travel' { & $control -Action travel -BotGuid $Guid -Destination $Destination }
    }
    $raw = (@($response | ForEach-Object { $_.ToString() }) | Select-Object -Last 1)
    if ([string]::IsNullOrWhiteSpace($raw)) { throw "Bridge '$Action' returned no JSON." }
    $parsed = $raw | ConvertFrom-Json
    if (-not $parsed.ok) {
        $message = if ($parsed.error) { $parsed.error } else { 'unknown bridge error' }
        throw "Bridge '$Action' failed: $message"
    }
    return $parsed
}

function Select-AvailableBot {
    param([object]$ListResponse, [uint32]$PreferredGuid)

    $bots = @($ListResponse.bots)
    if ($PreferredGuid -ne 0) {
        return @($bots | Where-Object { [uint32]$_.guid -eq $PreferredGuid } | Select-Object -First 1)[0]
    }
    return @($bots | Where-Object { $_.alive -and -not $_.combat -and -not $_.paused } | Select-Object -First 1)[0]
}

$deadline = (Get-Date).AddMinutes($DurationMinutes)
$selectedGuid = $BotGuid
$travelDue = (Get-Date).AddMinutes($TravelEveryMinutes)
$consecutiveFailures = 0
$hasPausedProbe = $false
$mustResume = $false

Write-AgentReceipt -Event 'agent_started' -Fields @{
    duration_minutes = $DurationMinutes
    poll_seconds = $PollSeconds
    travel_every_minutes = $TravelEveryMinutes
    travel_enabled = (-not $NoTravel)
    requested_bot_guid = $BotGuid
    bridge = '127.0.0.1:18787'
}

try {
    while ((Get-Date) -lt $deadline) {
        try {
            $list = Invoke-Bridge -Action list
            $selected = Select-AvailableBot -ListResponse $list -PreferredGuid $selectedGuid
            if (-not $selected) {
                if ($selectedGuid -ne 0) {
                    Write-AgentReceipt -Event 'requested_bot_unavailable' -Fields @{ bot_guid = $selectedGuid; online_bot_count = @($list.bots).Count }
                }
                else {
                    Write-AgentReceipt -Event 'no_safe_bot_available' -Fields @{ online_bot_count = @($list.bots).Count }
                }
                $consecutiveFailures = 0
                Start-Sleep -Seconds $PollSeconds
                continue
            }

            if ($selectedGuid -ne [uint32]$selected.guid) {
                $selectedGuid = [uint32]$selected.guid
                $hasPausedProbe = $false
                $mustResume = $false
                Write-AgentReceipt -Event 'bot_selected' -Fields @{ bot_guid = $selectedGuid; name = $selected.name }
            }

            if (-not $hasPausedProbe) {
                $null = Invoke-Bridge -Action pause -Guid $selectedGuid
                $mustResume = $true
                $paused = Invoke-Bridge -Action snapshot -Guid $selectedGuid
                if (-not $paused.bot.paused) { throw "Pause verification failed for bot $selectedGuid." }
                $null = Invoke-Bridge -Action resume -Guid $selectedGuid
                $mustResume = $false
                $resumed = Invoke-Bridge -Action snapshot -Guid $selectedGuid
                if ($resumed.bot.paused) { throw "Resume verification failed for bot $selectedGuid." }
                $hasPausedProbe = $true
                Write-AgentReceipt -Event 'pause_resume_probe_passed' -Fields @{ bot_guid = $selectedGuid; name = $resumed.bot.name }
            }

            $snapshotResponse = Invoke-Bridge -Action snapshot -Guid $selectedGuid
            $snapshot = $snapshotResponse.bot
            $position = $snapshot.position
            Write-AgentReceipt -Event 'snapshot' -Fields @{
                bot_guid = $selectedGuid
                name = $snapshot.name
                alive = [bool]$snapshot.alive
                combat = [bool]$snapshot.combat
                paused = [bool]$snapshot.paused
                map = $position.map
                x = $position.x
                y = $position.y
                z = $position.z
                travel_status = $snapshot.travel.status
                travel_destination = $snapshot.travel.destination
            }

            if (-not $NoTravel -and (Get-Date) -ge $travelDue) {
                if ($snapshot.alive -and -not $snapshot.combat -and -not $snapshot.paused) {
                    $order = Invoke-Bridge -Action travel -Guid $selectedGuid -Destination 'random'
                    Write-AgentReceipt -Event 'travel_random_issued' -Fields @{ bot_guid = $selectedGuid; name = $snapshot.name; mode = $order.mode }
                    $travelDue = (Get-Date).AddMinutes($TravelEveryMinutes)
                }
                else {
                    Write-AgentReceipt -Event 'travel_deferred' -Fields @{ bot_guid = $selectedGuid; alive = [bool]$snapshot.alive; combat = [bool]$snapshot.combat; paused = [bool]$snapshot.paused }
                    $travelDue = (Get-Date).AddMinutes(5)
                }
            }
            $consecutiveFailures = 0
        }
        catch {
            $consecutiveFailures++
            Write-AgentReceipt -Event 'bridge_error' -Fields @{ bot_guid = $selectedGuid; consecutive_failures = $consecutiveFailures; error = $_.Exception.Message }
            if ($consecutiveFailures -ge 3) { throw "AutoWow agent stopped after $consecutiveFailures consecutive bridge failures: $($_.Exception.Message)" }
        }
        Start-Sleep -Seconds $PollSeconds
    }
    Write-AgentReceipt -Event 'agent_completed' -Fields @{ bot_guid = $selectedGuid }
}
finally {
    if ($mustResume -and $selectedGuid -ne 0) {
        try {
            $null = Invoke-Bridge -Action resume -Guid $selectedGuid
            Write-AgentReceipt -Event 'safety_resume_issued' -Fields @{ bot_guid = $selectedGuid }
        }
        catch {
            Write-AgentReceipt -Event 'safety_resume_failed' -Fields @{ bot_guid = $selectedGuid; error = $_.Exception.Message }
        }
    }
    Write-AgentReceipt -Event 'agent_stopped' -Fields @{ bot_guid = $selectedGuid }
}
