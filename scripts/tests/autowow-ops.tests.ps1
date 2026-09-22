Describe 'AutoWoW operator console safety contract' {
    BeforeAll {
        $script:Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
        $script:Ops = Join-Path $script:Root 'scripts\autowow-ops.ps1'
    }

    It 'defaults to a non-mutating plan' {
        $plan = (& $script:Ops | Out-String) | ConvertFrom-Json
        $plan.schema | Should -Be 'autowow.ops.plan.v1'
        $plan.default_safe | Should -BeTrue
        $plan.guarded_mutations | Should -Contain 'Raiders'
        $plan.guarded_mutations | Should -Contain 'QuestersStart'
        $plan.persistent_questers.playerbots_log | Should -Match 'logs\\phase1-runtime\\Playerbots.log$'
    }

    It 'requires Apply for every runtime-changing action' {
        foreach ($action in 'Raiders','QuestersStart','QuestersStop','DevCycle') {
            { & $script:Ops -Action $action } | Should -Throw '*requires -Apply*'
        }
    }

    It 'rejects Apply on read-only actions before invoking dependencies' {
        foreach ($action in 'Status','Dashboard','QuestersStatus','FailureBundle') {
            { & $script:Ops -Action $action -Apply } | Should -Throw '*not valid for read-only action*'
        }
    }

    It 'declares the active Phase 1 Playerbots log on the quest director' {
        $source = Get-Content (Join-Path $script:Root 'scripts\league-simulation-director.ps1') -Raw
        $source | Should -Match '\[string\]\$PlayerbotsLogPath'
        $source | Should -Match 'logs\\phase1-runtime\\Playerbots.log'
        $source | Should -Match 'playerbots_log_path = \$script:PlayerbotsLogPath'
    }

    It 'starts quest parties without parking unrelated raid fleets' {
        $opsSource = Get-Content $script:Ops -Raw
        $leagueSource = Get-Content (Join-Path $script:Root 'scripts\league-simulation.ps1') -Raw
        $opsSource | Should -Match '-PreserveOtherFleets'
        $leagueSource | Should -Match '\[switch\]\$PreserveOtherFleets'
        $leagueSource | Should -Match 'unrelated_fleet_preserved'
        $leagueSource | Should -Match "team -in @\('northstar','ember'\)"
        $opsSource | Should -Match "'-LeaderCsv'"
        $opsSource | Should -Match ([regex]::Escape('$LeaderGuid -join '','''))
        $directorSource = Get-Content (Join-Path $script:Root 'scripts\league-simulation-director.ps1') -Raw
        $directorSource | Should -Match '\[string\]\$LeaderCsv'
        $directorSource | Should -Match '\$LeaderCsv -split '','''
    }

    It 're-arms independent leaders with a bounded live verification after restart' {
        $leagueSource = Get-Content (Join-Path $script:Root 'scripts\league-simulation.ps1') -Raw
        $leagueSource | Should -Match 'function Invoke-IndependentQuestDispatch'
        $leagueSource | Should -Match 'for \(\$attempt = 1; \$attempt -le 3; \$attempt\+\+\)'
        $leagueSource | Should -Match '\$hasPermanentFollow'
        $leagueSource | Should -Match 'retained the permanent follow strategy after three bounded quest dispatches'
        $leagueSource | Should -Match "party_independence_rearmed"
    }
}
