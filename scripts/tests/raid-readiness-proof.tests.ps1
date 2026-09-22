Set-StrictMode -Version Latest

BeforeAll {
    $script:ScriptsRoot = Split-Path -Parent $PSScriptRoot
    $script:ProofPath = Join-Path $script:ScriptsRoot 'raid-readiness-proof.ps1'
    $script:LibraryPath = Join-Path $script:ScriptsRoot 'raid-readiness-proof-lib.ps1'
    . $script:LibraryPath

    function New-TestContract {
        param([ValidateSet(10, 25, 40)][int]$Size = 10)

        $roster = [uint32[]](1..$Size)
        $tankCount = if ($Size -eq 40) { 4 } else { 2 }
        $healerCount = if ($Size -eq 10) { 3 } elseif ($Size -eq 25) { 5 } else { 8 }
        $tanks = [uint32[]](1..$tankCount)
        $healers = [uint32[]](($tankCount + 1)..($tankCount + $healerCount))
        return New-RaidReadinessContract -RaidSize $Size -RosterGuid $roster -TankGuid $tanks -HealerGuid $healers `
            -LeaderGuid 1 -TargetGuid 900 -TargetName 'Synthetic Boss' -ExpectedMapId 533 -ExpectedInstanceId 42
    }

    function New-TestSample {
        param(
            [Parameter(Mandatory)][object]$Contract,
            [Parameter(Mandatory)][datetime]$At,
            [long]$TargetHealth = 1000,
            [uint32]$TargetOwnerGuid = 1,
            [uint32]$HealerTargetGuid = 1,
            [uint32]$LowHealthGuid = 0,
            [long]$InterruptCount = 2,
            [uint32[]]$DeadGuid = @(),
            [double]$PositionOffset = 0.0
        )

        $members = foreach ($expected in $Contract.members) {
            $guid = [uint32]$expected.guid
            $health = if ($guid -eq $LowHealthGuid) { 40.0 } else { 90.0 }
            [pscustomobject][ordered]@{
                guid = $guid
                name = "Bot$guid"
                expected_role = [string]$expected.role
                observed_role = [string]$expected.role
                online_match_count = 1
                combat_telemetry_present = $true
                alive = $DeadGuid -notcontains $guid
                in_combat = $true
                map_id = 533
                list_map_id = 533
                instance_id = 42
                group_members = [int]$Contract.raid_size
                leader_guid = 1
                health_pct = $health
                position = [pscustomobject]@{ x = [double]$guid + $PositionOffset; y = [double]($guid % 3); z = 0.0 }
                healer_target_guid = if ([string]$expected.role -eq 'healer') { [long]$HealerTargetGuid } else { $null }
                healer_target_health_pct = if ([string]$expected.role -eq 'healer' -and $HealerTargetGuid -eq $LowHealthGuid) { 40.0 } else { 90.0 }
                threat_links_truncated = $false
                interrupt_count = $InterruptCount
            }
        }
        return [pscustomobject][ordered]@{
            observed_at_utc = $At.ToUniversalTime().ToString('o')
            members = @($members)
            target = [pscustomobject]@{ observed = $true; guid = 900; health_current = $TargetHealth; health_max = 1000; health_pct = [double]$TargetHealth / 10.0; alive = $TargetHealth -gt 0 }
            target_threat_available = $true
            target_owner_guid = $TargetOwnerGuid
            target_owner_candidates = @($TargetOwnerGuid)
            hostile_owner_events = 4
            tank_hostile_owner_events = 4
            threat_links_truncated = $false
        }
    }

    function New-TestBridgeFixture {
        param(
            [Parameter(Mandatory)][object]$Contract,
            [System.Collections.IDictionary]$ErrorByGuid = @{},
            [uint32[]]$OmitFromListGuid = @()
        )

        $bots = New-Object System.Collections.Generic.List[object]
        $responses = [ordered]@{}
        foreach ($expected in @($Contract.members)) {
            $guid = [uint32]$expected.guid
            if ($OmitFromListGuid -notcontains $guid) {
                $bots.Add([pscustomobject]@{
                    guid = $guid
                    name = "Bot$guid"
                    alive = $true
                    combat = $true
                    position = [pscustomobject]@{ map = 533; x = [double]$guid; y = 0.0; z = 0.0 }
                    group = [pscustomobject]@{ members = [int]$Contract.raid_size; leader_guid = [uint32]$Contract.leader_guid }
                })
            }

            $errorCode = Get-RaidReadinessDictionaryValue -Dictionary $ErrorByGuid -Guid $guid
            if ($null -ne $errorCode) {
                $responses[[string]$guid] = [pscustomobject]@{ ok = $false; error = [string]$errorCode }
                continue
            }

            $role = [ordered]@{ tank = $false; healer = $false; dps = $false }
            $role[[string]$expected.role] = $true
            $responses[[string]$guid] = [pscustomobject]@{
                ok = $true
                telemetry = [pscustomobject]@{
                    guid = $guid
                    name = "Bot$guid"
                    alive = $true
                    in_combat = $true
                    location = [pscustomobject]@{ map_id = 533; instance_id = 42 }
                    group = [pscustomobject]@{ members = [int]$Contract.raid_size; leader_guid = [uint32]$Contract.leader_guid }
                    role = [pscustomobject]$role
                    health = [pscustomobject]@{ current = 900; max = 1000; pct = 90.0 }
                    victim = [pscustomobject]@{ guid = 900; alive = $true; health = [pscustomobject]@{ current = 800; max = 1000; pct = 80.0 } }
                    threat = [pscustomobject]@{ links_truncated = $false; links = @() }
                    recent = [pscustomobject]@{ counters = [pscustomobject]@{ available = $false } }
                }
            }
        }

        return [pscustomobject]@{
            list_response = [pscustomobject]@{ ok = $true; bots = $bots.ToArray() }
            combat_responses = $responses
        }
    }

    function New-OnyxiaTestContract {
        return New-RaidReadinessContract -RaidSize 10 -RosterGuid ([uint32[]](1..10)) -TankGuid ([uint32[]]@(1, 2)) `
            -HealerGuid ([uint32[]]@(3, 4, 5)) -LeaderGuid 1 -TargetGuid 900 -TargetName 'Onyxia' `
            -ExpectedMapId 249 -ExpectedInstanceId 42
    }

    function New-OnyxiaBindingRows {
        param(
            [Parameter(Mandatory)][object]$Contract,
            [uint32]$InstanceId = 42,
            [uint32[]]$OmitGuid = @()
        )

        return @($Contract.roster_guids | Where-Object { $OmitGuid -notcontains [uint32]$_ } | ForEach-Object {
            [pscustomobject]@{ guid = [uint32]$_; instance_id = [uint32]$InstanceId }
        })
    }

    function New-OnyxiaInstanceRow {
        param(
            [uint32]$InstanceId = 42,
            [uint32]$MapId = 249,
            [long]$CompletedEncounters = 1
        )

        return [pscustomobject]@{
            instance_id = $InstanceId
            map_id = $MapId
            difficulty = 0
            completed_encounters = $CompletedEncounters
        }
    }

    function New-OnyxiaEncounterBitRow {
        return [pscustomobject]@{ encounter_id = 707; map_id = 249; difficulty = 0; encounter_bit = 0 }
    }
}

Describe 'Raid-readiness dry-run contract' {
    It 'plans exact <Size>-member observation without contacting the bridge' -ForEach @(
        @{ Size = 10 }, @{ Size = 25 }, @{ Size = 40 }
    ) {
        $contract = New-TestContract -Size $Size
        $json = & $script:ProofPath -RaidSize $Size -RosterGuid $contract.roster_guids `
            -TankGuid $contract.tank_guids -HealerGuid $contract.healer_guids -LeaderGuid $contract.leader_guid `
            -TargetGuid $contract.target_guid -TargetName $contract.target_name -ExpectedMapId $contract.expected_map_id `
            -ExpectedInstanceId $contract.expected_instance_id -Port 1
        $plan = $json | ConvertFrom-Json
        $plan.dry_run | Should -BeTrue
        $plan.apply | Should -BeFalse
        $plan.mode | Should -Be 'plan-only'
        $plan.contract.raid_size | Should -Be $Size
        @($plan.contract.roster_guids).Count | Should -Be $Size
        @($plan.bridge_contract.allowed_actions) | Should -Be @('list', 'combatlog')
        @($plan.bridge_contract.mutation_actions).Count | Should -Be 0
        $plan.bridge_contract.fixture_staging.supported | Should -BeFalse
        $plan.schema_version | Should -Be 2
        @($plan.telemetry_expectations.required) | Should -Contain 'exact target death'
        @($plan.telemetry_expectations.required) | Should -Contain 'encounter DONE'
        @($plan.telemetry_expectations.required) | Should -Contain 'exact roster credit'
        $plan.overall_status | Should -Be 'DRY_RUN'
    }

    It 'rejects wrong-sized, duplicate, overlapping, and non-positive contracts offline' {
        { New-RaidReadinessContract -RaidSize 10 -RosterGuid ([uint32[]](1..9)) -TankGuid 1 -HealerGuid 2 -TargetGuid 900 -ExpectedMapId 533 -ExpectedInstanceId 42 } | Should -Throw
        { New-RaidReadinessContract -RaidSize 10 -RosterGuid ([uint32[]](@(1..9) + 9)) -TankGuid 1 -HealerGuid 2 -TargetGuid 900 -ExpectedMapId 533 -ExpectedInstanceId 42 } | Should -Throw
        { New-RaidReadinessContract -RaidSize 10 -RosterGuid ([uint32[]](1..10)) -TankGuid 1 -HealerGuid 1 -TargetGuid 900 -ExpectedMapId 533 -ExpectedInstanceId 42 } | Should -Throw
        { New-RaidReadinessContract -RaidSize 10 -RosterGuid ([uint32[]](1..10)) -TankGuid 1 -HealerGuid 2 -TargetGuid 0 -ExpectedMapId 533 -ExpectedInstanceId 42 } | Should -Throw
    }

    It 'contains no live dispatch for mutating bridge actions' {
        $source = Get-Content -LiteralPath $script:ProofPath -Raw
        $source | Should -Not -Match '(?i)&\s*\$controlPath\s+-Action\s+(activate|deactivate|party|rally|deploy|route|advance|engage|scout|quest|fixture-init|pause|resume|travel|recover)\b'
        $source | Should -Match "ValidateSet\('list', 'combatlog'\)"
    }
}

