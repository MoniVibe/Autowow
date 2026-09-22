Set-StrictMode -Version Latest

function ConvertTo-ZoneScoutUtc {
    param($Value)
    if ($Value -is [datetimeoffset]) { return $Value.UtcDateTime }
    if ($Value -is [datetime]) { return $Value.ToUniversalTime() }
    return [datetimeoffset]::Parse(
        [string]$Value,
        [Globalization.CultureInfo]::InvariantCulture,
        [Globalization.DateTimeStyles]::RoundtripKind
    ).UtcDateTime
}
function New-ZoneScoutState {
    param([hashtable]$Scout, [datetime]$At)
    $At=ConvertTo-ZoneScoutUtc $At
    return @{
        guid = [uint32]$Scout.guid
        name = [string]$Scout.name
        home_zone = [string]$Scout.home_zone
        session = ''
        last_online_session = ''
        started_utc = $At.ToUniversalTime().ToString('o')
        last_seen_utc = $null
        last_progress_utc = $At.ToUniversalTime().ToString('o')
        progress_count = 0
        quest_context_id = 0
        quest_last_credit_utc = $At.ToUniversalTime().ToString('o')
        status = 'OBSERVING'
        baseline = $null
        breadcrumbs = [System.Collections.Generic.List[object]]::new()
        active = @{}
        incident_totals = @{}
        first_failure_context = $null
        grace_started_utc = $null
        failure_key = ''
        failure_repeats = 0
    }
}

function Get-ZoneScoutObjectiveCredits {
    param($QuestLog)
    $result = @{}
    if ($null -eq $QuestLog) { return $result }
    foreach ($quest in @($QuestLog.quests)) {
        foreach ($objective in @($quest.objectives)) {
            $key = '{0}:{1}:{2}' -f $quest.id, $objective.kind, $objective.entry
            $result[$key] = [int]$objective.current
        }
    }
    return $result
}

