# AutoWoW Autopilot V1.1 - offline unit/behavior tests.
# Run: Invoke-Pester -Path .\autopilot\tests\autopilot.tests.ps1 -Output Detailed
# Fully offline: TestDrive state, fixture responses/manifests, no bridge socket,
# no MySQL, no writes outside TestDrive (ownership/enrollment paths are redirected).

BeforeAll {
    $script:AutopilotRoot = Split-Path -Parent $PSScriptRoot
    $script:ProjectRoot = Split-Path -Parent $script:AutopilotRoot
    . (Join-Path $script:AutopilotRoot 'AutopilotLib.ps1')

    $script:FixturesDefault = Join-Path $script:AutopilotRoot 'fixtures\default'
    $script:FixturesStep2 = Join-Path $script:AutopilotRoot 'fixtures\scenario-a-step2'
    $script:FixturesStep3 = Join-Path $script:AutopilotRoot 'fixtures\scenario-a-step3'
    $script:ManifestCurrent = Join-Path $script:AutopilotRoot 'fixtures\capabilities\current.json'
    $script:ManifestDeployedQuest = Join-Path $script:AutopilotRoot 'fixtures\capabilities\deployed-quest.json'
    $script:ManifestDeployedGather = Join-Path $script:AutopilotRoot 'fixtures\capabilities\deployed-gather.json'
    $script:ManifestUnknownSchema = Join-Path $script:AutopilotRoot 'fixtures\capabilities\unavailable.json'
    $script:ManifestMalformed = Join-Path $script:AutopilotRoot 'fixtures\capabilities\malformed.json'
    $script:OracleReceiptsFixture = Join-Path $script:AutopilotRoot 'fixtures\oracle-receipts\quest-792-run.jsonl'
    $script:T0 = [datetime]::new(2026, 7, 18, 12, 0, 0, [System.DateTimeKind]::Utc)

    function New-TestPaths {
        param([string]$Name = ('sandbox-' + [guid]::NewGuid().ToString('n').Substring(0, 8)))
        $root = Join-Path $TestDrive $Name
        Get-AutopilotPaths -StateRoot (Join-Path $root 'state') -ProjectRoot $script:ProjectRoot `
            -LogsRoot (Join-Path $root 'logs\autopilot') -RunId 'run-test-0001' `
            -OwnershipPath (Join-Path $root 'work\ownership.json') `
            -EnrollmentRequestPath (Join-Path $root 'work\oracle-enrollment-request.json')
    }

    function New-TestQuestJob {
        param([datetime]$Now = $script:T0, $Paths)
        $jobParams = @{
            Kind            = 'QuestLevel'
            Spec            = @{ partyLeaderGuid = 10 }
            EligibleGuids   = @([uint32]10)
            SuccessCriteria = @{ measure = 'quest-turnins'; counters = @{ questsTurnedIn = 1 } }
            Campaign        = 'test-league'
            Team            = 'northstar'
            Now             = $Now
        }
        if ($Paths) { $jobParams['Paths'] = $Paths }
        New-AutopilotJob @jobParams
    }

    function New-TestGatherJob {
        param([datetime]$Now = $script:T0, $Paths)
        $jobParams = @{
            Kind            = 'Gather'
            Spec            = @{ item = @{ itemId = 2770; count = 100 }; stopAtCount = 100 }
            EligibleGuids   = @([uint32]6)
            SuccessCriteria = @{ measure = 'inventory-item-count'; counters = @{ itemCount = 100 } }
            Campaign        = 'test-league'
            Now             = $Now
        }
        if ($Paths) { $jobParams['Paths'] = $Paths }
        New-AutopilotJob @jobParams
    }

    function Write-ForeignLease {
        param($Paths, [int64]$Guid, [string]$ExpiresAt, [string]$Instance = 'ap-foreign-ctrl', [string]$JobId = 'job-20260718T000000Z-ffffffff')
        $doc = [ordered]@{
            schema         = 'autowow.autopilot.ownership.v1'
            schema_version = 1
            updated_utc    = '2026-07-18T11:00:00.0000000Z'
            leases         = @([ordered]@{
                    schema               = 'autowow.autopilot.ownership.v1'
                    controllerInstanceId = $Instance
                    pid                  = 99999
                    jobId                = $JobId
                    characterGuid        = $Guid
                    acquiredAt           = '2026-07-18T11:00:00.0000000Z'
                    renewedAt            = '2026-07-18T11:00:00.0000000Z'
                    expiresAt            = $ExpiresAt
                    state                = 'active'
                })
        }
        Write-AutopilotFileAtomic -Path $Paths.OwnershipPath -Content ($doc | ConvertTo-Json -Depth 10)
    }
}

Describe 'Job creation and schema validation' {
    It 'creates a QuestLevel job that passes validation' {
        $paths = New-TestPaths
        $job = New-TestQuestJob
        $result = Test-AutopilotJob -Job $job -SchemaPath $paths.SchemaPath
        $result.Errors | Should -BeNullOrEmpty
        $result.Valid | Should -BeTrue
    }

    It 'produces a stable idempotency key and jobId for identical logical input' {
        $a = New-TestQuestJob
        $b = New-TestQuestJob
        $a.idempotencyKey | Should -Be $b.idempotencyKey
        $a.jobId | Should -Be $b.jobId
        $a.jobId | Should -Match '^job-20260718T120000Z-[0-9a-f]{8}$'
    }

    It 'changes the idempotency key when the spec changes' {
        $a = New-AutopilotJob -Kind 'QuestLevel' -Spec @{ partyLeaderGuid = 10 } -EligibleGuids @([uint32]10) `
            -SuccessCriteria @{ measure = 'quest-turnins'; counters = @{ questsTurnedIn = 1 } } -Now $script:T0
        $b = New-AutopilotJob -Kind 'QuestLevel' -Spec @{ partyLeaderGuid = 7 } -EligibleGuids @([uint32]7) `
            -SuccessCriteria @{ measure = 'quest-turnins'; counters = @{ questsTurnedIn = 1 } } -Now $script:T0
        $a.idempotencyKey | Should -Not -Be $b.idempotencyKey
    }

    It 'rejects a job with no success counters' {
        $paths = New-TestPaths
        $job = New-TestQuestJob
        $job.successCriteria = [ordered]@{ measure = 'quest-turnins'; counters = [ordered]@{} }
        (Test-AutopilotJob -Job $job -SchemaPath $paths.SchemaPath).Valid | Should -BeFalse
    }

    It 'rejects a QuestLevel job missing the questsTurnedIn counter (no fake-success path)' {
        $paths = New-TestPaths
        $job = New-TestQuestJob
        $job.successCriteria = [ordered]@{ measure = 'quest-turnins'; counters = [ordered]@{ somethingElse = 1 } }
        $result = Test-AutopilotJob -Job $job -SchemaPath $paths.SchemaPath
        $result.Valid | Should -BeFalse
        ($result.Errors -join ';') | Should -Match 'questsTurnedIn'
    }

    It 'rejects a Gather job without stopAtCount' {
        $paths = New-TestPaths
        $job = New-TestGatherJob
        $null = $job.spec.Remove('stopAtCount')
        (Test-AutopilotJob -Job $job -SchemaPath $paths.SchemaPath).Valid | Should -BeFalse
    }

    It 'rejects unknown capabilities (closed allow-list)' {
        $paths = New-TestPaths
        $job = New-TestQuestJob
        $job.permittedCapabilities = @('bridge.quest', 'bridge.mind_control')
        $result = Test-AutopilotJob -Job $job -SchemaPath $paths.SchemaPath
        $result.Valid | Should -BeFalse
        ($result.Errors -join ';') | Should -Match 'bridge.mind_control'
    }

    It 'rejects an unsupported schemaVersion (fail closed)' {
        $paths = New-TestPaths
        $job = New-TestQuestJob
        $job.schemaVersion = 2
        (Test-AutopilotJob -Job $job -SchemaPath $paths.SchemaPath).Valid | Should -BeFalse
    }
}

Describe 'Job store idempotence' {
    It 'refuses a duplicate non-terminal submission and receipts the dedupe' {
        $paths = New-TestPaths
        $first = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob)
        $second = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob -Now $script:T0.AddMinutes(5))
        $second.jobId | Should -Be $first.jobId
        @(Get-AutopilotJobs -Paths $paths).Count | Should -Be 1
        $receipts = Get-AutopilotReceipts -Paths $paths -JobId $first.jobId
        @($receipts | Where-Object { $_.event -eq 'job_deduplicated' }).Count | Should -Be 1
    }
}

Describe 'State machine' {
    It 'walks the legal main path and emits one receipt per transition with increasing seq' {
        $paths = New-TestPaths
        $job = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob)
        foreach ($to in @('Validating', 'Preparing', 'Executing', 'Verifying', 'Completed')) {
            $job = Invoke-AutopilotTransition -Paths $paths -Job $job -To $to -Reason 'test'
        }
        $job.phase | Should -Be 'Completed'
        $job.completedAt | Should -Not -BeNullOrEmpty
        $transitions = @(Get-AutopilotReceipts -Paths $paths -JobId $job.jobId | Where-Object { $_.event -eq 'job_transition' })
        $transitions.Count | Should -Be 5
        $seqs = @($transitions | ForEach-Object { [int]$_.seq })
        ($seqs | Sort-Object) | Should -Be $seqs
    }

    It 'throws on an illegal transition' {
        $paths = New-TestPaths
        $job = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob)
        { Invoke-AutopilotTransition -Paths $paths -Job $job -To 'Executing' -Reason 'skip' } | Should -Throw '*Illegal transition*'
    }

    It 'requires a typed code and detail for Blocked' {
        $paths = New-TestPaths
        $job = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob)
        $job = Invoke-AutopilotTransition -Paths $paths -Job $job -To 'Validating' -Reason 'test'
        { Invoke-AutopilotTransition -Paths $paths -Job $job -To 'Blocked' -Reason 'test' } | Should -Throw '*typed blockedReason code*'
        { Invoke-AutopilotTransition -Paths $paths -Job $job -To 'Blocked' -Blocked @{ code = 'not-a-real-code'; detail = 'x' } } | Should -Throw '*Unknown blocked code*'
        $job = Invoke-AutopilotTransition -Paths $paths -Job $job -To 'Blocked' -Blocked @{ code = 'roster-owned'; detail = 'test detail' }
        $job.blockedReason.code | Should -Be 'roster-owned'
    }

    It 'suspends and resumes to the prior phase only' {
        $paths = New-TestPaths
        $job = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob)
        $job = Invoke-AutopilotTransition -Paths $paths -Job $job -To 'Validating' -Reason 'test'
        $job = Invoke-AutopilotTransition -Paths $paths -Job $job -To 'Suspended' -Reason 'operator pause'
        $job.metadata.suspendedFromPhase | Should -Be 'Validating'
        { Invoke-AutopilotTransition -Paths $paths -Job $job -To 'Executing' -Reason 'bad resume' } | Should -Throw '*Illegal transition*'
        $job = Invoke-AutopilotTransition -Paths $paths -Job $job -To 'Validating' -Reason 'resume'
        $job.phase | Should -Be 'Validating'
    }
}

Describe 'Wire requests (single-sourced from autowow-control.ps1)' {
    It 'emits the exact bridge wire strings' {
        $paths = New-TestPaths
        (Get-AutopilotWireRequest -Paths $paths -Action 'quest' -ActionParams @{ BotGuid = 10 }) | Should -Be 'quest 10'
        (Get-AutopilotWireRequest -Paths $paths -Action 'quest-acquire' -ActionParams @{ BotGuid = 10 }) | Should -Be 'quest 10 acquire'
        (Get-AutopilotWireRequest -Paths $paths -Action 'questobjective' -ActionParams @{ BotGuid = 10 }) | Should -Be 'questobjective 10'
        (Get-AutopilotWireRequest -Paths $paths -Action 'snapshot' -ActionParams @{ BotGuid = 10 }) | Should -Be 'snapshot 10'
        (Get-AutopilotWireRequest -Paths $paths -Action 'party' -ActionParams @{ BotGuid = 10; MemberGuid = @(11, 14, 15, 19) }) | Should -Be 'party 10 11 14 15 19'
    }
}

Describe 'Capability provider (no source inference, fail closed)' {
    It 'reports EVERY capability undeployed under the default unavailable provider' {
        $provider = New-AutopilotCapabilityProvider -Kind unavailable
        foreach ($capability in @('bridge.quest', 'bridge.snapshot', 'oracle.gather_source', 'oracle.craft', 'oracle.quest_objective', 'sql.read')) {
            (Get-AutopilotCapabilityStatus -Capability $capability -Provider $provider).available | Should -BeFalse
        }
    }

    It 'keeps a unit-tested / source-present capability undeployed when the manifest says so' {
        $provider = New-AutopilotCapabilityProvider -Kind manifest -ManifestPath $script:ManifestCurrent
        $provider.Available | Should -BeTrue
        # gather_source: native adapter integrated + focused tests pass, but
        # runtime_authorized=false and deployed=false -> NOT available.
        $gather = Get-AutopilotCapabilityStatus -Capability 'oracle.gather_source' -Provider $provider
        $gather.nativeAdapterAvailable | Should -BeTrue
        $gather.available | Should -BeFalse
        # craft: 14/14 authoritative tests pass -> still NOT available.
        (Get-AutopilotCapabilityStatus -Capability 'oracle.craft' -Provider $provider).available | Should -BeFalse
        # pvp: 9/9 -> still NOT available.
        (Get-AutopilotCapabilityStatus -Capability 'oracle.pvp_objective' -Provider $provider).available | Should -BeFalse
        # bridge: server intentionally stopped for the maintenance build.
        (Get-AutopilotCapabilityStatus -Capability 'bridge.quest' -Provider $provider).available | Should -BeFalse
    }

    It 'reports available only when deployed + native adapter + runtime authorized + runtime enabled' {
        $provider = New-AutopilotCapabilityProvider -Kind manifest -ManifestPath $script:ManifestDeployedQuest
        (Get-AutopilotCapabilityStatus -Capability 'oracle.quest_objective' -Provider $provider).available | Should -BeTrue
        (Get-AutopilotCapabilityStatus -Capability 'bridge.quest' -Provider $provider).available | Should -BeTrue
        (Get-AutopilotCapabilityStatus -Capability 'oracle.gather_source' -Provider $provider).available | Should -BeFalse
    }

    It 'fails closed on an unknown manifest schema version' {
        $provider = New-AutopilotCapabilityProvider -Kind manifest -ManifestPath $script:ManifestUnknownSchema
        $provider.Available | Should -BeFalse
        $provider.Reason | Should -Match 'not supported'
        (Get-AutopilotCapabilityStatus -Capability 'oracle.quest_objective' -Provider $provider).available | Should -BeFalse
    }

    It 'fails closed on a malformed manifest' {
        $provider = New-AutopilotCapabilityProvider -Kind manifest -ManifestPath $script:ManifestMalformed
        $provider.Available | Should -BeFalse
        (Get-AutopilotCapabilityStatus -Capability 'bridge.quest' -Provider $provider).available | Should -BeFalse
    }

    It 'treats the future bridge provider as unavailable rather than inferring' {
        $provider = New-AutopilotCapabilityProvider -Kind bridge
        $provider.Available | Should -BeFalse
        $provider.Reason | Should -Match 'fail closed'
    }
}

Describe 'Capability gate (fail closed)' {
    It 'refuses a capability outside the job allow-list' {
        $paths = New-TestPaths
        $job = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob)
        { Invoke-AutopilotCommand -Paths $paths -Job $job -Action 'wsg-queue' -ActionParams @{ MemberGuid = @(1..20) } } |
            Should -Throw '*not in job*'
    }

    It 'refuses lab-only capabilities even when listed' {
        $paths = New-TestPaths
        $job = New-TestQuestJob
        $job.permittedCapabilities = @('bridge.fixture.init')
        $job = Add-AutopilotJob -Paths $paths -Job $job
        { Invoke-AutopilotCommand -Paths $paths -Job $job -Action 'fixture-init' -ActionParams @{ BotGuid = 10; Level = 80; SpecIndex = 0; Quality = 4 } } |
            Should -Throw '*lab-only*'
    }

    It 'records capability evidence state on dry-run command receipts without claiming availability' {
        $paths = New-TestPaths
        $job = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob)
        $job.characters.assignedGuids = @(10)
        $null = Invoke-AutopilotCommand -Paths $paths -Job $job -Action 'quest' -ActionParams @{ BotGuid = 10 }
        $command = @(Get-AutopilotReceipts -Paths $paths -JobId $job.jobId | Where-Object { $_.event -eq 'command' })[-1]
        $command.command.capability_available | Should -BeFalse
        $command.command.capability_source | Should -Be 'unavailable'
        $command.command.sent | Should -BeFalse
    }
}

Describe 'Kind capability gating (Sol #6 + V1.1 corrections)' {
    It 'blocks Travel on oracle.navigate (same-map travel developed in isolation)' {
        $paths = New-TestPaths
        $job = New-AutopilotJob -Kind 'Travel' -Spec @{ destination = 'Stormwind' } -EligibleGuids @([uint32]10) `
            -SuccessCriteria @{ measure = 'arrival-at-destination'; counters = @{ arrived = 1 } } -Now $script:T0
        $job = Add-AutopilotJob -Paths $paths -Job $job
        1..2 | ForEach-Object { $null = Invoke-AutopilotPlannerTick -Paths $paths }
        $loaded = Get-AutopilotJob -Paths $paths -JobId $job.jobId
        $loaded.phase | Should -Be 'Blocked'
        $loaded.blockedReason.code | Should -Be 'oracle-capability-not-deployed'
        $loaded.blockedReason.detail | Should -Match 'oracle.navigate'
    }

    It 'blocks WSG Battleground on oracle.pvp_objective (no legacy tactics as authority)' {
        $paths = New-TestPaths
        $job = New-AutopilotJob -Kind 'Battleground' -Spec @{ name = 'wsg'; teamSize = 10 } -EligibleGuids @(1..20 | ForEach-Object { [uint32]$_ }) `
            -SuccessCriteria @{ measure = 'battleground-scoreboard'; counters = @{ matchesCompleted = 1 } } -Now $script:T0
        $job = Add-AutopilotJob -Paths $paths -Job $job
        1..2 | ForEach-Object { $null = Invoke-AutopilotPlannerTick -Paths $paths }
        $loaded = Get-AutopilotJob -Paths $paths -JobId $job.jobId
        $loaded.phase | Should -Be 'Blocked'
        $loaded.blockedReason.detail | Should -Match 'oracle.pvp_objective'
    }

    It 'blocks Craft on oracle.craft even though its executor passes 14/14 unit tests' {
        $paths = New-TestPaths
        $provider = New-AutopilotCapabilityProvider -Kind manifest -ManifestPath $script:ManifestCurrent
        $job = New-AutopilotJob -Kind 'Craft' -Spec @{ recipeSpellId = 2660; count = 5 } -EligibleGuids @([uint32]6) `
            -SuccessCriteria @{ measure = 'inventory-item-count'; counters = @{ crafted = 5 } } -Now $script:T0
        $job = Add-AutopilotJob -Paths $paths -Job $job
        1..2 | ForEach-Object { $null = Invoke-AutopilotPlannerTick -Paths $paths -CapabilityProvider $provider }
        $loaded = Get-AutopilotJob -Paths $paths -JobId $job.jobId
        $loaded.phase | Should -Be 'Blocked'
        $loaded.blockedReason.code | Should -Be 'oracle-capability-not-deployed'
        $loaded.blockedReason.detail | Should -Match 'oracle.craft'
    }
}