Describe 'Raid-readiness fail-closed roster guard' {
    It 'normalizes the established list and combatlog bridge shapes' {
        $contract = New-TestContract
        $bots = @()
        $combat = [ordered]@{}
        foreach ($expected in $contract.members) {
            $guid = [uint32]$expected.guid
            $bots += [pscustomobject]@{
                guid = $guid
                name = "Bot$guid"
                alive = $true
                combat = $true
                position = [pscustomobject]@{ map = 533; x = [double]$guid; y = 0.0; z = 0.0 }
                group = [pscustomobject]@{ members = 10; leader_guid = 1 }
            }
            $role = [ordered]@{ tank = $false; healer = $false; dps = $false }
            $role[[string]$expected.role] = $true
            $combat[[string]$guid] = [pscustomobject]@{
                guid = $guid
                name = "Bot$guid"
                alive = $true
                in_combat = $true
                location = [pscustomobject]@{ map_id = 533; instance_id = 42 }
                group = [pscustomobject]@{ members = 10; leader_guid = 1 }
                role = [pscustomobject]$role
                health = [pscustomobject]@{ current = 900; max = 1000; pct = 90.0 }
                victim = [pscustomobject]@{ guid = 900; alive = $true; health = [pscustomobject]@{ current = 800; max = 1000; pct = 80.0 } }
                healer = [pscustomobject]@{ target = if ([string]$expected.role -eq 'healer') { [pscustomobject]@{ guid = 1; health = [pscustomobject]@{ pct = 80.0 } } } else { $null } }
                threat = [pscustomobject]@{
                    links_truncated = $false
                    links = @([pscustomobject]@{
                        source = [pscustomobject]@{ guid = 900; alive = $true; health = [pscustomobject]@{ current = 800; max = 1000; pct = 80.0 } }
                        victim = [pscustomobject]@{ guid = 1 }
                    })
                }
                recent = [pscustomobject]@{ counters = [pscustomobject]@{ available = $false } }
            }
        }
        $sample = ConvertTo-RaidReadinessCanonicalSample -ListResponse ([pscustomobject]@{ ok = $true; bots = $bots }) -CombatByGuid $combat -Contract $contract
        (Test-RaidReadinessRosterGuard -Sample $sample -Contract $contract).status | Should -Be 'PASS'
        $sample.members.Count | Should -Be 10
        $sample.target.health_current | Should -Be 800
        $sample.target_owner_guid | Should -Be 1
        $sample.target_threat_available | Should -BeTrue
    }

    It 'records combatlog unavailability after observed death as offline dead attrition' {
        $contract = New-TestContract
        $deathSample = New-TestSample -Contract $contract -At ([datetime]'2026-07-14T10:00:00Z') -DeadGuid 10
        $deathGuard = Test-RaidReadinessRosterGuard -Sample $deathSample -Contract $contract -AllowReleasedDead
        $fixture = New-TestBridgeFixture -Contract $contract `
            -ErrorByGuid ([ordered]@{ '10' = 'bot_not_online' }) -OmitFromListGuid 10

        $sample = ConvertTo-RaidReadinessCanonicalBridgeSample -ListResponse $fixture.list_response `
            -CombatResponseByGuid $fixture.combat_responses -Contract $contract `
            -ObservedDeadGuid $deathGuard.attrition_guids -ObservedAtUtc ([datetime]'2026-07-14T10:00:10Z')
        $offline = @($sample.members | Where-Object { [uint32]$_.guid -eq 10 })[0]
        $guard = Test-RaidReadinessRosterGuard -Sample $sample -Contract $contract -AllowReleasedDead
        $measurement = Measure-RaidReadinessSamples -Samples @($deathSample, $sample) `
            -Contract $contract -MaximumAllowedDeaths 1

        @($deathGuard.attrition_guids) | Should -Be @([uint32]10)
        $offline.alive | Should -BeFalse
        $offline.online_match_count | Should -Be 0
        $offline.combat_telemetry_present | Should -BeFalse
        $offline.offline_dead_attrition | Should -BeTrue
        $offline.attrition_observation | Should -Be 'combatlog_bot_not_online_after_observed_death'
        $guard.status | Should -Be 'PASS'
        @($guard.attrition_guids) | Should -Be @([uint32]10)
        @($guard.offline_dead_guids) | Should -Be @([uint32]10)
        $measurement.survival.status | Should -Be 'PASS'
        $measurement.survival.unique_deaths | Should -Be 1

        $source = Get-Content -LiteralPath $script:ProofPath -Raw
        $source | Should -Match 'offline_dead_guids\s*=\s*@\(\$newOfflineAttrition'
    }

    It 'fails closed when combatlog loses a member before death was observed' {
        $contract = New-TestContract
        $fixture = New-TestBridgeFixture -Contract $contract `
            -ErrorByGuid ([ordered]@{ '10' = 'bot_not_online' }) -OmitFromListGuid 10

        { ConvertTo-RaidReadinessCanonicalBridgeSample -ListResponse $fixture.list_response `
                -CombatResponseByGuid $fixture.combat_responses -Contract $contract } |
            Should -Throw '*Bridge action combatlog failed for GUID 10: bot_not_online*'
    }

    It 'fails closed for a bridge error even when the member death was observed' {
        $contract = New-TestContract
        $fixture = New-TestBridgeFixture -Contract $contract `
            -ErrorByGuid ([ordered]@{ '10' = 'bridge_unavailable' }) -OmitFromListGuid 10

        { ConvertTo-RaidReadinessCanonicalBridgeSample -ListResponse $fixture.list_response `
                -CombatResponseByGuid $fixture.combat_responses -Contract $contract -ObservedDeadGuid 10 } |
            Should -Throw '*Bridge action combatlog failed for GUID 10: bridge_unavailable*'
    }

    It 'passes the exact roster and fails role, map, instance, group-size, and leader drift' {
        $contract = New-TestContract
        $sample = New-TestSample -Contract $contract -At ([datetime]'2026-07-14T10:00:00Z')
        (Test-RaidReadinessRosterGuard -Sample $sample -Contract $contract).status | Should -Be 'PASS'

        $sample.members[9].observed_role = 'healer'
        $sample.members[8].map_id = 1
        $sample.members[7].instance_id = 99
        $sample.members[6].group_members = 9
        $sample.members[5].leader_guid = 2
        $failed = Test-RaidReadinessRosterGuard -Sample $sample -Contract $contract
        $failed.status | Should -Be 'FAIL_CLOSED'
        $failed.reasons.Count | Should -BeGreaterOrEqual 5

        $wrongRoster = New-TestSample -Contract $contract -At ([datetime]'2026-07-14T10:00:00Z')
        $wrongRoster.members[9].guid = 99
        $wrongRosterGuard = Test-RaidReadinessRosterGuard -Sample $wrongRoster -Contract $contract -AllowReleasedDead
        $wrongRosterGuard.status | Should -Be 'FAIL_CLOSED'
        @($wrongRosterGuard.reasons) | Should -Contain 'roster_guid_set_mismatch'
    }
}

