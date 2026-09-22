# AutoWoW Autopilot V1.2 - progress summary module tests.
# Run: Invoke-Pester -Path .\autopilot\tests\summary.tests.ps1 -Output Detailed
# Fully offline: TestDrive state, fixture responses, no bridge socket, no MySQL,
# no writes outside TestDrive (ownership/enrollment paths are redirected).

BeforeAll {
    $script:AutopilotRoot = Split-Path -Parent $PSScriptRoot
    $script:ProjectRoot = Split-Path -Parent $script:AutopilotRoot
    . (Join-Path $script:AutopilotRoot 'AutopilotLib.ps1')
    . (Join-Path $script:AutopilotRoot 'modules\AutopilotSummary.ps1')

    $script:FixturesDefault = Join-Path $script:AutopilotRoot 'fixtures\default'
    $script:FixturesStep3 = Join-Path $script:AutopilotRoot 'fixtures\scenario-a-step3'
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
        param([datetime]$Now = $script:T0, $Paths, [string]$Campaign = 'test-league')
        $jobParams = @{
            Kind            = 'QuestLevel'
            Spec            = @{ partyLeaderGuid = 10 }
            EligibleGuids   = @([uint32]10)
            SuccessCriteria = @{ measure = 'quest-turnins'; counters = @{ questsTurnedIn = 1 } }
            Campaign        = $Campaign
            Team            = 'northstar'
            Now             = $Now
        }
        if ($Paths) { $jobParams['Paths'] = $Paths }
        New-AutopilotJob @jobParams
    }

    function New-TestGatherJob {
        param([datetime]$Now = $script:T0, $Paths, [string]$Campaign = 'test-league')
        $jobParams = @{
            Kind            = 'Gather'
            Spec            = @{ item = @{ itemId = 2770; count = 100 }; stopAtCount = 100 }
            EligibleGuids   = @([uint32]6)
            SuccessCriteria = @{ measure = 'inventory-item-count'; counters = @{ itemCount = 100 } }
            Campaign        = $Campaign
            Now             = $Now
        }
        if ($Paths) { $jobParams['Paths'] = $Paths }
        New-AutopilotJob @jobParams
    }

    function Invoke-QuestScenarioToCompletion {
        # Ticks 1-4: Queued to Verifying (quest order recorded would-send).
        # Tick 5: baseline objective 0/8, back to Executing.
        # Tick 6: Executing again - the identical quest order is deduplicated.
        # Tick 7: step3 fixture observes 8/8 rewarded and the job Completes.
        param([Parameter(Mandatory)]$Paths)
        1..5 | ForEach-Object { $null = Invoke-AutopilotPlannerTick -Paths $Paths -FixtureRoot $script:FixturesDefault }
        1..2 | ForEach-Object { $null = Invoke-AutopilotPlannerTick -Paths $Paths -FixtureRoot $script:FixturesStep3 }
    }
}

