# AutoWoW Autopilot V1 - core library (minimal slice).
# Strategic job controller: persistent typed jobs, deterministic state machine,
# append-only receipts, dry-run-by-default bridge adapter.
# It composes the existing guarded surfaces (autowow-control.ps1 wire builder,
# QuestLogSource.ps1 Send-Bridge) and never opens a live socket unless the
# operator passes the live double gate. See AUTOWOW_AUTOPILOT_V1_PLAN.md.

Set-StrictMode -Version Latest

$script:AutopilotReceiptSchema = 'autowow.autopilot.receipt.v1'
$script:AutopilotJobSchemaVersion = 1

$script:AutopilotJobKinds = @(
    'QuestLevel', 'Gather', 'Craft', 'TrainProfession', 'VendorRepair', 'BankDeposit',
    'MailTransfer', 'FormParty', 'Travel', 'Dungeon', 'Raid', 'Battleground',
    'OpenWorldPvp', 'Restock', 'Idle'
)

$script:AutopilotPhases = @(
    'Queued', 'Validating', 'Preparing', 'Traveling', 'Executing', 'Verifying',
    'Completed', 'Blocked', 'Recovering', 'Suspended', 'Cancelled', 'Failed'
)

$script:AutopilotTerminalPhases = @('Completed', 'Cancelled', 'Failed')

$script:AutopilotBlockedCodes = @(
    'prerequisite-unmet', 'capability-missing', 'oracle-capability-not-deployed',
    'character-unavailable', 'character-offline', 'character-dead-unrecoverable',
    'bags-full', 'gear-broken', 'out-of-consumables', 'insufficient-funds',
    'no-legal-action', 'server-unreachable', 'bridge-timeout', 'lease-denied',
    'stale-state', 'roster-conflict', 'roster-owned', 'party-contract-unmet', 'quest-flow-blocked', 'operator-suspended', 'recovery-budget-exhausted',
    'phase-timeout', 'job-timeout', 'awaiting-verification'
)

# Legal transitions. Suspended additionally resumes to the phase recorded in
# metadata.suspendedFromPhase (checked in Invoke-AutopilotTransition).
$script:AutopilotLegalTransitions = @{
    'Queued'     = @('Validating', 'Cancelled', 'Suspended')
    'Validating' = @('Preparing', 'Blocked', 'Cancelled', 'Failed', 'Suspended')
    'Preparing'  = @('Traveling', 'Executing', 'Blocked', 'Cancelled', 'Failed', 'Suspended')
    'Traveling'  = @('Executing', 'Recovering', 'Blocked', 'Cancelled', 'Failed', 'Suspended')
    'Executing'  = @('Verifying', 'Recovering', 'Blocked', 'Cancelled', 'Failed', 'Suspended')
    'Verifying'  = @('Completed', 'Executing', 'Recovering', 'Blocked', 'Cancelled', 'Failed', 'Suspended')
    'Recovering' = @('Traveling', 'Executing', 'Blocked', 'Cancelled', 'Failed', 'Suspended')
    'Blocked'    = @('Validating', 'Cancelled', 'Failed', 'Suspended')
    'Suspended'  = @('Cancelled')
    'Completed'  = @()
    'Cancelled'  = @()
    'Failed'     = @()
}

# V1.1 (Sol decision #4): capability availability is NEVER inferred from source
# files, unit tests, or wire-verb existence. It comes only from a deployed-runtime
# capability manifest (schema autowow.oracle.capabilities.v1) or, later, a bridge
# endpoint. Without such evidence every capability is undeployed (fail closed).
$script:AutopilotLabOnlyCapabilities = @('bridge.fixture.init', 'bridge.fixture.status', 'bridge.probe.reset')
$script:AutopilotCapabilityManifestSchema = 'autowow.oracle.capabilities.v1'

$script:AutopilotKnownCapabilities = @(
    'bridge.list', 'bridge.snapshot', 'bridge.destinations', 'bridge.questlog',
    'bridge.questobjective', 'bridge.acceptance', 'bridge.combatlog', 'bridge.encounterlog',
    'bridge.quest', 'bridge.quest.acquire', 'bridge.travel', 'bridge.recover',
    'bridge.pause', 'bridge.resume', 'bridge.party', 'bridge.rally', 'bridge.deploy',
    'bridge.route', 'bridge.advance', 'bridge.advancepoint', 'bridge.engage',
    'bridge.scout', 'bridge.pathprobe', 'bridge.boss', 'bridge.activate', 'bridge.deactivate',
    'bridge.wsg.queue', 'bridge.wsg.status', 'bridge.wsg.leave',
    'bridge.raid.create', 'bridge.raid.status', 'bridge.raid.leave', 'bridge.observe',
    'bridge.fixture.init', 'bridge.fixture.status', 'bridge.probe.reset',
    'oracle.combat_action', 'oracle.navigate', 'oracle.quest_acquire', 'oracle.quest_accept',
    'oracle.quest_objective', 'oracle.quest_turn_in', 'oracle.gather_route', 'oracle.gather_source',
    'oracle.craft', 'oracle.bank_withdraw', 'oracle.bank_deposit', 'oracle.mail_receive',
    'oracle.mail_send', 'oracle.vendor_buy', 'oracle.vendor_sell', 'oracle.auction_buy',
    'oracle.auction_sell', 'oracle.roster_assignment', 'oracle.recover',
    'oracle.pvp_objective', 'oracle.pvp_challenge', 'oracle.disengage',
    'sql.read'
)

function New-AutopilotCapabilityProvider {
    <#
    Provider kinds:
      unavailable - no deployed-runtime evidence exists; everything undeployed. DEFAULT.
      manifest    - a static autowow.oracle.capabilities.v1 document (fixture or
                    Sol-published file). Unknown schema versions and malformed
                    documents degrade to unavailable (fail closed), with a reason.
      bridge      - reserved future endpoint; until it exists it reports
                    unavailable rather than scraping anything.
    #>
    param(
        [Parameter(Mandatory)][ValidateSet('unavailable', 'manifest', 'bridge')][string]$Kind,
        [string]$ManifestPath
    )
    $provider = [ordered]@{
        Kind         = $Kind
        Source       = $null
        Available    = $false
        Reason       = 'no deployed-runtime capability evidence'
        Manifest     = $null
        ServerBuild  = $null
        SessionId    = $null
        RuntimeEnabled = $false
        BridgeDeployed = $false
        SqlReadDeployed = $false
        Operations   = @{}
        GeneratedAt  = $null
        # Provenance: 'deployed-runtime' (Sol-published, live-eligible),
        # 'interim-probe' / 'fixture' / 'unspecified' (NEVER live-eligible).
        Origin       = 'unspecified'
    }
    switch ($Kind) {
        'unavailable' { }
        'bridge' {
            $provider.Reason = 'bridge capability endpoint is not deployed yet; refusing to infer (fail closed)'
        }
        'manifest' {
            if (-not $ManifestPath) { throw 'manifest provider requires -ManifestPath.' }
            $provider.Source = $ManifestPath
            if (-not (Test-Path $ManifestPath)) {
                $provider.Reason = "manifest not found: $ManifestPath"
                break
            }
            try {
                $doc = ConvertTo-AutopilotHashtable (Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json)
            }
            catch {
                $provider.Reason = "manifest unreadable (fail closed): $($_.Exception.Message)"
                break
            }
            if (-not ($doc -is [System.Collections.IDictionary]) -or -not $doc.Contains('schema') -or
                [string]$doc.schema -ne $script:AutopilotCapabilityManifestSchema -or
                -not $doc.Contains('schema_version') -or [int64]$doc.schema_version -ne 1) {
                $provider.Reason = "manifest schema not supported (fail closed): expected $($script:AutopilotCapabilityManifestSchema) v1"
                break
            }
            $provider.Manifest = $doc
            $provider.Available = $true
            $provider.Reason = 'manifest loaded'
            $provider.ServerBuild = if ($doc.Contains('server_build')) { [string]$doc.server_build } else { $null }
            $provider.SessionId = if ($doc.Contains('session_id')) { [string]$doc.session_id } else { $null }
            $provider.RuntimeEnabled = if ($doc.Contains('runtime_enabled')) { [bool]$doc.runtime_enabled } else { $false }
            $provider.GeneratedAt = if ($doc.Contains('generated_at')) { [string]$doc.generated_at } else { $null }
            if ($doc.Contains('source')) { $provider.Origin = [string]$doc.source }
            if ($doc.Contains('bridge') -and $doc.bridge -is [System.Collections.IDictionary] -and $doc.bridge.Contains('deployed')) {
                $provider.BridgeDeployed = [bool]$doc.bridge.deployed
            }
            if ($doc.Contains('sql_read_deployed')) { $provider.SqlReadDeployed = [bool]$doc.sql_read_deployed }
            if ($doc.Contains('operations')) {
                foreach ($op in @($doc.operations)) {
                    if ($op -is [System.Collections.IDictionary] -and $op.Contains('operation')) {
                        $provider.Operations[[string]$op.operation] = $op
                    }
                }
            }
        }
    }
    return [pscustomobject]$provider
}

function Test-AutopilotCapabilityProviderLiveEligible {
    <#
    Live mutation gate (Sol #4 hygiene): a manifest may authorize LIVE sends
    only when it is (a) loadable, (b) explicitly published from the deployed
    runtime (source = 'deployed-runtime' - interim probes and fixtures never
    qualify), and (c) fresh. Dry-run rehearsal and capability display are
    unaffected. Fail closed on every missing/unparseable field.
    #>
    param(
        [Parameter(Mandatory)]$Provider,
        [ValidateRange(1, 10080)][int]$MaxAgeMinutes = 240,
        [datetime]$Now
    )
    $utc = if ($PSBoundParameters.ContainsKey('Now')) { Get-AutopilotUtcNow -Now $Now } else { Get-AutopilotUtcNow }
    if (-not $Provider -or -not $Provider.Available) {
        return [pscustomobject]@{ Eligible = $false; Reason = 'no capability manifest is loaded' }
    }
    if ([string]$Provider.Origin -ne 'deployed-runtime') {
        return [pscustomobject]@{ Eligible = $false; Reason = "manifest source is '$($Provider.Origin)'; live sends require a Sol-published 'deployed-runtime' manifest" }
    }
    if (-not $Provider.GeneratedAt) {
        return [pscustomobject]@{ Eligible = $false; Reason = 'manifest has no generated_at timestamp' }
    }
    try {
        $ageMinutes = ($utc - (ConvertTo-AutopilotUtcDateTime $Provider.GeneratedAt)).TotalMinutes
    }
    catch {
        return [pscustomobject]@{ Eligible = $false; Reason = 'manifest generated_at is unparseable' }
    }
    if ($ageMinutes -lt 0 -or $ageMinutes -gt $MaxAgeMinutes) {
        return [pscustomobject]@{ Eligible = $false; Reason = "manifest is stale or future-dated (age $([math]::Round($ageMinutes,1)) min, max $MaxAgeMinutes)" }
    }
    if (-not $Provider.ServerBuild -or -not $Provider.SessionId) {
        return [pscustomobject]@{ Eligible = $false; Reason = 'manifest lacks server_build/session_id identity' }
    }
    return [pscustomobject]@{ Eligible = $true; Reason = "deployed-runtime manifest, age $([math]::Round($ageMinutes,1)) min, session $($Provider.ServerBuild)/$($Provider.SessionId)" }
}

function Resolve-AutopilotCapabilityProvider {
    # Explicit manifest path wins; otherwise the Sol-published conventional path
    # (work\autopilot\capabilities.json) if present; otherwise unavailable.
    param([Parameter(Mandatory)]$Paths, [string]$ManifestPath)
    if ($ManifestPath) { return New-AutopilotCapabilityProvider -Kind manifest -ManifestPath $ManifestPath }
    $conventional = Join-Path $Paths.ProjectRoot 'work\autopilot\capabilities.json'
    if (Test-Path $conventional) { return New-AutopilotCapabilityProvider -Kind manifest -ManifestPath $conventional }
    return New-AutopilotCapabilityProvider -Kind unavailable
}

function Get-AutopilotCapabilityStatus {
    param(
        [Parameter(Mandatory)][string]$Capability,
        $Provider
    )
    if (-not $Provider) { $Provider = New-AutopilotCapabilityProvider -Kind unavailable }
    $status = [ordered]@{
        capability             = $Capability
        known                  = ($Capability -in $script:AutopilotKnownCapabilities)
        labOnly                = ($Capability -in $script:AutopilotLabOnlyCapabilities)
        deployed               = $false
        nativeAdapterAvailable = $false
        runtimeAuthorized      = $false
        receiptExportAvailable = $false
        available              = $false
        constraints            = $null
        source                 = $Provider.Kind
        reason                 = $null
    }
    if (-not $status.known) {
        $status.reason = 'unknown capability'
        return [pscustomobject]$status
    }
    if (-not $Provider.Available) {
        $status.reason = $Provider.Reason
        return [pscustomobject]$status
    }
    if ($Capability -eq 'sql.read') {
        $status.deployed = $Provider.SqlReadDeployed
        $status.available = $Provider.SqlReadDeployed
        $status.reason = if ($status.available) { 'manifest: sql_read_deployed' } else { 'manifest does not report sql read deployed' }
    }
    elseif ($Capability.StartsWith('bridge.')) {
        $status.deployed = $Provider.BridgeDeployed
        $status.available = $Provider.BridgeDeployed
        $status.reason = if ($status.available) { 'manifest: bridge deployed' } else { 'manifest does not report the bridge deployed' }
    }
    elseif ($Capability.StartsWith('oracle.')) {
        $operation = $Capability.Substring('oracle.'.Length)
        if ($Provider.Operations.ContainsKey($operation)) {
            $op = $Provider.Operations[$operation]
            $status.deployed = if ($op.Contains('deployed')) { [bool]$op.deployed } else { $false }
            $status.nativeAdapterAvailable = if ($op.Contains('native_adapter_available')) { [bool]$op.native_adapter_available } else { $false }
            $status.runtimeAuthorized = if ($op.Contains('runtime_authorized')) { [bool]$op.runtime_authorized } else { $false }
            $status.receiptExportAvailable = if ($op.Contains('receipt_export_available')) { [bool]$op.receipt_export_available } else { $false }
            $status.constraints = if ($op.Contains('constraints')) { $op.constraints } else { $null }
            # Available only when the runtime is enabled AND every execution flag is
            # true. Passing unit tests or existing source never flips this.
            $status.available = $Provider.RuntimeEnabled -and $status.deployed -and $status.nativeAdapterAvailable -and $status.runtimeAuthorized
            $status.reason = if ($status.available) { 'manifest: deployed + native adapter + runtime authorized' }
            elseif (-not $Provider.RuntimeEnabled) { 'oracle runtime not enabled in deployed manifest' }
            else { 'manifest reports operation not fully deployed/authorized' }
        }
        else {
            $status.reason = "operation '$operation' absent from deployed manifest"
        }
    }
    return [pscustomobject]$status
}

# Capabilities a kind's execution authority REQUIRES to leave Validating.
# Travel/Battleground/OpenWorldPvp are Oracle-gated by Sol decision #6 and the
# V1.1 correction (exact same-map travel and exact PvP adapters are the sanctioned
# authorities; legacy bridge travel/wsg tactics are not Autopilot execution
# authority). QuestLevel is rehearsable dry-run and gated per-command when live.
$script:AutopilotKindRequiredCapabilities = @{
    'QuestLevel'      = @()
    'Gather'          = @('oracle.gather_route', 'oracle.gather_source')
    'Craft'           = @('oracle.craft')
    'TrainProfession' = @('oracle.vendor_buy')
    'VendorRepair'    = @('oracle.vendor_sell')
    'BankDeposit'     = @('oracle.bank_deposit')
    'MailTransfer'    = @('oracle.mail_send')
    'FormParty'       = @()
    'Travel'          = @('oracle.navigate')
    'Dungeon'         = @()
    'Raid'            = @()
    'Battleground'    = @('oracle.pvp_objective')
    'OpenWorldPvp'    = @('oracle.pvp_challenge', 'oracle.pvp_objective')
    'Restock'         = @('oracle.vendor_buy')
    'Idle'            = @()
}

# Default capability grants per job kind (closed allow-lists; adapter fails closed).
$script:AutopilotDefaultCapabilities = @{
    'QuestLevel'      = @('bridge.snapshot', 'bridge.questlog', 'bridge.questobjective', 'bridge.acceptance', 'bridge.quest', 'bridge.quest.acquire', 'bridge.recover', 'sql.read')
    'Gather'          = @('oracle.gather_route', 'oracle.gather_source', 'bridge.snapshot', 'sql.read')
    'Craft'           = @('oracle.craft', 'bridge.snapshot', 'sql.read')
    'TrainProfession' = @('oracle.vendor_buy', 'oracle.navigate', 'bridge.snapshot', 'sql.read')
    'VendorRepair'    = @('oracle.vendor_sell', 'oracle.vendor_buy', 'oracle.navigate', 'bridge.snapshot', 'sql.read')
    'BankDeposit'     = @('oracle.bank_deposit', 'oracle.navigate', 'bridge.snapshot', 'sql.read')
    'MailTransfer'    = @('oracle.mail_send', 'oracle.navigate', 'bridge.snapshot', 'sql.read')
    'FormParty'       = @('bridge.party', 'bridge.snapshot', 'sql.read')
    'Travel'          = @('oracle.navigate', 'bridge.travel', 'bridge.destinations', 'bridge.snapshot')
    'Dungeon'         = @('bridge.party', 'bridge.route', 'bridge.advance', 'bridge.advancepoint', 'bridge.engage', 'bridge.boss', 'bridge.combatlog', 'bridge.encounterlog', 'bridge.snapshot', 'sql.read')
    'Raid'            = @('bridge.raid.create', 'bridge.raid.status', 'bridge.raid.leave', 'bridge.rally', 'bridge.advance', 'bridge.engage', 'bridge.combatlog', 'bridge.encounterlog', 'bridge.snapshot', 'sql.read')
    'Battleground'    = @('oracle.pvp_objective', 'bridge.wsg.queue', 'bridge.wsg.status', 'bridge.wsg.leave', 'bridge.snapshot', 'sql.read')
    'OpenWorldPvp'    = @('oracle.pvp_challenge', 'oracle.pvp_objective', 'bridge.snapshot', 'sql.read')
    'Restock'         = @('oracle.vendor_buy', 'oracle.navigate', 'bridge.snapshot', 'sql.read')
    'Idle'            = @('bridge.snapshot')
}

# ---------------------------------------------------------------------------
# Paths and small utilities
# ---------------------------------------------------------------------------

function Get-AutopilotPaths {
    param(
        [string]$StateRoot = (Join-Path $PSScriptRoot 'state'),
        [string]$ProjectRoot = (Split-Path -Parent $PSScriptRoot),
        # Live receipts land under <LogsRoot>\<RunId>\ (Sol decision #8);
        # tests/scratch stay under <StateRoot>\receipts.
        [string]$LogsRoot,
        [string]$RunId,
        [string]$OwnershipPath,
        [string]$EnrollmentRequestPath
    )
    if (-not $LogsRoot) { $LogsRoot = Join-Path $ProjectRoot 'logs\autopilot' }
    if (-not $RunId) {
        $RunId = 'run-{0}-{1}' -f (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssZ'), ([guid]::NewGuid().ToString('n').Substring(0, 4))
    }
    if (-not $OwnershipPath) { $OwnershipPath = Join-Path $ProjectRoot 'work\autopilot\ownership.json' }
    if (-not $EnrollmentRequestPath) { $EnrollmentRequestPath = Join-Path $ProjectRoot 'work\autopilot\oracle-enrollment-request.json' }
    $paths = [pscustomobject]@{
        StateRoot              = $StateRoot
        JobsDir                = Join-Path $StateRoot 'jobs'
        ReceiptsDir            = Join-Path $StateRoot 'receipts'
        AllowlistPath          = Join-Path $StateRoot 'allowlist.json'
        LockPath               = Join-Path $StateRoot 'autopilot.pid.json'
        ControllerInstancePath = Join-Path $StateRoot 'controller-instance.json'
        LogsRoot               = $LogsRoot
        RunId                  = $RunId
        OwnershipPath          = $OwnershipPath
        EnrollmentRequestPath  = $EnrollmentRequestPath
        ProjectRoot            = $ProjectRoot
        ControlScript          = Join-Path $ProjectRoot 'scripts\autowow-control.ps1'
        SchemaPath             = Join-Path $ProjectRoot 'AUTOWOW_AUTOPILOT_JOB_SCHEMA_V1.json'
        FixtureRoot            = Join-Path $PSScriptRoot 'fixtures\default'
    }
    foreach ($dir in @($paths.StateRoot, $paths.JobsDir, $paths.ReceiptsDir)) {
        if (-not (Test-Path $dir)) { $null = New-Item -ItemType Directory -Force -Path $dir }
    }
    if (-not (Test-Path $paths.AllowlistPath)) {
        Write-AutopilotFileAtomic -Path $paths.AllowlistPath -Content '[]'
    }
    return $paths
}

function Get-AutopilotControllerInstanceId {
    # Stable per state root, survives restarts: the same controller identity can
    # reclaim its own leases after a crash; a different state root is a different
    # controller and must never supersede another's active lease.
    param([Parameter(Mandatory)]$Paths)
    if (Test-Path $Paths.ControllerInstancePath) {
        $doc = Get-Content -LiteralPath $Paths.ControllerInstancePath -Raw | ConvertFrom-Json
        return [string]$doc.instanceId
    }
    $instanceId = 'ap-' + [guid]::NewGuid().ToString('n').Substring(0, 12)
    Write-AutopilotFileAtomic -Path $Paths.ControllerInstancePath -Content ((
            [ordered]@{ instanceId = $instanceId; createdAt = ConvertTo-AutopilotTimestamp -Time (Get-AutopilotUtcNow) }
        ) | ConvertTo-Json)
    return $instanceId
}

function Get-AutopilotUtcNow {
    param([datetime]$Now)
    if ($PSBoundParameters.ContainsKey('Now')) { return $Now.ToUniversalTime() }
    return (Get-Date).ToUniversalTime()
}

function ConvertTo-AutopilotTimestamp {
    param([Parameter(Mandatory)][datetime]$Time)
    return $Time.ToUniversalTime().ToString('o')
}

function ConvertTo-AutopilotUtcDateTime {
    # All persisted Autopilot timestamps are UTC ISO-8601. ConvertFrom-Json may
    # hand them back as [datetime] with a non-Local kind holding the UTC face
    # value; naive [string] -> Parse -> ToUniversalTime() would then shift by
    # the machine timezone. This helper is the single safe conversion.
    param([Parameter(Mandatory)]$Value)
    if ($Value -is [datetime]) {
        switch ($Value.Kind) {
            ([System.DateTimeKind]::Utc) { return $Value }
            ([System.DateTimeKind]::Local) { return $Value.ToUniversalTime() }
            default { return [datetime]::SpecifyKind($Value, [System.DateTimeKind]::Utc) }
        }
    }
    return [System.DateTimeOffset]::Parse([string]$Value, [System.Globalization.CultureInfo]::InvariantCulture,
        [System.Globalization.DateTimeStyles]::AssumeUniversal).UtcDateTime
}

function Write-AutopilotFileAtomic {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][AllowEmptyString()][string]$Content
    )
    $dir = Split-Path -Parent $Path
    if (-not (Test-Path $dir)) { $null = New-Item -ItemType Directory -Force -Path $dir }
    $tmp = "$Path.tmp-$PID"
    [System.IO.File]::WriteAllText($tmp, $Content, [System.Text.UTF8Encoding]::new($false))
    Move-Item -Force -LiteralPath $tmp -Destination $Path
}

