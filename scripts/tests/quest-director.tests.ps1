<#
    Pester v5 offline tests for Quest Director V1 selection + classification + backoff.
    Pure logic only — no bridge, no server, no database.

    Run:  Invoke-Pester -Path .\scripts\tests\quest-director.tests.ps1
#>

BeforeAll {
    . (Join-Path (Split-Path -Parent $PSScriptRoot) 'QuestDirectorLib.ps1')

    function New-Quest {
        param([int]$Id, [string]$Class, [bool]$Complete = $false, [object[]]$Objectives = @())
        $st = 3; if ($Complete) { $st = 1 }
        [pscustomobject]@{
            id = $Id; title = "q$Id"; status = $st; is_complete = $Complete
            quest_type = 0; suggested_players = 0; src_item = 0
            capability_class = $Class; supported = (Test-CapabilitySupported $Class); objectives = $Objectives
        }
    }
    function New-Obj { param([int]$Req, [int]$Cur) [pscustomobject]@{ kind = 'npc'; entry = 1; required = $Req; current = $Cur; done = ($Cur -ge $Req) } }
    function New-Backoff { param([string[]]$Keys = @()) $h = [System.Collections.Generic.HashSet[string]]::new(); foreach ($k in $Keys) { [void]$h.Add($k) }; return $h }
    function New-Position { param([int]$Map = 1, [double]$X = 0, [double]$Y = 0, [double]$Z = 0) [pscustomobject]@{ map = $Map; x = $X; y = $Y; z = $Z } }
}

Describe 'Get-CapabilityClass' {
    It 'classifies a complete quest as turnin' { Get-CapabilityClass -QuestType 0 -SuggestedPlayers 0 -SrcItem 0 -HasNpc $true -HasGo $false -HasItem $false -IsComplete $true | Should -Be 'turnin' }
    It 'classifies group/dungeon only when non-normal type AND 2+ suggested players' { Get-CapabilityClass -QuestType 81 -SuggestedPlayers 5 -SrcItem 0 -HasNpc $true -HasGo $false -HasItem $false -IsComplete $false | Should -Be 'dungeon_group' }
    It 'does NOT flag a normal quest (QuestType 2, 0 suggested) as group' { Get-CapabilityClass -QuestType 2 -SuggestedPlayers 0 -SrcItem 0 -HasNpc $true -HasGo $false -HasItem $false -IsComplete $false | Should -Be 'kill' }
    It 'classifies source-item + npc as item_use' { Get-CapabilityClass -QuestType 0 -SuggestedPlayers 0 -SrcItem 5202 -HasNpc $true -HasGo $false -HasItem $false -IsComplete $false | Should -Be 'item_use' }
    It 'classifies a gameobject objective as unsupported gameobject' { Get-CapabilityClass -QuestType 0 -SuggestedPlayers 0 -SrcItem 0 -HasNpc $false -HasGo $true -HasItem $false -IsComplete $false | Should -Be 'gameobject' }
    It 'classifies a kill objective' { Get-CapabilityClass -QuestType 0 -SuggestedPlayers 0 -SrcItem 0 -HasNpc $true -HasGo $false -HasItem $false -IsComplete $false | Should -Be 'kill' }
    It 'classifies a loot objective' { Get-CapabilityClass -QuestType 0 -SuggestedPlayers 0 -SrcItem 0 -HasNpc $false -HasGo $false -HasItem $true -IsComplete $false | Should -Be 'loot' }
    It 'classifies a no-objective quest as talk' { Get-CapabilityClass -QuestType 0 -SuggestedPlayers 0 -SrcItem 0 -HasNpc $false -HasGo $false -HasItem $false -IsComplete $false | Should -Be 'talk' }
}

Describe 'Test-CapabilitySupported' {
    It 'supports live-proven talk/kill/loot/item-use/turnin' { foreach ($c in @('talk', 'kill', 'loot', 'item_use', 'turnin')) { Test-CapabilitySupported $c | Should -BeTrue } }
    It 'rejects unproven or group classes' { foreach ($c in @('gameobject', 'dungeon_group')) { Test-CapabilitySupported $c | Should -BeFalse } }
}

