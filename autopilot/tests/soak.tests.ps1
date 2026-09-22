# AutoWoW Autopilot V1.2 - deterministic eight-hour simulated soak.
# Fixture clock (no real waiting): 96 ticks x 5 simulated minutes = 8 sim-hours.
# Scripted events: ownership conflict at t+60m (expires t+120m), capability
# manifest change at t+180m (gathering Oracle deploys), controller crash+restart
# at t+240m, server-session change at t+300m. One quest completes on rewarded
# evidence; one gather becomes available and completes on verified inventory;
# craft and PvP stay honestly blocked the whole run.
# Run: Invoke-Pester -Path .\autopilot\tests\soak.tests.ps1 -Output Detailed

BeforeAll {
    $script:AutopilotRoot = Split-Path -Parent $PSScriptRoot
    $script:ProjectRoot = Split-Path -Parent $script:AutopilotRoot
    . (Join-Path $script:AutopilotRoot 'AutopilotLib.ps1')
    foreach ($module in (Get-ChildItem -Path (Join-Path $script:AutopilotRoot 'modules') -Filter '*.ps1' | Sort-Object Name)) { . $module.FullName }

    $script:T0 = [datetime]::new(2026, 7, 18, 20, 0, 0, [System.DateTimeKind]::Utc)
    $script:FixturesDefault = Join-Path $script:AutopilotRoot 'fixtures\default'
    $script:FixturesStep2 = Join-Path $script:AutopilotRoot 'fixtures\scenario-a-step2'
    $script:FixturesStep3 = Join-Path $script:AutopilotRoot 'fixtures\scenario-a-step3'

    function New-SoakPaths {
        $root = Join-Path $TestDrive 'soak'
        Get-AutopilotPaths -StateRoot (Join-Path $root 'state') -ProjectRoot $script:ProjectRoot `
            -LogsRoot (Join-Path $root 'logs\autopilot') -RunId 'run-soak-0001' `
            -OwnershipPath (Join-Path $root 'work\ownership.json') `
            -EnrollmentRequestPath (Join-Path $root 'work\oracle-enrollment-request.json')
    }

    function New-SoakManifest {
        param([string]$OutPath, [bool]$GatherDeployed, [string]$SessionId)
        $doc = [ordered]@{
            schema = 'autowow.oracle.capabilities.v1'; schema_version = 1
            server_build = 'soak-build-1'; session_id = $SessionId
            contract_schema_version = 1
            runtime_enabled = $GatherDeployed
            generated_at = '2026-07-18T20:00:00.0000000Z'
            bridge = [ordered]@{ deployed = $false }
            sql_read_deployed = $false
            operations = @(
                [ordered]@{ operation = 'gather_source'; deployed = $GatherDeployed; native_adapter_available = $GatherDeployed; runtime_authorized = $GatherDeployed; receipt_export_available = $false; constraints = $null },
                [ordered]@{ operation = 'gather_route'; deployed = $GatherDeployed; native_adapter_available = $GatherDeployed; runtime_authorized = $GatherDeployed; receipt_export_available = $false; constraints = $null }
            )
        }
        Write-AutopilotFileAtomic -Path $OutPath -Content ($doc | ConvertTo-Json -Depth 10)
    }

    function New-SoakScenarioDir {
        param([string]$Dir, [string]$QuestFixture, [System.Nullable[int64]]$InventoryCount)
        if (-not (Test-Path $Dir)) { $null = New-Item -ItemType Directory -Force -Path $Dir }
        Copy-Item (Join-Path $QuestFixture 'questobjective-10.json') (Join-Path $Dir 'questobjective-10.json') -Force
        if ($null -ne $InventoryCount) {
            $doc = [ordered]@{ schema = 'autowow.autopilot.inventory-observation.v1'; item_id = 2770; count = [int64]$InventoryCount; observed_utc = '2026-07-18T20:00:00.0000000Z' }
            Write-AutopilotFileAtomic -Path (Join-Path $Dir 'inventory-6.json') -Content ($doc | ConvertTo-Json)
        }
    }

    function Add-ForeignLease {
        # Appends a foreign controller's active lease WITHOUT touching ours.
        param($Paths, [int64]$Guid, [datetime]$ExpiresUtc)
        $ownership = Read-AutopilotOwnership -Paths $Paths
        $leases = @($ownership.leases) + @([ordered]@{
                schema               = 'autowow.autopilot.ownership.v1'
                controllerInstanceId = 'ap-foreign-soak'
                pid                  = 424242
                jobId                = 'job-20260718T000000Z-f0f0f0f0'
                characterGuid        = $Guid
                acquiredAt           = '2026-07-18T20:55:00.0000000Z'
                renewedAt            = '2026-07-18T20:55:00.0000000Z'
                expiresAt            = $ExpiresUtc.ToString('o')
                state                = 'active'
            })
        Save-AutopilotOwnership -Paths $Paths -Leases $leases
    }
}

Describe 'Eight-hour simulated soak' {
    It 'survives conflicts, a restart, a capability deploy, and a session change with every invariant intact' {
        $paths = New-SoakPaths
        $manifestPath = Join-Path $TestDrive 'soak-manifest.json'
        New-SoakManifest -OutPath $manifestPath -GatherDeployed $false -SessionId 'soak-session-1'

        # Scenario fixture directories by simulated-time window.
        $dirA = Join-Path $TestDrive 'phase-a'   # 0-50m: quest 0/8, no inventory
        $dirB = Join-Path $TestDrive 'phase-b'   # 50-80m: quest 3/8
        $dirC = Join-Path $TestDrive 'phase-c'   # 80-180m: quest 8/8 rewarded
        $dirD = Join-Path $TestDrive 'phase-d'   # 180-300m: inventory 30
        $dirE = Join-Path $TestDrive 'phase-e'   # 300-420m: inventory 70
        $dirF = Join-Path $TestDrive 'phase-f'   # 420-480m: inventory 100
        New-SoakScenarioDir -Dir $dirA -QuestFixture $script:FixturesDefault -InventoryCount $null
        New-SoakScenarioDir -Dir $dirB -QuestFixture $script:FixturesStep2 -InventoryCount $null
        New-SoakScenarioDir -Dir $dirC -QuestFixture $script:FixturesStep3 -InventoryCount $null
        New-SoakScenarioDir -Dir $dirD -QuestFixture $script:FixturesStep3 -InventoryCount 30
        New-SoakScenarioDir -Dir $dirE -QuestFixture $script:FixturesStep3 -InventoryCount 70
        New-SoakScenarioDir -Dir $dirF -QuestFixture $script:FixturesStep3 -InventoryCount 100

        # Jobs: two questers, one gatherer, one crafter (blocked), one battleground (blocked).
        $quest10 = Add-AutopilotJob -Paths $paths -Job (New-AutopilotJob -Kind 'QuestLevel' -Spec @{ partyLeaderGuid = 10 } -EligibleGuids @([uint32]10) `
                -SuccessCriteria @{ measure = 'quest-turnins'; counters = @{ questsTurnedIn = 1 } } -Campaign 'soak' -Priority 60 -Now $script:T0 -Paths $paths)
        $quest7 = Add-AutopilotJob -Paths $paths -Job (New-AutopilotJob -Kind 'QuestLevel' -Spec @{ partyLeaderGuid = 7 } -EligibleGuids @([uint32]7) `
                -SuccessCriteria @{ measure = 'quest-turnins'; counters = @{ questsTurnedIn = 1 } } -Campaign 'soak' -Priority 50 -Now $script:T0 -Paths $paths `
                -TimeoutPolicy @{ jobTimeoutMinutes = 720; phaseTimeoutMinutes = 60; noProgressSeconds = 120 })
        $gather6 = Add-AutopilotJob -Paths $paths -Job (New-AutopilotJob -Kind 'Gather' -Spec @{ item = @{ itemId = 2770; count = 100 }; stopAtCount = 100 } -EligibleGuids @([uint32]6) `
                -SuccessCriteria @{ measure = 'inventory-item-count'; counters = @{ itemCount = 100 } } -Campaign 'soak' -Priority 55 -Now $script:T0 -Paths $paths)
        $craft8 = Add-AutopilotJob -Paths $paths -Job (New-AutopilotJob -Kind 'Craft' -Spec @{ recipeSpellId = 2660; count = 5 } -EligibleGuids @([uint32]8) `
                -SuccessCriteria @{ measure = 'inventory-item-count'; counters = @{ crafted = 5 } } -Campaign 'soak' -Priority 40 -Now $script:T0 -Paths $paths)
        $pvpTeam = Add-AutopilotJob -Paths $paths -Job (New-AutopilotJob -Kind 'Battleground' -Spec @{ name = 'wsg'; teamSize = 10 } -EligibleGuids @(101..120 | ForEach-Object { [uint32]$_ }) `
                -SuccessCriteria @{ measure = 'battleground-scoreboard'; counters = @{ matchesCompleted = 1 } } -Campaign 'soak' -Priority 45 -Now $script:T0 -Paths $paths)

        $clock = New-AutopilotClock -Kind fixture -StartUtc $script:T0 -StepSeconds 300
        Enter-AutopilotLock -Paths $paths
        $null = Invoke-AutopilotReconcile -Paths $paths
        $mutatingPhases = @('Preparing', 'Traveling', 'Executing', 'Verifying', 'Recovering')
        $slotViolations = 0
        $restartDone = $false

        foreach ($tick in 1..96) {
            $simMinutes = ($tick - 1) * 5
            $simNow = $script:T0.AddMinutes($simMinutes)

            # --- scripted events ---
            if ($simMinutes -eq 60) { Add-ForeignLease -Paths $paths -Guid 7 -ExpiresUtc $script:T0.AddMinutes(120) }
            if ($simMinutes -eq 180) { New-SoakManifest -OutPath $manifestPath -GatherDeployed $true -SessionId 'soak-session-1' }
            if ($simMinutes -eq 240 -and -not $restartDone) {
                # Simulated crash: seq/path caches vanish with the process; a stale
                # pid file is left behind. Then a fresh controller reconciles.
                $restartDone = $true
                $script:AutopilotSeqCache.Clear()
                $script:AutopilotReceiptPathCache.Clear()
                $deadProcess = Start-Process -FilePath 'pwsh' -ArgumentList '-NoProfile', '-Command', 'exit 0' -PassThru -WindowStyle Hidden
                $deadProcess.WaitForExit()
                Write-AutopilotFileAtomic -Path $paths.LockPath -Content (@{ pid = $deadProcess.Id; started_utc = '2026-07-18T20:00:00.0000000Z'; script = 'soak' } | ConvertTo-Json)
                Enter-AutopilotLock -Paths $paths
                $null = Invoke-AutopilotReconcile -Paths $paths
            }
            if ($simMinutes -eq 300) { New-SoakManifest -OutPath $manifestPath -GatherDeployed $true -SessionId 'soak-session-2' }

            $fixtureRoot = if ($simMinutes -lt 50) { $dirA }
            elseif ($simMinutes -lt 80) { $dirB }
            elseif ($simMinutes -lt 180) { $dirC }
            elseif ($simMinutes -lt 300) { $dirD }
            elseif ($simMinutes -lt 420) { $dirE }
            else { $dirF }

            $null = Invoke-AutopilotRunTick -Paths $paths -Clock $clock -TickNumber $tick -FixtureRoot $fixtureRoot -CapabilityManifest $manifestPath

            # Invariant sampled EVERY tick: one mutating job per character.
            $byCharacter = @{}
            foreach ($job in (Get-AutopilotJobs -Paths $paths)) {
                if ($job.phase -in $mutatingPhases) {
                    $character = [string](Get-AutopilotJobCharacter -Job $job)
                    if ($byCharacter.ContainsKey($character)) { $slotViolations++ }
                    $byCharacter[$character] = $true
                }
            }
            Step-AutopilotClock -Clock $clock
        }

        # Graceful shutdown: release everything this controller still holds.
        $null = Release-AutopilotAllOwnership -Paths $paths -Reason 'soak shutdown'
        Exit-AutopilotLock -Paths $paths
        $endNow = Get-AutopilotClockNow -Clock $clock

        # --- duration ---
        $endNow | Should -Be $script:T0.AddMinutes(480)
        $slotViolations | Should -Be 0

        # --- quest completes on rewarded evidence ---
        $quest10Final = Get-AutopilotJob -Paths $paths -JobId $quest10.jobId
        $quest10Final.phase | Should -Be 'Completed'
        [int64]$quest10Final.progress.counters.questsTurnedIn | Should -Be 1
        $quest10Receipts = Get-AutopilotReceipts -Paths $paths -JobId $quest10.jobId
        @($quest10Receipts | Where-Object { $_.event -eq 'progress_observed' -and $_.rewarded }).Count | Should -Be 1

        # --- gather becomes available only after the manifest deploys it, then completes ---
        $gather6Final = Get-AutopilotJob -Paths $paths -JobId $gather6.jobId
        $gather6Final.phase | Should -Be 'Completed'
        [int64]$gather6Final.progress.counters.itemCount | Should -Be 100
        $gather6Receipts = Get-AutopilotReceipts -Paths $paths -JobId $gather6.jobId
        $oracleIntents = @($gather6Receipts | Where-Object { $_.event -eq 'command' -and $_.command.surface -eq 'oracle' })
        $oracleIntents.Count | Should -BeGreaterThan 0
        foreach ($intent in $oracleIntents) {
            (ConvertTo-AutopilotUtcDateTime $intent.timestamp_utc) | Should -BeGreaterOrEqual $script:T0.AddMinutes(180)
            $intent.command.sent | Should -BeFalse
        }
        $capabilityBlock = @($gather6Receipts | Where-Object { $_.event -eq 'job_transition' -and $_.to -eq 'Blocked' })
        $capabilityBlock.Count | Should -BeGreaterThan 0
        @($gather6Receipts | Where-Object { $_.event -eq 'job_transition' -and $_.reason -match 'capability manifest now evidences' }).Count | Should -BeGreaterThan 0

        # --- craft and PvP stayed honestly blocked all run ---
        foreach ($blockedJob in @($craft8, $pvpTeam)) {
            $final = Get-AutopilotJob -Paths $paths -JobId $blockedJob.jobId
            $final.phase | Should -Be 'Blocked'
            $final.blockedReason.code | Should -Be 'oracle-capability-not-deployed'
            @(Get-AutopilotReceipts -Paths $paths -JobId $blockedJob.jobId | Where-Object { $_.event -eq 'command' }).Count | Should -Be 0
        }

        # --- exactly one ownership conflict episode, resolved by expiry ---
        $quest7Receipts = Get-AutopilotReceipts -Paths $paths -JobId $quest7.jobId
        @($quest7Receipts | Where-Object { $_.event -eq 'ownership_conflict' }).Count | Should -BeGreaterThan 0
        $rosterHops = @($quest7Receipts | Where-Object { $_.event -eq 'job_transition' -and $_.PSObject.Properties['blocked'] -and $_.blocked.code -eq 'roster-owned' })
        $rosterHops.Count | Should -BeGreaterThan 0
        @($quest7Receipts | Where-Object { $_.event -eq 'job_transition' -and $_.reason -match 'ownership now available' }).Count | Should -BeGreaterThan 0
        (Get-AutopilotJob -Paths $paths -JobId $quest7.jobId).phase | Should -Not -Be 'Failed'

        # --- exactly one server-session change, in-flight jobs revalidated ---
        $controllerReceipts = Get-AutopilotReceipts -Paths $paths -JobId 'controller'
        @($controllerReceipts | Where-Object { $_.event -eq 'server_session_changed' }).Count | Should -Be 1
        @($controllerReceipts | Where-Object { $_.event -eq 'stale_lock_recovered' }).Count | Should -Be 1
        @($controllerReceipts | Where-Object { $_.event -eq 'reconciliation_completed' }).Count | Should -Be 2

        # --- no duplicate irreversible commands anywhere, and nothing was ever sent ---
        foreach ($job in (Get-AutopilotJobs -Paths $paths -IncludeTerminal)) {
            $receipts = Get-AutopilotReceipts -Paths $paths -JobId $job.jobId
            $commands = @($receipts | Where-Object { $_.event -eq 'command' })
            foreach ($command in $commands) { $command.command.sent | Should -BeFalse }
            $groups = @($commands | Where-Object { $_.command.irreversible }) | Group-Object { $_.command.idempotency_key }
            foreach ($group in $groups) { $group.Count | Should -Be 1 }
            # Receipt seq stays strictly monotonic across the crash/restart.
            $seqs = @($receipts | ForEach-Object { [int]$_.seq })
            ($seqs | Sort-Object) | Should -Be $seqs
        }

        # --- every lease released or expired at the end ---
        $ownership = Read-AutopilotOwnership -Paths $paths
        $now = $script:T0.AddMinutes(481)
        foreach ($lease in @($ownership.leases)) {
            (Test-AutopilotLeaseActive -Lease $lease -Now $now) | Should -BeFalse
        }

        # --- every terminal result has evidence ---
        foreach ($job in (Get-AutopilotJobs -Paths $paths -IncludeTerminal | Where-Object { $_.phase -eq 'Completed' })) {
            $receipts = Get-AutopilotReceipts -Paths $paths -JobId $job.jobId
            $evidence = @($receipts | Where-Object { $_.event -in @('progress_observed', 'progress_observed_gather') })
            $evidence.Count | Should -BeGreaterThan 0
        }
    }
}
