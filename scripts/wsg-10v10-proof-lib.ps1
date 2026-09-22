Set-StrictMode -Version Latest

function Get-Wsg10v10Property {
    [CmdletBinding()]
    param(
        [AllowNull()][object]$Object,
        [Parameter(Mandatory)][string[]]$Name
    )

    if ($null -eq $Object) { return $null }
    foreach ($candidate in $Name) {
        if ($Object -is [System.Collections.IDictionary] -and $Object.Contains($candidate)) {
            return $Object[$candidate]
        }
        $property = $Object.PSObject.Properties[$candidate]
        if ($null -ne $property) { return $property.Value }
    }
    return $null
}

function Test-Wsg10v10ExistingFixturePreflight {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][uint32[]]$OrderedRosterGuid,
        [Parameter(Mandatory)][object[]]$EvidenceRows,
        [ValidateRange(1, 80)][int]$ExpectedLevel = 80,
        [ValidateRange(0, 7)][int]$ExpectedBracket = 7,
        [ValidateRange(0, 5)][int]$ExpectedQuality = 3,
        [ValidateSet('poor', 'normal', 'uncommon', 'rare', 'epic', 'legendary')][string]$ExpectedQualityName = 'rare'
    )

    $expectedRoster = @($OrderedRosterGuid | ForEach-Object { [uint32]$_ })
    $rows = @($EvidenceRows)
    $reasons = New-Object System.Collections.Generic.List[string]
    $observedRoster = @($rows | ForEach-Object { ConvertTo-Wsg10v10Int (Get-Wsg10v10Property -Object $_ -Name @('guid')) })

    if ($expectedRoster.Count -ne 20) { $reasons.Add("expected_roster_count:$($expectedRoster.Count)") }
    if ($rows.Count -ne $expectedRoster.Count) { $reasons.Add("evidence_count:$($rows.Count)") }
    if (@($observedRoster | Where-Object { $null -eq $_ }).Count -ne 0) { $reasons.Add('missing_status_guid') }
    if (@($observedRoster | Sort-Object -Unique).Count -ne $observedRoster.Count) { $reasons.Add('duplicate_status_guid') }
    if (($observedRoster -join ',') -ne ($expectedRoster -join ',')) { $reasons.Add('status_roster_mismatch') }

    foreach ($row in $rows) {
        $guid = ConvertTo-Wsg10v10Int (Get-Wsg10v10Property -Object $row -Name @('guid'))
        $guidLabel = if ($null -eq $guid) { 'unknown' } else { [string]$guid }
        $statusGuid = ConvertTo-Wsg10v10Int (Get-Wsg10v10Property -Object $row -Name @('status_guid', 'guid'))
        $statusOk = Get-Wsg10v10Property -Object $row -Name @('status_ok')
        $statusOperation = [string](Get-Wsg10v10Property -Object $row -Name @('status_operation', 'operation'))
        $online = Get-Wsg10v10Property -Object $row -Name @('online')
        $fixtureEligible = Get-Wsg10v10Property -Object $row -Name @('fixture_eligible')
        $level = ConvertTo-Wsg10v10Int (Get-Wsg10v10Property -Object $row -Name @('level'))
        $bracket = ConvertTo-Wsg10v10Int (Get-Wsg10v10Property -Name @('bracket_index') -Object $row)
        $specIndex = ConvertTo-Wsg10v10Int (Get-Wsg10v10Property -Name @('spec_index') -Object $row)
        $specName = [string](Get-Wsg10v10Property -Name @('spec_name') -Object $row)
        $equippedSlots = ConvertTo-Wsg10v10Int (Get-Wsg10v10Property -Name @('equipped_slots') -Object $row)
        $equipmentSlotsChecked = ConvertTo-Wsg10v10Int (Get-Wsg10v10Property -Name @('equipment_slots_checked') -Object $row)
        $expectedQualitySlots = ConvertTo-Wsg10v10Int (Get-Wsg10v10Property -Name @('expected_quality_slots') -Object $row)
        $otherQualitySlots = ConvertTo-Wsg10v10Int (Get-Wsg10v10Property -Name @('other_quality_slots') -Object $row)
        $initialized = Get-Wsg10v10Property -Name @('initialized') -Object $row

        if ($online -ne $true -or $fixtureEligible -ne $true) { $reasons.Add("online_or_eligible_mismatch:$guidLabel") }
        if ($statusOk -ne $true) { $reasons.Add("fixture_status_not_ok:$guidLabel") }
        if ($statusOperation -ne 'fixture_status') { $reasons.Add("fixture_status_operation_mismatch:$guidLabel") }
        if ($null -eq $guid -or $null -eq $statusGuid -or $statusGuid -ne $guid) { $reasons.Add("fixture_status_guid_mismatch:$guidLabel") }
        if ($level -ne $ExpectedLevel) { $reasons.Add("level_mismatch:$guidLabel") }
        if ($bracket -ne $ExpectedBracket) { $reasons.Add("bracket_mismatch:$guidLabel") }
        if ($null -eq $specIndex -or $specIndex -lt 0 -or $specIndex -gt 19 -or
            [string]::IsNullOrWhiteSpace($specName) -or $specName -in @('unknown', 'unconfigured')) {
            $reasons.Add("uninitialized_or_missing_spec:$guidLabel")
        }
        if ($initialized -ne $true) { $reasons.Add("initialized_evidence_missing:$guidLabel") }
        if ($null -eq $equippedSlots -or $equippedSlots -le 0 -or
            $null -eq $equipmentSlotsChecked -or $equipmentSlotsChecked -le 0 -or
            $equippedSlots -lt ($equipmentSlotsChecked - 1) -or
            $null -eq $expectedQualitySlots -or $expectedQualitySlots -lt ($equippedSlots - 1) -or
            $otherQualitySlots -ne 0) {
            $reasons.Add("gear_quality_mismatch:$ExpectedQualityName")
        }
    }

    $passed = $reasons.Count -eq 0
    return [pscustomobject][ordered]@{
        status = if ($passed) { 'PASS' } else { 'FAIL' }
        passed = $passed
        mode = 'skip_fixture_init_preflight'
        spec_policy = 'preserve_existing'
        expected_level = $ExpectedLevel
        expected_bracket = $ExpectedBracket
        expected_quality = $ExpectedQuality
        expected_quality_name = $ExpectedQualityName
        evidence_count = $rows.Count
        online_eligible_count = @($rows | Where-Object {
            (Get-Wsg10v10Property -Name @('online') -Object $_) -eq $true -and
            (Get-Wsg10v10Property -Name @('fixture_eligible') -Object $_) -eq $true
        }).Count
        initialized_count = @($rows | Where-Object { (Get-Wsg10v10Property -Name @('initialized') -Object $_) -eq $true }).Count
        reasons = @($reasons)
        members = $rows
    }
}

