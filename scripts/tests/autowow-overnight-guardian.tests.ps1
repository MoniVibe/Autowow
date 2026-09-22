Describe 'autowow overnight guardian recovery ordering' {
    BeforeAll {
        $script:GuardianPath = Join-Path $PSScriptRoot '..\autowow-overnight-guardian.ps1'
        $script:Source = Get-Content -LiteralPath $script:GuardianPath -Raw
        $tokens = $null
        $errors = $null
        $script:GuardianAst = [System.Management.Automation.Language.Parser]::ParseFile(
            $script:GuardianPath, [ref]$tokens, [ref]$errors)
        $decisionFunction = $script:GuardianAst.Find({
            param($node)
            $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
                $node.Name -eq 'Test-GathererDeathRecoveryDeployDue'
        }, $true)
        if ($null -eq $decisionFunction) { throw 'Gatherer death recovery decision function was not found.' }
        . ([scriptblock]::Create($decisionFunction.Extent.Text))

        function New-TestGathererBot {
            param(
                [uint32]$Guid = 3,
                [bool]$Alive = $false,
                [bool]$ExplicitWorker = $true
            )
            return [pscustomobject]@{
                guid = $Guid
                alive = $Alive
                gather_route = [pscustomobject]@{ explicit_worker = $ExplicitWorker }
            }
        }
    }

    It 'parses without PowerShell errors' {
        $tokens = $null
        $errors = $null
        [System.Management.Automation.Language.Parser]::ParseFile($script:GuardianPath, [ref]$tokens, [ref]$errors) | Out-Null
        @($errors).Count | Should -Be 0
    }

    It 'repairs a fragmented roster before issuing full-wipe recovery' {
        $repair = $script:Source.IndexOf("-Event 'pre_recovery_party_repair'")
        $recover = $script:Source.IndexOf("-Event 'full_wipe_recovery'")
        $repair | Should -BeGreaterThan -1
        $recover | Should -BeGreaterThan $repair
        $script:Source | Should -Match 'if \(-not \$exactRosterGrouped\)'
    }

    It 'accepts a complete requested roster under a transferred bot leader' {
        $script:Source | Should -Match '\$fullRosterGroupStates\.Count -eq 5'
        $script:Source | Should -Match '\$fullRosterLeaders\.Count -eq 1'
        $script:Source | Should -Match '\$recoveryLeaderGuid = if \(\$exactRosterGrouped\)'
        $script:Source | Should -Match 'Invoke-GuardianControl -Action recover -Guid \$recoveryLeaderGuid'
    }

    It 'uses the largest safe requested subgroup leader to repair a fragmented roster' {
        $script:Source | Should -Match '\$repairLeaderGuid = \[uint32\]\$party\.leader'
        $script:Source | Should -Match '\[uint32\]\$_\.group\.leader_guid -in \$roster'
        $script:Source | Should -Match '\$repairMembers = @\(\$roster \| Where-Object'
        $script:Source | Should -Match 'Invoke-GuardianControl -Action party -Guid \$repairLeaderGuid -Members \$repairMembers'
    }

    It 'contains a bounded recovery refusal receipt instead of aborting the cycle' {
        $script:Source | Should -Match "-Event 'full_wipe_recovery_deferred'"
        $script:Source | Should -Match "-Event 'pre_recovery_party_repair_deferred'"
    }

    It 'never relogs a dead persistent gatherer' {
        $deadBranch = $script:Source.Substring(
            $script:Source.IndexOf('$isDead = -not [bool]$bot.alive'),
            $script:Source.IndexOf('$candidateName =') -
                $script:Source.IndexOf('$isDead = -not [bool]$bot.alive'))

        $deadBranch | Should -Not -Match 'Invoke-GuardianControl -Action deactivate'
        $deadBranch | Should -Not -Match 'Invoke-GuardianControl -Action activate'
        $deadBranch | Should -Match 'Invoke-GuardianControl -Action deploy -Guid \$guid'
        $controlActions = @([regex]::Matches($deadBranch, 'Invoke-GuardianControl -Action (?<action>[a-z]+)') |
            ForEach-Object { $_.Groups['action'].Value } | Sort-Object -Unique)
        $controlActions | Should -Be @('deploy')
        $deadBranch | Should -Match "-Event 'gatherer_dead_recovery_blocked'"
        $deadBranch | Should -Match "reason='normal_death_recovery_only_no_relog'"
        $deadBranch | Should -Match 'forced_relocation_or_relog_issued=0'
    }

    It 'dispatches normal deploy recovery for one explicit dead configured gatherer' {
        $state = @{}
        $generation = [DateTimeOffset]'2026-07-17T10:31:35Z'
        $bot = New-TestGathererBot

        Test-GathererDeathRecoveryDeployDue -Guid 3 -Bot $bot -ConfiguredGatherers @(3,6,49) `
            -RecoveryState $state -DeathGeneration $generation -Now $generation `
            -CooldownSeconds 300 -LiveMutationsReleased $true | Should -BeTrue
        $state.ContainsKey('3') | Should -BeTrue
        $script:Source | Should -Match 'Invoke-GuardianControl -Action deploy -Guid \$guid'
        $script:Source | Should -Match "-Event 'gatherer_death_recovery_started'"
        $script:Source | Should -Match "recovery='ordinary_find_corpse'"
        $script:Source | Should -Match 'result=\$result'
    }

    It 'keeps an alive explicit worker as a no-op and clears its recovery latch' {
        $state = @{ '3' = [ordered]@{ generation_ticks = 1; last_attempt_utc = [DateTimeOffset]'2026-07-17T10:31:35Z' } }
        $now = [DateTimeOffset]'2026-07-17T10:32:00Z'

        Test-GathererDeathRecoveryDeployDue -Guid 3 -Bot (New-TestGathererBot -Alive $true) `
            -ConfiguredGatherers @(3,6,49) -RecoveryState $state -DeathGeneration $now -Now $now `
            -CooldownSeconds 300 -LiveMutationsReleased $true | Should -BeFalse
        $state.ContainsKey('3') | Should -BeFalse
    }

    It 'suppresses repeat dead deploys until the bounded cooldown expires' {
        $state = @{}
        $generation = [DateTimeOffset]'2026-07-17T10:31:35Z'
        $bot = New-TestGathererBot
        $decisions = @(
            Test-GathererDeathRecoveryDeployDue -Guid 3 -Bot $bot -ConfiguredGatherers @(3,6,49) -RecoveryState $state -DeathGeneration $generation -Now $generation -CooldownSeconds 300 -LiveMutationsReleased $true
            Test-GathererDeathRecoveryDeployDue -Guid 3 -Bot $bot -ConfiguredGatherers @(3,6,49) -RecoveryState $state -DeathGeneration $generation -Now $generation.AddSeconds(20) -CooldownSeconds 300 -LiveMutationsReleased $true
            Test-GathererDeathRecoveryDeployDue -Guid 3 -Bot $bot -ConfiguredGatherers @(3,6,49) -RecoveryState $state -DeathGeneration $generation -Now $generation.AddSeconds(299) -CooldownSeconds 300 -LiveMutationsReleased $true
        )

        @($decisions | Where-Object { $_ }).Count | Should -Be 1
        Test-GathererDeathRecoveryDeployDue -Guid 3 -Bot $bot -ConfiguredGatherers @(3,6,49) `
            -RecoveryState $state -DeathGeneration $generation -Now $generation.AddSeconds(300) `
            -CooldownSeconds 300 -LiveMutationsReleased $true | Should -BeTrue
    }

    It 'rearms immediately when the observed death generation changes' {
        $state = @{}
        $firstGeneration = [DateTimeOffset]'2026-07-17T10:31:35Z'
        $nextGeneration = $firstGeneration.AddSeconds(20)
        $bot = New-TestGathererBot

        Test-GathererDeathRecoveryDeployDue -Guid 3 -Bot $bot -ConfiguredGatherers @(3,6,49) -RecoveryState $state -DeathGeneration $firstGeneration -Now $firstGeneration -CooldownSeconds 300 -LiveMutationsReleased $true | Should -BeTrue
        Test-GathererDeathRecoveryDeployDue -Guid 3 -Bot $bot -ConfiguredGatherers @(3,6,49) -RecoveryState $state -DeathGeneration $nextGeneration -Now $nextGeneration -CooldownSeconds 300 -LiveMutationsReleased $true | Should -BeTrue
        [int64]$state['3'].generation_ticks | Should -Be $nextGeneration.ToUniversalTime().Ticks
    }

    It 'fails closed for wrong GUID, mismatched bot, normal worker mode, or unreleased mutations' {
        $generation = [DateTimeOffset]'2026-07-17T10:31:35Z'

        Test-GathererDeathRecoveryDeployDue -Guid 99 -Bot (New-TestGathererBot -Guid 99) -ConfiguredGatherers @(3,6,49) -RecoveryState @{} -DeathGeneration $generation -Now $generation -LiveMutationsReleased $true | Should -BeFalse
        Test-GathererDeathRecoveryDeployDue -Guid 3 -Bot (New-TestGathererBot -Guid 6) -ConfiguredGatherers @(3,6,49) -RecoveryState @{} -DeathGeneration $generation -Now $generation -LiveMutationsReleased $true | Should -BeFalse
        Test-GathererDeathRecoveryDeployDue -Guid 3 -Bot (New-TestGathererBot -ExplicitWorker $false) -ConfiguredGatherers @(3,6,49) -RecoveryState @{} -DeathGeneration $generation -Now $generation -LiveMutationsReleased $true | Should -BeFalse
        Test-GathererDeathRecoveryDeployDue -Guid 3 -Bot (New-TestGathererBot) -ConfiguredGatherers @(3,6,49) -RecoveryState @{} -DeathGeneration $generation -Now $generation -LiveMutationsReleased $false | Should -BeFalse
    }

    It 'writes recovered gatherer alive as a Boolean literal, not a command token' {
        $script:Source | Should -Match "-Event 'gatherer_normal_recovery_confirmed'"
        $script:Source | Should -Match '(?m)^[ \t]+alive=\$true[ \t]*$'
        $script:Source | Should -Not -Match '(?m)^[ \t]+alive=true[ \t]*$'
        $script:Source | Should -Match "recovery='observed_by_snapshot'"
    }
}
