[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [ValidateRange(1,1440)][int]$DurationMinutes = 480,
    [ValidateRange(10,300)][int]$ScoutEverySeconds = 20,
    [ValidateRange(60,900)][int]$NoProgressSeconds = 120,
    [ValidateRange(1,5)][int]$MaxRecoveriesPerQuest = 2
)

. (Join-Path $PSScriptRoot 'Common.ps1')

$ErrorActionPreference = 'Stop'
$ServerRoot = (Resolve-Path -LiteralPath $ServerRoot).Path
$director = Join-Path $PSScriptRoot 'league-simulation-director.ps1'
$pidPath = Join-Path $ServerRoot 'league-v0-director.pid.json'
if (-not (Test-Path -LiteralPath $director)) { throw "League director script is missing: $director" }
if (Test-Path -LiteralPath $pidPath) {
    $prior = Get-Content -LiteralPath $pidPath -Raw | ConvertFrom-Json
    if (Get-Process -Id ([int]$prior.process_id) -ErrorAction SilentlyContinue) { throw "League v0 director is already running with PID $($prior.process_id)." }
    Remove-Item -LiteralPath $pidPath -Force
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$stdout = Join-Path $ServerRoot "logs\league-v0-director-$stamp-stdout.log"
$stderr = Join-Path $ServerRoot "logs\league-v0-director-$stamp-stderr.log"
$args = @('-NoProfile','-ExecutionPolicy','Bypass','-File',$director,'-ServerRoot',$ServerRoot,'-DurationMinutes',$DurationMinutes,'-ScoutEverySeconds',$ScoutEverySeconds,'-NoProgressSeconds',$NoProgressSeconds,'-MaxRecoveriesPerQuest',$MaxRecoveriesPerQuest)
$process = Start-Process -FilePath (Get-Command powershell.exe -ErrorAction Stop).Source -ArgumentList $args -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
$state = [ordered]@{ process_id = $process.Id; started_utc = (Get-Date).ToUniversalTime().ToString('o'); duration_minutes = $DurationMinutes; quest_every_seconds = $ScoutEverySeconds; no_progress_seconds = $NoProgressSeconds; max_recoveries_per_quest = $MaxRecoveriesPerQuest; stdout = $stdout; stderr = $stderr }
[System.IO.File]::WriteAllText($pidPath, ($state | ConvertTo-Json), [System.Text.UTF8Encoding]::new($false))
Write-Output "League v0 director started with PID $($process.Id)."
