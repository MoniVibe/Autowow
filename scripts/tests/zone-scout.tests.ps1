BeforeAll {
    . (Join-Path $PSScriptRoot '..\zone-scout-lib.ps1')
    $script:T=[datetime]::SpecifyKind([datetime]'2026-09-22T00:00:00',[DateTimeKind]::Utc)
    function New-TestState {
        New-ZoneScoutState @{guid=101;name='Scout';home_zone='Test home'} $script:T
    }
    function New-TestSample {
        param([int]$Seconds=0,[double]$X=0,[int]$Xp=0,[int]$Credit=0,
              [string]$Phase='travel',[string]$Failure='none',[bool]$Online=$true,
              [bool]$Alive=$true,[bool]$Combat=$false,[double]$Health=100,
              [string]$Session='session-a',[int]$QuestId=6,[bool]$Reward=$false)
        return @{
            at_utc=$script:T.AddSeconds($Seconds).ToString('o'); guid=101;name='Scout';home_zone='Test home'
            actual_zone=$null;online=$Online;alive=$Alive;combat=$Combat;state='non-combat'
            action='';map=0;x=$X;y=0.0;z=0.0;level=5;xp=$Xp;health_pct=$Health
            session=$Session;phase=$Phase;failure=$Failure;quest_id=$QuestId
            objective_credits=@{'6:npc:1'=$Credit};native_reward_confirmed=$Reward
            target='';oracle_receipt_sequence=100
        }
    }
    function Send-TestSample {
        param([hashtable]$State,[hashtable]$Sample,[int]$Seconds,[int]$Stall=60,[int]$QuestNoCredit=600)
        return @(Update-ZoneScoutState $State $Sample $script:T.AddSeconds($Seconds) $Stall $QuestNoCredit)
    }
}
Describe 'zone scout incident rules' {
    It 'records optional zone and movement telemetry without inventing old-binary values' {
        $bot=[pscustomobject]@{
            alive=$true; combat=$false; state='non-combat'; action=''
            position=[pscustomobject]@{ map=1; x=1.0; y=2.0; z=3.0 }
            progress=[pscustomobject]@{ level=5; xp=10 }
            health=[pscustomobject]@{ pct=100 }
            target=[pscustomobject]@{ name='' }
            oracle=[pscustomobject]@{ process_session_id='session-a'; phase='travel'; failure_code='none'; last_receipt_sequence=1; quest_id=6 }
        }
        $scout=@{ guid=101; name='Scout'; home_zone='Home label' }
        $sample=ConvertTo-ZoneScoutSample $scout $bot $null $null $script:T
        $sample.actual_zone | Should -BeNullOrEmpty
        $sample.observed_zone_id | Should -BeNullOrEmpty
        $sample.run_speed_yards_per_second | Should -BeNullOrEmpty
        $bot.position | Add-Member NoteProperty zone 141
        $bot.position | Add-Member NoteProperty area 142
        $bot.position | Add-Member NoteProperty instance 3
        $bot | Add-Member NoteProperty movement ([pscustomobject]@{ is_moving=$true; run_speed_yards_per_second=10.5; walk_speed_yards_per_second=3.75; swim_speed_yards_per_second=6 })
        $sample=ConvertTo-ZoneScoutSample $scout $bot $null $null $script:T
        $sample.observed_zone_id | Should -Be 141
        $sample.observed_area_id | Should -Be 142
        $sample.observed_instance_id | Should -Be 3
        $sample.run_speed_yards_per_second | Should -Be 10.5
        $bot.oracle.quest_id=0
        $log=[pscustomobject]@{quests=@([pscustomobject]@{id=6;objectives=@()})}
        (ConvertTo-ZoneScoutSample $scout $bot $null $log $script:T).quest_id | Should -Be 6
    }
    It 'uses UTC elapsed time for a live-style respawn wait' {
        $now=[datetime]::UtcNow
        $now.Kind | Should -Be 'Utc'
        $s=New-ZoneScoutState @{guid=101;name='Scout';home_zone='Test home'} $now.AddSeconds(-200)
        $sample=New-TestSample -Phase wait_for_respawn
        $events=@(Update-ZoneScoutState $s $sample $now 180 600)
        @($events | Where-Object kind -eq 'wait_respawn').Count | Should -Be 1
    }
    It 'normalizes explicit offsets in breadcrumb windows and quest timers' {
        $breadcrumbs=[System.Collections.Generic.List[object]]::new()
        $breadcrumbs.Add(@{at_utc='2026-09-22T03:00:00+03:00';online=$true;alive=$true;map=0;x=0;y=0;z=0})
        $breadcrumbs.Add(@{at_utc='2026-09-21T20:00:30-04:00';online=$true;alive=$true;map=0;x=0;y=0;z=0})
        $breadcrumbs.Add(@{at_utc='2026-09-22T00:01:00Z';online=$true;alive=$true;map=0;x=0;y=0;z=0})
        (Get-ZoneScoutMovementKind $breadcrumbs $script:T.AddSeconds(60) 60) | Should -Be 'stationary'
        $s=New-TestState
        $s.quest_context_id=6
        $s.quest_last_credit_utc='2026-09-22T03:00:00+03:00'
        $events=@(Send-TestSample $s (New-TestSample -Seconds 200) 200 180 180)
        @($events | Where-Object kind -eq 'quest_no_credit').Count | Should -Be 1
    }
    It 'ends bounded grace after sixty seconds with an explicit-offset timestamp' {
        $s=New-TestState
        Send-TestSample $s (New-TestSample -Health 50) 0 | Out-Null
        Send-TestSample $s (New-TestSample -Seconds 30 -Combat $true -Health 60) 30 | Out-Null
        $s.grace_started_utc='2026-09-22T03:00:30+03:00'
        Send-TestSample $s (New-TestSample -Seconds 60 -Combat $true -Health 65) 60 | Out-Null
        @(Send-TestSample $s (New-TestSample -Seconds 75 -Combat $true -Health 68) 75 | Where-Object kind -eq 'stall_stationary').Count | Should -Be 0
        @(Send-TestSample $s (New-TestSample -Seconds 95 -Combat $true -Health 70) 95 | Where-Object kind -eq 'stall_stationary').Count | Should -Be 1
    }
    It 'flags a stationary no-progress bot and dedupes repeated polls' {
        $s=New-TestState
        Send-TestSample $s (New-TestSample) 0 | Out-Null
        Send-TestSample $s (New-TestSample -Seconds 30) 30 | Out-Null
        $first=@(Send-TestSample $s (New-TestSample -Seconds 60) 60)
        @($first | Where-Object kind -eq 'stall_stationary').Count | Should -Be 1
        @(Send-TestSample $s (New-TestSample -Seconds 70) 70).Count | Should -Be 0
        $s.active.stall_stationary.recurrence_count | Should -Be 2
        $s.incident_totals.stall_stationary | Should -Be 1
    }
    It 'classifies bounded back-and-forth movement as oscillation' {
        $s=New-TestState
        Send-TestSample $s (New-TestSample -X 0) 0 | Out-Null
        Send-TestSample $s (New-TestSample -Seconds 20 -X 12) 20 | Out-Null
        Send-TestSample $s (New-TestSample -Seconds 40 -X 0) 40 | Out-Null
        $events=@(Send-TestSample $s (New-TestSample -Seconds 60 -X 0) 60)
        @($events | Where-Object kind -eq 'stall_oscillation').Count | Should -Be 1
    }
    It 'resets general stall and emits recovery on actual XP or native objective progress' {
        $s=New-TestState
        Send-TestSample $s (New-TestSample) 0 | Out-Null
        Send-TestSample $s (New-TestSample -Seconds 60) 60 | Out-Null
        $events=@(Send-TestSample $s (New-TestSample -Seconds 65 -Xp 20) 65)
        @($events | Where-Object { $_.state -eq 'RECOVERED' -and $_.kind -eq 'stall_stationary' }).Count | Should -Be 1
        $s.status | Should -Be 'OBSERVING'
        $s.last_progress_utc | Should -Be $script:T.AddSeconds(65).ToUniversalTime().ToString('o')
    }
    It 'bounds combat and health recovery grace rather than suppressing stalls forever' {
        $s=New-TestState
        Send-TestSample $s (New-TestSample -Health 50) 0 | Out-Null
        Send-TestSample $s (New-TestSample -Seconds 30 -Combat $true -Health 60) 30 | Out-Null
        @(Send-TestSample $s (New-TestSample -Seconds 60 -Combat $true -Health 65) 60).Count | Should -Be 0
        @(Send-TestSample $s (New-TestSample -Seconds 75 -Combat $true -Health 68) 75 | Where-Object kind -eq 'stall_stationary').Count | Should -Be 0
        @(Send-TestSample $s (New-TestSample -Seconds 91 -Combat $true -Health 70) 91 | Where-Object kind -eq 'stall_stationary').Count | Should -Be 1
    }
    It 'flags repeated same-quest failure without treating failure churn as progress' {
        $s=New-TestState
        Send-TestSample $s (New-TestSample -Failure path_blocked) 0 | Out-Null
        Send-TestSample $s (New-TestSample -Seconds 30 -Failure path_blocked) 30 | Out-Null
        $events=@(Send-TestSample $s (New-TestSample -Seconds 60 -Failure path_blocked) 60)
        @($events | Where-Object kind -eq 'repeated_failure').Count | Should -Be 1
    }
    It 'does not call long directed travel a local oscillation' {
        $s=New-TestState
        Send-TestSample $s (New-TestSample -X 0) 0 | Out-Null
        Send-TestSample $s (New-TestSample -Seconds 30 -X 40) 30 | Out-Null
        @(Send-TestSample $s (New-TestSample -Seconds 60 -X 80) 60 | Where-Object { $_.kind -match 'stall|no_progress' }).Count | Should -Be 0
        @(Send-TestSample $s (New-TestSample -Seconds 120 -X 160) 120 | Where-Object { $_.kind -match 'stall|no_progress' }).Count | Should -Be 0
    }
    It 'suspects an endless respawn wait despite phase and receipt churn' {
        $s=New-TestState
        Send-TestSample $s (New-TestSample -Phase wait_for_respawn) 0 | Out-Null
        $sample=New-TestSample -Seconds 60 -Phase wait_for_respawn
        $sample.oracle_receipt_sequence=9999
        @((Send-TestSample $s $sample 60) | Where-Object kind -eq 'wait_respawn').Count | Should -Be 1
    }
    It 'marks offline and dead separately without inventing recovery' {
        $s=New-TestState
        Send-TestSample $s (New-TestSample) 0 | Out-Null
        @((Send-TestSample $s (New-TestSample -Seconds 5 -Online $false) 5) | Where-Object kind -eq 'offline').Count | Should -Be 1
        $s.status | Should -Be 'OFFLINE'
        @((Send-TestSample $s (New-TestSample -Seconds 10 -Alive $false) 10) | Where-Object kind -eq 'dead').Count | Should -Be 1
        $s.status | Should -Be 'DEAD'
        @((Send-TestSample $s (New-TestSample -Seconds 15) 15) | Where-Object state -eq 'RECOVERED').Count | Should -Be 0
    }
    It 'resets on process session change without calling it recovered' {
        $s=New-TestState
        Send-TestSample $s (New-TestSample) 0 | Out-Null
        Send-TestSample $s (New-TestSample -Seconds 60) 60 | Out-Null
        $events=@(Send-TestSample $s (New-TestSample -Seconds 65 -Session session-b) 65)
        @($events | Where-Object state -eq 'SESSION_RESET').Count | Should -Be 1
        @($events | Where-Object state -eq 'RECOVERED').Count | Should -Be 0
        $s.status | Should -Be 'OBSERVING'
    }
    It 'tracks same-quest no-credit independently of XP and recovers only on native credit' {
        $s=New-TestState
        Send-TestSample $s (New-TestSample) 0 | Out-Null
        Send-TestSample $s (New-TestSample -Seconds 30 -Xp 10) 30 | Out-Null
        $events=@(Send-TestSample $s (New-TestSample -Seconds 60 -Xp 20) 60 60 60)
        @($events | Where-Object kind -eq 'quest_no_credit').Count | Should -Be 1
        @(Send-TestSample $s (New-TestSample -Seconds 65 -Xp 30) 65 60 60 | Where-Object { $_.kind -eq 'quest_no_credit' -and $_.state -eq 'RECOVERED' }).Count | Should -Be 0
        $events=@(Send-TestSample $s (New-TestSample -Seconds 70 -Xp 30 -Credit 1) 70 60 60)
        @($events | Where-Object { $_.kind -eq 'quest_no_credit' -and $_.state -eq 'RECOVERED' }).Count | Should -Be 1
    }
    It 'detects a new process session after an offline gap' {
        $s=New-TestState
        Send-TestSample $s (New-TestSample) 0 | Out-Null
        Send-TestSample $s (New-TestSample -Seconds 60) 60 | Out-Null
        Send-TestSample $s (New-TestSample -Seconds 65 -Online $false) 65 | Out-Null
        $events=@(Send-TestSample $s (New-TestSample -Seconds 70 -Session session-b) 70)
        @($events | Where-Object state -eq 'SESSION_RESET').Count | Should -BeGreaterThan 0
        @($events | Where-Object state -eq 'RECOVERED').Count | Should -Be 0
        $s.last_online_session | Should -Be 'session-b'
        $s.last_progress_utc | Should -Be $script:T.AddSeconds(70).ToUniversalTime().ToString('o')
    }
    It 'does not recover quest no-credit from a different quest objective or reward' {
        $s=New-TestState
        $first=New-TestSample
        $first.objective_credits['7:npc:2']=0
        Send-TestSample $s $first 0 | Out-Null
        $stalled=New-TestSample -Seconds 60
        $stalled.objective_credits['7:npc:2']=0
        Send-TestSample $s $stalled 60 60 60 | Out-Null
        $other=New-TestSample -Seconds 65
        $other.objective_credits['7:npc:2']=1
        $events=@(Send-TestSample $s $other 65 60 60)
        @($events | Where-Object { $_.kind -eq 'quest_no_credit' -and $_.state -eq 'RECOVERED' }).Count | Should -Be 0
        $s.active.ContainsKey('quest_no_credit') | Should -BeTrue
        $s.quest_last_credit_utc | Should -Be $script:T.ToUniversalTime().ToString('o')
        $reward=New-TestSample -Seconds 67 -Reward $true
        $reward.native_reward_quest_id=7
        $reward.objective_credits['7:npc:2']=1
        $events=@(Send-TestSample $s $reward 67 60 60)
        @($events | Where-Object { $_.kind -eq 'quest_no_credit' -and $_.state -eq 'RECOVERED' }).Count | Should -Be 0
        $own=New-TestSample -Seconds 70 -Credit 1
        $own.objective_credits['7:npc:2']=1
        $events=@(Send-TestSample $s $own 70 60 60)
        @($events | Where-Object { $_.kind -eq 'quest_no_credit' -and $_.state -eq 'RECOVERED' }).Count | Should -Be 1
    }
    It 'recovers the tracked quest only from its own confirmed reward' {
        $s=New-TestState
        Send-TestSample $s (New-TestSample) 0 | Out-Null
        Send-TestSample $s (New-TestSample -Seconds 60) 60 60 60 | Out-Null
        $reward=New-TestSample -Seconds 65 -Reward $true
        $reward.native_reward_quest_id=6
        $events=@(Send-TestSample $s $reward 65 60 60)
        @($events | Where-Object { $_.kind -eq 'quest_no_credit' -and $_.state -eq 'RECOVERED' }).Count | Should -Be 1
    }
    It 'ends an old quest context without claiming success' {
        $s=New-TestState
        Send-TestSample $s (New-TestSample) 0 | Out-Null
        Send-TestSample $s (New-TestSample -Seconds 60) 60 60 60 | Out-Null
        $events=@(Send-TestSample $s (New-TestSample -Seconds 65 -QuestId 7) 65 60 60)
        @($events | Where-Object { $_.kind -eq 'quest_no_credit' -and $_.state -eq 'CONTEXT_CHANGED' }).Count | Should -Be 1
        @($events | Where-Object state -eq 'RECOVERED').Count | Should -Be 0
    }
}












