# AutoWoW Autopilot V1.2 - campaign engine.
# A campaign document turns player intent (characters + profiles + goals) into
# PROPOSED jobs and enrollment requests. plan shows every generated job, its
# prerequisites, capability requirements, and known blockers BEFORE anything can
# execute; apply creates jobs idempotently and never sends a command.
# Depends on: AutopilotLib.ps1, AutopilotGoals.ps1, AutopilotProfiles.ps1,
# AutopilotRoster.ps1 (optional roster facts for prerequisite checks).

Set-StrictMode -Version Latest

$script:AutopilotCampaignSchema = 'autowow.autopilot.campaign.v1'
$script:AutopilotCampaignStateSchema = 'autowow.autopilot.campaign-state.v1'

# Static WotLK profession skill-line ids (client data facts, not guesses).
$script:AutopilotProfessionSkillIds = @{
    'mining' = 186; 'herbalism' = 182; 'skinning' = 393; 'alchemy' = 171
    'blacksmithing' = 164; 'enchanting' = 333; 'engineering' = 202
    'leatherworking' = 165; 'tailoring' = 197; 'jewelcrafting' = 755
    'inscription' = 773; 'cooking' = 185; 'first aid' = 129; 'fishing' = 356
}

function Get-AutopilotCampaignSchemaPath {
    param([string]$SchemaPath)
    if ($SchemaPath) { return $SchemaPath }
    return (Join-Path (Split-Path -Parent $PSScriptRoot) 'schemas\campaign.schema.json')
}

function Import-AutopilotCampaign {
    # Fail closed: unknown schema/version, malformed JSON, duplicate GUIDs, or
    # invalid goals reject the whole document with every error listed.
    param(
        [Parameter(Mandatory)][string]$Path,
        [string]$SchemaPath
    )
    if (-not (Test-Path $Path)) { throw "Campaign file not found: $Path" }
    try {
        $doc = ConvertTo-AutopilotHashtable (Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json)
    }
    catch {
        throw "Campaign file unreadable (fail closed): $($_.Exception.Message)"
    }
    $errors = [System.Collections.Generic.List[string]]::new()
    if (-not ($doc -is [System.Collections.IDictionary]) -or [string]$doc.schema -ne $script:AutopilotCampaignSchema -or [int64]$doc.schema_version -ne 1) {
        throw "Campaign schema not supported (fail closed): expected $($script:AutopilotCampaignSchema) v1"
    }
    foreach ($field in @('campaign', 'characters')) {
        if (-not $doc.Contains($field)) { $errors.Add("missing required field '$field'") }
    }
    if ($errors.Count -eq 0) {
        if ([string]$doc.campaign -notmatch '^[a-z0-9][a-z0-9-]{1,40}$') { $errors.Add("campaign name '$($doc.campaign)' must be lowercase kebab-case") }
        $guids = @()
        foreach ($character in @($doc.characters)) {
            foreach ($field in @('guid', 'profile', 'goals')) {
                if (-not $character.Contains($field)) { $errors.Add("character entry missing '$field'") }
            }
            if ($character.Contains('guid')) {
                if ([int64]$character.guid -in $guids) { $errors.Add("duplicate character guid $($character.guid)") }
                $guids += [int64]$character.guid
            }
            if ($character.Contains('goals')) {
                # Player ergonomics: campaigns carry bare goals; the versioned
                # campaign envelope stamps the goal schema before validation.
                $normalizedGoals = @()
                $goalIndex = 0
                foreach ($goal in @($character.goals)) {
                    $normalized = ConvertTo-AutopilotHashtable $goal
                    if (-not $normalized.Contains('schema')) { $normalized.schema = 'autowow.autopilot.goal.v1' }
                    if (-not $normalized.Contains('schema_version')) { $normalized.schema_version = 1 }
                    $goalCheck = Test-AutopilotGoal -Goal $normalized
                    if (-not $goalCheck.Valid) {
                        $errors.Add(('character {0} goal[{1}]: {2}' -f $character.guid, $goalIndex, (@($goalCheck.Errors) -join '; ')))
                    }
                    $normalizedGoals += , $normalized
                    $goalIndex++
                }
                $character.goals = @($normalizedGoals)
            }
        }
    }
    $schemaFile = Get-AutopilotCampaignSchemaPath -SchemaPath $SchemaPath
    if ((Test-Path $schemaFile) -and $errors.Count -eq 0) {
        try {
            $null = Test-Json -Json ($doc | ConvertTo-Json -Depth 30) -SchemaFile $schemaFile -ErrorAction Stop
        }
        catch {
            if ($_.FullyQualifiedErrorId -match 'InvalidJsonAgainstSchema') { $errors.Add("JSON Schema violation: $($_.Exception.Message)") }
        }
    }
    if ($errors.Count -gt 0) { throw "Campaign rejected: $($errors -join ' | ')" }
    return $doc
}

