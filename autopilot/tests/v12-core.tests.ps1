# AutoWoW Autopilot V1.2 core tests: clock, checkpoint, session change,
# timeout/recovery enforcement, Gather executor, scheduler, run loop.
# Run: Invoke-Pester -Path .\autopilot\tests\v12-core.tests.ps1 -Output Detailed
# Fully offline; TestDrive state; no sockets.

BeforeAll {
    $script:AutopilotRoot = Split-Path -Parent $PSScriptRoot
    $script:ProjectRoot = Split-Path -Parent $script:AutopilotRoot
    . (Join-Path $script:AutopilotRoot 'AutopilotLib.ps1')
    . (Join-Path $script:AutopilotRoot 'modules\AutopilotScheduler.ps1')
    . (Join-Path $script:AutopilotRoot 'modules\AutopilotRunLoop.ps1')

    $script:FixturesDefault = Join-Path $script:AutopilotRoot 'fixtures\default'
    $script:FixturesStep3 = Join-Path $script:AutopilotRoot 'fixtures\scenario-a-step3'
    $script:ManifestDeployedGather = Join-Path $script:AutopilotRoot 'fixtures\capabilities\deployed-gather.json'
    $script:ManifestDeployedQuest = Join-Path $script:AutopilotRoot 'fixtures\capabilities\deployed-quest.json'
    $script:T0 = [datetime]::new(2026, 7, 18, 20, 0, 0, [System.DateTimeKind]::Utc)

    function New-TestPaths {
        param([string]$Name = ('sandbox-' + [guid]::NewGuid().ToString('n').Substring(0, 8)))
        $root = Join-Path $TestDrive $Name
        Get-AutopilotPaths -StateRoot (Join-Path $root 'state') -ProjectRoot $script:ProjectRoot `
            -LogsRoot (Join-Path $root 'logs\autopilot') -RunId 'run-test-0001' `
            -OwnershipPath (Join-Path $root 'work\ownership.json') `
            -EnrollmentRequestPath (Join-Path $root 'work\oracle-enrollment-request.json')
    }

    function New-TestQuestJob {
        param([datetime]$Now = $script:T0, $Paths, [hashtable]$TimeoutPolicy, [hashtable]$RetryPolicy, [int]$Priority = 50, [uint32]$Guid = 10)
        $jobParams = @{
            Kind            = 'QuestLevel'
            Spec            = @{ partyLeaderGuid = [int64]$Guid }
            EligibleGuids   = @([uint32]$Guid)
            SuccessCriteria = @{ measure = 'quest-turnins'; counters = @{ questsTurnedIn = 1 } }
            Campaign        = 'v12-test'
            Priority        = $Priority
            Now             = $Now
        }
        if ($Paths) { $jobParams['Paths'] = $Paths }
        if ($TimeoutPolicy) { $jobParams['TimeoutPolicy'] = $TimeoutPolicy }
        if ($RetryPolicy) { $jobParams['RetryPolicy'] = $RetryPolicy }
        New-AutopilotJob @jobParams
    }

    function New-TestGatherJob {
        param([datetime]$Now = $script:T0, [uint32]$Guid = 6, [int]$StopAt = 100)
        New-AutopilotJob -Kind 'Gather' -Spec @{ item = @{ itemId = 2770; count = $StopAt }; stopAtCount = $StopAt } `
            -EligibleGuids @([uint32]$Guid) `
            -SuccessCriteria @{ measure = 'inventory-item-count'; counters = @{ itemCount = $StopAt } } `
            -Campaign 'v12-test' -Now $Now
    }

    function New-InventoryFixture {
        param([string]$Dir, [int64]$Guid, [int64]$ItemId, [int64]$Count)
        if (-not (Test-Path $Dir)) { $null = New-Item -ItemType Directory -Force -Path $Dir }
        # Quest fixtures live alongside so QuestLevel jobs can also verify.
        Copy-Item (Join-Path $script:FixturesDefault 'questobjective-10.json') (Join-Path $Dir 'questobjective-10.json') -Force -ErrorAction SilentlyContinue
        $doc = [ordered]@{ schema = 'autowow.autopilot.inventory-observation.v1'; item_id = $ItemId; count = $Count; observed_utc = '2026-07-18T20:00:00.0000000Z' }
        Write-AutopilotFileAtomic -Path (Join-Path $Dir "inventory-$Guid.json") -Content ($doc | ConvertTo-Json)
    }
}

