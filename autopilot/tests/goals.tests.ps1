# AutoWoW Autopilot V1.2 - goal dependency planner tests (fully offline).
# Run: Invoke-Pester -Path .\autopilot\tests\goals.tests.ps1 -Output Detailed
# Pure planning structures under test: no sockets, no state writes, no paths
# needed (the module never touches the filesystem).

BeforeAll {
    $script:AutopilotRoot = Split-Path -Parent $PSScriptRoot
    . (Join-Path $script:AutopilotRoot 'AutopilotLib.ps1')
    . (Join-Path $script:AutopilotRoot 'modules\AutopilotGoals.ps1')

    $script:Guid = [int64]42

    function New-Goal {
        param(
            [Parameter(Mandatory)][string]$Kind,
            [hashtable]$Params = @{}
        )
        $goal = [ordered]@{
            schema         = 'autowow.autopilot.goal.v1'
            schema_version = 1
            kind           = $Kind
        }
        foreach ($key in ($Params.Keys | Sort-Object)) { $goal[$key] = $Params[$key] }
        return $goal
    }

    function Get-NodesOfKind {
        # Emits matching nodes onto the pipeline (no unary comma: every call
        # site wraps the call in @(...), which would otherwise nest the array).
        param($Expansion, [string]$JobKind)
        return @($Expansion.nodes) | Where-Object { [string]$_.jobKind -eq $JobKind }
    }

    $script:DungeonProfile = [ordered]@{
        allowedJobKinds = @('QuestLevel', 'Dungeon', 'Travel')
        economy         = [ordered]@{ allowSpending = $false }
    }
    $script:NoDungeonProfile = [ordered]@{
        allowedJobKinds = @('QuestLevel')
    }
}

Describe 'Goal validation (fail closed)' {
    It 'accepts every well-formed goal kind' {
        $goals = @(
            (New-Goal -Kind 'ReachLevel' -Params @{ level = 30 }),
            (New-Goal -Kind 'LearnProfession' -Params @{ profession = 'Mining' }),
            (New-Goal -Kind 'Stockpile' -Params @{ itemId = 2770; count = 100 }),
            (New-Goal -Kind 'CraftItem' -Params @{ itemId = 2841; count = 5 }),
            (New-Goal -Kind 'RunDungeon' -Params @{ name = 'Deadmines' }),
            (New-Goal -Kind 'RunRaid' -Params @{ name = 'Naxxramas' }),
            (New-Goal -Kind 'RunBattleground' -Params @{ name = 'wsg' })
        )
        foreach ($goal in $goals) {
            $result = Test-AutopilotGoal -Goal $goal
            $result.Errors | Should -BeNullOrEmpty
            $result.Valid | Should -BeTrue
        }
    }

    It 'fails closed on an unknown goal kind' {
        $bad = New-Goal -Kind 'GrindMobs' -Params @{ level = 5 }
        $result = Test-AutopilotGoal -Goal $bad
        $result.Valid | Should -BeFalse
        ($result.Errors -join ';') | Should -Match "unknown goal kind 'GrindMobs'"
        ($result.Errors -join ';') | Should -Match 'fail closed'
        { New-AutopilotGoalPlan -Goals @($bad) -CharacterGuid $script:Guid } | Should -Throw '*unknown goal kind*'
    }

    It 'fails closed on an unknown schema id' {
        $bad = New-Goal -Kind 'ReachLevel' -Params @{ level = 30 }
        $bad.schema = 'autowow.autopilot.goal.v9'
        $result = Test-AutopilotGoal -Goal $bad
        $result.Valid | Should -BeFalse
        ($result.Errors -join ';') | Should -Match 'unknown goal schema'
    }

    It 'fails closed on an unsupported schema_version' {
        $bad = New-Goal -Kind 'ReachLevel' -Params @{ level = 30 }
        $bad.schema_version = 2
        (Test-AutopilotGoal -Goal $bad).Valid | Should -BeFalse
    }

    It 'fails closed on malformed goal JSON' {
        $result = Test-AutopilotGoal -Goal '{ "schema": "autowow.autopilot.goal.v1", not valid json'
        $result.Valid | Should -BeFalse
        ($result.Errors -join ';') | Should -Match 'malformed'
    }

    It 'rejects ReachLevel level outside 2..80' {
        (Test-AutopilotGoal -Goal (New-Goal -Kind 'ReachLevel' -Params @{ level = 1 })).Valid | Should -BeFalse
        (Test-AutopilotGoal -Goal (New-Goal -Kind 'ReachLevel' -Params @{ level = 81 })).Valid | Should -BeFalse
        (Test-AutopilotGoal -Goal (New-Goal -Kind 'ReachLevel' -Params @{ level = 2 })).Valid | Should -BeTrue
        (Test-AutopilotGoal -Goal (New-Goal -Kind 'ReachLevel' -Params @{ level = 80 })).Valid | Should -BeTrue
    }

    It 'rejects a RunBattleground name other than wsg in v1' {
        (Test-AutopilotGoal -Goal (New-Goal -Kind 'RunBattleground' -Params @{ name = 'av' })).Valid | Should -BeFalse
        (Test-AutopilotGoal -Goal (New-Goal -Kind 'RunBattleground' -Params @{ name = 'wsg' })).Valid | Should -BeTrue
    }

    It 'New-AutopilotGoalPlan reports every error across all invalid goals in one throw' {
        $badKind = New-Goal -Kind 'Teleport' -Params @{}
        $badLevel = New-Goal -Kind 'ReachLevel' -Params @{ level = 999 }
        $thrown = $null
        try { $null = New-AutopilotGoalPlan -Goals @($badKind, $badLevel) -CharacterGuid $script:Guid }
        catch { $thrown = $_.Exception.Message }
        $thrown | Should -Not -BeNullOrEmpty
        $thrown | Should -Match 'goal\[0\]'
        $thrown | Should -Match 'goal\[1\]'
        $thrown | Should -Match "unknown goal kind 'Teleport'"
        $thrown | Should -Match 'out of range 2..80'
    }
}