function Get-AutopilotCampaignStatePath {
    param([Parameter(Mandatory)]$Paths, [Parameter(Mandatory)][string]$Name)
    $dir = Join-Path $Paths.StateRoot 'campaigns'
    if (-not (Test-Path $dir)) { $null = New-Item -ItemType Directory -Force -Path $dir }
    return (Join-Path $dir "$Name.json")
}

function Get-AutopilotCampaignState {
    param([Parameter(Mandatory)]$Paths, [Parameter(Mandatory)][string]$Name)
    $file = Get-AutopilotCampaignStatePath -Paths $Paths -Name $Name
    if (-not (Test-Path $file)) { return $null }
    try {
        $doc = ConvertTo-AutopilotHashtable (Get-Content -LiteralPath $file -Raw | ConvertFrom-Json)
        if ([string]$doc.schema -ne $script:AutopilotCampaignStateSchema) { return $null }
        return $doc
    }
    catch { return $null }
}

function Get-AutopilotCampaignJobSuccessCriteria {
    # Job-level, receipt-verifiable success counters per node kind. Goal-level
    # completion (e.g. "reached level 20") is reported separately and stays
    # UNKNOWN until a level-observation stream exists - never inferred.
    param([Parameter(Mandatory)][string]$JobKind, [Parameter(Mandatory)]$Spec)
    switch ($JobKind) {
        'QuestLevel' { return @{ measure = 'quest-turnins'; counters = @{ questsTurnedIn = 1 } } }
        'Gather' { return @{ measure = 'inventory-item-count'; counters = @{ itemCount = [int64]$Spec.stopAtCount } } }
        'Craft' { return @{ measure = 'inventory-item-count'; counters = @{ crafted = [int64]$(if ($Spec.Contains('count')) { $Spec.count } else { 1 }) } } }
        'TrainProfession' { return @{ measure = 'profession-skill-level'; counters = @{ skillReached = [int64]$(if ($Spec.Contains('targetSkill') -and $Spec.targetSkill) { $Spec.targetSkill } else { 75 }) } } }
        'VendorRepair' { return @{ measure = 'gold-delta-at-vendor'; counters = @{ repaired = 1 } } }
        'Restock' { return @{ measure = 'restock-inventory-count'; counters = @{ restocked = 1 } } }
        'FormParty' { return @{ measure = 'party-composition'; counters = @{ partyFormed = 1 } } }
        'Travel' { return @{ measure = 'arrival-at-destination'; counters = @{ arrived = 1 } } }
        'Dungeon' { return @{ measure = 'dungeon-boss-kills'; counters = @{ bossKills = 1 } } }
        'Raid' { return @{ measure = 'raid-encounter-receipts'; counters = @{ encounters = 1 } } }
        'Battleground' { return @{ measure = 'battleground-scoreboard'; counters = @{ matchesCompleted = 1 } } }
        default { return @{ measure = 'none'; counters = @{ done = 1 } } }
    }
}