function ConvertTo-Wsg10v10Int {
    [CmdletBinding()]
    param([AllowNull()][object]$Value)

    $parsed = 0L
    if ($null -eq $Value -or -not [long]::TryParse([string]$Value, [ref]$parsed)) { return $null }
    return $parsed
}

function ConvertTo-Wsg10v10Faction {
    [CmdletBinding()]
    param([AllowNull()][object]$Value)

    if ($null -eq $Value) { return '' }
    switch (([string]$Value).Trim().ToLowerInvariant()) {
        { $_ -in @('alliance', 'a', '0', '469') } { return 'Alliance' }
        { $_ -in @('horde', 'h', '1', '67') } { return 'Horde' }
        default { return [string]$Value }
    }
}

function ConvertTo-Wsg10v10FlagTelemetry {
    [CmdletBinding()]
    param([AllowNull()][object]$Source)

    if ($null -eq $Source) {
        return [pscustomobject]@{ state = $null; captures = $null; returns = $null }
    }
    return [pscustomobject]@{
        state = Get-Wsg10v10Property -Object $Source -Name @('state', 'flag_state', 'status')
        captures = ConvertTo-Wsg10v10Int (Get-Wsg10v10Property -Object $Source -Name @('captures', 'flag_captures', 'capture_count'))
        returns = ConvertTo-Wsg10v10Int (Get-Wsg10v10Property -Object $Source -Name @('returns', 'flag_returns', 'return_count'))
    }
}

