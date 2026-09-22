<#
    Offline Pester coverage for the disposable dungeon fixture lane.
    These tests do not start worldserver, connect to the bridge, call MySQL, or invoke an
    existing AutoWoW provisioner.  The Execute tests intentionally assert the fail-closed guard.

    Run:
      Invoke-Pester -Path .\scripts\tests\dungeon-fixture.tests.ps1 -Output Detailed
#>

BeforeAll {
    $script:ScriptsRoot = Split-Path -Parent $PSScriptRoot
    $script:LibPath = Join-Path $script:ScriptsRoot 'dungeon-fixture-lib.ps1'
    $script:EntryPath = Join-Path $script:ScriptsRoot 'dungeon-fixture-run.ps1'
    $script:TestPath = $PSCommandPath
    . $script:LibPath
    $script:Definition = Get-DungeonFixtureDefinition

    function New-PassingDungeonFixtureEvidence {
        param([Parameter(Mandatory = $true)][psobject]$Definition)

        $members = foreach ($member in @($Definition.members)) {
            [pscustomobject][ordered]@{
                fixture_role = $member.fixture_role
                character_name = $member.character_name
                class_name = $member.class_name
                class_id = $member.class_id
                faction = $member.faction
                role = $member.role
                spec_label = $member.spec_label
                spec_index = $member.spec_index
                level = $member.level
                template_generation = [pscustomobject][ordered]@{
                    provider = 'PlayerbotFactory.Randomize(false)'
                    factory_randomize = $true
                    skills = $true
                    talents = $true
                    gear = $true
                }
                gear = [pscustomobject][ordered]@{
                    completeness_ratio = 1.0
                    generated_by_factory = $true
                    missing_slots = @()
                    invalid_slots = @()
                    slots = @(
                        [pscustomobject][ordered]@{ slot = 'head'; occupied = $true; legitimate_empty = $false; verified = $true },
                        [pscustomobject][ordered]@{ slot = 'mainhand'; occupied = $true; legitimate_empty = $false; verified = $true },
                        [pscustomobject][ordered]@{ slot = 'offhand'; occupied = $false; legitimate_empty = $true; verified = $true }
                    )
                }
                deaths = 0
            }
        }

        $stages = @(
            [pscustomobject][ordered]@{
                stage_id = 'elite-pack'
                completed = $true
                deaths = 0
                tank = [pscustomobject][ordered]@{ victim_ownership_ratio = 0.95; taunt_casts = 2; taunt_confirmed = 2 }
                healer = [pscustomobject][ordered]@{ tank_priority_ratio = 0.80; effective_healing_tank = 1000; effective_healing_total = 1400 }
            },
            [pscustomobject][ordered]@{
                stage_id = 'ingvar'
                completed = $true
                deaths = 0
                tank = [pscustomobject][ordered]@{ victim_ownership_ratio = 0.95; taunt_casts = 3; taunt_confirmed = 3 }
                healer = [pscustomobject][ordered]@{ tank_priority_ratio = 0.75; effective_healing_tank = 2000; effective_healing_total = 2600 }
                boss_completion = [pscustomobject][ordered]@{
                    boss_name = 'Ingvar the Plunderer'
                    completed = $true
                    credited_fixture_roles = @($Definition.members | ForEach-Object fixture_role)
                }
            }
        )

        return [pscustomobject][ordered]@{
            schema = 'autowow.dungeon.fixture.evidence.v1'
            schema_version = 1
            fixture_id = $Definition.fixture_id
            members = @($members)
            provenance = [pscustomobject][ordered]@{
                direct_db_mutations = 0
                combat_result_mutations = 0
                teleports = 0
                gm_combat_commands = 0
            }
            stages = $stages
        }
    }

    function Invoke-DungeonFixturePlanForTest {
        param([Parameter(Mandatory = $true)][string]$Name)

        $receipt = Join-Path $TestDrive "$Name.jsonl"
        $output = Join-Path $TestDrive "$Name.json"
        & $script:EntryPath -Action Plan -ReceiptPath $receipt -OutputPath $output | Out-Null
        return [pscustomobject][ordered]@{
            receipt = $receipt
            output = $output
            document = Get-Content -LiteralPath $output -Raw | ConvertFrom-Json -Depth 50
            receipt_records = @(Get-Content -LiteralPath $receipt | ForEach-Object { $_ | ConvertFrom-Json -Depth 50 })
        }
    }
}