Describe 'Get-ObjectiveProgressRatio' {
    It 'treats no-objective quests as ready (1.0)' { Get-ObjectiveProgressRatio @() | Should -Be 1.0 }
    It 'computes a partial ratio' { Get-ObjectiveProgressRatio @((New-Obj 10 3), (New-Obj 10 2)) | Should -Be 0.25 }
    It 'caps current at required' { Get-ObjectiveProgressRatio @((New-Obj 5 99)) | Should -Be 1.0 }
}

Describe 'Phase-sensitive director progress' {
    It 'reduces the live questobjective finisher contract at the correct payload levels' {
        $observation = ConvertTo-DirectorFinisherObservation -Response ([pscustomobject]@{
            ok = $true
            directive_active = $true
            directive_quest_id = 806
            objective = [pscustomobject]@{
                quest_id = 806
                phase = 'travel_to_finisher'
                finisher_live = [pscustomobject]@{ loaded = $true; distance = 87.5 }
            }
        })

        $observation.available | Should -BeTrue
        $observation.quest_id | Should -Be 806
        $observation.phase | Should -Be 'travel_to_finisher'
        $observation.distance | Should -Be 87.5
        $observation.directive_active | Should -BeTrue
        $observation.directive_quest_id | Should -Be 806
    }

    It 'counts a meaningful finisher-distance decrease during turn-in' {
        $decision = Get-DirectorQuestProgressDecision -DirectorPhase turnin `
            -PreviousObjectiveSum 1 -CurrentObjectiveSum 1 `
            -PreviousFinisherPhase travel_to_finisher -CurrentFinisherPhase travel_to_finisher `
            -PreviousFinisherDistance 120 -CurrentFinisherDistance 114.9

        $decision.made_progress | Should -BeTrue
        $decision.reason | Should -Be 'finisher_distance_decreased'
    }

    It 'does not count flat or sub-epsilon finisher telemetry during turn-in' {
        $flat = Get-DirectorQuestProgressDecision -DirectorPhase turnin `
            -PreviousObjectiveSum 1 -CurrentObjectiveSum 1 `
            -PreviousFinisherPhase travel_to_finisher -CurrentFinisherPhase travel_to_finisher `
            -PreviousFinisherDistance 120 -CurrentFinisherDistance 120
        $smallDecrease = Get-DirectorQuestProgressDecision -DirectorPhase turnin `
            -PreviousObjectiveSum 1 -CurrentObjectiveSum 1 `
            -PreviousFinisherPhase travel_to_finisher -CurrentFinisherPhase travel_to_finisher `
            -PreviousFinisherDistance 120 -CurrentFinisherDistance 115.1

        $flat.made_progress | Should -BeFalse
        $smallDecrease.made_progress | Should -BeFalse
    }

    It 'counts advancement through the bounded finisher phase order' {
        $interact = Get-DirectorQuestProgressDecision -DirectorPhase turnin `
            -PreviousObjectiveSum 1 -CurrentObjectiveSum 1 `
            -PreviousFinisherPhase travel_to_finisher -CurrentFinisherPhase interact_finisher `
            -PreviousFinisherDistance 25 -CurrentFinisherDistance 25
        $verify = Get-DirectorQuestProgressDecision -DirectorPhase turnin `
            -PreviousObjectiveSum 1 -CurrentObjectiveSum 1 `
            -PreviousFinisherPhase interact_finisher -CurrentFinisherPhase verify_reward

        $interact.made_progress | Should -BeTrue
        $interact.reason | Should -Be 'finisher_phase_advanced'
        $verify.made_progress | Should -BeTrue
    }

    It 'keeps objective progress counter-centric' {
        $counter = Get-DirectorQuestProgressDecision -DirectorPhase objective `
            -PreviousObjectiveSum 0 -CurrentObjectiveSum 1
        $activityOnly = Get-DirectorQuestProgressDecision -DirectorPhase objective `
            -PreviousObjectiveSum 1 -CurrentObjectiveSum 1 `
            -PreviousFinisherPhase travel_to_finisher -CurrentFinisherPhase verify_reward `
            -PreviousFinisherDistance 120 -CurrentFinisherDistance 10

        $counter.made_progress | Should -BeTrue
        $counter.reason | Should -Be 'objective_counter_advanced'
        $activityOnly.made_progress | Should -BeFalse
        $activityOnly.reason | Should -Be 'objective_counter_flat'
    }
}