function ConvertTo-Wsg10v10CanonicalStatus {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][object]$Response,
        [datetime]$ObservedAtUtc = ([datetime]::UtcNow)
    )

    $battle = Get-Wsg10v10Property -Object $Response -Name @('battleground', 'match')
    $rootInstance = ConvertTo-Wsg10v10Int (Get-Wsg10v10Property -Object $Response -Name @('instance_id', 'battleground_instance_id'))
    if ($null -eq $rootInstance -and $null -ne $battle) {
        $rootInstance = ConvertTo-Wsg10v10Int (Get-Wsg10v10Property -Object $battle -Name @('instance_id', 'id'))
    }
    $state = Get-Wsg10v10Property -Object $Response -Name @('battleground_state', 'state', 'status')
    if ($null -eq $state -and $null -ne $battle) {
        $state = Get-Wsg10v10Property -Object $battle -Name @('state', 'status')
    }

    $score = Get-Wsg10v10Property -Object $Response -Name @('score', 'scores')
    if ($null -eq $score -and $null -ne $battle) { $score = Get-Wsg10v10Property -Object $battle -Name @('score', 'scores') }
    $allianceScore = ConvertTo-Wsg10v10Int (Get-Wsg10v10Property -Object $Response -Name @('alliance_score'))
    $hordeScore = ConvertTo-Wsg10v10Int (Get-Wsg10v10Property -Object $Response -Name @('horde_score'))
    if ($null -ne $score) {
        if ($null -eq $allianceScore) { $allianceScore = ConvertTo-Wsg10v10Int (Get-Wsg10v10Property -Object $score -Name @('alliance', 'alliance_score')) }
        if ($null -eq $hordeScore) { $hordeScore = ConvertTo-Wsg10v10Int (Get-Wsg10v10Property -Object $score -Name @('horde', 'horde_score')) }
    }
    $winner = Get-Wsg10v10Property -Object $Response -Name @('winner', 'winning_faction')
    if ($null -eq $winner -and $null -ne $battle) { $winner = Get-Wsg10v10Property -Object $battle -Name @('winner', 'winning_faction') }

    $flags = Get-Wsg10v10Property -Object $Response -Name @('flags', 'flag_state')
    if ($null -eq $flags -and $null -ne $battle) { $flags = Get-Wsg10v10Property -Object $battle -Name @('flags', 'flag_state') }
    $allianceFlag = ConvertTo-Wsg10v10FlagTelemetry -Source (Get-Wsg10v10Property -Object $flags -Name @('alliance', 'alliance_flag'))
    $hordeFlag = ConvertTo-Wsg10v10FlagTelemetry -Source (Get-Wsg10v10Property -Object $flags -Name @('horde', 'horde_flag'))

    $rawRoster = Get-Wsg10v10Property -Object $Response -Name @('roster', 'players', 'members')
    if ($null -eq $rawRoster -and $null -ne $battle) { $rawRoster = Get-Wsg10v10Property -Object $battle -Name @('roster', 'players', 'members') }
    $roster = @()
    foreach ($player in @($rawRoster)) {
        if ($null -eq $player) { continue }
        $playerScore = Get-Wsg10v10Property -Object $player -Name @('score', 'statistics', 'stats')
        $playerFlag = Get-Wsg10v10Property -Object $player -Name @('flag', 'flag_telemetry')
        $playerInstance = ConvertTo-Wsg10v10Int (Get-Wsg10v10Property -Object $player -Name @('instance_id', 'battleground_instance_id'))
        if ($null -eq $playerInstance) { $playerInstance = $rootInstance }
        $kills = ConvertTo-Wsg10v10Int (Get-Wsg10v10Property -Object $player -Name @('kills', 'honorable_kills', 'killing_blows'))
        $deaths = ConvertTo-Wsg10v10Int (Get-Wsg10v10Property -Object $player -Name @('deaths'))
        $damage = ConvertTo-Wsg10v10Int (Get-Wsg10v10Property -Object $player -Name @('damage', 'damage_done'))
        $healing = ConvertTo-Wsg10v10Int (Get-Wsg10v10Property -Object $player -Name @('healing', 'healing_done'))
        if ($null -ne $playerScore) {
            if ($null -eq $kills) { $kills = ConvertTo-Wsg10v10Int (Get-Wsg10v10Property -Object $playerScore -Name @('kills', 'honorable_kills', 'killing_blows')) }
            if ($null -eq $deaths) { $deaths = ConvertTo-Wsg10v10Int (Get-Wsg10v10Property -Object $playerScore -Name @('deaths')) }
            if ($null -eq $damage) { $damage = ConvertTo-Wsg10v10Int (Get-Wsg10v10Property -Object $playerScore -Name @('damage', 'damage_done')) }
            if ($null -eq $healing) { $healing = ConvertTo-Wsg10v10Int (Get-Wsg10v10Property -Object $playerScore -Name @('healing', 'healing_done')) }
        }
        $flagState = Get-Wsg10v10Property -Object $player -Name @('flag_state')
        $flagCaptures = ConvertTo-Wsg10v10Int (Get-Wsg10v10Property -Object $player -Name @('flag_captures', 'captures'))
        $flagReturns = ConvertTo-Wsg10v10Int (Get-Wsg10v10Property -Object $player -Name @('flag_returns', 'returns'))
        if ($null -ne $playerFlag) {
            if ($null -eq $flagState) { $flagState = Get-Wsg10v10Property -Object $playerFlag -Name @('state', 'status') }
            if ($null -eq $flagCaptures) { $flagCaptures = ConvertTo-Wsg10v10Int (Get-Wsg10v10Property -Object $playerFlag -Name @('captures', 'capture_count')) }
            if ($null -eq $flagReturns) { $flagReturns = ConvertTo-Wsg10v10Int (Get-Wsg10v10Property -Object $playerFlag -Name @('returns', 'return_count')) }
        }
        $roster += [pscustomobject]@{
            guid = ConvertTo-Wsg10v10Int (Get-Wsg10v10Property -Object $player -Name @('guid', 'character_guid'))
            faction = ConvertTo-Wsg10v10Faction (Get-Wsg10v10Property -Object $player -Name @('faction', 'team', 'side'))
            instance_id = $playerInstance
            queue_state = Get-Wsg10v10Property -Object $player -Name @('queue_state', 'queue_status')
            battleground_state = Get-Wsg10v10Property -Object $player -Name @('battleground_state', 'state', 'status')
            kills = $kills
            deaths = $deaths
            damage = $damage
            healing = $healing
            flag_state = $flagState
            flag_captures = $flagCaptures
            flag_returns = $flagReturns
        }
    }

    return [pscustomobject]@{
        observed_at_utc = $ObservedAtUtc.ToString('o')
        ok = [bool](Get-Wsg10v10Property -Object $Response -Name @('ok'))
        instance_id = $rootInstance
        battleground_state = if ($null -eq $state) { '' } else { [string]$state }
        roster = @($roster)
        flags = [pscustomobject]@{ alliance = $allianceFlag; horde = $hordeFlag }
        score = [pscustomobject]@{ alliance = $allianceScore; horde = $hordeScore }
        winner = if ($null -eq $winner) { '' } else { [string]$winner }
    }
}

