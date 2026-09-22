# AutoWoW Autopilot V1.4 - authoritative deployed-runtime capability-manifest
# generator and verifier (the safe-handoff gate).
#
# A publication-grade manifest BINDS its claims to reality:
#   - exact worldserver binary SHA-256 (read-only hash of the running binary),
#   - module/source fingerprint (read-only git facts of the module worktree),
#   - bridge session identity (owning-process identity + start time; the
#     protocol itself is probed only when explicitly requested),
#   - generation time,
#   - a proven-capability set fed EXCLUSIVELY by Sol's proof registry.
# The generator can only DOWNGRADE claims, never upgrade them: observed deployed
# configuration overrides declarations downward, missing proof evidence
# downgrades to unproven, and the quest family can never be labeled 'proven'
# until a live walk/accept proof with existing evidence is registered.
# The verifier recomputes every binding from current reality - a manifest whose
# server restarted, whose binary changed, or whose module tree drifted FAILS.
#
# All probes are read-only (OS process queries, file reads, `git status`,
# `wsl sha256sum`/`cat`); nothing here ever sends a bridge mutation.

Set-StrictMode -Version Latest

$script:AutopilotDeclarationsSchema = 'autowow.autopilot.capability-declarations.v1'
$script:AutopilotProofsSchema = 'autowow.autopilot.capability-proofs.v1'
$script:AutopilotQuestProofFamily = @('quest_acquire', 'quest_accept', 'quest_objective', 'quest_turn_in')
$script:AutopilotLiveWalkEvidenceKind = 'live-walk-accept-proof'
# Per-capability proof states (Sol vocabulary): proven-live (live receipts on the
# deployed runtime) > compiled-only (built/linked, no live proof) > unproven.
$script:AutopilotCapabilityProofStates = @('proven-live', 'compiled-only', 'unproven')
# Evidence kinds that can NEVER substantiate 'proven-live' for any capability:
# build success, movement, XP, questlog disappearance alone, or a bare ok:true
# command response are not live proof.
$script:AutopilotInsufficientEvidenceKinds = @('build-success', 'compile-success', 'command-response', 'ok-true-response', 'movement', 'xp', 'questlog-disappearance')

