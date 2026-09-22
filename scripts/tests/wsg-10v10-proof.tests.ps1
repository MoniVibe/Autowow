Set-StrictMode -Version Latest

BeforeAll {
    $script:ScriptsRoot = Split-Path -Parent $PSScriptRoot
    $script:ControlPath = Join-Path $script:ScriptsRoot 'autowow-control.ps1'
    $script:ProofPath = Join-Path $script:ScriptsRoot 'wsg-10v10-proof.ps1'
    $script:ProofLibraryPath = Join-Path $script:ScriptsRoot 'wsg-10v10-proof-lib.ps1'
    . $script:ProofLibraryPath
    $script:Roster = [uint32[]](1..20)

    function New-WsgStatusSample {
        param(
            [int]$Kills = 0,
            [int]$Deaths = 0,
            [int]$Damage = 0,
            [int]$Healing = 0,
            [int]$Captures = 0,
            [int]$Returns = 0,
            [string]$AllianceFlagState = 'base',
            [string]$HordeFlagState = 'base',
            [int]$AllianceScore = 0,
            [int]$HordeScore = 0,
            [string]$State = 'in_progress',
            [string]$Winner = ''
        )

        $players = @()
        foreach ($guid in 1..20) {
            $players += [pscustomobject]@{
                guid = $guid
                faction = if ($guid -le 10) { 'Alliance' } else { 'Horde' }
                instance_id = 77
                queue_state = 'in_progress'
                battleground_state = $State
                kills = if ($guid -eq 1) { $Kills } else { 0 }
                deaths = if ($guid -eq 11) { $Deaths } else { 0 }
                damage = if ($guid -eq 1) { $Damage } else { 0 }
                healing = if ($guid -eq 2) { $Healing } else { 0 }
                flag_state = 'none'
                flag_captures = if ($guid -eq 1) { $Captures } else { 0 }
                flag_returns = if ($guid -eq 2) { $Returns } else { 0 }
            }
        }
        return [pscustomobject]@{
            ok = $true
            instance_id = 77
            battleground_state = $State
            roster = $players
            flags = [pscustomobject]@{
                alliance = [pscustomobject]@{ state = $AllianceFlagState; captures = $Captures; returns = $Returns }
                horde = [pscustomobject]@{ state = $HordeFlagState; captures = 0; returns = 0 }
            }
            score = [pscustomobject]@{ alliance = $AllianceScore; horde = $HordeScore }
            winner = $Winner
        }
    }

    function New-WsgExistingFixturePreflightRows {
        $rows = @()
        foreach ($guid in $script:Roster) {
            $rows += [pscustomobject]@{
                guid = $guid
                online = $true
                fixture_eligible = $true
                status_ok = $true
                status_operation = 'fixture_status'
                status_guid = $guid
                level = 80
                class_id = if ($guid -le 10) { 11 } else { 5 }
                spec_index = [int]($guid % 7)
                spec_name = "existing-spec-$guid"
                initialized = $true
                quality_signature = "equipped=16;other=0;rare=16"
                equipped_slots = 16
                equipment_slots_checked = 16
                expected_quality_slots = 16
                other_quality_slots = 0
                bracket_index = 7
            }
        }
        return $rows
    }
}

Describe 'AutoWoW WSG control request construction' {
    It 'builds one ordered atomic queue request for exactly twenty GUIDs' {
        & $script:ControlPath -Action wsg-queue -MemberGuid $script:Roster -EmitRequestOnly |
            Should -BeExactly ('wsg queue ' + ((1..20) -join ' '))
    }

    It 'builds status and leave requests over the same complete roster' {
        & $script:ControlPath -Action wsg-status -MemberGuid $script:Roster -EmitRequestOnly |
            Should -BeExactly ('wsg status ' + ((1..20) -join ' '))
        & $script:ControlPath -Action wsg-leave -MemberGuid $script:Roster -EmitRequestOnly |
            Should -BeExactly ('wsg leave ' + ((1..20) -join ' '))
    }

    It 'rejects short, duplicate, zero, and mixed bot-scoped WSG requests offline' {
        { & $script:ControlPath -Action wsg-queue -MemberGuid ([uint32[]](1..19)) -EmitRequestOnly } | Should -Throw
        { & $script:ControlPath -Action wsg-status -MemberGuid ([uint32[]](@(1..19) + 19)) -EmitRequestOnly } | Should -Throw
        { & $script:ControlPath -Action wsg-leave -MemberGuid ([uint32[]](@(0) + @(2..20))) -EmitRequestOnly } | Should -Throw
        { & $script:ControlPath -Action wsg-queue -BotGuid 99 -MemberGuid $script:Roster -EmitRequestOnly } | Should -Throw
    }
}

