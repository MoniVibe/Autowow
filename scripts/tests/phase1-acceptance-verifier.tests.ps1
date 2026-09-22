<#
    Pester v5 offline tests for phase1-acceptance-verifier.ps1.
    These tests write only under Pester's TestDrive.  They do not contact MySQL, the bridge,
    a server process, or a live client.

    Run:
      Invoke-Pester -Path .\scripts\tests\phase1-acceptance-verifier.tests.ps1
#>

BeforeAll {
    $script:VerifierPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'phase1-acceptance-verifier.ps1'
    $script:AutoWoWRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
    $script:Utf8NoBom = [System.Text.UTF8Encoding]::new($false)

    function New-TestReceipt {
        param(
            [int]$QuestId = 459,
            [int]$SelectedSource = 1988,
            [int]$Finisher = 1992,
            [int]$BaselineCount = 0,
            [int]$CurrentCount = 1,
            [int]$RequiredCount = 1,
            [int]$UnrelatedTargets = 0,
            [int]$RandomGrindFallbacks = 0,
            [bool]$TeleportUsed = $false,
            [int]$DirectDbMutations = 0,
            [bool]$RewardConfirmed = $true,
            [bool]$ContextPresent = $true,
            [bool]$Supported = $true,
            [string]$Schema = 'autowow.phase1.acceptance.evidence.v1',
            [int]$SchemaVersion = 1,
            [switch]$IncludeSchema
        )

        $facts = @{
            459 = @{ family = 'item'; kind = 'collect_item'; item = 3297; entry = 0; sources = @(1988, 1989); finisher = 1992 }
            789 = @{ family = 'item'; kind = 'collect_item'; item = 4862; entry = 0; sources = @(3124, 3281); finisher = 3143 }
            792 = @{ family = 'npc_or_gameobject'; kind = 'creature_credit'; item = 0; entry = 3101; sources = @(3101); finisher = 3145 }
            916 = @{ family = 'item'; kind = 'collect_item'; item = 5166; entry = 0; sources = @(1986); finisher = 2082 }
        }
        $fact = $facts[$QuestId]
        if ($null -eq $fact) { throw "No test fact for quest $QuestId" }

        $record = [ordered]@{
            schema = $Schema
            schema_version = $SchemaVersion
            event = 'phase1_acceptance_snapshot'
            scope = 'quest'
            bot_guid = 700 + $QuestId
            quest_id = $QuestId
            objective = [ordered]@{
                quest_id = $QuestId
                objective_family = $fact.family
                objective_slot = 0
                objective_kind = $fact.kind
                phase = 'verify_reward'
                failure_reason = 'none'
                supported = $Supported
                required_npc_or_go_entry = $fact.entry
                required_item_id = $fact.item
                current_count = $CurrentCount
                required_count = $RequiredCount
                baseline_count = $BaselineCount
                selected_source_entry = $SelectedSource
                selected_target_guid = 90000 + $QuestId
                finisher_entry = $Finisher
                finisher_guid = 91000 + $QuestId
                has_lock = $true
            }
            allowed_source_entries = @($fact.sources)
            exact_finisher_match = ($Finisher -eq $fact.finisher)
            reward_postcondition_confirmed = $RewardConfirmed
            objective_context_present = $ContextPresent
            unrelated_offensive_target_count = $UnrelatedTargets
            random_grind_fallback_count = $RandomGrindFallbacks
            teleport_used = $TeleportUsed
            direct_quest_db_mutation_count = $DirectDbMutations
        }
        if (-not $IncludeSchema) {
            $record.Remove('schema')
            $record.Remove('schema_version')
        }
        return [pscustomobject]$record
    }

    # Matches the nested names emitted by phase1-acceptance-evidence.ps1, rather than the verifier's
    # historical bridge-shaped aliases. Keeping this separate catches schema drift at the harness boundary.
    function New-HarnessShapeReceipt {
        param([Parameter(Mandatory = $true)][int]$QuestId)

        $facts = @{
            459 = @{ source = 1988; finisher = 1992; required = 1 }
            789 = @{ source = 3124; finisher = 3143; required = 1 }
            792 = @{ source = 3101; finisher = 3145; required = 8 }
            916 = @{ source = 1986; finisher = 2082; required = 1 }
        }
        $fact = $facts[$QuestId]
        if ($null -eq $fact) { throw "No harness-shape fact for quest $QuestId" }
        $canonical = New-TestReceipt -QuestId $QuestId -SelectedSource $fact.source -Finisher $fact.finisher `
            -RequiredCount $fact.required -IncludeSchema
        $objective = $canonical.objective
        return [pscustomobject][ordered]@{
            schema = 'autowow.phase1.acceptance.evidence.v1'
            schema_version = 1
            event = 'phase1_acceptance_fixture_sample'
            kind = 'fixture_sample'
            bot_guid = $canonical.bot_guid
            quest_id = $QuestId
            objective_context_present = $true
            objective = [ordered]@{
                family = $objective.objective_family
                slot = $objective.objective_slot
                kind = $objective.objective_kind
                phase = $objective.phase
                failure_reason = $objective.failure_reason
                supported = $objective.supported
                required_entry = $objective.required_npc_or_go_entry
                required_item = $objective.required_item_id
                baseline_count = $objective.baseline_count
                current_count = $objective.current_count
                required_count = $objective.required_count
            }
            selected_source_entry = $objective.selected_source_entry
            selected_target_guid = $objective.selected_target_guid
            allowed_source_entries = @($canonical.allowed_source_entries)
            finisher = [ordered]@{ entry = $objective.finisher_entry; guid = $objective.finisher_guid; relation_verified = $true }
            reward = [ordered]@{ turnin_verified = $true }
            exact_finisher_match = $true
            invariants = [ordered]@{
                unrelated_offensive_target_count = 0
                random_grind_fallback_count = 0
                teleport_count = 0
                direct_quest_db_mutation_count = 0
            }
        }
    }

    function Write-TestJsonl {
        param([string]$Path, [object[]]$Records)
        $lines = @($Records | ForEach-Object { $_ | ConvertTo-Json -Depth 20 -Compress })
        [System.IO.File]::WriteAllText($Path, (($lines -join [Environment]::NewLine) + [Environment]::NewLine), $script:Utf8NoBom)
    }

    function Invoke-TestVerifier {
        param(
            [string]$Name,
            [string]$Evidence = '',
            [string]$Bridge = '',
            [switch]$Offline,
            [switch]$Live
        )
        $json = Join-Path $TestDrive "$Name.json"
        $markdown = Join-Path $TestDrive "$Name.md"
        & $script:VerifierPath -ServerRoot $script:AutoWoWRoot -EvidencePath $Evidence -BridgeSnapshotPath $Bridge `
            -JsonReportPath $json -MarkdownReportPath $markdown -OfflineFixture:$Offline -LiveSnapshot:$Live `
            -BotGuid 42 | Out-Null
        return [pscustomobject]@{
            json_path = $json
            markdown_path = $markdown
            report = (Get-Content -LiteralPath $json -Raw | ConvertFrom-Json -Depth 30)
            markdown = Get-Content -LiteralPath $markdown -Raw
        }
    }
}

Describe 'Phase 1 acceptance verifier' {
    It 'passes all four explicit offline fixtures' {
        $run = Invoke-TestVerifier -Name 'offline-pass' -Offline

        $run.report.status | Should -Be 'PASS'
        $run.report.deterministic | Should -BeTrue
        @($run.report.fixtures).Count | Should -Be 4
        @($run.report.fixtures | Where-Object { $_.status -ne 'PASS' }).Count | Should -Be 0
        $run.report.schema.schema_gaps | Should -Be 0
        $run.markdown | Should -Match 'q459'
        $run.markdown | Should -Match 'q789'
        $run.markdown | Should -Match 'q792'
        $run.markdown | Should -Match 'q916'
    }

    It 'accepts all four fixture samples emitted in the harness JSON shape' {
        $input = Join-Path $TestDrive 'harness-shape.jsonl'
        Write-TestJsonl -Path $input -Records @(
            (New-HarnessShapeReceipt -QuestId 459),
            (New-HarnessShapeReceipt -QuestId 789),
            (New-HarnessShapeReceipt -QuestId 792),
            (New-HarnessShapeReceipt -QuestId 916)
        )

        $run = Invoke-TestVerifier -Name 'harness-shape' -Evidence $input
        $run.report.status | Should -Be 'PASS'
        @($run.report.fixtures | Where-Object { $_.status -ne 'PASS' }).Count | Should -Be 0
    }

    It 'is byte deterministic when the inputs and output paths are unchanged' {
        $json = Join-Path $TestDrive 'deterministic.json'
        $markdown = Join-Path $TestDrive 'deterministic.md'
        & $script:VerifierPath -ServerRoot $script:AutoWoWRoot -OfflineFixture -JsonReportPath $json -MarkdownReportPath $markdown | Out-Null
        $firstJson = Get-Content -LiteralPath $json -Raw
        $firstMarkdown = Get-Content -LiteralPath $markdown -Raw
        & $script:VerifierPath -ServerRoot $script:AutoWoWRoot -OfflineFixture -JsonReportPath $json -MarkdownReportPath $markdown | Out-Null
        (Get-Content -LiteralPath $json -Raw) | Should -Be $firstJson
        (Get-Content -LiteralPath $markdown -Raw) | Should -Be $firstMarkdown
    }

    It 'returns INCONCLUSIVE when required evidence is missing' {
        $input = Join-Path $TestDrive 'missing-required.jsonl'
        $record = [pscustomobject][ordered]@{
            schema = 'autowow.phase1.acceptance.evidence.v1'
            schema_version = 1
            event = 'phase1_acceptance_started'
            quest_id = 459
        }
        Write-TestJsonl -Path $input -Records @($record)
        $run = Invoke-TestVerifier -Name 'missing-required' -Evidence $input

        $run.report.status | Should -Be 'INCONCLUSIVE'
        $q459 = @($run.report.fixtures | Where-Object id -eq 'q459')[0]
        @($q459.checks | Where-Object name -eq 'required_item_exact')[0].status | Should -Be 'NOT_OBSERVED'
        @($q459.release_invariants | Where-Object name -eq 'reward_postcondition_confirmed')[0].status | Should -Be 'NOT_OBSERVED'
        @($run.report.diagnostics | Where-Object code -eq 'input_schema_unversioned').Count | Should -Be 0
    }

    It 'returns FAIL for an explicit wrong source even when other evidence is present' {
        $input = Join-Path $TestDrive 'wrong-source.jsonl'
        Write-TestJsonl -Path $input -Records @(New-TestReceipt -QuestId 459 -SelectedSource 999)
        $run = Invoke-TestVerifier -Name 'wrong-source' -Evidence $input

        $run.report.status | Should -Be 'FAIL'
        $q459 = @($run.report.fixtures | Where-Object id -eq 'q459')[0]
        @($q459.checks | Where-Object name -eq 'selected_source_is_allowed')[0].status | Should -Be 'FAIL'
    }

    It 'distinguishes a release invariant failure from missing telemetry' {
        $input = Join-Path $TestDrive 'invariant-failure.jsonl'
        Write-TestJsonl -Path $input -Records @(New-TestReceipt -QuestId 792 -SelectedSource 3101 -RequiredCount 8 -CurrentCount 8 -UnrelatedTargets 1)
        $run = Invoke-TestVerifier -Name 'invariant-failure' -Evidence $input

        $run.report.status | Should -Be 'FAIL'
        $q792 = @($run.report.fixtures | Where-Object id -eq 'q792')[0]
        @($q792.release_invariants | Where-Object name -eq 'unrelated_offensive_targets_zero')[0].status | Should -Be 'FAIL'
        @($q792.release_invariants | Where-Object name -eq 'random_grind_fallbacks_zero')[0].status | Should -Be 'PASS'
    }

    It 'accepts the saved unversioned bridge shape but refuses to claim release PASS' {
        $bridge = Join-Path $TestDrive 'questobjective.json'
        $fixture = New-TestReceipt -QuestId 459 -IncludeSchema
        $objective = $fixture.objective
        $rawBridge = [pscustomobject][ordered]@{
            ok = $true
            guid = 459
            objective = $objective
        }
        [System.IO.File]::WriteAllText($bridge, ($rawBridge | ConvertTo-Json -Depth 20 -Compress), $script:Utf8NoBom)
        $run = Invoke-TestVerifier -Name 'bridge-saved' -Bridge $bridge

        $run.report.status | Should -Be 'INCONCLUSIVE'
        $run.report.schema.schema_gaps | Should -Be 1
        @($run.report.diagnostics | Where-Object code -eq 'bridge_schema_unversioned').Count | Should -Be 1
        $q459 = @($run.report.fixtures | Where-Object id -eq 'q459')[0]
        @($q459.checks | Where-Object name -eq 'required_item_exact')[0].status | Should -Be 'PASS'
        @($q459.release_invariants | Where-Object name -eq 'reward_postcondition_confirmed')[0].status | Should -Be 'NOT_OBSERVED'
    }

    It 'rejects unsupported evidence schema versions without using their fields' {
        $input = Join-Path $TestDrive 'unsupported-schema.jsonl'
        Write-TestJsonl -Path $input -Records @(New-TestReceipt -QuestId 459 -Schema 'autowow.phase1.acceptance.evidence.v99' -IncludeSchema)
        $run = Invoke-TestVerifier -Name 'unsupported-schema' -Evidence $input

        $run.report.status | Should -Be 'INCONCLUSIVE'
        $run.report.schema.records_rejected | Should -Be 1
        @($run.report.diagnostics | Where-Object code -eq 'input_schema_unsupported').Count | Should -Be 1
        $q459 = @($run.report.fixtures | Where-Object id -eq 'q459')[0]
        @($q459.checks | Where-Object name -eq 'required_item_exact')[0].status | Should -Be 'NOT_OBSERVED'
    }

    It 'keeps LiveSnapshot dry-run by default and issues no control command' {
        $run = Invoke-TestVerifier -Name 'live-dry-run' -Live

        $run.report.status | Should -Be 'INCONCLUSIVE'
        $run.report.live_snapshot.requested | Should -BeTrue
        $run.report.live_snapshot.executed | Should -BeFalse
        $run.report.live_snapshot.dry_run | Should -BeTrue
        @($run.report.live_snapshot.control_commands_issued).Count | Should -Be 0
        @($run.report.diagnostics | Where-Object code -eq 'live_snapshot_dry_run').Count | Should -Be 1
    }
}
