Describe 'q786 GameObject interaction fixture contract' {
    BeforeAll {
        $scriptPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'interact-fixture-q786.ps1'
        $source = Get-Content -LiteralPath $scriptPath -Raw
        $reducerPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'interact-fixture-q786.reducer.ps1'
        . $reducerPath
        $bridgePath = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) '_phase1_worktree\mod-playerbots\src\AutoWow\AutoWowBridge.cpp'
        $bridgeSource = Get-Content -LiteralPath $bridgePath -Raw
        $moduleRoot = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) '_phase1_worktree\mod-playerbots'
        $actionSource = Get-Content -LiteralPath (Join-Path $moduleRoot 'src\Ai\World\Rpg\Action\NewRpgAction.cpp') -Raw
        $contextSource = Get-Content -LiteralPath (Join-Path $moduleRoot 'src\Ai\World\Rpg\QuestObjectiveContext.h') -Raw
        $viewSource = Get-Content -LiteralPath (Join-Path $moduleRoot 'src\AutoWow\QuestLogView.cpp') -Raw
        $fixtureManifestPath = Join-Path $TestDrive 'q786-fixture-manifest.json'
        $fixtureManifest = [pscustomobject][ordered]@{
            schema = 'autowow.q786.fixture-provision.manifest.v1'
            status = 'READY_FOR_Q786_FIXTURE_INIT'
            quest = [pscustomobject]@{ id = 786 }
            owner_guid = 9001
            helper_guid = 9002
            freshness = [pscustomobject]@{
                level_one_before_fixture_init = $true
                inventory_rows = 0
                quest_log_rows = 0
                rewarded_quest_rows = 0
            }
            members = @(
                [pscustomobject]@{ role = 'owner'; account_name = 'AWQ786OWNER'; character_name = 'Kolkarown'; character_guid = 9001 },
                [pscustomobject]@{ role = 'helper'; account_name = 'AWQ786HELPER'; character_name = 'Kolkarhelp'; character_guid = 9002 }
            )
        }
        [System.IO.File]::WriteAllText($fixtureManifestPath, ($fixtureManifest | ConvertTo-Json -Depth 12), [System.Text.UTF8Encoding]::new($false))
        $plan = (& $scriptPath -FixtureManifestPath $fixtureManifestPath | Out-String) | ConvertFrom-Json -Depth 100

        function New-Q786TestReceipt {
            param(
                [uint64]$Sequence,
                [uint32]$Slot,
                [uint32]$Entry,
                [uint64]$Guid,
                [uint32]$Before = 0,
                [uint32]$After = 1,
                [uint32]$QuestId = 786
            )
            return [pscustomobject][ordered]@{
                sequence = $Sequence
                quest_id = $QuestId
                objective_slot = $Slot
                entry = $Entry
                guid = $Guid
                before = $Before
                after = $After
            }
        }
    }

    It 'parses cleanly' {
        $errors = $null
        [void][System.Management.Automation.Language.Parser]::ParseFile($scriptPath, [ref]$null, [ref]$errors)
        $errors.Count | Should -Be 0
    }

    It 'is a side-effect-free dry-run by default with exact fixture GUIDs' {
        $plan.applies | Should -BeFalse
        $plan.apply_ready | Should -BeTrue
        $plan.owner_guid | Should -Be 9001
        $plan.helper_guid | Should -Be 9002
        @($plan.fixture_guids) | Should -Be @(9001, 9002)
        $plan.fixture_manifest | Should -Be $fixtureManifestPath
        $plan.direct_quest_or_character_database_writes | Should -Be 0
        $plan.evidence | Should -Match '[\\/]logs[\\/]'
    }

    It 'declares the exact authoritative q786 and GameObject contract' {
        $plan.quest | Should -Be 786
        $plan.title | Should -Be 'Thwarting Kolkar Aggression'
        $plan.quest_giver.entry | Should -Be 3140
        $plan.quest_giver.spawn_guid | Should -Be 4699
        @($plan.quest_objectives).Count | Should -Be 3
        @($plan.quest_objectives.entry) | Should -Be @(3189, 3190, 3192)
        @($plan.quest_objectives.signed_entry) | Should -Be @(-3189, -3190, -3192)
        @($plan.quest_objectives.required | Select-Object -Unique) | Should -Be @(1)
        @($plan.required_native_go_proof_entries) | Should -Be @(3189, 3190)
        $plan.native_gameobject_credit_path | Should -Be 'CMSG_GAMEOBJ_USE'
        $plan.max_turnin_directives | Should -Be 1
        $plan.post_turnin_requests_read_only | Should -BeTrue
    }

    It 'uses the exact bounded q786 staging route' {
        $plan.stage.route | Should -Be 'q786-lar-prowltusk'
        $plan.stage.available | Should -BeTrue
        $plan.blocked_by | Should -BeNullOrEmpty
        $bridgeSource | Should -Match '\{ "q786-lar-prowltusk", 1, -776\.0f, -4830\.36f, 19\.8326f, 0\.0f \}'
        $bridgeSource | Should -Match 'selectNearbyStarter\(3140, 786, 4699\)'
        $source | Should -Match 'guard intentionally precedes every dot-source, bridge request'
    }

    It 'limits mutations to the exact normal bridge surfaces and requests' {
        @($plan.mutation_surfaces) | Should -Be @('activate', 'fixture-init', 'party', 'route', 'quest')
        @($plan.mutation_allowlist) | Should -Be @(
            'activate 9001',
            'activate 9002',
            'fixture init 9001 8 0 2',
            'fixture init 9002 7 0 2',
            'party 9001 9002',
            'route 9001 q786-lar-prowltusk',
            'quest 9001',
            'quest 9001 786'
        )
        $source | Should -Match "ValidateSet\('list', 'activate', 'fixture-init', 'fixture-status', 'party', 'route', 'quest'\)"
        $source | Should -Not -Match "ValidateSet\([^\r\n]*'deactivate'"
        $source | Should -Not -Match "ValidateSet\([^\r\n]*'travel'"
        $source | Should -Not -Match "ValidateSet\([^\r\n]*'recover'"
    }

    It 'contains no direct database mutation surface or language' {
        $source | Should -Not -Match '(?i)Invoke-FixtureSql|mysql(?:\.exe)?|CharacterDatabaseInfo|WorldDatabaseInfo|PlayerbotsDatabaseInfo'
        $source | Should -Not -Match '(?im)^\s*(INSERT|UPDATE|DELETE|REPLACE|ALTER|CREATE|DROP|TRUNCATE)(?:\s+|$)'
        $source | Should -Not -Match '(?i)character_queststatus|character_queststatus_rewarded'
    }

    It 'requires native per-objective telemetry and zero forbidden deltas' {
        $source | Should -Match 'questobjective'
        $source | Should -Match 'direct_go_receipts'
        $source | Should -Match 'Update-Q786DirectGoProof'
        $source | Should -Match 'Test-Q786DirectGoProofComplete'
        $source | Should -Match 'unrelated_offensive_target_count'
        $source | Should -Match 'random_grind_fallback_count'
        $source | Should -Match 'teleport_count'
        $source | Should -Match 'Get-UnrelatedObjectiveProgress'
        $source | Should -Match 'reward_status'
    }

    It 'captures the direct GO receipt at the opcode call and immediately rebases' {
        $actionSource | Should -Match '(?s)uint32 const before.*WorldPacket use\(CMSG_GAMEOBJ_USE, 8\).*HandleGameObjectUseOpcode\(use\).*uint32 const after.*RecordDirectGameObjectReceipt'
        $actionSource | Should -Match 'RebaseAfterDirectGameObjectCredit'
        $actionSource | Should -Match 'GetValue<QuestObjectiveSpec>\("active quest objective"\)->Reset\(\)'
        $actionSource | Should -Match 'GetValue<QuestFinisherRef>\("active quest finisher"\)->Reset\(\)'
        $contextSource | Should -Match 'uint64 sequence = 0'
        $contextSource | Should -Match 'uint32 before = 0'
        $contextSource | Should -Match 'uint32 after = 0'
    }

    It 'exposes completed directive receipts and live finisher telemetry read-only' {
        $viewSource | Should -Match 'directive_active'
        $viewSource | Should -Match 'directive_quest_id'
        $viewSource | Should -Match 'direct_go_receipts'
        $viewSource | Should -Match 'finisher_live'
        $viewSource | Should -Match 'directiveActive \? directiveQuestId : spec\.key\.questId'
    }

    It 'classifies selected reward storage failure as inventory full' {
        $actionSource | Should -Match 'BestRewardIndex\(quest\)'
        $actionSource | Should -Match 'CanRewardQuest\(quest, rt\.selectedRewardIndex, false\)'
        $contextSource | Should -Match 'return QuestFailureReason::InventoryFull'
    }

    It 'issues at most one turn-in directive and then keeps polling read-only' {
        $source | Should -Match '-not \$directiveActive -and -not \$turnInDirectiveIssued'
        $source | Should -Match '(?s)\$turnInDirectiveIssued = \$true\s+\$turnInDirectiveCount = 1\s+\$turnIn = Invoke-Q786Control -Action quest'
        $source | Should -Match 'Completed q786 observed while directive \$directiveQuestId is active; refusing to replace it'
        ([regex]::Matches($source, "quest_turnin_directive_issued")).Count | Should -Be 1
    }

    It 'reduces cumulative durable receipts once and proves both required entries' {
        $state = New-Q786DirectGoProofState -BaselineSequence 10
        $first = New-Q786TestReceipt -Sequence 11 -Slot 0 -Entry 3189 -Guid 12388
        $second = New-Q786TestReceipt -Sequence 12 -Slot 1 -Entry 3190 -Guid 12389

        Update-Q786DirectGoProof -State $state -Receipts @($first)
        Update-Q786DirectGoProof -State $state -Receipts @($first)
        $state.receipt_count | Should -Be 1
        Test-Q786DirectGoProofComplete -State $state | Should -BeFalse

        Update-Q786DirectGoProof -State $state -Receipts @($first, $second)
        $view = Get-Q786DirectGoProofView -State $state
        $view.complete | Should -BeTrue
        $view.last_sequence | Should -Be 12
        $view.receipt_count | Should -Be 2
        $view.entries['3189'].sequence | Should -Be 11
        $view.entries['3189'].before | Should -Be 0
        $view.entries['3189'].after | Should -Be 1
        $view.entries['3190'].guid | Should -Be 12389
    }

    It 'fails closed on nonmonotonic or mismatched receipt identity' {
        $state = New-Q786DirectGoProofState
        $later = New-Q786TestReceipt -Sequence 2 -Slot 1 -Entry 3190 -Guid 12389
        $earlier = New-Q786TestReceipt -Sequence 1 -Slot 0 -Entry 3189 -Guid 12388
        { Update-Q786DirectGoProof -State $state -Receipts @($later, $earlier) } |
            Should -Throw '*not strictly increasing*'

        $wrongEntry = New-Q786TestReceipt -Sequence 1 -Slot 0 -Entry 3190 -Guid 12388
        { Update-Q786DirectGoProof -State (New-Q786DirectGoProofState) -Receipts @($wrongEntry) } |
            Should -Throw '*expected entry 3189*'

        $durable = New-Q786DirectGoProofState
        Update-Q786DirectGoProof -State $durable -Receipts @(
            (New-Q786TestReceipt -Sequence 1 -Slot 0 -Entry 3189 -Guid 12388)
        )
        { Update-Q786DirectGoProof -State $durable -Receipts @() } |
            Should -Throw '*ledger disappeared after sequence 1*'
    }

    It 'retries only exact fixture initialization across transient login combat' {
        $source | Should -Match 'function\s+Invoke-Q786FixtureInitWithRetry'
        $source | Should -Match '\$attempt\s*=\s*1;\s*\$attempt\s*-le\s*10'
        $source | Should -Match 'Invoke-Q786Control\s+-Action\s+fixture-init\s+-Guid\s+\$Guid\s+-Level\s+\$Level'
    }
}
