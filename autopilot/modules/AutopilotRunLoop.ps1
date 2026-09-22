# AutoWoW Autopilot V1.2 - persistent foreground run loop.
# Deterministic cadence, graceful shutdown (finally runs on Ctrl+C), pid lock,
# per-tick capability-manifest refresh, server-session-change detection, atomic
# checkpointing, startup reconciliation, clean ownership release on shutdown.
# Dry-run only: a live loop is refused until Sol enables the supervised quester.

Set-StrictMode -Version Latest

function Invoke-AutopilotRunTick {
    # One controller tick: refresh capability evidence, detect server-session
    # changes, run the scheduler, checkpoint. Deterministic under a fixture clock.
    param(
        [Parameter(Mandatory)]$Paths,
        [Parameter(Mandatory)]$Clock,
        [string]$FixtureRoot,
        [string]$CapabilityManifest,
        $ProfileResolver,
        [int]$TickNumber = 0
    )
    $now = Get-AutopilotClockNow -Clock $Clock
    # Manifest refresh: re-read every tick so a Sol-published change (deploy,
    # session restart) is picked up without restarting the controller.
    $provider = if ($CapabilityManifest) { New-AutopilotCapabilityProvider -Kind manifest -ManifestPath $CapabilityManifest }
    else { Resolve-AutopilotCapabilityProvider -Paths $Paths }

    $checkpoint = Get-AutopilotCheckpoint -Paths $Paths
    $previous = $null
    if ($checkpoint -and $checkpoint.Contains('session') -and $checkpoint.session) {
        $previous = [pscustomobject]@{
            Known     = [bool]$checkpoint.session.known
            Build     = [string]$checkpoint.session.build
            SessionId = [string]$checkpoint.session.session_id
        }
    }
    $current = Get-AutopilotSessionIdentity -Provider $provider
    $sessionChanged = Test-AutopilotSessionChanged -Previous $previous -Current $current

    if ($sessionChanged) {
        $null = Write-AutopilotReceipt -Paths $Paths -JobId 'controller' -Event 'server_session_changed' -Data @{
            previous_build      = $previous.Build
            previous_session_id = $previous.SessionId
            current_build       = $current.Build
            current_session_id  = $current.SessionId
        } -Now $now
        # Server-side leases are dead; the ownership FILE stays valid (it is
        # controller-scoped, not server-scoped). In-flight jobs re-validate.
        foreach ($job in (Get-AutopilotJobs -Paths $Paths)) {
            if ($job.phase -in @('Preparing', 'Traveling', 'Executing', 'Verifying', 'Recovering')) {
                $job.leaseRef = $null
                $job = Save-AutopilotJob -Paths $Paths -Job $job
                $job = Invoke-AutopilotTransition -Paths $Paths -Job $job -To 'Blocked' -Reason 'server session changed' -Blocked @{
                    code       = 'stale-state'
                    detail     = 'server session changed ({0}/{1} -> {2}/{3}); revalidating against the new session' -f $previous.Build, $previous.SessionId, $current.Build, $current.SessionId
                    nextAction = 'automatic re-validation next tick'
                } -Now $now
            }
        }
    }

    $tickParams = @{ Paths = $Paths; CapabilityProvider = $provider; Now = $now }
    if ($FixtureRoot) { $tickParams['FixtureRoot'] = $FixtureRoot }
    if ($ProfileResolver) { $tickParams['ProfileResolver'] = $ProfileResolver }
    $scheduled = Invoke-AutopilotScheduledTick @tickParams

    $actionCounts = [ordered]@{}
    foreach ($result in @($scheduled.Results)) {
        $key = [string]$result.action
        $actionCounts[$key] = 1 + $(if ($actionCounts.Contains($key)) { [int]$actionCounts[$key] } else { 0 })
    }
    $null = Save-AutopilotCheckpoint -Paths $Paths -Data @{
        tick                = $TickNumber
        clock_kind          = [string]$Clock.Kind
        sim_now             = ConvertTo-AutopilotTimestamp -Time $now
        session             = [ordered]@{ known = [bool]$current.Known; build = $current.Build; session_id = $current.SessionId }
        capability_provider = [string]$provider.Kind
        manifest_source     = $provider.Source
        action_counts       = $actionCounts
    } -Now $now

    [pscustomobject]@{
        Now            = $now
        TickNumber     = $TickNumber
        SessionChanged = $sessionChanged
        Provider       = $provider
        Scheduled      = $scheduled
    }
}