function Get-ZoneScoutOptional {
    param($Object,[string]$Name)
    if ($null -eq $Object) { return $null }
    if ($Object -is [System.Collections.IDictionary]) {
        if ($Object.Contains($Name)) { return $Object[$Name] }
        return $null
    }
    $property=$Object.PSObject.Properties[$Name]
    if ($property) { return $property.Value }
    return $null
}
function ConvertTo-ZoneScoutSample {
    param([hashtable]$Scout, $Bot, $QuestObjective, $QuestLog, [datetime]$At)
    $At=ConvertTo-ZoneScoutUtc $At
    $online = $null -ne $Bot
    $session = if ($online -and $Bot.oracle) { [string]$Bot.oracle.process_session_id } else { '' }
    $credits = Get-ZoneScoutObjectiveCredits $QuestLog
    $rewardConfirmed = $false
    if ($QuestObjective) {
        $rewardConfirmed = [bool]$QuestObjective.finisher.reward.reward_confirmed -or
            [bool]$QuestObjective.objective.finisher_receipt.reward_confirmed
    }
    $phase = if ($QuestObjective -and $QuestObjective.objective) { [string]$QuestObjective.objective.phase }
             elseif ($online -and $Bot.oracle) { [string]$Bot.oracle.phase } else { '' }
    $failure = if ($QuestObjective -and $QuestObjective.objective) { [string]$QuestObjective.objective.failure_reason } else { '' }
    if ($failure -in @('','none') -and $online -and $Bot.oracle) { $failure = [string]$Bot.oracle.failure_code }
    return @{
        at_utc = $At.ToUniversalTime().ToString('o')
        guid = [uint32]$Scout.guid
        name = [string]$Scout.name
        home_zone = [string]$Scout.home_zone
        actual_zone = $null
        observed_zone_id = if ($online) { Get-ZoneScoutOptional $Bot.position 'zone' } else { $null }
        observed_area_id = if ($online) { Get-ZoneScoutOptional $Bot.position 'area' } else { $null }
        observed_instance_id = if ($online) { Get-ZoneScoutOptional $Bot.position 'instance' } else { $null }
        movement_is_moving = if ($online) { Get-ZoneScoutOptional (Get-ZoneScoutOptional $Bot 'movement') 'is_moving' } else { $null }
        run_speed_yards_per_second = if ($online) { Get-ZoneScoutOptional (Get-ZoneScoutOptional $Bot 'movement') 'run_speed_yards_per_second' } else { $null }
        walk_speed_yards_per_second = if ($online) { Get-ZoneScoutOptional (Get-ZoneScoutOptional $Bot 'movement') 'walk_speed_yards_per_second' } else { $null }
        swim_speed_yards_per_second = if ($online) { Get-ZoneScoutOptional (Get-ZoneScoutOptional $Bot 'movement') 'swim_speed_yards_per_second' } else { $null }
        online = $online
        alive = if ($online) { [bool]$Bot.alive } else { $false }
        combat = if ($online) { [bool]$Bot.combat } else { $false }
        state = if ($online) { [string]$Bot.state } else { 'offline' }
        action = if ($online) { [string]$Bot.action } else { '' }
        map = if ($online) { [int]$Bot.position.map } else { $null }
        x = if ($online) { [double]$Bot.position.x } else { $null }
        y = if ($online) { [double]$Bot.position.y } else { $null }
        z = if ($online) { [double]$Bot.position.z } else { $null }
        level = if ($online) { [int]$Bot.progress.level } else { $null }
        xp = if ($online) { [long]$Bot.progress.xp } else { $null }
        health_pct = if ($online) { [double]$Bot.health.pct } else { $null }
        session = $session
        phase = $phase
        failure = $failure
        quest_id = if ($QuestObjective -and $QuestObjective.objective -and [int]$QuestObjective.objective.quest_id -gt 0) { [int]$QuestObjective.objective.quest_id } elseif ($online -and $Bot.oracle -and $Bot.oracle.quest_id) { [int]$Bot.oracle.quest_id } elseif ($QuestLog -and @($QuestLog.quests).Count -eq 1) { [int]@($QuestLog.quests)[0].id } else { 0 }
        objective_credits = $credits
        native_reward_confirmed = $rewardConfirmed
        native_reward_quest_id = if ($rewardConfirmed -and $QuestObjective.finisher.reward.reward_confirmed -and [int]$QuestObjective.finisher.quest_id -gt 0) { [int]$QuestObjective.finisher.quest_id } elseif ($rewardConfirmed -and $QuestObjective.objective.finisher_receipt.reward_confirmed) { [int]$QuestObjective.objective.finisher_receipt.quest_id } else { 0 }
        target = if ($online) { [string]$Bot.target.name } else { '' }
        oracle_receipt_sequence = if ($online -and $Bot.oracle) { $Bot.oracle.last_receipt_sequence } else { $null }
    }
}

function Get-ZoneScoutDistance {
    param($A, $B)
    if ($null -eq $A -or $null -eq $B -or $null -eq $A.x -or $null -eq $B.x -or $A.map -ne $B.map) { return [double]::PositiveInfinity }
    return [math]::Sqrt([math]::Pow(([double]$A.x-[double]$B.x),2)+[math]::Pow(([double]$A.y-[double]$B.y),2)+[math]::Pow(([double]$A.z-[double]$B.z),2))
}

function Get-ZoneScoutMovementKind {
    param([System.Collections.Generic.List[object]]$Breadcrumbs, [datetime]$At, [int]$WindowSeconds)
    $At=ConvertTo-ZoneScoutUtc $At
    $recent = @($Breadcrumbs | Where-Object { (ConvertTo-ZoneScoutUtc $_.at_utc) -ge $At.AddSeconds(-$WindowSeconds) -and $_.online -and $_.alive })
    if ($recent.Count -lt 2) { return 'unknown' }
    $age = ($At - (ConvertTo-ZoneScoutUtc $recent[0].at_utc)).TotalSeconds
    if ($age -lt [math]::Min(30,$WindowSeconds * 0.7)) { return 'unknown' }
    $path = 0.0
    $maxRadius = 0.0
    for ($i=1; $i -lt $recent.Count; $i++) {
        $distance = Get-ZoneScoutDistance $recent[$i-1] $recent[$i]
        if (-not [double]::IsInfinity($distance)) { $path += $distance }
        $radius = Get-ZoneScoutDistance $recent[0] $recent[$i]
        if (-not [double]::IsInfinity($radius)) { $maxRadius = [math]::Max($maxRadius,$radius) }
    }
    $net = Get-ZoneScoutDistance $recent[0] $recent[-1]
    if ($maxRadius -le 3.0) { return 'stationary' }
    if ($path -ge 20.0 -and $net -le 8.0 -and $maxRadius -le 30.0) { return 'oscillation' }
    return 'moving'
}