function ConvertTo-AutopilotHashtable {
    # Deep-converts ConvertFrom-Json output (PSCustomObject graphs) into ordered
    # hashtables so loaded job documents can be mutated uniformly.
    param([Parameter(ValueFromPipeline)]$InputObject)
    process {
        if ($null -eq $InputObject) { return $null }
        if ($InputObject -is [System.Collections.IDictionary]) {
            $out = [ordered]@{}
            foreach ($key in $InputObject.Keys) { $out[[string]$key] = ConvertTo-AutopilotHashtable $InputObject[$key] }
            return $out
        }
        if ($InputObject -is [System.Management.Automation.PSCustomObject]) {
            $out = [ordered]@{}
            foreach ($prop in $InputObject.PSObject.Properties) { $out[$prop.Name] = ConvertTo-AutopilotHashtable $prop.Value }
            return $out
        }
        if ($InputObject -is [System.Collections.IEnumerable] -and $InputObject -isnot [string]) {
            # Unary comma keeps an empty array from unrolling into $null on return.
            return , @($InputObject | ForEach-Object { ConvertTo-AutopilotHashtable $_ })
        }
        return $InputObject
    }
}

function ConvertTo-AutopilotCanonicalJson {
    # Deterministic serialization: keys sorted at every level. Used for
    # idempotency keys, so the same logical input always hashes identically.
    param($Value)
    if ($null -eq $Value) { return 'null' }
    if ($Value -is [bool]) { if ($Value) { return 'true' } else { return 'false' } }
    if ($Value -is [string]) { return ($Value | ConvertTo-Json -Compress) }
    if ($Value -is [System.Collections.IDictionary] -or $Value -is [System.Management.Automation.PSCustomObject]) {
        $table = ConvertTo-AutopilotHashtable $Value
        $parts = foreach ($key in ($table.Keys | Sort-Object)) {
            '{0}:{1}' -f ($key | ConvertTo-Json -Compress), (ConvertTo-AutopilotCanonicalJson $table[$key])
        }
        return '{' + ($parts -join ',') + '}'
    }
    if ($Value -is [System.Collections.IEnumerable] -and $Value -isnot [string]) {
        $parts = @($Value | ForEach-Object { ConvertTo-AutopilotCanonicalJson $_ })
        return '[' + ($parts -join ',') + ']'
    }
    return ($Value | ConvertTo-Json -Compress)
}

function Get-AutopilotSha256 {
    param([Parameter(Mandatory)][AllowEmptyString()][string]$Text)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = $sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($Text))
        return ([System.BitConverter]::ToString($bytes) -replace '-', '').ToLowerInvariant()
    }
    finally { $sha.Dispose() }
}

# ---------------------------------------------------------------------------
# Job creation and validation
# ---------------------------------------------------------------------------

function New-AutopilotJob {
    param(
        [Parameter(Mandatory)][string]$Kind,
        [Parameter(Mandatory)][hashtable]$Spec,
        [Parameter(Mandatory)][uint32[]]$EligibleGuids,
        [Parameter(Mandatory)][hashtable]$SuccessCriteria,
        [string]$Campaign = 'adhoc',
        [string]$RequestedBy = 'operator',
        [string]$Team,
        [ValidateRange(0, 100)][int]$Priority = 50,
        [string[]]$PermittedCapabilities,
        [array]$Prerequisites = @(),
        [hashtable]$TimeoutPolicy,
        [hashtable]$RetryPolicy,
        [ValidateSet('dry-run', 'live')][string]$ExecutionMode = 'dry-run',
        $Paths,
        # Distinct idempotency namespace for successor/retry jobs: appended to
        # the key material so a successor never collides with (or dedupes into)
        # the job it supersedes, while double-invoking the retry itself still
        # dedupes to one successor.
        [string]$IdempotencyNamespace,
        [datetime]$Now
    )
    if ($Kind -notin $script:AutopilotJobKinds) { throw "Unknown job kind '$Kind'." }
    $utc = if ($PSBoundParameters.ContainsKey('Now')) { Get-AutopilotUtcNow -Now $Now } else { Get-AutopilotUtcNow }
    $stamp = ConvertTo-AutopilotTimestamp -Time $utc

    if (-not $PermittedCapabilities) { $PermittedCapabilities = $script:AutopilotDefaultCapabilities[$Kind] }
    if (-not $TimeoutPolicy) { $TimeoutPolicy = @{ jobTimeoutMinutes = 480; phaseTimeoutMinutes = 60; noProgressSeconds = 120 } }
    if (-not $RetryPolicy) { $RetryPolicy = @{ maxAttempts = 3; maxRecoveriesPerObjective = 2; backoffSeconds = 60; onExhaust = 'block' } }

    $sortedGuids = @($EligibleGuids | Sort-Object -Unique)
    $keyMaterial = '{0}|{1}|{2}|{3}' -f $Campaign, $Kind, (ConvertTo-AutopilotCanonicalJson $Spec), ($sortedGuids -join ',')
    if ($IdempotencyNamespace) { $keyMaterial = '{0}|ns:{1}' -f $keyMaterial, $IdempotencyNamespace }
    $idempotencyKey = Get-AutopilotSha256 -Text $keyMaterial
    $jobId = 'job-{0}-{1}' -f $utc.ToString('yyyyMMddTHHmmssZ'), $idempotencyKey.Substring(0, 8)

    # Receipt placement (Sol decision #8): live jobs write under
    # <LogsRoot>\<RunId>\; dry-run/scratch jobs stay under <StateRoot>\receipts.
    # The job document stores the absolute path plus the run id.
    $receiptFile, $receiptRunId = if ($ExecutionMode -eq 'live' -and $Paths) {
        (Join-Path (Join-Path $Paths.LogsRoot $Paths.RunId) "$jobId.jsonl"), $Paths.RunId
    }
    elseif ($Paths) {
        (Join-Path $Paths.ReceiptsDir "$jobId.jsonl"), 'state'
    }
    else {
        "receipts/$jobId.jsonl", 'state'
    }

    [ordered]@{
        schemaVersion         = $script:AutopilotJobSchemaVersion
        jobId                 = $jobId
        idempotencyKey        = $idempotencyKey
        kind                  = $Kind
        owner                 = [ordered]@{ campaign = $Campaign; requestedBy = $RequestedBy; team = $Team }
        characters            = [ordered]@{ eligibleGuids = $sortedGuids; assignedGuids = @(); maxConcurrent = 1 }
        priority              = $Priority
        createdAt             = $stamp
        updatedAt             = $stamp
        startedAt             = $null
        completedAt           = $null
        notBefore             = $null
        prerequisites         = @($Prerequisites)
        successCriteria       = $SuccessCriteria
        timeoutPolicy         = $TimeoutPolicy
        retryPolicy           = $RetryPolicy
        cancellation          = [ordered]@{ requested = $false; requestedAt = $null; reason = $null; acknowledgedAt = $null }
        permittedCapabilities = @($PermittedCapabilities)
        progress              = [ordered]@{ counters = [ordered]@{}; lastProgressAt = $null; attempt = 0; recoveriesUsed = 0; evidence = @() }
        phase                 = 'Queued'
        phaseEnteredAt        = $stamp
        blockedReason         = $null
        leaseRef              = $null
        receiptLog            = [ordered]@{ file = $receiptFile; lastSeq = 0; runId = $receiptRunId }
        executionMode         = $ExecutionMode
        spec                  = $Spec
    }
}

function Test-AutopilotJob {
    # Semantic validation is authoritative; JSON-Schema validation (Test-Json)
    # runs additionally when the engine supports the schema draft.
    param(
        [Parameter(Mandatory)]$Job,
        [string]$SchemaPath
    )
    $job = ConvertTo-AutopilotHashtable $Job
    $errors = [System.Collections.Generic.List[string]]::new()

    foreach ($field in @('schemaVersion', 'jobId', 'idempotencyKey', 'kind', 'owner', 'characters', 'priority',
            'createdAt', 'updatedAt', 'prerequisites', 'successCriteria', 'timeoutPolicy', 'retryPolicy',
            'cancellation', 'permittedCapabilities', 'progress', 'phase', 'executionMode', 'spec')) {
        if (-not $job.Contains($field)) { $errors.Add("missing required field '$field'") }
    }
    if ($errors.Count -eq 0) {
        if ($job.schemaVersion -ne 1) { $errors.Add("unsupported schemaVersion '$($job.schemaVersion)' (fail closed)") }
        if ($job.kind -notin $script:AutopilotJobKinds) { $errors.Add("unknown kind '$($job.kind)'") }
        if ($job.phase -notin $script:AutopilotPhases) { $errors.Add("unknown phase '$($job.phase)'") }
        if ($job.executionMode -notin @('dry-run', 'live')) { $errors.Add("executionMode must be dry-run or live") }
        if ($job.jobId -notmatch '^job-\d{8}T\d{6}Z-[0-9a-f]{8}$') { $errors.Add("jobId '$($job.jobId)' does not match the required pattern") }
        if (-not $job.characters.eligibleGuids -or @($job.characters.eligibleGuids).Count -lt 1) {
            $errors.Add('characters.eligibleGuids must contain at least one GUID')
        }
        else {
            foreach ($guid in $job.characters.eligibleGuids) {
                if ([int64]$guid -lt 1) { $errors.Add("eligible GUID '$guid' must be positive") }
            }
        }
        if ($null -eq $job.successCriteria -or -not $job.successCriteria.Contains('counters') -or
            @($job.successCriteria.counters.Keys).Count -lt 1) {
            $errors.Add('successCriteria.counters must name at least one objective counter')
        }
        if (-not $job.permittedCapabilities -or @($job.permittedCapabilities).Count -lt 1) {
            $errors.Add('permittedCapabilities must not be empty (closed allow-list)')
        }
        else {
            foreach ($cap in $job.permittedCapabilities) {
                if ($cap -notin $script:AutopilotKnownCapabilities) { $errors.Add("unknown capability '$cap'") }
            }
        }
        if ($job.phase -eq 'Blocked' -and ($null -eq $job.blockedReason)) {
            $errors.Add('phase Blocked requires a non-null blockedReason')
        }

        switch ($job.kind) {
            'QuestLevel' {
                if ($job.spec.Contains('partyLeaderGuid') -and $null -ne $job.spec.partyLeaderGuid) {
                    if ([int64]$job.spec.partyLeaderGuid -notin @($job.characters.eligibleGuids | ForEach-Object { [int64]$_ })) {
                        $errors.Add('QuestLevel.partyLeaderGuid must be one of characters.eligibleGuids')
                    }
                }
                # The v1 planner judges QuestLevel success only by this counter;
                # requiring it here keeps an absent counter from coercing to a
                # zero target and completing instantly.
                if ($null -ne $job.successCriteria -and $job.successCriteria.Contains('counters')) {
                    if (-not $job.successCriteria.counters.Contains('questsTurnedIn') -or [int64]$job.successCriteria.counters['questsTurnedIn'] -lt 1) {
                        $errors.Add('QuestLevel successCriteria.counters.questsTurnedIn must be >= 1')
                    }
                }
            }
            'Gather' {
                foreach ($field in @('item', 'stopAtCount')) {
                    if (-not $job.spec.Contains($field)) { $errors.Add("Gather spec requires '$field'") }
                }
                if ($job.spec.Contains('item') -and (-not $job.spec.item.Contains('itemId') -or [int64]$job.spec.item.itemId -lt 1)) {
                    $errors.Add('Gather spec.item.itemId must be a positive item id')
                }
                # The v1.2 planner judges Gather success only by this counter;
                # requiring it prevents a missing counter from coercing to a
                # zero target and completing instantly.
                if ($null -ne $job.successCriteria -and $job.successCriteria.Contains('counters')) {
                    if (-not $job.successCriteria.counters.Contains('itemCount') -or [int64]$job.successCriteria.counters['itemCount'] -lt 1) {
                        $errors.Add('Gather successCriteria.counters.itemCount must be >= 1')
                    }
                }
            }
            'Travel' {
                if (-not $job.spec.Contains('destination') -or [string]::IsNullOrWhiteSpace([string]$job.spec.destination)) {
                    $errors.Add("Travel spec requires a named Playerbots 'destination' (never raw coordinates)")
                }
            }
            'Battleground' {
                if (-not $job.spec.Contains('name') -or $job.spec.name -ne 'wsg') {
                    $errors.Add("Battleground spec.name must be 'wsg' in v1")
                }
            }
            'Dungeon' {
                if (-not $job.spec.Contains('name')) { $errors.Add("Dungeon spec requires 'name'") }
                if (-not $job.spec.Contains('roles') -or @($job.spec.roles).Count -ne 5) { $errors.Add('Dungeon spec.roles must list exactly 5 roles') }
            }
            'Idle' {
                if (-not $job.spec.Contains('reason') -or [string]::IsNullOrWhiteSpace([string]$job.spec.reason)) {
                    $errors.Add('Idle spec requires an explicit reason')
                }
            }
        }
    }

    $schemaEngine = 'skipped'
    if ($SchemaPath -and (Test-Path $SchemaPath) -and $errors.Count -eq 0) {
        try {
            $json = $job | ConvertTo-Json -Depth 40
            if (Test-Json -Json $json -SchemaFile $SchemaPath -ErrorAction Stop) { $schemaEngine = 'passed' }
        }
        catch {
            # Test-Json engine limitations (draft support) are reported but only
            # real schema violations count as errors.
            if ($_.FullyQualifiedErrorId -match 'InvalidJsonAgainstSchema') {
                $errors.Add("JSON Schema violation: $($_.Exception.Message)")
                $schemaEngine = 'failed'
            }
            else { $schemaEngine = "unavailable: $($_.Exception.Message)" }
        }
    }

    [pscustomobject]@{
        Valid        = ($errors.Count -eq 0)
        Errors       = @($errors)
        SchemaEngine = $schemaEngine
    }
}