Describe 'Get-AutopilotHistory' {
    It 'returns a time-sorted history for guid 10 with transition, would-send, and objective lines' {
        $paths = New-TestPaths
        $job = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob)
        Invoke-QuestScenarioToCompletion -Paths $paths
        (Get-AutopilotJob -Paths $paths -JobId $job.jobId).phase | Should -Be 'Completed'

        $history = Get-AutopilotHistory -Paths $paths -Guid 10
        @($history).Count | Should -BeGreaterThan 10
        foreach ($entry in @($history)) {
            $entry.jobId | Should -Be $job.jobId
            $entry.timestamp_utc | Should -Not -BeNullOrEmpty
            $entry.event | Should -Not -BeNullOrEmpty
            $entry.detail | Should -Not -BeNullOrEmpty
        }
        # Ascending by ConvertTo-AutopilotUtcDateTime.
        $times = @(@($history) | ForEach-Object { ConvertTo-AutopilotUtcDateTime $_.timestamp_utc })
        ($times | Sort-Object) | Should -Be $times

        $details = @(@($history) | ForEach-Object { [string]$_.detail })
        @($details | Where-Object { $_ -eq 'Queued -> Validating (planner tick)' }).Count | Should -Be 1
        @($details | Where-Object { $_ -eq 'would-send: quest 10' }).Count | Should -Be 1
        @($details | Where-Object { $_ -match '^deduplicated: quest 10' }).Count | Should -BeGreaterOrEqual 1
        @($details | Where-Object { $_ -eq 'objective 0/8 quest 792' }).Count | Should -Be 1
        @($details | Where-Object { $_ -eq 'objective 8/8 quest 792 (rewarded)' }).Count | Should -Be 1
    }

    It 'applies the Since filter to drop earlier entries' {
        $paths = New-TestPaths
        $job = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob)
        1..4 | ForEach-Object { $null = Invoke-AutopilotPlannerTick -Paths $paths -FixtureRoot $script:FixturesDefault }
        Start-Sleep -Milliseconds 50
        $cut = (Get-Date).ToUniversalTime()
        Start-Sleep -Milliseconds 50
        $null = Invoke-AutopilotPlannerTick -Paths $paths -FixtureRoot $script:FixturesDefault
        1..2 | ForEach-Object { $null = Invoke-AutopilotPlannerTick -Paths $paths -FixtureRoot $script:FixturesStep3 }

        $full = Get-AutopilotHistory -Paths $paths -Guid 10
        $recent = Get-AutopilotHistory -Paths $paths -Guid 10 -Since $cut
        @($recent).Count | Should -BeGreaterThan 0
        @($recent).Count | Should -BeLessThan @($full).Count
        @(@($recent) | Where-Object { $_.event -eq 'job_created' }).Count | Should -Be 0
        foreach ($entry in @($recent)) {
            (ConvertTo-AutopilotUtcDateTime $entry.timestamp_utc) -ge $cut | Should -BeTrue
        }
    }

    It 'matches a job by eligible guid before any character is assigned' {
        $paths = New-TestPaths
        $job = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob)
        $history = Get-AutopilotHistory -Paths $paths -Guid 10
        @($history).Count | Should -Be 1
        @($history)[0].event | Should -Be 'job_created'
        @($history)[0].detail | Should -Match 'created: QuestLevel job \(dry-run\)'
    }

    It 'returns an empty array for a guid that appears in no job' {
        $paths = New-TestPaths
        $null = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob)
        $history = Get-AutopilotHistory -Paths $paths -Guid 999
        @($history).Count | Should -Be 0
    }

    It 'surfaces an unreadable ledger as receipts_unavailable instead of dropping or guessing' {
        $paths = New-TestPaths
        $job = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob)
        $receiptFile = Get-AutopilotReceiptPath -Paths $paths -JobId $job.jobId
        [System.IO.File]::AppendAllText($receiptFile, '{ this is not json' + [Environment]::NewLine)
        $history = Get-AutopilotHistory -Paths $paths -Guid 10
        @($history).Count | Should -Be 1
        @($history)[0].event | Should -Be 'receipts_unavailable'
        @($history)[0].detail | Should -Match 'fail closed'
    }
}