Describe 'Capability manifest transition (unavailable -> deployed)' {
    It 'unblocks a Gather job when the manifest starts evidencing the gathering Oracle' {
        $paths = New-TestPaths
        $job = Add-AutopilotJob -Paths $paths -Job (New-TestGatherJob)
        1..2 | ForEach-Object { $null = Invoke-AutopilotPlannerTick -Paths $paths }   # unavailable provider
        (Get-AutopilotJob -Paths $paths -JobId $job.jobId).phase | Should -Be 'Blocked'

        $deployed = New-AutopilotCapabilityProvider -Kind manifest -ManifestPath $script:ManifestDeployedGather
        $tick = Invoke-AutopilotPlannerTick -Paths $paths -CapabilityProvider $deployed
        @($tick | Where-Object { $_.action -eq 'capability-unblocked' }).Count | Should -Be 1
        (Get-AutopilotJob -Paths $paths -JobId $job.jobId).phase | Should -Be 'Validating'

        # V1.2: Gather has a real dry-run executor, so once the manifest
        # evidences the capability the job proceeds - through Preparing into
        # Executing - recording oracle would_send intents, never sending.
        $null = Invoke-AutopilotPlannerTick -Paths $paths -CapabilityProvider $deployed   # Validating -> Preparing
        (Get-AutopilotJob -Paths $paths -JobId $job.jobId).phase | Should -Be 'Preparing'
        $null = Invoke-AutopilotPlannerTick -Paths $paths -CapabilityProvider $deployed   # Preparing -> Executing
        $null = Invoke-AutopilotPlannerTick -Paths $paths -CapabilityProvider $deployed   # Executing -> Verifying (oracle intent)
        $receipts = Get-AutopilotReceipts -Paths $paths -JobId $job.jobId
        $oracleCommands = @($receipts | Where-Object { $_.event -eq 'command' -and $_.command.surface -eq 'oracle' })
        $oracleCommands.Count | Should -BeGreaterThan 0
        $oracleCommands[0].command.operation | Should -Be 'gather_source'
        $oracleCommands[0].command.sent | Should -BeFalse
        $oracleCommands[0].command.would_send | Should -BeTrue
    }
}