function Test-Wsg10v10InstanceStatus {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][object]$Status,
        [Parameter(Mandatory)][uint32[]]$OrderedRosterGuid
    )

    $reasons = New-Object System.Collections.Generic.List[string]
    if ($OrderedRosterGuid.Count -ne 20 -or @($OrderedRosterGuid | Sort-Object -Unique).Count -ne 20) {
        $reasons.Add('expected_roster_is_not_exactly_20_unique_guids')
    }
    $roster = @($Status.roster)
    $observedGuids = @($roster | ForEach-Object { [uint32]$_.guid })
    if ($roster.Count -ne 20) { $reasons.Add("observed_roster_count=$($roster.Count)") }
    if ((@($observedGuids | Sort-Object) -join ',') -ne (@($OrderedRosterGuid | Sort-Object) -join ',')) {
        $reasons.Add('observed_roster_guid_set_does_not_match')
    }
    $alliance = @($roster | Where-Object faction -eq 'Alliance')
    $horde = @($roster | Where-Object faction -eq 'Horde')
    if ($alliance.Count -ne 10 -or $horde.Count -ne 10) { $reasons.Add("observed_factions=$($alliance.Count)+$($horde.Count)") }
    for ($index = 0; $index -lt [Math]::Min(20, $roster.Count); $index++) {
        $expectedFaction = if ($index -lt 10) { 'Alliance' } else { 'Horde' }
        $expectedGuid = [uint32]$OrderedRosterGuid[$index]
        $member = @($roster | Where-Object { [uint32]$_.guid -eq $expectedGuid })
        if ($member.Count -ne 1 -or $member[0].faction -ne $expectedFaction) {
            $reasons.Add("guid_${expectedGuid}_faction_mismatch")
        }
    }
    $instances = @($roster | ForEach-Object { ConvertTo-Wsg10v10Int $_.instance_id } | Where-Object { $null -ne $_ } | Sort-Object -Unique)
    if ($instances.Count -ne 1 -or [long]$instances[0] -le 0) { $reasons.Add('roster_does_not_share_one_nonzero_instance') }
    if ($null -eq $Status.instance_id -or [long]$Status.instance_id -le 0) { $reasons.Add('root_instance_id_is_not_nonzero') }
    elseif ($instances.Count -eq 1 -and [long]$Status.instance_id -ne [long]$instances[0]) { $reasons.Add('root_and_roster_instance_ids_differ') }

    return [pscustomobject]@{
        status = if ($reasons.Count -eq 0) { 'PASS' } else { 'FAIL' }
        passed = $reasons.Count -eq 0
        reasons = @($reasons)
        instance_id = if ($instances.Count -eq 1) { [long]$instances[0] } else { 0 }
        roster_count = $roster.Count
        alliance_count = $alliance.Count
        horde_count = $horde.Count
    }
}

