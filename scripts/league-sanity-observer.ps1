[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [ValidateRange(1,1440)][int]$DurationMinutes = 480,
    [ValidateRange(300,3600)][int]$PollSeconds = 900,
    [string]$ReceiptPath = ''
)

. (Join-Path $PSScriptRoot 'Common.ps1')

$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
$snapshot = Join-Path $PSScriptRoot 'league-sanity-snapshot.ps1'
if (-not (Test-Path -LiteralPath $snapshot)) { throw "League sanity snapshot script is missing: $snapshot" }
if ([string]::IsNullOrWhiteSpace($ReceiptPath)) {
    $ReceiptPath = Join-Path $ServerRoot ('leagues\results\league-v0-sanity-{0}.jsonl' -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
New-Item -ItemType Directory -Path (Split-Path -Parent $ReceiptPath) -Force | Out-Null
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)

function Write-Receipt {
    param([string]$Event, [hashtable]$Fields = @{})
    $record = [ordered]@{ timestamp_utc = (Get-Date).ToUniversalTime().ToString('o'); event = $Event }
    foreach ($key in $Fields.Keys) { $record[$key] = $Fields[$key] }
    [System.IO.File]::AppendAllText($ReceiptPath, (($record | ConvertTo-Json -Compress -Depth 8) + [Environment]::NewLine), $utf8NoBom)
}

$deadline = (Get-Date).AddMinutes($DurationMinutes)
$failures = 0
Write-Receipt -Event 'sanity_observer_started' -Fields @{ duration_minutes = $DurationMinutes; poll_seconds = $PollSeconds }
try {
    while ((Get-Date) -lt $deadline) {
        try {
            & $snapshot -ServerRoot $ServerRoot -ReceiptPath $ReceiptPath | Out-Null
            $failures = 0
        }
        catch {
            $failures++
            Write-Receipt -Event 'sanity_observer_error' -Fields @{ consecutive_failures = $failures; error = $_.Exception.Message; stack = $_.ScriptStackTrace }
            if ($failures -ge 3) { throw "Sanity observer stopped after $failures consecutive failures: $($_.Exception.Message)" }
        }
        Start-Sleep -Seconds $PollSeconds
    }
    Write-Receipt -Event 'sanity_observer_completed'
}
finally {
    Write-Receipt -Event 'sanity_observer_stopped'
}

Write-Output "League sanity observer receipt: $ReceiptPath"