Describe 'Ownership leasing (Sol #1)' {
    It 'acquires an active lease with the full field set' {
        $paths = New-TestPaths
        $job = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob)
        $result = Request-AutopilotOwnership -Paths $paths -JobId $job.jobId -Guids @([int64]10) -Now $script:T0
        $result.Acquired | Should -BeTrue
        $ownership = Read-AutopilotOwnership -Paths $paths
        $lease = @($ownership.leases)[0]
        $lease.schema | Should -Be 'autowow.autopilot.ownership.v1'
        $lease.controllerInstanceId | Should -Match '^ap-'
        [int64]$lease.pid | Should -Be $PID
        $lease.jobId | Should -Be $job.jobId
        [int64]$lease.characterGuid | Should -Be 10
        $lease.state | Should -Be 'active'
        $lease.acquiredAt | Should -Not -BeNullOrEmpty
        $lease.expiresAt | Should -Not -BeNullOrEmpty
        (Test-AutopilotOwnershipActive -Paths $paths -JobId $job.jobId -Guid 10 -Now $script:T0.AddMinutes(1)) | Should -BeTrue
    }

    It 'refuses a foreign active lease and receipts the conflict; never supersedes' {
        $paths = New-TestPaths
        $job = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob)
        Write-ForeignLease -Paths $paths -Guid 10 -ExpiresAt ($script:T0.AddHours(2).ToString('o'))
        $result = Request-AutopilotOwnership -Paths $paths -JobId $job.jobId -Guids @([int64]10) -Now $script:T0
        $result.Acquired | Should -BeFalse
        @($result.Conflicts)[0].ownerInstance | Should -Be 'ap-foreign-ctrl'
        $receipts = Get-AutopilotReceipts -Paths $paths -JobId $job.jobId
        @($receipts | Where-Object { $_.event -eq 'ownership_conflict' }).Count | Should -Be 1
        # The foreign lease is untouched.
        $ownership = Read-AutopilotOwnership -Paths $paths
        @($ownership.leases)[0].controllerInstanceId | Should -Be 'ap-foreign-ctrl'
        @($ownership.leases)[0].state | Should -Be 'active'
    }

    It 'reclaims a foreign lease only after time expires it' {
        $paths = New-TestPaths
        $job = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob)
        Write-ForeignLease -Paths $paths -Guid 10 -ExpiresAt ($script:T0.AddMinutes(-5).ToString('o'))
        $result = Request-AutopilotOwnership -Paths $paths -JobId $job.jobId -Guids @([int64]10) -Now $script:T0
        $result.Acquired | Should -BeTrue
        $ownership = Read-AutopilotOwnership -Paths $paths
        $states = @($ownership.leases | ForEach-Object { [string]$_.state })
        $states | Should -Contain 'expired'
        $states | Should -Contain 'active'
        $mine = @($ownership.leases | Where-Object { [string]$_.state -eq 'active' })[0]
        $mine.jobId | Should -Be $job.jobId
    }

    It 'renews its own lease and releases it on demand' {
        $paths = New-TestPaths
        $job = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob)
        $null = Request-AutopilotOwnership -Paths $paths -JobId $job.jobId -Guids @([int64]10) -Now $script:T0
        $renew = Request-AutopilotOwnership -Paths $paths -JobId $job.jobId -Guids @([int64]10) -Now $script:T0.AddMinutes(5)
        $renew.Acquired | Should -BeTrue
        $renew.Reason | Should -Be 'renewed'
        (Release-AutopilotOwnership -Paths $paths -JobId $job.jobId) | Should -BeTrue
        (Test-AutopilotOwnershipActive -Paths $paths -JobId $job.jobId -Guid 10 -Now $script:T0.AddMinutes(6)) | Should -BeFalse
        $receipts = Get-AutopilotReceipts -Paths $paths -JobId $job.jobId
        @($receipts | Where-Object { $_.event -eq 'ownership_released' }).Count | Should -Be 1
    }

    It 'fails closed on an unsupported ownership schema' {
        $paths = New-TestPaths
        $job = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob)
        Write-AutopilotFileAtomic -Path $paths.OwnershipPath -Content '{"schema":"autowow.autopilot.ownership.v9","schema_version":9,"leases":[]}'
        $result = Request-AutopilotOwnership -Paths $paths -JobId $job.jobId -Guids @([int64]10) -Now $script:T0
        $result.Acquired | Should -BeFalse
        $result.FailClosed | Should -BeTrue
    }

    It 'planner blocks Blocked(roster-owned) on conflict and re-acquires after release' {
        $paths = New-TestPaths
        $job = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob)
        Write-ForeignLease -Paths $paths -Guid 10 -ExpiresAt ((Get-Date).ToUniversalTime().AddHours(2).ToString('o'))
        1..3 | ForEach-Object { $null = Invoke-AutopilotPlannerTick -Paths $paths -FixtureRoot $script:FixturesDefault }
        $loaded = Get-AutopilotJob -Paths $paths -JobId $job.jobId
        $loaded.phase | Should -Be 'Blocked'
        $loaded.blockedReason.code | Should -Be 'roster-owned'
        $loaded.blockedReason.detail | Should -Match 'ap-foreign-ctrl'

        # Foreign controller releases; the next tick re-acquires and resumes.
        Save-AutopilotOwnership -Paths $paths -Leases @()
        $tick = Invoke-AutopilotPlannerTick -Paths $paths -FixtureRoot $script:FixturesDefault
        @($tick | Where-Object { $_.action -eq 'ownership-unblocked' }).Count | Should -Be 1
        (Get-AutopilotJob -Paths $paths -JobId $job.jobId).phase | Should -Be 'Validating'
    }
}

