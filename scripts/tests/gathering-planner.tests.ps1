Describe 'gathering-planner.ps1' {
    BeforeAll {
        $planner = Join-Path $PSScriptRoot '..\gathering-planner.ps1'
        $script:PlannerFixturePath = Join-Path $TestDrive 'gathering-roster.json'
        $script:PlannerTelemetryPath = Join-Path $TestDrive 'gathering-skills.json'

        $members = [System.Collections.Generic.List[object]]::new()
        $slotPairs = @(
            @('Herbalism', 'Alchemy'),
            @('Mining', 'Engineering'),
            @('Skinning', 'Leatherworking'),
            @('Herbalism', 'Inscription'),
            @('Mining', 'Jewelcrafting'),
            @('Mining', 'Blacksmithing'),
            @('Tailoring', 'Enchanting')
        )
        foreach ($team in @('northstar', 'ember')) {
            for ($index = 0; $index -lt $slotPairs.Count; $index++) {
                $guid = if ($team -eq 'northstar') { 100 + $index + 1 } else { 200 + $index + 1 }
                $one = $null
                $two = $null
                if ($team -eq 'northstar') {
                    $one = $slotPairs[$index][0]
                    $two = $slotPairs[$index][1]
                } elseif ($index -eq 0) {
                    $one = 'Alchemy'
                    $two = 'Mining'
                }
                [void]$members.Add([ordered]@{
                    guid = $guid
                    team = $team
                    role = 'worker'
                    active = $true
                    name = "${team}-worker-$($index + 1)"
                    profession_one = $one
                    profession_two = $two
                })
            }
        }

        [ordered]@{ members = @($members) } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $script:PlannerFixturePath -Encoding utf8
        [ordered]@{
            schema_version = 'gathering-telemetry-v0'
            skills = @(
                [ordered]@{ guid = 101; skill_id = 182; value = 5; max = 75 }
                [ordered]@{ guid = 101; skill_id = 171; value = 5; max = 75 }
            )
        } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $script:PlannerTelemetryPath -Encoding utf8
    }

    It 'assigns dependency-valid slots and gives each side a complete seven-slot primary plan' {
        $plan = (& $planner -RosterPath $script:PlannerFixturePath -WorkersPerTeam 7 -AsJson | Out-String) | ConvertFrom-Json

        @($plan.teams).Count | Should -Be 2
        foreach ($team in @($plan.teams)) {
            @($team.assignments).Count | Should -Be 7
            @($team.coverage.missing_from_plan).Count | Should -Be 0
            @($team.coverage.missing_worker_coverage).Count | Should -Be 0
            @($team.assignments | Where-Object { -not $_.planned_pair_valid }).Count | Should -Be 0
        }

        @($plan.teams[1].assignments | Where-Object { $_.status -eq 'invalid_current_pair_change_proposed' }).Count | Should -BeGreaterThan 0
        $invalidPair = $plan.teams[1].assignments | Where-Object { $_.current_profession_one -eq 'Alchemy' -and $_.current_profession_two -eq 'Mining' } | Select-Object -First 1
        $invalidPair.current_profession_pair_valid | Should -BeFalse
        $invalidPair.blocking_reasons | Should -Contain 'current_pair_invalid'
    }

    It 'requires observed character skill ranks before marking a worker live-ready' {
        $plan = (& $planner -RosterPath $script:PlannerFixturePath -TelemetryPath $script:PlannerTelemetryPath -WorkersPerTeam 1 -AsJson | Out-String) | ConvertFrom-Json

        $northstarAssignment = $plan.teams | Where-Object team -eq 'northstar' | ForEach-Object assignments | Select-Object -First 1
        $emberAssignment = $plan.teams | Where-Object team -eq 'ember' | ForEach-Object assignments | Select-Object -First 1
        $northstarAssignment.live_ready | Should -BeTrue
        $northstarAssignment.observed_profession_pair_valid | Should -BeTrue
        $emberAssignment.live_ready | Should -BeFalse
        $emberAssignment.blocking_reasons | Should -Contain 'skill_rank_telemetry_required'
    }
}