Describe 'Clock abstraction' {
    It 'fixture clock is deterministic and steps without wall time' {
        $clock = New-AutopilotClock -Kind fixture -StartUtc $script:T0 -StepSeconds 300
        (Get-AutopilotClockNow -Clock $clock) | Should -Be $script:T0
        Step-AutopilotClock -Clock $clock
        (Get-AutopilotClockNow -Clock $clock) | Should -Be $script:T0.AddSeconds(300)
        Step-AutopilotClock -Clock $clock -Seconds 60
        (Get-AutopilotClockNow -Clock $clock) | Should -Be $script:T0.AddSeconds(360)
    }

    It 'requires StartUtc for fixture clocks and returns UTC for real clocks' {
        { New-AutopilotClock -Kind fixture } | Should -Throw '*StartUtc*'
        (Get-AutopilotClockNow -Clock (New-AutopilotClock -Kind real)).Kind | Should -Be 'Utc'
    }
}

Describe 'Checkpoint' {
    It 'round-trips atomically and fails closed on unknown schema' {
        $paths = New-TestPaths
        $null = Save-AutopilotCheckpoint -Paths $paths -Data @{ tick = 7; session = @{ known = $true; build = 'b1'; session_id = 's1' } } -Now $script:T0
        $loaded = Get-AutopilotCheckpoint -Paths $paths
        [int]$loaded.tick | Should -Be 7
        [string]$loaded.session.build | Should -Be 'b1'
        Write-AutopilotFileAtomic -Path (Join-Path $paths.StateRoot 'checkpoint.json') -Content '{"schema":"other.v9","schema_version":9}'
        Get-AutopilotCheckpoint -Paths $paths | Should -BeNullOrEmpty
    }
}

Describe 'Session identity and change detection' {
    It 'detects a change only between two KNOWN identities' {
        $known1 = [pscustomobject]@{ Known = $true; Build = 'b1'; SessionId = 's1' }
        $known2 = [pscustomobject]@{ Known = $true; Build = 'b1'; SessionId = 's2' }
        $unknown = [pscustomobject]@{ Known = $false; Build = $null; SessionId = $null }
        Test-AutopilotSessionChanged -Previous $known1 -Current $known2 | Should -BeTrue
        Test-AutopilotSessionChanged -Previous $known1 -Current $known1 | Should -BeFalse
        Test-AutopilotSessionChanged -Previous $unknown -Current $known1 | Should -BeFalse
        Test-AutopilotSessionChanged -Previous $known1 -Current $unknown | Should -BeFalse
    }
}