Describe 'Enrollment proposal (Sol #3)' {
    It 'writes a receipted proposal file and never touches server config' {
        $paths = New-TestPaths
        $job = Add-AutopilotJob -Paths $paths -Job (New-TestGatherJob)
        $proposal = New-AutopilotEnrollmentProposal -Paths $paths -JobId $job.jobId -Now $script:T0
        Test-Path $paths.EnrollmentRequestPath | Should -BeTrue
        $doc = Get-Content $paths.EnrollmentRequestPath -Raw | ConvertFrom-Json
        $doc.schema | Should -Be 'autowow.autopilot.oracle-enrollment-request.v1'
        $doc.controllerInstanceId | Should -Match '^ap-'
        $request = @($doc.requests)[0]
        $request.jobId | Should -Be $job.jobId
        @($request.guids) | Should -Be @(6)
        @($request.requiredCapabilities) | Should -Contain 'oracle.gather_source'
        $request.expiresAt | Should -Not -BeNullOrEmpty
        $request.reason | Should -Not -BeNullOrEmpty
        $receipts = Get-AutopilotReceipts -Paths $paths -JobId $job.jobId
        @($receipts | Where-Object { $_.event -eq 'enrollment_proposed' }).Count | Should -Be 1
    }

    It 'upserts by jobId (idempotent re-proposal)' {
        $paths = New-TestPaths
        $job = Add-AutopilotJob -Paths $paths -Job (New-TestGatherJob)
        $null = New-AutopilotEnrollmentProposal -Paths $paths -JobId $job.jobId -Now $script:T0
        $null = New-AutopilotEnrollmentProposal -Paths $paths -JobId $job.jobId -Now $script:T0.AddMinutes(10)
        $doc = Get-Content $paths.EnrollmentRequestPath -Raw | ConvertFrom-Json
        @($doc.requests).Count | Should -Be 1
    }
}

