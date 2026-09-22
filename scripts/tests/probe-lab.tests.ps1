Set-StrictMode -Version Latest

BeforeAll {
    $script:ScriptsRoot = Split-Path -Parent $PSScriptRoot
    $script:LibraryPath = Join-Path $script:ScriptsRoot 'probe-lab-lib.ps1'
    $script:RunnerPath = Join-Path $script:ScriptsRoot 'probe-lab.ps1'
    $script:WrapperPath = Join-Path $script:ScriptsRoot 'probe-lab-run.ps1'
    $script:ExamplePath = Join-Path $script:ScriptsRoot 'fixtures\probe-lab.example.json'
    . $script:LibraryPath

    function New-TestProbeManifest {
        param([int]$Size = 5, [string]$Kind = 'party', [uint32]$FirstGuid = 1)
        $members = foreach ($offset in 0..($Size - 1)) {
            [pscustomobject]@{ guid = [uint32]($FirstGuid + $offset); name = "Bot$($FirstGuid + $offset)"; level = 80; spec_index = 0; quality = 3 }
        }
        [pscustomobject]@{
            schema = 'autowow.probe-lab.manifest.v1'
            lab_id = 'test-lab'
            probes = @([pscustomobject]@{
                id = 'test-probe'; kind = $Kind; size = $Size; leader_guid = $FirstGuid
                expected_map_id = 576; exterior_route = 'safe-exterior'; advance_waypoint = 'safe-entrance'; members = @($members)
            })
        }
    }

    function New-TestProbeSample {
        param([int]$Size = 5, [string]$Kind = 'party', [switch]$AllDead, [uint32]$InstanceId = 7)
        $members = foreach ($guid in 1..$Size) {
            [pscustomobject]@{
                guid = [uint32]$guid; name = "Bot$guid"; online = $true; combat_telemetry = $true
                bridge_error = $null; alive = -not $AllDead; death_state = if ($AllDead) { 'corpse' } else { 'alive' }
                released_corpse = $false; in_combat = $false; map_id = 576
                instance_id = $InstanceId; x = [double]$guid; y = 0.0; z = 0.0
                group_members = $Size; leader_guid = [uint32]1
            }
        }
        [pscustomobject]@{ probe_id = 'test-probe'; kind = $Kind; size = $Size; leader_guid = [uint32]1; expected_map_id = [uint32]576; fail_on_any_death = $false; members = @($members) }
    }
}