function Start-AutopilotRun {
    <#
    Foreground persistent controller. Defaults: dry-run, real clock, 30 s ticks,
    run until stop.request / Ctrl+C / -Once / -DurationMinutes elapse.
    The finally block releases every lease this instance holds and drops the pid
    lock - Ctrl+C lands here too. After a crash (no finally), lease TTLs expire
    on their own and the next start reconciles from the receipt ledger.
    #>
    param(
        [Parameter(Mandatory)]$Paths,
        [switch]$Once,
        [ValidateRange(0, 10080)][int]$DurationMinutes = 0,
        [ValidateRange(1, 3600)][int]$TickSeconds = 30,
        [switch]$FixtureClock,
        [datetime]$FixtureStartUtc,
        [string]$FixtureRoot,
        [string]$CapabilityManifest,
        $ProfileResolver,
        [bool]$DryRun = $true,
        [ValidateRange(0, 100000)][int]$MaxTicks = 0,
        # Exit as soon as no job can make progress: everything terminal,
        # suspended, or blocked on a code with no self-heal (party contract,
        # timeouts, exhausted recovery, unsupported kind). The decisive-run
        # switch for short supervised proofs.
        [switch]$StopOnQuiescence
    )
    if (-not $DryRun) {
        throw 'Live run loops are not enabled in V1.2: the worldserver is in maintenance and the supervised live quester requires Sol-published capability evidence, enrollment results, and disjoint GUID assignment first.'
    }
    $clock = if ($FixtureClock) {
        $start = if ($PSBoundParameters.ContainsKey('FixtureStartUtc')) { $FixtureStartUtc } else { (Get-Date).ToUniversalTime() }
        New-AutopilotClock -Kind fixture -StartUtc $start -StepSeconds $TickSeconds
    }
    else { New-AutopilotClock -Kind real }

    Enter-AutopilotLock -Paths $Paths
    $tickNumber = 0
    $stopReason = 'unknown'
    try {
        $null = Invoke-AutopilotReconcile -Paths $Paths
        $null = Write-AutopilotReceipt -Paths $Paths -JobId 'controller' -Event 'run_started' -Data @{
            mode             = 'dry-run'
            clock            = [string]$clock.Kind
            tick_seconds     = $TickSeconds
            duration_minutes = $DurationMinutes
            once             = [bool]$Once
        } -Now (Get-AutopilotClockNow -Clock $clock)

        $startUtc = Get-AutopilotClockNow -Clock $clock
        $endAt = if ($DurationMinutes -gt 0) { $startUtc.AddMinutes($DurationMinutes) } else { $null }
        $stopFile = Join-Path $Paths.StateRoot 'stop.request'

        while ($true) {
            $tickNumber++
            $tickParams = @{ Paths = $Paths; Clock = $clock; TickNumber = $tickNumber }
            if ($FixtureRoot) { $tickParams['FixtureRoot'] = $FixtureRoot }
            if ($CapabilityManifest) { $tickParams['CapabilityManifest'] = $CapabilityManifest }
            if ($ProfileResolver) { $tickParams['ProfileResolver'] = $ProfileResolver }
            $null = Invoke-AutopilotRunTick @tickParams

            if ($Once) { $stopReason = 'once'; break }
            if ($StopOnQuiescence) {
                $healableCodes = @('oracle-capability-not-deployed', 'capability-missing', 'roster-owned', 'prerequisite-unmet')
                $activeJobs = @(Get-AutopilotJobs -Paths $Paths | Where-Object {
                        ($_.phase -in @('Queued', 'Validating', 'Preparing', 'Traveling', 'Executing', 'Verifying', 'Recovering')) -or
                        ($_.cancellation.requested) -or
                        ($_.phase -eq 'Blocked' -and $_.blockedReason -and (
                            [string]$_.blockedReason.code -in $healableCodes -or
                            [string]$_.blockedReason.detail -like '*server session changed*'))
                    })
                if ($activeJobs.Count -eq 0) {
                    $null = Write-AutopilotReceipt -Paths $Paths -JobId 'controller' -Event 'run_quiescent' -Data @{
                        reason = 'no job can make further progress: everything is terminal, suspended, or blocked on a non-healable reason'
                    } -Now (Get-AutopilotClockNow -Clock $clock)
                    $stopReason = 'quiescence'
                    break
                }
            }
            if (Test-Path $stopFile) {
                Remove-Item -LiteralPath $stopFile -Force
                $null = Write-AutopilotReceipt -Paths $Paths -JobId 'controller' -Event 'stop_requested' -Data @{ via = 'stop.request file' } -Now (Get-AutopilotClockNow -Clock $clock)
                $stopReason = 'stop-request'
                break
            }
            if ($MaxTicks -gt 0 -and $tickNumber -ge $MaxTicks) { $stopReason = 'max-ticks'; break }
            if ($FixtureClock) { Step-AutopilotClock -Clock $clock } else { Start-Sleep -Seconds $TickSeconds }
            if ($endAt -and ((Get-AutopilotClockNow -Clock $clock) -ge $endAt)) { $stopReason = 'duration'; break }
        }
        return [pscustomobject]@{
            Ticks      = $tickNumber
            StopReason = $stopReason
            StartedUtc = ConvertTo-AutopilotTimestamp -Time $startUtc
            EndedUtc   = ConvertTo-AutopilotTimestamp -Time (Get-AutopilotClockNow -Clock $clock)
        }
    }
    finally {
        try {
            $released = Release-AutopilotAllOwnership -Paths $Paths -Reason 'run loop shutdown'
            $null = Write-AutopilotReceipt -Paths $Paths -JobId 'controller' -Event 'run_stopped' -Data @{
                ticks         = $tickNumber
                stop_reason   = $stopReason
                released_jobs = @($released)
            } -Now (Get-AutopilotClockNow -Clock $clock)
        }
        catch { }
        Exit-AutopilotLock -Paths $Paths
    }
}