Describe 'Oracle receipt provider and correlation (Sol #2)' {
    It 'reports evidence unavailable by default (never scrapes the in-memory ring)' {
        $paths = New-TestPaths
        $job = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob)
        $provider = New-AutopilotOracleReceiptProvider -Kind unavailable
        $result = Join-AutopilotOracleEvidence -Paths $paths -JobId $job.jobId -Provider $provider
        $result.available | Should -BeFalse
        $result.reason | Should -Match 'never scraped'
    }

    It 'parses a JSONL fixture and correlates by botGuid + timestamp window' {
        $paths = New-TestPaths
        $job = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob -Paths $paths)
        1..4 | ForEach-Object { $null = Invoke-AutopilotPlannerTick -Paths $paths -FixtureRoot $script:FixturesDefault -Now $script:T0 }
        $provider = New-AutopilotOracleReceiptProvider -Kind jsonl -Path $script:OracleReceiptsFixture
        $provider.Available | Should -BeTrue
        @($provider.Receipts).Count | Should -Be 3
        $result = Join-AutopilotOracleEvidence -Paths $paths -JobId $job.jobId -Provider $provider
        $result.available | Should -BeTrue
        $questCommand = @($result.correlations | Where-Object { $_.command_action -eq 'quest' })[0]
        # Bot 10's two oracle receipts match; bot 7's receipt must not.
        @($questCommand.oracle_matches).Count | Should -Be 2
        @($questCommand.oracle_matches | ForEach-Object { [int64]$_.bot_guid } | Sort-Object -Unique) | Should -Be @(10)
        $match = @($questCommand.oracle_matches)[0]
        $match.decision_id | Should -Be 501
        $match.operation | Should -Be 'quest_objective'
        $match.target_hash | Should -Be 'q792:f1:s0:e3101'
        $match.native_steps | Should -Be 1
        @($match.matched_by) | Should -Contain 'botGuid'
    }
}

