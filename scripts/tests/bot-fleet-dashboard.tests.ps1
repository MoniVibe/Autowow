BeforeAll {
    $script:DashboardPath = Join-Path $PSScriptRoot '..\bot-fleet-dashboard.ps1'
    . (Resolve-Path $script:DashboardPath).Path

    function New-DashboardFakeInvoker {
        param([Parameter(Mandatory)][object[]]$Bots, [Parameter(Mandatory)][hashtable]$CombatByGuid)
        $calls = [System.Collections.Generic.List[object]]::new()
        $invoker = {
            param([string]$Action, [uint32]$Guid)
            $calls.Add([pscustomobject]@{ action = $Action; guid = $Guid })
            switch ($Action) {
                'list' { return [pscustomobject]@{ ok = $true; bots = @($Bots) } }
                'combatlog' { return $CombatByGuid[[string]$Guid] }
                'questlog' {
                    if ($Guid -eq 1) {
                        return [pscustomobject]@{ ok = $true; guid = 1; quests = @([pscustomobject]@{
                            id = 1234; title = 'Test Quest'; is_complete = $false
                            objectives = @([pscustomobject]@{ kind = 'npc'; entry = 55; current = 2; required = 5; done = $false })
                        }) }
                    }
                    return [pscustomobject]@{ ok = $true; guid = $Guid; quests = @() }
                }
                'questobjective' {
                    if ($Guid -eq 1) {
                        return [pscustomobject]@{ ok = $true; guid = 1; objective = [pscustomobject]@{
                            quest_id = 1234; phase = 'engage_target'; objective_kind = 'creature_credit'
                            required_npc_or_go_entry = 55; current_count = 2; required_count = 5
                        } }
                    }
                    return [pscustomobject]@{ ok = $false; error = 'objective_value_unavailable' }
                }
            }
        }.GetNewClosure()
        [pscustomobject]@{ invoker = $invoker.GetNewClosure(); calls = $calls }
    }
}