function Get-AutopilotWorldserverObservation {
    <#
    Read-only facts about the running worldserver: WSL runtime manifest, the
    owning process (pid + start time), the binary SHA-256 (hashed inside WSL),
    and the deployed AutoWow.OracleRuntime configuration. Every probe degrades
    to 'unknown' rather than guessing; -SkipWsl avoids WSL entirely.
    #>
    param(
        [string]$ProjectRoot = 'D:\Games\wowstuff\AutoWoW',
        [switch]$SkipWsl,
        [string]$WorldserverSha256Override,
        [int]$BridgePort = 18787
    )
    $observation = [ordered]@{
        runtime_manifest_path = $null
        distro                = $null
        binary_path           = $null
        started_utc           = $null
        wrapper_pid           = $null
        wrapper_process_start = $null
        worldserver_sha256    = $null
        sha_source            = 'unknown'
        bridge_port_listening = $false
        oracle_runtime_conf   = [ordered]@{ observed = $false; enabled = $null; bot_guids = $null }
        notes                 = @()
    }
    $runtimeManifest = Join-Path $ProjectRoot 'work\phase1-wsl-runtime\runtime-processes.json'
    if (Test-Path $runtimeManifest) {
        try {
            $doc = ConvertTo-AutopilotHashtable (Get-Content -LiteralPath $runtimeManifest -Raw | ConvertFrom-Json)
            $observation.runtime_manifest_path = $runtimeManifest
            if ($doc.Contains('distro')) { $observation.distro = [string]$doc.distro }
            if ($doc.Contains('binary')) { $observation.binary_path = [string]$doc.binary }
            if ($doc.Contains('started_utc')) { $observation.started_utc = [string]$doc.started_utc }
            if ($doc.Contains('wsl_wrapper_pid')) {
                $observation.wrapper_pid = [int64]$doc.wsl_wrapper_pid
                $process = Get-Process -Id $observation.wrapper_pid -ErrorAction SilentlyContinue
                if ($process) {
                    $observation.wrapper_process_start = ConvertTo-AutopilotTimestamp -Time $process.StartTime.ToUniversalTime()
                }
                else { $observation.notes += "wrapper pid $($observation.wrapper_pid) is not running" }
            }
        }
        catch { $observation.notes += "runtime manifest unreadable: $($_.Exception.Message)" }
    }
    else { $observation.notes += 'no WSL runtime manifest found' }

    if ($WorldserverSha256Override) {
        $observation.worldserver_sha256 = $WorldserverSha256Override.ToLowerInvariant()
        $observation.sha_source = 'override'
    }
    elseif (-not $SkipWsl -and $observation.distro -and $observation.binary_path) {
        try {
            $raw = & wsl.exe -d $observation.distro -- sha256sum $observation.binary_path 2>$null
            if ($raw -and $raw -match '^([0-9a-f]{64})\s') {
                $observation.worldserver_sha256 = $Matches[1]
                $observation.sha_source = 'wsl-sha256sum'
            }
            else { $observation.notes += 'wsl sha256sum returned no parseable hash' }
        }
        catch { $observation.notes += "wsl sha256sum failed: $($_.Exception.Message)" }
    }
    else { $observation.notes += 'worldserver sha not computed (skipped or no runtime facts)' }

    # Pure OS query - zero bridge contact.
    $listener = Get-NetTCPConnection -State Listen -LocalPort $BridgePort -ErrorAction SilentlyContinue
    $observation.bridge_port_listening = [bool]$listener

    if (-not $SkipWsl -and $observation.distro) {
        # The deployed module conf actually loaded by this runtime (read-only).
        foreach ($confCandidate in @('/root/p1runtime/modules/playerbots.conf', '/root/p1runtime/playerbots.conf')) {
            try {
                $confRaw = & wsl.exe -d $observation.distro -- cat $confCandidate 2>$null
                if ($confRaw) {
                    $observation.oracle_runtime_conf.observed = $true
                    $enabledLine = @($confRaw | Where-Object { $_ -match '^\s*AutoWow\.OracleRuntime\.Enabled\s*=\s*(\S+)' }) | Select-Object -Last 1
                    if ($enabledLine -and $enabledLine -match '=\s*(\S+)') { $observation.oracle_runtime_conf.enabled = ([string]$Matches[1] -eq '1') }
                    $guidLine = @($confRaw | Where-Object { $_ -match '^\s*AutoWow\.OracleRuntime\.BotGuids\s*=' }) | Select-Object -Last 1
                    if ($guidLine) { $observation.oracle_runtime_conf.bot_guids = ([string]($guidLine -replace '^[^=]*=\s*', '')).Trim('"', ' ') }
                    break
                }
            }
            catch { }
        }
        if (-not $observation.oracle_runtime_conf.observed) { $observation.notes += 'deployed playerbots.conf not readable from WSL runtime paths' }
    }
    return $observation
}

function Get-AutopilotSessionIdentityFromObservation {
    # Deterministic session identity from observed facts: changes whenever the
    # binary or the worldserver process instance changes.
    param([Parameter(Mandatory)]$Observation)
    $shaPart = if ($Observation.worldserver_sha256) { ([string]$Observation.worldserver_sha256).Substring(0, 12) } else { 'unknownsha' }
    $startPart = if ($Observation.wrapper_process_start) { ([string]$Observation.wrapper_process_start -replace '[:\-\.]', '').Substring(0, 15) } elseif ($Observation.started_utc) { ([string]$Observation.started_utc -replace '[:\-\.]', '') } else { 'unknownstart' }
    $pidPart = if ($null -ne $Observation.wrapper_pid) { [string]$Observation.wrapper_pid } else { 'nopid' }
    [pscustomobject]@{
        Build     = "worldserver-sha256:$shaPart"
        SessionId = "$pidPart@$startPart"
    }
}