Describe 'Live-socket quintuple gate (V1.1)' {
    It 'refuses live commands without -ConfirmLiveBridge' {
        $paths = New-TestPaths
        $job = New-TestQuestJob -Paths $paths
        $job.executionMode = 'live'
        $job = Add-AutopilotJob -Paths $paths -Job $job
        $job.characters.assignedGuids = @(10)
        { Invoke-AutopilotCommand -Paths $paths -Job $job -Action 'quest' -ActionParams @{ BotGuid = 10 } } |
            Should -Throw '*ConfirmLiveBridge*'
    }

    It 'refuses live commands for characters not enrolled in the allow-list' {
        $paths = New-TestPaths
        $job = New-TestQuestJob -Paths $paths
        $job.executionMode = 'live'
        $job = Add-AutopilotJob -Paths $paths -Job $job
        $job.characters.assignedGuids = @(10)
        { Invoke-AutopilotCommand -Paths $paths -Job $job -Action 'quest' -ActionParams @{ BotGuid = 10 } -ConfirmLiveBridge } |
            Should -Throw '*not enrolled*'
    }

    It 'refuses live commands without an active ownership lease' {
        $paths = New-TestPaths
        Write-AutopilotFileAtomic -Path $paths.AllowlistPath -Content '[10]'
        $job = New-TestQuestJob -Paths $paths
        $job.executionMode = 'live'
        $job = Add-AutopilotJob -Paths $paths -Job $job
        $job.characters.assignedGuids = @(10)
        { Invoke-AutopilotCommand -Paths $paths -Job $job -Action 'quest' -ActionParams @{ BotGuid = 10 } -ConfirmLiveBridge } |
            Should -Throw '*ownership*'
    }

    It 'refuses live commands without deployed capability evidence' {
        $paths = New-TestPaths
        Write-AutopilotFileAtomic -Path $paths.AllowlistPath -Content '[10]'
        $job = New-TestQuestJob -Paths $paths
        $job.executionMode = 'live'
        $job = Add-AutopilotJob -Paths $paths -Job $job
        $job.characters.assignedGuids = @(10)
        $null = Request-AutopilotOwnership -Paths $paths -JobId $job.jobId -Guids @([int64]10)
        { Invoke-AutopilotCommand -Paths $paths -Job $job -Action 'quest' -ActionParams @{ BotGuid = 10 } -ConfirmLiveBridge } |
            Should -Throw '*no deployed-runtime evidence*'
    }
}

Describe 'Receipt placement (Sol #8)' {
    It 'places live job receipts under the logs run directory with the runId in the job doc' {
        $paths = New-TestPaths
        $job = New-AutopilotJob -Kind 'QuestLevel' -Spec @{ partyLeaderGuid = 10 } -EligibleGuids @([uint32]10) `
            -SuccessCriteria @{ measure = 'quest-turnins'; counters = @{ questsTurnedIn = 1 } } `
            -ExecutionMode 'live' -Paths $paths -Now $script:T0
        $job.receiptLog.runId | Should -Be 'run-test-0001'
        $job.receiptLog.file | Should -Be (Join-Path (Join-Path $paths.LogsRoot 'run-test-0001') "$($job.jobId).jsonl")
        $saved = Add-AutopilotJob -Paths $paths -Job $job
        Test-Path $saved.receiptLog.file | Should -BeTrue
        # And it landed under LogsRoot, not the state receipts dir.
        $saved.receiptLog.file.StartsWith($paths.LogsRoot) | Should -BeTrue
        Test-Path (Join-Path $paths.ReceiptsDir "$($saved.jobId).jsonl") | Should -BeFalse
    }

    It 'keeps dry-run job receipts under the state root' {
        $paths = New-TestPaths
        $job = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob -Paths $paths)
        $job.receiptLog.runId | Should -Be 'state'
        (Get-AutopilotReceiptPath -Paths $paths -JobId $job.jobId).StartsWith($paths.ReceiptsDir) | Should -BeTrue
    }
}

