# AutoWoW Autopilot V1.2 - capability-aware scheduler.
# Enforces: one active mutating job per character; ownership before activation
# (via the Preparing/Executing steps); higher priority first with bounded aging;
# dependencies before dependants; deterministic tie-breaking; no work outside
# the campaign/profile allow-list (profiles gate at job creation, schedules gate
# here); explicit idle when nothing legal is available. A blocked job is never
# silently transformed into a different kind - both jobs exist and the waiting
# one says why it waits.

Set-StrictMode -Version Latest

$script:AutopilotSchedulerNotesSchema = 'autowow.autopilot.scheduler-notes.v1'

function Get-AutopilotJobCharacter {
    param([Parameter(Mandatory)]$Job)
    $assigned = @($Job.characters.assignedGuids)
    if ($assigned.Count -gt 0) { return [int64]$assigned[0] }
    $eligible = @($Job.characters.eligibleGuids)
    if ($eligible.Count -gt 0) { return [int64]$eligible[0] }
    return $null
}

function Get-AutopilotSchedulerNotes {
    param([Parameter(Mandatory)]$Paths)
    $file = Join-Path $Paths.StateRoot 'scheduler-notes.json'
    if (-not (Test-Path $file)) { return [ordered]@{ held = [ordered]@{}; idle = [ordered]@{} } }
    try {
        $doc = ConvertTo-AutopilotHashtable (Get-Content -LiteralPath $file -Raw | ConvertFrom-Json)
        if ([string]$doc.schema -ne $script:AutopilotSchedulerNotesSchema) { return [ordered]@{ held = [ordered]@{}; idle = [ordered]@{} } }
        $held = if ($doc.Contains('held') -and $doc.held) { $doc.held } else { [ordered]@{} }
        $idle = if ($doc.Contains('idle') -and $doc.idle) { $doc.idle } else { [ordered]@{} }
        return [ordered]@{ held = $held; idle = $idle }
    }
    catch { return [ordered]@{ held = [ordered]@{}; idle = [ordered]@{} } }
}

function Save-AutopilotSchedulerNotes {
    param([Parameter(Mandatory)]$Paths, [Parameter(Mandatory)]$Notes)
    $doc = [ordered]@{
        schema         = $script:AutopilotSchedulerNotesSchema
        schema_version = 1
        held           = $Notes.held
        idle           = $Notes.idle
    }
    Write-AutopilotFileAtomic -Path (Join-Path $Paths.StateRoot 'scheduler-notes.json') -Content ($doc | ConvertTo-Json -Depth 10)
}

