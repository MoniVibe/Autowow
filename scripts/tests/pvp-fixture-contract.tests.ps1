Set-StrictMode -Version Latest

BeforeAll {
    $scriptsRoot = Split-Path -Parent $PSScriptRoot
    $fixtureLibrary = Join-Path $scriptsRoot 'pvp-fixture-lib.ps1'
    . $fixtureLibrary
    $fixtureDefinition = Get-PvpFixtureDefinition -FixtureId 'awpvp1' -Level 80
    $fixturePlan = Get-PvpFixturePlan -Definition $fixtureDefinition
}

Describe 'AutoWoW disposable PvP fixture contracts' {
    It 'parse-tests every uniquely named pvp-fixture script' {
        $files = @(Get-ChildItem -LiteralPath $scriptsRoot -Filter 'pvp-fixture-*.ps1' -File)
        $files.Count | Should -BeGreaterThan 0
        foreach ($file in $files) {
            $tokens = $null
            $errors = $null
            [System.Management.Automation.Language.Parser]::ParseFile($file.FullName, [ref]$tokens, [ref]$errors) | Out-Null
            @($errors).Count | Should -Be 0 -Because $file.FullName
        }
    }

    It 'defines twenty equal-level characters across exactly ten Alliance and ten Horde slots' {
        @($fixtureDefinition.accounts).Count | Should -Be 20
        @($fixtureDefinition.accounts | Where-Object faction -eq 'Alliance').Count | Should -Be 10
        @($fixtureDefinition.accounts | Where-Object faction -eq 'Horde').Count | Should -Be 10
        @($fixtureDefinition.accounts | Select-Object -ExpandProperty level -Unique).Count | Should -Be 1
        $fixtureDefinition.accounts.level | Select-Object -Unique | Should -Be 80
        @($fixtureDefinition.accounts | Where-Object { $_.account_name -notmatch '^[A-Z][A-Z0-9]{2,15}$' }).Count | Should -Be 0
        @($fixtureDefinition.accounts | Where-Object { $_.character_name -notmatch '^[A-Z][a-z]{2,11}$' }).Count | Should -Be 0
    }

    It 'keeps the required stage order and fails closed for unsupported ladders' {
        @($fixtureDefinition.stages | Sort-Object ordinal | Select-Object -ExpandProperty stage_id) | Should -Be @('world-pvp-2v2', 'ladder-3v3', 'wsg-10v10')
        $fixtureDefinition.stages[0].players_per_side | Should -Be 2
        $fixtureDefinition.stages[1].players_per_side | Should -Be 3
        $fixtureDefinition.stages[2].players_per_side | Should -Be 10
        $fixtureDefinition.stages[0].supported_start | Should -BeTrue
        $fixtureDefinition.stages[1].supported_start | Should -BeFalse
        $fixtureDefinition.stages[2].supported_start | Should -BeTrue
        $fixtureDefinition.stages[2].supported_stop | Should -BeTrue
        $fixtureDefinition.stages[2].requires_party | Should -BeFalse
        $fixtureDefinition.stages[2].requires_rally | Should -BeFalse
        $fixtureDefinition.stages[1].start_surface | Should -Match 'blocked'
        $fixtureDefinition.stages[2].start_surface | Should -Match 'atomic ordered 20-GUID'
    }

    It 'uses only the local bridge allowlist in the plan' {
        $safeActions = Get-PvpFixtureSafeBridgeActions
        $safeActions | Should -Contain 'activate'
        $safeActions | Should -Contain 'party'
        $safeActions | Should -Contain 'rally'
        $safeActions | Should -Contain 'route'
        $safeActions | Should -Contain 'engage'
        $safeActions | Should -Contain 'wsg-queue'
        $safeActions | Should -Contain 'wsg-status'
        $safeActions | Should -Contain 'wsg-leave'
        $safeActions | Should -Not -Contain 'deploy'
        $fixturePlan.local_only | Should -BeTrue
        @($fixturePlan.bridge_commands).Count | Should -Be 5
        @($fixturePlan.bridge_commands | Where-Object { $_.unsupported_surface -match 'blocked' }).Count | Should -Be 2
        $wsgPlan = @($fixturePlan.bridge_commands | Where-Object stage_id -eq 'wsg-10v10')
        $wsgPlan.Count | Should -Be 1
        $wsgPlan[0].side | Should -Be 'both'
        $wsgPlan[0].atomic_roster_count | Should -Be 20
        @($wsgPlan[0].activate).Count | Should -Be 20
        @($wsgPlan[0].party_members).Count | Should -Be 0
        $wsgPlan[0].start | Should -Be 'wsg-queue'
        $wsgPlan[0].status | Should -Be 'wsg-status'
        $wsgPlan[0].stop | Should -Contain 'wsg-leave'
        $worldPlan = @($fixturePlan.bridge_commands | Where-Object stage_id -eq 'world-pvp-2v2')
        @($worldPlan).Count | Should -Be 2
        @($worldPlan | Where-Object rally_route -ne 'valley-of-trials').Count | Should -Be 0
    }

    It 'keeps plan account commands secret-free and explicitly non-executing' {
        @($fixturePlan.account_console_commands).Count | Should -Be 20
        @($fixturePlan.account_console_commands | Where-Object { $_.command -notmatch '^\.account create [A-Z0-9]+ <WOW_ACCOUNT_PASSWORD>$' }).Count | Should -Be 0
        @($fixturePlan.account_console_commands | Where-Object { $_.would_execute -ne $true }).Count | Should -Be 0
        ($fixturePlan | ConvertTo-Json -Depth 20 -Compress) | Should -Not -Match '(?i)secret|password\s*[:=]\s*[^<]'
    }

    It 'generates exact-scope character, membership, active, and cleanup SQL' {
        $guidMap = @{}
        $guid = 1001
        foreach ($member in $fixtureDefinition.accounts) {
            $guidMap[$member.character_name] = $guid
            $guid++
        }
        $characterSql = New-PvpFixtureCharacterInsertSql -CharactersDatabaseName 'acore_characters' -AccountId 42 -Member $fixtureDefinition.accounts[0]
        $characterSql | Should -Match 'NOT EXISTS'
        $characterSql | Should -Match 'Awalliaa'
        $characterSql | Should -Not -Match '(?i)DROP|TRUNCATE|DELETE FROM'

        $membershipSql = New-PvpFixtureTeamMembershipSql -PlayerbotsDatabaseName 'acore_playerbots' -Definition $fixtureDefinition -CharacterGuidByName $guidMap
        $membershipSql | Should -Match 'autowow_league_team'
        $membershipSql | Should -Match 'autowow_league_member'
        $membershipSql | Should -Match 'roster_cap,worker_cap,treasury_copper\) VALUES \([^\r\n]+,10,0,0\)'
        $membershipSql | Should -Match 'roster_cap=VALUES\(roster_cap\)'
        $membershipSql | Should -Not -Match '(?i)characters|DROP|TRUNCATE'

        $activeSql = New-PvpFixtureActiveMembershipSql -PlayerbotsDatabaseName 'acore_playerbots' -CharacterGuids @(1001, 1002) -Active $true
        $activeSql | Should -Match 'character_guid IN \(1001,1002\)'
        $activeSql | Should -Not -Match 'awpvp%'
        $scopedActiveSql = New-PvpFixtureActiveMembershipSql -PlayerbotsDatabaseName 'acore_playerbots' -CharacterGuids @(1001, 1002) -Active $true -TeamIds @('awpvp1-a', 'awpvp1-h')
        $scopedActiveSql | Should -Match "team_id IN \('awpvp1-a','awpvp1-h'\)"

        $cleanupSql = New-PvpFixtureCleanupSql -PlayerbotsDatabaseName 'acore_playerbots' -CharactersDatabaseName 'acore_characters' -CharacterGuids @($guidMap.Values) -TeamIds @('awpvp1-a', 'awpvp1-h')
        @($cleanupSql -split "`n" | Where-Object { $_ -match '^DELETE FROM' }).Count | Should -Be 3
        $cleanupSql | Should -Match 'DELETE FROM.*characters WHERE guid IN'
        $cleanupSql | Should -Not -Match '(?i)DROP|TRUNCATE|DELETE FROM .*characters\s*;'
    }

    It 'publishes exact acceptance telemetry fields and local-only invariants' {
        $contract = Get-PvpFixtureAcceptanceContract -Definition $fixtureDefinition
        $contract.schema | Should -Be 'autowow.pvp.fixture.acceptance.v1'
        $contract.invariants.local_only | Should -BeTrue
        $contract.invariants.explicit_execute_switch | Should -BeTrue
        $contract.invariants.dry_run_default | Should -BeTrue
        $contract.invariants.expected_level | Should -Be 80
        $contract.invariants.expected_account_count | Should -Be 20
        $contract.invariants.expected_alliance_count | Should -Be 10
        $contract.invariants.expected_horde_count | Should -Be 10
        $contract.invariants.wsg_bracket | Should -Be '10v10'
        $contract.invariants.wsg_atomic_roster_guid_count | Should -Be 20
        $contract.invariants.wsg_no_grouping | Should -BeTrue
        $contract.invariants.world_pvp_start_proof | Should -Be 'opposing_target_guid_observed >= 1'
        $contract.invariants.world_pvp_stop_proof | Should -Match 'online_stage_bots = 0'
        $contract.telemetry_fields | Should -Contain 'group_members'
        $contract.telemetry_fields | Should -Contain 'group_leader_guid'
        $contract.telemetry_fields | Should -Contain 'opposing_target_observed'
        $contract.telemetry_fields | Should -Contain 'instance_id'
        $contract.telemetry_fields | Should -Contain 'kills'
        $contract.telemetry_fields | Should -Contain 'deaths'
        $contract.telemetry_fields | Should -Contain 'flag_state'
        $contract.telemetry_fields | Should -Contain 'flag_captures'
        $contract.telemetry_fields | Should -Contain 'flag_returns'
        $contract.telemetry_fields | Should -Contain 'winner'
        $contract.aggregate_fields | Should -Contain 'unrelated_mutation_count'
    }

    It 'rejects fixture ids that could collide with unsafe or oversized names' {
        { Get-PvpFixtureDefinition -FixtureId 'bad-id' } | Should -Throw
        { Get-PvpFixtureDefinition -FixtureId 'A12345678' } | Should -Throw
    }

    It 'preserves tabular MySQL rows and the 3.3.5 account password limit' {
        $driver = Get-Content -LiteralPath (Join-Path $scriptsRoot 'pvp-fixture-world-pvp.ps1') -Raw
        $driver | Should -Match '\[regex\]::Split\(\$line, "`t"\)'
        $driver | Should -Not -Match '\$line\s+-split\s+"`t",\s*-1'
        $driver | Should -Match '\$password\.Length\s+-gt\s+16'
        $driver | Should -Match '3-16 characters'
        $driver | Should -Match 'Ensure-PvpFixtureExactWsgTemplate'
        $driver | Should -Match 'MinPlayersPerTeam=10 WHERE ID=2 AND MaxPlayersPerTeam=10'
    }
}