Describe 'Timeout and bounded recovery enforcement' {
    It 'blocks a job that exceeds jobTimeoutMinutes and releases ownership' {
        $paths = New-TestPaths
        $job = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob -TimeoutPolicy @{ jobTimeoutMinutes = 60; phaseTimeoutMinutes = 30; noProgressSeconds = 120 })
        1..3 | ForEach-Object { $null = Invoke-AutopilotPlannerTick -Paths $paths -FixtureRoot $script:FixturesDefault -Now $script:T0 }
        (Get-AutopilotJob -Paths $paths -JobId $job.jobId).phase | Should -Be 'Executing'
        (Test-AutopilotOwnershipActive -Paths $paths -JobId $job.jobId -Guid 10 -Now $script:T0) | Should -BeTrue
        # 61 minutes later with no completion: job timeout.
        $null = Invoke-AutopilotPlannerTick -Paths $paths -FixtureRoot $script:FixturesDefault -Now $script:T0.AddMinutes(61)
        $loaded = Get-AutopilotJob -Paths $paths -JobId $job.jobId
        $loaded.phase | Should -Be 'Blocked'
        $loaded.blockedReason.code | Should -Be 'job-timeout'
        (Test-AutopilotOwnershipActive -Paths $paths -JobId $job.jobId -Guid 10 -Now $script:T0.AddMinutes(61)) | Should -BeFalse
    }

    It 'phase timeout triggers bounded Recovering, issues a recover command, then exhausts to Blocked' {
        $paths = New-TestPaths
        $job = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob -TimeoutPolicy @{ jobTimeoutMinutes = 480; phaseTimeoutMinutes = 10; noProgressSeconds = 120 } -RetryPolicy @{ maxAttempts = 3; maxRecoveriesPerObjective = 1; backoffSeconds = 60; onExhaust = 'block' })
        1..3 | ForEach-Object { $null = Invoke-AutopilotPlannerTick -Paths $paths -FixtureRoot $script:FixturesDefault -Now $script:T0 }
        (Get-AutopilotJob -Paths $paths -JobId $job.jobId).phase | Should -Be 'Executing'

        # 11 minutes stuck in Executing -> Recovering (recovery 1/1).
        $null = Invoke-AutopilotPlannerTick -Paths $paths -FixtureRoot $script:FixturesDefault -Now $script:T0.AddMinutes(11)
        $loaded = Get-AutopilotJob -Paths $paths -JobId $job.jobId
        $loaded.phase | Should -Be 'Recovering'
        [int64]$loaded.progress.recoveriesUsed | Should -Be 1
        # Recovery step issues the bridge recover order (dry-run) and resumes.
        $null = Invoke-AutopilotPlannerTick -Paths $paths -FixtureRoot $script:FixturesDefault -Now $script:T0.AddMinutes(12)
        (Get-AutopilotJob -Paths $paths -JobId $job.jobId).phase | Should -Be 'Executing'
        $receipts = Get-AutopilotReceipts -Paths $paths -JobId $job.jobId
        $recoverCommands = @($receipts | Where-Object { $_.event -eq 'command' -and $_.command.action -eq 'recover' })
        $recoverCommands.Count | Should -Be 1
        $recoverCommands[0].command.wire | Should -Be 'recover 10'
        $recoverCommands[0].command.sent | Should -BeFalse

        # Second stall: recovery budget exhausted -> Blocked, never grinding.
        $null = Invoke-AutopilotPlannerTick -Paths $paths -FixtureRoot $script:FixturesDefault -Now $script:T0.AddMinutes(25)
        $loaded = Get-AutopilotJob -Paths $paths -JobId $job.jobId
        $loaded.phase | Should -Be 'Blocked'
        $loaded.blockedReason.code | Should -Be 'recovery-budget-exhausted'
    }
}

Describe 'Gather executor (dry-run oracle intents)' {
    It 'walks a Gather job to Completed on verified inventory observations once the manifest deploys the capability' {
        $paths = New-TestPaths
        $provider = New-AutopilotCapabilityProvider -Kind manifest -ManifestPath $script:ManifestDeployedGather
        $job = Add-AutopilotJob -Paths $paths -Job (New-TestGatherJob -StopAt 100)
        $fixturesPartial = Join-Path $TestDrive 'gather-partial'
        $fixturesFull = Join-Path $TestDrive 'gather-full'
        New-InventoryFixture -Dir $fixturesPartial -Guid 6 -ItemId 2770 -Count 40
        New-InventoryFixture -Dir $fixturesFull -Guid 6 -ItemId 2770 -Count 100

        # Queued -> Validating -> Preparing -> Executing -> Verifying(40/100) -> Executing
        1..5 | ForEach-Object { $null = Invoke-AutopilotPlannerTick -Paths $paths -FixtureRoot $fixturesPartial -CapabilityProvider $provider -Now $script:T0.AddMinutes($_) }
        $loaded = Get-AutopilotJob -Paths $paths -JobId $job.jobId
        $loaded.phase | Should -Be 'Executing'
        [int64]$loaded.progress.counters.itemCount | Should -Be 40

        # Verified count reaches the stop target -> Completed, ownership released.
        1..2 | ForEach-Object { $null = Invoke-AutopilotPlannerTick -Paths $paths -FixtureRoot $fixturesFull -CapabilityProvider $provider -Now $script:T0.AddMinutes(10 + $_) }
        $final = Get-AutopilotJob -Paths $paths -JobId $job.jobId
        $final.phase | Should -Be 'Completed'
        [int64]$final.progress.counters.itemCount | Should -Be 100
        (Test-AutopilotOwnershipActive -Paths $paths -JobId $job.jobId -Guid 6 -Now $script:T0.AddMinutes(20)) | Should -BeFalse

        $receipts = Get-AutopilotReceipts -Paths $paths -JobId $job.jobId
        $oracleCommands = @($receipts | Where-Object { $_.event -eq 'command' -and $_.command.surface -eq 'oracle' })
        $oracleCommands.Count | Should -BeGreaterThan 0
        foreach ($command in $oracleCommands) {
            $command.command.sent | Should -BeFalse
            $command.command.operation | Should -Be 'gather_source'
        }
        $observations = @($receipts | Where-Object { $_.event -eq 'progress_observed_gather' })
        @($observations | ForEach-Object { [int64]$_.count }) | Should -Be @(40, 100)
    }

    It 'never moves counters without an observation (unknown stays unknown)' {
        $paths = New-TestPaths
        $provider = New-AutopilotCapabilityProvider -Kind manifest -ManifestPath $script:ManifestDeployedGather
        $job = Add-AutopilotJob -Paths $paths -Job (New-TestGatherJob -StopAt 50)
        $emptyFixtures = Join-Path $TestDrive 'gather-empty'
        $null = New-Item -ItemType Directory -Force -Path $emptyFixtures
        1..5 | ForEach-Object { $null = Invoke-AutopilotPlannerTick -Paths $paths -FixtureRoot $emptyFixtures -CapabilityProvider $provider -Now $script:T0.AddMinutes($_) }
        $loaded = Get-AutopilotJob -Paths $paths -JobId $job.jobId
        $loaded.phase | Should -Be 'Executing'
        @($loaded.progress.counters.Keys).Count | Should -Be 0
        $observations = @(Get-AutopilotReceipts -Paths $paths -JobId $job.jobId | Where-Object { $_.event -eq 'progress_observed_gather' })
        $observations.Count | Should -BeGreaterThan 0
        $observations[0].source | Should -Be 'none'
        $observations[0].count | Should -BeNullOrEmpty
    }
}