function Get-Wsg10v10GameplayDeltas {
    [CmdletBinding()]
    param([Parameter(Mandatory)][object[]]$Samples)

    if ($Samples.Count -eq 0) {
        return [pscustomobject]@{ kills = 0; deaths = 0; damage = 0; healing = 0; flag_state_changes = 0; flag_captures = 0; flag_returns = 0; alliance_score = 0; horde_score = 0 }
    }
    $first = $Samples[0]
    $last = $Samples[$Samples.Count - 1]
    $kills = 0L
    $deaths = 0L
    $damage = 0L
    $healing = 0L
    $captures = 0L
    $returns = 0L
    foreach ($finalMember in @($last.roster)) {
        $initialMember = @($first.roster | Where-Object { [long]$_.guid -eq [long]$finalMember.guid } | Select-Object -First 1)
        if ($initialMember.Count -ne 1) { continue }
        foreach ($metric in @('kills', 'deaths', 'damage', 'healing', 'flag_captures', 'flag_returns')) {
            $startValue = ConvertTo-Wsg10v10Int $initialMember[0].$metric
            $endValue = ConvertTo-Wsg10v10Int $finalMember.$metric
            if ($null -eq $startValue -or $null -eq $endValue) { continue }
            $delta = [Math]::Max(0L, $endValue - $startValue)
            switch ($metric) {
                'kills' { $kills += $delta }
                'deaths' { $deaths += $delta }
                'damage' { $damage += $delta }
                'healing' { $healing += $delta }
                'flag_captures' { $captures += $delta }
                'flag_returns' { $returns += $delta }
            }
        }
    }
    if ($captures -eq 0) {
        foreach ($side in @('alliance', 'horde')) {
            $startValue = ConvertTo-Wsg10v10Int $first.flags.$side.captures
            $endValue = ConvertTo-Wsg10v10Int $last.flags.$side.captures
            if ($null -ne $startValue -and $null -ne $endValue) { $captures += [Math]::Max(0L, $endValue - $startValue) }
        }
    }
    if ($returns -eq 0) {
        foreach ($side in @('alliance', 'horde')) {
            $startValue = ConvertTo-Wsg10v10Int $first.flags.$side.returns
            $endValue = ConvertTo-Wsg10v10Int $last.flags.$side.returns
            if ($null -ne $startValue -and $null -ne $endValue) { $returns += [Math]::Max(0L, $endValue - $startValue) }
        }
    }
    $stateChanges = 0
    for ($sampleIndex = 1; $sampleIndex -lt $Samples.Count; $sampleIndex++) {
        foreach ($side in @('alliance', 'horde')) {
            $previous = [string]$Samples[$sampleIndex - 1].flags.$side.state
            $current = [string]$Samples[$sampleIndex].flags.$side.state
            if (-not [string]::IsNullOrWhiteSpace($previous) -and -not [string]::IsNullOrWhiteSpace($current) -and $previous -ne $current) { $stateChanges++ }
        }
    }
    $allianceStart = ConvertTo-Wsg10v10Int $first.score.alliance
    $allianceEnd = ConvertTo-Wsg10v10Int $last.score.alliance
    $hordeStart = ConvertTo-Wsg10v10Int $first.score.horde
    $hordeEnd = ConvertTo-Wsg10v10Int $last.score.horde
    return [pscustomobject]@{
        kills = $kills
        deaths = $deaths
        damage = $damage
        healing = $healing
        flag_state_changes = $stateChanges
        flag_captures = $captures
        flag_returns = $returns
        alliance_score = if ($null -ne $allianceStart -and $null -ne $allianceEnd) { [Math]::Max(0L, $allianceEnd - $allianceStart) } else { 0 }
        horde_score = if ($null -ne $hordeStart -and $null -ne $hordeEnd) { [Math]::Max(0L, $hordeEnd - $hordeStart) } else { 0 }
    }
}

