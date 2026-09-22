[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [ValidateRange(1,1440)][int]$DurationMinutes = 480,
    [ValidateRange(5,300)][int]$PollSeconds = 20
)

. (Join-Path $PSScriptRoot 'Common.ps1')

$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
$monitor = Join-Path $PSScriptRoot 'league-simulation-monitor.ps1'
$pidPath = Join-Path $ServerRoot 'league-v0-monitor.pid.json'
if (-not (Test-Path -LiteralPath $monitor)) { throw "League monitor script is missing: $monitor" }
if (Test-Path -LiteralPath $pidPath) {
    $prior = Get-Content -LiteralPath $pidPath -Raw | ConvertFrom-Json
    if (Get-Process -Id ([int]$prior.process_id) -ErrorAction SilentlyContinue) { throw "League v0 monitor is already running with PID $($prior.process_id)." }
    Remove-Item -LiteralPath $pidPath -Force
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$stdout = Join-Path $ServerRoot "logs\league-v0-monitor-$stamp-stdout.log"
$stderr = Join-Path $ServerRoot "logs\league-v0-monitor-$stamp-stderr.log"
$args = @('-NoProfile','-ExecutionPolicy','Bypass','-File',$monitor,'-ServerRoot',$ServerRoot,'-DurationMinutes',$DurationMinutes,'-PollSeconds',$PollSeconds)
$process = Start-Process -FilePath (Get-Command powershell.exe -ErrorAction Stop).Source -ArgumentList $args -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
$state = [ordered]@{ process_id = $process.Id; started_utc = (Get-Date).ToUniversalTime().ToString('o'); duration_minutes = $DurationMinutes; poll_seconds = $PollSeconds; stdout = $stdout; stderr = $stderr }
[System.IO.File]::WriteAllText($pidPath, ($state | ConvertTo-Json), [System.Text.UTF8Encoding]::new($false))
Write-Output "League v0 monitor started with PID $($process.Id)."
