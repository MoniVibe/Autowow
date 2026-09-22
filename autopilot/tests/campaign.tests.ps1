# AutoWoW Autopilot V1.2 campaign engine tests.
# Run: Invoke-Pester -Path .\autopilot\tests\campaign.tests.ps1 -Output Detailed
# Fully offline; TestDrive state; fixture providers only.

BeforeAll {
    $script:AutopilotRoot = Split-Path -Parent $PSScriptRoot
    $script:ProjectRoot = Split-Path -Parent $script:AutopilotRoot
    . (Join-Path $script:AutopilotRoot 'AutopilotLib.ps1')
    foreach ($module in (Get-ChildItem -Path (Join-Path $script:AutopilotRoot 'modules') -Filter '*.ps1' | Sort-Object Name)) { . $module.FullName }

    $script:SampleCampaign = Join-Path $script:AutopilotRoot 'campaigns\northstar-sample.json'
    $script:RosterFixture = Join-Path $script:AutopilotRoot 'fixtures\roster\league-sample.json'
    $script:ManifestCurrent = Join-Path $script:AutopilotRoot 'fixtures\capabilities\current.json'
    $script:FixturesDefault = Join-Path $script:AutopilotRoot 'fixtures\default'
    $script:T0 = [datetime]::new(2026, 7, 18, 20, 0, 0, [System.DateTimeKind]::Utc)

    function New-TestPaths {
        param([string]$Name = ('sandbox-' + [guid]::NewGuid().ToString('n').Substring(0, 8)))
        $root = Join-Path $TestDrive $Name
        Get-AutopilotPaths -StateRoot (Join-Path $root 'state') -ProjectRoot $script:ProjectRoot `
            -LogsRoot (Join-Path $root 'logs\autopilot') -RunId 'run-test-0001' `
            -OwnershipPath (Join-Path $root 'work\ownership.json') `
            -EnrollmentRequestPath (Join-Path $root 'work\oracle-enrollment-request.json')
    }

    function Get-SamplePlan {
        param($Paths, $CampaignPath = $script:SampleCampaign)
        $doc = Import-AutopilotCampaign -Path $CampaignPath
        $rosterProvider = New-AutopilotRosterProvider -Kind fixture -Path $script:RosterFixture
        $provider = New-AutopilotCapabilityProvider -Kind manifest -ManifestPath $script:ManifestCurrent
        $plan = Get-AutopilotCampaignPlan -Paths $Paths -Campaign $doc -CapabilityProvider $provider -RosterProvider $rosterProvider
        [pscustomobject]@{ Doc = $doc; Plan = $plan }
    }
}

Describe 'Campaign import (fail closed)' {
    It 'accepts the sample campaign' {
        $doc = Import-AutopilotCampaign -Path $script:SampleCampaign
        $doc.campaign | Should -Be 'northstar'
        @($doc.characters).Count | Should -Be 2
    }

    It 'rejects unknown schema versions, malformed JSON, duplicate guids, and invalid goals' {
        $bad1 = Join-Path $TestDrive 'bad-schema.json'
        Write-AutopilotFileAtomic -Path $bad1 -Content '{"schema":"autowow.autopilot.campaign.v9","schema_version":9,"campaign":"x","characters":[]}'
        { Import-AutopilotCampaign -Path $bad1 } | Should -Throw '*fail closed*'

        $bad2 = Join-Path $TestDrive 'malformed.json'
        Write-AutopilotFileAtomic -Path $bad2 -Content '{ nope'
        { Import-AutopilotCampaign -Path $bad2 } | Should -Throw '*unreadable*'

        $bad3 = Join-Path $TestDrive 'dupe.json'
        Write-AutopilotFileAtomic -Path $bad3 -Content (@{
                schema = 'autowow.autopilot.campaign.v1'; schema_version = 1; campaign = 'dupes'
                characters = @(
                    @{ guid = 10; profile = 'quester'; goals = @(@{ kind = 'ReachLevel'; level = 20 }) },
                    @{ guid = 10; profile = 'gatherer'; goals = @(@{ kind = 'Stockpile'; itemId = 2770; count = 5 }) }
                )
            } | ConvertTo-Json -Depth 10)
        { Import-AutopilotCampaign -Path $bad3 } | Should -Throw '*duplicate character guid*'

        $bad4 = Join-Path $TestDrive 'badgoal.json'
        Write-AutopilotFileAtomic -Path $bad4 -Content (@{
                schema = 'autowow.autopilot.campaign.v1'; schema_version = 1; campaign = 'badgoal'
                characters = @(@{ guid = 10; profile = 'quester'; goals = @(@{ kind = 'BecomeKing' }) })
            } | ConvertTo-Json -Depth 10)
        { Import-AutopilotCampaign -Path $bad4 } | Should -Throw '*goal*'
    }
}

Describe 'Campaign plan' {
    It 'shows every generated job with capabilities, prerequisites, and honest blockers' {
        $paths = New-TestPaths
        $sample = Get-SamplePlan -Paths $paths
        $plan = $sample.Plan
        $plan.campaign | Should -Be 'northstar'

        $brandreas = @($plan.characters | Where-Object { $_.guid -eq 10 })[0]
        $brandreas.profileValid | Should -BeTrue
        $questNode = @($brandreas.nodes | Where-Object { $_.jobKind -eq 'QuestLevel' })[0]
        $questNode.state | Should -Be 'proposed'
        [int64]$questNode.spec.targetLevel | Should -Be 20
        $trainNode = @($brandreas.nodes | Where-Object { $_.jobKind -eq 'TrainProfession' })[0]
        # Mining resolved to its static skill id; blocked only on the undeployed capability.
        [int64]$trainNode.spec.professionSkillId | Should -Be 186
        $trainNode.state | Should -Be 'planned-currently-unavailable'
        (@($trainNode.blockers) -join ';') | Should -Match 'planned, currently unavailable: oracle.vendor_buy'

        $kurl = @($plan.characters | Where-Object { $_.guid -eq 6 })[0]
        $gatherNode = @($kurl.nodes | Where-Object { $_.jobKind -eq 'Gather' })[0]
        $gatherNode.state | Should -Be 'planned-currently-unavailable'
        (@($gatherNode.blockers) -join ';') | Should -Match 'oracle.gather_source'
        @($plan.enrollmentNeeded | Where-Object { $_.guid -eq 6 }).Count | Should -BeGreaterThan 0
    }

    It 'uses roster facts for prerequisite findings and keeps unknown unknown' {
        $paths = New-TestPaths
        $dungeonCampaign = Join-Path $TestDrive 'dungeon-campaign.json'
        Write-AutopilotFileAtomic -Path $dungeonCampaign -Content (@{
                schema = 'autowow.autopilot.campaign.v1'; schema_version = 1; campaign = 'dungeon-probe'
                characters = @(
                    @{ guid = 10; profile = 'all-rounder'; goals = @(@{ kind = 'RunDungeon'; name = 'Deadmines'; minLevel = 15 }) },
                    @{ guid = 99; profile = 'all-rounder'; goals = @(@{ kind = 'RunDungeon'; name = 'Deadmines'; minLevel = 15 }) }
                )
            } | ConvertTo-Json -Depth 10)
        $sample = Get-SamplePlan -Paths $paths -CampaignPath $dungeonCampaign
        $brandreas = @($sample.Plan.characters | Where-Object { $_.guid -eq 10 })[0]
        $dungeonNode = @($brandreas.nodes | Where-Object { $_.jobKind -eq 'Dungeon' })[0]
        # Brandreas is observed level 9: the min-level 15 prerequisite is a real blocker.
        (@($dungeonNode.blockers) -join ';') | Should -Match 'min-level 15 not met \(observed level 9\)'
        # Guid 99 has an unobserved level: unverifiable, NOT claimed missing.
        $unknown = @($sample.Plan.characters | Where-Object { $_.guid -eq 99 })[0]
        $unknownDungeon = @($unknown.nodes | Where-Object { $_.jobKind -eq 'Dungeon' })[0]
        (@($unknownDungeon.prerequisiteFindings) -join ';') | Should -Match 'unverifiable \(level not observed\)'
        (@($unknownDungeon.blockers) -join ';') | Should -Not -Match 'min-level 15 not met'
    }

    It 'wires the dungeon chain dependencies in deterministic topological order' {
        $paths = New-TestPaths
        $dungeonCampaign = Join-Path $TestDrive 'dungeon-chain.json'
        Write-AutopilotFileAtomic -Path $dungeonCampaign -Content (@{
                schema = 'autowow.autopilot.campaign.v1'; schema_version = 1; campaign = 'chain'
                characters = @(@{ guid = 2; profile = 'dungeon'; goals = @(@{ kind = 'RunDungeon'; name = 'Utgarde Keep' }) })
            } | ConvertTo-Json -Depth 10)
        $sample = Get-SamplePlan -Paths $paths -CampaignPath $dungeonCampaign
        $nodes = @(@($sample.Plan.characters)[0].nodes)
        $kinds = @($nodes | ForEach-Object { $_.jobKind })
        # VendorRepair -> Restock -> FormParty -> Travel -> Dungeon (dependencies before dependants).
        $kinds.IndexOf('VendorRepair') | Should -BeLessThan $kinds.IndexOf('Restock')
        $kinds.IndexOf('Restock') | Should -BeLessThan $kinds.IndexOf('FormParty')
        $kinds.IndexOf('FormParty') | Should -BeLessThan $kinds.IndexOf('Travel')
        $kinds.IndexOf('Travel') | Should -BeLessThan $kinds.IndexOf('Dungeon')
        $dungeonNode = @($nodes | Where-Object { $_.jobKind -eq 'Dungeon' })[0]
        @($dungeonNode.dependsOn).Count | Should -BeGreaterThan 0
    }
}

Describe 'Campaign apply (dry-run, idempotent, never sends)' {
    It 'creates jobs with profile metadata and dependency prerequisites, proposes enrollment, and is idempotent' {
        $paths = New-TestPaths
        $sample = Get-SamplePlan -Paths $paths
        $apply = Invoke-AutopilotCampaignApply -Paths $paths -Campaign $sample.Doc -Plan $sample.Plan -Now $script:T0
        @($apply.created).Count | Should -Be 3   # QuestLevel + TrainProfession (guid 10) + Gather (guid 6)

        $jobs = @(Get-AutopilotJobs -Paths $paths)
        $jobs.Count | Should -Be 3
        $questJob = @($jobs | Where-Object { $_.kind -eq 'QuestLevel' })[0]
        $questJob.metadata.profile | Should -Be 'quester'
        $questJob.owner.campaign | Should -Be 'northstar'
        # No command receipts of any kind: apply proposes, never sends.
        foreach ($job in $jobs) {
            @(Get-AutopilotReceipts -Paths $paths -JobId $job.jobId | Where-Object { $_.event -eq 'command' }).Count | Should -Be 0
        }
        # Enrollment request written for the oracle-gated jobs.
        Test-Path $paths.EnrollmentRequestPath | Should -BeTrue
        $enrollment = Get-Content $paths.EnrollmentRequestPath -Raw | ConvertFrom-Json
        @($enrollment.requests).Count | Should -BeGreaterThan 0

        # Re-apply: same jobs, no duplicates.
        $again = Invoke-AutopilotCampaignApply -Paths $paths -Campaign $sample.Doc -Plan $sample.Plan -Now $script:T0.AddMinutes(5)
        @(Get-AutopilotJobs -Paths $paths).Count | Should -Be 3
        (@($again.created | ForEach-Object { $_.jobId }) | Sort-Object) | Should -Be (@($apply.created | ForEach-Object { $_.jobId }) | Sort-Object)
    }

    It 'campaign status reports jobs honestly and stop cancels them' {
        $paths = New-TestPaths
        $sample = Get-SamplePlan -Paths $paths
        $null = Invoke-AutopilotCampaignApply -Paths $paths -Campaign $sample.Doc -Plan $sample.Plan -Now $script:T0
        $status = Get-AutopilotCampaignStatus -Paths $paths -Name 'northstar'
        @($status.jobs).Count | Should -Be 3
        $status.goalEvidenceNote | Should -Match 'unknown'

        $stop = Stop-AutopilotCampaign -Paths $paths -Name 'northstar'
        @($stop.cancellationRequested).Count | Should -Be 3
        $null = Invoke-AutopilotScheduledTick -Paths $paths -Now $script:T0.AddMinutes(1)
        foreach ($entry in @($status.jobs)) {
            (Get-AutopilotJob -Paths $paths -JobId $entry.jobId).phase | Should -Be 'Cancelled'
        }
    }

    It 'schedules the quester: QuestLevel advances while TrainProfession waits with its reason' {
        $paths = New-TestPaths
        $sample = Get-SamplePlan -Paths $paths
        $null = Invoke-AutopilotCampaignApply -Paths $paths -Campaign $sample.Doc -Plan $sample.Plan -Now $script:T0
        $provider = New-AutopilotCapabilityProvider -Kind manifest -ManifestPath $script:ManifestCurrent
        1..4 | ForEach-Object { $null = Invoke-AutopilotScheduledTick -Paths $paths -FixtureRoot $script:FixturesDefault -CapabilityProvider $provider -Now $script:T0.AddMinutes($_) }
        $jobs = @(Get-AutopilotJobs -Paths $paths)
        $questJob = @($jobs | Where-Object { $_.kind -eq 'QuestLevel' })[0]
        $trainJob = @($jobs | Where-Object { $_.kind -eq 'TrainProfession' })[0]
        $gatherJob = @($jobs | Where-Object { $_.kind -eq 'Gather' })[0]
        $questJob.phase | Should -BeIn @('Preparing', 'Executing', 'Verifying')
        # The same character's TrainProfession job is HELD (one mutating job per
        # character) with a receipted reason - kept, not transformed.
        $trainJob.phase | Should -Be 'Queued'
        $heldReceipts = @(Get-AutopilotReceipts -Paths $paths -JobId $trainJob.jobId | Where-Object { $_.event -eq 'job_held' })
        $heldReceipts.Count | Should -BeGreaterThan 0
        $heldReceipts[0].reason | Should -Match 'character busy'
        # The gatherer (different character) reached its own honest capability block.
        $gatherJob.phase | Should -Be 'Blocked'
        $gatherJob.blockedReason.code | Should -Be 'oracle-capability-not-deployed'
        (Get-AutopilotExplanation -Paths $paths -JobId $gatherJob.jobId).waitState | Should -Be 'waiting-for-server-capability'
    }
}
