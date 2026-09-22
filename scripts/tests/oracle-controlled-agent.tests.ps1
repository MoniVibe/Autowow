BeforeAll {
    $script:Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
    . (Join-Path $script:Root 'scripts\oracle-controlled-agent-lib.ps1')
    $script:CatalogPath = Join-Path $script:Root 'contracts\oracle-agent-contracts.v1.json'
    $script:Catalog = Get-OracleControlledAgentCatalog -Path $script:CatalogPath
}

Describe 'Oracle controlled-agent catalog' {
    It 'validates the shipped catalog' {
        $result = Test-OracleControlledAgentCatalog -Catalog $script:Catalog
        $result.valid | Should -BeTrue
        $result.errors | Should -BeNullOrEmpty
        $result.contract_count | Should -Be 6
        $result.doctrine_count | Should -Be 4
    }

    It 'keeps every doctrine independent and voluntary' {
        foreach ($doctrine in @($script:Catalog.doctrines)) {
            $doctrine.requires_leader | Should -BeFalse
            $doctrine.cooperation | Should -Be 'voluntary'
        }
    }

    It 'requires the controlled rule order on every contract' {
        foreach ($contract in @($script:Catalog.contracts)) {
            $contract.controlled_rule | Should -Match '^WHEN .+ DO .+ VERIFY .+ ON_FAILURE .+ THEN .+$'
            $contract.action.operation | Should -Not -Match 'random|generic|nearby'
        }
    }

    It 'does not claim craft execution before a runtime verb exists' {
        $craft = @($script:Catalog.contracts | Where-Object id -eq 'craft.exact_recipe.v1')
        $craft.Count | Should -Be 1
        $craft[0].status | Should -Be 'planned'
        $craft[0].failure.code | Should -Be 'craft:execution_unavailable'
    }

    It 'uses live proof sources for quest completion and reward' {
        $quest = @($script:Catalog.contracts | Where-Object id -eq 'quest.npc_objective.v1')[0]
        $turnIn = @($script:Catalog.contracts | Where-Object id -eq 'quest.turn_in.v1')[0]
        (@($quest.evidence.live_sources) -contains 'questobjective') | Should -BeTrue
        (@($turnIn.evidence.live_sources) -contains 'acceptance.reward.reward_status') | Should -BeTrue
    }

    It 'rejects an invented action verb' {
        $copy = $script:Catalog | ConvertTo-Json -Depth 20 | ConvertFrom-Json
        $copy.contracts[0].action.verb = 'improvise'
        $result = Test-OracleControlledAgentCatalog -Catalog $copy
        $result.valid | Should -BeFalse
        (@($result.errors) -join "`n") | Should -Match 'invalid action verb'
    }

    It 'rejects a leader-dependent doctrine' {
        $copy = $script:Catalog | ConvertTo-Json -Depth 20 | ConvertFrom-Json
        $copy.doctrines[0].requires_leader = $true
        $result = Test-OracleControlledAgentCatalog -Catalog $copy
        $result.valid | Should -BeFalse
        (@($result.errors) -join "`n") | Should -Match 'requires a leader'
    }
}
