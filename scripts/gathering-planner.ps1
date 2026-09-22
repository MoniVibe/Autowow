[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$RosterPath = '',
    [string]$LeaguePath = '',
    [string]$TelemetryPath = '',
    [string]$OutputPath = '',
    [ValidateRange(1, 10)]
    [int]$WorkersPerTeam = 10,
    [switch]$WorkersOnly,
    [switch]$AsJson
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:PrimaryProfessions = [ordered]@{
    '164' = 'Blacksmithing'
    '165' = 'Leatherworking'
    '171' = 'Alchemy'
    '182' = 'Herbalism'
    '186' = 'Mining'
    '197' = 'Tailoring'
    '202' = 'Engineering'
    '333' = 'Enchanting'
    '393' = 'Skinning'
    '755' = 'Jewelcrafting'
    '773' = 'Inscription'
}

$script:ProfessionAliases = @{
    '164' = 'Blacksmithing'
    '165' = 'Leatherworking'
    '171' = 'Alchemy'
    '182' = 'Herbalism'
    '186' = 'Mining'
    '197' = 'Tailoring'
    '202' = 'Engineering'
    '333' = 'Enchanting'
    '393' = 'Skinning'
    '755' = 'Jewelcrafting'
    '773' = 'Inscription'
    'blacksmithing' = 'Blacksmithing'
    'blacksmith' = 'Blacksmithing'
    'leatherworking' = 'Leatherworking'
    'leatherworking ' = 'Leatherworking'
    'alchemy' = 'Alchemy'
    'herbalism' = 'Herbalism'
    'herb' = 'Herbalism'
    'mining' = 'Mining'
    'tailoring' = 'Tailoring'
    'engineering' = 'Engineering'
    'enchanting' = 'Enchanting'
    'enchantment' = 'Enchanting'
    'skinning' = 'Skinning'
    'jewelcrafting' = 'Jewelcrafting'
    'jewel crafting' = 'Jewelcrafting'
    'inscription' = 'Inscription'
}

$script:SecondaryProfessionAliases = @{
    '129' = 'First Aid'
    '185' = 'Cooking'
    '356' = 'Fishing'
    'first aid' = 'First Aid'
    'firstaid' = 'First Aid'
    'cooking' = 'Cooking'
    'fishing' = 'Fishing'
}

$script:AssignmentCatalog = @(
    [ordered]@{ slot = 1; key = 'herbalism-alchemy'; gather = 'Herbalism'; craft = 'Alchemy'; dependency = 'Alchemy consumes Herbalism materials' }
    [ordered]@{ slot = 2; key = 'mining-engineering'; gather = 'Mining'; craft = 'Engineering'; dependency = 'Engineering consumes Mining materials' }
    [ordered]@{ slot = 3; key = 'skinning-leatherworking'; gather = 'Skinning'; craft = 'Leatherworking'; dependency = 'Leatherworking consumes Skinning materials' }
    [ordered]@{ slot = 4; key = 'herbalism-inscription'; gather = 'Herbalism'; craft = 'Inscription'; dependency = 'Inscription consumes Herbalism materials' }
    [ordered]@{ slot = 5; key = 'mining-jewelcrafting'; gather = 'Mining'; craft = 'Jewelcrafting'; dependency = 'Jewelcrafting consumes Mining materials' }
    [ordered]@{ slot = 6; key = 'mining-blacksmithing'; gather = 'Mining'; craft = 'Blacksmithing'; dependency = 'Blacksmithing consumes Mining materials' }
    [ordered]@{ slot = 7; key = 'tailoring-enchanting'; gather = 'Tailoring'; craft = 'Enchanting'; dependency = 'Tailoring supplies cloth; Enchanting supplies disenchanting and enchants' }
    [ordered]@{ slot = 8; key = 'herbalism-alchemy-2'; gather = 'Herbalism'; craft = 'Alchemy'; dependency = 'Redundant Alchemy capacity for consumable throughput' }
    [ordered]@{ slot = 9; key = 'mining-engineering-2'; gather = 'Mining'; craft = 'Engineering'; dependency = 'Redundant Engineering capacity for utility throughput' }
    [ordered]@{ slot = 10; key = 'skinning-leatherworking-2'; gather = 'Skinning'; craft = 'Leatherworking'; dependency = 'Redundant Leatherworking capacity for armor throughput' }
)

function Get-OptionalProperty {
    param(
        [AllowNull()]
        [object]$InputObject,
        [Parameter(Mandatory)]
        [string]$Name,
        [AllowNull()]
        [object]$Default = $null
    )

    if ($null -eq $InputObject) {
        return $Default
    }

    $property = $InputObject.PSObject.Properties[$Name]
    if ($null -eq $property -or $null -eq $property.Value) {
        return $Default
    }

    return $property.Value
}

function ConvertTo-Boolean {
    param(
        [AllowNull()]
        [object]$Value,
        [bool]$Default = $true
    )

    if ($null -eq $Value) {
        return $Default
    }

    if ($Value -is [bool]) {
        return [bool]$Value
    }

    $text = "$Value".Trim().ToLowerInvariant()
    if ($text -in @('0', 'false', 'no', 'off', 'inactive')) {
        return $false
    }
    if ($text -in @('1', 'true', 'yes', 'on', 'active')) {
        return $true
    }

    return $Default
}

function ConvertTo-NullableInt {
    param([AllowNull()][object]$Value)

    if ($null -eq $Value -or [string]::IsNullOrWhiteSpace("$Value")) {
        return $null
    }

    $number = 0L
    if ([long]::TryParse("$Value", [Globalization.NumberStyles]::Integer, [Globalization.CultureInfo]::InvariantCulture, [ref]$number)) {
        return $number
    }

    return $null
}

function Normalize-ProfessionName {
    param([AllowNull()][object]$Value)

    if ($null -eq $Value) {
        return $null
    }

    $key = "$Value".Trim().ToLowerInvariant()
    $key = $key -replace '[\-_]+', ' '
    $key = [regex]::Replace($key, '\s+', ' ')
    if ($script:ProfessionAliases.ContainsKey($key)) {
        return $script:ProfessionAliases[$key]
    }
    if ($script:SecondaryProfessionAliases.ContainsKey($key)) {
        return $script:SecondaryProfessionAliases[$key]
    }

    return $null
}

function Get-ProfessionValues {
    param([AllowNull()][object]$Member)

    $names = [System.Collections.Generic.List[string]]::new()
    $propertyNames = @(
        'profession_one',
        'profession_two',
        'professionOne',
        'professionTwo',
        'professions',
        'primary_professions',
        'observed_primary_professions'
    )

    foreach ($propertyName in $propertyNames) {
        $value = Get-OptionalProperty -InputObject $Member -Name $propertyName
        foreach ($entry in @($value)) {
            if ($entry -is [System.Collections.IDictionary]) {
                foreach ($dictionaryValue in $entry.Values) {
                    $normalized = Normalize-ProfessionName -Value $dictionaryValue
                    if ($null -ne $normalized -and -not $names.Contains($normalized)) {
                        [void]$names.Add($normalized)
                    }
                }
                continue
            }

            $normalized = Normalize-ProfessionName -Value $entry
            if ($null -ne $normalized -and -not $names.Contains($normalized)) {
                [void]$names.Add($normalized)
            }
        }
    }

    return @($names)
}

function Test-PrimaryProfession {
    param([AllowNull()][string]$Profession)

    return $script:PrimaryProfessions.Values -contains $Profession
}

function Get-ProfessionPairKey {
    param([AllowNull()][object[]]$Professions)

    $primary = @($Professions | Where-Object { Test-PrimaryProfession -Profession $_ } | Select-Object -Unique)
    if ($primary.Count -ne 2) {
        return $null
    }

    return (@($primary | Sort-Object) -join '|')
}

function Test-ProfessionPair {
    param([AllowNull()][object[]]$Professions)

    $pairKey = Get-ProfessionPairKey -Professions $Professions
    if ($null -eq $pairKey) {
        return $false
    }

    $validKeys = @(
        'Alchemy|Herbalism',
        'Engineering|Mining',
        'Leatherworking|Skinning',
        'Herbalism|Inscription',
        'Jewelcrafting|Mining',
        'Blacksmithing|Mining',
        'Enchanting|Tailoring'
    )

    return $validKeys -contains $pairKey
}

function Get-CanonicalTeamId {
    param(
        [AllowNull()][object]$Team,
        [AllowNull()][object]$Affiliation
    )

    $teamText = if ($null -eq $Team) { '' } else { "$Team".Trim().ToLowerInvariant() }
    if ($teamText -in @('northstar', 'north-star', 'north', 'alliance', '1')) {
        return 'northstar'
    }
    if ($teamText -in @('ember', 'ember-horde', 'horde', '2')) {
        return 'ember'
    }

    $affiliationText = if ($null -eq $Affiliation) { '' } else { "$Affiliation".Trim().ToLowerInvariant() }
    if ($affiliationText -eq 'alliance') {
        return 'northstar'
    }
    if ($affiliationText -eq 'horde') {
        return 'ember'
    }

    return $teamText
}

function Get-RosterMembersFromObject {
    param([Parameter(Mandatory)][object]$RosterObject)

    if ($RosterObject -is [System.Collections.IEnumerable] -and $RosterObject -isnot [string] -and $RosterObject.PSObject.Properties.Count -eq 0) {
        return @($RosterObject)
    }

    foreach ($propertyName in @('members', 'characters', 'roster', 'data')) {
        $value = Get-OptionalProperty -InputObject $RosterObject -Name $propertyName
        if ($null -ne $value) {
            return @($value)
        }
    }

    return @($RosterObject)
}

function Read-Roster {
    if (-not [string]::IsNullOrWhiteSpace($RosterPath)) {
        if (-not (Test-Path -LiteralPath $RosterPath -PathType Leaf)) {
            throw "Roster fixture not found: $RosterPath"
        }
        $json = Get-Content -LiteralPath $RosterPath -Raw | ConvertFrom-Json
        return @($json | ForEach-Object { Get-RosterMembersFromObject -RosterObject $_ })
    }

    $leagueScript = Join-Path $PSScriptRoot 'league-simulation.ps1'
    if (-not (Test-Path -LiteralPath $leagueScript -PathType Leaf)) {
        throw "Existing league roster helper not found: $leagueScript"
    }

    # PowerShell array splatting does not preserve named-parameter binding for
    # a script invocation here: `-Action` was being consumed as the value of
    # Action. Invoke the existing read-only roster surface with explicit named
    # arguments so its ValidateSet receives `roster`.
    $rawOutput = & $leagueScript -Action roster -ServerRoot $ServerRoot -AsJson 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
        throw "Existing league roster helper failed with exit code $LASTEXITCODE."
    }

    try {
        $json = $rawOutput | ConvertFrom-Json
    } catch {
        throw "Existing league roster helper did not return JSON: $($_.Exception.Message)"
    }

    return @($json | ForEach-Object { Get-RosterMembersFromObject -RosterObject $_ })
}