Describe 'Raid-readiness aggregation math' {
    It 'aggregates existing gates but does not pass on positive boss damage alone' {
        $contract = New-TestContract
        $first = New-TestSample -Contract $contract -At ([datetime]'2026-07-14T10:00:00Z') -TargetHealth 1000 -HealerTargetGuid 1 -InterruptCount 2
        $second = New-TestSample -Contract $contract -At ([datetime]'2026-07-14T10:00:10Z') -TargetHealth 600 -HealerTargetGuid 6 -LowHealthGuid 6 -InterruptCount 5 -PositionOffset 1
        $result = Measure-RaidReadinessSamples -Samples @($first, $second) -Contract $contract

        $result.overall_status | Should -Be 'FAIL'
        $result.tank_threat.target_owner_samples | Should -Be 2
        $result.tank_threat.tank_ownership_ratio | Should -Be 1
        $result.healer.selection_samples | Should -Be 6
        $result.healer.tank_selections | Should -Be 3
        $result.healer.tank_priority_ratio | Should -Be 0.5
        $result.healer.non_tank_triage_opportunities | Should -Be 1
        $result.healer.non_tank_triage_hits | Should -Be 1
        $result.healer.non_tank_triage_ratio | Should -Be 1
        $result.target_health.health_delta | Should -Be 400
        $result.target_health.health_delta_pct_of_start | Should -Be 40
        $result.target_health.status | Should -Be 'FAIL'
        $result.target_health.exact_target_death_required | Should -BeTrue
        $result.target_health.death_confirmed | Should -BeFalse
        $result.encounter.duration_seconds | Should -Be 10
        $result.survival.death_transitions | Should -Be 0
        $result.movement_cohesion.maximum_member_step | Should -Be 1
        $result.movement_cohesion.potential_teleport_events.Count | Should -Be 0
        $result.vertical_safety.status | Should -Be 'PASS'
        $result.vertical_safety.suspicious_vertical_drop_events.Count | Should -Be 0
        $result.interrupts.status | Should -Be 'MEASURED'
        $result.interrupts.total_counter_delta | Should -Be 30
    }

    It 'fails vertical safety when a living combatant steadily falls far below its encounter high point' {
        $contract = New-TestContract
        $first = New-TestSample -Contract $contract -At ([datetime]'2026-07-14T10:00:00Z') -TargetHealth 1000 -HealerTargetGuid 1
        $second = New-TestSample -Contract $contract -At ([datetime]'2026-07-14T10:00:10Z') -TargetHealth 900 -HealerTargetGuid 1
        $third = New-TestSample -Contract $contract -At ([datetime]'2026-07-14T10:00:20Z') -TargetHealth 800 -HealerTargetGuid 1
        $first.members[2].position.z = 100.0
        $second.members[2].position.z = 84.0
        $third.members[2].position.z = 69.0

        $result = Measure-RaidReadinessSamples -Samples @($first, $second, $third) -Contract $contract `
            -MaximumSuspiciousVerticalDrop 25.0

        $result.vertical_safety.status | Should -Be 'FAIL'
        $result.vertical_safety.anomaly_samples | Should -Be 1
        $result.vertical_safety.suspicious_vertical_drop_events.Count | Should -Be 1
        $result.vertical_safety.suspicious_vertical_drop_events[0].guid | Should -Be $third.members[2].guid
        $result.vertical_safety.suspicious_vertical_drop_events[0].cumulative_vertical_drop | Should -Be 31
    }

    It 'counts reachable non-tank triage once per sample and honors coordinated healing' {
        $contract = New-TestContract
        $sample = New-TestSample -Contract $contract -At ([datetime]'2026-07-14T10:00:00Z') `
            -HealerTargetGuid 1 -LowHealthGuid 6
        $sample.members[2].healer_target_guid = 6

        $result = Measure-RaidReadinessSamples -Samples @($sample) -Contract $contract
        $result.healer.non_tank_triage_opportunities | Should -Be 1
        $result.healer.non_tank_triage_hits | Should -Be 1

        $sample.members[5].position.x = 1000.0
        $unreachable = Measure-RaidReadinessSamples -Samples @($sample) -Contract $contract
        $unreachable.healer.non_tank_triage_opportunities | Should -Be 0
        $unreachable.healer.non_tank_triage_hits | Should -Be 0
    }

    It 'requires exact target death and blocks PASS when completion and exact credit are unproven' {
        $contract = New-TestContract
        $first = New-TestSample -Contract $contract -At ([datetime]'2026-07-14T10:00:00Z') -TargetHealth 1000 -HealerTargetGuid 1
        $second = New-TestSample -Contract $contract -At ([datetime]'2026-07-14T10:00:10Z') -TargetHealth 0 -HealerTargetGuid 6 -LowHealthGuid 6
        $result = Measure-RaidReadinessSamples -Samples @($first, $second) -Contract $contract

        $result.target_health.status | Should -Be 'PASS'
        $result.target_health.target_guid | Should -Be 900
        $result.target_health.death_confirmed | Should -BeTrue
        $result.target_health.death_evidence | Should -Be 'exact_target_guid_observed_alive_false'
        $result.encounter_completion_credit.status | Should -Be 'BLOCKED'
        $result.encounter_completion_credit.required_for_pass | Should -BeTrue
        $result.encounter_completion_credit.encounter_done.status | Should -Be 'UNPROVEN'
        $result.encounter_completion_credit.encounter_done.done | Should -BeNullOrEmpty
        $result.encounter_completion_credit.exact_roster_credit.status | Should -Be 'UNPROVEN'
        $result.encounter_completion_credit.exact_roster_credit.exact_match | Should -BeNullOrEmpty
        $result.encounter_completion_credit.exact_roster_credit.credited_roster_guids | Should -BeNullOrEmpty
        @($result.encounter_completion_credit.exact_roster_credit.expected_roster_guids) | Should -Be @($contract.roster_guids)
        $result.overall_status | Should -Be 'BLOCKED'
    }

    It 'does not accept another target death as exact boss-death proof' {
        $contract = New-TestContract
        $sample = New-TestSample -Contract $contract -At ([datetime]'2026-07-14T10:00:00Z') -TargetHealth 0 -HealerTargetGuid 1
        $sample.target.guid = 901
        $result = Measure-RaidReadinessSamples -Samples @($sample) -Contract $contract

        $result.target_health.status | Should -Be 'UNPROVEN'
        $result.target_health.death_confirmed | Should -BeFalse
        $result.target_health.exact_target_observation_samples | Should -Be 0
        $result.target_health.mismatched_target_guid_samples | Should -Be 1
        $result.overall_status | Should -Not -Be 'PASS'
    }

    It 'accepts exact target alive-false evidence without inferring death from health values' {
        $contract = New-TestContract
        $sample = New-TestSample -Contract $contract -At ([datetime]'2026-07-14T10:00:00Z') -TargetHealth 0 -HealerTargetGuid 1
        $sample.target.health_current = $null
        $sample.target.health_max = $null
        $sample.target.health_pct = $null
        $result = Measure-RaidReadinessSamples -Samples @($sample) -Contract $contract

        $result.target_health.status | Should -Be 'PASS'
        $result.target_health.observed_samples | Should -Be 0
        $result.target_health.alive_evidence_samples | Should -Be 1
        $result.target_health.death_confirmed | Should -BeTrue
        $result.overall_status | Should -Be 'BLOCKED'
    }

    It 'counts death transitions and wipes and fails survival' {
        $contract = New-TestContract
        $first = New-TestSample -Contract $contract -At ([datetime]'2026-07-14T10:00:00Z') -TargetHealth 1000 -HealerTargetGuid 1
        $second = New-TestSample -Contract $contract -At ([datetime]'2026-07-14T10:00:10Z') -TargetHealth 500 -HealerTargetGuid 6 -LowHealthGuid 6 -DeadGuid ([uint32[]](1..10))
        $result = Measure-RaidReadinessSamples -Samples @($first, $second) -Contract $contract

        $result.survival.status | Should -Be 'FAIL'
        $result.survival.death_transitions | Should -Be 10
        $result.survival.wipe_samples | Should -Be 1
        $result.overall_status | Should -Be 'FAIL'
    }

    It 'permits bounded attrition but never a full wipe' {
        $contract = New-TestContract
        $first = New-TestSample -Contract $contract -At ([datetime]'2026-07-14T10:00:00Z') -TargetHealth 1000 -HealerTargetGuid 1
        $second = New-TestSample -Contract $contract -At ([datetime]'2026-07-14T10:00:10Z') -TargetHealth 0 -HealerTargetGuid 6 -LowHealthGuid 6 -DeadGuid ([uint32[]](9,10))
        $result = Measure-RaidReadinessSamples -Samples @($first, $second) -Contract $contract -MaximumAllowedDeaths 2

        $result.survival.status | Should -Be 'PASS'
        $result.survival.unique_deaths | Should -Be 2
        $result.survival.maximum_allowed_deaths | Should -Be 2
        $result.survival.wipe_samples | Should -Be 0
    }

    It 'treats an automatically released dead member as attrition rather than roster drift' {
        $contract = New-TestContract
        $sample = New-TestSample -Contract $contract -At ([datetime]'2026-07-14T10:00:00Z')
        $dead = $sample.members[9]
        $dead.alive = $false
        $dead.map_id = 1
        $dead.list_map_id = 1
        $dead.instance_id = 0

        (Test-RaidReadinessRosterGuard -Sample $sample -Contract $contract).status | Should -Be 'FAIL_CLOSED'
        $guard = Test-RaidReadinessRosterGuard -Sample $sample -Contract $contract -AllowReleasedDead
        $guard.status | Should -Be 'PASS'
        @($guard.attrition_guids) | Should -Be @([uint32]10)
    }

    It 'reports interrupt telemetry unsupported without guessing from spell ids' {
        $contract = New-TestContract
        $first = New-TestSample -Contract $contract -At ([datetime]'2026-07-14T10:00:00Z') -TargetHealth 1000 -HealerTargetGuid 1
        $second = New-TestSample -Contract $contract -At ([datetime]'2026-07-14T10:00:10Z') -TargetHealth 500 -HealerTargetGuid 6 -LowHealthGuid 6
        foreach ($sample in @($first, $second)) { foreach ($member in $sample.members) { $member.interrupt_count = $null } }
        $result = Measure-RaidReadinessSamples -Samples @($first, $second) -Contract $contract
        $result.interrupts.status | Should -Be 'UNSUPPORTED'
        $result.interrupts.total_counter_delta | Should -Be 0
        $result.interrupts.note | Should -Match 'not treated as interrupt proof'
    }
}

