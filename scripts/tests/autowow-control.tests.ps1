Describe 'AutoWoW control read-only quest surfaces' {
    BeforeAll { $script:Control = Join-Path (Split-Path -Parent $PSScriptRoot) 'autowow-control.ps1' }

    It 'emits the live objective request' {
        & $script:Control -Action questobjective -BotGuid 7 -EmitRequestOnly | Should -BeExactly 'questobjective 7'
    }

    It 'emits the read-only encounter telemetry request' {
        & $script:Control -Action encounterlog -BotGuid 7 -EmitRequestOnly | Should -BeExactly 'encounterlog 7'
    }

    It 'emits the generic boss approach and status requests' {
        & $script:Control -Action boss -BotGuid 17392 -EmitRequestOnly |
            Should -BeExactly 'boss 17392'
        & $script:Control -Action boss-status -BotGuid 17392 -EmitRequestOnly |
            Should -BeExactly 'boss status 17392'
    }

    It 'emits acceptance with and without an explicit quest id' {
        & $script:Control -Action acceptance -BotGuid 7 -EmitRequestOnly | Should -BeExactly 'acceptance 7'
        & $script:Control -Action acceptance -BotGuid 7 -QuestId 789 -EmitRequestOnly | Should -BeExactly 'acceptance 7 789'
    }

    It 'emits the exact persistent quest-acquisition request' {
        & $script:Control -Action quest-acquire -BotGuid 7 -EmitRequestOnly |
            Should -BeExactly 'quest 7 acquire'
    }

    It 'keeps QuestId closed to unrelated commands' {
        { & $script:Control -Action combatlog -BotGuid 7 -QuestId 789 -EmitRequestOnly } | Should -Throw '*quest or acceptance*'
    }

    It 'emits the exact craft and craft-status requests' {
        & $script:Control -Action craft -BotGuid 101 -RecipeSpellId 2329 -EmitRequestOnly |
            Should -BeExactly 'craft 101 2329'
        & $script:Control -Action craft-status -BotGuid 101 -EmitRequestOnly |
            Should -BeExactly 'craft-status 101'
    }

    It 'keeps crafting parameters fail-closed' {
        { & $script:Control -Action craft -BotGuid 101 -EmitRequestOnly } | Should -Throw '*RecipeSpellId*'
        { & $script:Control -Action craft-status -BotGuid 101 -RecipeSpellId 2329 -EmitRequestOnly } |
            Should -Throw '*valid only with the craft action*'
        { & $script:Control -Action snapshot -BotGuid 101 -RecipeSpellId 2329 -EmitRequestOnly } |
            Should -Throw '*valid only with the craft action*'
    }

    It 'emits an exact bounded guild trade and status request' {
        & $script:Control -Action guild-trade -BotGuid 2 -BuyerGuid 26 -ItemGuid 603762 `
            -ItemEntry 33470 -Quantity 1 -PriceCopper 100 -EmitRequestOnly |
            Should -BeExactly 'guild-trade 2 26 603762 33470 1 100'
        & $script:Control -Action guild-trade-status -BotGuid 2 -EmitRequestOnly |
            Should -BeExactly 'guild-trade-status 2'
    }

    It 'rejects ambiguous or over-budget guild trades before contacting the bridge' {
        { & $script:Control -Action guild-trade -BotGuid 2 -BuyerGuid 2 -ItemGuid 603762 `
            -ItemEntry 33470 -Quantity 1 -PriceCopper 100 -EmitRequestOnly } |
            Should -Throw '*distinct BuyerGuid*'
        { & $script:Control -Action guild-trade -BotGuid 2 -BuyerGuid 26 -ItemGuid 603762 `
            -ItemEntry 33470 -Quantity 1 -PriceCopper 501 -EmitRequestOnly } |
            Should -Throw '*PriceCopper*'
        { & $script:Control -Action snapshot -BotGuid 2 -BuyerGuid 26 -EmitRequestOnly } |
            Should -Throw '*valid only with guild-trade*'
    }
}

Describe 'AutoWoW control probe reset surface' {
    BeforeAll { $script:Control = Join-Path (Split-Path -Parent $PSScriptRoot) 'autowow-control.ps1' }

    It 'emits one exact exterior roster reset request' {
        & $script:Control -Action probe-reset -BotGuid 7 -MemberGuid 8,9 `
            -Destination uk-exterior -ExpectedMapId 574 -ExpectedDifficulty 0 -EmitRequestOnly |
            Should -BeExactly 'probe-reset uk-exterior 574 0 7 8 9'
    }

    It 'rejects interior names, missing target maps, and duplicate roster members' {
        { & $script:Control -Action probe-reset -BotGuid 7 -MemberGuid 8 -Destination uk-entry -ExpectedMapId 574 -EmitRequestOnly } | Should -Throw '*-exterior*'
        { & $script:Control -Action probe-reset -BotGuid 7 -MemberGuid 8 -Destination uk-exterior -EmitRequestOnly } | Should -Throw '*ExpectedMapId*'
        { & $script:Control -Action probe-reset -BotGuid 7 -MemberGuid 7 -Destination uk-exterior -ExpectedMapId 574 -EmitRequestOnly } | Should -Throw '*unique positive roster*'
    }
}
