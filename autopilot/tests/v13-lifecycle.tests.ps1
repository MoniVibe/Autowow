# AutoWoW Autopilot V1.3 - job lifecycle, party-contract planning, manifest hygiene.
# Run: Invoke-Pester -Path .\autopilot\tests\v13-lifecycle.tests.ps1 -Output Detailed
# Fully offline; TestDrive state; no sockets (live refusals throw BEFORE any connect).

BeforeAll {
    $script:AutopilotRoot = Split-Path -Parent $PSScriptRoot
    $script:ProjectRoot = Split-Path -Parent $script:AutopilotRoot
    . (Join-Path $script:AutopilotRoot 'AutopilotLib.ps1')
    . (Join-Path $script:AutopilotRoot 'modules\AutopilotScheduler.ps1')
    . (Join-Path $script:AutopilotRoot 'modules\AutopilotRunLoop.ps1')

    $script:T0 = [datetime]::new(2026, 7, 18, 20, 0, 0, [System.DateTimeKind]::Utc)
    $script:FixturesDefault = Join-Path $script:AutopilotRoot 'fixtures\default'

    function New-TestPaths {
        param([string]$Name = ('sandbox-' + [guid]::NewGuid().ToString('n').Substring(0, 8)))
        $root = Join-Path $TestDrive $Name
        Get-AutopilotPaths -StateRoot (Join-Path $root 'state') -ProjectRoot $script:ProjectRoot `
            -LogsRoot (Join-Path $root 'logs\autopilot') -RunId 'run-test-0001' `
            -OwnershipPath (Join-Path $root 'work\ownership.json') `
            -EnrollmentRequestPath (Join-Path $root 'work\oracle-enrollment-request.json')
    }

    function New-TestQuestJob {
        param([datetime]$Now = $script:T0, $Paths, [int64[]]$QuestAllowlist, [hashtable]$TimeoutPolicy)
        $spec = @{ partyLeaderGuid = 10 }
        if ($QuestAllowlist) { $spec['questAllowlist'] = @($QuestAllowlist) }
        $jobParams = @{
            Kind            = 'QuestLevel'
            Spec            = $spec
            EligibleGuids   = @([uint32]10)
            SuccessCriteria = @{ measure = 'quest-turnins'; counters = @{ questsTurnedIn = 1 } }
            Campaign        = 'v13-test'
            Now             = $Now
        }
        if ($Paths) { $jobParams['Paths'] = $Paths }
        if ($TimeoutPolicy) { $jobParams['TimeoutPolicy'] = $TimeoutPolicy }
        New-AutopilotJob @jobParams
    }

    function New-QuestResponseFixture {
        param([string]$Dir, [string]$FileName, [string]$Json)
        if (-not (Test-Path $Dir)) { $null = New-Item -ItemType Directory -Force -Path $Dir }
        Write-AutopilotFileAtomic -Path (Join-Path $Dir $FileName) -Content $Json
    }

    function New-ManifestFixture {
        param([string]$Path, [string]$Source, [string]$GeneratedAt, [string]$Build = 'b1', [string]$Session = 's1', [bool]$BridgeDeployed = $true)
        $doc = [ordered]@{
            schema = 'autowow.oracle.capabilities.v1'; schema_version = 1
            server_build = $Build; session_id = $Session; contract_schema_version = 1
            runtime_enabled = $false; generated_at = $GeneratedAt
            bridge = [ordered]@{ deployed = $BridgeDeployed }
            sql_read_deployed = $false; operations = @()
        }
        if ($Source) { $doc['source'] = $Source }
        Write-AutopilotFileAtomic -Path $Path -Content ($doc | ConvertTo-Json -Depth 10)
    }

    function Get-ExpiredSuspendedJob {
        # A job that ran (startedAt set), was suspended, and whose 60-minute
        # wall clock has long expired by T0+8h.
        param($Paths)
        $job = Add-AutopilotJob -Paths $Paths -Job (New-TestQuestJob -Paths $Paths -TimeoutPolicy @{ jobTimeoutMinutes = 60; phaseTimeoutMinutes = 30; noProgressSeconds = 120 })
        1..3 | ForEach-Object { $null = Invoke-AutopilotPlannerTick -Paths $Paths -FixtureRoot $script:FixturesDefault -Now $script:T0.AddMinutes($_) }
        (Get-AutopilotJob -Paths $Paths -JobId $job.jobId).phase | Should -Be 'Executing'
        $null = Suspend-AutopilotJob -Paths $Paths -JobId $job.jobId -Reason 'test park' -Now $script:T0.AddMinutes(5)
        return (Get-AutopilotJob -Paths $Paths -JobId $job.jobId)
    }
}

Describe 'Resume remains resume (expired jobs cannot re-enter execution)' {
    It 'refuses to resume an expired suspended job with a successor recommendation' {
        $paths = New-TestPaths
        $job = Get-ExpiredSuspendedJob -Paths $paths
        $result = Invoke-AutopilotResume -Paths $paths -JobId $job.jobId -Now $script:T0.AddHours(8)
        $result.Resumed | Should -BeFalse
        $result.Diagnostic | Should -Match 'wall clock has expired'
        $result.Diagnostic | Should -Match ('retry -JobId ' + [regex]::Escape($job.jobId))
        # The job did NOT re-enter execution.
        (Get-AutopilotJob -Paths $paths -JobId $job.jobId).phase | Should -Be 'Suspended'
        @(Get-AutopilotReceipts -Paths $paths -JobId $job.jobId | Where-Object { $_.event -eq 'resume_refused_expired' }).Count | Should -Be 1
    }

    It 'still resumes an unexpired suspended job normally, without touching clocks' {
        $paths = New-TestPaths
        $job = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob -Paths $paths)
        1..3 | ForEach-Object { $null = Invoke-AutopilotPlannerTick -Paths $paths -FixtureRoot $script:FixturesDefault -Now $script:T0.AddMinutes($_) }
        $null = Suspend-AutopilotJob -Paths $paths -JobId $job.jobId -Reason 'park' -Now $script:T0.AddMinutes(5)
        $before = Get-AutopilotJob -Paths $paths -JobId $job.jobId
        $result = Invoke-AutopilotResume -Paths $paths -JobId $job.jobId -Now $script:T0.AddMinutes(10)
        $result.Resumed | Should -BeTrue
        $result.Job.phase | Should -Be 'Executing'
        # Resume never reset the clocks.
        [string]$result.Job.startedAt | Should -Be ([string]$before.startedAt)
        [string]$result.Job.createdAt | Should -Be ([string]$before.createdAt)
    }
}

Describe 'Successor jobs (honest retry)' {
    It 'creates a successor with provenance, fresh clocks/budgets, a new ledger, and a distinct idempotency key' {
        $paths = New-TestPaths
        $original = Get-ExpiredSuspendedJob -Paths $paths
        $successor = New-AutopilotSuccessorJob -Paths $paths -JobId $original.jobId -RetryReason 'wall clock expired; corrected build deployed' -Now $script:T0.AddHours(9)

        $successor.jobId | Should -Not -Be $original.jobId
        $successor.idempotencyKey | Should -Not -Be $original.idempotencyKey
        $successor.provenance.supersedesJobId | Should -Be $original.jobId
        $successor.provenance.retryReason | Should -Match 'corrected build'
        # Inherited goal/spec and policies.
        [int64]$successor.spec.partyLeaderGuid | Should -Be 10
        [int64]$successor.timeoutPolicy.jobTimeoutMinutes | Should -Be ([int64]$original.timeoutPolicy.jobTimeoutMinutes)
        # Fresh clocks and budgets.
        $successor.phase | Should -Be 'Queued'
        $successor.startedAt | Should -BeNullOrEmpty
        [int64]$successor.progress.attempt | Should -Be 0
        [int64]$successor.progress.recoveriesUsed | Should -Be 0
        @($successor.progress.counters.Keys).Count | Should -Be 0
        # Its own receipt ledger, carrying the provenance receipt.
        $successor.receiptLog.file | Should -Not -Be $original.receiptLog.file
        $receipts = Get-AutopilotReceipts -Paths $paths -JobId $successor.jobId
        $retried = @($receipts | Where-Object { $_.event -eq 'job_retried_from' })
        $retried.Count | Should -Be 1
        $retried[0].supersedes_job_id | Should -Be $original.jobId
    }

    It 'leaves the original job document and ledger byte-for-byte unchanged' {
        $paths = New-TestPaths
        $original = Get-ExpiredSuspendedJob -Paths $paths
        $jobFile = Join-Path $paths.JobsDir "$($original.jobId).json"
        $ledgerFile = Get-AutopilotReceiptPath -Paths $paths -JobId $original.jobId
        $jobHashBefore = (Get-FileHash $jobFile -Algorithm SHA256).Hash
        $ledgerHashBefore = (Get-FileHash $ledgerFile -Algorithm SHA256).Hash
        $null = New-AutopilotSuccessorJob -Paths $paths -JobId $original.jobId -Now $script:T0.AddHours(9)
        (Get-FileHash $jobFile -Algorithm SHA256).Hash | Should -Be $jobHashBefore
        (Get-FileHash $ledgerFile -Algorithm SHA256).Hash | Should -Be $ledgerHashBefore
    }

    It 'dedupes a double retry to one successor and refuses retrying a runnable job' {
        $paths = New-TestPaths
        $original = Get-ExpiredSuspendedJob -Paths $paths
        $first = New-AutopilotSuccessorJob -Paths $paths -JobId $original.jobId -Now $script:T0.AddHours(9)
        $second = New-AutopilotSuccessorJob -Paths $paths -JobId $original.jobId -Now $script:T0.AddHours(9).AddMinutes(5)
        $second.jobId | Should -Be $first.jobId

        $runnable = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob -Paths $paths -Now $script:T0.AddHours(10) -QuestAllowlist @(999))
        { New-AutopilotSuccessorJob -Paths $paths -JobId $runnable.jobId -Now $script:T0.AddHours(10) } | Should -Throw '*still runnable*'
    }
}

Describe 'Party-contract preconditions (planning failures, never movement stalls)' {
    It 'blocks with party-contract-unmet, spends zero recovery budget, releases ownership, and never re-issues' {
        $paths = New-TestPaths
        $scenario = Join-Path $TestDrive 'party-fail'
        New-QuestResponseFixture -Dir $scenario -FileName 'quest-10.json' -Json '{"ok":false,"error":"party_member_exact_quest_preflight_failed"}'
        $job = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob -Paths $paths -QuestAllowlist @(2520))
        1..4 | ForEach-Object { $null = Invoke-AutopilotPlannerTick -Paths $paths -FixtureRoot $scenario -Now $script:T0.AddMinutes($_) }
        $loaded = Get-AutopilotJob -Paths $paths -JobId $job.jobId
        $loaded.phase | Should -Be 'Blocked'
        $loaded.blockedReason.code | Should -Be 'party-contract-unmet'
        $loaded.blockedReason.detail | Should -Match 'party_member_exact_quest_preflight_failed'
        # Zero movement-recovery budget spent; ownership released.
        [int64]$loaded.progress.recoveriesUsed | Should -Be 0
        (Test-AutopilotOwnershipActive -Paths $paths -JobId $job.jobId -Guid 10 -Now $script:T0.AddMinutes(10)) | Should -BeFalse
        # Actionable receipt names the exact unmet condition.
        $receipts = Get-AutopilotReceipts -Paths $paths -JobId $job.jobId
        $precondition = @($receipts | Where-Object { $_.event -eq 'party_precondition' })
        $precondition.Count | Should -Be 1
        $precondition[0].condition | Should -Match 'every party member must satisfy the quest contract'
        # More ticks: held, no re-issue of the impossible command.
        1..3 | ForEach-Object { $null = Invoke-AutopilotPlannerTick -Paths $paths -FixtureRoot $scenario -Now $script:T0.AddMinutes(10 + $_) }
        @(Get-AutopilotReceipts -Paths $paths -JobId $job.jobId | Where-Object { $_.event -eq 'command' -and $_.command.action -like 'quest*' }).Count | Should -Be 1
        (Get-AutopilotJob -Paths $paths -JobId $job.jobId).phase | Should -Be 'Blocked'
        (Get-AutopilotWaitState -Job (Get-AutopilotJob -Paths $paths -JobId $job.jobId)) | Should -Be 'waiting-for-party-contract'
    }

    It 'classifies every listed party-contract code' {
        foreach ($code in @('quest_requires_party_leader', 'party_member_exact_quest_preflight_failed', 'quest_not_in_member_log', 'quest_not_in_leader_log', 'cross_faction_party_not_allowed')) {
            $script:AutopilotPartyContractErrors.ContainsKey($code) | Should -BeTrue
        }
    }
}

Describe 'Acquisition vs in-log execution (distinct verbs, honest replanning)' {
    It 'replans quest_not_in_leader_log toward acquisition using the distinct wire commands' {
        $paths = New-TestPaths
        $scenario = Join-Path $TestDrive 'replan'
        New-QuestResponseFixture -Dir $scenario -FileName 'quest-10.json' -Json '{"ok":false,"error":"quest_not_in_leader_log"}'
        New-QuestResponseFixture -Dir $scenario -FileName 'quest-acquire-10.json' -Json '{"ok":true,"order":"quest","phase":"acquire","state":"issued","guid":10,"quest_id":954}'
        $job = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob -Paths $paths -QuestAllowlist @(2520))
        # Ticks: Queued -> Validating -> Preparing -> Executing(execute:2520 refused -> replan) -> Executing(acquire) -> Verifying
        1..5 | ForEach-Object { $null = Invoke-AutopilotPlannerTick -Paths $paths -FixtureRoot $scenario -Now $script:T0.AddMinutes($_) }
        $loaded = Get-AutopilotJob -Paths $paths -JobId $job.jobId
        $loaded.metadata.questMode | Should -Be 'acquire'
        $receipts = Get-AutopilotReceipts -Paths $paths -JobId $job.jobId
        @($receipts | Where-Object { $_.event -eq 'replan_to_acquisition' }).Count | Should -Be 1
        $commands = @($receipts | Where-Object { $_.event -eq 'command' })
        # The two DISTINCT wire commands, in order: exact in-log execution, then acquisition.
        $commands[0].command.wire | Should -Be 'quest 10 2520'
        $commands[1].command.wire | Should -Be 'quest 10 acquire'
        # No recovery was spent on the replan.
        [int64]$loaded.progress.recoveriesUsed | Should -Be 0
        # Never blocked - the job continued into acquisition.
        $loaded.phase | Should -BeIn @('Executing', 'Verifying')
    }

    It 'does not replan when acquisition is not permitted; blocks honestly instead' {
        $paths = New-TestPaths
        $scenario = Join-Path $TestDrive 'no-acquire'
        New-QuestResponseFixture -Dir $scenario -FileName 'quest-10.json' -Json '{"ok":false,"error":"quest_not_in_leader_log"}'
        $job = New-TestQuestJob -Paths $paths -QuestAllowlist @(2520)
        $job.permittedCapabilities = @('bridge.snapshot', 'bridge.questlog', 'bridge.questobjective', 'bridge.acceptance', 'bridge.quest', 'bridge.recover', 'sql.read')  # no bridge.quest.acquire
        $job = Add-AutopilotJob -Paths $paths -Job $job
        1..4 | ForEach-Object { $null = Invoke-AutopilotPlannerTick -Paths $paths -FixtureRoot $scenario -Now $script:T0.AddMinutes($_) }
        $loaded = Get-AutopilotJob -Paths $paths -JobId $job.jobId
        $loaded.phase | Should -Be 'Blocked'
        $loaded.blockedReason.code | Should -Be 'party-contract-unmet'
        $loaded.blockedReason.detail | Should -Match 'quest_not_in_leader_log'
        @(Get-AutopilotReceipts -Paths $paths -JobId $job.jobId | Where-Object { $_.event -eq 'replan_to_acquisition' }).Count | Should -Be 0
    }

    It 'duplicate ticks cannot duplicate the acquisition command' {
        $paths = New-TestPaths
        $scenario = Join-Path $TestDrive 'dedupe-acquire'
        New-QuestResponseFixture -Dir $scenario -FileName 'quest-acquire-10.json' -Json '{"ok":true,"order":"quest","phase":"acquire","state":"issued","guid":10}'
        $job = New-TestQuestJob -Paths $paths
        $job.metadata = [ordered]@{ questMode = 'acquire' }
        $job = Add-AutopilotJob -Paths $paths -Job $job
        1..8 | ForEach-Object { $null = Invoke-AutopilotPlannerTick -Paths $paths -FixtureRoot $scenario -Now $script:T0.AddMinutes($_) }
        $receipts = Get-AutopilotReceipts -Paths $paths -JobId $job.jobId
        $issued = @($receipts | Where-Object { $_.event -eq 'command' -and $_.command.action -eq 'quest-acquire' })
        $issued.Count | Should -Be 1
        @($receipts | Where-Object { $_.event -eq 'command_deduplicated' }).Count | Should -BeGreaterThan 0
    }
}

Describe 'Quest flow blockers (wire-level blocked payloads)' {
    It 'attempts recovery once on quest_no_destination, then blocks as quest-flow-blocked when budget is exhausted' {
        $paths = New-TestPaths
        $scenario = Join-Path $TestDrive 'quest-no-destination'
        New-QuestResponseFixture -Dir $scenario -FileName 'quest-10.json' -Json '{"ok":true,"order":"quest","phase":"blocked","code":"quest_no_destination","reason":"quest_no_destination"}'
        $job = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob -Paths $paths -RetryPolicy @{
                maxAttempts = 3
                maxRecoveriesPerObjective = 1
                backoffSeconds = 60
                onExhaust = 'block'
            })
        $results = @()
        1..6 | ForEach-Object { $results += Invoke-AutopilotPlannerTick -Paths $paths -FixtureRoot $scenario -Now $script:T0.AddMinutes($_) }
        @($results | Where-Object { $_.action -eq 'recovering-from-quest-flow' }).Count | Should -Be 1
        @($results | Where-Object { $_.action -eq 'blocked-quest-flow' }).Count | Should -Be 1

        $loaded = Get-AutopilotJob -Paths $paths -JobId $job.jobId
        $loaded.blockedReason.code | Should -Be 'quest-flow-blocked'
        [int64]$loaded.progress.recoveriesUsed | Should -Be 1

        $receipts = Get-AutopilotReceipts -Paths $paths -JobId $job.jobId
        @($receipts | Where-Object { $_.event -eq 'quest_no_destination_recovery' }).Count | Should -Be 1
        @($receipts | Where-Object { $_.event -eq 'recovery_delegated' }).Count | Should -Be 1
        @($receipts | Where-Object { $_.event -eq 'command' -and $_.command.action -eq 'recover' }).Count | Should -Be 1
        @($receipts | Where-Object { $_.event -eq 'command' -and $_.command.action -eq 'quest' }).Count | Should -Be 2
    }

    It 'maps quest_not_party_compatible to party-contract-unmet with zero recovery spend' {
        $paths = New-TestPaths
        $scenario = Join-Path $TestDrive 'party-block-compat'
        New-QuestResponseFixture -Dir $scenario -FileName 'quest-10.json' -Json '{"ok":true,"order":"quest","phase":"blocked","code":"quest_not_party_compatible","reason":"quest_not_party_compatible"}'
        $job = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob -Paths $paths)

        1..4 | ForEach-Object { $null = Invoke-AutopilotPlannerTick -Paths $paths -FixtureRoot $scenario -Now $script:T0.AddMinutes($_) }
        $loaded = Get-AutopilotJob -Paths $paths -JobId $job.jobId
        $loaded.phase | Should -Be 'Blocked'
        $loaded.blockedReason.code | Should -Be 'party-contract-unmet'
        $loaded.blockedReason.detail | Should -Match 'quest not compatible'
        [int64]$loaded.progress.recoveriesUsed | Should -Be 0
        $receipts = Get-AutopilotReceipts -Paths $paths -JobId $job.jobId
        @($receipts | Where-Object { $_.event -eq 'party_precondition' }).Count | Should -Be 1
    }
}

Describe 'Capability-manifest live eligibility (fail closed)' {
    It 'accepts only fresh deployed-runtime manifests with session identity' {
        $fresh = Join-Path $TestDrive 'manifest-deployed.json'
        New-ManifestFixture -Path $fresh -Source 'deployed-runtime' -GeneratedAt ($script:T0.ToString('o'))
        $provider = New-AutopilotCapabilityProvider -Kind manifest -ManifestPath $fresh
        (Test-AutopilotCapabilityProviderLiveEligible -Provider $provider -Now $script:T0.AddMinutes(30)).Eligible | Should -BeTrue
    }

    It 'rejects interim-probe, unspecified-source, stale, future-dated, and identity-less manifests' {
        $cases = @(
            @{ Name = 'interim'; Source = 'interim-probe'; GeneratedAt = $script:T0.ToString('o'); Now = $script:T0.AddMinutes(30); Match = "source is 'interim-probe'" },
            @{ Name = 'nosource'; Source = $null; GeneratedAt = $script:T0.ToString('o'); Now = $script:T0.AddMinutes(30); Match = "source is 'unspecified'" },
            @{ Name = 'stale'; Source = 'deployed-runtime'; GeneratedAt = $script:T0.ToString('o'); Now = $script:T0.AddHours(9); Match = 'stale' },
            @{ Name = 'future'; Source = 'deployed-runtime'; GeneratedAt = $script:T0.AddHours(2).ToString('o'); Now = $script:T0; Match = 'future-dated|stale' }
        )
        foreach ($case in $cases) {
            $path = Join-Path $TestDrive "manifest-$($case.Name).json"
            New-ManifestFixture -Path $path -Source $case.Source -GeneratedAt $case.GeneratedAt
            $provider = New-AutopilotCapabilityProvider -Kind manifest -ManifestPath $path
            $result = Test-AutopilotCapabilityProviderLiveEligible -Provider $provider -Now $case.Now
            $result.Eligible | Should -BeFalse
            $result.Reason | Should -Match $case.Match
        }
        (Test-AutopilotCapabilityProviderLiveEligible -Provider (New-AutopilotCapabilityProvider -Kind unavailable)).Eligible | Should -BeFalse
    }

    It 'refuses a live send end-to-end on a non-deployed-runtime manifest, before any socket' {
        $paths = New-TestPaths
        $interim = Join-Path $TestDrive 'manifest-interim-live.json'
        New-ManifestFixture -Path $interim -Source 'interim-probe' -GeneratedAt ((Get-Date).ToUniversalTime().ToString('o'))
        $provider = New-AutopilotCapabilityProvider -Kind manifest -ManifestPath $interim
        Write-AutopilotFileAtomic -Path $paths.AllowlistPath -Content '[10]'
        $job = New-TestQuestJob -Paths $paths
        $job.executionMode = 'live'
        $job = Add-AutopilotJob -Paths $paths -Job $job
        $job.characters.assignedGuids = @(10)
        $null = Request-AutopilotOwnership -Paths $paths -JobId $job.jobId -Guids @([int64]10)
        # Every other gate passes (capability IS available on this manifest);
        # the manifest hygiene gate still refuses - fail closed, no socket.
        { Invoke-AutopilotCommand -Paths $paths -Job $job -Action 'quest' -ActionParams @{ BotGuid = 10 } -ConfirmLiveBridge -CapabilityProvider $provider } |
            Should -Throw '*not live-eligible*'
    }
}

Describe 'Suspension and cancellation always release leases' {
    It 'Suspend-AutopilotJob releases every lease the job holds' {
        $paths = New-TestPaths
        $job = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob -Paths $paths)
        1..3 | ForEach-Object { $null = Invoke-AutopilotPlannerTick -Paths $paths -FixtureRoot $script:FixturesDefault -Now $script:T0.AddMinutes($_) }
        (Test-AutopilotOwnershipActive -Paths $paths -JobId $job.jobId -Guid 10 -Now $script:T0.AddMinutes(4)) | Should -BeTrue
        $suspended = Suspend-AutopilotJob -Paths $paths -JobId $job.jobId -Reason 'test' -Now $script:T0.AddMinutes(5)
        $suspended.phase | Should -Be 'Suspended'
        (Test-AutopilotOwnershipActive -Paths $paths -JobId $job.jobId -Guid 10 -Now $script:T0.AddMinutes(5)) | Should -BeFalse
    }

    It 'cancellation through the planner releases every lease' {
        $paths = New-TestPaths
        $job = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob -Paths $paths)
        1..3 | ForEach-Object { $null = Invoke-AutopilotPlannerTick -Paths $paths -FixtureRoot $script:FixturesDefault -Now $script:T0.AddMinutes($_) }
        $loaded = Get-AutopilotJob -Paths $paths -JobId $job.jobId
        $loaded.cancellation.requested = $true
        $loaded.cancellation.reason = 'test cancel'
        $null = Save-AutopilotJob -Paths $paths -Job $loaded
        $null = Invoke-AutopilotPlannerTick -Paths $paths -FixtureRoot $script:FixturesDefault -Now $script:T0.AddMinutes(4)
        (Get-AutopilotJob -Paths $paths -JobId $job.jobId).phase | Should -Be 'Cancelled'
        $ownership = Read-AutopilotOwnership -Paths $paths
        @($ownership.leases | Where-Object { [string]$_.state -eq 'active' }).Count | Should -Be 0
    }
}
