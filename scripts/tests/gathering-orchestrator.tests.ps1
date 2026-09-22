Describe 'gathering-orchestrator.ps1' {
    BeforeAll {
        $orchestrator = Join-Path $PSScriptRoot '..\gathering-orchestrator.ps1'
        $script:OrchestratorPlanPath = Join-Path $TestDrive 'gathering-plan.json'
        $fakeDirectory = Join-Path $TestDrive 'fake-bridge'
        [void](New-Item -ItemType Directory -Path $fakeDirectory -Force)
        $script:FakeBridgePath = Join-Path $fakeDirectory 'autowow-control-fake.ps1'
        $script:FakeCallLog = Join-Path $fakeDirectory 'calls.log'

        [ordered]@{
            schema_version = 'gathering-progression-v0'
            teams = @(
                [ordered]@{
                    team = 'northstar'
                    name = 'Northstar Alliance'
                    assignments = @(
                        [ordered]@{
                            slot = 1
                            slot_key = 'herbalism-alchemy'
                            guid = 101
                            name = 'NorthstarWorker'
                            role = 'worker'
                            candidate_type = 'worker'
                            planned_profession_one = 'Herbalism'
                            planned_profession_two = 'Alchemy'
                            planned_pair_valid = $true
                            live_ready = $true
                            status = 'ready'
                            blocking_reasons = @()
                        }
                        [ordered]@{
                            slot = 2
                            slot_key = 'mining-engineering'
                            guid = $null
                            name = ''
                            role = ''
                            candidate_type = 'unbound'
                            planned_profession_one = 'Mining'
                            planned_profession_two = 'Engineering'
                            planned_pair_valid = $true
                            live_ready = $false
                            status = 'unbound'
                            blocking_reasons = @('no_guild_member_candidate')
                        }
                    )
                }
                [ordered]@{
                    team = 'ember'
                    name = 'Ember Horde'
                    assignments = @(
                        [ordered]@{
                            slot = 1
                            slot_key = 'mining-engineering'
                            guid = 202
                            name = 'EmberCandidate'
                            role = 'adventurer'
                            candidate_type = 'roster_candidate'
                            planned_profession_one = 'Mining'
                            planned_profession_two = 'Engineering'
                            planned_pair_valid = $true
                            live_ready = $false
                            status = 'worker_role_required'
                            blocking_reasons = @('role_is_not_worker')
                        }
                    )
                }
            )
        } | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $script:OrchestratorPlanPath -Encoding utf8

        @'
param(
    [string]$Action,
    [long]$BotGuid
)
$logPath = Join-Path $PSScriptRoot 'calls.log'
Add-Content -LiteralPath $logPath -Value ("{0}|{1}" -f $Action, $BotGuid)
if ($Action -eq 'list') {
    [ordered]@{ ok = $true; bots = @() } | ConvertTo-Json -Compress
    exit 0
}
[ordered]@{
    ok = $true
    order = $Action
    guid = $BotGuid
    mode = if ($Action -eq 'deploy') { 'worker_gather' } else { $null }
} | ConvertTo-Json -Compress
'@ | Set-Content -LiteralPath $script:FakeBridgePath -Encoding utf8
    }

    It 'is dry-run by default and never calls the bridge' {
        $result = (& $orchestrator -PlanPath $script:OrchestratorPlanPath -AsJson | Out-String) | ConvertFrom-Json

        $result.dry_run | Should -BeTrue
        $result.mode | Should -BeExactly 'dry_run'
        $result.safety.bridge_calls | Should -Be 0
        @($result.executions).Count | Should -Be 0
        $ready = $result.planned_actions | Where-Object ready_for_bridge
        @($ready.bridge_steps | Where-Object action -eq 'deploy').Count | Should -Be 1
        Test-Path -LiteralPath $script:FakeCallLog | Should -BeFalse
    }

    It 'requires an explicit confirmation before bridge apply' {
        { & $orchestrator -PlanPath $script:OrchestratorPlanPath -BridgeScriptPath $script:FakeBridgePath -Apply -AsJson } | Should -Throw '*ConfirmLiveBridge*'
    }

    It 'applies only the ready worker through the existing bridge when explicitly confirmed' {
        $result = (& $orchestrator -PlanPath $script:OrchestratorPlanPath -BridgeScriptPath $script:FakeBridgePath -Apply -ConfirmLiveBridge -AsJson | Out-String) | ConvertFrom-Json

        $result.dry_run | Should -BeFalse
        $result.mode | Should -BeExactly 'bridge_apply_explicit'
        $result.safety.db_writes | Should -Be 0
        $result.safety.chat_commands_sent | Should -Be 0
        $result.safety.bridge_calls | Should -Be 4
        @($result.executions | Where-Object { $_.guid -eq 101 }).Count | Should -Be 3
        @($result.executions | Where-Object { $_.guid -eq 202 }).Count | Should -Be 0

        $calls = Get-Content -LiteralPath $script:FakeCallLog
        @($calls | Where-Object { $_ -match '^list\|' }).Count | Should -Be 1
        @($calls | Where-Object { $_ -match '^activate\|101$' }).Count | Should -Be 1
        @($calls | Where-Object { $_ -match '^deploy\|101$' }).Count | Should -Be 1
        @($calls | Where-Object { $_ -match '^snapshot\|101$' }).Count | Should -Be 1
    }
}