Describe 'Probe Lab manifest safety contract' {
    It 'accepts the example and plans only named bridge primitives' {
        $manifest = Read-ProbeLabManifest -Path $script:ExamplePath
        $plan = New-ProbeLabPlan -Manifest $manifest -ManifestPath $script:ExamplePath
        $plan.dry_run | Should -BeTrue
        @($plan.operations.action | Sort-Object -Unique) | Should -Not -Contain 'rally'
        @($plan.operations.action | Sort-Object -Unique) | Should -Not -Contain 'travel'
        $plan.safety.database_access | Should -BeFalse
        $plan.safety.server_restart | Should -BeFalse
        $plan.safety.interior_teleport | Should -BeFalse
        $manifest.probes[0].expected_map_id | Should -Be 576
        (Get-ProbeLabProperty $manifest.probes[0] advance_waypoint '') | Should -Be ''
        $manifest.probes[1].exterior_route | Should -Be 'voa-exterior'
        $manifest.probes[1].advance_waypoint | Should -Be 'voa-portal'
    }

    It 'supports party 5 and raid 10, 25, and 40' {
        { Assert-ProbeLabManifest (New-TestProbeManifest -Size 5 -Kind party) } | Should -Not -Throw
        foreach ($size in @(10, 25, 40)) {
            { Assert-ProbeLabManifest (New-TestProbeManifest -Size $size -Kind raid) } | Should -Not -Throw
        }
    }

    It 'plans probe-reset after online reconciliation and before every fixture-init' {
        $plan = New-ProbeLabPlan -Manifest (New-TestProbeManifest)
        $reset = @($plan.operations | Where-Object action -eq 'probe-reset-until-ready')[0]
        $firstFixture = @($plan.operations | Where-Object action -eq 'fixture-init')[0]
        $waitOnline = @($plan.operations | Where-Object action -eq 'wait-online')[0]
        $reset.sequence | Should -BeGreaterThan $waitOnline.sequence
        $reset.sequence | Should -BeLessThan $firstFixture.sequence
        $reset.destination | Should -Be 'safe-exterior'
        $reset.expected_map_id | Should -Be 576
    }

    It 'rejects wrong sizes, duplicate GUIDs, repeated leaders, unsafe names, and secrets' {
        { Assert-ProbeLabManifest (New-TestProbeManifest -Size 6 -Kind party) } | Should -Throw '*size 5*'
        $duplicate = New-TestProbeManifest
        $duplicate.probes[0].members[4].guid = 1
        { Assert-ProbeLabManifest $duplicate } | Should -Throw '*repeats member GUID*'

        $second = New-TestProbeManifest -FirstGuid 6
        $second.probes[0].id = 'second-probe'
        $second.probes[0].leader_guid = 1
        $multi = New-TestProbeManifest
        $multi.probes = @($multi.probes[0], $second.probes[0])
        { Assert-ProbeLabManifest $multi } | Should -Throw '*Leader GUID 1 is reused*'

        $unsafe = New-TestProbeManifest
        $unsafe.probes[0].exterior_route = 'route with spaces'
        { Assert-ProbeLabManifest $unsafe } | Should -Throw '*without whitespace*'
        $interior = New-TestProbeManifest
        $interior.probes[0].exterior_route = 'dungeon-interior'
        { Assert-ProbeLabManifest $interior } | Should -Throw '*exterior staging*'
        $missingMap = New-TestProbeManifest
        $missingMap.probes[0].expected_map_id = 0
        { Assert-ProbeLabManifest $missingMap } | Should -Throw '*expected_map_id must be positive*'
        $badPolicy = New-TestProbeManifest
        $badPolicy.probes[0] | Add-Member -NotePropertyName failure_policy -NotePropertyValue ([pscustomobject]@{ fail_on_any_death = 'yes' })
        { Assert-ProbeLabManifest $badPolicy } | Should -Throw '*must be boolean*'

        $secret = New-TestProbeManifest
        $secret | Add-Member -NotePropertyName password -NotePropertyValue 'forbidden'
        { Assert-ProbeLabManifest $secret } | Should -Throw '*Secret-bearing property*'
    }
}

Describe 'Probe Lab dry-run and bridge boundary' {
    It 'defaults to Plan and does not need a live bridge' {
        $plan = & $script:RunnerPath -ManifestPath $script:ExamplePath -Port 1 | ConvertFrom-Json
        $plan.mode | Should -Be 'Plan'
        $plan.dry_run | Should -BeTrue
    }

    It 'keeps Launch dry without Apply and rejects Apply in Monitor' {
        $plan = & $script:RunnerPath -Mode Launch -ManifestPath $script:ExamplePath -Port 1 | ConvertFrom-Json
        $plan.dry_run | Should -BeTrue
        $plan.note | Should -Match 'Supply -Apply'
        { & $script:RunnerPath -Mode Monitor -Apply -ManifestPath $script:ExamplePath } | Should -Throw '*not valid*'
    }

    It 'uses autowow-control and contains no TCP, DB, server restart, or teleport implementation' {
        $source = Get-Content -LiteralPath $script:RunnerPath -Raw
        $source | Should -Match 'Join-Path\s+\$PSScriptRoot\s+''autowow-control.ps1'''
        $source | Should -Not -Match 'TcpClient|System\.Net\.Sockets|mysql|Restart-Service|Start-Process|Stop-Process|rally|recover'
        $source | Should -Not -Match '(?i)\$(password|passwd|secret|credential|token)\b'
        $source | Should -Match "ValidateSet\('127\.0\.0\.1'\)"
    }

    It 'defaults the one-command wrapper to a dry-run Plan' {
        $plan = & $script:WrapperPath -ManifestPath $script:ExamplePath -Port 1 | ConvertFrom-Json
        $plan.mode | Should -Be 'Plan'
        $plan.dry_run | Should -BeTrue
        (Get-Content -LiteralPath $script:WrapperPath -Raw) | Should -Match 'probe-lab\.ps1'
    }

    It 'requires Apply for LaunchThenMonitor and keeps Monitor read-only' {
        { & $script:WrapperPath -Mode LaunchThenMonitor -ManifestPath $script:ExamplePath -Port 1 } | Should -Throw '*requires -Apply*'
        { & $script:WrapperPath -Mode Monitor -Apply -ManifestPath $script:ExamplePath } | Should -Throw '*only valid*'
    }

    It 'can restrict a plan to exact probe ids for partial-launch monitoring' {
        $all = & $script:RunnerPath -Mode Plan -ManifestPath $script:ExamplePath | ConvertFrom-Json
        $firstProbeId = @($all.operations | Where-Object probe_id -ne '*')[0].probe_id
        $filtered = & $script:RunnerPath -Mode Plan -ManifestPath $script:ExamplePath -ProbeId $firstProbeId | ConvertFrom-Json
        @($filtered.operations | Where-Object { $_.probe_id -notin @('*', $firstProbeId) }).Count | Should -Be 0
        { & $script:RunnerPath -Mode Plan -ManifestPath $script:ExamplePath -ProbeId 'missing-probe' } | Should -Throw '*Unknown ProbeId*'

        $wrapperFiltered = & $script:WrapperPath -Mode Plan -ManifestPath $script:ExamplePath -ProbeId $firstProbeId | ConvertFrom-Json
        @($wrapperFiltered.operations | Where-Object { $_.probe_id -notin @('*', $firstProbeId) }).Count | Should -Be 0
        { & $script:WrapperPath -Mode Plan -ManifestPath $script:ExamplePath -ProbeId 'missing-probe' } | Should -Throw '*Unknown ProbeId*'

        $wrapperSource = Get-Content -LiteralPath $script:WrapperPath -Raw
        $wrapperSource | Should -Match '\[string\[\]\]\$ProbeId\s*=\s*@\(\)'
        $wrapperSource | Should -Match '\$common\.ProbeId\s*=\s*@\(\$ProbeId\)'
        $wrapperSource | Should -Match '\$monitorArguments\.ProbeId\s*=\s*\$launchedProbeIds'
        $wrapperSource | Should -Match "reason\s*=\s*'no_probes_launched'"
    }
}