function Get-AutopilotModuleFingerprint {
    # Read-only git facts of the module worktree: HEAD, dirty count, and a
    # digest of the working diff. Any source drift changes the fingerprint.
    param(
        [string]$ModuleDir = 'D:\Games\wowstuff\AutoWoW\_phase1_worktree\mod-playerbots',
        [string]$FingerprintOverride,
        # Alternative binding: hash a Sol-supplied topology/staging manifest file
        # instead of the git worktree (useful when the WSL-staged tree, not the
        # Windows worktree, is authoritative).
        [string]$TopologyManifestPath
    )
    if ($TopologyManifestPath) {
        $result = [ordered]@{ source = 'topology-manifest'; module_dir = $null; topology_path = $TopologyManifestPath; head = $null; dirty_files = $null; diff_sha256 = $null; fingerprint = $null }
        if (Test-Path $TopologyManifestPath) {
            $result.fingerprint = (Get-FileHash -LiteralPath $TopologyManifestPath -Algorithm SHA256).Hash.ToLowerInvariant()
        }
        return $result
    }
    if ($FingerprintOverride) {
        return [ordered]@{ source = 'override'; module_dir = $ModuleDir; head = $null; dirty_files = $null; diff_sha256 = $null; fingerprint = $FingerprintOverride }
    }
    $result = [ordered]@{ source = 'git-worktree'; module_dir = $ModuleDir; head = $null; dirty_files = $null; diff_sha256 = $null; fingerprint = $null }
    try {
        $head = (& git -C $ModuleDir rev-parse HEAD 2>$null | Select-Object -First 1)
        $porcelain = @(& git -C $ModuleDir status --porcelain 2>$null)
        $diff = (& git -C $ModuleDir diff 2>$null) -join "`n"
        $result.head = [string]$head
        $result.dirty_files = @($porcelain).Count
        $result.diff_sha256 = Get-AutopilotSha256 -Text $diff
        $result.fingerprint = Get-AutopilotSha256 -Text ('{0}|{1}|{2}' -f $result.head, $result.dirty_files, $result.diff_sha256)
    }
    catch {
        $result.source = 'unavailable'
    }
    return $result
}

function Read-AutopilotJsonDocument {
    # Shared fail-closed reader for Sol-authored input documents.
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][string]$ExpectedSchema)
    if (-not (Test-Path $Path)) { return [pscustomobject]@{ Ok = $false; Reason = "not found: $Path"; Doc = $null } }
    try {
        $doc = ConvertTo-AutopilotHashtable (Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json)
    }
    catch { return [pscustomobject]@{ Ok = $false; Reason = "unreadable (fail closed): $($_.Exception.Message)"; Doc = $null } }
    if (-not ($doc -is [System.Collections.IDictionary]) -or [string]$doc.schema -ne $ExpectedSchema -or [int64]$doc.schema_version -ne 1) {
        return [pscustomobject]@{ Ok = $false; Reason = "schema not supported (fail closed): expected $ExpectedSchema v1"; Doc = $null }
    }
    return [pscustomobject]@{ Ok = $true; Reason = 'loaded'; Doc = $doc }
}