Describe 'Dungeon fixture definition and plan' {
    It 'defines exactly the level-appropriate five-player Utgarde Keep roster' {
        $script:Definition.level | Should -Be 70
        $script:Definition.dungeon.target_id | Should -Be 'utgarde-keep-normal'
        $script:Definition.dungeon.map_id | Should -Be 574
        @($script:Definition.members).Count | Should -Be 5
        @($script:Definition.members | Where-Object { $_.fixture_role -eq 'tank' -and $_.class_name -eq 'Paladin' -and $_.spec_label -eq 'prot pve' }).Count | Should -Be 1
        @($script:Definition.members | Where-Object { $_.fixture_role -eq 'healer' -and $_.class_name -eq 'Priest' -and $_.spec_label -eq 'disc pve' }).Count | Should -Be 1
        @($script:Definition.members | Where-Object { $_.fixture_role -eq 'dps-mage' -and $_.class_name -eq 'Mage' }).Count | Should -Be 1
        @($script:Definition.members | Where-Object { $_.fixture_role -eq 'dps-rogue' -and $_.class_name -eq 'Rogue' }).Count | Should -Be 1
        @($script:Definition.members | Where-Object { $_.fixture_role -eq 'dps-hunter' -and $_.class_name -eq 'Hunter' }).Count | Should -Be 1
        @($script:Definition.members | Select-Object -ExpandProperty character_name -Unique).Count | Should -Be 5
    }

    It 'is deterministic and idempotence-scoped by fixture identity' {
        $first = Get-DungeonFixtureDefinition -FixtureId 'lane7' -Level 70 -GearQuality rare
        $second = Get-DungeonFixtureDefinition -FixtureId 'lane7' -Level 70 -GearQuality rare
        ($first | ConvertTo-Json -Depth 30 -Compress) | Should -Be ($second | ConvertTo-Json -Depth 30 -Compress)
        $first.idempotence.random_selection_allowed | Should -BeFalse
        $first.idempotence.direct_character_sql_allowed | Should -BeFalse
        $first.teardown.ownership_key | Should -Match 'fixture_id'
    }

    It 'creates a dry-run plan with read-only DB probes and explicit blockers' {
        $plan = Get-DungeonFixtureProvisionPlan -Definition $script:Definition
        $plan.dry_run_default | Should -BeTrue
        $plan.local_only | Should -BeTrue
        $plan.direct_database_mutations | Should -BeFalse
        $plan.execute_allowed_now | Should -BeFalse
        @($plan.operations).Count | Should -BeGreaterThan 20
        @($plan.operations | Where-Object { $_.mutation -and -not $_.supported }).Count | Should -BeGreaterThan 0
        @($plan.gap_report | Where-Object severity -eq 'blocker').Count | Should -Be 6
        @($plan.read_only_db_queries | Where-Object { $_.sql -notmatch '^\s*SELECT' }).Count | Should -Be 0
        @($plan.operations | Where-Object { $_.command -match 'DELETE|INSERT|UPDATE|DROP' }).Count | Should -Be 0
    }

    It 'logs every planned command without invoking live surfaces' {
        $run = Invoke-DungeonFixturePlanForTest -Name 'plan-receipt'
        $run.document.action | Should -Be 'Plan'
        @($run.document.plan.operations).Count | Should -Be (@($run.receipt_records | Where-Object event -eq 'command_planned').Count)
        @($run.receipt_records | Where-Object event -eq 'run_completed').Count | Should -Be 1
        ($run.receipt_records | ConvertTo-Json -Depth 30 -Compress) | Should -Not -Match 'WOW_ACCOUNT_PASSWORD value'
        foreach ($path in @($script:LibPath, $script:EntryPath)) {
            $content = Get-Content -LiteralPath $path -Raw
            $content | Should -Not -Match 'TcpClient|Start-Process|mysql\.exe|Invoke-WebRequest'
        }
    }
}

Describe 'Dungeon fixture fail-closed execution and teardown' {
    It 'requires the explicit Execute switch' {
        { & $script:EntryPath -Action Execute -ReceiptPath (Join-Path $TestDrive 'missing-execute.jsonl') } | Should -Throw '*DNG-EXEC-REQUIRES-EXPLICIT*'
    }

    It 'writes a gap report and blocks explicit Execute before any live command' {
        $receipt = Join-Path $TestDrive 'blocked-execute.jsonl'
        $gap = Join-Path $TestDrive 'blocked-execute.gap.json'
        { & $script:EntryPath -Action Execute -Execute -ReceiptPath $receipt -GapReportPath $gap } | Should -Throw '*DNG-EXEC-BLOCKED*'
        Test-Path -LiteralPath $gap -PathType Leaf | Should -BeTrue
        $gapDocument = Get-Content -LiteralPath $gap -Raw | ConvertFrom-Json -Depth 30
        $gapDocument.execute_allowed_now | Should -BeFalse
        @($gapDocument.blockers).Count | Should -Be 6
        @((Get-Content -LiteralPath $receipt | ForEach-Object { $_ | ConvertFrom-Json -Depth 30 }) | Where-Object event -eq 'execution_blocked').Count | Should -Be 1
    }

    It 'exposes teardown as a dry-run and refuses unsafe deletion' {
        $receipt = Join-Path $TestDrive 'teardown.jsonl'
        $output = Join-Path $TestDrive 'teardown.json'
        & $script:EntryPath -Action Teardown -ReceiptPath $receipt -OutputPath $output | Out-Null
        $document = Get-Content -LiteralPath $output -Raw | ConvertFrom-Json -Depth 40
        $document.plan.plan_kind | Should -Be 'teardown'
        $document.plan.preserve_receipts | Should -BeTrue
        $document.plan.execute_allowed_now | Should -BeFalse
        { & $script:EntryPath -Action Teardown -Execute -ReceiptPath (Join-Path $TestDrive 'teardown-blocked.jsonl') } | Should -Throw '*DNG-EXEC-BLOCKED*'
    }
}