function New-ZoneScoutEvent {
    param([hashtable]$State, [string]$Kind, [string]$EventState, [hashtable]$Sample, [datetime]$At, [string]$Reason)
    $At=ConvertTo-ZoneScoutUtc $At
    $incident = $State.active[$Kind]
    return [ordered]@{
        schema = 'autowow.zone-scout.incident.v1'
        at_utc = $At.ToUniversalTime().ToString('o')
        guid = $State.guid
        name = $State.name
        home_zone = $State.home_zone
        state = $EventState
        kind = $Kind
        reason = $Reason
        first_seen_utc = if ($incident) { $incident.first_seen_utc } else { $At.ToUniversalTime().ToString('o') }
        recurrence_count = if ($incident) { $incident.recurrence_count } else { 1 }
        first_failure_context = if ($incident) { $incident.first_failure_context } else { $null }
        breadcrumbs = @($State.breadcrumbs | Select-Object -Last 12)
        evidence = [ordered]@{
            map = $Sample.map; position = @($Sample.x,$Sample.y,$Sample.z)
            phase = $Sample.phase; failure = $Sample.failure; quest_id = $Sample.quest_id
            level = $Sample.level; xp = $Sample.xp; health_pct = $Sample.health_pct
            session = $Sample.session
        }
    }
}