function Get-AutopilotCampaignPlan {
    <#
    Expands every character's goals through the dependency planner and annotates
    each node with: profile permission, campaign permitted-activities check,
    capability evidence, roster-fact prerequisite checks (when a provider is
    supplied), and the deterministic job idempotency key. Read-only.
    #>
    param(
        [Parameter(Mandatory)]$Paths,
        [Parameter(Mandatory)]$Campaign,
        $CapabilityProvider,
        $RosterProvider,
        [string]$ProfilesDir,
        [datetime]$Now
    )
    if (-not $CapabilityProvider) { $CapabilityProvider = Resolve-AutopilotCapabilityProvider -Paths $Paths }
    $campaign = ConvertTo-AutopilotHashtable $Campaign
    $permitted = @($(if ($campaign.Contains('permittedActivities') -and $campaign.permittedActivities) { $campaign.permittedActivities } else { @() }))
    $characterPlans = @()
    $enrollmentNeeded = @()

    foreach ($character in @($campaign.characters)) {
        $guid = [int64]$character.guid
        $profileName = [string]$character.profile
        $basePriority = [int64]$(if ($character.Contains('priority') -and $null -ne $character.priority) { $character.priority } else { 50 })
        $profile = $null
        $profileError = $null
        try {
            $profileParams = @{ Name = $profileName }
            if ($ProfilesDir) { $profileParams['ProfilesDir'] = $ProfilesDir }
            $profile = Get-AutopilotProfile @profileParams
        }
        catch { $profileError = [string]$_.Exception.Message }

        $goalPlanParams = @{ Goals = @($character.goals); CharacterGuid = $guid }
        if ($profile) { $goalPlanParams['Profile'] = $profile }
        $goalPlan = New-AutopilotGoalPlan @goalPlanParams

        $nodes = @()
        foreach ($nodeId in @($goalPlan.order)) {
            $node = @($goalPlan.nodes | Where-Object { [string]$_.nodeId -eq [string]$nodeId })[0]
            $blockers = @()
            $excluded = $false
            $nodeSpec = ConvertTo-AutopilotHashtable $node.spec

            # Resolve well-known profession names to skill ids; a profession the
            # controller cannot resolve stays excluded, never guessed.
            if ([string]$node.jobKind -eq 'TrainProfession') {
                $hasId = $nodeSpec.Contains('professionSkillId') -and $null -ne $nodeSpec.professionSkillId -and [int64]$nodeSpec.professionSkillId -ge 1
                if (-not $hasId -and $nodeSpec.Contains('professionName') -and $nodeSpec.professionName) {
                    $lookup = ([string]$nodeSpec.professionName).ToLowerInvariant()
                    if ($script:AutopilotProfessionSkillIds.ContainsKey($lookup)) {
                        $nodeSpec['professionSkillId'] = [int64]$script:AutopilotProfessionSkillIds[$lookup]
                        $hasId = $true
                    }
                }
                if (-not $hasId) {
                    $blockers += "profession skill id unresolved for '$($nodeSpec.professionName)'; requires roster/DBC facts"
                    $excluded = $true
                }
                if (-not $nodeSpec.Contains('targetSkill') -or $null -eq $nodeSpec.targetSkill) { $nodeSpec['targetSkill'] = 75 }
            }
            # Placeholder material nodes (unresolved recipes) cannot become jobs.
            if ([string]$node.jobKind -eq 'Gather' -and (-not $nodeSpec.Contains('item') -or -not $nodeSpec.item.Contains('itemId') -or [int64]$nodeSpec.item.itemId -lt 1)) {
                $blockers += 'material list requires recipe facts; unresolved placeholder is excluded'
                $excluded = $true
            }
            # Craft without a resolved recipe spell cannot become a job.
            if ([string]$node.jobKind -eq 'Craft' -and (-not $nodeSpec.Contains('recipeSpellId') -or $null -eq $nodeSpec.recipeSpellId -or [int64]$nodeSpec.recipeSpellId -lt 1)) {
                $blockers += 'recipe spell id unresolved; requires recipe facts'
                $excluded = $true
            }
            # Restock means topping up the profile's declared consumable reserve.
            if ([string]$node.jobKind -eq 'Restock' -and (-not $nodeSpec.Contains('consumables') -or @($nodeSpec.consumables).Count -eq 0)) {
                $reserve = @($(if ($profile -and $profile.Contains('consumableReserve')) { $profile.consumableReserve } else { @() }))
                if ($reserve.Count -gt 0) {
                    $nodeSpec['consumables'] = @($reserve | ForEach-Object { [ordered]@{ itemId = [int64]$_.itemId; minCount = [int64]$_.minCount } })
                }
                else {
                    $blockers += "profile '$profileName' declares no consumable reserve; restock has nothing to top up"
                    $excluded = $true
                }
            }

            if ($profileError) {
                $blockers += "profile '$profileName' failed to load: $profileError"
                $excluded = $true
            }
            elseif ($profile) {
                $permission = Test-AutopilotProfileJobKindAllowed -Profile $profile -JobKind ([string]$node.jobKind)
                if (-not $permission.allowed) {
                    $blockers += "excluded by profile: $($permission.reason)"
                    $excluded = $true
                }
            }
            if ($permitted.Count -gt 0 -and [string]$node.jobKind -notin $permitted) {
                $blockers += "excluded by campaign permittedActivities (kind '$($node.jobKind)' not listed)"
                $excluded = $true
            }
            $capabilityStates = @()
            foreach ($capability in @($node.requiredCapabilities)) {
                $status = Get-AutopilotCapabilityStatus -Capability $capability -Provider $CapabilityProvider
                $capabilityStates += [pscustomobject]@{ capability = $capability; available = $status.available; reason = $status.reason }
                if (-not $status.available) {
                    $blockers += "planned, currently unavailable: $capability ($($status.reason))"
                }
            }
            # Roster-fact prerequisite checks (unknown stays unknown).
            $prerequisiteFindings = @()
            if ($RosterProvider) {
                $fact = Get-AutopilotRosterFact -Provider $RosterProvider -Guid $guid
                foreach ($prerequisite in @($node.prerequisites)) {
                    if ([string]$prerequisite.kind -eq 'min-level') {
                        $needed = [int64]$prerequisite.params.level
                        if ($fact -and $null -ne $fact.level) {
                            if ([int64]$fact.level -lt $needed) {
                                $finding = "min-level $needed not met (observed level $($fact.level))"
                                $prerequisiteFindings += $finding
                                $blockers += $finding
                            }
                        }
                        else {
                            $prerequisiteFindings += "min-level $needed unverifiable (level not observed)"
                        }
                    }
                }
            }

            $spec = $nodeSpec
            $success = Get-AutopilotCampaignJobSuccessCriteria -JobKind ([string]$node.jobKind) -Spec $spec
            $keyMaterial = '{0}|{1}|{2}|{3}' -f [string]$campaign.campaign, [string]$node.jobKind, (ConvertTo-AutopilotCanonicalJson $spec), $guid
            $idempotencyKey = Get-AutopilotSha256 -Text $keyMaterial
            $priority = if ($profile) { Get-AutopilotProfileJobPriority -Profile $profile -JobKind ([string]$node.jobKind) -BasePriority $basePriority } else { $basePriority }

            $nodes += [pscustomobject]@{
                nodeId               = [string]$node.nodeId
                goalKind             = [string]$node.goalKind
                jobKind              = [string]$node.jobKind
                spec                 = $spec
                optional             = [bool]$node.optional
                notes                = $node.notes
                prerequisites        = @($node.prerequisites)
                prerequisiteFindings = @($prerequisiteFindings)
                requiredCapabilities = @($capabilityStates)
                dependsOn            = @(@($goalPlan.edges) | Where-Object { [string]$_.to -eq [string]$node.nodeId } | ForEach-Object { [string]$_.from })
                priority             = $priority
                successCriteria      = $success
                idempotencyKey       = $idempotencyKey
                excluded             = $excluded
                blockers             = @($blockers)
                state                = $(if ($excluded) { 'excluded' } elseif (@($blockers).Count -gt 0) { 'planned-currently-unavailable' } else { 'proposed' })
            }
            $oracleCaps = @($node.requiredCapabilities | Where-Object { $_ -like 'oracle.*' })
            if (-not $excluded -and $oracleCaps.Count -gt 0) {
                $enrollmentNeeded += [pscustomobject]@{ guid = $guid; nodeId = [string]$node.nodeId; capabilities = @($oracleCaps) }
            }
        }
        $characterPlans += [pscustomobject]@{
            guid         = $guid
            profile      = $profileName
            profileValid = ($null -eq $profileError)
            profileError = $profileError
            basePriority = $basePriority
            nodes        = @($nodes)
        }
    }

    [pscustomobject]@{
        schema           = 'autowow.autopilot.campaign-plan.v1'
        schema_version   = 1
        campaign         = [string]$campaign.campaign
        capabilitySource = [string]$CapabilityProvider.Kind
        characters       = @($characterPlans)
        enrollmentNeeded = @($enrollmentNeeded)
    }
}