Describe 'Dungeon fixture acceptance gates' {
    It 'passes a complete evidence fixture through elite pack and Ingvar in order' {
        $evidence = New-PassingDungeonFixtureEvidence -Definition $script:Definition
        $result = Get-DungeonFixtureGateResults -Evidence $evidence -Definition $script:Definition
        $result.status | Should -Be 'PASS'
        $result.all_gates_passed | Should -BeTrue
        $result.next_stage | Should -Be 'completed'
        @($result.gates | Where-Object { -not $_.passed }).Count | Should -Be 0
    }

    It 'blocks Ingvar when the elite-pack gate fails' {
        $evidence = New-PassingDungeonFixtureEvidence -Definition $script:Definition
        $evidence.stages[0].completed = $false
        $result = Get-DungeonFixtureGateResults -Evidence $evidence -Definition $script:Definition
        $result.status | Should -Be 'FAIL'
        $result.next_stage | Should -Be 'elite-pack'
        $ingvarPrerequisite = @($result.gates | Where-Object gate_id -eq 'ingvar.prerequisite-elite-pack')[0]
        $ingvarPrerequisite.status | Should -Be 'BLOCKED'
        $ingvarPrerequisite.passed | Should -BeFalse
    }

    It 'fails tank ownership, taunt confirmation, healer tank priority, deaths, and boss credit independently' {
        $evidence = New-PassingDungeonFixtureEvidence -Definition $script:Definition
        $evidence.stages[0].tank.victim_ownership_ratio = 0.50
        $evidence.stages[0].tank.taunt_confirmed = 0
        $evidence.stages[0].healer.tank_priority_ratio = 0.10
        $evidence.stages[0].healer.effective_healing_tank = 0
        $evidence.stages[0].deaths = 1
        $evidence.stages[1].boss_completion.completed = $false
        $evidence.stages[1].boss_completion.credited_fixture_roles = @('tank')
        $result = Get-DungeonFixtureGateResults -Evidence $evidence -Definition $script:Definition
        @($result.gates | Where-Object gate_id -eq 'elite-pack.tank-victim-and-taunt' | Where-Object passed).Count | Should -Be 0
        @($result.gates | Where-Object gate_id -eq 'elite-pack.healer-tank-priority' | Where-Object passed).Count | Should -Be 0
        @($result.gates | Where-Object gate_id -eq 'elite-pack.deaths' | Where-Object passed).Count | Should -Be 0
        @($result.gates | Where-Object gate_id -eq 'ingvar.boss-completion' | Where-Object passed).Count | Should -Be 0
    }

    It 'rejects a wrong exact spec and any direct or teleport mutation provenance' {
        $evidence = New-PassingDungeonFixtureEvidence -Definition $script:Definition
        $evidence.members[0].spec_label = 'holy pve'
        $evidence.provenance.direct_db_mutations = 1
        $evidence.provenance.teleports = 1
        $result = Get-DungeonFixtureGateResults -Evidence $evidence -Definition $script:Definition
        @($result.gates | Where-Object gate_id -eq 'readiness.identity.tank' | Where-Object passed).Count | Should -Be 0
        @($result.gates | Where-Object gate_id -eq 'readiness.provenance' | Where-Object passed).Count | Should -Be 0
    }
}

Describe 'Dungeon fixture script syntax' {
    It 'parses every new dungeon-fixture script and this test file' {
        foreach ($path in @($script:LibPath, $script:EntryPath, $script:TestPath)) {
            $tokens = $null
            $errors = $null
            [System.Management.Automation.Language.Parser]::ParseFile((Resolve-Path -LiteralPath $path).Path, [ref]$tokens, [ref]$errors) | Out-Null
            @($errors).Count | Should -Be 0 -Because $path
        }
    }
}
