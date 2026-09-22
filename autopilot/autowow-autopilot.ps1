<#
.SYNOPSIS
AutoWoW Autopilot V1.2 CLI - persistent, player-facing strategic controller (dry-run by default).

.DESCRIPTION
Express a long-running intention (a campaign of goals, or a one-off assignment),
start the controller, walk away, and later understand exactly what the characters
accomplished, why anything stopped, and what they need next.

Everything is fail-closed: capability truth comes only from a deployed-runtime
manifest; one gameplay mutator per character via ownership leases; live sends
require the full quintuple gate; blocked domains say "planned, currently
unavailable" - they are never emulated and never silently transformed.

.EXAMPLE
.\autowow-autopilot.ps1 campaign validate -Path .\campaigns\northstar-sample.json
.\autowow-autopilot.ps1 campaign plan     -Path .\campaigns\northstar-sample.json
.\autowow-autopilot.ps1 campaign apply    -Path .\campaigns\northstar-sample.json
.\autowow-autopilot.ps1 run -DurationMinutes 480 -TickSeconds 30
.\autowow-autopilot.ps1 explain -CharacterGuid 10
.\autowow-autopilot.ps1 summary -Since '2026-07-18T00:00:00Z'
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory, Position = 0)]
    [ValidateSet('status', 'roster', 'jobs', 'assign', 'form-party', 'run', 'queue', 'pause', 'resume', 'cancel', 'retry',
        'explain', 'capabilities', 'receipts', 'propose-enrollment', 'run-once', 'reconcile',
        'campaign', 'plan', 'start', 'stop', 'blockers', 'history', 'summary', 'manifest')]
    [string]$Command,

    [Parameter(Position = 1)]
    [string]$SubCommand,

    # --- targeting ---
    [uint32]$LeaderGuid,
    [uint32[]]$Guids = @(),
    [uint32]$CharacterGuid,
    [string]$JobId,
    [string]$Team,
    [string]$Campaign = 'adhoc',
    [string]$Path,
    [string]$Name,

    # --- assignment parameters ---
    [uint32]$ItemId,
    [int]$Count,
    [int]$TargetLevel,
    [Alias('UntilLevel')][int]$UntilLevelAlias,
    [uint32]$RecipeSpellId,
    [string]$Roles,
    [int]$TeamSize,
    [int]$Priority = 50,

    # --- run loop ---
    [switch]$Once,
    [int]$DurationMinutes = 0,
    [int]$TickSeconds = 30,
    [switch]$FixtureClock,
    [datetime]$FixtureStartUtc,
    [int]$MaxTicks = 0,
    [bool]$DryRun = $true,
    [switch]$StopOnQuiescence,

    # --- inspection ---
    [int]$Tail = 20,
    [datetime]$Since,
    [string]$Activity,
    [string]$Reason,

    # --- providers / paths ---
    [ValidateSet('dry-run', 'live')][string]$Mode = 'dry-run',
    [switch]$ConfirmLiveBridge,
    [string]$StateRoot,
    [string]$FixtureRoot,
    [string]$CapabilityManifest,
    [string]$RosterPath,
    [string]$ProfilesDir,
    [string]$OwnershipPath,
    [string]$EnrollmentRequestPath,
    [string]$OracleReceiptsPath,
    # --- manifest generate|verify ---
    [string]$OutPath,
    [string]$DeclarationsPath,
    [string]$ProofsPath,
    [switch]$AsSolPublication,
    [switch]$SkipWsl,
    [string]$WorldserverSha256Override,
    [string]$TopologyManifestPath,
    [int]$MaxAgeMinutes = 240,
    [Alias('Json')][switch]$AsJson
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'AutopilotLib.ps1')
foreach ($module in (Get-ChildItem -Path (Join-Path $PSScriptRoot 'modules') -Filter '*.ps1' -ErrorAction SilentlyContinue | Sort-Object Name)) {
    . $module.FullName
}

$pathParams = @{}
if ($StateRoot) { $pathParams['StateRoot'] = $StateRoot }
if ($OwnershipPath) { $pathParams['OwnershipPath'] = $OwnershipPath }
if ($EnrollmentRequestPath) { $pathParams['EnrollmentRequestPath'] = $EnrollmentRequestPath }
$paths = Get-AutopilotPaths @pathParams
$provider = if ($CapabilityManifest) { New-AutopilotCapabilityProvider -Kind manifest -ManifestPath $CapabilityManifest }
else { Resolve-AutopilotCapabilityProvider -Paths $paths }