function Invoke-AutopilotCampaignApply {
    <#
    Creates the plan's non-excluded jobs idempotently (dependency prerequisites
    wired as job-completed edges in topological order), stamps profile metadata,
    writes enrollment proposals for oracle-gated jobs, and records the campaign
    state document. NEVER sends a command - jobs wait for the run loop, and
    blocked domains stay honestly blocked.
    #>
    param(
        [Parameter(Mandatory)]$Paths,
        [Parameter(Mandatory)]$Campaign,
        [Parameter(Mandatory)]$Plan,
        [datetime]$Now
    )
    $campaign = ConvertTo-AutopilotHashtable $Campaign
    $nowParams = @{}
    if ($PSBoundParameters.ContainsKey('Now')) { $nowParams['Now'] = $Now }
    $nodeJobs = [ordered]@{}
    $created = @()
    $skipped = @()

    foreach ($characterPlan in @($Plan.characters)) {
        $guid = [int64]$characterPlan.guid
        foreach ($node in @($characterPlan.nodes)) {
            if ($node.excluded) {
                $skipped += [pscustomobject]@{ nodeId = $node.nodeId; reason = (@($node.blockers) -join ' | ') }
                continue
            }
            $prerequisites = @(@($node.prerequisites))
            foreach ($dependencyNodeId in @($node.dependsOn)) {
                if ($nodeJobs.Contains([string]$dependencyNodeId)) {
                    $prerequisites += @{ kind = 'job-completed'; params = @{ jobId = [string]$nodeJobs[[string]$dependencyNodeId] }; description = "dependency node $dependencyNodeId" }
                }
            }
            $spec = ConvertTo-AutopilotHashtable $node.spec
            $jobParams = @{
                Kind                  = [string]$node.jobKind
                Spec                  = $spec
                EligibleGuids         = @([uint32]$guid)
                SuccessCriteria       = $node.successCriteria
                Campaign              = [string]$campaign.campaign
                RequestedBy           = 'campaign-apply'
                Priority              = [int]$node.priority
                Prerequisites         = $prerequisites
                Paths                 = $Paths
            }
            if ($campaign.Contains('team') -and $campaign.team -and $campaign.team.Contains('name')) { $jobParams['Team'] = [string]$campaign.team.name }
            if ($PSBoundParameters.ContainsKey('Now')) { $jobParams['Now'] = $Now }
            $job = New-AutopilotJob @jobParams
            if (-not $job.Contains('metadata') -or $null -eq $job.metadata) { $job.metadata = [ordered]@{} }
            $job.metadata.profile = [string]$characterPlan.profile
            $job.metadata.campaignNode = [string]$node.nodeId
            $job.metadata.goalKind = [string]$node.goalKind
            $saved = Add-AutopilotJob -Paths $Paths -Job $job
            $nodeJobs[[string]$node.nodeId] = [string]$saved.jobId
            $created += [pscustomobject]@{ nodeId = $node.nodeId; jobId = [string]$saved.jobId; jobKind = [string]$node.jobKind; state = $node.state }
            # Enrollment proposal for oracle-gated jobs (proposal only).
            $oracleCaps = @(@($node.requiredCapabilities) | Where-Object { -not $_.available -and [string]$_.capability -like 'oracle.*' })
            if ($oracleCaps.Count -gt 0) {
                $proposalParams = @{ Paths = $Paths; JobId = [string]$saved.jobId }
                if ($PSBoundParameters.ContainsKey('Now')) { $proposalParams['Now'] = $Now }
                $null = New-AutopilotEnrollmentProposal @proposalParams
            }
        }
    }

    $stateDoc = [ordered]@{
        schema         = $script:AutopilotCampaignStateSchema
        schema_version = 1
        campaign       = [string]$campaign.campaign
        applied_utc    = ConvertTo-AutopilotTimestamp -Time $(if ($PSBoundParameters.ContainsKey('Now')) { Get-AutopilotUtcNow -Now $Now } else { Get-AutopilotUtcNow })
        doc_hash       = Get-AutopilotSha256 -Text (ConvertTo-AutopilotCanonicalJson $campaign)
        node_jobs      = $nodeJobs
    }
    Write-AutopilotFileAtomic -Path (Get-AutopilotCampaignStatePath -Paths $Paths -Name ([string]$campaign.campaign)) -Content ($stateDoc | ConvertTo-Json -Depth 15)
    $null = Write-AutopilotReceipt -Paths $Paths -JobId 'controller' -Event 'campaign_applied' -Data @{
        campaign = [string]$campaign.campaign
        created  = @($created | ForEach-Object { $_.jobId })
        skipped  = @($skipped | ForEach-Object { $_.nodeId })
    } @nowParams

    [pscustomobject]@{
        campaign = [string]$campaign.campaign
        created  = @($created)
        skipped  = @($skipped)
        statePath = (Get-AutopilotCampaignStatePath -Paths $Paths -Name ([string]$campaign.campaign))
    }
}