# ---------------------------------------------------------------------------
# Job store (atomic writes, idempotent add)
# ---------------------------------------------------------------------------

function Save-AutopilotJob {
    param([Parameter(Mandatory)]$Paths, [Parameter(Mandatory)]$Job)
    $job = ConvertTo-AutopilotHashtable $Job
    $file = Join-Path $Paths.JobsDir "$($job.jobId).json"
    Write-AutopilotFileAtomic -Path $file -Content ($job | ConvertTo-Json -Depth 40)
    return $job
}

function Get-AutopilotJobs {
    param([Parameter(Mandatory)]$Paths, [switch]$IncludeTerminal)
    $jobs = @()
    if (Test-Path $Paths.JobsDir) {
        foreach ($file in (Get-ChildItem -LiteralPath $Paths.JobsDir -Filter 'job-*.json' -File)) {
            $job = ConvertTo-AutopilotHashtable (Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json)
            if ($IncludeTerminal -or $job.phase -notin $script:AutopilotTerminalPhases) { $jobs += , $job }
        }
    }
    # Deterministic planner order: priority desc, createdAt asc, jobId asc.
    return @($jobs | Sort-Object -Property @{Expression = { -[int]$_.priority } }, @{Expression = { [string]$_.createdAt } }, @{Expression = { [string]$_.jobId } })
}

function Get-AutopilotJob {
    param([Parameter(Mandatory)]$Paths, [Parameter(Mandatory)][string]$JobId)
    $file = Join-Path $Paths.JobsDir "$JobId.json"
    if (-not (Test-Path $file)) { return $null }
    return ConvertTo-AutopilotHashtable (Get-Content -LiteralPath $file -Raw | ConvertFrom-Json)
}

function Add-AutopilotJob {
    # Idempotent submission: an existing non-terminal job with the same
    # idempotencyKey wins; the duplicate is refused and receipted.
    param([Parameter(Mandatory)]$Paths, [Parameter(Mandatory)]$Job)
    $job = ConvertTo-AutopilotHashtable $Job
    $validation = Test-AutopilotJob -Job $job -SchemaPath $Paths.SchemaPath
    if (-not $validation.Valid) {
        throw "Job rejected: $($validation.Errors -join '; ')"
    }
    foreach ($existing in (Get-AutopilotJobs -Paths $Paths)) {
        if ($existing.idempotencyKey -eq $job.idempotencyKey) {
            $null = Write-AutopilotReceipt -Paths $Paths -JobId $existing.jobId -Event 'job_deduplicated' -Data @{
                duplicate_job_id = $job.jobId
                idempotency_key  = $job.idempotencyKey
            }
            return $existing
        }
    }
    $saved = Save-AutopilotJob -Paths $Paths -Job $job
    $null = Write-AutopilotReceipt -Paths $Paths -JobId $saved.jobId -Event 'job_created' -Data @{
        kind            = $saved.kind
        idempotency_key = $saved.idempotencyKey
        execution_mode  = $saved.executionMode
        eligible_guids  = @($saved.characters.eligibleGuids)
        priority        = $saved.priority
    }
    return $saved
}

# ---------------------------------------------------------------------------
# Receipts (append-only JSONL)
# ---------------------------------------------------------------------------

$script:AutopilotSeqCache = @{}

$script:AutopilotReceiptPathCache = @{}

function Get-AutopilotReceiptPath {
    # Job documents carry their absolute receipt path (live jobs point under
    # logs\autopilot\<run-id>\). Fall back to the state receipts dir.
    param([Parameter(Mandatory)]$Paths, [Parameter(Mandatory)][string]$JobId)
    if ($JobId -eq 'controller') { return Join-Path $Paths.ReceiptsDir 'controller.jsonl' }
    $cacheKey = '{0}|{1}' -f $Paths.JobsDir, $JobId
    if ($script:AutopilotReceiptPathCache.ContainsKey($cacheKey)) { return $script:AutopilotReceiptPathCache[$cacheKey] }
    $default = Join-Path $Paths.ReceiptsDir "$JobId.jsonl"
    $jobFile = Join-Path $Paths.JobsDir "$JobId.json"
    $resolved = $default
    if (Test-Path $jobFile) {
        try {
            $doc = Get-Content -LiteralPath $jobFile -Raw | ConvertFrom-Json
            $file = [string]$doc.receiptLog.file
            if ($file -and [System.IO.Path]::IsPathRooted($file)) { $resolved = $file }
        }
        catch { }
    }
    $script:AutopilotReceiptPathCache[$cacheKey] = $resolved
    return $resolved
}

function Get-AutopilotReceipts {
    param([Parameter(Mandatory)]$Paths, [Parameter(Mandatory)][string]$JobId)
    $file = Get-AutopilotReceiptPath -Paths $Paths -JobId $JobId
    if (-not (Test-Path $file)) { return @() }
    return @([System.IO.File]::ReadAllLines($file) | Where-Object { $_ } | ForEach-Object { $_ | ConvertFrom-Json })
}

function Write-AutopilotReceipt {
    param(
        [Parameter(Mandatory)]$Paths,
        [Parameter(Mandatory)][string]$JobId,
        [Parameter(Mandatory)][string]$Event,
        [hashtable]$Data = @{},
        [datetime]$Now
    )
    $file = Get-AutopilotReceiptPath -Paths $Paths -JobId $JobId
    if (-not $script:AutopilotSeqCache.ContainsKey($file)) {
        $count = 0
        if (Test-Path $file) { $count = @([System.IO.File]::ReadAllLines($file) | Where-Object { $_ }).Count }
        $script:AutopilotSeqCache[$file] = $count
    }
    $seq = $script:AutopilotSeqCache[$file] + 1
    $utc = if ($PSBoundParameters.ContainsKey('Now')) { Get-AutopilotUtcNow -Now $Now } else { Get-AutopilotUtcNow }

    $record = [ordered]@{
        schema         = $script:AutopilotReceiptSchema
        schema_version = 1
        seq            = $seq
        receipt_id     = 'rcpt-{0}-{1}' -f $JobId, $seq
        timestamp_utc  = ConvertTo-AutopilotTimestamp -Time $utc
        job_id         = $JobId
        event          = $Event
    }
    foreach ($key in ($Data.Keys | Sort-Object)) { $record[$key] = $Data[$key] }

    $dir = Split-Path -Parent $file
    if ($dir -and -not (Test-Path $dir)) { $null = New-Item -ItemType Directory -Force -Path $dir }
    $line = ($record | ConvertTo-Json -Depth 30 -Compress) + [Environment]::NewLine
    [System.IO.File]::AppendAllText($file, $line, [System.Text.UTF8Encoding]::new($false))
    $script:AutopilotSeqCache[$file] = $seq
    return $record
}

# ---------------------------------------------------------------------------
# Ownership leasing (Sol decision #1): work\autopilot\ownership.json
# One gameplay mutator may own a GUID at a time. The Autopilot writes ONLY its
# own ownership file, never kills or supersedes another controller, and treats
# any foreign active lease as Blocked('roster-owned').
# ---------------------------------------------------------------------------

$script:AutopilotOwnershipSchema = 'autowow.autopilot.ownership.v1'

function Read-AutopilotOwnership {
    # Fail closed: an unreadable or unsupported ownership file means NO lease can
    # be acquired (returns usable=$false), never "assume unowned".
    param([Parameter(Mandatory)]$Paths)
    $empty = [pscustomobject]@{ usable = $true; reason = 'no ownership file yet'; leases = @() }
    if (-not (Test-Path $Paths.OwnershipPath)) { return $empty }
    try {
        $doc = ConvertTo-AutopilotHashtable (Get-Content -LiteralPath $Paths.OwnershipPath -Raw | ConvertFrom-Json)
    }
    catch {
        return [pscustomobject]@{ usable = $false; reason = "ownership file unreadable (fail closed): $($_.Exception.Message)"; leases = @() }
    }
    if (-not ($doc -is [System.Collections.IDictionary]) -or [string]$doc.schema -ne $script:AutopilotOwnershipSchema -or [int64]$doc.schema_version -ne 1) {
        return [pscustomobject]@{ usable = $false; reason = "ownership schema not supported (fail closed): expected $($script:AutopilotOwnershipSchema) v1"; leases = @() }
    }
    return [pscustomobject]@{ usable = $true; reason = 'loaded'; leases = @($doc.leases) }
}

function Save-AutopilotOwnership {
    param([Parameter(Mandatory)]$Paths, [Parameter(Mandatory)][AllowEmptyCollection()][array]$Leases)
    $doc = [ordered]@{
        schema         = $script:AutopilotOwnershipSchema
        schema_version = 1
        updated_utc    = ConvertTo-AutopilotTimestamp -Time (Get-AutopilotUtcNow)
        leases         = @($Leases)
    }
    Write-AutopilotFileAtomic -Path $Paths.OwnershipPath -Content ($doc | ConvertTo-Json -Depth 20)
}

function Test-AutopilotLeaseActive {
    param([Parameter(Mandatory)]$Lease, [Parameter(Mandatory)][datetime]$Now)
    if ([string]$Lease.state -ne 'active') { return $false }
    try { return ((ConvertTo-AutopilotUtcDateTime $Lease.expiresAt) -gt $Now.ToUniversalTime()) }
    catch { return $false }   # unparseable expiry = not provably active = treat as expired for OUR leases, but see conflict logic
}