function Read-TelemetrySkills {
    $result = @{}
    if ([string]::IsNullOrWhiteSpace($TelemetryPath)) {
        return $result
    }

    if (-not (Test-Path -LiteralPath $TelemetryPath -PathType Leaf)) {
        throw "Telemetry fixture not found: $TelemetryPath"
    }

    $telemetry = Get-Content -LiteralPath $TelemetryPath -Raw | ConvertFrom-Json
    $rows = @(Get-OptionalProperty -InputObject $telemetry -Name 'skills' -Default @())
    foreach ($row in $rows) {
        $guid = ConvertTo-NullableInt (Get-OptionalProperty -InputObject $row -Name 'guid')
        $skillId = ConvertTo-NullableInt (Get-OptionalProperty -InputObject $row -Name 'skill_id' -Default (Get-OptionalProperty -InputObject $row -Name 'skill'))
        if ($null -eq $guid -or $null -eq $skillId) {
            continue
        }

        $skillKey = "$skillId"
        $profession = if ($script:PrimaryProfessions.Contains($skillKey)) { $script:PrimaryProfessions[$skillKey] } else { $null }
        if ($null -eq $profession) {
            continue
        }

        $value = ConvertTo-NullableInt (Get-OptionalProperty -InputObject $row -Name 'value')
        if ($null -eq $value -or $value -le 0) {
            continue
        }

        $guidKey = "$guid"
        if (-not $result.ContainsKey($guidKey)) {
            $result[$guidKey] = [System.Collections.Generic.List[string]]::new()
        }
        if (-not $result[$guidKey].Contains($profession)) {
            [void]$result[$guidKey].Add($profession)
        }
    }

    return $result
}