Describe 'Goal expansion' {
    It 'expands ReachLevel to a single QuestLevel node with no edges' {
        $expansion = Expand-AutopilotGoal -Goal (New-Goal -Kind 'ReachLevel' -Params @{ level = 30 }) -GoalIndex 0 -CharacterGuid $script:Guid
        @($expansion.nodes).Count | Should -Be 1
        @($expansion.edges).Count | Should -Be 0
        $node = @($expansion.nodes)[0]
        $node.jobKind | Should -Be 'QuestLevel'
        $node.goalKind | Should -Be 'ReachLevel'
        $node.goalIndex | Should -Be 0
        [int64]$node.spec.partyLeaderGuid | Should -Be 42
        [int]$node.spec.targetLevel | Should -Be 30
        $node.optional | Should -BeFalse
        $node.nodeId | Should -Match '^gn-[0-9a-f]{10}$'
        (@($node.requiredCapabilities) -join ',') | Should -Be ''
    }

    It 'ReachLevel with a dungeon-allowing profile adds the optional Dungeon supplement node' {
        $goal = New-Goal -Kind 'ReachLevel' -Params @{ level = 30 }
        $expansion = Expand-AutopilotGoal -Goal $goal -GoalIndex 0 -CharacterGuid $script:Guid -Profile $script:DungeonProfile
        @($expansion.nodes).Count | Should -Be 2
        $quest = @(Get-NodesOfKind -Expansion $expansion -JobKind 'QuestLevel')[0]
        $dungeon = @(Get-NodesOfKind -Expansion $expansion -JobKind 'Dungeon')[0]
        $dungeon.optional | Should -BeTrue
        $dungeon.spec.name | Should -BeNullOrEmpty
        @($dungeon.spec.roles) | Should -Be @('tank', 'healer', 'dps', 'dps', 'dps')
        $dungeon.notes | Should -Match 'optional XP supplement'
        $dungeon.notes | Should -Match 'concrete dungeon choice'
        @($expansion.edges).Count | Should -Be 1
        @($expansion.edges)[0].from | Should -Be $quest.nodeId
        @($expansion.edges)[0].to | Should -Be $dungeon.nodeId
    }

    It 'ReachLevel without a dungeon-allowing profile stays a single node' {
        $goal = New-Goal -Kind 'ReachLevel' -Params @{ level = 30 }
        $withoutProfile = Expand-AutopilotGoal -Goal $goal -GoalIndex 0 -CharacterGuid $script:Guid
        $withNonDungeonProfile = Expand-AutopilotGoal -Goal $goal -GoalIndex 0 -CharacterGuid $script:Guid -Profile $script:NoDungeonProfile
        @($withoutProfile.nodes).Count | Should -Be 1
        @($withNonDungeonProfile.nodes).Count | Should -Be 1
        @(Get-NodesOfKind -Expansion $withNonDungeonProfile -JobKind 'Dungeon').Count | Should -Be 0
    }

    It 'LearnProfession by name keeps professionSkillId null with the unknown reason recorded' {
        $expansion = Expand-AutopilotGoal -Goal (New-Goal -Kind 'LearnProfession' -Params @{ profession = 'Mining' }) -GoalIndex 0 -CharacterGuid $script:Guid
        @($expansion.nodes).Count | Should -Be 1
        $node = @($expansion.nodes)[0]
        $node.jobKind | Should -Be 'TrainProfession'
        $node.spec.professionSkillId | Should -BeNullOrEmpty
        $node.spec.professionName | Should -Be 'Mining'
        [int]$node.spec.targetSkill | Should -Be 75
        $node.notes | Should -Match 'roster/DBC'
    }

    It 'LearnProfession by numeric skill id resolves professionSkillId and honors targetSkill' {
        $expansion = Expand-AutopilotGoal -Goal (New-Goal -Kind 'LearnProfession' -Params @{ profession = 186; targetSkill = 225 }) -GoalIndex 0 -CharacterGuid $script:Guid
        $node = @($expansion.nodes)[0]
        [int64]$node.spec.professionSkillId | Should -Be 186
        $node.spec.professionName | Should -BeNullOrEmpty
        [int]$node.spec.targetSkill | Should -Be 225
    }

    It 'expands Stockpile to one Gather node with the buy-alternative note' {
        $expansion = Expand-AutopilotGoal -Goal (New-Goal -Kind 'Stockpile' -Params @{ itemId = 2770; count = 100 }) -GoalIndex 0 -CharacterGuid $script:Guid
        @($expansion.nodes).Count | Should -Be 1
        @($expansion.edges).Count | Should -Be 0
        $gather = @($expansion.nodes)[0]
        $gather.jobKind | Should -Be 'Gather'
        [int64]$gather.spec.item.itemId | Should -Be 2770
        [int64]$gather.spec.item.count | Should -Be 100
        [int64]$gather.spec.stopAtCount | Should -Be 100
        $gather.notes | Should -Match 'buy alternative permitted only when profile economy.allowSpending and a Restock executor exists'
        # No Restock node is auto-added.
        @(Get-NodesOfKind -Expansion $expansion -JobKind 'Restock').Count | Should -Be 0
    }

    It 'Gather node requiredCapabilities equal the lib table entry exactly' {
        $expansion = Expand-AutopilotGoal -Goal (New-Goal -Kind 'Stockpile' -Params @{ itemId = 2770; count = 100 }) -GoalIndex 0 -CharacterGuid $script:Guid
        $gather = @($expansion.nodes)[0]
        (@($gather.requiredCapabilities) -join ',') | Should -Be 'oracle.gather_route,oracle.gather_source'
        (@($gather.requiredCapabilities) -join ',') | Should -Be (@($script:AutopilotKindRequiredCapabilities['Gather']) -join ',')
    }

    It 'expands CraftItem to optional Gather and TrainProfession feeding a Craft node' {
        $expansion = Expand-AutopilotGoal -Goal (New-Goal -Kind 'CraftItem' -Params @{ itemId = 2841; count = 5; recipeSpellId = 2660 }) -GoalIndex 0 -CharacterGuid $script:Guid
        @($expansion.nodes).Count | Should -Be 3
        $gather = @(Get-NodesOfKind -Expansion $expansion -JobKind 'Gather')[0]
        $train = @(Get-NodesOfKind -Expansion $expansion -JobKind 'TrainProfession')[0]
        $craft = @(Get-NodesOfKind -Expansion $expansion -JobKind 'Craft')[0]

        $gather.optional | Should -BeTrue
        [int64]$gather.spec.item.itemId | Should -Be 0
        [int64]$gather.spec.stopAtCount | Should -Be 0
        $gather.spec.note | Should -Match 'material list requires recipe facts; unresolved'

        $train.optional | Should -BeTrue
        $train.spec.professionSkillId | Should -BeNullOrEmpty
        $train.notes | Should -Match 'unknown or below the recipe requirement'

        $craft.optional | Should -BeFalse
        [int64]$craft.spec.recipeSpellId | Should -Be 2660
        [int64]$craft.spec.resultItemId | Should -Be 2841
        [int64]$craft.spec.count | Should -Be 5

        $edgePairs = @(@($expansion.edges) | ForEach-Object { '{0}->{1}' -f $_.from, $_.to } | Sort-Object)
        $expected = @(('{0}->{1}' -f $gather.nodeId, $craft.nodeId), ('{0}->{1}' -f $train.nodeId, $craft.nodeId) | Sort-Object)
        $edgePairs | Should -Be $expected
    }

    It 'expands RunDungeon to the exact VendorRepair, Restock, FormParty, Travel, Dungeon chain' {
        $expansion = Expand-AutopilotGoal -Goal (New-Goal -Kind 'RunDungeon' -Params @{ name = 'Deadmines'; minLevel = 17 }) -GoalIndex 0 -CharacterGuid $script:Guid
        @($expansion.nodes | ForEach-Object { [string]$_.jobKind }) | Should -Be @('VendorRepair', 'Restock', 'FormParty', 'Travel', 'Dungeon')
        @($expansion.edges).Count | Should -Be 4
        for ($i = 0; $i -lt 4; $i++) {
            @($expansion.edges)[$i].from | Should -Be (@($expansion.nodes)[$i].nodeId)
            @($expansion.edges)[$i].to | Should -Be (@($expansion.nodes)[$i + 1].nodeId)
        }
        $travel = @(Get-NodesOfKind -Expansion $expansion -JobKind 'Travel')[0]
        $travel.spec.destination | Should -Be 'Deadmines entrance'
        $party = @(Get-NodesOfKind -Expansion $expansion -JobKind 'FormParty')[0]
        @($party.spec.roles) | Should -Be @('tank', 'healer', 'dps', 'dps', 'dps')
        $dungeon = @(Get-NodesOfKind -Expansion $expansion -JobKind 'Dungeon')[0]
        $dungeon.spec.name | Should -Be 'Deadmines'
        $prerequisite = @($dungeon.prerequisites)[0]
        $prerequisite.kind | Should -Be 'min-level'
        [int64]$prerequisite.params.level | Should -Be 17
    }

    It 'RunDungeon min-level prerequisite defaults to 15 when the goal omits minLevel' {
        $expansion = Expand-AutopilotGoal -Goal (New-Goal -Kind 'RunDungeon' -Params @{ name = 'Deadmines' }) -GoalIndex 0 -CharacterGuid $script:Guid
        $dungeon = @(Get-NodesOfKind -Expansion $expansion -JobKind 'Dungeon')[0]
        [int64](@($dungeon.prerequisites)[0].params.level) | Should -Be 15
    }

    It 'expands RunRaid with rosterSize default 10, difficulty 0, min-level 80 and spec.minItemLevel' {
        $expansion = Expand-AutopilotGoal -Goal (New-Goal -Kind 'RunRaid' -Params @{ name = 'Naxxramas'; minItemLevel = 180 }) -GoalIndex 0 -CharacterGuid $script:Guid
        @($expansion.nodes | ForEach-Object { [string]$_.jobKind }) | Should -Be @('VendorRepair', 'Restock', 'FormParty', 'Travel', 'Raid')
        @($expansion.edges).Count | Should -Be 4
        $party = @(Get-NodesOfKind -Expansion $expansion -JobKind 'FormParty')[0]
        @($party.spec.roles) | Should -Be @('flex')
        $party.notes | Should -Match 'raid roster'
        $raid = @(Get-NodesOfKind -Expansion $expansion -JobKind 'Raid')[0]
        $raid.spec.name | Should -Be 'Naxxramas'
        [int64]$raid.spec.rosterSize | Should -Be 10
        [int64]$raid.spec.difficulty | Should -Be 0
        [int64]$raid.spec.minItemLevel | Should -Be 180
        $prerequisite = @($raid.prerequisites)[0]
        $prerequisite.kind | Should -Be 'min-level'
        [int64]$prerequisite.params.level | Should -Be 80
    }

    It 'RunRaid leaves minItemLevel null when unknown rather than guessing' {
        $expansion = Expand-AutopilotGoal -Goal (New-Goal -Kind 'RunRaid' -Params @{ name = 'Naxxramas' }) -GoalIndex 0 -CharacterGuid $script:Guid
        $raid = @(Get-NodesOfKind -Expansion $expansion -JobKind 'Raid')[0]
        $raid.spec.Contains('minItemLevel') | Should -BeTrue
        $raid.spec.minItemLevel | Should -BeNullOrEmpty
    }

    It 'expands RunBattleground to FormParty feeding Battleground with min-level 10' {
        $expansion = Expand-AutopilotGoal -Goal (New-Goal -Kind 'RunBattleground' -Params @{ name = 'wsg' }) -GoalIndex 0 -CharacterGuid $script:Guid
        @($expansion.nodes | ForEach-Object { [string]$_.jobKind }) | Should -Be @('FormParty', 'Battleground')
        $party = @(Get-NodesOfKind -Expansion $expansion -JobKind 'FormParty')[0]
        @($party.spec.roles).Count | Should -Be 10
        $party.notes | Should -Match 'bracket'
        $battleground = @(Get-NodesOfKind -Expansion $expansion -JobKind 'Battleground')[0]
        $battleground.spec.name | Should -Be 'wsg'
        [int64]$battleground.spec.teamSize | Should -Be 10
        $prerequisite = @($battleground.prerequisites)[0]
        $prerequisite.kind | Should -Be 'min-level'
        [int64]$prerequisite.params.level | Should -Be 10
        @($expansion.edges).Count | Should -Be 1
        @($expansion.edges)[0].from | Should -Be $party.nodeId
        @($expansion.edges)[0].to | Should -Be $battleground.nodeId
        (@($battleground.requiredCapabilities) -join ',') | Should -Be (@($script:AutopilotKindRequiredCapabilities['Battleground']) -join ',')
    }

    It 'every node carries its lib-table capabilities and a sanctioned job kind; no forbidden nodes' {
        $goals = @(
            (New-Goal -Kind 'ReachLevel' -Params @{ level = 30 }),
            (New-Goal -Kind 'LearnProfession' -Params @{ profession = 'Mining' }),
            (New-Goal -Kind 'Stockpile' -Params @{ itemId = 2770; count = 100 }),
            (New-Goal -Kind 'CraftItem' -Params @{ itemId = 2841; count = 5 }),
            (New-Goal -Kind 'RunDungeon' -Params @{ name = 'Deadmines' }),
            (New-Goal -Kind 'RunRaid' -Params @{ name = 'Naxxramas' }),
            (New-Goal -Kind 'RunBattleground' -Params @{ name = 'wsg' })
        )
        $plan = New-AutopilotGoalPlan -Goals $goals -CharacterGuid $script:Guid -Profile $script:DungeonProfile
        foreach ($node in @($plan.nodes)) {
            $script:AutopilotJobKinds | Should -Contain ([string]$node.jobKind)
            (@($node.requiredCapabilities) -join ',') | Should -Be (@($script:AutopilotKindRequiredCapabilities[[string]$node.jobKind]) -join ',')
            # No grinding, teleport-travel, item/reward-grant, or legacy-bot fallback nodes.
            @('OpenWorldPvp', 'Idle') | Should -Not -Contain ([string]$node.jobKind)
        }
        foreach ($travelNode in @($plan.nodes | Where-Object { [string]$_.jobKind -eq 'Travel' })) {
            $travelNode.spec.destination | Should -Match 'entrance$'
        }
    }

    It 'Expand-AutopilotGoal refuses an invalid goal outright' {
        $bad = New-Goal -Kind 'ReachLevel' -Params @{ level = 200 }
        { Expand-AutopilotGoal -Goal $bad -GoalIndex 0 -CharacterGuid $script:Guid } | Should -Throw '*Goal rejected*'
    }
}