function Update-ZoneScoutState {
    param([hashtable]$State, [hashtable]$Sample, [datetime]$At, [int]$StallSeconds=180, [int]$QuestNoCreditSeconds=600)
    $At=ConvertTo-ZoneScoutUtc $At
    $events=[System.Collections.Generic.List[object]]::new()
    $old=$State.baseline
    $atText=$At.ToUniversalTime().ToString('o')
    if ($Sample.online -and $Sample.session -and $State.last_online_session -and $Sample.session -ne $State.last_online_session) {
        foreach ($kind in @($State.active.Keys)) {
            $events.Add((New-ZoneScoutEvent $State $kind 'SESSION_RESET' $Sample $At 'process session changed; prior suspicion unresolved'))
        }
        $State.active.Clear()
        $State.breadcrumbs.Clear()
        $State.last_progress_utc=$atText
        $State.quest_last_credit_utc=$atText
        $State.quest_context_id=0
        $State.grace_started_utc=$null
        $State.failure_key=''
        $State.failure_repeats=0
        $old=$null
    }
    $trackedQuestId=$State.quest_context_id
    $rewardQuestId=Get-ZoneScoutOptional $Sample 'native_reward_quest_id'
    $rewardForTrackedQuest=($Sample.native_reward_confirmed -and $trackedQuestId -gt 0 -and $rewardQuestId -eq $trackedQuestId)
    if ($Sample.online -and $Sample.quest_id -ne $State.quest_context_id) {
        if ($State.active.ContainsKey('quest_no_credit') -and -not $rewardForTrackedQuest) {
            $events.Add((New-ZoneScoutEvent $State 'quest_no_credit' 'CONTEXT_CHANGED' $Sample $At 'quest context changed; outcome unknown'))
            $State.active.Remove('quest_no_credit')
        }
        $State.quest_context_id=$Sample.quest_id
        $State.quest_last_credit_utc=$atText
    }
    if ($old -and (-not $old.online -or -not $old.alive) -and $Sample.online -and $Sample.alive) {
        $State.quest_last_credit_utc=$atText
    }
    if ($Sample.online -and $Sample.session) { $State.last_online_session=$Sample.session }
    $State.session=$State.last_online_session
    $State.baseline=$Sample
    if ($Sample.online) { $State.last_seen_utc=$atText }
    $State.breadcrumbs.Add([ordered]@{
        at_utc=$atText; online=$Sample.online; alive=$Sample.alive; map=$Sample.map;
        x=$Sample.x; y=$Sample.y; z=$Sample.z; phase=$Sample.phase; quest_id=$Sample.quest_id
    })
    while ($State.breadcrumbs.Count -gt 96) { $State.breadcrumbs.RemoveAt(0) }

    $generalProgress=$false
    $nativeProgress=$false
    $sameQuestNativeProgress=$false
    $reasons=[System.Collections.Generic.List[string]]::new()
    if ($old -and $old.online -and $Sample.online -and $Sample.alive) {
        if ($Sample.level -gt $old.level -or ($Sample.level -eq $old.level -and $Sample.xp -gt $old.xp)) {
            $generalProgress=$true; $reasons.Add('xp_or_level')
        }
        foreach ($key in @($Sample.objective_credits.Keys)) {
            if ($old.objective_credits.ContainsKey($key) -and $Sample.objective_credits[$key] -gt $old.objective_credits[$key]) {
                $nativeProgress=$true; $reasons.Add('native_objective_credit')
                if ($State.quest_context_id -gt 0 -and $key.StartsWith(([string]$State.quest_context_id + ':'))) { $sameQuestNativeProgress=$true }
            }
        }
        if ($Sample.native_reward_confirmed -and -not $old.native_reward_confirmed) {
            $nativeProgress=$true; $reasons.Add('native_reward_confirmed')
            if ($rewardForTrackedQuest -or ($State.quest_context_id -gt 0 -and $rewardQuestId -eq $State.quest_context_id)) { $sameQuestNativeProgress=$true }
        }
    }
    if ($nativeProgress) { $generalProgress=$true }
    if ($sameQuestNativeProgress) { $State.quest_last_credit_utc=$atText }
    if ($generalProgress) {
        $State.last_progress_utc=$atText
        $State.progress_count++
        $State.grace_started_utc=$null
        $State.failure_key=''
        $State.failure_repeats=0
        foreach ($kind in @($State.active.Keys)) {
            if ($kind -eq 'quest_no_credit' -and -not $sameQuestNativeProgress) { continue }
            $events.Add((New-ZoneScoutEvent $State $kind 'RECOVERED' $Sample $At ($reasons -join ',')))
            $State.active.Remove($kind)
        }
    }
    $age=($At-(ConvertTo-ZoneScoutUtc $State.last_progress_utc)).TotalSeconds
    $kind=''
    if (-not $Sample.online) { $kind='offline' }
    elseif (-not $Sample.alive) { $kind='dead' }
    else {
        $failure=if ($Sample.failure -and $Sample.failure -ne 'none') { "$($Sample.quest_id):$($Sample.failure)" } else { '' }
        if ($failure -and $failure -eq $State.failure_key) { $State.failure_repeats++ }
        elseif ($failure) { $State.failure_key=$failure; $State.failure_repeats=1 }
        else { $State.failure_key=''; $State.failure_repeats=0 }
        if ($Sample.phase -match '(?i)wait.*respawn|respawn.*wait' -and $age -ge $StallSeconds) { $kind='wait_respawn' }
        elseif ($State.failure_repeats -ge 3 -and $age -ge [math]::Min(60,$StallSeconds)) { $kind='repeated_failure' }
        else {
            $recovery=($old -and $null -ne $old.health_pct -and $Sample.health_pct -gt $old.health_pct -and $Sample.health_pct -lt 95)
            $temporary=($Sample.combat -or $Sample.action -match '(?i)eat|drink|cast' -or $recovery)
            if ($temporary -and -not $State.grace_started_utc) { $State.grace_started_utc=$atText }
            if (-not $temporary) { $State.grace_started_utc=$null }
            $inGrace=($State.grace_started_utc -and ($At-(ConvertTo-ZoneScoutUtc $State.grace_started_utc)).TotalSeconds -lt 60)
            if ($age -ge $StallSeconds -and -not $inGrace) {
                $movement=Get-ZoneScoutMovementKind $State.breadcrumbs $At $StallSeconds
                if ($movement -eq 'stationary') { $kind='stall_stationary' }
                elseif ($movement -eq 'oscillation') { $kind='stall_oscillation' }
                elseif ($age -ge [math]::Max(4*$StallSeconds,900)) { $kind='no_progress' }
            }
        }
    }
    foreach ($other in @($State.active.Keys)) {
        if ($other -eq 'quest_no_credit') { continue }
        if ($other -ne $kind) {
            $events.Add((New-ZoneScoutEvent $State $other 'OBSERVING' $Sample $At 'classification changed; prior suspicion unresolved'))
            $State.active.Remove($other)
        }
    }
    if ($kind) {
        if (-not $State.active.ContainsKey($kind)) {
            $context=[ordered]@{ at_utc=$atText; phase=$Sample.phase; failure=$Sample.failure; quest_id=$Sample.quest_id; map=$Sample.map; x=$Sample.x; y=$Sample.y; z=$Sample.z }
            if (-not $State.first_failure_context) { $State.first_failure_context=$context }
            $State.active[$kind]=@{ first_seen_utc=$atText; recurrence_count=1; first_failure_context=$context }
            $State.incident_totals[$kind]=1+[int]$State.incident_totals[$kind]
            $events.Add((New-ZoneScoutEvent $State $kind 'SUSPECT' $Sample $At "no confirmed progress for $([int]$age)s"))
        } else { $State.active[$kind].recurrence_count++ }
    }
    if ($Sample.online -and $Sample.alive -and $Sample.quest_id -gt 0) {
        $questAge=($At-(ConvertTo-ZoneScoutUtc $State.quest_last_credit_utc)).TotalSeconds
        if ($questAge -ge $QuestNoCreditSeconds) {
            if (-not $State.active.ContainsKey('quest_no_credit')) {
                $context=[ordered]@{ at_utc=$atText; phase=$Sample.phase; failure=$Sample.failure; quest_id=$Sample.quest_id; map=$Sample.map; x=$Sample.x; y=$Sample.y; z=$Sample.z }
                if (-not $State.first_failure_context) { $State.first_failure_context=$context }
                $State.active['quest_no_credit']=@{ first_seen_utc=$atText; recurrence_count=1; first_failure_context=$context }
                $State.incident_totals['quest_no_credit']=1+[int]$State.incident_totals['quest_no_credit']
                $events.Add((New-ZoneScoutEvent $State 'quest_no_credit' 'SUSPECT' $Sample $At "quest $($Sample.quest_id) had no native credit for $([int]$questAge)s"))
            } else { $State.active['quest_no_credit'].recurrence_count++ }
        }
    }
    $State.status=if ($kind -eq 'offline') {'OFFLINE'} elseif ($kind -eq 'dead') {'DEAD'} elseif ($State.active.Count) {'SUSPECT'} else {'OBSERVING'}
    return @($events)
}
function Get-ZoneScoutSummary {
    param([hashtable]$State)
    return [ordered]@{
        guid=$State.guid; name=$State.name; home_zone=$State.home_zone; actual_zone=$null
        observed_map=if ($State.baseline) { $State.baseline.map } else { $null }
        observed_zone_id=if ($State.baseline) { $State.baseline.observed_zone_id } else { $null }
        observed_area_id=if ($State.baseline) { $State.baseline.observed_area_id } else { $null }
        observed_instance_id=if ($State.baseline) { $State.baseline.observed_instance_id } else { $null }
        run_speed_yards_per_second=if ($State.baseline) { $State.baseline.run_speed_yards_per_second } else { $null }
        status=$State.status; session=$State.session; last_seen_utc=$State.last_seen_utc
        last_progress_utc=$State.last_progress_utc; progress_count=$State.progress_count
        quest_context_id=$State.quest_context_id; quest_last_credit_utc=$State.quest_last_credit_utc
        incident_totals=$State.incident_totals; active_incidents=$State.active
        first_failure_context=$State.first_failure_context
        breadcrumbs=@($State.breadcrumbs | Select-Object -Last 12)
    }
}

function Write-ZoneScoutAtomicJson {
    param([string]$Path, $Value)
    $directory=Split-Path -Parent $Path
    $temp=Join-Path $directory ('.'+[IO.Path]::GetFileName($Path)+'.'+[guid]::NewGuid().ToString('N')+'.tmp')
    $json=ConvertTo-Json -InputObject $Value -Depth 18
    [IO.File]::WriteAllText($temp,$json,[Text.UTF8Encoding]::new($false))
    [IO.File]::Move($temp,$Path,$true)
}















