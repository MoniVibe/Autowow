[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ManifestPath,
    [ValidateRange(1,480)][int]$DurationMinutes=120,
    [ValidateRange(1,300)][int]$PollSeconds=5,
    [ValidateRange(30,3600)][int]$StallSeconds=180,
    [ValidateRange(60,3600)][int]$QuestNoCreditSeconds=600,
    [string]$ServerRoot=(Split-Path -Parent $PSScriptRoot)
)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'zone-scout-lib.ps1')
$ServerRoot=(Resolve-Path -LiteralPath $ServerRoot).Path
$ManifestPath=(Resolve-Path -LiteralPath $ManifestPath).Path
$control=Join-Path $PSScriptRoot 'autowow-control.ps1'
if (-not (Test-Path -LiteralPath $control)) { throw 'AutoWow read-only control script not found.' }
$manifest=Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json -AsHashtable
if ($manifest.schema -ne 'autowow.zone-scout.manifest.v1') { throw 'Unexpected zone scout manifest schema.' }
$scouts=@($manifest.scouts)
if ($scouts.Count -lt 1 -or $scouts.Count -gt 32) { throw 'Manifest requires 1 to 32 scouts.' }
$guids=[System.Collections.Generic.HashSet[uint32]]::new()
foreach ($scout in $scouts) {
    if (-not $scout.ContainsKey('guid') -or [uint32]$scout.guid -eq 0 -or
        [string]::IsNullOrWhiteSpace([string]$scout.name) -or
        [string]::IsNullOrWhiteSpace([string]$scout.home_zone) -or
        -not $guids.Add([uint32]$scout.guid)) {
        throw 'Each scout requires a unique positive guid, name, and home_zone.'
    }
}
$base=Join-Path $ServerRoot 'logs\zone-scouts'
New-Item -ItemType Directory -Path $base -Force | Out-Null
$lockPath=Join-Path $base 'monitor.lock'
try {
    $lock=[IO.FileStream]::new($lockPath,[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
} catch [IO.IOException] {
    throw 'Another zone scout monitor holds the exclusive monitor lock.'
}
try {
$runId=(Get-Date).ToUniversalTime().ToString('yyyyMMdd-HHmmss')+'-'+[guid]::NewGuid().ToString('N').Substring(0,8)
$runPath=Join-Path $base $runId
$null=New-Item -ItemType Directory -Path $runPath -ErrorAction Stop
$samplesPath=Join-Path $runPath 'samples.jsonl'
$incidentsPath=Join-Path $runPath 'incidents.jsonl'
$summaryPath=Join-Path $runPath 'summary.json'
$markdownPath=Join-Path $runPath 'summary.md'
$latestPath=Join-Path $base 'latest.json'
$utf8=[Text.UTF8Encoding]::new($false)
[IO.File]::WriteAllText($samplesPath,'',$utf8)
[IO.File]::WriteAllText($incidentsPath,'',$utf8)
$started=[datetime]::UtcNow
$deadline=$started.AddMinutes($DurationMinutes)
$states=@{}
foreach ($scout in $scouts) { $states[[uint32]$scout.guid]=New-ZoneScoutState $scout $started }
} catch { $lock.Dispose(); throw }
$sampleCount=0
$incidentEventCount=0
$errorCount=0
$runStatus='running'
$lastError=$null

function Write-JsonLine([string]$Path,$Record) {
    $json=ConvertTo-Json -InputObject $Record -Compress -Depth 18
    [IO.File]::AppendAllText($Path,$json+[Environment]::NewLine,$utf8)
}
function Read-ControlJson([string]$Action,[uint32]$Guid=0) {
    $args=@{ Action=$Action; TimeoutMs=5000 }
    if ($Guid) { $args.BotGuid=$Guid }
    $raw=(& $control @args | Out-String).Trim()
    if ([string]::IsNullOrWhiteSpace($raw)) { throw "$Action returned no telemetry." }
    $value=$raw | ConvertFrom-Json
    if (-not $value.ok) { throw "$Action reported unavailable telemetry." }
    return $value
}
function Publish-Summary {
    $now=[datetime]::UtcNow
    $scoutSummaries=@(foreach ($scout in $scouts) { Get-ZoneScoutSummary $states[[uint32]$scout.guid] })
    $suspect=@($scoutSummaries | Where-Object { $_.status -eq 'SUSPECT' }).Count
    $offline=@($scoutSummaries | Where-Object { $_.status -eq 'OFFLINE' }).Count
    $dead=@($scoutSummaries | Where-Object { $_.status -eq 'DEAD' }).Count
    $summary=[ordered]@{
        schema='autowow.zone-scout.summary.v1'; run_id=$runId; status=$runStatus
        started_utc=$started.ToString('o'); updated_utc=$now.ToString('o')
        ended_utc=if ($runStatus -eq 'running') {$null} else {$now.ToString('o')}
        manifest_path=$ManifestPath; duration_minutes=$DurationMinutes; poll_seconds=$PollSeconds
        stall_seconds=$StallSeconds; quest_no_credit_seconds=$QuestNoCreditSeconds
        sample_count=$sampleCount; incident_event_count=$incidentEventCount; telemetry_error_count=$errorCount
        last_error=$lastError; scout_count=$scouts.Count; suspect_count=$suspect
        offline_count=$offline; dead_count=$dead; scouts=$scoutSummaries
        evidence_note='Home zone is a manifest label; observed map, zone, area, instance, and movement speed values come from bridge telemetry when present, otherwise null.'
    }
    Write-ZoneScoutAtomicJson $summaryPath $summary
    $lines=[System.Collections.Generic.List[string]]::new()
    $lines.Add("# Zone scout observation: $runId")
    $lines.Add('')
    $lines.Add("Status: **$runStatus**. Updated: $($now.ToString('o')). Samples: $sampleCount. Telemetry errors: $errorCount.")
    $lines.Add('')
    $lines.Add('| Scout | Home label | Observed map | State | Last confirmed progress | Active suspicion |')
    $lines.Add('|---|---|---:|---|---|---|')
    foreach ($scoutSummary in $scoutSummaries) {
        $last=@($scoutSummary.breadcrumbs | Select-Object -Last 1)
        $map=if ($last.Count -and $null -ne $last[0].map) { [string]$last[0].map } else { '?' }
        $active=if ($scoutSummary.active_incidents.Count) { (@($scoutSummary.active_incidents.Keys | Sort-Object) -join ', ') } else { 'none' }
        $lines.Add("| $($scoutSummary.name) ($($scoutSummary.guid)) | $($scoutSummary.home_zone) | $map | $($scoutSummary.status) | $($scoutSummary.last_progress_utc) | $active |")
    }
    $lines.Add('')
    $lines.Add('SUSPECT means the observer saw a bounded no-progress pattern; OBSERVING does not certify that a zone passed. See incidents.jsonl for first failure context and rolling breadcrumbs.')
    $temp=Join-Path $runPath ('.summary.md.'+[guid]::NewGuid().ToString('N')+'.tmp')
    [IO.File]::WriteAllLines($temp,$lines,$utf8)
    [IO.File]::Move($temp,$markdownPath,$true)
    Write-ZoneScoutAtomicJson $latestPath ([ordered]@{
        schema='autowow.zone-scout.latest.v1'; run_id=$runId; run_path=$runPath
        summary_path=$summaryPath; status=$runStatus; updated_utc=$now.ToString('o')
        suspect_count=$suspect; offline_count=$offline; dead_count=$dead; telemetry_error_count=$errorCount
        scouts=@(foreach ($item in $scoutSummaries) { [ordered]@{ guid=$item.guid; name=$item.name; status=$item.status; observed_map=$item.observed_map; observed_zone_id=$item.observed_zone_id; quest_id=$item.quest_context_id; last_progress_utc=$item.last_progress_utc; quest_last_credit_utc=$item.quest_last_credit_utc; active_incident_kinds=@($item.active_incidents.Keys | Sort-Object) } })
    })
}
try {
    Publish-Summary
    while ([datetime]::UtcNow -lt $deadline) {
        $now=[datetime]::UtcNow
        try {
            $roster=Read-ControlJson 'list'
            $bots=@{}
            foreach ($bot in @($roster.bots)) { $bots[[uint32]$bot.guid]=$bot }
            foreach ($scout in $scouts) {
                $guid=[uint32]$scout.guid
                $bot=if ($bots.ContainsKey($guid)) { $bots[$guid] } else { $null }
                $questObjective=$null
                $questLog=$null
                if ($bot) {
                    try {
                        $questObjective=Read-ControlJson 'questobjective' $guid
                        $questLog=Read-ControlJson 'questlog' $guid
                    } catch {
                        $errorCount++
                        $lastError="Scout $guid diagnostic query failed: $($_.Exception.Message)"
                        Write-JsonLine $samplesPath ([ordered]@{ schema='autowow.zone-scout.sample-error.v1'; at_utc=$now.ToString('o'); guid=$guid; error=$lastError })
                        continue
                    }
                }
                $sample=ConvertTo-ZoneScoutSample $scout $bot $questObjective $questLog $now
                $sample.schema='autowow.zone-scout.sample.v1'
                Write-JsonLine $samplesPath $sample
                $sampleCount++
                $events=@(Update-ZoneScoutState $states[$guid] $sample $now $StallSeconds $QuestNoCreditSeconds)
                foreach ($event in $events) { Write-JsonLine $incidentsPath $event; $incidentEventCount++ }
            }
        } catch {
            $errorCount++
            $lastError="List telemetry failed: $($_.Exception.Message)"
            Write-JsonLine $samplesPath ([ordered]@{ schema='autowow.zone-scout.sample-error.v1'; at_utc=$now.ToString('o'); error=$lastError })
        }
        Publish-Summary
        $remaining=($deadline-[datetime]::UtcNow).TotalSeconds
        if ($remaining -gt 0) { Start-Sleep -Seconds ([math]::Min($PollSeconds,$remaining)) }
    }
    $runStatus='completed'
}
catch {
    $runStatus='failed'
    $lastError=$_.Exception.Message
    throw
}
finally {
    try { Publish-Summary } finally { $lock.Dispose() }
}
Write-Output "Zone scout summary: $summaryPath"