# Profile resolver with per-invocation caching (schedules are preference, not authority).
$script:ProfileCache = @{}
$profileResolver = {
    param([string]$name)
    if (-not (Get-Command -Name 'Get-AutopilotProfile' -ErrorAction SilentlyContinue)) { return $null }
    if ($script:ProfileCache.ContainsKey($name)) { return $script:ProfileCache[$name] }
    $profile = $null
    try {
        $profileParams = @{ Name = $name }
        if ($ProfilesDir) { $profileParams['ProfilesDir'] = $ProfilesDir }
        $profile = Get-AutopilotProfile @profileParams
    }
    catch { $profile = $null }
    $script:ProfileCache[$name] = $profile
    return $profile
}

function Get-CliRosterProvider {
    if (-not (Get-Command -Name 'New-AutopilotRosterProvider' -ErrorAction SilentlyContinue)) { return $null }
    if ($RosterPath) { return New-AutopilotRosterProvider -Kind fixture -Path $RosterPath }
    $default = Join-Path $PSScriptRoot 'fixtures\roster\league-sample.json'
    if (Test-Path $default) { return New-AutopilotRosterProvider -Kind fixture -Path $default }
    return New-AutopilotRosterProvider -Kind unavailable
}

function Out-AutopilotResult {
    param($Value)
    if ($AsJson) { $Value | ConvertTo-Json -Depth 25 }
    elseif ($Value -is [System.Collections.IDictionary]) { [pscustomobject]$Value }
    else { $Value }
}

function Get-JobSummary {
    param($Job)
    [pscustomobject]@{
        jobId     = $Job.jobId
        kind      = $Job.kind
        phase     = $Job.phase
        waitState = Get-AutopilotWaitState -Job $Job
        priority  = $Job.priority
        mode      = $Job.executionMode
        assigned  = @($Job.characters.assignedGuids) -join ','
        counters  = ($Job.progress.counters | ConvertTo-Json -Compress)
        blocked   = if ($Job.blockedReason) { $Job.blockedReason.code } else { $null }
        runId     = $Job.receiptLog.runId
    }
}

function New-CliAssignment {
    param([string]$Kind, [hashtable]$Spec, [hashtable]$SuccessCriteria, [uint32[]]$EligibleGuids)
    if ($EligibleGuids.Count -lt 1) { throw "$Kind assignment requires at least one character GUID." }
    $job = New-AutopilotJob -Kind $Kind -Spec $Spec -EligibleGuids $EligibleGuids `
        -SuccessCriteria $SuccessCriteria -Campaign $Campaign -Team $Team -Priority $Priority `
        -ExecutionMode $Mode -Paths $paths
    $saved = Add-AutopilotJob -Paths $paths -Job $job
    # Honest availability note at creation time.
    $missing = Get-AutopilotMissingRequiredCapabilities -Kind $Kind -Provider $provider
    [pscustomobject]@{
        jobId        = $saved.jobId
        kind         = $saved.kind
        phase        = $saved.phase
        mode         = $saved.executionMode
        availability = $(if (@($missing).Count -gt 0) {
                'planned, currently unavailable: ' + ((@($missing) | ForEach-Object { "$($_.capability) ($($_.reason))" }) -join '; ')
            }
            else { 'executable in dry-run rehearsal now' })
    }
}