Describe 'Get-AbandonmentsFromText' {
    It 'extracts leader guid + quest id from an abandonment line' {
        $text = "12:00 [New RPG] Saewash marked as abandoned quest 789`n12:01 [New RPG] Brandreas marked as abandoned quest 459"
        $r = Get-AbandonmentsFromText -Text $text -NameToGuid @{ 'Saewash' = 7; 'Brandreas' = 10 }
        @($r).Count | Should -Be 2
        @($r)[0].guid | Should -Be 7; @($r)[0].quest | Should -Be 789
        @($r)[1].guid | Should -Be 10; @($r)[1].quest | Should -Be 459
    }
    It 'ignores names not in the tracked map' {
        $r = Get-AbandonmentsFromText -Text '[New RPG] Someone marked as abandoned quest 111' -NameToGuid @{ 'Saewash' = 7 }
        @($r).Count | Should -Be 0
    }
    It 'returns empty for unrelated log text' {
        @(Get-AbandonmentsFromText -Text 'nothing to see here' -NameToGuid @{ 'Saewash' = 7 }).Count | Should -Be 0
    }
}

Describe 'Prolonged death-or-combat stall policy' {
    BeforeEach {
        $script:stallStart = [datetime]'2026-07-16T00:00:00Z'
        $script:stallThreshold = 300
    }

    It 'derives a configurable threshold from no-progress time with a five-minute floor' {
        Get-DeathOrCombatStallThresholdSeconds -NoProgressSeconds 120 | Should -Be 300
        Get-DeathOrCombatStallThresholdSeconds -NoProgressSeconds 240 | Should -Be 480
        Get-DeathOrCombatStallThresholdSeconds -NoProgressSeconds 120 -ConfiguredSeconds 420 | Should -Be 420
        Get-DeathOrCombatStallThresholdSeconds -NoProgressSeconds 120 -ConfiguredSeconds 60 | Should -Be 300
    }

    It 'keeps an ordinary defer below the prolonged-stall threshold out of backoff' {
        $first = Get-DeathOrCombatStallDecision -PreviousState $null -IsDeferred $true -QuestId 3521 `
            -Generation 'gen-a' -NowUtc $stallStart -ThresholdSeconds $stallThreshold
        $below = Get-DeathOrCombatStallDecision -PreviousState $first.state -IsDeferred $true -QuestId 3521 `
            -Generation 'gen-a' -NowUtc $stallStart.AddSeconds(299) -ThresholdSeconds $stallThreshold

        $first.action | Should -Be 'defer'
        $below.action | Should -Be 'defer'
        $below.deferred_seconds | Should -Be 299
    }

    It 'requests backoff exactly at the prolonged-stall boundary' {
        $first = Get-DeathOrCombatStallDecision -PreviousState $null -IsDeferred $true -QuestId 3521 `
            -Generation 'gen-a' -NowUtc $stallStart -ThresholdSeconds $stallThreshold
        $boundary = Get-DeathOrCombatStallDecision -PreviousState $first.state -IsDeferred $true -QuestId 3521 `
            -Generation 'gen-a' -NowUtc $stallStart.AddSeconds(300) -ThresholdSeconds $stallThreshold

        $boundary.action | Should -Be 'backoff'
        $boundary.reason | Should -Be 'repeated_death_or_combat_stall'
        $boundary.deferred_seconds | Should -Be 300
    }

    It 'resets on a healthy cycle and restarts on quest or generation change' {
        $first = Get-DeathOrCombatStallDecision -PreviousState $null -IsDeferred $true -QuestId 3521 `
            -Generation 'gen-a' -NowUtc $stallStart -ThresholdSeconds $stallThreshold
        $questChanged = Get-DeathOrCombatStallDecision -PreviousState $first.state -IsDeferred $true -QuestId 3600 `
            -Generation 'gen-a' -NowUtc $stallStart.AddSeconds(299) -ThresholdSeconds $stallThreshold
        $generationChanged = Get-DeathOrCombatStallDecision -PreviousState $questChanged.state -IsDeferred $true -QuestId 3600 `
            -Generation 'gen-b' -NowUtc $stallStart.AddSeconds(598) -ThresholdSeconds $stallThreshold
        $healthy = Get-DeathOrCombatStallDecision -PreviousState $generationChanged.state -IsDeferred $false -QuestId 3600 `
            -Generation 'gen-b' -NowUtc $stallStart.AddSeconds(599) -ThresholdSeconds $stallThreshold

        $questChanged.action | Should -Be 'defer'
        $questChanged.deferred_seconds | Should -Be 0
        $generationChanged.action | Should -Be 'defer'
        $generationChanged.deferred_seconds | Should -Be 0
        $healthy.action | Should -Be 'reset'
        $healthy.state | Should -BeNullOrEmpty
    }

    It 'never requests backoff when no quest id is known' {
        $unknown = Get-DeathOrCombatStallDecision -PreviousState $null -IsDeferred $true -QuestId 0 `
            -Generation 'gen-a' -NowUtc $stallStart.AddHours(1) -ThresholdSeconds $stallThreshold

        $unknown.action | Should -Be 'defer'
        $unknown.reason | Should -Be 'no_known_quest'
        $unknown.state | Should -BeNullOrEmpty
    }

    It 'associates defer context with the selected quest or a still-known last-issued quest' {
        $quests = @(
            (New-Quest -Id 3521 -Class 'kill' -Objectives @((New-Obj 1 0))),
            (New-Quest -Id 3600 -Class 'loot' -Objectives @((New-Obj 1 0)))
        )

        (Get-DeathOrCombatQuestContext -SelectedQuestId 3600 -LastIssued '3521:objective' -Quests $quests).quest_id | Should -Be 3600
        (Get-DeathOrCombatQuestContext -SelectedQuestId 0 -LastIssued '3521:objective' -Quests $quests).quest_id | Should -Be 3521
        (Get-DeathOrCombatQuestContext -SelectedQuestId 0 -LastIssued '9999:objective' -Quests $quests).quest_id | Should -Be 0
    }
}

Describe 'Select-DirectorQuest' {
    It 'chooses a turn-in before any objective' {
        $quests = @((New-Quest -Id 200 -Class 'kill' -Objectives @((New-Obj 10 9))), (New-Quest -Id 100 -Class 'turnin' -Complete $true))
        $sel = Select-DirectorQuest -Quests $quests -BackoffKeys (New-Backoff) -LeaderGuid 7
        $sel.action | Should -Be 'issue'; $sel.phase | Should -Be 'turnin'; $sel.quest_id | Should -Be 100
    }
    It 'chooses the lowest id among multiple turn-ins' {
        $quests = @((New-Quest -Id 300 -Class 'turnin' -Complete $true), (New-Quest -Id 150 -Class 'turnin' -Complete $true))
        (Select-DirectorQuest -Quests $quests -BackoffKeys (New-Backoff) -LeaderGuid 7).quest_id | Should -Be 150
    }
    It 'chooses the supported objective with the greatest progress ratio' {
        $quests = @(
            (New-Quest -Id 10 -Class 'kill' -Objectives @((New-Obj 10 1))),
            (New-Quest -Id 20 -Class 'loot' -Objectives @((New-Obj 10 8)))
        )
        $sel = Select-DirectorQuest -Quests $quests -BackoffKeys (New-Backoff) -LeaderGuid 7
        $sel.quest_id | Should -Be 20; $sel.phase | Should -Be 'objective'
    }
    It 'breaks a progress tie by lowest quest id' {
        $quests = @(
            (New-Quest -Id 40 -Class 'kill' -Objectives @((New-Obj 10 5))),
            (New-Quest -Id 30 -Class 'kill' -Objectives @((New-Obj 10 5)))
        )
        (Select-DirectorQuest -Quests $quests -BackoffKeys (New-Backoff) -LeaderGuid 7).quest_id | Should -Be 30
    }
    It 'skips a backed-off quest and picks the next' {
        $quests = @(
            (New-Quest -Id 20 -Class 'loot' -Objectives @((New-Obj 10 8))),
            (New-Quest -Id 10 -Class 'kill' -Objectives @((New-Obj 10 1)))
        )
        $sel = Select-DirectorQuest -Quests $quests -BackoffKeys (New-Backoff @('7:20')) -LeaderGuid 7
        $sel.quest_id | Should -Be 10
    }
    It 'idles with unsupported_only when nothing is supported' {
        $quests = @((New-Quest -Id 10 -Class 'escort'), (New-Quest -Id 20 -Class 'gameobject'))
        $sel = Select-DirectorQuest -Quests $quests -BackoffKeys (New-Backoff) -LeaderGuid 7
        $sel.action | Should -Be 'idle'; $sel.reason | Should -Be 'unsupported_only'
    }
    It 'idles with all_supported_backed_off when every supported quest is in backoff' {
        $quests = @((New-Quest -Id 10 -Class 'kill' -Objectives @((New-Obj 10 1))))
        $sel = Select-DirectorQuest -Quests $quests -BackoffKeys (New-Backoff @('7:10')) -LeaderGuid 7
        $sel.action | Should -Be 'idle'; $sel.reason | Should -Be 'all_supported_backed_off'
    }
    It 'idles with no_active_quest for an empty log' {
        (Select-DirectorQuest -Quests @() -BackoffKeys (New-Backoff) -LeaderGuid 7).reason | Should -Be 'no_active_quest'
    }
    It 'never selects random movement or grind (only issue or idle)' {
        $quests = @((New-Quest -Id 10 -Class 'gameobject'))
        (Select-DirectorQuest -Quests $quests -BackoffKeys (New-Backoff) -LeaderGuid 7).action | Should -Be 'idle'
    }
}

Describe 'Quest acquisition generation gate' {
    It 'issues acquire once for one stable all-backed-off generation' {
        $quests = @((New-Quest -Id 3521 -Class 'kill' -Objectives @((New-Obj 1 0))))
        $backoff = New-Backoff @('10:3521')
        $first = Get-AcquireDecision -SelectionReason 'all_supported_backed_off' -LeaderGuid 10 `
            -Quests $quests -BackoffKeys $backoff -LastGeneration ''
        $first.action | Should -Be 'acquire'
        $second = Get-AcquireDecision -SelectionReason 'all_supported_backed_off' -LeaderGuid 10 `
            -Quests $quests -BackoffKeys $backoff -LastGeneration $first.generation
        $second.action | Should -Be 'hold'
        $second.generation | Should -Be $first.generation
    }

    It 'reopens acquisition when the active backoff generation changes' {
        $quests = @(
            (New-Quest -Id 10 -Class 'kill' -Objectives @((New-Obj 1 0))),
            (New-Quest -Id 20 -Class 'loot' -Objectives @((New-Obj 1 0)))
        )
        $old = Get-AcquireDecision -SelectionReason 'all_supported_backed_off' -LeaderGuid 7 `
            -Quests $quests -BackoffKeys (New-Backoff @('7:10')) -LastGeneration ''
        $new = Get-AcquireDecision -SelectionReason 'all_supported_backed_off' -LeaderGuid 7 `
            -Quests $quests -BackoffKeys (New-Backoff @('7:10','7:20')) -LastGeneration $old.generation
        $new.action | Should -Be 'acquire'
        $new.generation | Should -Not -Be $old.generation
    }

    It 'acquires once for an empty quest log generation' {
        $first = Get-AcquireDecision -SelectionReason 'no_active_quest' -LeaderGuid 7 `
            -Quests @() -BackoffKeys (New-Backoff) -LastGeneration ''
        $first.action | Should -Be 'acquire'
        $first.reason | Should -Be 'no_active_quest'

        $second = Get-AcquireDecision -SelectionReason 'no_active_quest' -LeaderGuid 7 `
            -Quests @() -BackoffKeys (New-Backoff) -LastGeneration $first.generation
        $second.action | Should -Be 'hold'
        $second.reason | Should -Be 'acquire_generation_unchanged'
    }

    It 'never acquires for an unsupported-only idle state' {
        $decision = Get-AcquireDecision -SelectionReason 'unsupported_only' -LeaderGuid 7 `
            -Quests @() -BackoffKeys (New-Backoff) -LastGeneration ''
        $decision.action | Should -Be 'idle'
    }

    It 'holds acquisition while a complete supported quest owns the active finisher directive' {
        $quests = @((New-Quest -Id 806 -Class 'turnin' -Complete $true -Objectives @((New-Obj 1 1))))
        $decision = Get-AcquireDecision -SelectionReason 'all_supported_backed_off' -LeaderGuid 7 `
            -Quests $quests -BackoffKeys (New-Backoff @('7:806')) -LastGeneration '' `
            -ActiveFinisherQuestId 806

        $decision.action | Should -Be 'hold'
        $decision.reason | Should -Be 'active_supported_finisher_directive'
    }

    It 'lets a new supported quest take over through normal exact selection' {
        $quests = @(
            (New-Quest -Id 3521 -Class 'kill' -Objectives @((New-Obj 1 0))),
            (New-Quest -Id 3600 -Class 'loot' -Objectives @((New-Obj 5 1)))
        )
        $selection = Select-DirectorQuest -Quests $quests -BackoffKeys (New-Backoff @('10:3521')) -LeaderGuid 10
        $selection.action | Should -Be 'issue'
        $selection.quest_id | Should -Be 3600
        (Get-AcquireDecision -SelectionReason $selection.reason -LeaderGuid 10 -Quests $quests `
            -BackoffKeys (New-Backoff @('10:3521')) -LastGeneration 'old').action | Should -Be 'idle'
    }
}

Describe 'Issue-on-change and recovery reissue' {
    It 'holds an unchanged explicit directive' {
        Test-ShouldIssueDirective -LastIssued '3521:objective' -IssueKey '3521:objective' | Should -BeFalse
        Test-ShouldIssueDirective -LastIssued '3521:objective' -IssueKey '3600:objective' | Should -BeTrue
    }

    It 'clears lastIssued only after successful recovery so the directive reissues' {
        $issued = @{ '10' = '3521:objective' }
        Clear-LastIssuedAfterSuccessfulRecovery -LastIssuedByLeader $issued -LeaderKey '10' -RecoveryOk $false | Should -BeFalse
        $issued['10'] | Should -Be '3521:objective'
        Clear-LastIssuedAfterSuccessfulRecovery -LastIssuedByLeader $issued -LeaderKey '10' -RecoveryOk $true | Should -BeTrue
        $issued.ContainsKey('10') | Should -BeFalse
        Test-ShouldIssueDirective -LastIssued ([string]$issued['10']) -IssueKey '3521:objective' | Should -BeTrue
    }
}

Describe 'Issued acquire no-log/no-movement expiry' {
    BeforeEach {
        $script:acquireStart = ([datetime]'2026-07-17T00:00:00Z').ToUniversalTime()
        $script:acquirePosition = New-Position -X 100 -Y 200 -Z 10
        $script:acquireBackoffKeys = New-Backoff
        $script:acquireGeneration = (Get-AcquireDecision -SelectionReason 'no_active_quest' -LeaderGuid 7 `
            -Quests @() -BackoffKeys $script:acquireBackoffKeys -LastGeneration '').generation
        $script:issuedAcquire = New-AcquireIssuedState -Generation $script:acquireGeneration `
            -IssuedUtc $script:acquireStart -Position $script:acquirePosition
    }

    It 'holds the identical empty generation before expiry without issuing again' {
        $hold = Get-AcquireDecision -SelectionReason 'no_active_quest' -LeaderGuid 7 -Quests @() `
            -BackoffKeys $acquireBackoffKeys -LastGeneration $acquireGeneration -IssuedAcquireState $issuedAcquire `
            -NowUtc $acquireStart.AddSeconds(299) -AcquireNoLogExpirySeconds 300 -CurrentPosition $acquirePosition

        $hold.action | Should -Be 'hold'
        $hold.reason | Should -Be 'acquire_pending'
        $hold.path | Should -Be 'pending'
        $hold.no_movement_seconds | Should -Be 299
        $hold.state.state | Should -Be 'issued'
        $hold.state.issued_utc | Should -Be $acquireStart
    }

    It 'survives strict-mode live decision wiring when issued state has no backoff timestamp' {
        $wiredDecision = & {
            Set-StrictMode -Version Latest

            $decision = Get-AcquireDecision -SelectionReason 'no_active_quest' -LeaderGuid 7 -Quests @() `
                -BackoffKeys $acquireBackoffKeys -LastGeneration $acquireGeneration -IssuedAcquireState $issuedAcquire `
                -NowUtc $acquireStart.AddSeconds(1) -AcquireNoLogExpirySeconds 300 -CurrentPosition $acquirePosition

            $decisionState = Get-QuestDirectorProperty -Object $decision -Name 'state' -Default $null
            [pscustomobject]@{
                action = [string](Get-QuestDirectorProperty -Object $decision -Name 'action' -Default '')
                reason = [string](Get-QuestDirectorProperty -Object $decision -Name 'reason' -Default '')
                state = $decisionState
                elapsed_seconds = [int](Get-QuestDirectorProperty -Object $decision -Name 'elapsed_seconds' -Default 0)
                no_movement_seconds = [int](Get-QuestDirectorProperty -Object $decision -Name 'no_movement_seconds' -Default 0)
                movement_distance = Get-QuestDirectorProperty -Object $decision -Name 'movement_distance' -Default $null
                backoff_until_utc = Get-QuestDirectorProperty -Object $decision -Name 'backoff_until_utc' -Default $null
                state_backoff_until_utc = Get-QuestDirectorProperty -Object $decisionState -Name 'backoff_until_utc' -Default $null
            }
        }

        $wiredDecision.action | Should -Be 'hold'
        $wiredDecision.reason | Should -Be 'acquire_pending'
        $wiredDecision.elapsed_seconds | Should -Be 1
        $wiredDecision.backoff_until_utc | Should -BeNullOrEmpty
        $wiredDecision.state_backoff_until_utc | Should -BeNullOrEmpty
    }

    It 'keeps waiting while the issued travel order is making meaningful movement' {
        $moving = Get-AcquireDecision -SelectionReason 'no_active_quest' -LeaderGuid 7 -Quests @() `
            -BackoffKeys $acquireBackoffKeys -LastGeneration $acquireGeneration -IssuedAcquireState $issuedAcquire `
            -NowUtc $acquireStart.AddSeconds(301) -AcquireNoLogExpirySeconds 300 `
            -CurrentPosition (New-Position -X 110 -Y 200 -Z 10)

        $moving.action | Should -Be 'hold'
        $moving.reason | Should -Be 'acquire_travel_progress'
        $moving.no_movement_seconds | Should -Be 0
        $moving.state.last_movement_utc | Should -Be $acquireStart.AddSeconds(301)
    }

    It 'allows exactly one retry at the no-log/no-movement boundary' {
        $retry = Get-AcquireDecision -SelectionReason 'no_active_quest' -LeaderGuid 7 -Quests @() `
            -BackoffKeys $acquireBackoffKeys -LastGeneration $acquireGeneration -IssuedAcquireState $issuedAcquire `
            -NowUtc $acquireStart.AddSeconds(300) -AcquireNoLogExpirySeconds 300 -CurrentPosition $acquirePosition

        $retry.action | Should -Be 'acquire'
        $retry.path | Should -Be 'retry'
        $retry.reason | Should -Be 'acquire_no_log_no_movement_retry'
        $retry.state.state | Should -Be 'issued'
        $retry.state.retry_count | Should -Be 1
        $retry.state.issued_utc | Should -Be $acquireStart.AddSeconds(300)

        $heldAfterRetry = Get-AcquireDecision -SelectionReason 'no_active_quest' -LeaderGuid 7 -Quests @() `
            -BackoffKeys $acquireBackoffKeys -LastGeneration $acquireGeneration -IssuedAcquireState $retry.state `
            -NowUtc $acquireStart.AddSeconds(599) -AcquireNoLogExpirySeconds 300 -CurrentPosition $acquirePosition
        $heldAfterRetry.action | Should -Be 'hold'
        $heldAfterRetry.reason | Should -Be 'acquire_pending'
    }

    It 'backs off after the retry window and opens one switch path after backoff' {
        $retry = Get-AcquireIssuedStateDecision -IssuedState $issuedAcquire -Generation $acquireGeneration `
            -NowUtc $acquireStart.AddSeconds(300) -ExpirySeconds 300 -CurrentPosition $acquirePosition -BackoffSeconds 60
        $backoff = Get-AcquireIssuedStateDecision -IssuedState $retry.state -Generation $acquireGeneration `
            -NowUtc $acquireStart.AddSeconds(600) -ExpirySeconds 300 -CurrentPosition $acquirePosition -BackoffSeconds 60

        $backoff.action | Should -Be 'backoff'
        $backoff.path | Should -Be 'backoff_switch'
        $backoff.reason | Should -Be 'acquire_no_log_no_movement_backoff'
        $backoff.state.state | Should -Be 'backoff'
        $backoff.state.backoff_until_utc | Should -Be $acquireStart.AddSeconds(660)

        $during = Get-AcquireIssuedStateDecision -IssuedState $backoff.state -Generation $acquireGeneration `
            -NowUtc $acquireStart.AddSeconds(659) -ExpirySeconds 300 -CurrentPosition $acquirePosition -BackoffSeconds 60
        $during.action | Should -Be 'hold'
        $during.reason | Should -Be 'acquire_backoff_active'

        $switch = Get-AcquireIssuedStateDecision -IssuedState $backoff.state -Generation $acquireGeneration `
            -NowUtc $acquireStart.AddSeconds(660) -ExpirySeconds 300 -CurrentPosition $acquirePosition -BackoffSeconds 60
        $switch.action | Should -Be 'switch'
        $switch.path | Should -Be 'switch'
        $switch.reason | Should -Be 'acquire_backoff_expired_switch'
        $switch.state.state | Should -Be 'issued'
        $switch.state.retry_count | Should -Be 0
    }
}