Describe 'Get-AutopilotSummaryData - receipt-backed counts' {
    It 'counts questTurnIns from exactly the rewarded observation' {
        $paths = New-TestPaths
        $job = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob)
        Invoke-QuestScenarioToCompletion -Paths $paths
        $data = Get-AutopilotSummaryData -Paths $paths
        $data.questTurnIns.value | Should -Be 1
        $data.questTurnIns.evidence | Should -Be 'receipts'
        $data.questObjectiveProgressEvents.value | Should -Be 2
        $data.questObjectiveProgressEvents.evidence | Should -Be 'receipts'
    }

    It 'reports commands with sent 0, wouldSend positive, deduplicated at least 1' {
        $paths = New-TestPaths
        $null = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob)
        Invoke-QuestScenarioToCompletion -Paths $paths
        $data = Get-AutopilotSummaryData -Paths $paths
        $data.commands.evidence | Should -Be 'receipts'
        $data.commands.value.sent | Should -Be 0
        $data.commands.value.wouldSend | Should -BeGreaterThan 0
        $data.commands.value.deduplicated | Should -BeGreaterOrEqual 1
        $data.commands.value.total | Should -Be $data.commands.value.wouldSend
    }

    It 'counts completedObjectives only for Completed jobs with counters met and lists the jobIds' {
        $paths = New-TestPaths
        $quest = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob)
        $null = Add-AutopilotJob -Paths $paths -Job (New-TestGatherJob)   # will block, never complete
        Invoke-QuestScenarioToCompletion -Paths $paths
        $data = Get-AutopilotSummaryData -Paths $paths
        $data.completedJobs.value | Should -Be 1
        $data.completedObjectives.value | Should -Be 1
        $data.completedObjectives.note | Should -Match ([regex]::Escape($quest.jobId))
        $data.jobsByPhase.value['Completed'] | Should -Be 1
        $data.jobsByPhase.value['Blocked'] | Should -Be 1
    }

    It 'accrues blockedDurationsByCode to the -Now anchor for a still-blocked Gather job' {
        $paths = New-TestPaths
        $null = Add-AutopilotJob -Paths $paths -Job (New-TestGatherJob)
        1..2 | ForEach-Object { $null = Invoke-AutopilotPlannerTick -Paths $paths }
        $anchor = (Get-Date).ToUniversalTime().AddHours(1)
        $data = Get-AutopilotSummaryData -Paths $paths -Now $anchor
        $data.blockedDurationsByCode.evidence | Should -Be 'receipts'
        $data.blockedDurationsByCode.value.Contains('oracle-capability-not-deployed') | Should -BeTrue
        $seconds = [double]$data.blockedDurationsByCode.value['oracle-capability-not-deployed']
        $seconds | Should -BeGreaterThan 3500
        $seconds | Should -BeLessThan 3700
    }

    It 'counts an injected ownership_conflict receipt' {
        $paths = New-TestPaths
        $job = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob)
        $null = Write-AutopilotReceipt -Paths $paths -JobId $job.jobId -Event 'ownership_conflict' -Data @{
            guid           = 10
            owner_instance = 'ap-foreign-ctrl'
            owner_pid      = 99999
            owner_job      = 'job-20260718T000000Z-ffffffff'
            owner_expires  = '2026-07-18T13:00:00.0000000Z'
        }
        $data = Get-AutopilotSummaryData -Paths $paths
        $data.ownershipConflicts.value | Should -Be 1
        $data.ownershipConflicts.evidence | Should -Be 'receipts'
        $history = Get-AutopilotHistory -Paths $paths -Guid 10
        @(@($history) | Where-Object { $_.detail -match 'ownership conflict: guid 10 owned by ap-foreign-ctrl' }).Count | Should -Be 1
    }

    It 'counts controller starts and reconciliations from the controller ledger' {
        $paths = New-TestPaths
        $null = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob)
        Enter-AutopilotLock -Paths $paths
        try { $null = Invoke-AutopilotReconcile -Paths $paths }
        finally { Exit-AutopilotLock -Paths $paths }
        $data = Get-AutopilotSummaryData -Paths $paths
        $data.controller.evidence | Should -Be 'receipts'
        $data.controller.value.starts | Should -Be 1
        $data.controller.value.staleLockRecoveries | Should -Be 0
        $data.controller.value.reconciliations | Should -Be 1
    }
}