Describe 'Goal plan determinism and idempotent merging' {
    It 'produces byte-identical plans across two runs (canonical JSON comparison)' {
        $goals = @(
            (New-Goal -Kind 'ReachLevel' -Params @{ level = 40 }),
            (New-Goal -Kind 'RunDungeon' -Params @{ name = 'Deadmines'; minLevel = 17 }),
            (New-Goal -Kind 'CraftItem' -Params @{ itemId = 2841; count = 5 })
        )
        $planA = New-AutopilotGoalPlan -Goals $goals -CharacterGuid $script:Guid -Profile $script:DungeonProfile
        $planB = New-AutopilotGoalPlan -Goals $goals -CharacterGuid $script:Guid -Profile $script:DungeonProfile
        (ConvertTo-AutopilotCanonicalJson $planA) | Should -Be (ConvertTo-AutopilotCanonicalJson $planB)
    }

    It 'nodeIds are stable across independent expansions of the same goal' {
        $goal = New-Goal -Kind 'Stockpile' -Params @{ itemId = 2770; count = 100 }
        $a = Expand-AutopilotGoal -Goal $goal -GoalIndex 3 -CharacterGuid $script:Guid
        $b = Expand-AutopilotGoal -Goal $goal -GoalIndex 3 -CharacterGuid $script:Guid
        @($a.nodes)[0].nodeId | Should -Be (@($b.nodes)[0].nodeId)
        # Different goalIndex or character changes the identity.
        $c = Expand-AutopilotGoal -Goal $goal -GoalIndex 4 -CharacterGuid $script:Guid
        $d = Expand-AutopilotGoal -Goal $goal -GoalIndex 3 -CharacterGuid ([int64]7)
        @($c.nodes)[0].nodeId | Should -Not -Be (@($a.nodes)[0].nodeId)
        @($d.nodes)[0].nodeId | Should -Not -Be (@($a.nodes)[0].nodeId)
    }

    It 'merges duplicate goals idempotently to a single node set' {
        $goal = New-Goal -Kind 'Stockpile' -Params @{ itemId = 2770; count = 100 }
        $single = New-AutopilotGoalPlan -Goals @($goal) -CharacterGuid $script:Guid
        $double = New-AutopilotGoalPlan -Goals @($goal, $goal) -CharacterGuid $script:Guid
        @($double.nodes).Count | Should -Be (@($single.nodes).Count)
        (ConvertTo-AutopilotCanonicalJson $double) | Should -Be (ConvertTo-AutopilotCanonicalJson $single)
    }

    It 'emits the goal-plan document schema envelope' {
        $plan = New-AutopilotGoalPlan -Goals @((New-Goal -Kind 'ReachLevel' -Params @{ level = 10 })) -CharacterGuid $script:Guid
        $plan.schema | Should -Be 'autowow.autopilot.goalplan.v1'
        $plan.schema_version | Should -Be 1
        [int64]$plan.characterGuid | Should -Be 42
        @($plan.order).Count | Should -Be (@($plan.nodes).Count)
    }

    It 'builds an empty plan from an empty goal list' {
        $plan = New-AutopilotGoalPlan -Goals @() -CharacterGuid $script:Guid
        @($plan.nodes).Count | Should -Be 0
        @($plan.edges).Count | Should -Be 0
        @($plan.order).Count | Should -Be 0
    }
}