Describe 'WSG 10v10 proof dry-run contract' {
    It 'is dry-run by default and plans no grouping commands' {
        $plan = (& $script:ProofPath -RosterGuid $script:Roster) | ConvertFrom-Json
        $plan.dry_run | Should -BeTrue
        $plan.apply | Should -BeFalse
        $plan.stage_id | Should -Be 'wsg-10v10'
        @($plan.alliance_guids).Count | Should -Be 10
        @($plan.horde_guids).Count | Should -Be 10
        $plan.control_plan.grouping_requests | Should -Be 0
        $plan.control_plan.queue_requests | Should -Be 1
        $plan.control_plan.leave_requests | Should -Be 1
        $plan.queue_instance.status | Should -Be 'NOT_RUN'
        $plan.gameplay_telemetry.status | Should -Be 'NOT_RUN'
        $plan.overall_status | Should -Be 'DRY_RUN'
    }

    It 'keeps the legacy global-spec fixture initialization path by default' {
        $plan = (& $script:ProofPath -RosterGuid $script:Roster) | ConvertFrom-Json
        $plan.fixture_init_mode | Should -Be 'NORMALIZE_GLOBAL_SPEC'
        $plan.spec_policy | Should -Be 'global_spec_index'
        $plan.expected_spec_index | Should -Be 0
        $plan.expected_quality | Should -Be 2
        $plan.control_plan.fixture_init_requests | Should -Be 20
    }

    It 'plans skip-fixture-init as a read-only level-80 rare preflight that preserves existing specs' {
        $plan = (& $script:ProofPath -RosterGuid $script:Roster -SkipFixtureInit) | ConvertFrom-Json
        $plan.fixture_init_mode | Should -Be 'PRESERVE_EXISTING'
        $plan.spec_policy | Should -Be 'preserve_existing'
        $plan.expected_level | Should -Be 80
        $plan.expected_quality | Should -Be 3
        $plan.expected_spec_index | Should -BeNullOrEmpty
        $plan.control_plan.fixture_init_requests | Should -Be 0
        $plan.control_plan.fixture_status_requests | Should -Be 20
        $plan.control_plan.fixture_preflight | Should -Be 'strict_existing_fixture_status'
        $plan.control_plan.queue_requests | Should -Be 1
        $plan.overall_status | Should -Be 'DRY_RUN'
    }

    It 'rejects explicit skip-fixture-init level or quality values outside the role-roster contract' {
        { & $script:ProofPath -RosterGuid $script:Roster -SkipFixtureInit -Level 79 } | Should -Throw '*requires level 80*'
        { & $script:ProofPath -RosterGuid $script:Roster -SkipFixtureInit -Quality 2 } | Should -Throw '*requires quality 3*'
    }

    It 'guards the mutating fixture-init call and routes skip mode through strict preflight' {
        $source = Get-Content -LiteralPath $script:ProofPath -Raw
        $source | Should -Match '(?s)if\s*\(\s*-not\s+\$SkipFixtureInit\s*\)\s*\{.*?Get-WsgProofBridgeResponse\s+-Action\s+fixture-init'
        $source | Should -Match 'Test-Wsg10v10ExistingFixturePreflight'
        $source | Should -Not -Match '\(if\s*\('
    }

    It 'makes roster activation idempotent for bots already online' {
        $source = Get-Content -LiteralPath $script:ProofPath -Raw
        $source | Should -Match '\$initialOnlineGuids\s*=\s*@\(Get-WsgProofOnlineGuids\)'
        $source | Should -Match '(?s)foreach\s*\(\$guid in \$orderedRoster\).*?if\s*\(\$guid -notin \$initialOnlineGuids\).*?Action\s+activate'
    }

    It 'tolerates one transient roster poll and accepts verified offline cleanup after leave rejection' {
        $source = Get-Content -LiteralPath $script:ProofPath -Raw
        $source | Should -Match '\$instanceFailureStreak\s*-ge\s*3'
        $source | Should -Match 'transient_status_failures'
        $source | Should -Match 'max_consecutive_status_failures'
        $source | Should -Match '\$cleanupWarnings\.Add\("leave_verification_error:'
        $source | Should -Match '\$cleanupPassed\s*=\s*\$remaining\.Count\s*-eq\s*0'
    }

    It 'contains no dispatch of older grouping actions' {
        $source = Get-Content -LiteralPath $script:ProofPath -Raw
        $source | Should -Not -Match '(?i)-Action\s+(party|rally)\b'
        $source | Should -Match '\[ValidateRange\(1, 2\)\]\[int\]\$PollSeconds'
        $source | Should -Match '\[ValidateRange\(1, 35\)\]\[int\]\$MaxWaitMinutes'
        $source | Should -Match '\$instanceSamples\.ToArray\(\)'
        $source | Should -Not -Match 'Get-Wsg10v10GameplayVerdict\s+-Samples\s+@\(\$instanceSamples\)'
    }
}