function Read-TeamMetadata {
    $metadata = @{
        'northstar' = [ordered]@{ name = 'Northstar Alliance'; affiliation = 'Alliance' }
        'ember' = [ordered]@{ name = 'Ember Horde'; affiliation = 'Horde' }
    }

    if ([string]::IsNullOrWhiteSpace($LeaguePath) -or -not (Test-Path -LiteralPath $LeaguePath -PathType Leaf)) {
        return $metadata
    }

    $league = Get-Content -LiteralPath $LeaguePath -Raw | ConvertFrom-Json
    $teams = @(Get-OptionalProperty -InputObject $league -Name 'teams' -Default @())
    foreach ($team in $teams) {
        $teamId = Get-CanonicalTeamId -Team (Get-OptionalProperty -InputObject $team -Name 'id' -Default (Get-OptionalProperty -InputObject $team -Name 'team')) -Affiliation (Get-OptionalProperty -InputObject $team -Name 'affiliation')
        if ([string]::IsNullOrWhiteSpace($teamId) -or -not $metadata.ContainsKey($teamId)) {
            continue
        }

        $displayName = Get-OptionalProperty -InputObject $team -Name 'name' -Default $metadata[$teamId].name
        $affiliation = Get-OptionalProperty -InputObject $team -Name 'affiliation' -Default $metadata[$teamId].affiliation
        $metadata[$teamId] = [ordered]@{ name = "$displayName"; affiliation = "$affiliation" }
    }

    return $metadata
}

