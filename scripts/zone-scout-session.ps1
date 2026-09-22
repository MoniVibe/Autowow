[CmdletBinding()]
param(
    [ValidateSet('Status','Start','Stop')][string]$Action='Status',
    [ValidateRange(1,480)][int]$DurationMinutes=120,
    [string]$ServerRoot=(Split-Path -Parent $PSScriptRoot)
)
$ErrorActionPreference='Stop'
$ServerRoot=(Resolve-Path -LiteralPath $ServerRoot).Path
$monitor=Join-Path $PSScriptRoot 'zone-scout-monitor.ps1'
$manifest=Join-Path $PSScriptRoot 'fixtures\zone-scouts-20260922.json'
$base=Join-Path $ServerRoot 'logs\zone-scouts'
$recordPath=Join-Path $base 'monitor-process.json'
$latestPath=Join-Path $base 'latest.json'
$pausePath=Join-Path $base 'operator-paused.json'
$record=$null
$ownedProcess=$null
if (Test-Path -LiteralPath $recordPath) {
    $record=Get-Content -LiteralPath $recordPath -Raw | ConvertFrom-Json
    $candidate=Get-CimInstance Win32_Process -Filter "ProcessId = $([int]$record.pid)"
    if ($candidate -and
        $candidate.Name -ieq 'pwsh.exe' -and
        $candidate.CommandLine -like ('*"'+$monitor+'"*') -and
        ([datetime]$candidate.CreationDate).ToUniversalTime().Ticks -eq ([datetime]$record.created_utc).Ticks) {
        $ownedProcess=$candidate
    }
}
if ($Action -eq 'Stop') {
    if ($ownedProcess) { Stop-Process -Id $ownedProcess.ProcessId -ErrorAction Stop }
    elseif ($record -and $candidate) { throw 'The recorded PID now belongs to a different process; nothing was stopped.' }
    $null=New-Item -ItemType Directory -Path $base -Force
    @{ paused_utc=[datetime]::UtcNow.ToString('o'); reason='Stopped explicitly through zone-scout-session.ps1' } | ConvertTo-Json | Set-Content -LiteralPath $pausePath -Encoding utf8
    [ordered]@{ running=$false; stopped=[bool]$ownedProcess; paused_by_operator=$true; note='Bots and worldserver continue running. The heartbeat must honor this pause. Start resumes observation.' } | ConvertTo-Json
    return
}
if ($Action -eq 'Start' -and (Test-Path -LiteralPath $pausePath)) { Remove-Item -LiteralPath $pausePath }
if ($Action -eq 'Start' -and -not $ownedProcess) {
    foreach ($required in @($monitor,$manifest)) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "Missing scout file: $required" }
    }
    $null=New-Item -ItemType Directory -Path $base -Force
    $launchId=[datetime]::UtcNow.ToString('yyyyMMdd-HHmmss')+'-'+[guid]::NewGuid().ToString('N').Substring(0,8)
    $stdout=Join-Path $base ("launcher-$launchId.out.log")
    $stderr=Join-Path $base ("launcher-$launchId.err.log")
    $oldRun=if (Test-Path -LiteralPath $latestPath) { (Get-Content -LiteralPath $latestPath -Raw | ConvertFrom-Json).run_id } else { '' }
    $arguments=@('-NoProfile','-File',('"'+$monitor+'"'),'-ManifestPath',('"'+$manifest+'"'),'-DurationMinutes',[string]$DurationMinutes,'-ServerRoot',('"'+$ServerRoot+'"'))
    $launched=Start-Process -FilePath (Join-Path $PSHOME 'pwsh.exe') -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $ownedProcess=Get-CimInstance Win32_Process -Filter "ProcessId = $($launched.Id)"
    if (-not $ownedProcess) { throw "Observer exited during launch; inspect $stderr" }
    $record=[ordered]@{ schema='autowow.zone-scout.process.v1'; pid=$launched.Id; created_utc=([datetime]$ownedProcess.CreationDate).ToUniversalTime().ToString('o'); script=$monitor; manifest=$manifest; duration_minutes=$DurationMinutes; stdout=$stdout; stderr=$stderr }
    $record | ConvertTo-Json | Set-Content -LiteralPath $recordPath -Encoding utf8
    $published=$false
    $deadline=[datetime]::UtcNow.AddSeconds(15)
    do {
        $launched.Refresh()
        if ($launched.HasExited) { throw "Observer exited with code $($launched.ExitCode); inspect $stderr" }
        if (Test-Path -LiteralPath $latestPath) {
            $latest=Get-Content -LiteralPath $latestPath -Raw | ConvertFrom-Json
            $published=$latest.run_id -ne $oldRun -and $latest.status -eq 'running'
        }
        if (-not $published) { Start-Sleep -Milliseconds 250 }
    } while (-not $published -and [datetime]::UtcNow -lt $deadline)
    if (-not $published) { throw "Observer process started but has not published a run yet; inspect $stderr before retrying." }
}
$latest=if (Test-Path -LiteralPath $latestPath) { Get-Content -LiteralPath $latestPath -Raw | ConvertFrom-Json } else { $null }
[ordered]@{ running=[bool]$ownedProcess; paused_by_operator=(Test-Path -LiteralPath $pausePath); pid=if ($ownedProcess) { [int]$ownedProcess.ProcessId } else { $null }; latest=$latest } | ConvertTo-Json -Depth 8