Describe 'WSG role-preserving fixture-status preflight' {
    It 'accepts twenty online eligible level-80 rare initialized members with varied existing specs' {
        $rows = New-WsgExistingFixturePreflightRows
        $result = Test-Wsg10v10ExistingFixturePreflight -OrderedRosterGuid $script:Roster -EvidenceRows $rows `
            -ExpectedLevel 80 -ExpectedBracket 7 -ExpectedQuality 3 -ExpectedQualityName rare

        $result.status | Should -Be 'PASS'
        $result.mode | Should -Be 'skip_fixture_init_preflight'
        $result.spec_policy | Should -Be 'preserve_existing'
        $result.expected_quality | Should -Be 3
        $result.evidence_count | Should -Be 20
        $result.online_eligible_count | Should -Be 20
        $result.initialized_count | Should -Be 20
        @($result.members.spec_index | Sort-Object -Unique).Count | Should -BeGreaterThan 1
        @($result.reasons).Count | Should -Be 0
    }

    It 'fails closed for offline or ineligible members, wrong level, missing spec, non-rare gear, or an unoccupied slot' {
        $rows = New-WsgExistingFixturePreflightRows
        $rows[0].online = $false
        $rows[1].fixture_eligible = $false
        $rows[2].level = 79
        $rows[3].spec_index = -1
        $rows[3].spec_name = 'unknown'
        $rows[4].expected_quality_slots = 14
        $rows[5].equipped_slots = 15
        $result = Test-Wsg10v10ExistingFixturePreflight -OrderedRosterGuid $script:Roster -EvidenceRows $rows `
            -ExpectedLevel 80 -ExpectedBracket 7 -ExpectedQuality 3 -ExpectedQualityName rare

        $result.status | Should -Be 'FAIL'
        $result.reasons | Should -Contain 'online_or_eligible_mismatch:1'
        $result.reasons | Should -Contain 'online_or_eligible_mismatch:2'
        $result.reasons | Should -Contain 'level_mismatch:3'
        $result.reasons | Should -Contain 'uninitialized_or_missing_spec:4'
        $result.reasons | Should -Contain 'gear_quality_mismatch:rare'
    }

    It 'allows one empty slot and one rare-quality fallback but rejects two of either' {
        $oneFallback = New-WsgExistingFixturePreflightRows
        $oneFallback[0].equipped_slots = 15
        $oneFallback[0].expected_quality_slots = 14
        $oneResult = Test-Wsg10v10ExistingFixturePreflight -OrderedRosterGuid $script:Roster -EvidenceRows $oneFallback `
            -ExpectedLevel 80 -ExpectedBracket 7 -ExpectedQuality 3 -ExpectedQualityName rare
        $oneResult.status | Should -Be 'PASS'

        $twoFallback = New-WsgExistingFixturePreflightRows
        $twoFallback[0].equipped_slots = 14
        $twoFallback[0].expected_quality_slots = 14
        $twoFallback[1].expected_quality_slots = 14
        $twoResult = Test-Wsg10v10ExistingFixturePreflight -OrderedRosterGuid $script:Roster -EvidenceRows $twoFallback `
            -ExpectedLevel 80 -ExpectedBracket 7 -ExpectedQuality 3 -ExpectedQualityName rare
        $twoResult.status | Should -Be 'FAIL'
        $twoResult.reasons | Should -Contain 'gear_quality_mismatch:rare'
    }
}