Describe 'Director acquisition source safety' {
    It 'uses only the exact acquire wire order and preserves observe-only behavior' {
        $directorPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'league-simulation-director.ps1'
        $source = Get-Content -LiteralPath $directorPath -Raw
        $source | Should -Match 'quest \$guid acquire'
        $source | Should -Match 'quest_acquire_would_issue'
        $source | Should -Match "SelectionReason \(\[string\]\`$selection.reason\)"
        $source | Should -Not -Match 'quest acquire.*TeleportTo'
        $source | Should -Not -Match 'quest acquire.*move random'
    }

    It 'wires per-leader issued state to one retry/backoff/switch path' {
        $directorPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'league-simulation-director.ps1'
        $source = Get-Content -LiteralPath $directorPath -Raw
        $source | Should -Match '\$issuedAcquireByLeader'
        $source | Should -Match 'AcquireNoLogExpirySeconds'
        $source | Should -Match 'quest_acquire_retry'
        $source | Should -Match 'quest_acquire_backoff'
        $source | Should -Match 'quest_acquire_switch'
        $source | Should -Match 'New-AcquireIssuedState'
    }

    It 'preserves movement activation evidence without adding an acquire order' {
        $directorPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'league-simulation-director.ps1'
        $source = Get-Content -LiteralPath $directorPath -Raw
        $source | Should -Match "Name 'movement_activation'"
        $source | Should -Match "Name 'movement_kick_accepted'"
        ([regex]::Matches($source, 'Invoke-BridgeJson -Request "quest \$guid acquire"')).Count | Should -Be 1
    }

    It 'keeps the prolonged death-or-combat policy ledger-only' {
        $directorPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'league-simulation-director.ps1'
        $source = Get-Content -LiteralPath $directorPath -Raw
        $match = [regex]::Match($source, '# BEGIN prolonged death/combat defer policy(?<stallPolicy>.*?)# END prolonged death/combat defer policy', 'Singleline')
        $match.Success | Should -BeTrue
        $stallPolicy = [string]$match.Groups['stallPolicy'].Value

        $stallPolicy | Should -Match 'Save-BackoffLedger'
        $stallPolicy | Should -Not -Match 'Invoke-BridgeJson|Send-Bridge|Teleport|move random|recover \$guid|quest \$guid|grant|complete|abandon'
    }
}
