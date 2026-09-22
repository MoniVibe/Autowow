[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ServerRoot,
    [ValidateRange(1, 10080)][int]$DurationMinutes = 480,
    [ValidateRange(1, 3600)][int]$PollSeconds = 30,
    [string]$ReceiptPath = '',
    [string]$ProbeDefinitionsJson = ''
)

Set-StrictMode -Version Latest
$libraryPath = Join-Path $PSScriptRoot 'overnight-instance-observer-lib.ps1'
. $libraryPath

function Resolve-OvernightInstanceObserverDefinitions {
    param([string]$Json)

    if ([string]::IsNullOrWhiteSpace($Json)) {
        return @(
            [pscustomobject][ordered]@{ id = 'dtk-party'; instance_id = 11; map_id = 600; expected_mask = 15 },
            [pscustomobject][ordered]@{ id = 'onyxia-raid'; instance_id = 12; map_id = 249; expected_mask = 1 },
            [pscustomobject][ordered]@{ id = 'nexus-party'; instance_id = 13; map_id = 576; expected_mask = 15 }
        )
    }
    try { $definitions = @($Json | ConvertFrom-Json -ErrorAction Stop) } catch { throw 'ProbeDefinitionsJson must be a JSON array.' }
    if ($definitions.Count -eq 0) { throw 'ProbeDefinitionsJson must contain at least one definition.' }
    foreach ($definition in $definitions) {
        $id = [string](Get-OvernightInstanceObserverProperty $definition 'id' '')
        $instanceId = ConvertTo-OvernightInstanceObserverInt (Get-OvernightInstanceObserverProperty $definition 'instance_id' 0)
        $expectedMask = ConvertTo-OvernightInstanceObserverInt -Value (Get-OvernightInstanceObserverProperty $definition 'expected_mask' $null) -Default -1
        if ($id -notmatch '^[A-Za-z0-9_.-]+$' -or $instanceId -le 0 -or $expectedMask -lt 0) { throw 'Each probe definition requires safe id, positive instance_id, and non-negative expected_mask.' }
    }
    return $definitions
}

function Write-OvernightInstanceObserverEvent {
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][string]$Event, [hashtable]$Fields = @{})

    $record = [ordered]@{ schema = 'autowow.overnight-instance-observer.v1'; timestamp_utc = [DateTime]::UtcNow.ToString('o'); event = $Event }
    foreach ($key in $Fields.Keys) { $record[$key] = $Fields[$key] }
    $line = $record | ConvertTo-Json -Compress -Depth 12
    [IO.File]::AppendAllText($Path, $line + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
}

function Get-OvernightInstanceObserverPreviousProbeState {
    param([AllowNull()][object]$State, [Parameter(Mandatory)][string]$ProbeId)
    foreach ($probe in @((Get-OvernightInstanceObserverProperty $State 'probes' @()))) {
        if ([string](Get-OvernightInstanceObserverProperty $probe 'probe_id' '') -eq $ProbeId) { return $probe }
    }
    return $null
}

function Get-OvernightInstanceObserverInstanceRows {
    param(
        [Parameter(Mandatory)][string]$ServerRoot,
        [Parameter(Mandatory)][object[]]$Definitions
    )

    $helperPath = Join-Path $PSScriptRoot 'raid-readiness-proof-lib.ps1'
    if (-not (Test-Path -LiteralPath $helperPath -PathType Leaf)) { throw 'Existing read-only database helper is missing.' }
    . $helperPath
    $connections = Get-RaidReadinessDatabaseConnections -ServerRoot $ServerRoot
    $mysql = Get-RaidReadinessMySqlPath -ServerRoot $ServerRoot
    $ids = @($Definitions | ForEach-Object { ConvertTo-OvernightInstanceObserverInt (Get-OvernightInstanceObserverProperty $_ 'instance_id' 0) } | Sort-Object -Unique)
    $sql = 'SELECT id AS instance_id, map AS map_id, completedEncounters AS completed_encounters FROM acore_characters.instance WHERE id IN (' + ($ids -join ',') + ')'
    Assert-OvernightInstanceObserverSelectOnlySql -Sql $sql
    $rows = @(Invoke-RaidReadinessReadOnlySql -Connection $connections.characters -Sql $sql -Headers @('instance_id', 'map_id', 'completed_encounters') -MysqlPath $mysql)
    return $rows
}

function Get-OvernightInstanceObserverState {
    param([Parameter(Mandatory)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return [pscustomobject][ordered]@{ version = 1; probes = @(); playerbots_cursor = $null; last_error = $null } }
    try { return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json -ErrorAction Stop } catch { throw 'Observer state file is invalid; refusing to continue without risking duplicate evidence.' }
}

function Set-OvernightInstanceObserverState {
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][object]$State)
    $temporary = "$Path.$PID.tmp"
    $json = $State | ConvertTo-Json -Compress -Depth 12
    [IO.File]::WriteAllText($temporary, $json, [Text.UTF8Encoding]::new($false))
    Move-Item -LiteralPath $temporary -Destination $Path -Force
}