Describe 'Get-AutopilotSummaryData - honesty invariants' {
    It 'keeps metrics without an evidence stream null with evidence none, never zero' {
        $paths = New-TestPaths
        $null = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob)
        $data = Get-AutopilotSummaryData -Paths $paths
        foreach ($key in @('materialsGathered', 'itemsCrafted', 'bossAttempts', 'bossKills', 'deaths', 'recoveries', 'pvpMatches', 'levelsGained')) {
            $data[$key].value | Should -BeNullOrEmpty
            $data[$key].evidence | Should -Be 'none'
            $data[$key].note | Should -Match 'no evidence stream'
        }
    }

    It 'fails closed on a malformed receipt ledger with a typed reason' {
        $paths = New-TestPaths
        $job = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob)
        1..4 | ForEach-Object { $null = Invoke-AutopilotPlannerTick -Paths $paths -FixtureRoot $script:FixturesDefault }
        $receiptFile = Get-AutopilotReceiptPath -Paths $paths -JobId $job.jobId
        [System.IO.File]::AppendAllText($receiptFile, '{ this is not json' + [Environment]::NewLine)
        $data = Get-AutopilotSummaryData -Paths $paths
        @($data.invalidLedgers).Count | Should -Be 1
        $data.invalidLedgers[0].jobId | Should -Be $job.jobId
        $data.invalidLedgers[0].reason | Should -Match 'fail closed'
        # The only considered ledger is invalid: counts are unknown, not zero.
        $data.commands.evidence | Should -Be 'none'
        $data.commands.value | Should -BeNullOrEmpty
        $data.questTurnIns.evidence | Should -Be 'none'
        $data.questTurnIns.value | Should -BeNullOrEmpty
    }

    It 'rejects receipts with an unknown schema version instead of counting them' {
        $paths = New-TestPaths
        $job = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob)
        Invoke-QuestScenarioToCompletion -Paths $paths
        $receiptFile = Get-AutopilotReceiptPath -Paths $paths -JobId $job.jobId
        $forged = '{"schema":"autowow.autopilot.receipt.v99","schema_version":99,"seq":999,"receipt_id":"rcpt-forged","timestamp_utc":"2026-07-18T12:00:00.0000000Z","job_id":"' + $job.jobId + '","event":"progress_observed","rewarded":true}'
        [System.IO.File]::AppendAllText($receiptFile, $forged + [Environment]::NewLine)
        $data = Get-AutopilotSummaryData -Paths $paths
        $data.questTurnIns.value | Should -Be 1
        $data.questTurnIns.note | Should -Match 'unknown schema'
        $history = Get-AutopilotHistory -Paths $paths -Guid 10
        @(@($history) | Where-Object { $_.event -eq 'progress_observed' }).Count | Should -Be 2
    }

    It 'excludes jobs from other campaigns with the Campaign filter' {
        $paths = New-TestPaths
        $quest = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob -Campaign 'test-league')
        $null = Add-AutopilotJob -Paths $paths -Job (New-TestGatherJob -Campaign 'other-league')
        Invoke-QuestScenarioToCompletion -Paths $paths

        $all = Get-AutopilotSummaryData -Paths $paths
        @($all.jobsConsidered).Count | Should -Be 2
        $all.jobsByPhase.value['Completed'] | Should -Be 1
        $all.jobsByPhase.value['Blocked'] | Should -Be 1

        $league = Get-AutopilotSummaryData -Paths $paths -Campaign 'test-league'
        @($league.jobsConsidered) | Should -Be @($quest.jobId)
        $league.jobsByPhase.value.Contains('Blocked') | Should -BeFalse
        $league.completedJobs.value | Should -Be 1
        $league.questTurnIns.value | Should -Be 1

        $other = Get-AutopilotSummaryData -Paths $paths -Campaign 'other-league'
        $other.completedJobs.value | Should -Be 0
        $other.questTurnIns.value | Should -Be 0
        $other.jobsByPhase.value.Contains('Completed') | Should -BeFalse
    }
}

Describe 'Format-AutopilotSummary' {
    It 'renders unknown metrics as unknown (no evidence stream), never 0' {
        $paths = New-TestPaths
        $null = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob)
        Invoke-QuestScenarioToCompletion -Paths $paths
        $text = Format-AutopilotSummary -Data (Get-AutopilotSummaryData -Paths $paths)
        $text | Should -Match 'itemsCrafted: unknown \(no evidence stream\)'
        $text | Should -Match 'deaths: unknown \(no evidence stream\)'
        $text | Should -Match 'levelsGained: unknown \(no evidence stream\)'
        $text | Should -Not -Match 'itemsCrafted: 0'
        $text | Should -Not -Match 'deaths: 0'
        $text | Should -Match 'questTurnIns: 1'
        $text | Should -Match 'sent=0'
    }

    It 'emits parseable JSON with -AsJson that carries the schema and metric values' {
        $paths = New-TestPaths
        $null = Add-AutopilotJob -Paths $paths -Job (New-TestQuestJob)
        Invoke-QuestScenarioToCompletion -Paths $paths
        $json = Format-AutopilotSummary -Data (Get-AutopilotSummaryData -Paths $paths) -AsJson
        $parsed = $json | ConvertFrom-Json
        $parsed.schema | Should -Be 'autowow.autopilot.summary.v1'
        $parsed.schema_version | Should -Be 1
        $parsed.questTurnIns.value | Should -Be 1
        $parsed.questTurnIns.evidence | Should -Be 'receipts'
        $parsed.itemsCrafted.value | Should -BeNullOrEmpty
        $parsed.itemsCrafted.evidence | Should -Be 'none'
        $parsed.commands.value.sent | Should -Be 0
    }
}