function Get-AutopilotScheduleDecision {
    <#
    Pure decision (no side effects): which jobs advance this tick, which are
    held and why, which characters are explicitly idle. Deterministic for a
    given store + clock: priority desc (with bounded aging), createdAt asc,
    jobId asc.
    #>
    param(
        [Parameter(Mandatory)]$Paths,
        $CapabilityProvider,
        $ProfileResolver,
        [datetime]$Now,
        [ValidateRange(1, 1440)][int]$AgingMinutes = 30,
        [ValidateRange(0, 50)][int]$AgingCapBonus = 20
    )
    if (-not $CapabilityProvider) { $CapabilityProvider = New-AutopilotCapabilityProvider -Kind unavailable }
    $nowUtc = if ($PSBoundParameters.ContainsKey('Now')) { Get-AutopilotUtcNow -Now $Now } else { Get-AutopilotUtcNow }

    $active = @()
    $selfHeal = @()
    $held = @()
    $skipped = @()
    $contenders = @{}   # character guid -> list of candidate info
    $charactersSeen = @{}

    foreach ($job in (Get-AutopilotJobs -Paths $Paths)) {
        $character = Get-AutopilotJobCharacter -Job $job
        if ($null -ne $character) { $charactersSeen[[string]$character] = $true }

        if ($job.cancellation.requested) {
            $active += [pscustomobject]@{ job = $job; character = $character; reason = 'cancellation requested' }
            continue
        }
        if ($job.phase -eq 'Suspended') {
            $skipped += [pscustomobject]@{ jobId = $job.jobId; character = $character; reason = 'suspended by operator' }
            continue
        }
        if ($job.phase -eq 'Blocked') {
            # Self-heal steps read state and may re-validate; they never mutate
            # the world, so they do not occupy the character's mutating slot.
            $selfHeal += [pscustomobject]@{ job = $job; character = $character; reason = 'blocked self-heal check' }
            continue
        }

        # Dependency gate before contention: dependants wait for dependencies.
        $prerequisiteState = Test-AutopilotJobPrerequisites -Paths $Paths -Job $job
        if (-not $prerequisiteState.Met) {
            $waiting = (@($prerequisiteState.Unmet) | ForEach-Object { [string]$_.jobId }) -join ', '
            $held += [pscustomobject]@{ jobId = $job.jobId; character = $character; reason = "waiting-dependency: $waiting" }
            continue
        }

        # Profile play/rest schedule (strategic preference; never execution authority).
        if ($ProfileResolver -and $job.Contains('metadata') -and $job.metadata -and $job.metadata.Contains('profile') -and $job.metadata.profile) {
            $profile = & $ProfileResolver ([string]$job.metadata.profile)
            if ($profile -and (Get-Command -Name 'Test-AutopilotProfileScheduleActive' -ErrorAction SilentlyContinue)) {
                if (-not (Test-AutopilotProfileScheduleActive -Profile $profile -Now $nowUtc)) {
                    $held += [pscustomobject]@{ jobId = $job.jobId; character = $character; reason = "outside play window (profile $($job.metadata.profile))" }
                    continue
                }
            }
        }

        $ageMinutes = [math]::Max(0, ($nowUtc - (ConvertTo-AutopilotUtcDateTime $job.createdAt)).TotalMinutes)
        $effectivePriority = [int64]$job.priority + [math]::Min($AgingCapBonus, [math]::Floor($ageMinutes / $AgingMinutes))
        $info = [pscustomobject]@{
            job               = $job
            character         = $character
            effectivePriority = $effectivePriority
            createdAt         = [string]$job.createdAt
            jobId             = [string]$job.jobId
        }
        $key = [string]$character
        if (-not $contenders.ContainsKey($key)) { $contenders[$key] = @() }
        $contenders[$key] = @($contenders[$key]) + @($info)
    }

    # One active mutating job per character: deterministic winner selection.
    foreach ($key in ($contenders.Keys | Sort-Object)) {
        $ranked = @($contenders[$key] | Sort-Object -Property @{Expression = { -[int64]$_.effectivePriority } }, @{Expression = { $_.createdAt } }, @{Expression = { $_.jobId } })
        $winner = $ranked[0]
        $active += [pscustomobject]@{ job = $winner.job; character = $winner.character; reason = "selected (priority $($winner.effectivePriority))" }
        foreach ($loser in @($ranked | Select-Object -Skip 1)) {
            $held += [pscustomobject]@{ jobId = $loser.jobId; character = $loser.character; reason = "character busy with $($winner.jobId) (one mutating job per character)" }
        }
    }

    # Explicit idle: characters that have jobs but nothing active this tick.
    $activeCharacters = @{}
    foreach ($entry in $active) { if ($null -ne $entry.character) { $activeCharacters[[string]$entry.character] = $true } }
    $idleCharacters = @()
    foreach ($key in ($charactersSeen.Keys | Sort-Object)) {
        if (-not $activeCharacters.ContainsKey($key)) {
            $reasons = @(@($held) + @($skipped) | Where-Object { [string]$_.character -eq $key } | ForEach-Object { $_.reason })
            $blockedReasons = @($selfHeal | Where-Object { [string]$_.character -eq $key } | ForEach-Object {
                    if ($_.job.blockedReason) { '{0}: {1}' -f $_.job.blockedReason.code, $_.job.blockedReason.detail } else { 'blocked' }
                })
            $why = (@($reasons) + @($blockedReasons) | Select-Object -First 3) -join ' | '
            if (-not $why) { $why = 'no legal useful action available' }
            $idleCharacters += [pscustomobject]@{ guid = [int64]$key; reason = $why }
        }
    }

    [pscustomobject]@{
        Active         = @($active)
        SelfHeal       = @($selfHeal)
        Held           = @($held)
        Skipped        = @($skipped)
        IdleCharacters = @($idleCharacters)
    }
}