Describe 'Raid-readiness authoritative Onyxia completion and credit gate' {
    It 'treats an empty read-only query result as zero evidence rows' {
        $rows = @(ConvertFrom-RaidReadinessTabularText -Text '' -Headers @('guid', 'instance_id'))

        $rows.Count | Should -Be 0
    }

    It 'blocks a roster bound to the wrong shared instance' {
        $contract = New-OnyxiaTestContract
        $result = Test-RaidReadinessOnyxiaCompletionCredit -Contract $contract `
            -BindingRows (New-OnyxiaBindingRows -Contract $contract -InstanceId 77) `
            -InstanceRows @(New-OnyxiaInstanceRow) -EncounterRows @(New-OnyxiaEncounterBitRow)

        $result.status | Should -Be 'BLOCKED'
        $result.exact_roster_credit.status | Should -Be 'UNPROVEN'
        @($result.reasons | Where-Object { $_ -match 'roster_bound_to_other_instance=77' }).Count | Should -Be 1
    }

    It 'blocks when one exact roster binding is missing' {
        $contract = New-OnyxiaTestContract
        $result = Test-RaidReadinessOnyxiaCompletionCredit -Contract $contract `
            -BindingRows (New-OnyxiaBindingRows -Contract $contract -OmitGuid 10) `
            -InstanceRows @(New-OnyxiaInstanceRow) -EncounterRows @(New-OnyxiaEncounterBitRow)

        $result.status | Should -Be 'BLOCKED'
        $result.exact_roster_credit.status | Should -Be 'UNPROVEN'
        @($result.reasons | Where-Object { $_ -eq 'guid_10_missing_expected_instance_binding' }).Count | Should -Be 1
    }

    It 'blocks when the exact Onyxia encounter bit is missing' {
        $contract = New-OnyxiaTestContract
        $result = Test-RaidReadinessOnyxiaCompletionCredit -Contract $contract `
            -BindingRows (New-OnyxiaBindingRows -Contract $contract) `
            -InstanceRows @(New-OnyxiaInstanceRow) -EncounterRows @()

        $result.status | Should -Be 'BLOCKED'
        $result.encounter_done.status | Should -Be 'UNPROVEN'
        $result.encounter_done.done | Should -BeNullOrEmpty
        @($result.reasons | Where-Object { $_ -eq 'onyxia_encounter_bit_not_resolved' }).Count | Should -Be 1
    }

    It 'passes only a completed bit with exact shared-instance bindings' {
        $contract = New-OnyxiaTestContract
        $result = Test-RaidReadinessOnyxiaCompletionCredit -Contract $contract `
            -BindingRows (New-OnyxiaBindingRows -Contract $contract) `
            -InstanceRows @(New-OnyxiaInstanceRow -CompletedEncounters 1) `
            -EncounterRows @(New-OnyxiaEncounterBitRow)

        $result.status | Should -Be 'PASS'
        $result.shared_instance.resolved_instance_id | Should -Be 42
        $result.encounter_done.status | Should -Be 'PASS'
        $result.encounter_done.done | Should -BeTrue
        $result.encounter_done.encounter_bit | Should -Be 0
        $result.exact_roster_credit.status | Should -Be 'PASS'
        $result.exact_roster_credit.exact_match | Should -BeTrue
        $result.target_damage_authoritative | Should -BeFalse
        $result.database_mutations | Should -Be 0
    }

    It 'uses exact encounter credit as death proof without treating positive damage as authoritative' {
        $contract = New-TestContract
        $onyxiaContract = New-OnyxiaTestContract
        $first = New-TestSample -Contract $contract -At ([datetime]'2026-07-14T10:00:00Z') -TargetHealth 1000
        $second = New-TestSample -Contract $contract -At ([datetime]'2026-07-14T10:00:10Z') -TargetHealth 600
        $gate = Test-RaidReadinessOnyxiaCompletionCredit -Contract $onyxiaContract `
            -BindingRows (New-OnyxiaBindingRows -Contract $onyxiaContract) `
            -InstanceRows @(New-OnyxiaInstanceRow) -EncounterRows @(New-OnyxiaEncounterBitRow)
        $result = Measure-RaidReadinessSamples -Samples @($first, $second) -Contract $contract -CompletionCreditGate $gate

        $result.encounter_completion_credit.status | Should -Be 'PASS'
        $result.encounter_completion_credit.target_damage_authoritative | Should -BeFalse
        $result.encounter_completion_credit.exact_target_death_authoritative | Should -BeTrue
        $result.target_health.status | Should -Be 'PASS'
        $result.target_health.death_confirmed | Should -BeTrue
        $result.target_health.observed_death_confirmed | Should -BeFalse
        $result.target_health.authoritative_death_confirmed | Should -BeTrue
        $result.target_health.death_evidence | Should -Be 'exact_encounter_credit_entry_completed_for_exact_roster'
    }

    It 'builds only read-only Onyxia database queries' {
        $contract = New-OnyxiaTestContract
        $queries = New-RaidReadinessOnyxiaReadOnlyQuerySet -Contract $contract
        $queries.read_only | Should -BeTrue
        $queries.database_mutations | Should -Be 0
        foreach ($query in @($queries.bindings, $queries.instances, $queries.encounter_bits)) {
            $query.sql | Should -Match '^\s*SELECT\b'
            $query.sql | Should -Not -Match '(?i)\b(INSERT|UPDATE|DELETE|DROP|ALTER|CREATE|TRUNCATE|REPLACE|CALL|SET|GRANT|REVOKE|LOAD|RENAME|LOCK|UNLOCK)\b'
        }
        $queries.encounter_bits.sql | Should -Match 'FROM acore_world\.instance_encounters'
        $queries.encounter_bits.sql | Should -Match '\b707\b'
        $queries.encounter_bits.sql | Should -Match '\b708\b'
        $queries.encounter_bits.sql | Should -Match 'creditEntry\s*=\s*10184'
        $queries.encounter_bits.sql | Should -Not -Match 'dungeonencounter_dbc'
    }

    It 'uses the existing worldserver config convention and SELECT-only client path' {
        $contract = New-OnyxiaTestContract
        $configPath = Join-Path $TestDrive 'worldserver.conf'
        $mysqlPath = Join-Path $TestDrive 'mysql-fake.ps1'
        @'
WorldDatabaseInfo = "127.0.0.1;3306;acore;acore;acore_world"
CharacterDatabaseInfo = "127.0.0.1;3306;acore;acore;acore_characters"
'@ | Set-Content -LiteralPath $configPath -Encoding utf8
        @'
$execute = @($args | Where-Object { "$_" -like '--execute=*' } | Select-Object -First 1)
$sql = if ($execute.Count -gt 0) { $execute[0].Substring('--execute='.Length) } else { '' }
if ($sql -match 'character_instance') {
    1..10 | ForEach-Object { "$_`t42" }
} elseif ($sql -match 'FROM acore_characters.instance') {
    "42`t249`t0`t1"
} elseif ($sql -match 'FROM acore_world.instance_encounters') {
    "707`t249`t0`t0"
    "708`t249`t1`t0"
}
exit 0
'@ | Set-Content -LiteralPath $mysqlPath -Encoding utf8

        $result = Get-RaidReadinessOnyxiaCompletionCreditFromDatabase -Contract $contract `
            -ServerRoot $TestDrive -WorldServerConfigPath $configPath -MySqlPath $mysqlPath

        $result.status | Should -Be 'PASS'
        $result.read_only | Should -BeTrue
        $result.database_mutations | Should -Be 0
        $result.database_query_contract.read_only | Should -BeTrue
    }

    It 'defaults closed without an adapter error when a SELECT returns no bindings' {
        $contract = New-OnyxiaTestContract
        $configPath = Join-Path $TestDrive 'worldserver-empty-bindings.conf'
        $mysqlPath = Join-Path $TestDrive 'mysql-empty-bindings-fake.ps1'
        @'
WorldDatabaseInfo = "127.0.0.1;3306;acore;acore;acore_world"
CharacterDatabaseInfo = "127.0.0.1;3306;acore;acore;acore_characters"
'@ | Set-Content -LiteralPath $configPath -Encoding utf8
        @'
$execute = @($args | Where-Object { "$_" -like '--execute=*' } | Select-Object -First 1)
$sql = if ($execute.Count -gt 0) { $execute[0].Substring('--execute='.Length) } else { '' }
if ($sql -match 'FROM acore_characters.instance') {
    "42`t249`t0`t1"
} elseif ($sql -match 'FROM acore_world.instance_encounters') {
    "707`t249`t0`t0"
    "708`t249`t1`t0"
}
exit 0
'@ | Set-Content -LiteralPath $mysqlPath -Encoding utf8

        $result = Get-RaidReadinessOnyxiaCompletionCreditFromDatabase -Contract $contract `
            -ServerRoot $TestDrive -WorldServerConfigPath $configPath -MySqlPath $mysqlPath

        $result.status | Should -Be 'BLOCKED'
        (Get-RaidReadinessProperty -Object $result -Name @('error')) | Should -BeNullOrEmpty
        $result.exact_roster_credit.status | Should -Be 'UNPROVEN'
        @($result.reasons | Where-Object { $_ -eq 'exact_roster_instance_binding_not_resolved' }).Count | Should -Be 1
        $result.read_only | Should -BeTrue
        $result.database_mutations | Should -Be 0
        $result.database_query_contract.read_only | Should -BeTrue
    }
}

Describe 'Raid-readiness pinned encounter completion gate' {
    BeforeAll {
        $script:VoAContract = New-RaidReadinessContract -RaidSize 10 -RosterGuid ([uint32[]](1..10)) `
            -TankGuid ([uint32[]]@(1, 2)) -HealerGuid ([uint32[]]@(3, 4, 5)) -LeaderGuid 1 `
            -TargetGuid 900 -TargetName 'Archavon' -ExpectedMapId 624 -ExpectedInstanceId 42
        $script:VoABindings = @(1..10 | ForEach-Object { [pscustomobject]@{ guid = $_; instance_id = 42 } })
        $script:VoAEncounter = @([pscustomobject]@{ encounter_id = 772; map_id = 624; difficulty = 0; encounter_bit = 0 })
    }

    It 'passes only with exact roster, map, difficulty, mapping, and completed bit' {
        $instance = @([pscustomobject]@{ instance_id = 42; map_id = 624; difficulty = 0; completed_encounters = 1 })
        $result = Test-RaidReadinessPinnedEncounterCompletionCredit -Contract $script:VoAContract `
            -EncounterId 772 -EncounterBit 0 -ExpectedDifficulty 0 -BindingRows $script:VoABindings `
            -InstanceRows $instance -EncounterRows $script:VoAEncounter

        $result.status | Should -Be 'PASS'
        $result.encounter_done.done | Should -BeTrue
        $result.encounter_done.encounter_mask | Should -Be 1
        $result.exact_roster_credit.exact_match | Should -BeTrue
        $result.database_mutations | Should -Be 0
    }

    It 'fails when the pinned encounter bit is not set' {
        $instance = @([pscustomobject]@{ instance_id = 42; map_id = 624; difficulty = 0; completed_encounters = 0 })
        $result = Test-RaidReadinessPinnedEncounterCompletionCredit -Contract $script:VoAContract `
            -EncounterId 772 -EncounterBit 0 -ExpectedDifficulty 0 -BindingRows $script:VoABindings `
            -InstanceRows $instance -EncounterRows $script:VoAEncounter

        $result.status | Should -Be 'FAIL'
        $result.encounter_done.done | Should -BeFalse
        @($result.reasons) | Should -Contain 'encounter_bit_not_set=0'
    }

    It 'builds SELECT-only exact mapping queries for Archavon' {
        $queries = New-RaidReadinessPinnedEncounterReadOnlyQuerySet -Contract $script:VoAContract `
            -EncounterId 772 -CreditEntry 31125 -EncounterBit 0 -ExpectedDifficulty 0
        foreach ($query in @($queries.bindings, $queries.instances, $queries.encounter_bits)) {
            $query.sql | Should -Match '^\s*SELECT\b'
            $query.sql | Should -Not -Match '(?i)\b(INSERT|UPDATE|DELETE|DROP|ALTER|CREATE|TRUNCATE|REPLACE|CALL|SET|GRANT|REVOKE|LOAD|RENAME|LOCK|UNLOCK)\b'
        }
        $queries.encounter_bits.sql | Should -Match 'e\.entry\s*=\s*772'
        $queries.encounter_bits.sql | Should -Match 'e\.creditEntry\s*=\s*31125'
        $queries.encounter_bits.sql | Should -Match '0 AS encounter_bit'
    }

    It 'dry-runs with an explicit pinned encounter adapter' {
        $plan = & $script:ProofPath -RaidSize 10 -RosterGuid ([uint32[]](1..10)) -TankGuid ([uint32[]]@(1, 2)) `
            -HealerGuid ([uint32[]]@(3, 4, 5)) -LeaderGuid 1 -TargetGuid 900 -TargetName Archavon `
            -ExpectedMapId 624 -ExpectedInstanceId 42 -ExpectedEncounterId 772 `
            -ExpectedEncounterCreditEntry 31125 -ExpectedEncounterBit 0 -ExpectedEncounterDifficulty 0 | ConvertFrom-Json
        $plan.authoritative_completion_credit.adapter | Should -Be 'pinned-encounter-read-only-db'
        $plan.authoritative_completion_credit.encounter_id | Should -Be 772
        $plan.authoritative_completion_credit.encounter_bit | Should -Be 0
    }
}

Describe 'Raid-readiness PowerShell syntax' {
    It 'parse-checks the proof, library, and focused test scripts' {
        foreach ($path in @($script:ProofPath, $script:LibraryPath, $PSCommandPath)) {
            $tokens = $null
            $errors = $null
            [System.Management.Automation.Language.Parser]::ParseFile($path, [ref]$tokens, [ref]$errors) | Out-Null
            @($errors).Count | Should -Be 0 -Because $path
        }
    }
}