function Get-AutopilotCampaignStatus {
    param([Parameter(Mandatory)]$Paths, [Parameter(Mandatory)][string]$Name)
    $state = Get-AutopilotCampaignState -Paths $Paths -Name $Name
    if (-not $state) { return $null }
    $jobs = @()
    foreach ($nodeId in @($state.node_jobs.Keys)) {
        $job = Get-AutopilotJob -Paths $Paths -JobId ([string]$state.node_jobs[$nodeId])
        if ($job) {
            $jobs += [pscustomobject]@{
                nodeId    = [string]$nodeId
                jobId     = [string]$job.jobId
                kind      = [string]$job.kind
                phase     = [string]$job.phase
                waitState = Get-AutopilotWaitState -Job $job
                counters  = ($job.progress.counters | ConvertTo-Json -Compress)
                blocked   = $(if ($job.blockedReason) { [string]$job.blockedReason.code } else { $null })
            }
        }
    }
    [pscustomobject]@{
        campaign   = [string]$state.campaign
        appliedUtc = [string]$state.applied_utc
        jobs       = @($jobs)
        # Goal-level completion beyond receipted job counters is UNKNOWN until
        # observation streams (level, profession skill) exist - stated, not guessed.
        goalEvidenceNote = 'job counters are receipt-verified; goal-level outcomes such as reached-level are unknown until a level observation stream is deployed'
    }
}