Describe 'WSG 10v10 instance and gameplay evidence classification' {
    It 'accepts only one nonzero instance with the exact ordered 10+10 faction roster' {
        $status = ConvertTo-Wsg10v10CanonicalStatus -Response (New-WsgStatusSample)
        $result = Test-Wsg10v10InstanceStatus -Status $status -OrderedRosterGuid $script:Roster
        $result.status | Should -Be 'PASS'
        $result.instance_id | Should -Be 77

        $status.roster[19].faction = 'Alliance'
        (Test-Wsg10v10InstanceStatus -Status $status -OrderedRosterGuid $script:Roster).status | Should -Be 'FAIL'
    }

    It 'reports gameplay PASS only with combat, flag, terminal score, and winner evidence' {
        $first = ConvertTo-Wsg10v10CanonicalStatus -Response (New-WsgStatusSample)
        $middle = ConvertTo-Wsg10v10CanonicalStatus -Response (New-WsgStatusSample -Kills 2 -Deaths 2 -Damage 2000 -Healing 500 -AllianceFlagState 'carried' -HordeFlagState 'base')
        $last = ConvertTo-Wsg10v10CanonicalStatus -Response (New-WsgStatusSample -Kills 3 -Deaths 3 -Damage 3500 -Healing 900 -Captures 1 -Returns 1 -AllianceScore 1 -State 'completed' -Winner 'Alliance')
        $result = Get-Wsg10v10GameplayVerdict -Samples @($first, $middle, $last) -InstanceRosterMaintained $true
        $result.status | Should -Be 'PASS'
        $result.deltas.kills | Should -Be 3
        $result.deltas.deaths | Should -Be 3
        $result.deltas.damage | Should -Be 3500
        $result.deltas.healing | Should -Be 900
        $result.deltas.flag_state_changes | Should -BeGreaterThan 0
        $result.deltas.flag_captures | Should -Be 1
        $result.deltas.flag_returns | Should -Be 1
        $result.final_score.alliance | Should -Be 1
        $result.winner | Should -Be 'Alliance'
    }

    It 'keeps queue success distinct when gameplay telemetry is absent' {
        $first = ConvertTo-Wsg10v10CanonicalStatus -Response (New-WsgStatusSample)
        $last = ConvertTo-Wsg10v10CanonicalStatus -Response (New-WsgStatusSample -State 'completed' -AllianceScore 1 -Winner 'Alliance')
        (Test-Wsg10v10InstanceStatus -Status $first -OrderedRosterGuid $script:Roster).status | Should -Be 'PASS'
        $gameplay = Get-Wsg10v10GameplayVerdict -Samples @($first, $last) -InstanceRosterMaintained $true
        $gameplay.status | Should -Be 'FAIL'
        $gameplay.reasons | Should -Contain 'no_kill_or_death_delta_observed'
        $gameplay.reasons | Should -Contain 'no_flag_state_capture_or_return_delta_observed'
    }

    It 'classifies real combat and flag play as PARTIAL when the timebox ends before a winner' {
        $first = ConvertTo-Wsg10v10CanonicalStatus -Response (New-WsgStatusSample)
        $last = ConvertTo-Wsg10v10CanonicalStatus -Response (New-WsgStatusSample -Kills 4 -Deaths 4 -Damage 50000 -Healing 3000 -Captures 1 -Returns 1 -AllianceScore 1)
        $gameplay = Get-Wsg10v10GameplayVerdict -Samples @($first, $last) -InstanceRosterMaintained $true
        $gameplay.status | Should -Be 'PARTIAL'
        $gameplay.reasons | Should -Contain 'final_state_is_not_terminal:in_progress'
        $gameplay.deltas.damage | Should -Be 50000
        $gameplay.deltas.healing | Should -Be 3000
    }
}

Describe 'WSG proof PowerShell syntax' {
    It 'parse-checks the control, proof, library, and focused test scripts' {
        foreach ($path in @($script:ControlPath, $script:ProofPath, $script:ProofLibraryPath, $PSCommandPath)) {
            $tokens = $null
            $errors = $null
            [System.Management.Automation.Language.Parser]::ParseFile($path, [ref]$tokens, [ref]$errors) | Out-Null
            @($errors).Count | Should -Be 0 -Because $path
        }
    }
}