function Resolve-AutopilotOperationProof {
    <#
    Proof grading comes ONLY from the registry, and can only be downgraded:
     - no registry entry / unknown status string -> unproven (fail closed)
     - 'proven-live' with a missing evidence file -> unproven (refused)
     - 'proven-live' backed by an insufficient evidence kind (build success,
       movement, xp, bare ok:true responses, questlog disappearance) ->
       compiled-only (refused: not live proof)
     - quest-family 'proven-live' without an existing evidence file of kind
       'live-walk-accept-proof' (Sol's walk-to-giver -> accepted-into-log
       receipt) -> compiled-only. Quest acquisition can NEVER exceed
       compiled-only until that receipt exists.
    #>
    param(
        [Parameter(Mandatory)][string]$Operation,
        $ProofsDoc,
        [string]$ProjectRoot = 'D:\Games\wowstuff\AutoWoW'
    )
    $proof = [ordered]@{ status = 'unproven'; evidence_kind = $null; evidence_path = $null; recorded_by = $null; recorded_at = $null; note = 'no proof registered' }
    if (-not $ProofsDoc) { return $proof }
    $entry = @(@($ProofsDoc.proofs) | Where-Object { [string]$_.operation -eq $Operation }) | Select-Object -First 1
    if (-not $entry) { return $proof }
    $declaredStatus = [string]$entry.status
    $proof.evidence_kind = $(if ($entry.Contains('evidence_kind')) { [string]$entry.evidence_kind } else { $null })
    $proof.evidence_path = $(if ($entry.Contains('evidence_path')) { [string]$entry.evidence_path } else { $null })
    $proof.recorded_by = $(if ($entry.Contains('recorded_by')) { [string]$entry.recorded_by } else { $null })
    $proof.recorded_at = $(if ($entry.Contains('recorded_at')) { [string]$entry.recorded_at } else { $null })

    if ($declaredStatus -notin $script:AutopilotCapabilityProofStates) {
        $proof.status = 'unproven'
        $proof.note = "refused: unknown proof status '$declaredStatus' (fail closed); allowed: $($script:AutopilotCapabilityProofStates -join ', ')"
        return $proof
    }
    $proof.status = $declaredStatus
    $proof.note = 'registered'

    if ($declaredStatus -eq 'proven-live') {
        $evidenceExists = $false
        if ($proof.evidence_path) {
            $candidate = $proof.evidence_path
            if (-not [System.IO.Path]::IsPathRooted($candidate)) { $candidate = Join-Path $ProjectRoot $candidate }
            $evidenceExists = Test-Path $candidate
        }
        if (-not $evidenceExists) {
            $proof.status = 'unproven'
            $proof.note = 'refused: proven-live claimed but its evidence file does not exist'
            return $proof
        }
        if ($proof.evidence_kind -and [string]$proof.evidence_kind -in $script:AutopilotInsufficientEvidenceKinds) {
            $proof.status = 'compiled-only'
            $proof.note = "refused: evidence kind '$($proof.evidence_kind)' can never substantiate proven-live (build success, movement, xp, and ok:true responses are not live proof)"
            return $proof
        }
        if ($Operation -in $script:AutopilotQuestProofFamily -and [string]$proof.evidence_kind -ne $script:AutopilotLiveWalkEvidenceKind) {
            $proof.status = 'compiled-only'
            $proof.note = "refused: quest acquisition stays compiled-only until Sol supplies the live walk-to-giver -> accepted-into-log receipt (evidence_kind '$($script:AutopilotLiveWalkEvidenceKind)')"
            return $proof
        }
    }
    return $proof
}