function Get-StringOrEmpty {
    param([AllowNull()][object]$Value)
    if ($null -eq $Value) { return '' }
    return "$Value"
}

function New-NormalizedMember {
    param([Parameter(Mandatory)][object]$Member)

    $guid = ConvertTo-NullableInt (Get-OptionalProperty -InputObject $Member -Name 'guid' -Default (Get-OptionalProperty -InputObject $Member -Name 'character_guid'))
    $team = Get-CanonicalTeamId -Team (Get-OptionalProperty -InputObject $Member -Name 'team' -Default (Get-OptionalProperty -InputObject $Member -Name 'team_id')) -Affiliation (Get-OptionalProperty -InputObject $Member -Name 'affiliation')
    $role = Get-StringOrEmpty (Get-OptionalProperty -InputObject $Member -Name 'role' -Default 'adventurer')
    if ([string]::IsNullOrWhiteSpace($role)) { $role = 'adventurer' }
    $role = $role.Trim().ToLowerInvariant()
    $active = ConvertTo-Boolean -Value (Get-OptionalProperty -InputObject $Member -Name 'active') -Default $true
    $professions = @(Get-ProfessionValues -Member $Member | Where-Object { Test-PrimaryProfession -Profession $_ } | Select-Object -Unique)

    return [pscustomobject]@{
        guid = $guid
        team = $team
        role = $role
        active = $active
        name = Get-StringOrEmpty (Get-OptionalProperty -InputObject $Member -Name 'name' -Default (Get-OptionalProperty -InputObject $Member -Name 'character_name'))
        current_professions = @($professions)
        profession_source = if ($professions.Count -gt 0) { 'league_metadata' } else { 'none_reported' }
    }
}

function Select-Candidate {
    param(
        [Parameter(Mandatory)][object[]]$Candidates,
        [Parameter(Mandatory)][AllowEmptyCollection()][System.Collections.Generic.HashSet[string]]$UsedGuids,
        [Parameter(Mandatory)][string]$PairKey
    )

    $available = @($Candidates | Where-Object {
        $key = if ($null -eq $_.guid) { '' } else { "$($_.guid)" }
        -not $UsedGuids.Contains($key)
    })
    if ($available.Count -eq 0) {
        return $null
    }

    $matching = @($available | Where-Object { (Get-ProfessionPairKey -Professions $_.current_professions) -eq $PairKey })
    if ($matching.Count -gt 0) {
        return $matching[0]
    }

    $unassigned = @($available | Where-Object { $_.current_professions.Count -eq 0 })
    if ($unassigned.Count -gt 0) {
        return $unassigned[0]
    }

    return $available[0]
}

