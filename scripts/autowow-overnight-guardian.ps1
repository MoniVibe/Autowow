<#
    Fixed-roster overnight guardian for the two quest parties and three economy workers.
    It never routes, teleports, resets instances, grants progress, or writes databases.
#>
[CmdletBinding()]
param(
    [ValidateRange(1, 24)][int]$DurationHours = 8,
    [ValidateRange(20, 600)][int]$PollSeconds = 60,
    [int]$QuestDirectorPid = 0,
    [switch]$ReleaseLiveMutations,
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$ReceiptPath = '',
    [string]$ControlPath = (Join-Path $PSScriptRoot 'autowow-control.ps1')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$parties = @(
    [ordered]@{ name = 'ember'; leader = [uint32]7; members = @([uint32]12,[uint32]13,[uint32]18,[uint32]20) },
    [ordered]@{ name = 'northstar'; leader = [uint32]10; members = @([uint32]11,[uint32]14,[uint32]15,[uint32]19) }
)
$gatherers = @([uint32]3,[uint32]6,[uint32]49)
$allowed = @($parties | ForEach-Object { @($_.leader) + @($_.members) }) + $gatherers
$allowed = @($allowed | ForEach-Object { [uint32]$_ } | Sort-Object -Unique)
$runId = 'overnight-guardian-' + [DateTimeOffset]::UtcNow.ToString('yyyyMMdd-HHmmssfff')
if ([string]::IsNullOrWhiteSpace($ReceiptPath)) {
    $ReceiptPath = Join-Path $ServerRoot "logs\overnight\$runId.jsonl"
}
[void](New-Item -ItemType Directory -Path (Split-Path -Parent $ReceiptPath) -Force)
$utf8 = [System.Text.UTF8Encoding]::new($false)
$wipeSamples = @{}
$deadSince = @{}
$deadRecoveryBlockedReported = @{}
$deadRecoveryDeployState = @{}
$deadRecoveryDeployCooldownSeconds = 300
$lastPartyRepair = @{}

function Write-GuardianReceipt {
    param([string]$Event, [string]$Status, [object]$Details)
    $row = [ordered]@{ schema='autowow.overnight-guardian.v1'; run_id=$runId; utc=[DateTimeOffset]::UtcNow.ToString('o'); event=$Event; status=$Status; details=$Details }
    [IO.File]::AppendAllText($ReceiptPath, (($row | ConvertTo-Json -Depth 20 -Compress) + [Environment]::NewLine), $utf8)
}

function Invoke-GuardianControl {
    param(
        [Parameter(Mandatory)][ValidateSet('list','activate','deactivate','deploy','recover','party')][string]$Action,
        [uint32]$Guid = 0,
        [uint32[]]$Members = @()
    )
    if ($Action -ne 'list' -and $Guid -notin $allowed) { throw "GUID $Guid is outside guardian allowlist." }
    if ($Action -in @('activate','deactivate','deploy','recover','party') -and -not $ReleaseLiveMutations) {
        throw "Guardian mutation $Action requires -ReleaseLiveMutations."
    }
    if ($Action -eq 'party' -and @($Members | Where-Object { $_ -notin $allowed }).Count -ne 0) {
        throw 'Party request contains a GUID outside guardian allowlist.'
    }
    $arguments = @{ Action=$Action; Port=18787 }
    if ($Action -ne 'list') { $arguments.BotGuid=$Guid }
    if ($Action -eq 'party') { $arguments.MemberGuid=$Members }
    $raw = & $ControlPath @arguments
    $value = $raw | ConvertFrom-Json
    if (-not $value.ok) { throw "Bridge rejected $Action for ${Guid}: $raw" }
    return $value
}

function Get-Bot {
    param([object]$List,[uint32]$Guid)
    $match = @($List.bots | Where-Object { [uint32]$_.guid -eq $Guid } | Select-Object -First 1)
    if ($match.Count -eq 0) { return $null }
    return $match[0]
}

function Test-GathererDeathRecoveryDeployDue {
    param(
        [uint32]$Guid,
        [object]$Bot,
        [uint32[]]$ConfiguredGatherers,
        [hashtable]$RecoveryState,
        [DateTimeOffset]$DeathGeneration,
        [DateTimeOffset]$Now,
        [ValidateRange(20, 3600)][int]$CooldownSeconds = 300,
        [bool]$LiveMutationsReleased = $false
    )
    $key = [string]$Guid
    if ($null -eq $Bot -or $Guid -notin $ConfiguredGatherers -or [uint32]$Bot.guid -ne $Guid) { return $false }
    if ([bool]$Bot.alive) {
        [void]$RecoveryState.Remove($key)
        return $false
    }

    $generationTicks = $DeathGeneration.ToUniversalTime().Ticks
    if ($RecoveryState.ContainsKey($key) -and [int64]$RecoveryState[$key].generation_ticks -ne $generationTicks) {
        [void]$RecoveryState.Remove($key)
    }
    if (-not [bool]$Bot.gather_route.explicit_worker -or -not $LiveMutationsReleased) { return $false }
    if ($RecoveryState.ContainsKey($key)) {
        $elapsedSeconds = ($Now - [DateTimeOffset]$RecoveryState[$key].last_attempt_utc).TotalSeconds
        if ($elapsedSeconds -lt $CooldownSeconds) { return $false }
    }

    $RecoveryState[$key] = [ordered]@{
        generation_ticks=$generationTicks
        last_attempt_utc=$Now.ToUniversalTime()
    }
    return $true
}

Write-GuardianReceipt -Event 'started' -Status 'PASS' -Details ([ordered]@{ duration_hours=$DurationHours; poll_seconds=$PollSeconds; mutable_guids=$allowed; quest_director_pid=$QuestDirectorPid; released=[bool]$ReleaseLiveMutations })
$deadline = [DateTimeOffset]::UtcNow.AddHours($DurationHours)
while ([DateTimeOffset]::UtcNow -lt $deadline) {
    try {
        $list = Invoke-GuardianControl -Action list
        $summary = [System.Collections.Generic.List[object]]::new()

        foreach ($party in $parties) {
            $roster = @([uint32]$party.leader) + @($party.members)
            foreach ($guid in $roster) {
                if ($null -eq (Get-Bot -List $list -Guid $guid)) {
                    [void](Invoke-GuardianControl -Action activate -Guid $guid)
                    Write-GuardianReceipt -Event 'member_activation_queued' -Status 'WARN' -Details ([ordered]@{ team=$party.name; guid=$guid })
                }
            }

            $states = @($roster | ForEach-Object { Get-Bot -List $list -Guid $_ } | Where-Object { $null -ne $_ })
            $alive = @($states | Where-Object { [bool]$_.alive }).Count
            $dead = @($states | Where-Object { -not [bool]$_.alive }).Count
            $groupCount = 0
            $observedLeaderGuid = [uint32]0
            $leaderState = Get-Bot -List $list -Guid ([uint32]$party.leader)
            if ($null -ne $leaderState) {
                $groupCount = [int]$leaderState.group.members
                $observedLeaderGuid = [uint32]$leaderState.group.leader_guid
            }
            $fullRosterGroupStates = @($states | Where-Object {
                [int]$_.group.members -eq 5 -and [uint32]$_.group.leader_guid -in $roster
            })
            $fullRosterLeaders = @($fullRosterGroupStates | ForEach-Object {
                [uint32]$_.group.leader_guid
            } | Sort-Object -Unique)
            $exactRosterGrouped = $states.Count -eq 5 -and $fullRosterGroupStates.Count -eq 5 -and $fullRosterLeaders.Count -eq 1
            if ($exactRosterGrouped) {
                $groupCount = 5
                $observedLeaderGuid = [uint32]$fullRosterLeaders[0]
            }
            $repairLeaderGuid = [uint32]$party.leader
            $subgroupLeader = @($states | Where-Object {
                [int]$_.group.members -gt 0 -and [uint32]$_.group.leader_guid -in $roster
            } | Sort-Object @{ Expression = { [int]$_.group.members }; Descending = $true }, `
                @{ Expression = { [uint32]$_.group.leader_guid }; Descending = $false } | Select-Object -First 1)
            if ($subgroupLeader.Count -eq 1) {
                $repairLeaderGuid = [uint32]$subgroupLeader[0].group.leader_guid
            }
            $repairMembers = @($roster | Where-Object { [uint32]$_ -ne $repairLeaderGuid })

            if ($states.Count -eq 5 -and $dead -eq 5) { $wipeSamples[$party.name] = 1 + [int]$wipeSamples[$party.name] }
            else { $wipeSamples[$party.name] = 0 }
            if ([int]$wipeSamples[$party.name] -ge 2) {
                $recoveryLeaderGuid = if ($exactRosterGrouped) { [uint32]$observedLeaderGuid } else { [uint32]$party.leader }
                $recoveryReady = $exactRosterGrouped
                if (-not $exactRosterGrouped) {
                    try {
                        $repair = Invoke-GuardianControl -Action party -Guid $repairLeaderGuid -Members $repairMembers
                        Write-GuardianReceipt -Event 'pre_recovery_party_repair' -Status 'WARN' -Details ([ordered]@{ team=$party.name; configured_leader=$party.leader; repair_leader=$repairLeaderGuid; observed_leader=$observedLeaderGuid; result=$repair })
                        $lastPartyRepair[$party.name] = [DateTimeOffset]::UtcNow
                        $recoveryLeaderGuid = $repairLeaderGuid
                        $recoveryReady = $true
                    }
                    catch {
                        Write-GuardianReceipt -Event 'pre_recovery_party_repair_deferred' -Status 'WARN' -Details ([ordered]@{ team=$party.name; configured_leader=$party.leader; repair_leader=$repairLeaderGuid; observed_leader=$observedLeaderGuid; error=$_.Exception.Message })
                    }
                }
                if ($recoveryReady) {
                    try {
                        $result = Invoke-GuardianControl -Action recover -Guid $recoveryLeaderGuid
                        Write-GuardianReceipt -Event 'full_wipe_recovery' -Status 'WARN' -Details ([ordered]@{ team=$party.name; leader=$recoveryLeaderGuid; configured_leader=$party.leader; result=$result })
                    }
                    catch {
                        Write-GuardianReceipt -Event 'full_wipe_recovery_deferred' -Status 'WARN' -Details ([ordered]@{ team=$party.name; leader=$recoveryLeaderGuid; configured_leader=$party.leader; error=$_.Exception.Message })
                    }
                }
                else {
                    Write-GuardianReceipt -Event 'full_wipe_recovery_deferred' -Status 'WARN' -Details ([ordered]@{ team=$party.name; leader=$party.leader; error='exact_party_unavailable_after_repair_attempt' })
                }
                $wipeSamples[$party.name] = 0
            }

            if ($states.Count -eq 5 -and -not $exactRosterGrouped) {
                $last = if ($lastPartyRepair.ContainsKey($party.name)) { [DateTimeOffset]$lastPartyRepair[$party.name] } else { [DateTimeOffset]::MinValue }
                if (([DateTimeOffset]::UtcNow - $last).TotalSeconds -ge 300) {
                    try {
                        $result = Invoke-GuardianControl -Action party -Guid $repairLeaderGuid -Members $repairMembers
                        Write-GuardianReceipt -Event 'party_repair' -Status 'WARN' -Details ([ordered]@{ team=$party.name; configured_leader=$party.leader; repair_leader=$repairLeaderGuid; result=$result })
                    }
                    catch {
                        Write-GuardianReceipt -Event 'party_repair_deferred' -Status 'WARN' -Details ([ordered]@{ team=$party.name; configured_leader=$party.leader; repair_leader=$repairLeaderGuid; error=$_.Exception.Message })
                    }
                    $lastPartyRepair[$party.name] = [DateTimeOffset]::UtcNow
                }
            }
            [void]$summary.Add([ordered]@{ lane=$party.name; online=$states.Count; alive=$alive; dead=$dead; group_members=$groupCount; observed_leader_guid=$observedLeaderGuid; configured_leader_guid=[uint32]$party.leader; repair_leader_guid=$repairLeaderGuid; exact_roster_grouped=$exactRosterGrouped })
        }

        foreach ($guid in $gatherers) {
            $bot = Get-Bot -List $list -Guid $guid
            if ($null -eq $bot) {
                [void](Invoke-GuardianControl -Action activate -Guid $guid)
                Write-GuardianReceipt -Event 'gatherer_activation_queued' -Status 'WARN' -Details ([ordered]@{ guid=$guid })
                continue
            }
            $deadKey = [string]$guid
            $observedAt = [DateTimeOffset]::UtcNow
            $isDead = -not [bool]$bot.alive
            if ($isDead) {
                if (-not $deadSince.ContainsKey($deadKey)) {
                    $deadSince[$deadKey] = $observedAt
                    $deadRecoveryBlockedReported.Remove($deadKey)
                }
                else {
                    $deadSeconds = ($observedAt - [DateTimeOffset]$deadSince[$deadKey]).TotalSeconds
                    if ($deadSeconds -ge 300 -and -not $deadRecoveryBlockedReported.ContainsKey($deadKey)) {
                        # A relog can invoke the legacy random-bot revive path, which may relocate the
                        # character. Persistent workers must remain on ordinary corpse/spirit-healer
                        # recovery and fail closed when that path is blocked.
                        Write-GuardianReceipt -Event 'gatherer_dead_recovery_blocked' -Status 'WARN' -Details ([ordered]@{
                            guid=$guid
                            dead_seconds=[math]::Floor($deadSeconds)
                            reason='normal_death_recovery_only_no_relog'
                            forced_relocation_or_relog_issued=0
                        })
                        $deadRecoveryBlockedReported[$deadKey] = $true
                    }
                }
            }
            else {
                if ($deadSince.ContainsKey($deadKey)) {
                    Write-GuardianReceipt -Event 'gatherer_normal_recovery_confirmed' -Status 'PASS' -Details ([ordered]@{
                        guid=$guid
                        alive=$true
                        recovery='observed_by_snapshot'
                    })
                }
                $deadSince.Remove($deadKey)
                $deadRecoveryBlockedReported.Remove($deadKey)
            }
            $deathGeneration = if ($deadSince.ContainsKey($deadKey)) { [DateTimeOffset]$deadSince[$deadKey] } else { $observedAt }
            $deathRecoveryDeployDue = Test-GathererDeathRecoveryDeployDue -Guid $guid -Bot $bot `
                -ConfiguredGatherers $gatherers -RecoveryState $deadRecoveryDeployState `
                -DeathGeneration $deathGeneration -Now $observedAt `
                -CooldownSeconds $deadRecoveryDeployCooldownSeconds -LiveMutationsReleased ([bool]$ReleaseLiveMutations)
            # Deployment is also the durable policy handshake for a released worker: the bridge
            # re-enables no-teleport recovery and invokes Playerbots' ordinary find-corpse action.
            # It is safe and necessary while dead; never relog or relocate the character.
            if ($deathRecoveryDeployDue) {
                $result = Invoke-GuardianControl -Action deploy -Guid $guid
                Write-GuardianReceipt -Event 'gatherer_death_recovery_started' -Status 'WARN' -Details ([ordered]@{
                    guid=$guid
                    death_generation_utc=$deathGeneration.ToUniversalTime().ToString('o')
                    cooldown_seconds=$deadRecoveryDeployCooldownSeconds
                    recovery='ordinary_find_corpse'
                    result=$result
                })
            }
            elseif (-not [bool]$bot.gather_route.explicit_worker) {
                $result = Invoke-GuardianControl -Action deploy -Guid $guid
                Write-GuardianReceipt -Event 'gatherer_redeployed' -Status 'WARN' -Details ([ordered]@{ guid=$guid; result=$result })
            }
            $candidateName = if ($null -ne $bot.gather_route.candidate) { [string]$bot.gather_route.candidate.name } else { '' }
            [void]$summary.Add([ordered]@{ lane='gather'; guid=$guid; online=$true; alive=[bool]$bot.alive; event=$bot.gather_route.event; sequence=$bot.gather_route.event_sequence; misses=$bot.gather_route.miss_count; candidate=$candidateName })
        }

        $directorAlive = $null
        if ($QuestDirectorPid -gt 0) { $directorAlive = $null -ne (Get-Process -Id $QuestDirectorPid -ErrorAction SilentlyContinue) }
        Write-GuardianReceipt -Event 'sample' -Status 'PASS' -Details ([ordered]@{ quest_director_alive=$directorAlive; lanes=@($summary); total_bridge_bots=@($list.bots).Count })
    }
    catch {
        Write-GuardianReceipt -Event 'cycle_error' -Status 'FAIL' -Details ([ordered]@{ error=$_.Exception.Message; line=$_.InvocationInfo.ScriptLineNumber })
    }
    Start-Sleep -Seconds $PollSeconds
}
Write-GuardianReceipt -Event 'completed' -Status 'PASS' -Details ([ordered]@{ receipt=$ReceiptPath })
Write-Output "Overnight guardian receipt: $ReceiptPath"