Describe 'Capability-aware scheduler' {
    It 'runs one mutating job per character, holds the rest with a reason, and never transforms a job' {
        $paths = New-TestPaths
        # Same character: a high-priority quest job and a low-priority second quest job.
        $high = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob -Priority 80 -Now $script:T0)
        $low = New-AutopilotJob -Kind 'QuestLevel' -Spec @{ partyLeaderGuid = 10; zoneHint = 'Westfall' } -EligibleGuids @([uint32]10) `
            -SuccessCriteria @{ measure = 'quest-turnins'; counters = @{ questsTurnedIn = 1 } } -Campaign 'v12-test' -Priority 20 -Now $script:T0
        $low = Add-AutopilotJob -Paths $paths -Job $low
        # Different character: gather job (capability-blocked under unavailable provider).
        $gather = Add-AutopilotJob -Paths $paths -Job (New-TestGatherJob -Guid 6)

        $tick = Invoke-AutopilotScheduledTick -Paths $paths -FixtureRoot $script:FixturesDefault -Now $script:T0
        $decision = $tick.Decision
        @($decision.Active | Where-Object { $_.job.jobId -eq $high.jobId }).Count | Should -Be 1
        @($decision.Held | Where-Object { $_.jobId -eq $low.jobId }).Count | Should -Be 1
        @($decision.Held)[0].reason | Should -Match 'one mutating job per character'
        # The gather job is a different character: it advances (to Validating) independently.
        @($decision.Active | Where-Object { $_.job.jobId -eq $gather.jobId }).Count | Should -Be 1

        # Held receipt emitted once; unchanged reasons do not spam.
        $null = Invoke-AutopilotScheduledTick -Paths $paths -FixtureRoot $script:FixturesDefault -Now $script:T0.AddMinutes(1)
        $heldReceipts = @(Get-AutopilotReceipts -Paths $paths -JobId $low.jobId | Where-Object { $_.event -eq 'job_held' })
        $heldReceipts.Count | Should -Be 1
        # Both jobs still exist with their own kinds - nothing was transformed.
        (Get-AutopilotJob -Paths $paths -JobId $low.jobId).kind | Should -Be 'QuestLevel'
        (Get-AutopilotJob -Paths $paths -JobId $gather.jobId).kind | Should -Be 'Gather'
    }

    It 'keeps a blocked Gather job waiting with its reason while the same character quests' {
        $paths = New-TestPaths
        # One character with BOTH a gather job (blocked: no capability) and a quest job.
        $gather = New-AutopilotJob -Kind 'Gather' -Spec @{ item = @{ itemId = 2770; count = 10 }; stopAtCount = 10 } `
            -EligibleGuids @([uint32]10) -SuccessCriteria @{ measure = 'inventory-item-count'; counters = @{ itemCount = 10 } } `
            -Campaign 'v12-test' -Priority 90 -Now $script:T0
        $gather = Add-AutopilotJob -Paths $paths -Job $gather
        $quest = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob -Priority 40 -Now $script:T0.AddMinutes(1))

        # Tick 1: gather wins the slot (higher priority), steps to Validating.
        # Tick 2: gather blocks on capability; quest wins the slot.
        1..4 | ForEach-Object { $null = Invoke-AutopilotScheduledTick -Paths $paths -FixtureRoot $script:FixturesDefault -Now $script:T0.AddMinutes(1 + $_) }
        $gatherLoaded = Get-AutopilotJob -Paths $paths -JobId $gather.jobId
        $questLoaded = Get-AutopilotJob -Paths $paths -JobId $quest.jobId
        $gatherLoaded.phase | Should -Be 'Blocked'
        $gatherLoaded.blockedReason.code | Should -Be 'oracle-capability-not-deployed'
        $questLoaded.phase | Should -BeIn @('Preparing', 'Executing', 'Verifying')
        # Both jobs coexist; the gather job still says why it waits.
        $gatherLoaded.kind | Should -Be 'Gather'
    }

    It 'holds dependants until dependencies complete and ages priorities boundedly' {
        $paths = New-TestPaths
        $first = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob -Guid 7 -Priority 50 -Now $script:T0)
        $dependent = New-AutopilotJob -Kind 'QuestLevel' -Spec @{ partyLeaderGuid = 7; zoneHint = 'Barrens' } -EligibleGuids @([uint32]7) `
            -SuccessCriteria @{ measure = 'quest-turnins'; counters = @{ questsTurnedIn = 1 } } -Campaign 'v12-test' -Priority 99 -Now $script:T0 `
            -Prerequisites @(@{ kind = 'job-completed'; params = @{ jobId = $first.jobId } })
        $dependent = Add-AutopilotJob -Paths $paths -Job $dependent
        $decision = (Invoke-AutopilotScheduledTick -Paths $paths -FixtureRoot $script:FixturesDefault -Now $script:T0).Decision
        # Despite higher priority, the dependent waits for its dependency.
        @($decision.Active | Where-Object { $_.job.jobId -eq $first.jobId }).Count | Should -Be 1
        @($decision.Held | Where-Object { $_.jobId -eq $dependent.jobId })[0].reason | Should -Match 'waiting-dependency'
        # Aging is bounded: effective priority gains at most the cap.
        $aged = (Get-AutopilotScheduleDecision -Paths $paths -Now $script:T0.AddDays(10))
        $agedEntry = @($aged.Active | Where-Object { $_.job.jobId -eq $first.jobId })[0]
        $agedEntry.reason | Should -Match 'priority 70'   # 50 + capped 20
    }

    It 'emits an explicit character_idle receipt when a character has no legal action' {
        $paths = New-TestPaths
        $gather = Add-AutopilotJob -Paths $paths -Job (New-TestGatherJob -Guid 6)
        # Two ticks: job blocks on capability, then only self-heals -> idle char.
        1..3 | ForEach-Object { $null = Invoke-AutopilotScheduledTick -Paths $paths -Now $script:T0.AddMinutes($_) }
        $idleReceipts = @(Get-AutopilotReceipts -Paths $paths -JobId 'controller' | Where-Object { $_.event -eq 'character_idle' })
        $idleReceipts.Count | Should -BeGreaterThan 0
        [int64]$idleReceipts[0].guid | Should -Be 6
        $idleReceipts[0].reason | Should -Match 'oracle-capability-not-deployed'
    }
}

Describe 'Persistent run loop' {
    It 'refuses a live loop outright' {
        $paths = New-TestPaths
        { Start-AutopilotRun -Paths $paths -DryRun:$false -Once } | Should -Throw '*not enabled*'
    }

    It 'runs one fixture tick, checkpoints, releases leases, and receipts the lifecycle' {
        $paths = New-TestPaths
        $job = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob)
        $result = Start-AutopilotRun -Paths $paths -Once -FixtureClock -FixtureStartUtc $script:T0 -TickSeconds 60 -FixtureRoot $script:FixturesDefault -CapabilityManifest $script:ManifestDeployedQuest
        $result.Ticks | Should -Be 1
        $result.StopReason | Should -Be 'once'
        # Lock released; checkpoint written with the session identity.
        Test-Path $paths.LockPath | Should -BeFalse
        $checkpoint = Get-AutopilotCheckpoint -Paths $paths
        [string]$checkpoint.session.build | Should -Be 'fixture-deployed-quest'
        $controller = Get-AutopilotReceipts -Paths $paths -JobId 'controller'
        @($controller | Where-Object { $_.event -eq 'run_started' }).Count | Should -Be 1
        @($controller | Where-Object { $_.event -eq 'run_stopped' }).Count | Should -Be 1
    }

    It 'runs a bounded fixture duration and advances jobs each tick' {
        $paths = New-TestPaths
        $job = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob)
        # 30 sim-minutes at 5-minute ticks = 6 ticks + the initial one.
        $result = Start-AutopilotRun -Paths $paths -DurationMinutes 30 -TickSeconds 300 -FixtureClock -FixtureStartUtc $script:T0 -FixtureRoot $script:FixturesStep3 -MaxTicks 20
        $result.StopReason | Should -Be 'duration'
        (Get-AutopilotJob -Paths $paths -JobId $job.jobId).phase | Should -Be 'Completed'
        (ConvertTo-AutopilotUtcDateTime $result.EndedUtc) | Should -Be $script:T0.AddMinutes(30)
    }

    It 'detects a server-session change mid-run, revalidates in-flight jobs, and continues' {
        $paths = New-TestPaths
        $manifestPath = Join-Path $TestDrive 'session-manifest.json'
        Copy-Item $script:ManifestDeployedQuest $manifestPath -Force
        $job = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob)
        $clock = New-AutopilotClock -Kind fixture -StartUtc $script:T0 -StepSeconds 60

        # Ticks 1-4: reach Executing/Verifying under session fixture-session-1.
        1..4 | ForEach-Object {
            $null = Invoke-AutopilotRunTick -Paths $paths -Clock $clock -TickNumber $_ -FixtureRoot $script:FixturesDefault -CapabilityManifest $manifestPath
            Step-AutopilotClock -Clock $clock
        }
        (Get-AutopilotJob -Paths $paths -JobId $job.jobId).phase | Should -BeIn @('Executing', 'Verifying')

        # Session change: same build, new session id.
        $manifest = Get-Content $manifestPath -Raw | ConvertFrom-Json
        $manifest.session_id = 'fixture-session-2'
        Write-AutopilotFileAtomic -Path $manifestPath -Content ($manifest | ConvertTo-Json -Depth 10)

        $tick = Invoke-AutopilotRunTick -Paths $paths -Clock $clock -TickNumber 5 -FixtureRoot $script:FixturesDefault -CapabilityManifest $manifestPath
        $tick.SessionChanged | Should -BeTrue
        @(Get-AutopilotReceipts -Paths $paths -JobId 'controller' | Where-Object { $_.event -eq 'server_session_changed' }).Count | Should -Be 1
        # The job was blocked (stale-state, session change) and the same tick's
        # self-heal already revalidated it - both hops are in the ledger.
        $transitions = @(Get-AutopilotReceipts -Paths $paths -JobId $job.jobId | Where-Object { $_.event -eq 'job_transition' })
        $blockedHop = @($transitions | Where-Object { $_.to -eq 'Blocked' -and $_.reason -match 'session' })
        $blockedHop.Count | Should -Be 1
        (Get-AutopilotJob -Paths $paths -JobId $job.jobId).phase | Should -Be 'Validating'
        # leaseRef cleared for the in-flight job.
        (Get-AutopilotJob -Paths $paths -JobId $job.jobId).leaseRef | Should -BeNullOrEmpty
    }

    It 'honors a stop.request file gracefully' {
        $paths = New-TestPaths
        # Pre-place the stop request: the loop must exit after the first tick.
        Write-AutopilotFileAtomic -Path (Join-Path $paths.StateRoot 'stop.request') -Content '{}'
        $result = Start-AutopilotRun -Paths $paths -FixtureClock -FixtureStartUtc $script:T0 -TickSeconds 60 -MaxTicks 50
        $result.StopReason | Should -Be 'stop-request'
        $result.Ticks | Should -Be 1
        @(Get-AutopilotReceipts -Paths $paths -JobId 'controller' | Where-Object { $_.event -eq 'stop_requested' }).Count | Should -Be 1
    }
}