function Invoke-AutopilotScheduledTick {
    # The character-slot-aware planner tick used by the persistent run loop.
    param(
        [Parameter(Mandatory)]$Paths,
        [string]$FixtureRoot,
        $CapabilityProvider,
        $ProfileResolver,
        [switch]$ConfirmLiveBridge,
        [datetime]$Now
    )
    if (-not $FixtureRoot) { $FixtureRoot = $Paths.FixtureRoot }
    if (-not $CapabilityProvider) { $CapabilityProvider = Resolve-AutopilotCapabilityProvider -Paths $Paths }
    $nowParams = @{}
    if ($PSBoundParameters.ContainsKey('Now')) { $nowParams['Now'] = $Now }

    $decisionParams = @{ Paths = $Paths; CapabilityProvider = $CapabilityProvider }
    if ($ProfileResolver) { $decisionParams['ProfileResolver'] = $ProfileResolver }
    $decision = Get-AutopilotScheduleDecision @decisionParams @nowParams

    $stepParams = @{ Paths = $Paths; FixtureRoot = $FixtureRoot; CapabilityProvider = $CapabilityProvider }
    if ($ConfirmLiveBridge) { $stepParams['ConfirmLiveBridge'] = $true }
    if ($PSBoundParameters.ContainsKey('Now')) { $stepParams['Now'] = $Now }

    $results = @()
    foreach ($entry in @($decision.Active)) {
        $results += Invoke-AutopilotJobStep -Job $entry.job @stepParams
    }
    foreach ($entry in @($decision.SelfHeal)) {
        $results += Invoke-AutopilotJobStep -Job $entry.job @stepParams
    }

    # Receipt held/idle transitions only when the reason CHANGES (no per-tick spam).
    $notes = Get-AutopilotSchedulerNotes -Paths $Paths
    $newHeld = [ordered]@{}
    foreach ($entry in @($decision.Held)) {
        $newHeld[[string]$entry.jobId] = [string]$entry.reason
        $previous = if ($notes.held.Contains([string]$entry.jobId)) { [string]$notes.held[[string]$entry.jobId] } else { $null }
        if ($previous -ne [string]$entry.reason) {
            $null = Write-AutopilotReceipt -Paths $Paths -JobId ([string]$entry.jobId) -Event 'job_held' -Data @{ reason = [string]$entry.reason } @nowParams
        }
    }
    $newIdle = [ordered]@{}
    foreach ($entry in @($decision.IdleCharacters)) {
        $newIdle[[string]$entry.guid] = [string]$entry.reason
        $previous = if ($notes.idle.Contains([string]$entry.guid)) { [string]$notes.idle[[string]$entry.guid] } else { $null }
        if ($previous -ne [string]$entry.reason) {
            $null = Write-AutopilotReceipt -Paths $Paths -JobId 'controller' -Event 'character_idle' -Data @{ guid = [int64]$entry.guid; reason = [string]$entry.reason } @nowParams
        }
    }
    Save-AutopilotSchedulerNotes -Paths $Paths -Notes ([ordered]@{ held = $newHeld; idle = $newIdle })

    [pscustomobject]@{
        Results  = @($results)
        Decision = $decision
    }
}