Describe 'Topological order' {
    It 'orders every dependency before its dependant and is deterministic across runs' {
        $goals = @(
            (New-Goal -Kind 'RunDungeon' -Params @{ name = 'Deadmines' }),
            (New-Goal -Kind 'CraftItem' -Params @{ itemId = 2589; count = 20 }),
            (New-Goal -Kind 'ReachLevel' -Params @{ level = 30 })
        )
        $plan = New-AutopilotGoalPlan -Goals $goals -CharacterGuid $script:Guid -Profile $script:DungeonProfile
        $order = @($plan.order)
        $order.Count | Should -Be (@($plan.nodes).Count)
        foreach ($edge in @($plan.edges)) {
            $fromIndex = [array]::IndexOf($order, [string]$edge.from)
            $toIndex = [array]::IndexOf($order, [string]$edge.to)
            $fromIndex | Should -BeGreaterOrEqual 0
            $toIndex | Should -BeGreaterOrEqual 0
            $fromIndex | Should -BeLessThan $toIndex
        }
        $again = New-AutopilotGoalPlan -Goals $goals -CharacterGuid $script:Guid -Profile $script:DungeonProfile
        @($again.order) | Should -Be $order
    }

    It 'walks the RunDungeon chain strictly VendorRepair, Restock, FormParty, Travel, Dungeon' {
        $plan = New-AutopilotGoalPlan -Goals @((New-Goal -Kind 'RunDungeon' -Params @{ name = 'Deadmines' })) -CharacterGuid $script:Guid
        $kindByNodeId = @{}
        foreach ($node in @($plan.nodes)) { $kindByNodeId[[string]$node.nodeId] = [string]$node.jobKind }
        @($plan.order | ForEach-Object { $kindByNodeId[$_] }) | Should -Be @('VendorRepair', 'Restock', 'FormParty', 'Travel', 'Dungeon')
    }

    It 'processes the ready set in ascending nodeId order for independent nodes' {
        $goals = @(
            (New-Goal -Kind 'Stockpile' -Params @{ itemId = 2770; count = 10 }),
            (New-Goal -Kind 'Stockpile' -Params @{ itemId = 2771; count = 10 }),
            (New-Goal -Kind 'Stockpile' -Params @{ itemId = 2772; count = 10 })
        )
        $plan = New-AutopilotGoalPlan -Goals $goals -CharacterGuid $script:Guid
        @($plan.edges).Count | Should -Be 0
        $sorted = @($plan.order | Sort-Object { [string]$_ })
        @($plan.order) | Should -Be $sorted
    }

    It 'throws a typed cycle error naming the nodeIds when edges form a cycle' {
        $nodes = @(
            [ordered]@{ nodeId = 'gn-aaaaaaaaaa' },
            [ordered]@{ nodeId = 'gn-bbbbbbbbbb' },
            [ordered]@{ nodeId = 'gn-cccccccccc' }
        )
        $edges = @(
            [ordered]@{ from = 'gn-aaaaaaaaaa'; to = 'gn-bbbbbbbbbb' },
            [ordered]@{ from = 'gn-bbbbbbbbbb'; to = 'gn-cccccccccc' },
            [ordered]@{ from = 'gn-cccccccccc'; to = 'gn-aaaaaaaaaa' }
        )
        $act = { Get-AutopilotGoalPlanOrder -Nodes $nodes -Edges $edges }
        $act | Should -Throw '*cycle detected*'
        $message = $null
        try { $null = & $act } catch { $message = $_.Exception.Message }
        $message | Should -Match 'gn-aaaaaaaaaa'
        $message | Should -Match 'gn-bbbbbbbbbb'
        $message | Should -Match 'gn-cccccccccc'
    }

    It 'rejects edges that reference unknown nodes' {
        $nodes = @([ordered]@{ nodeId = 'gn-aaaaaaaaaa' })
        $edges = @([ordered]@{ from = 'gn-aaaaaaaaaa'; to = 'gn-zzzzzzzzzz' })
        { Get-AutopilotGoalPlanOrder -Nodes $nodes -Edges $edges } | Should -Throw '*unknown node*'
    }
}