Describe 'Scenario A (dry-run): QuestLevel walks to Completed on objective evidence' {
    It 'advances through the phase path, records would_send commands, and completes only on rewarded evidence' {
        $paths = New-TestPaths
        $job = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob)

        # Tick 1: Queued -> Validating
        $null = Invoke-AutopilotPlannerTick -Paths $paths -FixtureRoot $script:FixturesDefault
        (Get-AutopilotJob -Paths $paths -JobId $job.jobId).phase | Should -Be 'Validating'
        # Tick 2: Validating -> Preparing
        $null = Invoke-AutopilotPlannerTick -Paths $paths -FixtureRoot $script:FixturesDefault
        (Get-AutopilotJob -Paths $paths -JobId $job.jobId).phase | Should -Be 'Preparing'
        # Tick 3: Preparing -> Executing (leader bound, ownership acquired, travel delegated)
        $null = Invoke-AutopilotPlannerTick -Paths $paths -FixtureRoot $script:FixturesDefault
        $loaded = Get-AutopilotJob -Paths $paths -JobId $job.jobId
        $loaded.phase | Should -Be 'Executing'
        @($loaded.characters.assignedGuids) | Should -Be @(10)
        (Test-AutopilotOwnershipActive -Paths $paths -JobId $job.jobId -Guid 10) | Should -BeTrue
        # Tick 4: Executing -> Verifying (quest order recorded dry-run)
        $null = Invoke-AutopilotPlannerTick -Paths $paths -FixtureRoot $script:FixturesDefault
        (Get-AutopilotJob -Paths $paths -JobId $job.jobId).phase | Should -Be 'Verifying'
        # Tick 5: baseline objective 0/8 -> back to Executing, not complete
        $null = Invoke-AutopilotPlannerTick -Paths $paths -FixtureRoot $script:FixturesDefault
        (Get-AutopilotJob -Paths $paths -JobId $job.jobId).phase | Should -Be 'Executing'
        # Tick 6/7: progressed 3/8 -> still not complete
        $null = Invoke-AutopilotPlannerTick -Paths $paths -FixtureRoot $script:FixturesStep2
        $null = Invoke-AutopilotPlannerTick -Paths $paths -FixtureRoot $script:FixturesStep2
        (Get-AutopilotJob -Paths $paths -JobId $job.jobId).phase | Should -Be 'Executing'
        # Tick 8/9: complete 8/8 + rewarded -> Completed
        $null = Invoke-AutopilotPlannerTick -Paths $paths -FixtureRoot $script:FixturesStep3
        $null = Invoke-AutopilotPlannerTick -Paths $paths -FixtureRoot $script:FixturesStep3
        $final = Get-AutopilotJob -Paths $paths -JobId $job.jobId

        $final.phase | Should -Be 'Completed'
        [int64]$final.progress.counters.questsTurnedIn | Should -Be 1
        # Ownership released on completion.
        (Test-AutopilotOwnershipActive -Paths $paths -JobId $job.jobId -Guid 10) | Should -BeFalse

        $receipts = Get-AutopilotReceipts -Paths $paths -JobId $job.jobId
        $commands = @($receipts | Where-Object { $_.event -eq 'command' })
        $commands.Count | Should -BeGreaterThan 0
        foreach ($command in $commands) {
            $command.command.sent | Should -BeFalse
            $command.command.would_send | Should -BeTrue
            $command.command.mode | Should -Be 'dry-run'
        }
        $questIssues = @($commands | Where-Object { $_.command.action -eq 'quest' })
        $questIssues.Count | Should -Be 1
        $questIssues[0].command.wire | Should -Be 'quest 10'
        @($receipts | Where-Object { $_.event -eq 'command_deduplicated' }).Count | Should -BeGreaterThan 0
        @($receipts | Where-Object { $_.event -eq 'ownership_acquired' }).Count | Should -Be 1
        $progress = @($receipts | Where-Object { $_.event -eq 'progress_observed' })
        @($progress | ForEach-Object { [int64]$_.current }) | Should -Be @(0, 3, 8)
        @($progress | Where-Object { $_.rewarded }).Count | Should -Be 1
    }
}

Describe 'Scenario B (represented, disabled): Gather blocks on the missing Oracle capability' {
    It 'validates, then blocks with oracle-capability-not-deployed and issues zero commands' {
        $paths = New-TestPaths
        $job = Add-AutopilotJob -Paths $paths -Job (New-TestGatherJob)
        $null = Invoke-AutopilotPlannerTick -Paths $paths     # Queued -> Validating
        $null = Invoke-AutopilotPlannerTick -Paths $paths     # Validating -> Blocked
        $loaded = Get-AutopilotJob -Paths $paths -JobId $job.jobId
        $loaded.phase | Should -Be 'Blocked'
        $loaded.blockedReason.code | Should -Be 'oracle-capability-not-deployed'
        $loaded.blockedReason.detail | Should -Match 'oracle.gather_source'
        $receipts = Get-AutopilotReceipts -Paths $paths -JobId $job.jobId
        @($receipts | Where-Object { $_.event -eq 'command' }).Count | Should -Be 0
    }
}

Describe 'Scenario G: restart recovery without duplicate irreversible commands' {
    It 'recovers a stale lock, reconciles from the receipt ledger, and never re-issues a receipted command' {
        $paths = New-TestPaths
        $job = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob)

        # Run to the point where the irreversible quest order is receipted.
        1..4 | ForEach-Object { $null = Invoke-AutopilotPlannerTick -Paths $paths -FixtureRoot $script:FixturesDefault }
        (Get-AutopilotJob -Paths $paths -JobId $job.jobId).phase | Should -Be 'Verifying'

        # Simulate a crashed prior controller: stale pid file from a dead process.
        $deadProcess = Start-Process -FilePath 'pwsh' -ArgumentList '-NoProfile', '-Command', 'exit 0' -PassThru -WindowStyle Hidden
        $deadProcess.WaitForExit()
        $staleLock = @{ pid = $deadProcess.Id; started_utc = '2026-07-18T00:00:00.0000000Z'; script = 'autowow-autopilot.ps1' } | ConvertTo-Json
        Write-AutopilotFileAtomic -Path $paths.LockPath -Content $staleLock

        # Simulate the crash also leaving the job snapshot behind the ledger.
        $stale = Get-AutopilotJob -Paths $paths -JobId $job.jobId
        $stale.phase = 'Executing'
        $null = Save-AutopilotJob -Paths $paths -Job $stale

        # Restart: lock recovery + reconciliation.
        Enter-AutopilotLock -Paths $paths
        $reconciled = Invoke-AutopilotReconcile -Paths $paths
        @($reconciled | Where-Object { $_.jobId -eq $job.jobId })[0].corrected | Should -BeTrue
        (Get-AutopilotJob -Paths $paths -JobId $job.jobId).phase | Should -Be 'Verifying'

        $controllerReceipts = Get-AutopilotReceipts -Paths $paths -JobId 'controller'
        @($controllerReceipts | Where-Object { $_.event -eq 'stale_lock_recovered' }).Count | Should -Be 1
        @($controllerReceipts | Where-Object { $_.event -eq 'reconciliation_completed' }).Count | Should -Be 1

        # Continue after restart; the quest order must not be issued a second time.
        1..3 | ForEach-Object { $null = Invoke-AutopilotPlannerTick -Paths $paths -FixtureRoot $script:FixturesDefault }
        $receipts = Get-AutopilotReceipts -Paths $paths -JobId $job.jobId
        $issued = @($receipts | Where-Object { $_.event -eq 'command' -and $_.command.action -eq 'quest' })
        $issued.Count | Should -Be 1

        # Idempotency across the whole ledger: one issue per irreversible key.
        $byKey = @($receipts | Where-Object { $_.event -eq 'command' -and $_.command.irreversible }) |
            Group-Object { $_.command.idempotency_key }
        foreach ($group in $byKey) { $group.Count | Should -Be 1 }

        # Ownership survives the restart for the same controller instance + job.
        (Test-AutopilotOwnershipActive -Paths $paths -JobId $job.jobId -Guid 10) | Should -BeTrue

        Exit-AutopilotLock -Paths $paths
    }

    It 'refuses a second live controller instance' {
        $paths = New-TestPaths
        Enter-AutopilotLock -Paths $paths
        try {
            { Enter-AutopilotLock -Paths $paths } | Should -Throw '*already running*'
        }
        finally { Exit-AutopilotLock -Paths $paths }
    }
}

