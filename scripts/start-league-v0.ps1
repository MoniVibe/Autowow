[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [switch]$SkipBuild
)

. (Join-Path $PSScriptRoot 'Common.ps1')

$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
$config = Join-Path $ServerRoot 'server\configs\modules\playerbots.conf'
$backupRoot = Join-Path $ServerRoot 'logs'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$backup = Join-Path $backupRoot "playerbots-before-league-v0-$stamp.conf"

foreach ($required in @($config, (Join-Path $PSScriptRoot 'league-simulation.ps1'), (Join-Path $PSScriptRoot 'stop-server.ps1'), (Join-Path $PSScriptRoot 'start-server.ps1'))) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Required AutoWow file is missing: $required" }
}

New-Item -ItemType Directory -Path $backupRoot -Force | Out-Null
Copy-Item -LiteralPath $config -Destination $backup -ErrorAction Stop

# A clean campaign slice parks the 80 stress-test population. The seed roster is manually
# activated through the localhost bridge and is the only active bot population in this mode.
& (Join-Path $PSScriptRoot 'stop-autowow-agent.ps1') -ServerRoot $ServerRoot 2>$null
& (Join-Path $PSScriptRoot 'stop-server.ps1') -ServerRoot $ServerRoot

if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'build.ps1') -ServerRoot $ServerRoot -Configuration RelWithDebInfo -Parallel 2
}

Set-ConfigValue -Path $config -Key 'AiPlayerbot.MaxRandomBots' -Value '0'
Set-ConfigValue -Path $config -Key 'AiPlayerbot.MinRandomBots' -Value '0'
Set-ConfigValue -Path $config -Key 'AiPlayerbot.RandomBotFixedLevel' -Value '0'
Set-ConfigValue -Path $config -Key 'AiPlayerbot.RandomBotJoinLfg' -Value '0'
Set-ConfigValue -Path $config -Key 'AiPlayerbot.RandomBotJoinBG' -Value '0'
Set-ConfigValue -Path $config -Key 'AiPlayerbot.RandomBotAutoJoinBG' -Value '0'
Set-ConfigValue -Path $config -Key 'AiPlayerbot.RandomBotAutoJoinBGWSCount' -Value '0'
Set-ConfigValue -Path $config -Key 'AiPlayerbot.RandomBotAutoJoinBGAVCount' -Value '0'
Set-ConfigValue -Path $config -Key 'AiPlayerbot.RandomBotGuildCount' -Value '0'
Set-ConfigValue -Path $config -Key 'AiPlayerbot.BotCheats' -Value '"taxi,raid"'

& (Join-Path $PSScriptRoot 'start-server.ps1') -ServerRoot $ServerRoot

$simulation = Join-Path $PSScriptRoot 'league-simulation.ps1'
$bridge = Join-Path $PSScriptRoot 'autowow-control.ps1'
$deadline = (Get-Date).AddSeconds(120)
$bridgeReady = $false
while ((Get-Date) -lt $deadline) {
    try {
        $raw = @(& $bridge -Action list | ForEach-Object { $_.ToString() }) | Select-Object -Last 1
        if ($raw) {
            $response = $raw | ConvertFrom-Json
            if ($response.ok) { $bridgeReady = $true; break }
        }
    }
    catch { }
    Start-Sleep -Seconds 2
}
if (-not $bridgeReady) { throw 'worldserver started but the AutoWow bridge was not ready within 120 seconds.' }

& $simulation -Action install -ServerRoot $ServerRoot
& $simulation -Action launch -ServerRoot $ServerRoot
& $simulation -Action status -ServerRoot $ServerRoot

Write-Output "League v0 started. Playerbots configuration backup: $backup"