function New-Assignment {
    param(
        [Parameter(Mandatory)][object]$Slot,
        [AllowNull()][object]$Candidate,
        [AllowEmptyCollection()][hashtable]$ObservedSkills
    )

    $plannedProfessions = @($Slot.gather, $Slot.craft)
    $plannedPairKey = Get-ProfessionPairKey -Professions $plannedProfessions
    $currentProfessions = @()
    if ($null -ne $Candidate) {
        $currentProfessions = @($Candidate.current_professions)
    }
    $currentPairKey = Get-ProfessionPairKey -Professions $currentProfessions
    $currentPairValid = if ($currentProfessions.Count -eq 0) { $null } else { Test-ProfessionPair -Professions $currentProfessions }
    $observedProfessions = @()
    $observedPairKey = $null
    if ($null -ne $Candidate -and $null -ne $Candidate.guid) {
        $guidKey = "$($Candidate.guid)"
        if ($null -ne $ObservedSkills -and $ObservedSkills.ContainsKey($guidKey)) {
            $observedProfessions = @($ObservedSkills[$guidKey])
            $observedPairKey = Get-ProfessionPairKey -Professions $observedProfessions
        }
    }

    $blockingReasons = [System.Collections.Generic.List[string]]::new()
    $status = 'unbound'
    $liveReady = $false
    $guid = $null
    $name = ''
    $role = ''
    $candidateType = 'unbound'
    if ($null -eq $Candidate) {
        [void]$blockingReasons.Add('no_guild_member_candidate')
    } else {
        $guid = $Candidate.guid
        $name = $Candidate.name
        $role = $Candidate.role
        $candidateType = if ($Candidate.role -eq 'worker') { 'worker' } else { 'roster_candidate' }
        if ($Candidate.role -ne 'worker') {
            [void]$blockingReasons.Add('role_is_not_worker')
        }
        if ($currentProfessions.Count -eq 0) {
            [void]$blockingReasons.Add('profession_not_verified')
        } elseif ($null -eq $currentPairKey -or $currentPairKey -ne $plannedPairKey) {
            if ($false -eq $currentPairValid) {
                [void]$blockingReasons.Add('current_pair_invalid')
            }
            [void]$blockingReasons.Add('profession_pair_mismatch')
        }

        if ($observedProfessions.Count -eq 0) {
            [void]$blockingReasons.Add('skill_rank_telemetry_required')
        } elseif ($observedPairKey -ne $plannedPairKey) {
            [void]$blockingReasons.Add('observed_profession_pair_mismatch')
        }

        if ($blockingReasons.Count -eq 0) {
            $status = 'ready'
            $liveReady = $true
        } elseif ($Candidate.role -ne 'worker') {
            $status = 'worker_role_required'
        } elseif ($currentProfessions.Count -eq 0) {
            $status = 'proposed_assignment'
        } elseif ($false -eq $currentPairValid) {
            $status = 'invalid_current_pair_change_proposed'
        } elseif ($currentPairKey -ne $plannedPairKey) {
            $status = 'proposed_change'
        } else {
            $status = 'telemetry_required'
        }
    }

    return [ordered]@{
        slot = [int]$Slot.slot
        slot_key = "$($Slot.key)"
        guid = $guid
        name = $name
        role = $role
        candidate_type = $candidateType
        planned_profession_one = "$($Slot.gather)"
        planned_profession_two = "$($Slot.craft)"
        planned_pair_valid = (Test-ProfessionPair -Professions $plannedProfessions)
        dependency = "$($Slot.dependency)"
        current_profession_one = if ($currentProfessions.Count -ge 1) { $currentProfessions[0] } else { $null }
        current_profession_two = if ($currentProfessions.Count -ge 2) { $currentProfessions[1] } else { $null }
        current_profession_pair_valid = $currentPairValid
        observed_profession_one = if ($observedProfessions.Count -ge 1) { $observedProfessions[0] } else { $null }
        observed_profession_two = if ($observedProfessions.Count -ge 2) { $observedProfessions[1] } else { $null }
        observed_profession_pair_valid = if ($observedProfessions.Count -eq 0) { $null } else { Test-ProfessionPair -Professions $observedProfessions }
        profession_source = if ($null -eq $Candidate) { 'none' } else { $Candidate.profession_source }
        status = $status
        live_ready = $liveReady
        requires_worker_role = $true
        blocking_reasons = @($blockingReasons)
    }
}