Describe 'bot-fleet-dashboard pure read-only seams' {
    It 'groups live bots by exact leader and reports counts, locations, separation, and leader objective detail' {
        $bots = @(
            [pscustomobject]@{ guid = 1; name = 'Leader'; alive = $true; position = [pscustomobject]@{ map = 576; x = 0; y = 0; z = 0 }; group = [pscustomobject]@{ members = 3; leader_guid = 1 } },
            [pscustomobject]@{ guid = 2; name = 'Healer'; alive = $true; position = [pscustomobject]@{ map = 576; x = 3; y = 4; z = 0 }; group = [pscustomobject]@{ members = 3; leader_guid = 1 } },
            [pscustomobject]@{ guid = 3; name = 'Dps'; alive = $false; position = [pscustomobject]@{ map = 576; x = 0; y = 0; z = 12 }; group = [pscustomobject]@{ members = 3; leader_guid = 1 } },
            [pscustomobject]@{ guid = 4; name = 'Other'; alive = $true; position = [pscustomobject]@{ map = 571; x = 10; y = 10; z = 10 }; group = [pscustomobject]@{ members = 1; leader_guid = 4 } }
        )
        $combat = @{}
        foreach ($bot in $bots) {
            $combat[[string]$bot.guid] = [pscustomobject]@{ ok = $true; telemetry = [pscustomobject]@{
                guid = $bot.guid; alive = $bot.alive; death_state = if ($bot.alive) { 'alive' } else { 'corpse' }
                in_combat = ($bot.guid -eq 2); location = [pscustomobject]@{ map_id = $bot.position.map; instance_id = if ($bot.guid -eq 4) { 0 } else { 9 } }
                group = $bot.group
            } }
        }
        $fake = New-DashboardFakeInvoker -Bots $bots -CombatByGuid $combat
        $snapshot = Invoke-BotFleetDashboardSnapshot -ControlInvoker $fake.invoker

        @($snapshot.groups).Count | Should -Be 2
        $leaderGroup = @($snapshot.groups | Where-Object leader_guid -eq 1)[0]
        $leaderGroup.alive_count | Should -Be 2
        $leaderGroup.dead_count | Should -Be 1
        $leaderGroup.combat_count | Should -Be 1
        $leaderGroup.map_ids | Should -Be 576
        $leaderGroup.instance_ids | Should -Be 9
        $leaderGroup.max_separation | Should -Be 13
        $leaderGroup.quest.quest_id | Should -Be 1234
        $leaderGroup.quest.phase | Should -Be 'engage_target'
        $leaderGroup.quest.objective_progress | Should -Be '2/5'
        @($fake.calls | Where-Object action -eq 'questobjective').Count | Should -Be 2
    }

    It 'uses only the exact manifest roster and ignores extra online bots' {
        $manifestPath = Join-Path $TestDrive 'manifest.json'
        [IO.File]::WriteAllText($manifestPath, (@{
            schema = 'autowow.probe-lab.manifest.v1'; lab_id = 'test'; probes = @(@{
                id = 'exact-probe'; kind = 'party'; leader_guid = 1; members = @(@{ guid = 1; name = 'Leader' }, @{ guid = 2; name = 'Healer' })
            })
        } | ConvertTo-Json -Depth 10))
        $bots = @(
            [pscustomobject]@{ guid = 1; name = 'Leader'; alive = $true; position = [pscustomobject]@{ map = 576; x = 0; y = 0; z = 0 }; group = [pscustomobject]@{ members = 2; leader_guid = 1 } },
            [pscustomobject]@{ guid = 2; name = 'Healer'; alive = $true; position = [pscustomobject]@{ map = 576; x = 1; y = 0; z = 0 }; group = [pscustomobject]@{ members = 2; leader_guid = 1 } },
            [pscustomobject]@{ guid = 99; name = 'Foreign'; alive = $true; position = [pscustomobject]@{ map = 576; x = 99; y = 0; z = 0 }; group = [pscustomobject]@{ members = 3; leader_guid = 1 } }
        )
        $combat = @{}
        foreach ($bot in $bots) { $combat[[string]$bot.guid] = [pscustomobject]@{ ok = $true; telemetry = [pscustomobject]@{ alive = $true; location = [pscustomobject]@{ map_id = 576; instance_id = 1 }; group = [pscustomobject]@{ members = 2; leader_guid = 1 } } } }
        $fake = New-DashboardFakeInvoker -Bots $bots -CombatByGuid $combat
        $snapshot = Invoke-BotFleetDashboardSnapshot -ProbeManifestPath $manifestPath -ControlInvoker $fake.invoker

        $snapshot.groups[0].label | Should -Be 'exact-probe'
        $snapshot.groups[0].roster_count | Should -Be 2
        @($snapshot.groups[0].members.guid) | Should -Be @(1, 2)
        @($fake.calls | Where-Object { $_.action -eq 'combatlog' }).Count | Should -Be 2
        @($fake.calls | Where-Object { $_.guid -eq 99 }).Count | Should -Be 0
    }

    It 'supports a leader filter and records bridge errors without mutating state' {
        $bots = @([pscustomobject]@{ guid = 7; name = 'Only'; alive = $true; position = [pscustomobject]@{ map = 571; x = 0; y = 0; z = 0 }; group = [pscustomobject]@{ members = 1; leader_guid = 7 } })
        $combat = @{ '7' = [pscustomobject]@{ ok = $false; error = 'bot_not_online' } }
        $fake = New-DashboardFakeInvoker -Bots $bots -CombatByGuid $combat
        $snapshot = Invoke-BotFleetDashboardSnapshot -LeaderGuid 7 -ControlInvoker $fake.invoker
        $snapshot.groups[0].offline_count | Should -Be 0
        $snapshot.groups[0].dead_count | Should -Be 0
        $snapshot.groups[0].members[0].bridge_error | Should -Be 'bridge_error'
    }

    It 'accepts multiple leader GUIDs and exposes PollSeconds as the refresh alias' {
        $bots = @(
            [pscustomobject]@{ guid = 1; name = 'One'; alive = $true; position = [pscustomobject]@{ map = 1; x = 0; y = 0; z = 0 }; group = [pscustomobject]@{ members = 1; leader_guid = 1 } },
            [pscustomobject]@{ guid = 2; name = 'Two'; alive = $true; position = [pscustomobject]@{ map = 2; x = 0; y = 0; z = 0 }; group = [pscustomobject]@{ members = 1; leader_guid = 2 } }
        )
        $combat = @{}
        foreach ($bot in $bots) { $combat[[string]$bot.guid] = [pscustomobject]@{ ok = $true; telemetry = [pscustomobject]@{ alive = $true; location = [pscustomobject]@{ map_id = $bot.position.map; instance_id = 0 }; group = $bot.group } } }
        $fake = New-DashboardFakeInvoker -Bots $bots -CombatByGuid $combat
        $snapshot = Invoke-BotFleetDashboardSnapshot -LeaderGuid ([uint32[]]@(1, 2)) -ControlInvoker $fake.invoker
        @($snapshot.groups).Count | Should -Be 2
        $command = Get-Command Invoke-BotFleetDashboard
        $command.Parameters['LeaderGuid'].ParameterType.FullName | Should -Be 'System.UInt32[]'
        @($command.Parameters['RefreshSeconds'].Aliases) | Should -Contain 'PollSeconds'
    }

    It 'keeps all manifest probes and additively appends requested discovered leaders' {
        $manifestPath = Join-Path $TestDrive 'manifest-additive.json'
        [IO.File]::WriteAllText($manifestPath, (@{
            schema = 'autowow.probe-lab.manifest.v1'; lab_id = 'additive'; probes = @(@{
                id = 'exact-seven'; kind = 'party'; leader_guid = 7
                members = @(@{ guid = 7; name = 'Seven' }, @{ guid = 8; name = 'Eight' })
            })
        } | ConvertTo-Json -Depth 10))
        $bots = @(
            [pscustomobject]@{ guid = 7; name = 'Seven'; alive = $true; position = [pscustomobject]@{ map = 576; x = 0; y = 0; z = 0 }; group = [pscustomobject]@{ members = 2; leader_guid = 7 } },
            [pscustomobject]@{ guid = 8; name = 'Eight'; alive = $true; position = [pscustomobject]@{ map = 576; x = 1; y = 0; z = 0 }; group = [pscustomobject]@{ members = 2; leader_guid = 7 } },
            [pscustomobject]@{ guid = 10; name = 'Ten'; alive = $true; position = [pscustomobject]@{ map = 624; x = 0; y = 0; z = 0 }; group = [pscustomobject]@{ members = 2; leader_guid = 10 } },
            [pscustomobject]@{ guid = 11; name = 'Eleven'; alive = $true; position = [pscustomobject]@{ map = 624; x = 1; y = 0; z = 0 }; group = [pscustomobject]@{ members = 2; leader_guid = 10 } }
        )
        $combat = @{}
        foreach ($bot in $bots) {
            $combat[[string]$bot.guid] = [pscustomobject]@{ ok = $true; telemetry = [pscustomobject]@{
                alive = $true; location = [pscustomobject]@{ map_id = $bot.position.map; instance_id = 1 }; group = $bot.group
            } }
        }
        $fake = New-DashboardFakeInvoker -Bots $bots -CombatByGuid $combat
        $snapshot = Invoke-BotFleetDashboardSnapshot -ProbeManifestPath $manifestPath -LeaderGuid ([uint32[]]@(7, 10)) -ControlInvoker $fake.invoker

        @($snapshot.groups).Count | Should -Be 2
        $snapshot.groups[0].label | Should -Be 'exact-seven'
        $snapshot.groups[0].roster_count | Should -Be 2
        $snapshot.groups[1].label | Should -Be 'leader:10'
        @($snapshot.groups[1].members.guid) | Should -Be @(10, 11)
    }

    It 'parse-checks the script and pins the read-only action boundary' {
        $tokens = $null; $errors = $null
        [System.Management.Automation.Language.Parser]::ParseFile((Resolve-Path $script:DashboardPath).Path, [ref]$tokens, [ref]$errors) | Out-Null
        @($errors).Count | Should -Be 0
        $source = Get-Content -LiteralPath $script:DashboardPath -Raw
        $source | Should -Match "ValidateSet\('list', 'combatlog', 'questlog', 'questobjective'\)"
        $source | Should -Not -Match '(?i)pause|resume|travel|activate|deactivate|party|raid-create|database|mysql|TcpClient'
    }
}