Describe 'Probe Lab launch failure isolation' {
    It 'polls PENDING reset state until READY and fails closed on refusal' {
        $responses = [System.Collections.Generic.Queue[object]]::new()
        $responses.Enqueue([pscustomobject]@{ ok = $true; status = 'PENDING'; code = 'staging_pending' })
        $responses.Enqueue([pscustomobject]@{ ok = $true; status = 'READY'; code = 'ready' })
        $state = @{ polls = 0 }
        $result = Wait-ProbeLabReset -TimeoutSeconds 5 -PollSeconds 1 `
            -Invoke { ++$state.polls; $responses.Dequeue() } -Sleep { param([int]$Seconds) }
        $result.status | Should -Be 'READY'
        $state.polls | Should -Be 2

        { Wait-ProbeLabReset -TimeoutSeconds 5 -Invoke {
            [pscustomobject]@{ ok = $false; status = 'REFUSED'; code = 'active_combat' }
        } -Sleep { param([int]$Seconds) } } | Should -Throw '*active_combat*'
    }

    It 'builds a structured operation failure' {
        $failure = New-ProbeLabLaunchFailure -ProbeId 'probe-a' -Operation 'fixture-init' `
            -Detail 'Bridge action fixture-init failed for GUID 1: forced_failure' -Guid 1
        $failure.probe_id | Should -Be 'probe-a'
        $failure.code | Should -Be 'bridge_error'
        $failure.operation | Should -Be 'fixture-init'
        $failure.guid | Should -Be 1
        $failure.detail | Should -Match 'forced_failure'
        $failure.observed_at_utc | Should -Not -BeNullOrEmpty
    }

    It 'records one failed probe and continues launching an independent probe' {
        $serverRoot = Join-Path $TestDrive 'server'
        New-Item -ItemType Directory -Path (Join-Path $serverRoot 'logs') -Force | Out-Null

        $manifest = New-TestProbeManifest
        $manifest.probes[0].advance_waypoint = ''
        $second = New-TestProbeManifest -FirstGuid 6
        $second.probes[0].id = 'second-probe'
        $second.probes[0].exterior_route = 'second-exterior'
        $second.probes[0].advance_waypoint = ''
        $manifest.probes = @($manifest.probes[0], $second.probes[0])
        $manifestPath = Join-Path $TestDrive 'two-probes.json'
        $manifest | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $manifestPath -Encoding utf8NoBOM

        $callLog = Join-Path $TestDrive 'control-calls.txt'
        $controlPath = Join-Path $TestDrive 'fake-control.ps1'
        @'
[CmdletBinding()]
param(
    [string]$Action, [string]$BridgeHost, [int]$Port, [int]$TimeoutMs,
    [uint32]$BotGuid = 0, [uint32[]]$MemberGuid = @(), [int]$Level = 1,
    [int]$SpecIndex = 0, [int]$Quality = 2, [int]$RaidDifficulty = 0,
    [string]$Destination = '', [uint32]$ExpectedMapId = 0, [uint32]$ExpectedDifficulty = 0
)
[System.IO.File]::AppendAllText($env:PROBE_LAB_TEST_CALL_LOG, "$Action|$BotGuid`n")
if ($Action -eq 'fixture-init' -and $BotGuid -eq 1) {
    [pscustomobject]@{ ok = $false; error = 'forced_failure' } | ConvertTo-Json -Compress
    return
}
if ($Action -eq 'probe-reset') {
    [pscustomobject]@{ ok = $true; status = 'READY'; code = 'ready'; operation = 'probe_reset' } | ConvertTo-Json -Compress
    return
}
if ($Action -eq 'list') {
    $bots = foreach ($guid in 1..10) {
        [pscustomobject]@{
            guid = $guid; name = "Bot$guid"; alive = $true; combat = $false
            position = [pscustomobject]@{ map = 576; x = $guid; y = 0; z = 0 }
            group = [pscustomobject]@{ members = 0; leader_guid = 0 }
        }
    }
    [pscustomobject]@{ ok = $true; bots = @($bots) } | ConvertTo-Json -Compress -Depth 10
    return
}
if ($Action -eq 'combatlog') {
    $telemetry = [pscustomobject]@{
        alive = $true; death_state = 'alive'; in_combat = $false
        location = [pscustomobject]@{ map_id = 576; instance_id = 0 }
        group = [pscustomobject]@{ members = 0; leader_guid = 0 }
    }
    [pscustomobject]@{ ok = $true; telemetry = $telemetry } | ConvertTo-Json -Compress -Depth 10
    return
}
[pscustomobject]@{ ok = $true; action = $Action; guid = $BotGuid } | ConvertTo-Json -Compress
'@ | Set-Content -LiteralPath $controlPath -Encoding utf8NoBOM

        $previousCallLog = $env:PROBE_LAB_TEST_CALL_LOG
        try {
            $env:PROBE_LAB_TEST_CALL_LOG = $callLog
            $raw = & $script:RunnerPath -Mode Launch -Apply -ManifestPath $manifestPath `
                -ServerRoot $serverRoot -ControlScriptPath $controlPath -ReceiptPath 'isolation.jsonl' | Out-String
            $result = $raw | ConvertFrom-Json
        } finally {
            $env:PROBE_LAB_TEST_CALL_LOG = $previousCallLog
        }

        $result.status | Should -Be 'PARTIAL'
        $result.probes[0].status | Should -Be 'FAIL'
        $result.probes[0].first_failure.code | Should -Be 'bridge_error'
        $result.probes[0].first_failure.operation | Should -Be 'fixture-init'
        $result.probes[0].first_failure.guid | Should -Be 1
        $result.probes[1].status | Should -Be 'LAUNCHED'

        $calls = Get-Content -LiteralPath $callLog
        $calls | Should -Contain 'fixture-init|6'
        $calls | Should -Contain 'route|6'
        $calls | Should -Not -Contain 'route|1'

        $receipts = Get-Content -LiteralPath (Join-Path $serverRoot 'logs\isolation.jsonl') | ConvertFrom-Json
        $probeFailure = @($receipts | Where-Object event -eq 'probe_launch_failed')
        $probeFailure.Count | Should -Be 1
        $probeFailure[0].probe_id | Should -Be 'test-probe'
        $probeFailure[0].failure.operation | Should -Be 'fixture-init'
        @($receipts | Where-Object event -eq 'launch_complete')[0].status | Should -Be 'PARTIAL'
    }
}

Describe 'Probe Lab telemetry and first-failure classification' {
    It 'normalizes list plus combatlog for every exact member' {
        $manifest = New-TestProbeManifest
        $bots = foreach ($guid in 1..5) {
            [pscustomobject]@{ guid = $guid; name = "Bot$guid"; alive = $true; combat = $false; position = [pscustomobject]@{ map = 576; x = $guid; y = 0; z = 0 }; group = [pscustomobject]@{ members = 5; leader_guid = 1 } }
        }
        $combat = [ordered]@{}
        foreach ($guid in 1..5) {
            $combat[[string]$guid] = [pscustomobject]@{ ok = $true; telemetry = [pscustomobject]@{ alive = $true; death_state = 'alive'; in_combat = $false; location = [pscustomobject]@{ map_id = 576; instance_id = 7 }; group = [pscustomobject]@{ members = 5; leader_guid = 1 } } }
        }
        $sample = ConvertTo-ProbeLabSample -Manifest $manifest -ListResponse ([pscustomobject]@{ ok = $true; bots = $bots }) -CombatByGuid $combat
        $sample.probes[0].members.Count | Should -Be 5
        $sample.probes[0].members[0].death_state | Should -Be 'alive'
        (Test-ProbeLabAdmission $sample.probes[0]) | Should -BeTrue
    }

    It 'admits only the expected map with every exact member in one common nonzero instance' {
        $sample = New-TestProbeSample
        (Test-ProbeLabAdmission $sample) | Should -BeTrue
        $sample.members[4].map_id = 600
        (Test-ProbeLabAdmission $sample) | Should -BeFalse
        $sample.members[4].map_id = 576; $sample.members[4].instance_id = 0
        (Test-ProbeLabAdmission $sample) | Should -BeFalse
        $sample.members[4].instance_id = 8
        (Test-ProbeLabAdmission $sample) | Should -BeFalse
    }

    It 'latches successful admission when released deaths move ghosts to a graveyard map' {
        $started = [datetime]'2026-07-16T06:00:00Z'
        $admitted = New-TestProbeSample
        $first = Get-ProbeLabAdmissionState -ProbeSample $admitted -AdvanceWaypoint 'inside' `
            -StartedAt $started -Now $started.AddSeconds(5) -TimeoutSeconds 10
        $first.converged | Should -BeTrue
        $first.timed_out | Should -BeFalse

        $admitted.members[0].alive = $false
        $admitted.members[0].death_state = 'corpse'
        $admitted.members[0].released_corpse = $true
        $admitted.members[0].map_id = 1
        $admitted.members[0].instance_id = 0
        $afterDeath = Get-ProbeLabAdmissionState -ProbeSample $admitted -AdvanceWaypoint 'inside' `
            -PreviouslyConverged $first.converged -StartedAt $started -Now $started.AddSeconds(30) -TimeoutSeconds 10
        $afterDeath.admitted_now | Should -BeFalse
        $afterDeath.converged | Should -BeTrue
        $afterDeath.timed_out | Should -BeFalse
    }

    It 'classifies offline, bridge errors, split instances, dead party, raid wipe, cohesion, admission, and no progress' {
        $offline = New-TestProbeSample; $offline.members[4].online = $false
        (Get-ProbeLabFirstFailure $offline).code | Should -Be 'offline'
        $bridge = New-TestProbeSample; $bridge.members[0].bridge_error = 'timeout'
        (Get-ProbeLabFirstFailure $bridge).code | Should -Be 'bridge_error'
        $split = New-TestProbeSample; $split.members[4].instance_id = 8
        (Get-ProbeLabFirstFailure $split).code | Should -Be 'split_instance'
        (Get-ProbeLabFirstFailure (New-TestProbeSample -AllDead)).code | Should -Be 'dead_party'
        (Get-ProbeLabFirstFailure (New-TestProbeSample -Size 10 -Kind raid -AllDead)).code | Should -Be 'wipe'
        (Get-ProbeLabFirstFailure (New-TestProbeSample) -AdmissionTimedOut).code | Should -Be 'admission_timeout'

        $current = New-TestProbeSample; $current.members[4].x = 200
        (Get-ProbeLabFirstFailure $current -PreviousProbeSample (New-TestProbeSample) -ProgressTimedOut -CohesionRadius 60).code | Should -Be 'cohesion_stall'
        (Get-ProbeLabFirstFailure (New-TestProbeSample) -PreviousProbeSample (New-TestProbeSample) -ProgressTimedOut).code | Should -Be 'no_progress'
    }

    It 'does not fail isolated death or released corpse unless policy opts in' {
        $sample = New-TestProbeSample
        $sample.members[4].alive = $false
        $sample.members[4].death_state = 'corpse'
        $sample.members[4].released_corpse = $true
        $sample.members[4].map_id = 571
        $sample.members[4].instance_id = 0
        (Get-ProbeLabFirstFailure $sample) | Should -BeNullOrEmpty

        $sample.fail_on_any_death = $true
        $failure = Get-ProbeLabFirstFailure $sample
        $failure.code | Should -Be 'member_death'
        $failure.detail | Should -Match 'death_states=corpse'
    }

    It 'reports released-corpse context when surviving members are genuinely split' {
        $sample = New-TestProbeSample
        $sample.members[4].alive = $false
        $sample.members[4].death_state = 'corpse'
        $sample.members[4].released_corpse = $true
        $sample.members[4].map_id = 571
        $sample.members[4].instance_id = 0
        $sample.members[3].instance_id = 8
        $failure = Get-ProbeLabFirstFailure $sample
        $failure.code | Should -Be 'split_instance'
        $failure.detail | Should -Match 'released_corpse_guids=5'
        $failure.detail | Should -Match 'active_instance_ids=7,8'
    }

    It 'ignores an offline known-dead member while the active probe continues' {
        $sample = New-TestProbeSample
        $sample.members[4].online = $false
        $sample.members[4].combat_telemetry = $false
        $sample.members[4].bridge_error = 'bridge_error'
        $sample.members[4].alive = $false
        $sample.members[4].death_state = 'unknown'
        (Get-ProbeLabFirstFailure $sample -KnownDeadGuid 5) | Should -BeNullOrEmpty
    }

    It 'classifies current health as PASS, DEGRADED, or FAIL using combat capacity' {
        $healthy = New-TestProbeSample
        $healthyHealth = Get-ProbeLabCurrentHealth -ProbeSample $healthy
        $healthyHealth.status | Should -Be 'PASS'
        $healthyHealth.reason | Should -Be 'all_members_alive'
        $healthyHealth.count_summary | Should -Be '0 dead / 5 alive'

        $minority = New-TestProbeSample
        $minority.members[4].alive = $false
        $minority.members[4].death_state = 'released'
        $minority.members[4].released_corpse = $true
        $minorityHealth = Get-ProbeLabCurrentHealth -ProbeSample $minority
        $minorityHealth.status | Should -Be 'DEGRADED'
        $minorityHealth.reason | Should -Be 'minority_member_deaths'
        $minorityHealth.count_summary | Should -Be '1 dead / 4 alive; 1 released'

        $lostCapacity = New-TestProbeSample
        foreach ($member in @($lostCapacity.members[0..2])) { $member.alive = $false }
        $lostHealth = Get-ProbeLabCurrentHealth -ProbeSample $lostCapacity
        $lostHealth.status | Should -Be 'FAIL'
        $lostHealth.reason | Should -Be 'lost_combat_capacity'
        $lostHealth.counts.dead | Should -Be 3
        $lostHealth.counts.alive | Should -Be 2

        $failure = [pscustomobject]@{ code = 'navigator_blocked' }
        (Get-ProbeLabCurrentHealth -ProbeSample $healthy -FirstFailure $failure).status | Should -Be 'FAIL'
        (Get-ProbeLabCurrentHealth -ProbeSample $healthy -FirstFailure $failure).reason | Should -Be 'first_failure:navigator_blocked'
    }

    It 'renders aggregate status and worst-probe health in the Markdown headline and table' {
        $summary = [pscustomobject][ordered]@{
            lab_id = 'health-lab'; mode = 'Monitor'; status = 'DEGRADED'; reason = 'dtk:minority_member_deaths'
            sample_count = 1; receipt_path = 'logs/receipt.jsonl'
            probes = @(
                [pscustomobject]@{ probe_id = 'dtk'; kind = 'party'; size = 5; status = 'DEGRADED'; reason = 'minority_member_deaths'; current_health = [pscustomobject]@{ reason = 'minority_member_deaths'; count_summary = '1 dead / 4 alive; 1 released' }; first_failure = $null },
                [pscustomobject]@{ probe_id = 'voa'; kind = 'raid'; size = 10; status = 'PASS'; reason = 'all_members_alive'; current_health = [pscustomobject]@{ reason = 'all_members_alive'; count_summary = '0 dead / 10 alive' }; first_failure = $null }
            )
        }
        $markdown = ConvertTo-ProbeLabMarkdownSummary -Summary $summary
        $markdown | Should -Match '# Probe Lab: DEGRADED - health-lab'
        $markdown | Should -Match '\| dtk \| party/5 \| DEGRADED \| minority_member_deaths \| 1 dead / 4 alive; 1 released \|'
        $markdown | Should -Match 'FAIL = first failure or dead members at least living members'
    }

    It 'takes the worst aggregate status in FAIL over DEGRADED over PASS order' {
        $pass = [pscustomobject]@{ status = 'PASS' }
        $degraded = [pscustomobject]@{ status = 'DEGRADED' }
        $fail = [pscustomobject]@{ status = 'FAIL' }
        (Get-ProbeLabWorstStatus -ProbeSummaries @($pass, $degraded)).ToString() | Should -Be 'DEGRADED'
        (Get-ProbeLabWorstStatus -ProbeSummaries @($pass, $degraded, $fail)).ToString() | Should -Be 'FAIL'
        (Get-ProbeLabWorstStatus -ProbeSummaries @($pass)).ToString() | Should -Be 'PASS'
    }

    It 'extracts and attributes DungeonNavigator blocked reasons' {
        $events = Get-ProbeLabNavigatorEvents -Lines @(
            '[DungeonNavigator] bot=Bot1 map=576 route_source=travel_nodes blocked=unsafe_waypoint',
            '[DungeonNavigator] bot=Other map=576 blocked=unreachable'
        ) -MemberNames @('Bot1')
        $events.Count | Should -Be 1
        $events[0].reason | Should -Be 'unsafe_waypoint'
        $events[0].known_reason | Should -BeTrue
        (Get-ProbeLabFirstFailure (New-TestProbeSample) -NavigatorEvents $events).navigator_reason | Should -Be 'unsafe_waypoint'
    }

    It 'requires persistent timeout before party cohesion becomes a failure' {
        $events = Get-ProbeLabNavigatorEvents -Lines @('[DungeonNavigator] bot=Bot1 map=576 blocked=party_cohesion') -MemberNames @('Bot1')
        (Get-ProbeLabFirstFailure (New-TestProbeSample) -NavigatorEvents $events) | Should -BeNullOrEmpty
        $failure = Get-ProbeLabFirstFailure (New-TestProbeSample) -NavigatorEvents $events -ProgressTimedOut
        $failure.code | Should -Be 'cohesion_stall'
        $failure.navigator_reason | Should -Be 'party_cohesion'
    }

    It 'tails only newly appended Playerbots log bytes' {
        $path = Join-Path $TestDrive 'Playerbots.log'
        [System.IO.File]::WriteAllText($path, "old line`n", [System.Text.UTF8Encoding]::new($false))
        $offset = (Get-Item $path).Length
        [System.IO.File]::AppendAllText($path, "[DungeonNavigator] bot=Bot1 blocked=unreachable`n", [System.Text.UTF8Encoding]::new($false))
        $delta = Read-ProbeLabLogDelta -Path $path -Offset $offset
        $delta.lines.Count | Should -Be 1
        $delta.lines[0] | Should -Match 'blocked=unreachable'
        $delta.next_offset | Should -Be (Get-Item $path).Length
    }
}

Describe 'Probe Lab output safety and syntax' {
    It 'resolves an omitted output path beneath the logs directory' {
        $root = Join-Path $TestDrive 'server'
        New-Item -ItemType Directory -Path (Join-Path $root 'logs') -Force | Out-Null
        $resolved = Resolve-ProbeLabOutputPath -ServerRoot $root -Path '' -DefaultName 'probe.jsonl'
        $resolved | Should -Be ([System.IO.Path]::GetFullPath((Join-Path $root 'logs/probe.jsonl')))
    }

    It 'constrains all generated evidence beneath logs' {
        $root = Join-Path $TestDrive 'root'
        New-Item -ItemType Directory -Path (Join-Path $root logs) -Force | Out-Null
        (Resolve-ProbeLabOutputPath -ServerRoot $root -Path 'receipt.jsonl' -DefaultName 'unused') | Should -Be (Join-Path $root 'logs\receipt.jsonl')
        { Resolve-ProbeLabOutputPath -ServerRoot $root -Path (Join-Path $TestDrive 'outside.json') -DefaultName 'unused' } | Should -Throw '*must remain under*'
    }

    It 'parse-checks the library, runner, and focused tests' {
        foreach ($path in @($script:LibraryPath, $script:RunnerPath, $script:WrapperPath, $PSCommandPath)) {
            $tokens = $null; $errors = $null
            [System.Management.Automation.Language.Parser]::ParseFile($path, [ref]$tokens, [ref]$errors) | Out-Null
            @($errors).Count | Should -Be 0 -Because $path
        }
    }
}