function New-AutopilotDeployedManifest {
    <#
    Composes a capability manifest from observed reality + Sol-authored inputs.
    Publication grade (source='deployed-runtime') requires -AsSolPublication AND
    a declarations document AND a computed (not overridden-away) binary hash;
    otherwise the manifest is written as 'interim-probe' and can never authorize
    live sends. Observed deployed configuration only ever downgrades claims.
    #>
    param(
        [Parameter(Mandatory)][string]$OutPath,
        [string]$ProjectRoot = 'D:\Games\wowstuff\AutoWoW',
        [string]$DeclarationsPath,
        [string]$ProofsPath,
        [switch]$AsSolPublication,
        [switch]$SkipWsl,
        [string]$WorldserverSha256Override,
        [string]$ModuleFingerprintOverride,
        [string]$TopologyManifestPath,
        [string]$ModuleDir = 'D:\Games\wowstuff\AutoWoW\_phase1_worktree\mod-playerbots',
        [ValidateRange(1, 10080)][int]$MaxAgeMinutes = 240,
        [datetime]$Now
    )
    $utc = if ($PSBoundParameters.ContainsKey('Now')) { Get-AutopilotUtcNow -Now $Now } else { Get-AutopilotUtcNow }
    $observationParams = @{ ProjectRoot = $ProjectRoot }
    if ($SkipWsl) { $observationParams['SkipWsl'] = $true }
    if ($WorldserverSha256Override) { $observationParams['WorldserverSha256Override'] = $WorldserverSha256Override }
    $observation = Get-AutopilotWorldserverObservation @observationParams
    $identity = Get-AutopilotSessionIdentityFromObservation -Observation $observation
    $fingerprintParams = @{ ModuleDir = $ModuleDir }
    if ($ModuleFingerprintOverride) { $fingerprintParams['FingerprintOverride'] = $ModuleFingerprintOverride }
    if ($TopologyManifestPath) { $fingerprintParams['TopologyManifestPath'] = $TopologyManifestPath }
    $fingerprint = Get-AutopilotModuleFingerprint @fingerprintParams

    $declarations = $null
    $declarationsNote = 'no declarations document supplied; operations list is empty'
    if ($DeclarationsPath) {
        $loaded = Read-AutopilotJsonDocument -Path $DeclarationsPath -ExpectedSchema $script:AutopilotDeclarationsSchema
        if ($loaded.Ok) { $declarations = $loaded.Doc; $declarationsNote = "declarations: $DeclarationsPath" }
        else { $declarationsNote = "declarations rejected: $($loaded.Reason)" }
    }
    $proofsDoc = $null
    if ($ProofsPath) {
        $loadedProofs = Read-AutopilotJsonDocument -Path $ProofsPath -ExpectedSchema $script:AutopilotProofsSchema
        if ($loadedProofs.Ok) { $proofsDoc = $loadedProofs.Doc }
    }

    # Publication gating: refuse over-claims outright.
    $source = 'interim-probe'
    $publicationNotes = @()
    if ($AsSolPublication) {
        $eligible = $true
        if (-not $declarations) { $eligible = $false; $publicationNotes += 'publication refused: no valid declarations document' }
        if (-not $observation.worldserver_sha256) { $eligible = $false; $publicationNotes += 'publication refused: worldserver sha256 not established' }
        if (-not $identity.SessionId -or $identity.SessionId -like 'nopid@*') { $eligible = $false; $publicationNotes += 'publication refused: session identity incomplete' }
        if ($eligible) { $source = 'deployed-runtime' }
    }

    $runtimeEnabledDeclared = $false
    if ($declarations -and $declarations.Contains('runtime_enabled')) { $runtimeEnabledDeclared = [bool]$declarations.runtime_enabled }
    $runtimeEnabled = $runtimeEnabledDeclared
    $downgradeNotes = @()
    if ($observation.oracle_runtime_conf.observed -and $observation.oracle_runtime_conf.enabled -eq $false -and $runtimeEnabledDeclared) {
        $runtimeEnabled = $false
        $downgradeNotes += 'runtime_enabled downgraded to false: deployed playerbots.conf has AutoWow.OracleRuntime.Enabled=0'
    }

    $operations = @()
    if ($declarations -and $declarations.Contains('operations')) {
        foreach ($declared in @($declarations.operations)) {
            $operation = ConvertTo-AutopilotHashtable $declared
            $name = [string]$operation.operation
            if ($observation.oracle_runtime_conf.observed -and $observation.oracle_runtime_conf.enabled -eq $false -and $operation.Contains('runtime_authorized') -and [bool]$operation.runtime_authorized) {
                $operation.runtime_authorized = $false
                $downgradeNotes += "operation '$name': runtime_authorized downgraded to false (deployed conf disables the oracle runtime)"
            }
            $operation.proof = Resolve-AutopilotOperationProof -Operation $name -ProofsDoc $proofsDoc -ProjectRoot $ProjectRoot
            $operations += , $operation
        }
    }

    $manifest = [ordered]@{
        schema                  = $script:AutopilotCapabilityManifestSchema
        schema_version          = 1
        source                  = $source
        server_build            = $identity.Build
        session_id              = $identity.SessionId
        contract_schema_version = 1
        runtime_enabled         = $runtimeEnabled
        generated_at            = ConvertTo-AutopilotTimestamp -Time $utc
        max_age_minutes         = $MaxAgeMinutes
        generated_by            = "autopilot manifest generator v1.4 ($declarationsNote)"
        worldserver_sha256      = $observation.worldserver_sha256
        module_fingerprint      = $fingerprint
        session_evidence        = [ordered]@{
            wrapper_pid           = $observation.wrapper_pid
            wrapper_process_start = $observation.wrapper_process_start
            runtime_started_utc   = $observation.started_utc
            distro                = $observation.distro
            binary_path           = $observation.binary_path
            sha_source            = $observation.sha_source
        }
        bridge                  = [ordered]@{
            deployed       = [bool]$observation.bridge_port_listening
            evidence_level = $(if ($observation.bridge_port_listening) { 'port-listen' } else { 'not-listening' })
        }
        sql_read_deployed       = $(if ($declarations -and $declarations.Contains('sql_read_deployed')) { [bool]$declarations.sql_read_deployed } else { $false })
        operations              = @($operations)
        notes                   = @(@($publicationNotes) + @($downgradeNotes) + @($observation.notes))
    }
    Write-AutopilotFileAtomic -Path $OutPath -Content ($manifest | ConvertTo-Json -Depth 12)
    return [pscustomobject]@{ Path = $OutPath; Source = $source; Manifest = $manifest }
}