switch ($Command) {
    'status' {
        $jobs = Get-AutopilotJobs -Paths $paths -IncludeTerminal
        $lock = if (Test-Path $paths.LockPath) { Get-Content $paths.LockPath -Raw | ConvertFrom-Json } else { $null }
        $ownership = Read-AutopilotOwnership -Paths $paths
        $checkpoint = Get-AutopilotCheckpoint -Paths $paths
        $activeLeases = @($ownership.leases | Where-Object { [string]$_.state -eq 'active' })
        Out-AutopilotResult ([pscustomobject]@{
                controller   = [pscustomobject]@{
                    stateRoot  = $paths.StateRoot
                    instanceId = Get-AutopilotControllerInstanceId -Paths $paths
                    running    = [bool]$lock
                    lock       = $lock
                    lastTick   = $(if ($checkpoint) { [pscustomobject]@{ tick = $checkpoint.tick; simNow = $checkpoint.sim_now; clock = $checkpoint.clock_kind } } else { $null })
                    mode       = 'dry-run default; live requires allowlist + ownership + capability evidence + -ConfirmLiveBridge'
                }
                capabilities = [pscustomobject]@{
                    provider       = $provider.Kind
                    source         = $provider.Source
                    evidence       = $provider.Reason
                    serverBuild    = $provider.ServerBuild
                    sessionId      = $provider.SessionId
                    runtimeEnabled = $provider.RuntimeEnabled
                    bridgeDeployed = $provider.BridgeDeployed
                }
                ownership    = [pscustomobject]@{
                    path         = $paths.OwnershipPath
                    usable       = $ownership.usable
                    activeLeases = @($activeLeases | ForEach-Object { [pscustomobject]@{ guid = $_.characterGuid; job = $_.jobId; expiresAt = $_.expiresAt } })
                }
                jobs         = @($jobs | ForEach-Object { Get-JobSummary -Job $_ })
            })
    }
    'jobs' {
        Out-AutopilotResult (@(Get-AutopilotJobs -Paths $paths -IncludeTerminal | ForEach-Object { Get-JobSummary -Job $_ }))
    }
    'blockers' {
        $blocked = @()
        foreach ($job in (Get-AutopilotJobs -Paths $paths)) {
            $waitState = Get-AutopilotWaitState -Job $job
            if ($job.phase -in @('Blocked', 'Queued', 'Suspended') -or $waitState -like 'waiting-*') {
                $blocked += [pscustomobject]@{
                    jobId     = $job.jobId
                    kind      = $job.kind
                    character = (Get-AutopilotJobCharacter -Job $job)
                    waitState = $waitState
                    since     = $(if ($job.blockedReason) { $job.blockedReason.since } else { $job.phaseEnteredAt })
                    reason    = $(if ($job.blockedReason) { '{0}: {1}' -f $job.blockedReason.code, $job.blockedReason.detail } else { "phase $($job.phase)" })
                    nextStep  = $(if ($job.blockedReason -and $job.blockedReason.nextAction) { $job.blockedReason.nextAction } else { 'advances on the next tick' })
                }
            }
        }
        Out-AutopilotResult (@($blocked))
    }
    'roster' {
        $rosterProvider = Get-CliRosterProvider
        switch ($SubCommand) {
            'inspect' {
                if (-not $CharacterGuid) { throw 'roster inspect requires -CharacterGuid.' }
                if (-not $rosterProvider) { throw 'roster module not loaded.' }
                $fact = Get-AutopilotRosterFact -Provider $rosterProvider -Guid ([int64]$CharacterGuid)
                if (-not $fact) { throw "No roster fact for guid $CharacterGuid (provider: $($rosterProvider.Kind), available: $($rosterProvider.Available))." }
                Out-AutopilotResult ([pscustomobject]$fact)
            }
            'suitability' {
                if (-not $CharacterGuid -or -not $Activity) { throw 'roster suitability requires -CharacterGuid and -Activity dungeon|raid|gather|craft|pvp.' }
                if (-not $rosterProvider) { throw 'roster module not loaded.' }
                Out-AutopilotResult (Get-AutopilotRosterSuitability -Provider $rosterProvider -Guid ([int64]$CharacterGuid) -Activity $Activity)
            }
            default {
                $facts = if ($rosterProvider) { @(Get-AutopilotRosterFacts -Provider $rosterProvider | ForEach-Object { $_ } | ForEach-Object { $_ }) } else { @() }
                $ownership = Read-AutopilotOwnership -Paths $paths
                $allowlist = @(Get-Content $paths.AllowlistPath -Raw | ConvertFrom-Json)
                Out-AutopilotResult ([pscustomobject]@{
                        provider         = $(if ($rosterProvider) { $rosterProvider.Kind } else { 'module-not-loaded' })
                        available        = $(if ($rosterProvider) { $rosterProvider.Available } else { $false })
                        observedUtc      = $(if ($rosterProvider) { $rosterProvider.ObservedUtc } else { $null })
                        managedAllowlist = $allowlist
                        ownershipLeases  = @($ownership.leases | Where-Object { [string]$_.state -eq 'active' } | ForEach-Object { [pscustomobject]@{ guid = $_.characterGuid; job = $_.jobId } })
                        characters       = @($facts | ForEach-Object { [pscustomobject]@{
                                    guid   = $_.guid; name = $_.name; level = $_.level; class = $_.class
                                    online = $_.online; zone = $_.zone
                                    unknown = (@($_.unknown_fields) -join ',')
                                } })
                    })
            }
        }
    }
    'campaign' {
        if (-not $SubCommand) { throw 'campaign requires a subcommand: validate | plan | apply | status | explain | stop.' }
        switch ($SubCommand) {
            'validate' {
                if (-not $Path) { throw 'campaign validate requires -Path.' }
                $doc = Import-AutopilotCampaign -Path $Path
                Out-AutopilotResult ([pscustomobject]@{ valid = $true; campaign = $doc.campaign; characters = @($doc.characters).Count })
            }
            'plan' {
                if (-not $Path) { throw 'campaign plan requires -Path.' }
                $doc = Import-AutopilotCampaign -Path $Path
                $planParams = @{ Paths = $paths; Campaign = $doc; CapabilityProvider = $provider }
                $rosterProvider = Get-CliRosterProvider
                if ($rosterProvider) { $planParams['RosterProvider'] = $rosterProvider }
                if ($ProfilesDir) { $planParams['ProfilesDir'] = $ProfilesDir }
                Out-AutopilotResult (Get-AutopilotCampaignPlan @planParams)
            }
            'apply' {
                if (-not $Path) { throw 'campaign apply requires -Path.' }
                $doc = Import-AutopilotCampaign -Path $Path
                $planParams = @{ Paths = $paths; Campaign = $doc; CapabilityProvider = $provider }
                $rosterProvider = Get-CliRosterProvider
                if ($rosterProvider) { $planParams['RosterProvider'] = $rosterProvider }
                if ($ProfilesDir) { $planParams['ProfilesDir'] = $ProfilesDir }
                $campaignPlan = Get-AutopilotCampaignPlan @planParams
                Out-AutopilotResult (Invoke-AutopilotCampaignApply -Paths $paths -Campaign $doc -Plan $campaignPlan)
            }
            'status' {
                $campaignName = if ($Name) { $Name } elseif ($Path) { (Import-AutopilotCampaign -Path $Path).campaign } else { throw 'campaign status requires -Name or -Path.' }
                $status = Get-AutopilotCampaignStatus -Paths $paths -Name $campaignName
                if (-not $status) { throw "No applied campaign named '$campaignName'." }
                Out-AutopilotResult $status
            }
            'explain' {
                $campaignName = if ($Name) { $Name } elseif ($Path) { (Import-AutopilotCampaign -Path $Path).campaign } else { throw 'campaign explain requires -Name or -Path.' }
                $state = Get-AutopilotCampaignState -Paths $paths -Name $campaignName
                if (-not $state) { throw "No applied campaign named '$campaignName'." }
                $explanations = @()
                foreach ($nodeId in @($state.node_jobs.Keys)) {
                    $explanation = Get-AutopilotExplanation -Paths $paths -JobId ([string]$state.node_jobs[$nodeId]) -CapabilityProvider $provider
                    if ($explanation) { $explanations += $explanation }
                }
                Out-AutopilotResult (@($explanations))
            }
            'stop' {
                $campaignName = if ($Name) { $Name } elseif ($Path) { (Import-AutopilotCampaign -Path $Path).campaign } else { throw 'campaign stop requires -Name or -Path.' }
                Out-AutopilotResult (Stop-AutopilotCampaign -Paths $paths -Name $campaignName)
            }
            default { throw "campaign supports: validate | plan | apply | status | explain | stop (got '$SubCommand')." }
        }
    }
    'plan' {
        if ($Path) {
            $doc = Import-AutopilotCampaign -Path $Path
            $planParams = @{ Paths = $paths; Campaign = $doc; CapabilityProvider = $provider }
            $rosterProvider = Get-CliRosterProvider
            if ($rosterProvider) { $planParams['RosterProvider'] = $rosterProvider }
            if ($ProfilesDir) { $planParams['ProfilesDir'] = $ProfilesDir }
            Out-AutopilotResult (Get-AutopilotCampaignPlan @planParams)
        }
        else {
            # Global plan view: every job with wait-state and blockers.
            Out-AutopilotResult (@(Get-AutopilotJobs -Paths $paths | ForEach-Object {
                        $missing = Get-AutopilotMissingRequiredCapabilities -Kind $_.kind -Provider $provider
                        [pscustomobject]@{
                            jobId        = $_.jobId
                            kind         = $_.kind
                            phase        = $_.phase
                            waitState    = Get-AutopilotWaitState -Job $_
                            availability = $(if (@($missing).Count -gt 0) { 'planned, currently unavailable: ' + ((@($missing) | ForEach-Object { $_.capability }) -join ', ') } else { 'available for dry-run rehearsal' })
                        }
                    }))
        }
    }
    'assign' {
        switch ($SubCommand) {
            'quest' {
                if (-not $LeaderGuid -and $CharacterGuid) { $LeaderGuid = $CharacterGuid }
                if (-not $LeaderGuid) { throw 'assign quest requires -LeaderGuid (or -CharacterGuid).' }
                $level = if ($UntilLevelAlias) { $UntilLevelAlias } elseif ($TargetLevel) { $TargetLevel } else { 0 }
                $eligible = if ($Guids.Count -gt 0) { $Guids } else { @($LeaderGuid) }
                $spec = @{ partyLeaderGuid = [int64]$LeaderGuid }
                if ($level) { $spec['targetLevel'] = $level }
                Out-AutopilotResult (New-CliAssignment -Kind 'QuestLevel' -Spec $spec -EligibleGuids $eligible -SuccessCriteria @{ measure = 'quest-turnins'; counters = @{ questsTurnedIn = 1 } })
            }
            'gather' {
                if (-not $ItemId -or -not $Count) { throw 'assign gather requires -ItemId and -Count.' }
                $eligible = if ($Guids.Count -gt 0) { $Guids } elseif ($CharacterGuid) { @($CharacterGuid) } else { throw 'assign gather requires -Guids or -CharacterGuid.' }
                $spec = @{ item = @{ itemId = [int64]$ItemId; count = [int64]$Count }; stopAtCount = [int64]$Count }
                Out-AutopilotResult (New-CliAssignment -Kind 'Gather' -Spec $spec -EligibleGuids $eligible -SuccessCriteria @{ measure = 'inventory-item-count'; counters = @{ itemCount = [int64]$Count } })
            }
            'craft' {
                if (-not $ItemId -or -not $Count) { throw 'assign craft requires -ItemId and -Count.' }
                $eligible = if ($Guids.Count -gt 0) { $Guids } elseif ($CharacterGuid) { @($CharacterGuid) } else { throw 'assign craft requires -Guids or -CharacterGuid.' }
                $spec = @{ recipeSpellId = [int64]$(if ($RecipeSpellId) { $RecipeSpellId } else { 0 }); resultItemId = [int64]$ItemId; count = [int64]$Count }
                if (-not $RecipeSpellId) { $spec['recipeSpellId'] = 1; $spec['note'] = 'recipe spell id unresolved; requires recipe facts' }
                Out-AutopilotResult (New-CliAssignment -Kind 'Craft' -Spec $spec -EligibleGuids $eligible -SuccessCriteria @{ measure = 'inventory-item-count'; counters = @{ crafted = [int64]$Count } })
            }
            'dungeon' {
                if (-not $Name) { throw 'assign dungeon requires -Name.' }
                if ($Guids.Count -lt 1) { throw 'assign dungeon requires -Guids (the party pool).' }
                $roleList = if ($Roles) { @($Roles -split ',' | ForEach-Object { $_.Trim().ToLowerInvariant() }) } else { @('tank', 'healer', 'dps', 'dps', 'dps') }
                Out-AutopilotResult (New-CliAssignment -Kind 'Dungeon' -Spec @{ name = $Name; roles = $roleList } -EligibleGuids $Guids -SuccessCriteria @{ measure = 'dungeon-boss-kills'; counters = @{ bossKills = 1 } })
            }
            'raid' {
                if (-not $Name) { throw 'assign raid requires -Name.' }
                if ($Guids.Count -lt 1) { throw 'assign raid requires -Guids (the roster pool).' }
                Out-AutopilotResult (New-CliAssignment -Kind 'Raid' -Spec @{ name = $Name; rosterSize = 10; difficulty = 0 } -EligibleGuids $Guids -SuccessCriteria @{ measure = 'raid-encounter-receipts'; counters = @{ encounters = 1 } })
            }
            'battleground' {
                if (-not $Name) { $Name = 'wsg' }
                if (-not $TeamSize) { $TeamSize = 10 }
                if ($Guids.Count -lt 1) { throw 'assign battleground requires -Guids.' }
                Out-AutopilotResult (New-CliAssignment -Kind 'Battleground' -Spec @{ name = $Name.ToLowerInvariant(); teamSize = $TeamSize } -EligibleGuids $Guids -SuccessCriteria @{ measure = 'battleground-scoreboard'; counters = @{ matchesCompleted = 1 } })
            }
            default { throw "assign supports: quest | gather | craft | dungeon | raid | battleground (got '$SubCommand')." }
        }
    }
    'form-party' {
        if (-not $Roles) { throw 'form-party requires -Roles tank,healer,dps,dps,dps.' }
        if ($Guids.Count -lt 1) { throw 'form-party requires -Guids.' }
        $roleList = @($Roles -split ',' | ForEach-Object { $_.Trim().ToLowerInvariant() })
        Out-AutopilotResult (New-CliAssignment -Kind 'FormParty' -Spec @{ roles = $roleList; leaderGuid = $(if ($LeaderGuid) { [int64]$LeaderGuid } else { $null }) } -EligibleGuids $Guids -SuccessCriteria @{ measure = 'party-composition'; counters = @{ partyFormed = 1 } })
    }
    'queue' {
        if ($SubCommand -ne 'battleground') { throw "queue supports: battleground (got '$SubCommand')." }
        if (-not $Name) { $Name = 'wsg' }
        if (-not $TeamSize) { $TeamSize = 10 }
        if ($Guids.Count -lt 1) { throw 'queue battleground requires -Guids.' }
        Out-AutopilotResult (New-CliAssignment -Kind 'Battleground' -Spec @{ name = $Name.ToLowerInvariant(); teamSize = $TeamSize } -EligibleGuids $Guids -SuccessCriteria @{ measure = 'battleground-scoreboard'; counters = @{ matchesCompleted = 1 } })
    }
    'run' {
        if ($SubCommand -eq 'dungeon') {
            # Back-compat alias for `assign dungeon`.
            if (-not $Name) { throw 'run dungeon requires -Name.' }
            if ($Guids.Count -lt 1) { throw 'run dungeon requires -Guids.' }
            $roleList = if ($Roles) { @($Roles -split ',' | ForEach-Object { $_.Trim().ToLowerInvariant() }) } else { @('tank', 'healer', 'dps', 'dps', 'dps') }
            Out-AutopilotResult (New-CliAssignment -Kind 'Dungeon' -Spec @{ name = $Name; roles = $roleList } -EligibleGuids $Guids -SuccessCriteria @{ measure = 'dungeon-boss-kills'; counters = @{ bossKills = 1 } })
        }
        else {
            $runParams = @{ Paths = $paths; DryRun = $DryRun; TickSeconds = $TickSeconds }
            if ($Once) { $runParams['Once'] = $true }
            if ($DurationMinutes) { $runParams['DurationMinutes'] = $DurationMinutes }
            if ($FixtureClock) { $runParams['FixtureClock'] = $true }
            if ($PSBoundParameters.ContainsKey('FixtureStartUtc')) { $runParams['FixtureStartUtc'] = $FixtureStartUtc }
            if ($MaxTicks) { $runParams['MaxTicks'] = $MaxTicks }
            if ($FixtureRoot) { $runParams['FixtureRoot'] = $FixtureRoot }
            if ($CapabilityManifest) { $runParams['CapabilityManifest'] = $CapabilityManifest }
            if ($StopOnQuiescence) { $runParams['StopOnQuiescence'] = $true }
            $runParams['ProfileResolver'] = $profileResolver
            Out-AutopilotResult (Start-AutopilotRun @runParams)
        }
    }
    'start' {
        # Detached persistent controller: launches `run` in a background pwsh.
        $lock = if (Test-Path $paths.LockPath) { Get-Content $paths.LockPath -Raw | ConvertFrom-Json } else { $null }
        if ($lock -and (Get-Process -Id $lock.pid -ErrorAction SilentlyContinue)) {
            throw "Autopilot already running (pid $($lock.pid))."
        }
        $argumentList = @('-NoProfile', '-File', (Join-Path $PSScriptRoot 'autowow-autopilot.ps1'), 'run', '-TickSeconds', $TickSeconds)
        if ($DurationMinutes) { $argumentList += @('-DurationMinutes', $DurationMinutes) }
        if ($StateRoot) { $argumentList += @('-StateRoot', $StateRoot) }
        if ($CapabilityManifest) { $argumentList += @('-CapabilityManifest', $CapabilityManifest) }
        if ($FixtureRoot) { $argumentList += @('-FixtureRoot', $FixtureRoot) }
        $stdout = Join-Path $paths.StateRoot ('run-{0}-stdout.log' -f (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssZ'))
        $stderr = $stdout.Replace('-stdout.log', '-stderr.log')
        $process = Start-Process -FilePath 'pwsh' -ArgumentList $argumentList -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
        Out-AutopilotResult ([pscustomobject]@{ started = $true; pid = $process.Id; stdout = $stdout; stderr = $stderr })
    }
    'stop' {
        $stopFile = Join-Path $paths.StateRoot 'stop.request'
        Write-AutopilotFileAtomic -Path $stopFile -Content ('{"requested_utc":"' + (ConvertTo-AutopilotTimestamp -Time (Get-AutopilotUtcNow)) + '"}')
        Out-AutopilotResult ([pscustomobject]@{ stopRequested = $true; file = $stopFile; note = 'the running controller exits gracefully at its next tick and releases its leases' })
    }
    'history' {
        $guid = if ($CharacterGuid) { $CharacterGuid } else { throw 'history requires -CharacterGuid.' }
        if (-not (Get-Command -Name 'Get-AutopilotHistory' -ErrorAction SilentlyContinue)) { throw 'summary module not loaded.' }
        $historyParams = @{ Paths = $paths; Guid = [int64]$guid }
        if ($PSBoundParameters.ContainsKey('Since')) { $historyParams['Since'] = $Since }
        # Flatten the comma-protected return, then convert dictionaries for display.
        $records = @(Get-AutopilotHistory @historyParams | ForEach-Object { $_ } | ForEach-Object {
                if ($_ -is [System.Collections.IDictionary]) { [pscustomobject]$_ } else { $_ }
            })
        Out-AutopilotResult $records
    }
    'summary' {
        if (-not (Get-Command -Name 'Get-AutopilotSummaryData' -ErrorAction SilentlyContinue)) { throw 'summary module not loaded.' }
        $summaryParams = @{ Paths = $paths }
        if ($PSBoundParameters.ContainsKey('Since')) { $summaryParams['Since'] = $Since }
        if ($Name) { $summaryParams['Campaign'] = $Name }
        $data = Get-AutopilotSummaryData @summaryParams
        if ($AsJson) { Out-AutopilotResult $data } else { Format-AutopilotSummary -Data $data }
    }
    'pause' {
        $job = if ($JobId) { Get-AutopilotJob -Paths $paths -JobId $JobId }
        elseif ($CharacterGuid) { @(Get-AutopilotJobs -Paths $paths | Where-Object { [int64]$CharacterGuid -in @($_.characters.assignedGuids + $_.characters.eligibleGuids | ForEach-Object { [int64]$_ }) }) | Select-Object -First 1 }
        else { throw 'pause requires -JobId or -CharacterGuid.' }
        if (-not $job) { throw 'No matching job found.' }
        Out-AutopilotResult (Suspend-AutopilotJob -Paths $paths -JobId $job.jobId -Reason 'operator pause')
    }
    'resume' {
        # Resume remains resume: never resets clocks or budgets. An expired job
        # is refused with a diagnostic recommending `retry` (successor).
        if (-not $JobId) { throw 'resume requires -JobId.' }
        $result = Invoke-AutopilotResume -Paths $paths -JobId $JobId
        if (-not $result.Resumed) {
            Out-AutopilotResult ([pscustomobject]@{ resumed = $false; diagnostic = $result.Diagnostic })
        }
        else { Out-AutopilotResult $result.Job }
    }
    'retry' {
        # Honest successor: fresh job id/clocks/budgets/receipts + a distinct
        # idempotency namespace; provenance preserved; the original job and its
        # ledger are never modified.
        if (-not $JobId) { throw 'retry requires -JobId (a finished/expired job).' }
        $retryParams = @{ Paths = $paths; JobId = $JobId }
        if ($Reason) { $retryParams['RetryReason'] = $Reason }
        $successor = New-AutopilotSuccessorJob @retryParams
        Out-AutopilotResult ([pscustomobject]@{
                jobId           = $successor.jobId
                supersedes      = $successor.provenance.supersedesJobId
                retryReason     = $successor.provenance.retryReason
                phase           = $successor.phase
                mode            = $successor.executionMode
                receiptLog      = $successor.receiptLog.file
            })
    }
    'cancel' {
        if (-not $JobId) { throw 'cancel requires -JobId.' }
        $job = Get-AutopilotJob -Paths $paths -JobId $JobId
        if (-not $job) { throw "No job $JobId." }
        $stamp = ConvertTo-AutopilotTimestamp -Time (Get-AutopilotUtcNow)
        $job.cancellation.requested = $true
        $job.cancellation.requestedAt = $stamp
        $job.cancellation.reason = 'operator cancel'
        $job = Save-AutopilotJob -Paths $paths -Job $job
        if ($job.phase -notin @('Completed', 'Cancelled', 'Failed')) {
            $job.cancellation.acknowledgedAt = $stamp
            $job = Save-AutopilotJob -Paths $paths -Job $job
            $job = Invoke-AutopilotTransition -Paths $paths -Job $job -To 'Cancelled' -Reason 'operator cancel acknowledged'
            $null = Release-AutopilotOwnership -Paths $paths -JobId $job.jobId -Reason 'operator cancel'
        }
        Out-AutopilotResult $job
    }
    'explain' {
        $explainParams = @{ Paths = $paths; CapabilityProvider = $provider }
        if ($JobId) { $explainParams['JobId'] = $JobId }
        elseif ($CharacterGuid) { $explainParams['Guid'] = $CharacterGuid }
        else { throw 'explain requires -JobId or -CharacterGuid.' }
        $explanation = Get-AutopilotExplanation @explainParams
        if (-not $explanation) { throw 'No matching job found.' }
        Out-AutopilotResult $explanation
    }
    'capabilities' {
        $rows = foreach ($capability in $script:AutopilotKnownCapabilities) {
            $status = Get-AutopilotCapabilityStatus -Capability $capability -Provider $provider
            [pscustomobject]@{
                capability        = $capability
                available         = $status.available
                deployed          = $status.deployed
                nativeAdapter     = $status.nativeAdapterAvailable
                runtimeAuthorized = $status.runtimeAuthorized
                receiptExport     = $status.receiptExportAvailable
                labOnly           = $status.labOnly
                reason            = $status.reason
            }
        }
        Out-AutopilotResult ([pscustomobject]@{
                provider       = $provider.Kind
                source         = $provider.Source
                evidence       = $provider.Reason
                serverBuild    = $provider.ServerBuild
                sessionId      = $provider.SessionId
                runtimeEnabled = $provider.RuntimeEnabled
                generatedAt    = $provider.GeneratedAt
                capabilities   = @($rows)
            })
    }
    'receipts' {
        if (-not $JobId) { throw 'receipts requires -JobId (or "controller").' }
        $records = @(Get-AutopilotReceipts -Paths $paths -JobId $JobId | Select-Object -Last $Tail)
        if ($OracleReceiptsPath) {
            $oracleProvider = New-AutopilotOracleReceiptProvider -Kind jsonl -Path $OracleReceiptsPath
            $correlation = Join-AutopilotOracleEvidence -Paths $paths -JobId $JobId -Provider $oracleProvider
            Out-AutopilotResult ([pscustomobject]@{ receipts = $records; oracleEvidence = $correlation })
        }
        else {
            $unavailable = New-AutopilotOracleReceiptProvider -Kind unavailable
            Out-AutopilotResult ([pscustomobject]@{
                    receipts       = $records
                    oracleEvidence = [pscustomobject]@{ available = $false; reason = $unavailable.Reason; correlations = @() }
                })
        }
    }
    'propose-enrollment' {
        if (-not $JobId) { throw 'propose-enrollment requires -JobId.' }
        $proposalParams = @{ Paths = $paths; JobId = $JobId }
        if ($Reason) { $proposalParams['Reason'] = $Reason }
        Out-AutopilotResult (New-AutopilotEnrollmentProposal @proposalParams)
    }
    'run-once' {
        Enter-AutopilotLock -Paths $paths
        try {
            $null = Invoke-AutopilotReconcile -Paths $paths
            $tickParams = @{ Paths = $paths; CapabilityProvider = $provider; ProfileResolver = $profileResolver }
            if ($FixtureRoot) { $tickParams['FixtureRoot'] = $FixtureRoot }
            if ($ConfirmLiveBridge) { $tickParams['ConfirmLiveBridge'] = $true }
            $tick = Invoke-AutopilotScheduledTick @tickParams
            Out-AutopilotResult (@($tick.Results))
        }
        finally {
            Exit-AutopilotLock -Paths $paths
        }
    }
    'reconcile' {
        Enter-AutopilotLock -Paths $paths
        try { Out-AutopilotResult (Invoke-AutopilotReconcile -Paths $paths) }
        finally { Exit-AutopilotLock -Paths $paths }
    }
    'manifest' {
        switch ($SubCommand) {
            'generate' {
                # Binds observed reality (worldserver sha, module fingerprint,
                # session identity) + Sol-authored declarations/proofs into a
                # manifest. Publication grade requires -AsSolPublication and is
                # refused unless every binding is established. Read-only probes;
                # zero bridge mutations.
                if (-not $OutPath) { throw 'manifest generate requires -OutPath (use a staging path; only Sol publishes to work\autopilot\capabilities.json).' }
                $generateParams = @{ OutPath = $OutPath; ProjectRoot = $paths.ProjectRoot }
                if ($DeclarationsPath) { $generateParams['DeclarationsPath'] = $DeclarationsPath }
                if ($ProofsPath) { $generateParams['ProofsPath'] = $ProofsPath }
                if ($AsSolPublication) { $generateParams['AsSolPublication'] = $true }
                if ($SkipWsl) { $generateParams['SkipWsl'] = $true }
                if ($WorldserverSha256Override) { $generateParams['WorldserverSha256Override'] = $WorldserverSha256Override }
                if ($TopologyManifestPath) { $generateParams['TopologyManifestPath'] = $TopologyManifestPath }
                if ($PSBoundParameters.ContainsKey('MaxAgeMinutes')) { $generateParams['MaxAgeMinutes'] = $MaxAgeMinutes }
                $generated = New-AutopilotDeployedManifest @generateParams
                Out-AutopilotResult ([pscustomobject]@{
                        path      = $generated.Path
                        source    = $generated.Source
                        build     = $generated.Manifest.server_build
                        sessionId = $generated.Manifest.session_id
                        sha256    = $generated.Manifest.worldserver_sha256
                        notes     = @($generated.Manifest.notes)
                    })
            }
            'verify' {
                $manifestFile = if ($Path) { $Path } elseif ($CapabilityManifest) { $CapabilityManifest } else { Join-Path $paths.ProjectRoot 'work\autopilot\capabilities.json' }
                $verifyParams = @{ ManifestPath = $manifestFile; ProjectRoot = $paths.ProjectRoot; MaxAgeMinutes = $MaxAgeMinutes }
                if ($SkipWsl) { $verifyParams['SkipWsl'] = $true }
                Out-AutopilotResult (Test-AutopilotDeployedManifest @verifyParams)
            }
            default { throw "manifest supports: generate | verify (got '$SubCommand')." }
        }
    }
}