function New-TeamPlan {
    param(
        [Parameter(Mandatory)][string]$TeamId,
        [Parameter(Mandatory)][object[]]$Members,
        [Parameter(Mandatory)][System.Collections.IDictionary]$TeamMetadata,
        [Parameter(Mandatory)][AllowEmptyCollection()][hashtable]$ObservedSkills
    )

    $teamMembers = @($Members | Where-Object { $_.team -eq $TeamId -and $_.active })
    $candidates = @($teamMembers | Where-Object {
        $_.role -ne 'captain' -and ($WorkersOnly -eq $false -or $_.role -eq 'worker')
    } | Sort-Object @{ Expression = { if ($_.role -eq 'worker') { 0 } else { 1 } } }, guid, name)
    $usedGuids = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $assignments = [System.Collections.Generic.List[object]]::new()

    foreach ($slot in @($script:AssignmentCatalog | Select-Object -First $WorkersPerTeam)) {
        $pairKey = Get-ProfessionPairKey -Professions @($slot.gather, $slot.craft)
        $candidate = Select-Candidate -Candidates $candidates -UsedGuids $usedGuids -PairKey $pairKey
        if ($null -ne $candidate -and $null -ne $candidate.guid) {
            [void]$usedGuids.Add("$($candidate.guid)")
        }
        [void]$assignments.Add((New-Assignment -Slot $slot -Candidate $candidate -ObservedSkills $ObservedSkills))
    }

    $requiredProfessions = @($script:PrimaryProfessions.Values)
    $plannedCoverage = @($assignments | ForEach-Object { $_.planned_profession_one; $_.planned_profession_two } | Select-Object -Unique)
    $workerCoverage = @($assignments | Where-Object { $_.candidate_type -eq 'worker' } | ForEach-Object { $_.planned_profession_one; $_.planned_profession_two } | Select-Object -Unique)
    $readyCoverage = @($assignments | Where-Object { $_.live_ready } | ForEach-Object { $_.planned_profession_one; $_.planned_profession_two } | Select-Object -Unique)
    $missingFromPlan = @($requiredProfessions | Where-Object { $plannedCoverage -notcontains $_ })
    $missingWorkerCoverage = @($requiredProfessions | Where-Object { $workerCoverage -notcontains $_ })
    $missingLiveCoverage = @($requiredProfessions | Where-Object { $readyCoverage -notcontains $_ })
    $unboundCount = @($assignments | Where-Object { $_.candidate_type -eq 'unbound' }).Count
    $nonWorkerCount = @($assignments | Where-Object { $_.candidate_type -eq 'roster_candidate' }).Count

    $coverageStatus = 'incomplete'
    if ($missingFromPlan.Count -eq 0) {
        $coverageStatus = if ($missingWorkerCoverage.Count -eq 0) { 'covered' } else { 'proposal_complete' }
    }

    $warnings = [System.Collections.Generic.List[string]]::new()
    if ($missingFromPlan.Count -gt 0) {
        [void]$warnings.Add("assignment_slots_do_not_cover: $($missingFromPlan -join ', ')")
    }
    if ($missingWorkerCoverage.Count -gt 0) {
        [void]$warnings.Add("worker_roster_gap: $($missingWorkerCoverage -join ', ')")
    }
    if ($missingLiveCoverage.Count -gt 0) {
        [void]$warnings.Add("live_skill_proof_gap: $($missingLiveCoverage -join ', ')")
    }
    if ($nonWorkerCount -gt 0) {
        [void]$warnings.Add("non_worker_candidates: $nonWorkerCount")
    }
    if ($unboundCount -gt 0) {
        [void]$warnings.Add("unbound_assignment_slots: $unboundCount")
    }

    return [ordered]@{
        team = $TeamId
        name = "$($TeamMetadata.name)"
        affiliation = "$($TeamMetadata.affiliation)"
        assignment_count = $assignments.Count
        assignments = @($assignments)
        coverage = [ordered]@{
            status = $coverageStatus
            required_primary_professions = $requiredProfessions
            planned_professions = $plannedCoverage
            worker_professions = $workerCoverage
            live_ready_professions = $readyCoverage
            missing_from_plan = $missingFromPlan
            missing_worker_coverage = $missingWorkerCoverage
            missing_live_coverage = $missingLiveCoverage
            unbound_slots = $unboundCount
            non_worker_candidates = $nonWorkerCount
        }
        warnings = @($warnings)
    }
}