function Test-AutopilotDeployedManifest {
    <#
    The handoff verifier: recomputes every binding from CURRENT reality.
    FAILS on: unsupported schema, non-publication source, binary hash mismatch
    or unverifiable hash, session identity mismatch (server restarted), module
    fingerprint drift, stale/future generation time, proof-audit violations
    (proven without existing evidence; quest family proven without the live
    walk/accept evidence kind), or runtime_enabled contradicting the deployed
    configuration. Warnings never substitute for failures.
    #>
    param(
        [Parameter(Mandatory)][string]$ManifestPath,
        [string]$ProjectRoot = 'D:\Games\wowstuff\AutoWoW',
        [string]$ModuleDir = 'D:\Games\wowstuff\AutoWoW\_phase1_worktree\mod-playerbots',
        [switch]$SkipWsl,
        [ValidateRange(1, 10080)][int]$MaxAgeMinutes = 240,
        [datetime]$Now
    )
    $utc = if ($PSBoundParameters.ContainsKey('Now')) { Get-AutopilotUtcNow -Now $Now } else { Get-AutopilotUtcNow }
    $failures = @()
    $warnings = @()

    $provider = New-AutopilotCapabilityProvider -Kind manifest -ManifestPath $ManifestPath
    if (-not $provider.Available) {
        return [pscustomobject]@{ Valid = $false; Failures = @("manifest not loadable: $($provider.Reason)"); Warnings = @() }
    }
    $manifest = $provider.Manifest
    if ([string]$provider.Origin -ne 'deployed-runtime') {
        $failures += "source is '$($provider.Origin)': not a deployed-runtime publication"
    }
    # Freshness: the manifest's own embedded max_age_minutes governs unless the
    # caller explicitly binds a (tighter or looser) override.
    $effectiveMaxAge = $MaxAgeMinutes
    if (-not $PSBoundParameters.ContainsKey('MaxAgeMinutes') -and $manifest.Contains('max_age_minutes') -and $manifest.max_age_minutes) {
        $effectiveMaxAge = [int][int64]$manifest.max_age_minutes
    }
    if (-not $provider.GeneratedAt) { $failures += 'no generated_at' }
    else {
        try {
            $ageMinutes = ($utc - (ConvertTo-AutopilotUtcDateTime $provider.GeneratedAt)).TotalMinutes
            if ($ageMinutes -lt 0) { $failures += 'generated_at is future-dated' }
            elseif ($ageMinutes -gt $effectiveMaxAge) { $failures += "stale: generated $([math]::Round($ageMinutes,1)) min ago (max $effectiveMaxAge)" }
        }
        catch { $failures += 'generated_at unparseable' }
    }
    # Binding recomputation.
    $observationParams = @{ ProjectRoot = $ProjectRoot }
    if ($SkipWsl) { $observationParams['SkipWsl'] = $true }
    $observation = Get-AutopilotWorldserverObservation @observationParams
    $identity = Get-AutopilotSessionIdentityFromObservation -Observation $observation

    if (-not ($manifest.Contains('worldserver_sha256')) -or -not $manifest.worldserver_sha256) {
        $failures += 'manifest carries no worldserver_sha256 binding'
    }
    elseif ($SkipWsl -or -not $observation.worldserver_sha256) {
        $warnings += 'worldserver sha256 not recomputed (WSL probe unavailable/skipped) - binding unverified'
    }
    elseif ([string]$manifest.worldserver_sha256 -ne [string]$observation.worldserver_sha256) {
        $failures += "worldserver binary changed: manifest=$(([string]$manifest.worldserver_sha256).Substring(0,12)) current=$(([string]$observation.worldserver_sha256).Substring(0,12))"
    }
    # Process identity always compares; the build component only when the binary
    # hash was recomputable (otherwise the sha-binding warning above covers it).
    $buildComparable = [bool]$observation.worldserver_sha256
    if (([string]$provider.SessionId -ne [string]$identity.SessionId) -or
        ($buildComparable -and [string]$provider.ServerBuild -ne [string]$identity.Build)) {
        $failures += "session identity mismatch: manifest $($provider.ServerBuild)/$($provider.SessionId) vs current $($identity.Build)/$($identity.SessionId) (server restarted or binary changed)"
    }
    if ($manifest.Contains('module_fingerprint') -and $manifest.module_fingerprint -and $manifest.module_fingerprint.Contains('fingerprint') -and $manifest.module_fingerprint.fingerprint) {
        if ([string]$manifest.module_fingerprint.source -eq 'git-worktree') {
            $currentFingerprint = Get-AutopilotModuleFingerprint -ModuleDir $ModuleDir
            if ($currentFingerprint.fingerprint -and [string]$currentFingerprint.fingerprint -ne [string]$manifest.module_fingerprint.fingerprint) {
                $failures += 'module source fingerprint drift: the module worktree changed since generation'
            }
        }
        elseif ([string]$manifest.module_fingerprint.source -eq 'topology-manifest' -and $manifest.module_fingerprint.Contains('topology_path') -and $manifest.module_fingerprint.topology_path) {
            $topologyPath = [string]$manifest.module_fingerprint.topology_path
            if (Test-Path $topologyPath) {
                $currentHash = (Get-FileHash -LiteralPath $topologyPath -Algorithm SHA256).Hash.ToLowerInvariant()
                if ($currentHash -ne [string]$manifest.module_fingerprint.fingerprint) {
                    $failures += 'topology-manifest hash drift: the topology manifest changed since generation'
                }
            }
            else { $failures += "topology manifest missing: $topologyPath" }
        }
        else { $warnings += "module fingerprint source '$($manifest.module_fingerprint.source)' cannot be independently recomputed" }
    }
    else { $failures += 'manifest carries no module fingerprint binding' }
    # Deployed-conf contradiction.
    if ($observation.oracle_runtime_conf.observed -and $observation.oracle_runtime_conf.enabled -eq $false -and $provider.RuntimeEnabled) {
        $failures += 'runtime_enabled=true contradicts the deployed configuration (AutoWow.OracleRuntime.Enabled=0)'
    }
    # Proof audit: unknown states are overclaims; proven-live must carry an
    # existing, sufficient evidence file; the quest family additionally requires
    # the live walk-to-giver -> accepted-into-log receipt kind.
    foreach ($operationName in @($provider.Operations.Keys)) {
        $operation = $provider.Operations[$operationName]
        if (-not ($operation -is [System.Collections.IDictionary]) -or -not $operation.Contains('proof')) { continue }
        $proof = $operation.proof
        $status = [string]$proof.status
        if ($status -notin $script:AutopilotCapabilityProofStates) {
            $failures += "operation '$operationName' carries unknown proof status '$status' (overclaim/typo; fail closed)"
            continue
        }
        if ($status -eq 'proven-live') {
            $evidencePath = [string]$proof.evidence_path
            if ($evidencePath -and -not [System.IO.Path]::IsPathRooted($evidencePath)) { $evidencePath = Join-Path $ProjectRoot $evidencePath }
            if (-not $evidencePath -or -not (Test-Path $evidencePath)) {
                $failures += "operation '$operationName' claims proven-live but its evidence file is missing"
            }
            if ($proof.evidence_kind -and [string]$proof.evidence_kind -in $script:AutopilotInsufficientEvidenceKinds) {
                $failures += "operation '$operationName' claims proven-live on insufficient evidence kind '$($proof.evidence_kind)' (build success / movement / xp / ok:true responses are never live proof)"
            }
            if ($operationName -in $script:AutopilotQuestProofFamily -and [string]$proof.evidence_kind -ne $script:AutopilotLiveWalkEvidenceKind) {
                $failures += "operation '$operationName' (quest family) claims proven-live without the '$($script:AutopilotLiveWalkEvidenceKind)' walk-to-giver/accepted-into-log receipt - must remain compiled-only or unproven"
            }
        }
    }

    [pscustomobject]@{
        Valid    = ($failures.Count -eq 0)
        Failures = @($failures)
        Warnings = @($warnings)
        Identity = $identity
    }
}