Describe 'Explain (wait-state taxonomy)' {
    It 'answers the five questions and classifies a capability-blocked Gather job' {
        $paths = New-TestPaths
        $job = Add-AutopilotJob -Paths $paths -Job (New-TestGatherJob)
        1..2 | ForEach-Object { $null = Invoke-AutopilotPlannerTick -Paths $paths }
        $explanation = Get-AutopilotExplanation -Paths $paths -Guid 6
        $explanation | Should -Not -BeNullOrEmpty
        $explanation.waitState | Should -Be 'waiting-for-server-capability'
        $explanation.whatItIsDoing | Should -Match 'Gather'
        $explanation.whyWaiting | Should -Match 'oracle-capability-not-deployed'
        $explanation.whatNext | Should -Match 'manifest'
        @($explanation.requiredCapabilities | Where-Object { $_.capability -eq 'oracle.gather_source' })[0].available | Should -BeFalse
        $explanation.executionMode | Should -Be 'dry-run'
    }

    It 'classifies an ownership-blocked job as waiting-for-ownership' {
        $paths = New-TestPaths
        $job = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob)
        Write-ForeignLease -Paths $paths -Guid 10 -ExpiresAt ((Get-Date).ToUniversalTime().AddHours(2).ToString('o'))
        1..3 | ForEach-Object { $null = Invoke-AutopilotPlannerTick -Paths $paths -FixtureRoot $script:FixturesDefault }
        $explanation = Get-AutopilotExplanation -Paths $paths -JobId $job.jobId
        $explanation.waitState | Should -Be 'waiting-for-ownership'
        $explanation.whyWaiting | Should -Match 'ap-foreign-ctrl'
    }

    It 'prefers a job the character was assigned to over a pool it is merely eligible for' {
        $paths = New-TestPaths
        $quest = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob)
        $wsg = New-AutopilotJob -Kind 'Battleground' -Spec @{ name = 'wsg'; teamSize = 10 } `
            -EligibleGuids @(1..20 | ForEach-Object { [uint32]$_ }) `
            -SuccessCriteria @{ measure = 'battleground-scoreboard'; counters = @{ matchesCompleted = 1 } } -Now $script:T0.AddMinutes(1)
        $null = Add-AutopilotJob -Paths $paths -Job $wsg
        # Walk the quest to Completed (guid 10 assigned there; wsg only lists it eligible).
        1..5 | ForEach-Object { $null = Invoke-AutopilotPlannerTick -Paths $paths -FixtureRoot $script:FixturesDefault }
        1..4 | ForEach-Object { $null = Invoke-AutopilotPlannerTick -Paths $paths -FixtureRoot $script:FixturesStep3 }
        (Get-AutopilotJob -Paths $paths -JobId $quest.jobId).phase | Should -Be 'Completed'
        (Get-AutopilotExplanation -Paths $paths -Guid 10).jobId | Should -Be $quest.jobId
        # A character only in the eligible pool still resolves to that job.
        (Get-AutopilotExplanation -Paths $paths -Guid 15).jobId | Should -Not -Be $quest.jobId
    }

    It 'classifies active and terminal phases' {
        $paths = New-TestPaths
        $job = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob)
        1..4 | ForEach-Object { $null = Invoke-AutopilotPlannerTick -Paths $paths -FixtureRoot $script:FixturesDefault }
        (Get-AutopilotExplanation -Paths $paths -JobId $job.jobId).waitState | Should -Be 'verifying'
        $loaded = Get-AutopilotJob -Paths $paths -JobId $job.jobId
        $loaded.cancellation.requested = $true
        $loaded.cancellation.reason = 'test'
        $null = Save-AutopilotJob -Paths $paths -Job $loaded
        $null = Invoke-AutopilotPlannerTick -Paths $paths -FixtureRoot $script:FixturesDefault
        (Get-AutopilotExplanation -Paths $paths -JobId $job.jobId).waitState | Should -Be 'terminal-failure'
    }
}

Describe 'Receipt file integrity' {
    It 'writes append-only JSONL with schema, monotonic seq, and no secrets' {
        $paths = New-TestPaths
        $job = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob)
        1..4 | ForEach-Object { $null = Invoke-AutopilotPlannerTick -Paths $paths -FixtureRoot $script:FixturesDefault }
        $file = Get-AutopilotReceiptPath -Paths $paths -JobId $job.jobId
        $lines = @([System.IO.File]::ReadAllLines($file) | Where-Object { $_ })
        $lines.Count | Should -BeGreaterThan 3
        $seq = 0
        foreach ($line in $lines) {
            $record = $line | ConvertFrom-Json
            $record.schema | Should -Be 'autowow.autopilot.receipt.v1'
            [int]$record.seq | Should -Be ($seq + 1)
            $seq = [int]$record.seq
            $line | Should -Not -Match '(?i)password|MYSQL_PWD|connectionstring'
        }
    }
}