$rawMembers = @(Read-Roster)
$members = @($rawMembers | ForEach-Object { New-NormalizedMember -Member $_ } | Where-Object { $_.team -in @('northstar', 'ember') })
$observedSkills = Read-TelemetrySkills
$teamMetadata = Read-TeamMetadata
$teamPlans = @(
    New-TeamPlan -TeamId 'northstar' -Members $members -TeamMetadata $teamMetadata['northstar'] -ObservedSkills $observedSkills
    New-TeamPlan -TeamId 'ember' -Members $members -TeamMetadata $teamMetadata['ember'] -ObservedSkills $observedSkills
)

$plan = [ordered]@{
    schema_version = 'gathering-progression-v0'
    mode = 'read_only_plan'
    generated_utc = [DateTime]::UtcNow.ToString('o')
    source = [ordered]@{
        roster = if ([string]::IsNullOrWhiteSpace($RosterPath)) { 'scripts/league-simulation.ps1 -Action roster -AsJson' } else { $RosterPath }
        telemetry = if ([string]::IsNullOrWhiteSpace($TelemetryPath)) { $null } else { $TelemetryPath }
        league_manifest = if ([string]::IsNullOrWhiteSpace($LeaguePath)) { $null } else { $LeaguePath }
        profession_declarations_are_not_skill_proof = $true
    }
    planner_rules = [ordered]@{
        workers_per_team = $WorkersPerTeam
        workers_only = [bool]$WorkersOnly
        max_workers_per_team = 10
        full_primary_coverage_minimum_slots = 7
        valid_dependency_pairs = @(
            'Herbalism + Alchemy',
            'Mining + Engineering',
            'Skinning + Leatherworking',
            'Herbalism + Inscription',
            'Mining + Jewelcrafting',
            'Mining + Blacksmithing',
            'Tailoring + Enchanting'
        )
        secondary_professions_not_counted_as_primary = @('Fishing', 'Cooking', 'First Aid')
    }
    teams = $teamPlans
    safety = [ordered]@{
        read_only = $true
        db_writes = 0
        bridge_calls = 0
        live_activation = $false
    }
}

if (-not [string]::IsNullOrWhiteSpace($OutputPath)) {
    $parent = Split-Path -Parent $OutputPath
    if (-not [string]::IsNullOrWhiteSpace($parent)) {
        [void](New-Item -ItemType Directory -Path $parent -Force)
    }
    $plan | ConvertTo-Json -Depth 15 | Set-Content -LiteralPath $OutputPath -Encoding utf8
}

if ($AsJson) {
    $plan | ConvertTo-Json -Depth 15 -Compress
    exit 0
}

Write-Output "Gathering progression plan: $($teamPlans.Count) teams, $WorkersPerTeam slots per team."
foreach ($teamPlan in $teamPlans) {
    Write-Output ("{0}: {1}; worker coverage {2}/{3}; live-ready coverage {4}/{3}; status {5}" -f $teamPlan.name, $teamPlan.assignment_count, $teamPlan.coverage.worker_professions.Count, $teamPlan.coverage.required_primary_professions.Count, $teamPlan.coverage.live_ready_professions.Count, $teamPlan.coverage.status)
    foreach ($warning in @($teamPlan.warnings)) {
        Write-Output "  warning: $warning"
    }
}