function Get-Wsg10v10GameplayVerdict {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][object[]]$Samples,
        [Parameter(Mandatory)][bool]$InstanceRosterMaintained
    )

    $reasons = New-Object System.Collections.Generic.List[string]
    if ($Samples.Count -lt 2) { $reasons.Add('fewer_than_two_in_instance_samples') }
    if (-not $InstanceRosterMaintained) { $reasons.Add('exact_instance_roster_was_not_maintained') }
    if ($Samples.Count -eq 0) {
        return [pscustomobject]@{ status = 'FAIL'; passed = $false; reasons = @($reasons); deltas = Get-Wsg10v10GameplayDeltas -Samples @(); final_score = $null; winner = '' }
    }
    $first = $Samples[0]
    $last = $Samples[$Samples.Count - 1]
    $killDeathFields = @($first.roster | Where-Object { $null -eq $_.kills -or $null -eq $_.deaths }).Count -eq 0 -and
                       @($last.roster | Where-Object { $null -eq $_.kills -or $null -eq $_.deaths }).Count -eq 0
    if (-not $killDeathFields) { $reasons.Add('kill_or_death_telemetry_missing') }
    $flagFields = (($null -ne $first.flags.alliance.state -and $null -ne $first.flags.horde.state -and
                    $null -ne $last.flags.alliance.state -and $null -ne $last.flags.horde.state) -or
                   (@($first.roster | Where-Object { $null -eq $_.flag_state }).Count -eq 0 -and
                    @($last.roster | Where-Object { $null -eq $_.flag_state }).Count -eq 0))
    $captureReturnFields = (($null -ne $first.flags.alliance.captures -and $null -ne $first.flags.horde.captures -and
                             $null -ne $first.flags.alliance.returns -and $null -ne $first.flags.horde.returns -and
                             $null -ne $last.flags.alliance.captures -and $null -ne $last.flags.horde.captures -and
                             $null -ne $last.flags.alliance.returns -and $null -ne $last.flags.horde.returns) -or
                            (@($first.roster | Where-Object { $null -eq $_.flag_captures -or $null -eq $_.flag_returns }).Count -eq 0 -and
                             @($last.roster | Where-Object { $null -eq $_.flag_captures -or $null -eq $_.flag_returns }).Count -eq 0))
    if (-not $flagFields) { $reasons.Add('flag_state_telemetry_missing') }
    if (-not $captureReturnFields) { $reasons.Add('flag_capture_or_return_telemetry_missing') }
    $deltas = Get-Wsg10v10GameplayDeltas -Samples $Samples
    if (($deltas.kills + $deltas.deaths) -le 0) { $reasons.Add('no_kill_or_death_delta_observed') }
    if (($deltas.flag_state_changes + $deltas.flag_captures + $deltas.flag_returns) -le 0) { $reasons.Add('no_flag_state_capture_or_return_delta_observed') }
    $terminal = $last.battleground_state -match '^(complete|completed|ended|finished|wait_leave|closed)$'
    if (-not $terminal) { $reasons.Add("final_state_is_not_terminal:$($last.battleground_state)") }
    if ($null -eq $last.score.alliance -or $null -eq $last.score.horde) { $reasons.Add('final_score_missing') }
    if ([string]::IsNullOrWhiteSpace([string]$last.winner) -or [string]$last.winner -match '^(none|unknown|pending)$') { $reasons.Add('final_winner_missing') }
    $objectiveGameplayObserved = $InstanceRosterMaintained -and $killDeathFields -and $flagFields -and
        $captureReturnFields -and (($deltas.kills + $deltas.deaths) -gt 0) -and
        (($deltas.flag_state_changes + $deltas.flag_captures + $deltas.flag_returns) -gt 0)
    $status = if ($reasons.Count -eq 0) { 'PASS' } elseif ($objectiveGameplayObserved) { 'PARTIAL' } else { 'FAIL' }
    return [pscustomobject]@{
        status = $status
        passed = $reasons.Count -eq 0
        reasons = @($reasons)
        deltas = $deltas
        final_score = [pscustomobject]@{ alliance = $last.score.alliance; horde = $last.score.horde }
        winner = [string]$last.winner
        final_state = [string]$last.battleground_state
        sample_count = $Samples.Count
    }
}
