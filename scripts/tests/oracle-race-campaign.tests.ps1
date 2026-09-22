BeforeAll {
    $script:Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
    . (Join-Path $script:Root 'scripts\oracle-race-campaign-lib.ps1')
}

Describe 'AutoWow Oracle race campaign definitions' {
    It 'contains exactly one seed for every playable Wrath race' {
        $definitions = @(Get-OracleRaceSeedDefinitions)
        $definitions.Count | Should -Be 10
        @($definitions | Select-Object -ExpandProperty race_id -Unique).Count | Should -Be 10
        @($definitions | Select-Object -ExpandProperty guid -Unique).Count | Should -Be 10
    }

    It 'uses valid deterministic race/class identities and profession pairs' {
        Assert-OracleRaceSeedDefinitions | Should -BeTrue
        foreach ($definition in @(Get-OracleRaceSeedDefinitions)) {
            $definition.guid | Should -BeGreaterThan 0
            $definition.race_id | Should -BeGreaterThan 0
            $definition.class_id | Should -BeGreaterThan 0
            $definition.profession_one | Should -Not -Be $definition.profession_two
        }
    }

    It 'keeps the runtime allowlist deterministic by GUID' {
        @(Get-OracleRaceSeedGuidList) | Should -Be @(101,112,121,123,139,144,154,166,236,244)
    }
}

Describe 'AutoWow Oracle failure keys' {
    It 'normalizes a failure without allowing secret-like punctuation or whitespace' {
        ConvertTo-OracleFailureCode -Domain 'Progression' -Code 'No Progress / XP' | Should -BeExactly 'progression:no_progress_xp'
    }

    It 'does not collapse different domains into one key' {
        (ConvertTo-OracleFailureCode -Domain 'quest' -Code 'blocked') | Should -Not -Be (ConvertTo-OracleFailureCode -Domain 'craft' -Code 'blocked')
    }
}

Describe 'AutoWow independent seed control' {
    It 'builds the explicit native-independent bridge request' {
        $control = Join-Path $script:Root 'scripts\autowow-control.ps1'
        (& $control -Action independent -BotGuid 101 -EmitRequestOnly) | Should -BeExactly 'independent 101'
    }
}

Describe 'AutoWow profession telemetry control' {
    It 'builds the read-only profession economy request' {
        $control = Join-Path $script:Root 'scripts\autowow-control.ps1'
        (& $control -Action professioneconomy -BotGuid 101 -EmitRequestOnly) | Should -BeExactly 'professioneconomy 101'
    }
}
