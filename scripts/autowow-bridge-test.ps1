[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [uint32]$BotGuid = 0,
    [ValidateRange(5, 60)][int]$TravelSettleSeconds = 10
)

. (Join-Path $PSScriptRoot 'Common.ps1')

$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
Initialize-AutoWoWLayout -Root $ServerRoot
$control = Join-Path $PSScriptRoot 'autowow-control.ps1'
if (-not (Test-Path -LiteralPath $control)) { throw "AutoWow bridge client is missing: $control" }
$logPath = Join-Path $ServerRoot ('logs\\autowow-bridge-test-{0}.log' -f (Get-Date -Format 'yyyyMMdd-HHmmss'))

function Invoke-BridgeTestCommand {
    param([ValidateSet('list','snapshot','pause','resume','travel')][string]$Action, [uint32]$Guid = 0, [string]$Destination = '')
    $response = switch ($Action) {
        'list' { & $control -Action list }
        'snapshot' { & $control -Action snapshot -BotGuid $Guid }
        'pause' { & $control -Action pause -BotGuid $Guid }
        'resume' { & $control -Action resume -BotGuid $Guid }
        'travel' { & $control -Action travel -BotGuid $Guid -Destination $Destination }
    }
    $raw = (@($response | ForEach-Object { $_.ToString() }) | Select-Object -Last 1)
    $parsed = $raw | ConvertFrom-Json
    if (-not $parsed.ok) { throw "Bridge '$Action' failed: $($parsed.error)" }
    return $parsed
}

$selectedGuid = 0
$mustResume = $false
try {
    $list = Invoke-BridgeTestCommand -Action list
    $candidate = if ($BotGuid -ne 0) {
        @($list.bots | Where-Object { [uint32]$_.guid -eq $BotGuid } | Select-Object -First 1)[0]
    }
    else {
        @($list.bots | Where-Object { $_.alive -and -not $_.combat -and -not $_.paused } | Select-Object -First 1)[0]
    }
    if (-not $candidate) { throw 'No living non-combat bot is available for a non-disruptive bridge test.' }
    $selectedGuid = [uint32]$candidate.guid
    $before = Invoke-BridgeTestCommand -Action snapshot -Guid $selectedGuid
    $null = Invoke-BridgeTestCommand -Action pause -Guid $selectedGuid
    $mustResume = $true
    $paused = Invoke-BridgeTestCommand -Action snapshot -Guid $selectedGuid
    if (-not $paused.bot.paused) { throw "Pause did not hold for bot $selectedGuid." }
    $null = Invoke-BridgeTestCommand -Action resume -Guid $selectedGuid
    $mustResume = $false
    $resumed = Invoke-BridgeTestCommand -Action snapshot -Guid $selectedGuid
    if ($resumed.bot.paused) { throw "Resume did not clear pause for bot $selectedGuid." }
    $travel = Invoke-BridgeTestCommand -Action travel -Guid $selectedGuid -Destination 'random'
    Start-Sleep -Seconds $TravelSettleSeconds
    $after = Invoke-BridgeTestCommand -Action snapshot -Guid $selectedGuid
    $moved = ([int]$before.bot.position.map -ne [int]$after.bot.position.map) -or ([math]::Abs([double]$before.bot.position.x - [double]$after.bot.position.x) -gt 1.0) -or ([math]::Abs([double]$before.bot.position.y - [double]$after.bot.position.y) -gt 1.0)
    if (-not $moved) { throw "Random travel order for bot $selectedGuid did not change the observed position." }

    $report = @"
# AutoWow bridge control test

Generated: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')

| Check | Result |
|---|---|
| Selected bot | $($after.bot.name) ($selectedGuid) |
| Pause | PASS |
| Resume | PASS |
| Random travel order | PASS ($($travel.mode)) |
| Position changed | PASS |

Before: map $($before.bot.position.map), $($before.bot.position.x), $($before.bot.position.y), $($before.bot.position.z)

After: map $($after.bot.position.map), $($after.bot.position.x), $($after.bot.position.y), $($after.bot.position.z)
"@
    [System.IO.File]::WriteAllText((Join-Path $ServerRoot 'AUTOWOW_BRIDGE_TEST_REPORT.md'), $report, [System.Text.UTF8Encoding]::new($false))
    Add-Content -LiteralPath $logPath -Value "PASS bot=$selectedGuid moved=$moved"
    Write-Output "Bridge control test passed. Report: $(Join-Path $ServerRoot 'AUTOWOW_BRIDGE_TEST_REPORT.md')"
}
finally {
    if ($mustResume -and $selectedGuid -ne 0) {
        try { $null = Invoke-BridgeTestCommand -Action resume -Guid $selectedGuid } catch { Add-Content -LiteralPath $logPath -Value "SAFETY_RESUME_FAILED $($_.Exception.Message)" }
    }
}