function Stop-AutopilotCampaign {
    # Requests cancellation of every non-terminal campaign job; the next tick
    # acknowledges each (Cancelled + ownership release). Receipted.
    param([Parameter(Mandatory)]$Paths, [Parameter(Mandatory)][string]$Name, [datetime]$Now)
    $nowParams = @{}
    if ($PSBoundParameters.ContainsKey('Now')) { $nowParams['Now'] = $Now }
    $state = Get-AutopilotCampaignState -Paths $Paths -Name $Name
    if (-not $state) { throw "No applied campaign named '$Name'." }
    $stamp = ConvertTo-AutopilotTimestamp -Time $(if ($PSBoundParameters.ContainsKey('Now')) { Get-AutopilotUtcNow -Now $Now } else { Get-AutopilotUtcNow })
    $requested = @()
    foreach ($nodeId in @($state.node_jobs.Keys)) {
        $job = Get-AutopilotJob -Paths $Paths -JobId ([string]$state.node_jobs[$nodeId])
        if ($job -and $job.phase -notin $script:AutopilotTerminalPhases) {
            $job.cancellation.requested = $true
            $job.cancellation.requestedAt = $stamp
            $job.cancellation.reason = "campaign '$Name' stopped by operator"
            $null = Save-AutopilotJob -Paths $Paths -Job $job
            $requested += [string]$job.jobId
        }
    }
    $null = Write-AutopilotReceipt -Paths $Paths -JobId 'controller' -Event 'campaign_stopped' -Data @{ campaign = $Name; cancellation_requested = @($requested) } @nowParams
    [pscustomobject]@{ campaign = $Name; cancellationRequested = @($requested) }
}