$root = [IO.Path]::GetFullPath($ServerRoot)
$workDir = Join-Path $root 'work\overnight-instance-observer'
New-Item -ItemType Directory -Path $workDir -Force | Out-Null
$receipt = if ([string]::IsNullOrWhiteSpace($ReceiptPath)) { Join-Path $workDir 'overnight-instance-observer.jsonl' } else { [IO.Path]::GetFullPath($ReceiptPath) }
$statePath = Join-Path $workDir 'state.json'
$lockPath = Join-Path $workDir 'observer.lock'
$playerbotsPath = Join-Path $root 'logs\phase1-runtime\Playerbots.log'
$probePaths = @(Get-ChildItem -LiteralPath (Join-Path $root 'logs') -File -Filter 'probe-lab-live-*.jsonl' -ErrorAction SilentlyContinue | ForEach-Object FullName)
$definitions = @(Resolve-OvernightInstanceObserverDefinitions -Json $ProbeDefinitionsJson)
$state = Get-OvernightInstanceObserverState -Path $statePath
$lock = $null
$summary = [ordered]@{ polls = 0; mask_changes = 0; roster_changes = 0; loot_awards = 0; loot_equips = 0; complete = 0; errors = 0 }

try {
    $lock = Enter-OvernightInstanceObserverLock -LockPath $lockPath
    New-Item -ItemType Directory -Path (Split-Path -Parent $receipt) -Force | Out-Null
    Write-OvernightInstanceObserverEvent -Path $receipt -Event 'observer_started' -Fields ([ordered]@{ duration_minutes = $DurationMinutes; poll_seconds = $PollSeconds; probe_count = $definitions.Count; read_only = $true })
    if ($null -eq (Get-OvernightInstanceObserverProperty $state 'playerbots_cursor' $null) -and (Test-Path -LiteralPath $playerbotsPath -PathType Leaf)) {
        $state.playerbots_cursor = New-OvernightInstanceObserverLogCursor -Path $playerbotsPath
        Set-OvernightInstanceObserverState -Path $statePath -State $state
    }

    $deadline = (Get-Date).AddMinutes($DurationMinutes)
    while ((Get-Date) -lt $deadline) {
        $summary.polls++
        $probePaths = @(Get-ChildItem -LiteralPath (Join-Path $root 'logs') -File -Filter 'probe-lab-live-*.jsonl' -ErrorAction SilentlyContinue | ForEach-Object FullName)
        $samples = @(Get-OvernightInstanceObserverLatestProbeSamples -Paths $probePaths)
        $rows = @()
        $dbError = $null
        try { $rows = @(Get-OvernightInstanceObserverInstanceRows -ServerRoot $root -Definitions $definitions) } catch { $dbError = 'read_only_instance_query_failed' }
        $rowById = @{}
        foreach ($row in $rows) { $rowById[[string](ConvertTo-OvernightInstanceObserverInt (Get-OvernightInstanceObserverProperty $row 'instance_id' 0))] = $row }

        foreach ($definition in $definitions) {
            $probeId = [string]$definition.id
            $sample = @($samples | Where-Object { [string]$_.probe_id -eq $probeId } | Select-Object -First 1)
            $previous = Get-OvernightInstanceObserverPreviousProbeState -State $state -ProbeId $probeId
            $next = [ordered]@{ probe_id = $probeId; instance_id = [int]$definition.instance_id; mask = if ($null -ne $previous) { $previous.mask } else { $null }; roster = if ($null -ne $previous) { $previous.roster } else { $null }; complete = if ($null -ne $previous) { [bool]$previous.complete } else { $false }; last_sample_at = if ($null -ne $previous) { $previous.last_sample_at } else { $null } }
            if ($sample.Count -gt 0) {
                $sample = $sample[0]
                $roster = Get-OvernightInstanceObserverRosterState -Members @(Get-OvernightInstanceObserverProperty $sample 'members' @())
                if ($null -eq $previous -or ($roster | ConvertTo-Json -Compress) -ne ((Get-OvernightInstanceObserverProperty $previous 'roster' $null) | ConvertTo-Json -Compress)) {
                    Write-OvernightInstanceObserverEvent -Path $receipt -Event 'roster_state_changed' -Fields ([ordered]@{ probe_id = $probeId; old = if ($null -ne $previous) { $previous.roster } else { $null }; new = $roster })
                    $summary.roster_changes++
                }
                $next.roster = $roster
                $sampleTime = [string](Get-OvernightInstanceObserverProperty $sample 'observed_at_utc' '')
                $isNewSample = $sampleTime -ne [string](Get-OvernightInstanceObserverProperty $previous 'last_sample_at' '')
                if ($isNewSample) {
                    foreach ($navigator in @(Get-OvernightInstanceObserverProperty $sample 'navigator_events' @())) {
                        $navigatorText = ($navigator | ConvertTo-Json -Compress -Depth 6)
                        if ($navigatorText -match '(?i)(terminal|complete|completed|already_complete)') { Write-OvernightInstanceObserverEvent -Path $receipt -Event 'probe_terminal' -Fields ([ordered]@{ probe_id = $probeId; detail = $navigator }) }
                        elseif ($navigatorText -match '(?i)(stall|blocked|timeout|unreachable|cohesion|exhausted)') { Write-OvernightInstanceObserverEvent -Path $receipt -Event 'probe_stalled' -Fields ([ordered]@{ probe_id = $probeId; detail = $navigator }) }
                    }
                    $progressTimedOut = [bool](Get-OvernightInstanceObserverProperty $sample 'progress_timed_out' $false)
                    $admissionTimedOut = [bool](Get-OvernightInstanceObserverProperty $sample 'admission_timed_out' $false)
                    if ($progressTimedOut -or $admissionTimedOut) { Write-OvernightInstanceObserverEvent -Path $receipt -Event 'probe_stalled' -Fields ([ordered]@{ probe_id = $probeId; progress_timed_out = $progressTimedOut; admission_timed_out = $admissionTimedOut }) }
                }
                $next.last_sample_at = $sampleTime
            }
            $row = $rowById[[string][int]$definition.instance_id]
            if ($null -ne $row) {
                $mask = ConvertTo-OvernightInstanceObserverInt (Get-OvernightInstanceObserverProperty $row 'completed_encounters' 0)
                $delta = Get-OvernightInstanceObserverMaskDelta -OldMask (Get-OvernightInstanceObserverProperty $previous 'mask' $null) -NewMask $mask -ExpectedMask ([int]$definition.expected_mask)
                if ($null -eq $previous -or $delta.old_mask -ne $delta.new_mask) {
                    Write-OvernightInstanceObserverEvent -Path $receipt -Event 'encounter_mask_changed' -Fields ([ordered]@{ probe_id = $probeId; instance_id = [int]$definition.instance_id; map_id = [int]$definition.map_id; old_mask = $delta.old_mask; new_mask = $delta.new_mask; added_bits = $delta.added_bits; expected_mask = [int]$definition.expected_mask })
                    $summary.mask_changes++
                }
                $next.mask = $mask
                if ($delta.terminal -and -not [bool]$next.complete) {
                    Write-OvernightInstanceObserverEvent -Path $receipt -Event 'instance_complete' -Fields ([ordered]@{ probe_id = $probeId; instance_id = [int]$definition.instance_id; map_id = [int]$definition.map_id; completed_encounters = $mask; expected_mask = [int]$definition.expected_mask })
                    $next.complete = $true
                    $summary.complete++
                }
            }
            $state.probes = @(@($state.probes | Where-Object { [string]$_.probe_id -ne $probeId }) + [pscustomobject]$next)
        }

        $cursor = Get-OvernightInstanceObserverProperty $state 'playerbots_cursor' $null
        $delta = Read-OvernightInstanceObserverLogDelta -Path $playerbotsPath -Cursor $cursor
        foreach ($loot in @(Get-OvernightInstanceObserverRaidLootEvents -Lines $delta.lines)) {
            $eventName = if ($loot.source_event -eq 'award') { 'loot_award'; $summary.loot_awards++ } else { 'loot_equip'; $summary.loot_equips++ }
            Write-OvernightInstanceObserverEvent -Path $receipt -Event $eventName -Fields ([ordered]@{ source_event = $loot.source_event; fields = $loot.fields; raw_sha256 = $loot.raw_sha256 })
        }
        $state.playerbots_cursor = $delta.cursor
        if ($null -ne $dbError -and [string]$state.last_error -ne $dbError) {
            Write-OvernightInstanceObserverEvent -Path $receipt -Event 'observer_error' -Fields ([ordered]@{ code = $dbError; read_only = $true })
            $summary.errors++
        }
        $state.last_error = $dbError
        Set-OvernightInstanceObserverState -Path $statePath -State $state
        if ((Get-Date) -lt $deadline) { Start-Sleep -Seconds $PollSeconds }
    }
    Write-OvernightInstanceObserverEvent -Path $receipt -Event 'observer_summary' -Fields ([ordered]@{ status = 'completed'; summary = $summary; read_only = $true })
} catch {
    if ($null -ne $lock -and (Test-Path -LiteralPath $receipt -PathType Leaf)) {
        Write-OvernightInstanceObserverEvent -Path $receipt -Event 'observer_summary' -Fields ([ordered]@{ status = 'stopped_with_error'; summary = $summary; read_only = $true })
    }
    throw
} finally {
    if ($null -ne $lock -and (Test-Path -LiteralPath $receipt -PathType Leaf)) {
        Write-OvernightInstanceObserverEvent -Path $receipt -Event 'observer_stopped' -Fields ([ordered]@{ read_only = $true })
    }
    if ($null -ne $lock) { Exit-OvernightInstanceObserverLock -Lock $lock }
}