function Request-AutopilotOwnership {
    <#
    Atomic acquire/renew for every requested GUID. Results:
      Acquired  - all GUIDs now held active by (instance, job).
      Conflicts - foreign active (unexpired) leases; caller must Block('roster-owned').
    Expired/released leases are reclaimable; a foreign lease is NEVER superseded
    while unexpired, even if its PID looks dead (we cannot verify another
    controller's process model - only time expires it).
    #>
    param(
        [Parameter(Mandatory)]$Paths,
        [Parameter(Mandatory)][string]$JobId,
        [Parameter(Mandatory)][int64[]]$Guids,
        [int]$TtlSeconds = 900,
        [datetime]$Now
    )
    $utc = if ($PSBoundParameters.ContainsKey('Now')) { Get-AutopilotUtcNow -Now $Now } else { Get-AutopilotUtcNow }
    $instanceId = Get-AutopilotControllerInstanceId -Paths $Paths
    $ownership = Read-AutopilotOwnership -Paths $Paths
    if (-not $ownership.usable) {
        return [pscustomobject]@{ Acquired = $false; Reason = $ownership.reason; Conflicts = @(); FailClosed = $true }
    }
    $stamp = ConvertTo-AutopilotTimestamp -Time $utc
    $expires = ConvertTo-AutopilotTimestamp -Time $utc.AddSeconds($TtlSeconds)
    $leases = @($ownership.leases)
    $conflicts = @()

    foreach ($guid in $Guids) {
        $existing = @($leases | Where-Object { [int64]$_.characterGuid -eq [int64]$guid -and [string]$_.state -eq 'active' })
        $blocking = @($existing | Where-Object {
                $active = $true
                try { $active = ((ConvertTo-AutopilotUtcDateTime $_.expiresAt) -gt $utc) } catch { $active = $true }  # unparseable foreign expiry: fail closed, treat as active
                $foreign = ([string]$_.controllerInstanceId -ne $instanceId -or [string]$_.jobId -ne $JobId)
                $active -and $foreign
            })
        if ($blocking.Count -gt 0) {
            $owner = $blocking[0]
            $conflicts += [pscustomobject]@{
                guid          = [int64]$guid
                ownerInstance = [string]$owner.controllerInstanceId
                ownerPid      = [int64]$owner.pid
                ownerJob      = [string]$owner.jobId
                expiresAt     = [string]$owner.expiresAt
            }
        }
    }

    if ($conflicts.Count -gt 0) {
        foreach ($conflict in $conflicts) {
            $null = Write-AutopilotReceipt -Paths $Paths -JobId $JobId -Event 'ownership_conflict' -Data @{
                guid           = $conflict.guid
                owner_instance = $conflict.ownerInstance
                owner_pid      = $conflict.ownerPid
                owner_job      = $conflict.ownerJob
                owner_expires  = $conflict.expiresAt
            }
        }
        return [pscustomobject]@{ Acquired = $false; Reason = 'foreign active lease'; Conflicts = $conflicts; FailClosed = $false }
    }

    # No blocking lease: drop our stale entries for these GUIDs, mark foreign
    # expired entries as expired (bookkeeping only - it is time that expired
    # them, not us), and append fresh active leases.
    $kept = @($leases | ForEach-Object {
            $lease = $_
            if ([int64]$lease.characterGuid -in @($Guids | ForEach-Object { [int64]$_ })) {
                $mine = ([string]$lease.controllerInstanceId -eq $instanceId)
                if ($mine) { return }   # replaced below
                if ([string]$lease.state -eq 'active' -and -not (Test-AutopilotLeaseActive -Lease $lease -Now $utc)) {
                    $lease.state = 'expired'
                }
            }
            $lease
        })
    $renewedExisting = @($ownership.leases | Where-Object {
            [string]$_.controllerInstanceId -eq $instanceId -and [string]$_.jobId -eq $JobId -and
            [int64]$_.characterGuid -in @($Guids | ForEach-Object { [int64]$_ }) -and [string]$_.state -eq 'active'
        }).Count -gt 0
    $new = foreach ($guid in $Guids) {
        [ordered]@{
            schema               = $script:AutopilotOwnershipSchema
            controllerInstanceId = $instanceId
            pid                  = $PID
            jobId                = $JobId
            characterGuid        = [int64]$guid
            acquiredAt           = $stamp
            renewedAt            = $stamp
            expiresAt            = $expires
            state                = 'active'
        }
    }
    Save-AutopilotOwnership -Paths $Paths -Leases (@($kept) + @($new))
    $null = Write-AutopilotReceipt -Paths $Paths -JobId $JobId -Event $(if ($renewedExisting) { 'ownership_renewed' } else { 'ownership_acquired' }) -Data @{
        guids      = @($Guids)
        instance   = $instanceId
        expires_at = $expires
        ttl_s      = $TtlSeconds
    }
    return [pscustomobject]@{ Acquired = $true; Reason = $(if ($renewedExisting) { 'renewed' } else { 'acquired' }); Conflicts = @(); FailClosed = $false }
}

function Release-AutopilotOwnership {
    param([Parameter(Mandatory)]$Paths, [Parameter(Mandatory)][string]$JobId, [string]$Reason = 'released')
    $ownership = Read-AutopilotOwnership -Paths $Paths
    if (-not $ownership.usable) { return $false }
    $instanceId = Get-AutopilotControllerInstanceId -Paths $Paths
    $changed = $false
    $released = @()
    $leases = @($ownership.leases | ForEach-Object {
            if ([string]$_.controllerInstanceId -eq $instanceId -and [string]$_.jobId -eq $JobId -and [string]$_.state -eq 'active') {
                $_.state = 'released'
                $released += [int64]$_.characterGuid
                $changed = $true
            }
            $_
        })
    if ($changed) {
        Save-AutopilotOwnership -Paths $Paths -Leases $leases
        $null = Write-AutopilotReceipt -Paths $Paths -JobId $JobId -Event 'ownership_released' -Data @{ guids = @($released); reason = $Reason }
    }
    return $changed
}

function Test-AutopilotOwnershipActive {
    param(
        [Parameter(Mandatory)]$Paths,
        [Parameter(Mandatory)][string]$JobId,
        [Parameter(Mandatory)][int64]$Guid,
        [datetime]$Now
    )
    $utc = if ($PSBoundParameters.ContainsKey('Now')) { Get-AutopilotUtcNow -Now $Now } else { Get-AutopilotUtcNow }
    $ownership = Read-AutopilotOwnership -Paths $Paths
    if (-not $ownership.usable) { return $false }
    $instanceId = Get-AutopilotControllerInstanceId -Paths $Paths
    foreach ($lease in $ownership.leases) {
        if ([int64]$lease.characterGuid -eq $Guid -and [string]$lease.controllerInstanceId -eq $instanceId -and
            [string]$lease.jobId -eq $JobId -and (Test-AutopilotLeaseActive -Lease $lease -Now $utc)) {
            return $true
        }
    }
    return $false
}

# ---------------------------------------------------------------------------
# Oracle enrollment proposal (Sol decision #3): a receipted request file only.
# Never edits playerbots.conf, never restarts worldserver, never enrolls a bot.
# ---------------------------------------------------------------------------

$script:AutopilotEnrollmentSchema = 'autowow.autopilot.oracle-enrollment-request.v1'

function New-AutopilotEnrollmentProposal {
    param(
        [Parameter(Mandatory)]$Paths,
        [Parameter(Mandatory)][string]$JobId,
        [int]$TtlHours = 24,
        [string]$Reason,
        [datetime]$Now
    )
    $utc = if ($PSBoundParameters.ContainsKey('Now')) { Get-AutopilotUtcNow -Now $Now } else { Get-AutopilotUtcNow }
    $job = Get-AutopilotJob -Paths $Paths -JobId $JobId
    if (-not $job) { throw "No job $JobId." }
    $required = @($script:AutopilotKindRequiredCapabilities[$job.kind])
    $oracleCaps = @(@($job.permittedCapabilities) + $required | Where-Object { $_ -like 'oracle.*' } | Sort-Object -Unique)
    if (-not $Reason) {
        $Reason = "job $JobId ($($job.kind)) requires oracle capabilities for campaign '$($job.owner.campaign)'"
    }
    $request = [ordered]@{
        jobId                = $JobId
        guids                = @(@($job.characters.eligibleGuids) | ForEach-Object { [int64]$_ })
        requiredCapabilities = $oracleCaps
        expiresAt            = ConvertTo-AutopilotTimestamp -Time $utc.AddHours($TtlHours)
        reason               = $Reason
        requestedAt          = ConvertTo-AutopilotTimestamp -Time $utc
    }

    $existingRequests = @()
    if (Test-Path $Paths.EnrollmentRequestPath) {
        try {
            $doc = ConvertTo-AutopilotHashtable (Get-Content -LiteralPath $Paths.EnrollmentRequestPath -Raw | ConvertFrom-Json)
            if ([string]$doc.schema -eq $script:AutopilotEnrollmentSchema) { $existingRequests = @($doc.requests) }
        }
        catch { $existingRequests = @() }
    }
    $requests = @($existingRequests | Where-Object { [string]$_.jobId -ne $JobId }) + @($request)   # upsert by jobId
    $document = [ordered]@{
        schema               = $script:AutopilotEnrollmentSchema
        schema_version       = 1
        generatedAt          = ConvertTo-AutopilotTimestamp -Time $utc
        controllerInstanceId = Get-AutopilotControllerInstanceId -Paths $Paths
        note                 = 'Proposal only. Sol owns deployed Oracle configuration and GUID allowlists; the Autopilot never edits playerbots.conf, never restarts worldserver, never silently enrolls a bot.'
        requests             = @($requests)
    }
    Write-AutopilotFileAtomic -Path $Paths.EnrollmentRequestPath -Content ($document | ConvertTo-Json -Depth 20)
    $null = Write-AutopilotReceipt -Paths $Paths -JobId $JobId -Event 'enrollment_proposed' -Data @{
        path                  = $Paths.EnrollmentRequestPath
        guids                 = $request.guids
        required_capabilities = $oracleCaps
        expires_at            = $request.expiresAt
    }
    return [pscustomobject]$document
}

# ---------------------------------------------------------------------------
# Oracle receipt provider (Sol decision #2): pluggable, read-only.
# The in-memory ring is NEVER scraped; without a deployed export, evidence is
# reported unavailable rather than fabricated.
# ---------------------------------------------------------------------------

$script:AutopilotOracleReceiptSchema = 'autowow.oracle.receipt.v1'

function New-AutopilotOracleReceiptProvider {
    param(
        [Parameter(Mandatory)][ValidateSet('unavailable', 'jsonl', 'bridge')][string]$Kind,
        [string]$Path
    )
    $provider = [ordered]@{
        Kind      = $Kind
        Available = $false
        Reason    = 'oracle receipt export is not deployed; evidence unavailable (never scraped from the in-memory ring)'
        Source    = $Path
        Receipts  = @()
    }
    switch ($Kind) {
        'unavailable' { }
        'bridge' { $provider.Reason = 'bridge receipt endpoint not deployed yet (future oraclelog surface); fail closed' }
        'jsonl' {
            if (-not $Path) { throw 'jsonl provider requires -Path.' }
            if (-not (Test-Path $Path)) { $provider.Reason = "oracle receipt file not found: $Path"; break }
            $parsed = @()
            $bad = 0
            foreach ($line in ([System.IO.File]::ReadAllLines($Path) | Where-Object { $_ })) {
                try {
                    $record = ConvertTo-AutopilotHashtable ($line | ConvertFrom-Json)
                    if ([string]$record.schema -eq $script:AutopilotOracleReceiptSchema) { $parsed += , $record } else { $bad++ }
                }
                catch { $bad++ }
            }
            $provider.Receipts = $parsed
            $provider.Available = $true
            $provider.Reason = "loaded $($parsed.Count) receipts" + $(if ($bad) { " ($bad unrecognized lines skipped)" } else { '' })
        }
    }
    return [pscustomobject]$provider
}

function Join-AutopilotOracleEvidence {
    <#
    Correlates Autopilot command receipts with Oracle receipts using Sol's keys:
    decisionId, jobId (when the Oracle supplies it), botGuid, operation, lease
    resource/epoch, target hash, before/after fact versions, native step count,
    status/reason, timestamp. Correlation is evidence linking only - it never
    upgrades a command into success by itself.
    #>
    param(
        [Parameter(Mandatory)]$Paths,
        [Parameter(Mandatory)][string]$JobId,
        [Parameter(Mandatory)]$Provider,
        [int]$WindowSeconds = 300
    )
    if (-not $Provider.Available) {
        return [pscustomobject]@{ available = $false; reason = $Provider.Reason; correlations = @() }
    }
    $job = Get-AutopilotJob -Paths $Paths -JobId $JobId
    $botGuids = if ($job) { @(@($job.characters.assignedGuids) + @($job.characters.eligibleGuids) | ForEach-Object { [int64]$_ } | Sort-Object -Unique) } else { @() }
    $commands = @(Get-AutopilotReceipts -Paths $Paths -JobId $JobId | Where-Object { $_.event -eq 'command' })
    $correlations = @()
    foreach ($command in $commands) {
        $commandTime = ConvertTo-AutopilotUtcDateTime $command.timestamp_utc
        $matches_ = @($Provider.Receipts | Where-Object {
                $oracle = $_
                if ($oracle.Contains('job_id') -and [string]$oracle.job_id) {
                    if ([string]$oracle.job_id -ne $JobId) { return $false }
                }
                if (-not $oracle.Contains('bot_guid') -or [int64]$oracle.bot_guid -notin $botGuids) { return $false }
                try {
                    $oracleTime = ConvertTo-AutopilotUtcDateTime $oracle.timestamp_utc
                    if ([math]::Abs(($oracleTime - $commandTime).TotalSeconds) -gt $WindowSeconds) { return $false }
                }
                catch { return $false }
                $true
            })
        $correlations += [pscustomobject]@{
            command_receipt_id = [string]$command.receipt_id
            command_action     = [string]$command.command.action
            command_time       = [string]$command.timestamp_utc
            oracle_matches     = @($matches_ | ForEach-Object {
                    [pscustomobject]@{
                        decision_id         = $_.decision_id
                        job_id              = $(if ($_.Contains('job_id')) { $_.job_id } else { $null })
                        bot_guid            = $_.bot_guid
                        operation           = $_.operation
                        resource            = $_.resource
                        epoch               = $_.epoch
                        target_hash         = $_.target_hash
                        fact_version_before = $_.fact_version_before
                        fact_version_after  = $_.fact_version_after
                        native_steps        = $_.native_steps
                        status              = $_.status
                        reason              = $_.reason
                        timestamp_utc       = $_.timestamp_utc
                        matched_by          = @('botGuid', 'timestamp') + @($(if ($_.Contains('job_id') -and [string]$_.job_id) { 'jobId' }))
                    }
                })
        }
    }
    return [pscustomobject]@{ available = $true; reason = $Provider.Reason; correlations = @($correlations) }
}

# ---------------------------------------------------------------------------
# State machine
# ---------------------------------------------------------------------------

function Invoke-AutopilotTransition {
    param(
        [Parameter(Mandatory)]$Paths,
        [Parameter(Mandatory)]$Job,
        [Parameter(Mandatory)][string]$To,
        [string]$Reason = '',
        [hashtable]$Blocked,
        [datetime]$Now
    )
    $job = ConvertTo-AutopilotHashtable $Job
    $from = [string]$job.phase
    if ($To -notin $script:AutopilotPhases) { throw "Unknown target phase '$To'." }

    $legal = $script:AutopilotLegalTransitions[$from]
    $suspendResume = $false
    if ($from -eq 'Suspended') {
        $resumeTo = if ($job.Contains('metadata') -and $job.metadata -and $job.metadata.Contains('suspendedFromPhase')) { [string]$job.metadata.suspendedFromPhase } else { $null }
        if ($To -eq $resumeTo) { $suspendResume = $true }
    }
    if (-not $suspendResume -and $To -notin $legal) {
        throw "Illegal transition $from -> $To for job $($job.jobId)."
    }
    if ($To -eq 'Blocked') {
        if (-not $Blocked -or -not $Blocked.ContainsKey('code')) { throw 'Blocked transitions require a typed blockedReason code.' }
        if ($Blocked.code -notin $script:AutopilotBlockedCodes) { throw "Unknown blocked code '$($Blocked.code)'." }
        if (-not $Blocked.ContainsKey('detail') -or [string]::IsNullOrWhiteSpace([string]$Blocked.detail)) { throw 'Blocked transitions require a non-empty detail.' }
    }

    $utc = if ($PSBoundParameters.ContainsKey('Now')) { Get-AutopilotUtcNow -Now $Now } else { Get-AutopilotUtcNow }
    $stamp = ConvertTo-AutopilotTimestamp -Time $utc

    if ($To -eq 'Suspended') {
        if (-not $job.Contains('metadata') -or $null -eq $job.metadata) { $job.metadata = [ordered]@{} }
        $job.metadata.suspendedFromPhase = $from
    }

    $job.phase = $To
    $job.phaseEnteredAt = $stamp
    $job.updatedAt = $stamp
    if ($from -eq 'Queued' -and $To -eq 'Validating' -and -not $job.startedAt) { $job.startedAt = $stamp }
    if ($To -in $script:AutopilotTerminalPhases) { $job.completedAt = $stamp }
    if ($To -eq 'Blocked') {
        $job.blockedReason = [ordered]@{
            code       = $Blocked.code
            detail     = $Blocked.detail
            since      = $stamp
            nextAction = if ($Blocked.ContainsKey('nextAction')) { $Blocked.nextAction } else { $null }
        }
    }
    elseif ($To -notin @('Suspended')) {
        $job.blockedReason = $null
    }

    $receiptData = @{ from = $from; to = $To; reason = $Reason }
    if ($To -eq 'Blocked') { $receiptData['blocked'] = @{ code = $Blocked.code; detail = $Blocked.detail } }
    # Receipts are the durable ledger; the job document is a snapshot. Append
    # the receipt first so reconciliation can always roll the snapshot forward.
    $receipt = Write-AutopilotReceipt -Paths $Paths -JobId $job.jobId -Event 'job_transition' -Data $receiptData -Now:$utc
    $job.receiptLog.lastSeq = $receipt.seq
    return (Save-AutopilotJob -Paths $Paths -Job $job)
}

# ---------------------------------------------------------------------------
# Bridge adapter (wire strings single-sourced from autowow-control.ps1)
# ---------------------------------------------------------------------------

function Get-AutopilotWireRequest {
    param(
        [Parameter(Mandatory)]$Paths,
        [Parameter(Mandatory)][string]$Action,
        [hashtable]$ActionParams = @{}
    )
    if (-not (Test-Path $Paths.ControlScript)) { throw "autowow-control.ps1 not found at $($Paths.ControlScript)" }
    $wire = & $Paths.ControlScript -Action $Action @ActionParams -EmitRequestOnly
    if ([string]::IsNullOrWhiteSpace($wire)) { throw "Empty wire request for action '$Action'." }
    return [string]$wire
}

$script:AutopilotCapabilityByAction = @{
    'list' = 'bridge.list'; 'snapshot' = 'bridge.snapshot'; 'destinations' = 'bridge.destinations'
    'questlog' = 'bridge.questlog'; 'questobjective' = 'bridge.questobjective'; 'acceptance' = 'bridge.acceptance'
    'combatlog' = 'bridge.combatlog'; 'encounterlog' = 'bridge.encounterlog'
    'quest' = 'bridge.quest'; 'quest-acquire' = 'bridge.quest.acquire'; 'travel' = 'bridge.travel'
    'recover' = 'bridge.recover'; 'pause' = 'bridge.pause'; 'resume' = 'bridge.resume'
    'party' = 'bridge.party'; 'rally' = 'bridge.rally'; 'deploy' = 'bridge.deploy'
    'route' = 'bridge.route'; 'advance' = 'bridge.advance'; 'advance-point' = 'bridge.advancepoint'
    'engage' = 'bridge.engage'; 'scout' = 'bridge.scout'; 'pathprobe' = 'bridge.pathprobe'
    'boss' = 'bridge.boss'; 'boss-status' = 'bridge.boss'; 'activate' = 'bridge.activate'
    'deactivate' = 'bridge.deactivate'
    'wsg-queue' = 'bridge.wsg.queue'; 'wsg-status' = 'bridge.wsg.status'; 'wsg-leave' = 'bridge.wsg.leave'
    'raid-create' = 'bridge.raid.create'; 'raid-status' = 'bridge.raid.status'; 'raid-leave' = 'bridge.raid.leave'
    'fixture-init' = 'bridge.fixture.init'; 'fixture-status' = 'bridge.fixture.status'; 'probe-reset' = 'bridge.probe.reset'
}

$script:AutopilotIrreversibleActions = @(
    'quest', 'quest-acquire', 'party', 'raid-create', 'wsg-queue', 'deploy', 'activate', 'engage'
)

# Party-contract refusals from the bridge (Sol lane, 2026-07-18). These are
# PLANNING preconditions, never movement stalls: they consume zero recovery
# budget, the same impossible command is never re-issued, and the controller
# never works around them (no fabricated parties, no arbitrary invites, no
# quest sharing/granting, no faction changes).
$script:AutopilotPartyContractErrors = @{
    'quest_requires_party_leader'              = 'explicit quest orders require the target character to be an actual party leader'
    'party_member_exact_quest_preflight_failed' = 'the exact-quest party preflight rejected the roster; every party member must satisfy the quest contract'
    'quest_not_in_member_log'                  = 'a party companion does not hold the quest; companions must obtain it normally (sharing/granting is not permitted)'
    'quest_not_in_leader_log'                  = 'the leader does not hold the quest; acquisition must run first'
    'cross_faction_party_not_allowed'          = 'party members must all share one faction'
    'quest_not_party_compatible'               = 'the requested quest is incompatible with this party (size/class/group constraints or quest lock flags)'
}

function Get-AutopilotSentCommandKeys {
    # Keys of irreversible commands already issued (sent live, or recorded as
    # would_send in dry-run) that have not been explicitly expired since.
    param([Parameter(Mandatory)]$Paths, [Parameter(Mandatory)][string]$JobId)
    $keys = @{}
    foreach ($receipt in (Get-AutopilotReceipts -Paths $Paths -JobId $JobId)) {
        if ($receipt.event -eq 'command' -and $receipt.PSObject.Properties['command']) {
            $cmd = $receipt.command
            if ($cmd.irreversible -and ($cmd.sent -or $cmd.would_send)) { $keys[$cmd.idempotency_key] = $true }
        }
        elseif ($receipt.event -in @('order_expired', 'retry_authorized') -and $receipt.PSObject.Properties['idempotency_key']) {
            $keys.Remove([string]$receipt.idempotency_key)
        }
    }
    return $keys
}

function Invoke-AutopilotCommand {
    param(
        [Parameter(Mandatory)]$Paths,
        [Parameter(Mandatory)]$Job,
        [Parameter(Mandatory)][string]$Action,
        [hashtable]$ActionParams = @{},
        [string]$FixturePath,
        [switch]$ConfirmLiveBridge,
        $CapabilityProvider,
        [datetime]$Now
    )
    $job = ConvertTo-AutopilotHashtable $Job
    $capability = $script:AutopilotCapabilityByAction[$Action]
    if (-not $capability) { throw "Action '$Action' has no capability mapping." }
    if (-not $CapabilityProvider) { $CapabilityProvider = New-AutopilotCapabilityProvider -Kind unavailable }

    # Closed allow-list, fail closed on every layer.
    if ($capability -notin @($job.permittedCapabilities)) {
        throw "Capability '$capability' is not in job $($job.jobId) permittedCapabilities."
    }
    $status = Get-AutopilotCapabilityStatus -Capability $capability -Provider $CapabilityProvider
    if ($status.labOnly) {
        throw "Capability '$capability' is lab-only and may not be used by campaign jobs."
    }
    # Dry-run rehearses without claiming server capability (the receipt records
    # the capability's evidence state at issue time). Live requires evidence.

    $wire = Get-AutopilotWireRequest -Paths $Paths -Action $Action -ActionParams $ActionParams
    $irreversible = $Action -in $script:AutopilotIrreversibleActions
    $idempotencyKey = Get-AutopilotSha256 -Text ('{0}|{1}|{2}|{3}' -f $job.jobId, $job.phase, $job.progress.attempt, $wire)

    if ($irreversible) {
        $sentKeys = Get-AutopilotSentCommandKeys -Paths $Paths -JobId $job.jobId
        if ($sentKeys.ContainsKey($idempotencyKey)) {
            $null = Write-AutopilotReceipt -Paths $Paths -JobId $job.jobId -Event 'command_deduplicated' -Data @{
                idempotency_key = $idempotencyKey
                wire            = $wire
            }
            return [pscustomobject]@{ Deduplicated = $true; Sent = $false; Wire = $wire; IdempotencyKey = $idempotencyKey; Response = $null }
        }
    }

    $mode = [string]$job.executionMode
    $response = $null
    $sent = $false

    if ($mode -eq 'live') {
        # Live quintuple gate (V1.1): (1) job executionMode=live, (2) explicit
        # -ConfirmLiveBridge, (3) every assigned character on the managed
        # allow-list, (4) an ACTIVE ownership lease for every assigned character
        # held by this controller+job, (5) deployed capability evidence from the
        # capability provider. Any missing layer refuses before a socket opens.
        if (-not $ConfirmLiveBridge) { throw 'Live mode requires -ConfirmLiveBridge (double gate).' }
        $allowlist = @()
        if (Test-Path $Paths.AllowlistPath) { $allowlist = @((Get-Content -LiteralPath $Paths.AllowlistPath -Raw | ConvertFrom-Json)) }
        $uncovered = @($job.characters.assignedGuids | Where-Object { [int64]$_ -notin @($allowlist | ForEach-Object { [int64]$_ }) })
        if (@($job.characters.assignedGuids).Count -eq 0 -or $uncovered.Count -gt 0) {
            throw "Live mode refused: assigned GUIDs [$($uncovered -join ',')] are not enrolled in $($Paths.AllowlistPath)."
        }
        foreach ($guid in @($job.characters.assignedGuids)) {
            if (-not (Test-AutopilotOwnershipActive -Paths $Paths -JobId $job.jobId -Guid ([int64]$guid))) {
                throw "Live mode refused: no active ownership lease for GUID $guid (job $($job.jobId)); Blocked('roster-owned') applies."
            }
        }
        if (-not $status.available) {
            throw "Live mode refused: capability '$capability' has no deployed-runtime evidence ($($status.reason)); never emulated."
        }
        $manifestEligibility = Test-AutopilotCapabilityProviderLiveEligible -Provider $CapabilityProvider
        if (-not $manifestEligibility.Eligible) {
            throw "Live mode refused: capability manifest is not live-eligible ($($manifestEligibility.Reason))."
        }
        $questLogSource = Join-Path $Paths.ProjectRoot 'scripts\QuestLogSource.ps1'
        if (-not (Get-Command -Name 'Send-Bridge' -ErrorAction SilentlyContinue)) { . $questLogSource }
        $raw = Send-Bridge -Request $wire
        $sent = $true
        try { $response = $raw | ConvertFrom-Json } catch { $response = @{ raw = [string]$raw } }
    }
    elseif ($FixturePath) {
        if (-not (Test-Path $FixturePath)) { throw "Fixture not found: $FixturePath" }
        $response = Get-Content -LiteralPath $FixturePath -Raw | ConvertFrom-Json
    }

    $commandData = @{
        command = @{
            surface              = 'bridge'
            capability           = $capability
            capability_available = $status.available
            capability_source    = $status.source
            action               = $Action
            wire                 = $wire
            mode                 = $mode
            sent                 = $sent
            would_send           = (-not $sent)
            irreversible         = $irreversible
            idempotency_key      = $idempotencyKey
        }
    }
    if ($null -ne $response) { $commandData['response'] = (ConvertTo-AutopilotHashtable $response) }
    $writeParams = @{ Paths = $Paths; JobId = $job.jobId; Event = 'command'; Data = $commandData }
    if ($PSBoundParameters.ContainsKey('Now')) { $writeParams['Now'] = $Now }
    $null = Write-AutopilotReceipt @writeParams

    return [pscustomobject]@{ Deduplicated = $false; Sent = $sent; Wire = $wire; IdempotencyKey = $idempotencyKey; Response = $response }
}

# ---------------------------------------------------------------------------
# Character capability profile (observed vs inferred; fixture-first in v1)
# ---------------------------------------------------------------------------

$script:AutopilotRoleByClass = @{
    # WotLK class ids -> possible roles (inference table only).
    1  = @('tank', 'dps')          # warrior
    2  = @('tank', 'healer', 'dps') # paladin
    3  = @('dps')                  # hunter
    4  = @('dps')                  # rogue
    5  = @('healer', 'dps')        # priest
    6  = @('tank', 'dps')          # death knight
    7  = @('healer', 'dps')        # shaman
    8  = @('dps')                  # mage
    9  = @('dps')                  # warlock
    11 = @('tank', 'healer', 'dps') # druid
}

function Get-AutopilotCharacterProfile {
    param(
        [Parameter(Mandatory)]$Paths,
        [Parameter(Mandatory)][uint32]$Guid,
        [string]$FixtureRoot
    )
    if (-not $FixtureRoot) { $FixtureRoot = $Paths.FixtureRoot }
    $snapshotPath = Join-Path $FixtureRoot "snapshot-$Guid.json"
    $observed = [ordered]@{ bridge = $null; source = 'fixture'; collectedAt = ConvertTo-AutopilotTimestamp -Time (Get-AutopilotUtcNow) }
    if (Test-Path $snapshotPath) {
        $snapshot = Get-Content -LiteralPath $snapshotPath -Raw | ConvertFrom-Json
        $observed.bridge = ConvertTo-AutopilotHashtable $snapshot
    }
    $inferred = [ordered]@{ roles = @(); notes = @() }
    if ($observed.bridge -and $observed.bridge.Contains('bot')) {
        $bot = $observed.bridge.bot
        if ($bot.Contains('class')) {
            $classId = [int]$bot.class
            if ($script:AutopilotRoleByClass.ContainsKey($classId)) {
                $inferred.roles = $script:AutopilotRoleByClass[$classId]
                $inferred.notes += "roles derived from observed.bridge.bot.class=$classId"
            }
        }
    }
    else {
        $inferred.notes += 'no snapshot available; profile is identity-only'
    }
    [ordered]@{
        guid     = $Guid
        observed = $observed
        inferred = $inferred
    }
}

# ---------------------------------------------------------------------------
# Planner (one deterministic tick; v1 slice implements QuestLevel + Gather)
# ---------------------------------------------------------------------------

function Get-AutopilotQuestObjectiveFacts {
    # Defensive parse of a questobjective response. Understands BOTH the real
    # bridge wire shape (QuestLogView: single `objective` object with
    # current_count/required_count/phase/supported/has_lock; no reward field -
    # turn-in proof belongs to the `acceptance` verb) and the flat fixture shape
    # used by dry-run rehearsals.
    param($Response)
    $facts = [ordered]@{ questId = $null; anyObjective = $false; current = $null; required = $null; complete = $false; rewarded = $false; supported = $null; phase = $null; hasLock = $null }
    if ($null -eq $Response) { return $facts }
    $r = ConvertTo-AutopilotHashtable $Response
    if ($r.Contains('objective') -and $r.objective -is [System.Collections.IDictionary]) {
        # Live bridge shape.
        $objective = $r.objective
        if ($objective.Contains('quest_id') -and [int64]$objective.quest_id -gt 0) { $facts.questId = [int64]$objective.quest_id }
        if ($objective.Contains('current_count')) { $facts.current = [int64]$objective.current_count }
        if ($objective.Contains('required_count')) { $facts.required = [int64]$objective.required_count }
        if ($objective.Contains('supported')) { $facts.supported = [bool]$objective.supported }
        if ($objective.Contains('phase')) { $facts.phase = [string]$objective.phase }
        if ($objective.Contains('has_lock')) { $facts.hasLock = [bool]$objective.has_lock }
        $facts.anyObjective = (($null -ne $facts.questId) -or ($null -ne $facts.required -and [int64]$facts.required -gt 0))
        $facts.complete = ($null -ne $facts.required -and [int64]$facts.required -gt 0 -and [int64]$facts.current -ge [int64]$facts.required)
        return $facts
    }
    if ($r.Contains('quest_id')) { $facts.questId = [int64]$r.quest_id }
    if ($r.Contains('rewarded')) { $facts.rewarded = [bool]$r.rewarded }
    if ($r.Contains('objectives')) {
        foreach ($objective in @($r.objectives)) {
            $facts.anyObjective = $true
            $facts.current = [int64]$objective.current
            $facts.required = [int64]$objective.required
            $facts.complete = ($facts.current -ge $facts.required)
            break
        }
    }
    if ($r.Contains('quest_status') -and [string]$r.quest_status -eq 'rewarded') { $facts.rewarded = $true }
    return $facts
}

function Get-AutopilotMissingRequiredCapabilities {
    # Returns [{capability, reason}] for every capability the kind's execution
    # authority requires that the provider cannot evidence as available.
    param([Parameter(Mandatory)][string]$Kind, [Parameter(Mandatory)]$Provider)
    $missing = @()
    foreach ($capability in @($script:AutopilotKindRequiredCapabilities[$Kind])) {
        $status = Get-AutopilotCapabilityStatus -Capability $capability -Provider $Provider
        if (-not $status.available) {
            $missing += [pscustomobject]@{ capability = $capability; reason = $status.reason }
        }
    }
    return , $missing
}

function Invoke-AutopilotJobStep {
    # Advances ONE job by at most one transition. Deterministic; kind-dispatched
    # (QuestLevel via the bridge quest cycle, Gather via dry-run oracle intents).
    # Timeout and bounded-recovery enforcement run before the phase logic.
    param(
        [Parameter(Mandatory)]$Paths,
        [Parameter(Mandatory)]$Job,
        [string]$FixtureRoot,
        [switch]$ConfirmLiveBridge,
        $CapabilityProvider,
        [datetime]$Now
    )
    if (-not $FixtureRoot) { $FixtureRoot = $Paths.FixtureRoot }
    if (-not $CapabilityProvider) { $CapabilityProvider = New-AutopilotCapabilityProvider -Kind unavailable }
    $nowParams = @{}
    if ($PSBoundParameters.ContainsKey('Now')) { $nowParams['Now'] = $Now }
    $nowUtc = if ($PSBoundParameters.ContainsKey('Now')) { Get-AutopilotUtcNow -Now $Now } else { Get-AutopilotUtcNow }
    $job = ConvertTo-AutopilotHashtable $Job
    $results = @()

    if ($true) {
        if ($job.cancellation.requested -and $job.phase -notin $script:AutopilotTerminalPhases) {
            $job = Invoke-AutopilotTransition -Paths $Paths -Job $job -To 'Cancelled' -Reason ('operator cancellation: {0}' -f $job.cancellation.reason) @nowParams
            $null = Release-AutopilotOwnership -Paths $Paths -JobId $job.jobId -Reason 'job cancelled'
            return [pscustomobject]@{ jobId = $job.jobId; phase = $job.phase; action = 'cancelled' }
        }

        # Timeout and bounded-recovery enforcement (active phases only).
        if ($job.phase -in @('Preparing', 'Traveling', 'Executing', 'Verifying', 'Recovering')) {
            if ($job.startedAt) {
                $jobMinutes = ($nowUtc - (ConvertTo-AutopilotUtcDateTime $job.startedAt)).TotalMinutes
                if ($jobMinutes -gt [double][int64]$job.timeoutPolicy.jobTimeoutMinutes) {
                    $job = Invoke-AutopilotTransition -Paths $Paths -Job $job -To 'Blocked' -Reason 'job timeout' -Blocked @{
                        code       = 'job-timeout'
                        detail     = 'job exceeded jobTimeoutMinutes={0} (started {1})' -f $job.timeoutPolicy.jobTimeoutMinutes, $job.startedAt
                        nextAction = 'operator review: cancel the job or resubmit with a larger timeout'
                    } @nowParams
                    $null = Release-AutopilotOwnership -Paths $Paths -JobId $job.jobId -Reason 'job timeout'
                    return [pscustomobject]@{ jobId = $job.jobId; phase = $job.phase; action = 'blocked-job-timeout' }
                }
            }
            if ($job.phaseEnteredAt -and $job.phase -ne 'Recovering') {
                $phaseMinutes = ($nowUtc - (ConvertTo-AutopilotUtcDateTime $job.phaseEnteredAt)).TotalMinutes
                if ($phaseMinutes -gt [double][int64]$job.timeoutPolicy.phaseTimeoutMinutes) {
                    $maxRecoveries = [int64]$job.retryPolicy.maxRecoveriesPerObjective
                    if ($job.phase -in @('Traveling', 'Executing', 'Verifying') -and [int64]$job.progress.recoveriesUsed -lt $maxRecoveries) {
                        $job.progress.recoveriesUsed = [int64]$job.progress.recoveriesUsed + 1
                        $job = Save-AutopilotJob -Paths $Paths -Job $job
                        $job = Invoke-AutopilotTransition -Paths $Paths -Job $job -To 'Recovering' -Reason ('phase timeout after {0} min; bounded recovery {1}/{2}' -f [math]::Round($phaseMinutes, 1), $job.progress.recoveriesUsed, $maxRecoveries) @nowParams
                        return [pscustomobject]@{ jobId = $job.jobId; phase = $job.phase; action = 'recovering' }
                    }
                    $code = if ([int64]$job.progress.recoveriesUsed -ge $maxRecoveries -and $maxRecoveries -gt 0) { 'recovery-budget-exhausted' } else { 'phase-timeout' }
                    $job = Invoke-AutopilotTransition -Paths $Paths -Job $job -To 'Blocked' -Reason 'phase timeout' -Blocked @{
                        code       = $code
                        detail     = 'phase {0} exceeded phaseTimeoutMinutes={1}; recoveries used {2}/{3}' -f $job.phase, $job.timeoutPolicy.phaseTimeoutMinutes, $job.progress.recoveriesUsed, $job.retryPolicy.maxRecoveriesPerObjective
                        nextAction = 'operator review; recovery never degrades the objective into grinding'
                    } @nowParams
                    $null = Release-AutopilotOwnership -Paths $Paths -JobId $job.jobId -Reason 'phase timeout'
                    return [pscustomobject]@{ jobId = $job.jobId; phase = $job.phase; action = "blocked-$code" }
                }
            }
        }

        switch ($job.phase) {
            'Queued' {
                $job = Invoke-AutopilotTransition -Paths $Paths -Job $job -To 'Validating' -Reason 'planner tick' @nowParams
                $results += [pscustomobject]@{ jobId = $job.jobId; phase = $job.phase; action = 'advanced' }
            }
            'Validating' {
                $validation = Test-AutopilotJob -Job $job -SchemaPath $Paths.SchemaPath
                if (-not $validation.Valid) {
                    $job = Invoke-AutopilotTransition -Paths $Paths -Job $job -To 'Failed' -Reason ("schema validation failed: {0}" -f ($validation.Errors -join '; ')) @nowParams
                    $results += [pscustomobject]@{ jobId = $job.jobId; phase = $job.phase; action = 'failed-validation' }
                    break
                }
                # Capability gate: required capabilities must be evidenced as
                # available by the deployed-runtime provider. Source presence and
                # passing unit tests never count (Sol decision #4).
                $missing = Get-AutopilotMissingRequiredCapabilities -Kind $job.kind -Provider $CapabilityProvider
                if (@($missing).Count -gt 0) {
                    $detail = (@($missing) | ForEach-Object { '{0} ({1})' -f $_.capability, $_.reason }) -join '; '
                    $job = Invoke-AutopilotTransition -Paths $Paths -Job $job -To 'Blocked' -Reason 'required capability lacks deployed-runtime evidence' -Blocked @{
                        code       = 'oracle-capability-not-deployed'
                        detail     = "missing: $detail"
                        nextAction = 'await a deployed capability manifest reporting these operations deployed + native adapter + runtime authorized; the planner re-validates automatically'
                    } @nowParams
                    $results += [pscustomobject]@{ jobId = $job.jobId; phase = $job.phase; action = 'blocked-capability' }
                    break
                }
                $prerequisiteState = Test-AutopilotJobPrerequisites -Paths $Paths -Job $job
                if (-not $prerequisiteState.Met) {
                    $waiting = (@($prerequisiteState.Unmet) | ForEach-Object { '{0} ({1})' -f $_.jobId, $_.phase }) -join ', '
                    $job = Invoke-AutopilotTransition -Paths $Paths -Job $job -To 'Blocked' -Reason 'dependency jobs not complete' -Blocked @{
                        code       = 'prerequisite-unmet'
                        detail     = "waiting for dependency jobs: $waiting"
                        nextAction = 'dependencies complete first; the planner re-validates automatically'
                    } @nowParams
                    $results += [pscustomobject]@{ jobId = $job.jobId; phase = $job.phase; action = 'blocked-dependency' }
                    break
                }
                if ($job.kind -notin @('QuestLevel', 'Gather')) {
                    $job = Invoke-AutopilotTransition -Paths $Paths -Job $job -To 'Blocked' -Reason 'kind executor not implemented in this slice' -Blocked @{
                        code       = 'no-legal-action'
                        detail     = "capability evidence is satisfied but the '$($job.kind)' executor is not implemented in the v1.2 slice (QuestLevel and Gather only); see AUTOWOW_AUTOPILOT_V1_PLAN.md"
                        nextAction = 'implement the kind executor in a later slice'
                    } @nowParams
                    $results += [pscustomobject]@{ jobId = $job.jobId; phase = $job.phase; action = 'blocked-unimplemented' }
                    break
                }
                $job = Invoke-AutopilotTransition -Paths $Paths -Job $job -To 'Preparing' -Reason 'validation passed (schema + capability evidence + dependencies)' @nowParams
                $results += [pscustomobject]@{ jobId = $job.jobId; phase = $job.phase; action = 'advanced' }
            }
            'Preparing' {
                # Bind the acting character (kind-aware) and record the plan.
                $leader = if ($job.kind -eq 'QuestLevel' -and $job.spec.Contains('partyLeaderGuid') -and $job.spec.partyLeaderGuid) { [int64]$job.spec.partyLeaderGuid } else { [int64]@($job.characters.eligibleGuids)[0] }
                $job.characters.assignedGuids = @($leader)
                $null = Write-AutopilotReceipt -Paths $Paths -JobId $job.jobId -Event 'plan_selected' -Data @{
                    leader_guid = $leader
                    rationale   = 'spec.partyLeaderGuid preferred; otherwise first eligible GUID (deterministic)'
                } @nowParams
                $job = Save-AutopilotJob -Paths $Paths -Job $job

                # Ownership gate (Sol decision #1): one gameplay mutator per GUID.
                $ownership = Request-AutopilotOwnership -Paths $Paths -JobId $job.jobId -Guids @([int64]$leader) @nowParams
                if ($ownership.FailClosed) {
                    $job = Invoke-AutopilotTransition -Paths $Paths -Job $job -To 'Blocked' -Reason 'ownership registry unusable' -Blocked @{
                        code = 'stale-state'; detail = $ownership.Reason; nextAction = 'repair or remove the ownership file; the planner re-validates automatically'
                    } @nowParams
                    $results += [pscustomobject]@{ jobId = $job.jobId; phase = $job.phase; action = 'blocked-ownership-failclosed' }
                    break
                }
                if (-not $ownership.Acquired) {
                    $conflict = @($ownership.Conflicts)[0]
                    $job = Invoke-AutopilotTransition -Paths $Paths -Job $job -To 'Blocked' -Reason 'character owned by another controller' -Blocked @{
                        code       = 'roster-owned'
                        detail     = 'guid {0} owned by controller {1} (pid {2}) for job {3} until {4}' -f $conflict.guid, $conflict.ownerInstance, $conflict.ownerPid, $conflict.ownerJob, $conflict.expiresAt
                        nextAction = 'wait for the owning controller to release or the lease to expire; never supersede another mutator. For live ops Sol assigns disjoint GUIDs.'
                    } @nowParams
                    $results += [pscustomobject]@{ jobId = $job.jobId; phase = $job.phase; action = 'blocked-roster-owned' }
                    break
                }

                # Native execution performs its own travel; Traveling is
                # receipted as delegated rather than silently skipped.
                $travelNote = if ($job.kind -eq 'Gather') {
                    'gather routing is native (oracle gather_route / worker seek); no client-side pathing is issued'
                }
                else {
                    'bridge quest cycle resolves quest destinations natively (AutoWowBridge.cpp QuestParty); no separate travel order needed'
                }
                $null = Write-AutopilotReceipt -Paths $Paths -JobId $job.jobId -Event 'travel_delegated' -Data @{ reason = $travelNote } @nowParams
                $job = Invoke-AutopilotTransition -Paths $Paths -Job $job -To 'Executing' -Reason 'prepared; ownership acquired; travel delegated to native flow' @nowParams
                $results += [pscustomobject]@{ jobId = $job.jobId; phase = $job.phase; action = 'advanced' }
            }
            'Executing' {
                if (@($job.characters.assignedGuids).Count -eq 0) {
                    $job = Invoke-AutopilotTransition -Paths $Paths -Job $job -To 'Blocked' -Reason 'no assigned characters in Executing' -Blocked @{
                        code = 'stale-state'; detail = 'job reached Executing with no assignedGuids; refusing to guess a character'
                    } @nowParams
                    $results += [pscustomobject]@{ jobId = $job.jobId; phase = $job.phase; action = 'blocked-stale' }
                    break
                }
                $leader = [int64]@($job.characters.assignedGuids)[0]
                $renewal = Request-AutopilotOwnership -Paths $Paths -JobId $job.jobId -Guids @($leader) @nowParams
                if (-not $renewal.Acquired) {
                    $job = Invoke-AutopilotTransition -Paths $Paths -Job $job -To 'Blocked' -Reason 'lost ownership during Executing' -Blocked @{
                        code = 'roster-owned'; detail = "ownership renewal refused: $($renewal.Reason)"; nextAction = 'wait for release/expiry; the planner re-acquires automatically'
                    } @nowParams
                    $results += [pscustomobject]@{ jobId = $job.jobId; phase = $job.phase; action = 'blocked-roster-owned' }
                    break
                }
                if ($job.kind -eq 'Gather') {
                    $null = Invoke-AutopilotOracleIntent -Paths $Paths -Job $job -Operation 'gather_source' -Payload @{
                        item_id       = [int64]$job.spec.item.itemId
                        stop_at_count = [int64]$job.spec.stopAtCount
                        worker_guid   = $leader
                    } -CapabilityProvider $CapabilityProvider @nowParams
                    $job = Invoke-AutopilotTransition -Paths $Paths -Job $job -To 'Verifying' -Reason 'gather intent recorded (dry-run would_send; oracle dispatch surface not deployed)' @nowParams
                    $results += [pscustomobject]@{ jobId = $job.jobId; phase = $job.phase; action = 'executed' }
                    break
                }
                # QuestLevel verb separation (Sol contract): `quest <leader> acquire`
                # is acquisition; `quest <leader> <id>` is exact in-log execution/
                # turn-in; bare `quest <leader>` is the native director cycle.
                $questMode = 'auto'
                if ($job.Contains('metadata') -and $job.metadata -and $job.metadata.Contains('questMode') -and $job.metadata.questMode) {
                    $questMode = [string]$job.metadata.questMode
                }
                elseif ($job.spec.Contains('questAllowlist') -and @($job.spec.questAllowlist).Count -gt 0) {
                    $questMode = 'execute:{0}' -f [int64]@($job.spec.questAllowlist)[0]
                }
                $action = 'quest'
                $actionParams = @{ BotGuid = [uint32]$leader }
                $fixtureName = "quest-$leader.json"
                if ($questMode -eq 'acquire') {
                    $action = 'quest-acquire'
                    $fixtureName = "quest-acquire-$leader.json"
                }
                elseif ($questMode -like 'execute:*') {
                    $actionParams['QuestId'] = [uint32]([int64]$questMode.Substring('execute:'.Length))
                }
                $fixture = Join-Path $FixtureRoot $fixtureName
                $commandParams = @{ Paths = $Paths; Job = $job; Action = $action; ActionParams = $actionParams; CapabilityProvider = $CapabilityProvider }
                if (Test-Path $fixture) { $commandParams['FixturePath'] = $fixture }
                if ($ConfirmLiveBridge) { $commandParams['ConfirmLiveBridge'] = $true }
                $result = Invoke-AutopilotCommand @commandParams @nowParams

                # Bridge response classification: party-contract refusals are
                # planning preconditions, not movement stalls.
                $serverError = $null
                $bridgeOrder = $null
                $bridgePhase = $null
                $bridgeCode = $null
                $bridgeReason = $null
                if ($null -ne $result.Response) {
                    $response = ConvertTo-AutopilotHashtable $result.Response
                    if ($response -is [System.Collections.IDictionary]) {
                        if ($response.Contains('order')) { $bridgeOrder = [string]$response.order }
                        if ($response.Contains('phase')) { $bridgePhase = [string]$response.phase }
                        if ($response.Contains('code')) { $bridgeCode = [string]$response.code }
                        if ($response.Contains('reason')) { $bridgeReason = [string]$response.reason }
                        if ($response.Contains('ok') -and -not [bool]$response.ok -and $response.Contains('error')) {
                            $serverError = [string]$response.error
                        }
                    }
                }
                if ($serverError -eq 'quest_not_in_leader_log' -and 'bridge.quest.acquire' -in @($job.permittedCapabilities)) {
                    # Exact in-log execution needs the quest in the log first:
                    # re-plan toward acquisition (permitted), never recovery.
                    $null = Write-AutopilotReceipt -Paths $Paths -JobId $job.jobId -Event 'replan_to_acquisition' -Data @{
                        server_code = $serverError
                        from_mode   = $questMode
                        reason      = $script:AutopilotPartyContractErrors[$serverError]
                    } @nowParams
                    if (-not $job.Contains('metadata') -or $null -eq $job.metadata) { $job.metadata = [ordered]@{} }
                    $job.metadata.questMode = 'acquire'
                    $job = Save-AutopilotJob -Paths $Paths -Job $job
                    $results += [pscustomobject]@{ jobId = $job.jobId; phase = $job.phase; action = 'replanned-to-acquisition' }
                    break
                }
                if ($serverError -and $script:AutopilotPartyContractErrors.ContainsKey($serverError)) {
                    $condition = [string]$script:AutopilotPartyContractErrors[$serverError]
                    $null = Write-AutopilotReceipt -Paths $Paths -JobId $job.jobId -Event 'party_precondition' -Data @{
                        server_code = $serverError
                        condition   = $condition
                        quest_mode  = $questMode
                    } @nowParams
                    $job = Invoke-AutopilotTransition -Paths $Paths -Job $job -To 'Blocked' -Reason 'bridge party-contract precondition refused the order' -Blocked @{
                        code       = 'party-contract-unmet'
                        detail     = "server refused '$serverError': $condition"
                        nextAction = 'change the plan (roster/quest selection) or wait for the party to satisfy the contract normally; the controller never fabricates parties, shares quests, or changes factions'
                    } @nowParams
                    $null = Release-AutopilotOwnership -Paths $Paths -JobId $job.jobId -Reason 'party contract unmet'
                    $results += [pscustomobject]@{ jobId = $job.jobId; phase = $job.phase; action = 'blocked-party-contract' }
                    break
                }
                if ($bridgeOrder -eq 'quest' -and $bridgePhase -eq 'blocked') {
                    if ($bridgeCode -eq 'quest_not_in_leader_log' -and 'bridge.quest.acquire' -in @($job.permittedCapabilities)) {
                        $null = Write-AutopilotReceipt -Paths $Paths -JobId $job.jobId -Event 'replan_to_acquisition' -Data @{
                            server_code = $bridgeCode
                            from_mode   = $questMode
                            reason      = $script:AutopilotPartyContractErrors[$bridgeCode]
                        } @nowParams
                        if (-not $job.Contains('metadata') -or $null -eq $job.metadata) { $job.metadata = [ordered]@{} }
                        $job.metadata.questMode = 'acquire'
                        $job = Save-AutopilotJob -Paths $Paths -Job $job
                        $results += [pscustomobject]@{ jobId = $job.jobId; phase = $job.phase; action = 'replanned-to-acquisition' }
                        break
                    }

                    if ($bridgeCode -eq 'quest_no_destination') {
                        $maxQuestRecoveries = [int64]([int64]$job.retryPolicy.maxRecoveriesPerObjective)
                        if ($maxQuestRecoveries -lt 1) { $maxQuestRecoveries = 1 }
                        $questFlowRecoveries = if ($job.Contains('metadata') -and $job.metadata -and $job.metadata.Contains('questFlowRecoveries')) {
                            [int64]$job.metadata.questFlowRecoveries
                        }
                        else { [int64]0 }
                        if ($questFlowRecoveries -lt $maxQuestRecoveries) {
                            if (-not $job.Contains('metadata') -or $null -eq $job.metadata) { $job.metadata = [ordered]@{} }
                            $job.metadata.questFlowRecoveries = [int64]($questFlowRecoveries + 1)
                            $job.progress.recoveriesUsed = [int64]$job.progress.recoveriesUsed + 1
                            $job = Save-AutopilotJob -Paths $Paths -Job $job
                            $null = Write-AutopilotReceipt -Paths $Paths -JobId $job.jobId -Event 'quest_no_destination_recovery' -Data @{
                                blocked_code = $bridgeCode
                                attempt      = [int64]$job.metadata.questFlowRecoveries
                                max_retries  = $maxQuestRecoveries
                                reason       = $bridgeReason
                            } @nowParams
                            $job = Invoke-AutopilotTransition -Paths $Paths -Job $job -To 'Recovering' -Reason 'recovering from quest_no_destination blocker' @nowParams
                            $results += [pscustomobject]@{ jobId = $job.jobId; phase = $job.phase; action = 'recovering-from-quest-flow' }
                            break
                        }
                    }

                    if ($bridgeCode -and $script:AutopilotPartyContractErrors.ContainsKey($bridgeCode)) {
                        $condition = [string]$script:AutopilotPartyContractErrors[$bridgeCode]
                        $null = Write-AutopilotReceipt -Paths $Paths -JobId $job.jobId -Event 'party_precondition' -Data @{
                            server_code = $bridgeCode
                            condition   = $condition
                            quest_mode  = $questMode
                        } @nowParams
                        $job = Invoke-AutopilotTransition -Paths $Paths -Job $job -To 'Blocked' -Reason 'bridge party-contract precondition refused the order' -Blocked @{
                            code       = 'party-contract-unmet'
                            detail     = "server refused '$bridgeCode': $condition"
                            nextAction = 'change the plan (roster/quest selection) or wait for the party to satisfy the contract normally; the controller never fabricates parties, shares quests, or changes factions'
                        } @nowParams
                        $null = Release-AutopilotOwnership -Paths $Paths -JobId $job.jobId -Reason 'party contract unmet'
                        $results += [pscustomobject]@{ jobId = $job.jobId; phase = $job.phase; action = 'blocked-party-contract' }
                        break
                    }

                    $questFlowDetail = if ($bridgeCode -and $bridgeReason) {
                        "quest flow blocked in bridge: $bridgeCode ($bridgeReason)"
                    } elseif ($bridgeCode) {
                        "quest flow blocked in bridge: $bridgeCode"
                    } else {
                        'quest flow blocked in bridge without a typed code'
                    }

                    $null = Write-AutopilotReceipt -Paths $Paths -JobId $job.jobId -Event 'bridge_quest_blocked' -Data @{
                        order       = $bridgeOrder
                        phase       = $bridgePhase
                        code        = $bridgeCode
                        reason      = $bridgeReason
                        quest_mode  = $questMode
                    } @nowParams
                    $job = Invoke-AutopilotTransition -Paths $Paths -Job $job -To 'Blocked' -Reason 'bridge quest flow blocked' -Blocked @{
                        code       = 'quest-flow-blocked'
                        detail     = $questFlowDetail
                        nextAction = 'inspect quest flow blockers (quest/no-starter/party composition); release blockers and rerun'
                    } @nowParams
                    $null = Release-AutopilotOwnership -Paths $Paths -JobId $job.jobId -Reason 'quest flow blocked'
                    $results += [pscustomobject]@{ jobId = $job.jobId; phase = $job.phase; action = 'blocked-quest-flow' }
                    break
                }
                $job = Invoke-AutopilotTransition -Paths $Paths -Job $job -To 'Verifying' -Reason ("quest {0} {1}" -f $questMode, ($(if ($result.Deduplicated) { 'already issued (deduplicated)' } elseif ($result.Sent) { 'issued live' } else { 'recorded (dry-run would_send)' }))) @nowParams
                $results += [pscustomobject]@{ jobId = $job.jobId; phase = $job.phase; action = 'executed' }
            }
            'Verifying' {
                if (@($job.characters.assignedGuids).Count -eq 0) {
                    $job = Invoke-AutopilotTransition -Paths $Paths -Job $job -To 'Blocked' -Reason 'no assigned characters in Verifying' -Blocked @{
                        code = 'stale-state'; detail = 'job reached Verifying with no assignedGuids; refusing to guess a character'
                    } @nowParams
                    $results += [pscustomobject]@{ jobId = $job.jobId; phase = $job.phase; action = 'blocked-stale' }
                    break
                }
                $leader = [int64]@($job.characters.assignedGuids)[0]
                if ($job.kind -eq 'Gather') {
                    # Verified inventory observation (fixture today; SELECT-only
                    # snapshot or oracle receipt export later). Unknown stays
                    # unknown - no observation means no counter movement.
                    $fixture = Join-Path $FixtureRoot "inventory-$leader.json"
                    $count = $null
                    $source = 'none'
                    if (Test-Path $fixture) {
                        try {
                            $inventory = ConvertTo-AutopilotHashtable (Get-Content -LiteralPath $fixture -Raw | ConvertFrom-Json)
                            if ([int64]$inventory.item_id -eq [int64]$job.spec.item.itemId) {
                                $count = [int64]$inventory.count
                                $source = 'fixture'
                            }
                        }
                        catch { $count = $null }
                    }
                    $null = Write-AutopilotReceipt -Paths $Paths -JobId $job.jobId -Event 'progress_observed_gather' -Data @{
                        item_id = [int64]$job.spec.item.itemId
                        count   = $count
                        source  = $source
                    } @nowParams
                    if ($null -ne $count) {
                        $job.progress.counters['itemCount'] = [int64]$count
                        $job.progress.lastProgressAt = ConvertTo-AutopilotTimestamp -Time $nowUtc
                        $job = Save-AutopilotJob -Paths $Paths -Job $job
                    }
                    $target = [int64]$job.successCriteria.counters['itemCount']
                    $achieved = [int64]$(if ($job.progress.counters.Contains('itemCount')) { $job.progress.counters['itemCount'] } else { 0 })
                    if ($achieved -ge $target) {
                        $job = Invoke-AutopilotTransition -Paths $Paths -Job $job -To 'Completed' -Reason "successCriteria met: itemCount $achieved/$target (verified inventory observation receipted)" @nowParams
                        $null = Release-AutopilotOwnership -Paths $Paths -JobId $job.jobId -Reason 'job completed'
                        $results += [pscustomobject]@{ jobId = $job.jobId; phase = $job.phase; action = 'completed' }
                    }
                    else {
                        $job = Invoke-AutopilotTransition -Paths $Paths -Job $job -To 'Executing' -Reason "gathered $achieved/$target; success counters unmet" @nowParams
                        $results += [pscustomobject]@{ jobId = $job.jobId; phase = $job.phase; action = 'continuing' }
                    }
                    break
                }
                $fixture = Join-Path $FixtureRoot "questobjective-$leader.json"
                $observation = Invoke-AutopilotCommand -Paths $Paths -Job $job -Action 'questobjective' -ActionParams @{ BotGuid = [uint32]$leader } -FixturePath $(if (Test-Path $fixture) { $fixture } else { $null }) -ConfirmLiveBridge:$ConfirmLiveBridge -CapabilityProvider $CapabilityProvider @nowParams
                $facts = Get-AutopilotQuestObjectiveFacts -Response $observation.Response
                $null = Write-AutopilotReceipt -Paths $Paths -JobId $job.jobId -Event 'progress_observed' -Data @{
                    quest_id  = $facts.questId
                    current   = $facts.current
                    required  = $facts.required
                    rewarded  = $facts.rewarded
                    supported = $facts.supported
                    obj_phase = $facts.phase
                    source    = $(if ($observation.Sent) { 'live-bridge' } else { 'fixture' })
                } @nowParams

                # Live turn-in detection: the objective view never claims rewards.
                # Track the quest log; when a tracked quest VANISHES, probe the
                # read-only acceptance verb for the reward postcondition
                # (reward.reward_status / quest_status 6 = REWARDED). Only that
                # evidence counts as a turn-in - never inference.
                if (-not $facts.rewarded -and $observation.Sent) {
                    $logObservation = Invoke-AutopilotCommand -Paths $Paths -Job $job -Action 'questlog' -ActionParams @{ BotGuid = [uint32]$leader } -ConfirmLiveBridge:$ConfirmLiveBridge -CapabilityProvider $CapabilityProvider @nowParams
                    $currentQuests = @()
                    if ($logObservation.Response) {
                        $log = ConvertTo-AutopilotHashtable $logObservation.Response
                        if ($log.Contains('quests')) {
                            $currentQuests = @(@($log.quests) | ForEach-Object { [int64]$_.id })
                            $null = Write-AutopilotReceipt -Paths $Paths -JobId $job.jobId -Event 'questlog_observed' -Data @{
                                quests = @(@($log.quests) | ForEach-Object { [ordered]@{ id = [int64]$_.id; title = [string]$_.title; objectives_all_done = [bool]$_.objectives_all_done } })
                            } @nowParams
                        }
                    }
                    if (-not $job.Contains('metadata') -or $null -eq $job.metadata) { $job.metadata = [ordered]@{} }
                    $previousQuests = @($(if ($job.metadata.Contains('trackedQuests') -and $job.metadata.trackedQuests) { $job.metadata.trackedQuests } else { @() }) | ForEach-Object { [int64]$_ })
                    foreach ($vanished in @($previousQuests | Where-Object { $_ -notin $currentQuests })) {
                        $probe = Invoke-AutopilotCommand -Paths $Paths -Job $job -Action 'acceptance' -ActionParams @{ BotGuid = [uint32]$leader; QuestId = [uint32]$vanished } -ConfirmLiveBridge:$ConfirmLiveBridge -CapabilityProvider $CapabilityProvider @nowParams
                        if ($probe.Response) {
                            $evidence = ConvertTo-AutopilotHashtable $probe.Response
                            $rewardStatus = $false
                            $questStatus = $null
                            if ($evidence.Contains('reward') -and $evidence.reward -is [System.Collections.IDictionary]) {
                                if ($evidence.reward.Contains('reward_status')) { $rewardStatus = [bool]$evidence.reward.reward_status }
                                if ($evidence.reward.Contains('quest_status')) { $questStatus = [int64]$evidence.reward.quest_status }
                            }
                            $rewarded = ($rewardStatus -or $questStatus -eq 6)   # QUEST_STATUS_REWARDED
                            $null = Write-AutopilotReceipt -Paths $Paths -JobId $job.jobId -Event 'turnin_probe' -Data @{
                                quest_id      = $vanished
                                reward_status = $rewardStatus
                                quest_status  = $questStatus
                                rewarded      = $rewarded
                            } @nowParams
                            if ($rewarded) {
                                $facts.rewarded = $true
                                $facts.questId = $vanished
                            }
                        }
                    }
                    $job.metadata.trackedQuests = @($currentQuests)
                    $job = Save-AutopilotJob -Paths $Paths -Job $job
                }

                if ($facts.rewarded) {
                    $job.progress.counters['questsTurnedIn'] = [int64]($(if ($job.progress.counters.Contains('questsTurnedIn')) { $job.progress.counters['questsTurnedIn'] } else { 0 })) + 1
                    $job.progress.lastProgressAt = ConvertTo-AutopilotTimestamp -Time $nowUtc
                    $job = Save-AutopilotJob -Paths $Paths -Job $job
                }

                $target = [int64]$job.successCriteria.counters['questsTurnedIn']
                $achieved = [int64]$(if ($job.progress.counters.Contains('questsTurnedIn')) { $job.progress.counters['questsTurnedIn'] } else { 0 })
                if ($achieved -ge $target) {
                    $job = Invoke-AutopilotTransition -Paths $Paths -Job $job -To 'Completed' -Reason "successCriteria met: questsTurnedIn $achieved/$target (rewarded postcondition evidence receipted)" @nowParams
                    $null = Release-AutopilotOwnership -Paths $Paths -JobId $job.jobId -Reason 'job completed'
                    $results += [pscustomobject]@{ jobId = $job.jobId; phase = $job.phase; action = 'completed' }
                }
                else {
                    $job = Invoke-AutopilotTransition -Paths $Paths -Job $job -To 'Executing' -Reason "objective progress $($facts.current)/$($facts.required); success counters unmet ($achieved/$target)" @nowParams
                    $results += [pscustomobject]@{ jobId = $job.jobId; phase = $job.phase; action = 'continuing' }
                }
            }
            'Recovering' {
                if (@($job.characters.assignedGuids).Count -eq 0) {
                    $job = Invoke-AutopilotTransition -Paths $Paths -Job $job -To 'Blocked' -Reason 'no assigned characters in Recovering' -Blocked @{
                        code = 'stale-state'; detail = 'job reached Recovering with no assignedGuids; refusing to guess a character'
                    } @nowParams
                    $results += [pscustomobject]@{ jobId = $job.jobId; phase = $job.phase; action = 'blocked-stale' }
                    break
                }
                $leader = [int64]@($job.characters.assignedGuids)[0]
                if ($job.kind -eq 'QuestLevel') {
                    # Bounded native recovery: bridge `recover` resets transient
                    # travel/RPG state only - never teleports, never edits quests.
                    $null = Invoke-AutopilotCommand -Paths $Paths -Job $job -Action 'recover' -ActionParams @{ BotGuid = [uint32]$leader } -ConfirmLiveBridge:$ConfirmLiveBridge -CapabilityProvider $CapabilityProvider @nowParams
                }
                else {
                    $null = Write-AutopilotReceipt -Paths $Paths -JobId $job.jobId -Event 'recovery_delegated' -Data @{
                        reason = 'native oracle recovery lane owns corpse/stall recovery for this kind; no client-side action exists'
                    } @nowParams
                }
                # A fresh attempt after recovery: new idempotency keys authorize
                # exactly one re-kick of the native cycle (still bounded by the
                # recovery budget above).
                $job.progress.attempt = [int64]$job.progress.attempt + 1
                $job = Save-AutopilotJob -Paths $Paths -Job $job
                $job = Invoke-AutopilotTransition -Paths $Paths -Job $job -To 'Executing' -Reason 'recovery step issued; resuming the objective (never degraded to grinding)' @nowParams
                $results += [pscustomobject]@{ jobId = $job.jobId; phase = $job.phase; action = 'recovered' }
            }
            'Blocked' {
                # Bounded self-healing: capability blocks re-validate when the
                # manifest starts reporting the requirement; roster blocks retry
                # acquisition (release/expiry may have freed the GUID).
                $code = if ($job.blockedReason) { [string]$job.blockedReason.code } else { '' }
                if ($code -in @('oracle-capability-not-deployed', 'capability-missing')) {
                    $missing = Get-AutopilotMissingRequiredCapabilities -Kind $job.kind -Provider $CapabilityProvider
                    if (@($missing).Count -eq 0) {
                        $job = Invoke-AutopilotTransition -Paths $Paths -Job $job -To 'Validating' -Reason 'capability manifest now evidences all required operations; re-validating' @nowParams
                        $results += [pscustomobject]@{ jobId = $job.jobId; phase = $job.phase; action = 'capability-unblocked' }
                        break
                    }
                }
                elseif ($code -eq 'roster-owned') {
                    $guids = @($job.characters.assignedGuids)
                    if ($guids.Count -eq 0) { $guids = @($job.characters.eligibleGuids | Select-Object -First 1) }
                    $retry = Request-AutopilotOwnership -Paths $Paths -JobId $job.jobId -Guids @($guids | ForEach-Object { [int64]$_ }) @nowParams
                    if ($retry.Acquired) {
                        $job = Invoke-AutopilotTransition -Paths $Paths -Job $job -To 'Validating' -Reason 'ownership now available; re-validating' @nowParams
                        $results += [pscustomobject]@{ jobId = $job.jobId; phase = $job.phase; action = 'ownership-unblocked' }
                        break
                    }
                }
                elseif ($code -eq 'prerequisite-unmet') {
                    $prerequisiteState = Test-AutopilotJobPrerequisites -Paths $Paths -Job $job
                    if ($prerequisiteState.Met) {
                        $job = Invoke-AutopilotTransition -Paths $Paths -Job $job -To 'Validating' -Reason 'dependency jobs now complete; re-validating' @nowParams
                        $results += [pscustomobject]@{ jobId = $job.jobId; phase = $job.phase; action = 'dependency-unblocked' }
                        break
                    }
                }
                elseif ($code -eq 'stale-state' -and $job.blockedReason -and [string]$job.blockedReason.detail -like '*server session changed*') {
                    $job = Invoke-AutopilotTransition -Paths $Paths -Job $job -To 'Validating' -Reason 'revalidating after server session change' @nowParams
                    $results += [pscustomobject]@{ jobId = $job.jobId; phase = $job.phase; action = 'session-revalidated' }
                    break
                }
                $results += [pscustomobject]@{ jobId = $job.jobId; phase = $job.phase; action = 'held' }
            }
            default {
                $results += [pscustomobject]@{ jobId = $job.jobId; phase = $job.phase; action = 'held' }
            }
        }
    }
    return $results
}

function Invoke-AutopilotPlannerTick {
    # Advances every non-terminal job by at most one transition. Deterministic:
    # jobs are processed in store order (priority desc, createdAt, jobId).
    # V1.2: per-job logic lives in Invoke-AutopilotJobStep; the scheduler
    # (Invoke-AutopilotScheduledTick) is the character-slot-aware alternative.
    param(
        [Parameter(Mandatory)]$Paths,
        [string]$FixtureRoot,
        [switch]$ConfirmLiveBridge,
        $CapabilityProvider,
        [datetime]$Now
    )
    if (-not $FixtureRoot) { $FixtureRoot = $Paths.FixtureRoot }
    if (-not $CapabilityProvider) { $CapabilityProvider = Resolve-AutopilotCapabilityProvider -Paths $Paths }
    $stepParams = @{ Paths = $Paths; FixtureRoot = $FixtureRoot; CapabilityProvider = $CapabilityProvider }
    if ($ConfirmLiveBridge) { $stepParams['ConfirmLiveBridge'] = $true }
    if ($PSBoundParameters.ContainsKey('Now')) { $stepParams['Now'] = $Now }
    $results = @()
    foreach ($job in (Get-AutopilotJobs -Paths $Paths)) {
        $results += Invoke-AutopilotJobStep -Job $job @stepParams
    }
    return , @($results)
}

# ---------------------------------------------------------------------------
# Single-owner lock, reconciliation, explain
# ---------------------------------------------------------------------------

function Enter-AutopilotLock {
    param([Parameter(Mandatory)]$Paths, [datetime]$Now)
    $nowParams = @{}
    if ($PSBoundParameters.ContainsKey('Now')) { $nowParams['Now'] = $Now }
    if (Test-Path $Paths.LockPath) {
        $lock = Get-Content -LiteralPath $Paths.LockPath -Raw | ConvertFrom-Json
        $existing = Get-Process -Id $lock.pid -ErrorAction SilentlyContinue
        if ($existing) {
            throw "Autopilot already running (pid $($lock.pid), started $($lock.started_utc)). Refusing a second instance."
        }
        Remove-Item -LiteralPath $Paths.LockPath -Force
        $null = Write-AutopilotReceipt -Paths $Paths -JobId 'controller' -Event 'stale_lock_recovered' -Data @{ stale_pid = $lock.pid; stale_started_utc = $lock.started_utc } @nowParams
    }
    $content = [ordered]@{
        pid         = $PID
        started_utc = ConvertTo-AutopilotTimestamp -Time $(if ($PSBoundParameters.ContainsKey('Now')) { $Now } else { Get-AutopilotUtcNow })
        script      = 'autowow-autopilot.ps1'
    } | ConvertTo-Json
    Write-AutopilotFileAtomic -Path $Paths.LockPath -Content $content
    $null = Write-AutopilotReceipt -Paths $Paths -JobId 'controller' -Event 'controller_started' -Data @{ pid = $PID } @nowParams
}

function Exit-AutopilotLock {
    param([Parameter(Mandatory)]$Paths)
    if (Test-Path $Paths.LockPath) {
        $lock = Get-Content -LiteralPath $Paths.LockPath -Raw | ConvertFrom-Json
        if ([int]$lock.pid -eq $PID) { Remove-Item -LiteralPath $Paths.LockPath -Force }
    }
    $null = Write-AutopilotReceipt -Paths $Paths -JobId 'controller' -Event 'controller_stopped' -Data @{ pid = $PID }
}

function Invoke-AutopilotReconcile {
    # Receipts are the ledger; job documents are snapshots. After a crash the
    # snapshot may lag the ledger - roll it forward, never backward, and never
    # re-issue receipted irreversible commands.
    param([Parameter(Mandatory)]$Paths, [datetime]$Now)
    $nowParams = @{}
    if ($PSBoundParameters.ContainsKey('Now')) { $nowParams['Now'] = $Now }
    $null = Write-AutopilotReceipt -Paths $Paths -JobId 'controller' -Event 'reconciliation_started' -Data @{} @nowParams
    $reconciled = @()
    foreach ($job in (Get-AutopilotJobs -Paths $Paths)) {
        $receipts = Get-AutopilotReceipts -Paths $Paths -JobId $job.jobId
        $lastTransition = @($receipts | Where-Object { $_.event -eq 'job_transition' }) | Select-Object -Last 1
        $corrected = $false
        if ($lastTransition -and [string]$lastTransition.to -ne [string]$job.phase) {
            $ledgerStamp = ConvertTo-AutopilotTimestamp -Time (ConvertTo-AutopilotUtcDateTime $lastTransition.timestamp_utc)
            $job.phase = [string]$lastTransition.to
            $job.phaseEnteredAt = $ledgerStamp
            if ($job.phase -eq 'Blocked' -and $lastTransition.PSObject.Properties['blocked']) {
                $job.blockedReason = [ordered]@{
                    code       = [string]$lastTransition.blocked.code
                    detail     = [string]$lastTransition.blocked.detail
                    since      = $ledgerStamp
                    nextAction = $null
                }
            }
            $job = Save-AutopilotJob -Paths $Paths -Job $job
            $corrected = $true
        }
        $sentKeys = Get-AutopilotSentCommandKeys -Paths $Paths -JobId $job.jobId
        $null = Write-AutopilotReceipt -Paths $Paths -JobId $job.jobId -Event 'job_reconciled' -Data @{
            phase                 = $job.phase
            corrected_from_ledger = $corrected
            known_command_keys    = @($sentKeys.Keys).Count
        } @nowParams
        $reconciled += [pscustomobject]@{ jobId = $job.jobId; phase = $job.phase; corrected = $corrected }
    }
    $null = Write-AutopilotReceipt -Paths $Paths -JobId 'controller' -Event 'reconciliation_completed' -Data @{
        jobs_reconciled                      = $reconciled.Count
        duplicate_irreversible_command_count = 0
    } @nowParams
    return $reconciled
}

function Get-AutopilotWaitState {
    # Sol decision E: explain must distinguish these wait states.
    param([Parameter(Mandatory)]$Job)
    $code = if ($Job.blockedReason) { [string]$Job.blockedReason.code } else { '' }
    switch ($Job.phase) {
        'Blocked' {
            if ($code -in @('oracle-capability-not-deployed', 'capability-missing')) { return 'waiting-for-server-capability' }
            if ($code -eq 'roster-owned') { return 'waiting-for-ownership' }
            if ($code -eq 'party-contract-unmet') { return 'waiting-for-party-contract' }
            if ($code -eq 'quest-flow-blocked') { return 'waiting-for-quest-blocked' }
            if ($code -eq 'prerequisite-unmet') { return 'preparing-prerequisites' }
            return "blocked-$code"
        }
        'Queued' { return 'validating' }
        'Validating' { return 'validating' }
        'Preparing' { return 'preparing-prerequisites' }
        'Traveling' { return 'traveling' }
        'Executing' { return 'executing' }
        'Verifying' { return 'verifying' }
        'Recovering' { return 'recovering' }
        'Suspended' { return 'suspended' }
        'Completed' { return 'completed' }
        'Cancelled' { return 'terminal-failure' }
        'Failed' { return 'terminal-failure' }
    }
    return 'unknown'
}

function Get-AutopilotExplanation {
    param(
        [Parameter(Mandatory)]$Paths,
        [string]$JobId,
        [uint32]$Guid,
        $CapabilityProvider
    )
    $job = $null
    if ($JobId) { $job = Get-AutopilotJob -Paths $Paths -JobId $JobId }
    elseif ($Guid) {
        # Selection priority for "what is this character doing?": a live job the
        # character is ASSIGNED to beats a job it is merely eligible for, and a
        # recently finished assigned job beats an unrelated eligible pool match.
        $all = @(Get-AutopilotJobs -Paths $Paths -IncludeTerminal)
        $assignedTo = { param($j) [int64]$Guid -in @(@($j.characters.assignedGuids) | ForEach-Object { [int64]$_ }) }
        $eligibleFor = { param($j) [int64]$Guid -in @(@($j.characters.eligibleGuids) | ForEach-Object { [int64]$_ }) }
        $active = @($all | Where-Object { $_.phase -notin $script:AutopilotTerminalPhases })
        $job = @($active | Where-Object { & $assignedTo $_ }) | Select-Object -First 1
        if (-not $job) {
            $job = @($all | Where-Object { $_.phase -in $script:AutopilotTerminalPhases -and (& $assignedTo $_) } |
                    Sort-Object -Property @{Expression = { ConvertTo-AutopilotUtcDateTime $_.updatedAt } } -Descending) | Select-Object -First 1
        }
        if (-not $job) { $job = @($active | Where-Object { & $eligibleFor $_ }) | Select-Object -First 1 }
    }
    if (-not $job) { return $null }
    if (-not $CapabilityProvider) { $CapabilityProvider = Resolve-AutopilotCapabilityProvider -Paths $Paths }
    $receipts = Get-AutopilotReceipts -Paths $Paths -JobId $job.jobId
    $lastPlan = @($receipts | Where-Object { $_.event -eq 'plan_selected' }) | Select-Object -Last 1
    $lastProgress = @($receipts | Where-Object { $_.event -eq 'progress_observed' }) | Select-Object -Last 1
    $lastTransition = @($receipts | Where-Object { $_.event -eq 'job_transition' }) | Select-Object -Last 1

    $waiting = if ($job.phase -eq 'Blocked' -and $job.blockedReason) {
        '{0}: {1} (since {2})' -f $job.blockedReason.code, $job.blockedReason.detail, $job.blockedReason.since
    }
    elseif ($job.phase -in $script:AutopilotTerminalPhases) { "terminal phase $($job.phase)" }
    else { "active in phase $($job.phase); next planner tick advances it" }

    $next = if ($job.phase -eq 'Blocked' -and $job.blockedReason -and $job.blockedReason.nextAction) { $job.blockedReason.nextAction }
    elseif ($job.phase -eq 'Executing') { 'verify objective counters via questobjective' }
    elseif ($job.phase -eq 'Verifying') { 'compare verified counters against successCriteria' }
    elseif ($job.phase -in $script:AutopilotTerminalPhases) { 'nothing; job is terminal' }
    else { 'advance to the next phase in the state machine' }

    $requiredCapabilities = @(
        foreach ($capability in @($script:AutopilotKindRequiredCapabilities[$job.kind])) {
            $status = Get-AutopilotCapabilityStatus -Capability $capability -Provider $CapabilityProvider
            [pscustomobject]@{ capability = $capability; available = $status.available; reason = $status.reason }
        }
    )

    [pscustomobject]@{
        jobId            = $job.jobId
        waitState        = Get-AutopilotWaitState -Job $job
        whatItIsDoing    = '{0} job in phase {1}: {2}' -f $job.kind, $job.phase, (ConvertTo-AutopilotCanonicalJson $job.spec)
        whyThisAction    = if ($lastPlan) { '{0} (receipt {1})' -f $lastPlan.rationale, $lastPlan.receipt_id } else { 'no plan receipt yet (job has not reached Preparing)' }
        exactProgress    = if ($lastProgress) { 'quest {0}: objective {1}/{2}, rewarded={3} (receipt {4}); counters: {5}' -f $lastProgress.quest_id, $lastProgress.current, $lastProgress.required, $lastProgress.rewarded, $lastProgress.receipt_id, (ConvertTo-AutopilotCanonicalJson $job.progress.counters) } else { 'no verified progress yet; counters: ' + (ConvertTo-AutopilotCanonicalJson $job.progress.counters) }
        whyWaiting       = $waiting
        whatNext         = $next
        requiredCapabilities = $requiredCapabilities
        capabilitySource = $CapabilityProvider.Kind
        lastTransition   = if ($lastTransition) { '{0} -> {1} at {2}: {3}' -f $lastTransition.from, $lastTransition.to, $lastTransition.timestamp_utc, $lastTransition.reason } else { $null }
        executionMode    = $job.executionMode
    }
}

# ---------------------------------------------------------------------------
# V1.2 core primitives: clock, checkpoint, session identity, oracle intents,
# prerequisite evaluation, bulk ownership release.
# ---------------------------------------------------------------------------

function New-AutopilotClock {
    # Deterministic time source. 'fixture' clocks never touch the wall clock,
    # so an eight-hour soak runs in seconds and every timestamp is reproducible.
    param(
        [ValidateSet('real', 'fixture')][string]$Kind = 'real',
        [datetime]$StartUtc,
        [ValidateRange(1, 86400)][int]$StepSeconds = 60
    )
    if ($Kind -eq 'fixture' -and -not $PSBoundParameters.ContainsKey('StartUtc')) {
        throw 'A fixture clock requires -StartUtc.'
    }
    [pscustomobject]@{
        Kind        = $Kind
        CurrentUtc  = $(if ($Kind -eq 'fixture') { $StartUtc.ToUniversalTime() } else { $null })
        StepSeconds = $StepSeconds
    }
}

function Get-AutopilotClockNow {
    param($Clock)
    if ($Clock -and $Clock.Kind -eq 'fixture') { return $Clock.CurrentUtc }
    return (Get-Date).ToUniversalTime()
}

function Step-AutopilotClock {
    param([Parameter(Mandatory)]$Clock, [int]$Seconds)
    if ($Clock.Kind -ne 'fixture') { return }
    $step = if ($PSBoundParameters.ContainsKey('Seconds')) { $Seconds } else { $Clock.StepSeconds }
    $Clock.CurrentUtc = $Clock.CurrentUtc.AddSeconds($step)
}

$script:AutopilotCheckpointSchema = 'autowow.autopilot.checkpoint.v1'

function Save-AutopilotCheckpoint {
    param([Parameter(Mandatory)]$Paths, [Parameter(Mandatory)][hashtable]$Data, [datetime]$Now)
    $utc = if ($PSBoundParameters.ContainsKey('Now')) { Get-AutopilotUtcNow -Now $Now } else { Get-AutopilotUtcNow }
    $doc = [ordered]@{
        schema         = $script:AutopilotCheckpointSchema
        schema_version = 1
        updated_utc    = ConvertTo-AutopilotTimestamp -Time $utc
    }
    foreach ($key in ($Data.Keys | Sort-Object)) { $doc[$key] = $Data[$key] }
    Write-AutopilotFileAtomic -Path (Join-Path $Paths.StateRoot 'checkpoint.json') -Content ($doc | ConvertTo-Json -Depth 20)
    return $doc
}

function Get-AutopilotCheckpoint {
    # Fail closed: unreadable or unknown-version checkpoints are ignored (null),
    # never partially trusted.
    param([Parameter(Mandatory)]$Paths)
    $file = Join-Path $Paths.StateRoot 'checkpoint.json'
    if (-not (Test-Path $file)) { return $null }
    try {
        $doc = ConvertTo-AutopilotHashtable (Get-Content -LiteralPath $file -Raw | ConvertFrom-Json)
        if ([string]$doc.schema -ne $script:AutopilotCheckpointSchema -or [int64]$doc.schema_version -ne 1) { return $null }
        return $doc
    }
    catch { return $null }
}

function Get-AutopilotSessionIdentity {
    # Server identity = (server_build, session_id) from the deployed capability
    # manifest. Without a manifest the identity is unknown, not assumed stable.
    param($Provider)
    if (-not $Provider -or -not $Provider.Available) {
        return [pscustomobject]@{ Known = $false; Build = $null; SessionId = $null }
    }
    [pscustomobject]@{
        Known     = $true
        Build     = [string]$Provider.ServerBuild
        SessionId = [string]$Provider.SessionId
    }
}

function Test-AutopilotSessionChanged {
    # Only two KNOWN identities can prove a change; unknown never counts as a
    # change (fail closed toward no side effects).
    param($Previous, $Current)
    if (-not $Previous -or -not $Current) { return $false }
    $prevKnown = [bool]$Previous.Known
    $currKnown = [bool]$Current.Known
    if (-not $prevKnown -or -not $currKnown) { return $false }
    return ([string]$Previous.Build -ne [string]$Current.Build) -or ([string]$Previous.SessionId -ne [string]$Current.SessionId)
}

function Invoke-AutopilotOracleIntent {
    <#
    Records a dry-run Oracle operation intent (surface 'oracle'). There is no
    deployed Oracle dispatch surface, so live oracle intents always refuse -
    this function can rehearse, it can never execute.
    #>
    param(
        [Parameter(Mandatory)]$Paths,
        [Parameter(Mandatory)]$Job,
        [Parameter(Mandatory)][string]$Operation,
        [Parameter(Mandatory)][hashtable]$Payload,
        $CapabilityProvider,
        [datetime]$Now
    )
    $job = ConvertTo-AutopilotHashtable $Job
    $capability = "oracle.$Operation"
    if (-not $CapabilityProvider) { $CapabilityProvider = New-AutopilotCapabilityProvider -Kind unavailable }
    if ($capability -notin @($job.permittedCapabilities)) {
        throw "Capability '$capability' is not in job $($job.jobId) permittedCapabilities."
    }
    $status = Get-AutopilotCapabilityStatus -Capability $capability -Provider $CapabilityProvider
    if ([string]$job.executionMode -eq 'live') {
        throw 'Live oracle intents are not implemented: no deployed Oracle dispatch surface exists. The job must stay dry-run until Sol ships one.'
    }
    $canonical = ConvertTo-AutopilotCanonicalJson $Payload
    $idempotencyKey = Get-AutopilotSha256 -Text ('{0}|{1}|{2}|{3}' -f $job.jobId, $job.phase, $Operation, $canonical)
    $writeParams = @{
        Paths = $Paths; JobId = $job.jobId; Event = 'command'
        Data  = @{
            command = @{
                surface              = 'oracle'
                capability           = $capability
                capability_available = $status.available
                capability_source    = $status.source
                operation            = $Operation
                payload              = $Payload
                mode                 = 'dry-run'
                sent                 = $false
                would_send           = $true
                irreversible         = $false
                idempotency_key      = $idempotencyKey
            }
        }
    }
    if ($PSBoundParameters.ContainsKey('Now')) { $writeParams['Now'] = $Now }
    $null = Write-AutopilotReceipt @writeParams
    return [pscustomobject]@{ Sent = $false; WouldSend = $true; Operation = $Operation; IdempotencyKey = $idempotencyKey; CapabilityAvailable = $status.available }
}

function Test-AutopilotJobPrerequisites {
    # Evaluates only prerequisites the controller can verify from its own state
    # today: 'job-completed'. Everything else is reported unverifiable (the
    # campaign planner cross-checks those against roster facts), never guessed.
    param([Parameter(Mandatory)]$Paths, [Parameter(Mandatory)]$Job)
    $job = ConvertTo-AutopilotHashtable $Job
    $unmet = @()
    $unverifiable = @()
    foreach ($prerequisite in @($job.prerequisites)) {
        $kind = [string]$prerequisite.kind
        if ($kind -eq 'job-completed') {
            $dependencyId = [string]$prerequisite.params.jobId
            $dependency = Get-AutopilotJob -Paths $Paths -JobId $dependencyId
            if (-not $dependency -or [string]$dependency.phase -ne 'Completed') {
                $unmet += [pscustomobject]@{ kind = $kind; jobId = $dependencyId; phase = $(if ($dependency) { [string]$dependency.phase } else { 'missing' }) }
            }
        }
        else {
            $unverifiable += [pscustomobject]@{ kind = $kind; params = $prerequisite.params }
        }
    }
    [pscustomobject]@{
        Met          = ($unmet.Count -eq 0)
        Unmet        = @($unmet)
        Unverifiable = @($unverifiable)
    }
}

# ---------------------------------------------------------------------------
# V1.3 job lifecycle: wall-clock expiry, honest successors, resume diagnostics,
# suspension that always releases leases. A terminal/expired job is historical
# evidence - it is never edited, its ledger is never appended to.
# ---------------------------------------------------------------------------

function Test-AutopilotJobWallClockExpired {
    param([Parameter(Mandatory)]$Job, [datetime]$Now)
    $job = ConvertTo-AutopilotHashtable $Job
    if (-not $job.startedAt) { return $false }
    $utc = if ($PSBoundParameters.ContainsKey('Now')) { Get-AutopilotUtcNow -Now $Now } else { Get-AutopilotUtcNow }
    $elapsedMinutes = ($utc - (ConvertTo-AutopilotUtcDateTime $job.startedAt)).TotalMinutes
    return ($elapsedMinutes -gt [double][int64]$job.timeoutPolicy.jobTimeoutMinutes)
}

function Test-AutopilotJobSuccessorEligible {
    # A successor may be created only from a job that is genuinely finished:
    # terminal, timed out / recovery-exhausted, cancellation-requested, or
    # wall-clock expired. Anything still runnable must be cancelled first -
    # two live jobs for one intent would be a duplicate mutator.
    param([Parameter(Mandatory)]$Job, [datetime]$Now)
    $job = ConvertTo-AutopilotHashtable $Job
    $nowParams = @{}
    if ($PSBoundParameters.ContainsKey('Now')) { $nowParams['Now'] = $Now }
    if ($job.phase -in $script:AutopilotTerminalPhases) { return [pscustomobject]@{ Eligible = $true; Reason = "phase $($job.phase) is terminal" } }
    if ($job.phase -eq 'Blocked' -and $job.blockedReason -and [string]$job.blockedReason.code -in @('job-timeout', 'phase-timeout', 'recovery-budget-exhausted')) {
        return [pscustomobject]@{ Eligible = $true; Reason = "blocked terminal-equivalent: $($job.blockedReason.code)" }
    }
    if ($job.cancellation.requested) { return [pscustomobject]@{ Eligible = $true; Reason = 'cancellation requested' } }
    if (Test-AutopilotJobWallClockExpired -Job $job @nowParams) { return [pscustomobject]@{ Eligible = $true; Reason = 'job wall clock expired' } }
    return [pscustomobject]@{ Eligible = $false; Reason = "job is still runnable (phase $($job.phase)); cancel it before creating a successor" }
}

function New-AutopilotSuccessorJob {
    <#
    Creates a fresh retry/successor job from a finished historical job. The
    successor inherits the goal (kind/spec/success criteria/policies/mode) and
    carries provenance (supersedesJobId, retryReason), but receives a NEW job
    id, a distinct idempotency namespace, fresh creation/start clocks, fresh
    retry/recovery budgets, and its own receipt ledger. The ORIGINAL job
    document and ledger are never modified - not even appended to.
    #>
    param(
        [Parameter(Mandatory)]$Paths,
        [Parameter(Mandatory)][string]$JobId,
        [string]$RetryReason = 'operator retry of a finished job',
        [datetime]$Now
    )
    $original = Get-AutopilotJob -Paths $Paths -JobId $JobId
    if (-not $original) { throw "No job $JobId." }
    $nowParams = @{}
    if ($PSBoundParameters.ContainsKey('Now')) { $nowParams['Now'] = $Now }
    $eligibility = Test-AutopilotJobSuccessorEligible -Job $original @nowParams
    if (-not $eligibility.Eligible) { throw "Successor refused: $($eligibility.Reason)." }

    $utc = if ($PSBoundParameters.ContainsKey('Now')) { Get-AutopilotUtcNow -Now $Now } else { Get-AutopilotUtcNow }
    # Per-item conversion: converting the whole array would nest it (comma-return).
    $inheritedPrerequisites = @()
    foreach ($prerequisite in @($original.prerequisites)) { $inheritedPrerequisites += , (ConvertTo-AutopilotHashtable $prerequisite) }
    $jobParams = @{
        Kind                  = [string]$original.kind
        Spec                  = (ConvertTo-AutopilotHashtable $original.spec)
        EligibleGuids         = @(@($original.characters.eligibleGuids) | ForEach-Object { [uint32]$_ })
        SuccessCriteria       = (ConvertTo-AutopilotHashtable $original.successCriteria)
        Campaign              = [string]$original.owner.campaign
        RequestedBy           = 'successor-retry'
        Priority              = [int]$original.priority
        Prerequisites         = $inheritedPrerequisites
        TimeoutPolicy         = (ConvertTo-AutopilotHashtable $original.timeoutPolicy)
        RetryPolicy           = (ConvertTo-AutopilotHashtable $original.retryPolicy)
        ExecutionMode         = [string]$original.executionMode
        Paths                 = $Paths
        IdempotencyNamespace  = "supersedes:$($original.jobId)"
        Now                   = $utc
    }
    if ($original.owner.Contains('team') -and $original.owner.team) { $jobParams['Team'] = [string]$original.owner.team }
    $successor = New-AutopilotJob @jobParams
    $successor.provenance = [ordered]@{
        supersedesJobId = [string]$original.jobId
        retryReason     = $RetryReason
        retriedAt       = ConvertTo-AutopilotTimestamp -Time $utc
    }
    if ($original.Contains('metadata') -and $original.metadata -and $original.metadata.Contains('profile')) {
        if (-not $successor.Contains('metadata') -or $null -eq $successor.metadata) { $successor.metadata = [ordered]@{} }
        $successor.metadata.profile = [string]$original.metadata.profile
    }
    $saved = Add-AutopilotJob -Paths $Paths -Job $successor
    $null = Write-AutopilotReceipt -Paths $Paths -JobId $saved.jobId -Event 'job_retried_from' -Data @{
        supersedes_job_id = [string]$original.jobId
        retry_reason      = $RetryReason
        original_phase    = [string]$original.phase
        original_blocked  = $(if ($original.blockedReason) { [string]$original.blockedReason.code } else { $null })
        eligibility       = [string]$eligibility.Reason
    } @nowParams
    return $saved
}

function Invoke-AutopilotResume {
    # Resume remains resume: it never resets clocks or budgets. Resuming a job
    # whose wall clock has already expired is refused with a diagnostic that
    # recommends the successor operation instead of silently re-running into
    # an immediate job-timeout.
    param(
        [Parameter(Mandatory)]$Paths,
        [Parameter(Mandatory)][string]$JobId,
        [datetime]$Now
    )
    $job = Get-AutopilotJob -Paths $Paths -JobId $JobId
    if (-not $job) { throw "No job $JobId." }
    if ($job.phase -ne 'Suspended') { throw "Job $JobId is not Suspended (phase: $($job.phase))." }
    $nowParams = @{}
    if ($PSBoundParameters.ContainsKey('Now')) { $nowParams['Now'] = $Now }
    if (Test-AutopilotJobWallClockExpired -Job $job @nowParams) {
        $diagnostic = ('resume refused: job {0} started {1} and its jobTimeoutMinutes={2} wall clock has expired; resuming would only produce an immediate job-timeout. Create a fresh successor instead: retry -JobId {0} (provenance preserved, clocks and budgets reset).' -f $job.jobId, $job.startedAt, $job.timeoutPolicy.jobTimeoutMinutes)
        $null = Write-AutopilotReceipt -Paths $Paths -JobId $job.jobId -Event 'resume_refused_expired' -Data @{
            started_at          = [string]$job.startedAt
            job_timeout_minutes = [int64]$job.timeoutPolicy.jobTimeoutMinutes
            recommendation      = "retry -JobId $($job.jobId)"
        } @nowParams
        return [pscustomobject]@{ Resumed = $false; Diagnostic = $diagnostic; Job = $job }
    }
    $resumeTo = [string]$job.metadata.suspendedFromPhase
    $job = Invoke-AutopilotTransition -Paths $Paths -Job $job -To $resumeTo -Reason 'operator resume' @nowParams
    return [pscustomobject]@{ Resumed = $true; Diagnostic = $null; Job = $job }
}

function Suspend-AutopilotJob {
    # The single suspension path: transition + guaranteed lease release.
    param(
        [Parameter(Mandatory)]$Paths,
        [Parameter(Mandatory)][string]$JobId,
        [string]$Reason = 'operator pause',
        [datetime]$Now
    )
    $job = Get-AutopilotJob -Paths $Paths -JobId $JobId
    if (-not $job) { throw "No job $JobId." }
    $nowParams = @{}
    if ($PSBoundParameters.ContainsKey('Now')) { $nowParams['Now'] = $Now }
    $job = Invoke-AutopilotTransition -Paths $Paths -Job $job -To 'Suspended' -Reason $Reason @nowParams
    $null = Release-AutopilotOwnership -Paths $Paths -JobId $job.jobId -Reason $Reason
    return $job
}

function Release-AutopilotAllOwnership {
    # Graceful-shutdown helper: releases every ACTIVE lease held by this
    # controller instance (all jobs). Foreign leases are never touched.
    param([Parameter(Mandatory)]$Paths, [string]$Reason = 'controller shutdown')
    $ownership = Read-AutopilotOwnership -Paths $Paths
    if (-not $ownership.usable) { return @() }
    $instanceId = Get-AutopilotControllerInstanceId -Paths $Paths
    $jobIds = @($ownership.leases | Where-Object {
            [string]$_.controllerInstanceId -eq $instanceId -and [string]$_.state -eq 'active'
        } | ForEach-Object { [string]$_.jobId } | Sort-Object -Unique)
    $released = @()
    foreach ($jobId in $jobIds) {
        if (Release-AutopilotOwnership -Paths $Paths -JobId $jobId -Reason $Reason) { $released += $jobId }
    }
    return , @($released)
}
