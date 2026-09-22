<#!
.SYNOPSIS
    Builds an offline plan, or explicitly applies, an exact-GUID Oracle comparison fixture.

.DESCRIPTION
    The default mode is plan-only and never contacts the bridge. Apply mode is intentionally
    double-gated and uses only the existing fixture-init/factory bridge command. It performs
    read-only identity/status/telemetry checks before and after each deterministic operation.
    It never provisions accounts, edits configuration, runs SQL, starts/stops/builds a server,
    or performs gear/talent ablation.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ManifestPath,
    [ValidateSet(5, 10, 25, 40)][int]$Size = 5,
    [ValidateSet('127.0.0.1')][string]$BridgeHost = '127.0.0.1',
    [ValidateRange(1, 65535)][int]$Port = 18787,
    [ValidateRange(1000, 120000)][int]$TimeoutMs = 5000,
    [string]$EvidenceDirectory = '',
    [switch]$Apply,
    [switch]$AllowFixtureFactoryMutation
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$moduleRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..') -ErrorAction Stop).Path
$libraryPath = Join-Path $PSScriptRoot 'oracle-gear-fixture-lib.ps1'
if (-not (Test-Path -LiteralPath $libraryPath)) { throw "Missing Oracle fixture library: $libraryPath" }
. $libraryPath

function Assert-OraclePathUnderModule {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][string]$Label)

    $full = [System.IO.Path]::GetFullPath($Path)
    $prefix = $moduleRoot.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    if (-not $full.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "$Label must stay inside the isolated module worktree: $full"
    }
    return $full
}

$manifestFullPath = Assert-OraclePathUnderModule -Path ((Resolve-Path -LiteralPath $ManifestPath -ErrorAction Stop).Path) -Label 'ManifestPath'
$manifest = Read-OracleGearFixtureManifest -Path $manifestFullPath
$members = @(Get-OracleGearProfileMembers -Manifest $manifest -Size $Size)
$ablation = Get-OracleProperty $manifest 'ablation_matrix'
if ($null -eq $ablation) { $ablation = Get-OracleGearAblationMatrix }
Assert-OracleGearAblationMatrix -Matrix $ablation | Out-Null

if ($Apply -and -not $AllowFixtureFactoryMutation) {
    throw 'Apply requires the explicit -AllowFixtureFactoryMutation guard.'
}

if ([string]::IsNullOrWhiteSpace($EvidenceDirectory)) {
    $EvidenceDirectory = Join-Path $moduleRoot 'logs\oracle-gear-fixture'
}
$evidenceDirectoryFull = Assert-OraclePathUnderModule -Path ([System.IO.Path]::GetFullPath($EvidenceDirectory)) -Label 'EvidenceDirectory'
New-Item -ItemType Directory -Path $evidenceDirectoryFull -Force | Out-Null
$runId = "ORACLE-GEAR-$([int]$Size)-$(Get-Date -Format 'yyyyMMdd-HHmmss')-$([guid]::NewGuid().ToString('N').Substring(0, 8))"
$receiptPath = Join-Path $evidenceDirectoryFull "$runId.jsonl"
$summaryJsonPath = Join-Path $evidenceDirectoryFull "$runId-summary.json"
$summaryMarkdownPath = Join-Path $evidenceDirectoryFull "$runId-summary.md"

function Write-OracleEvidence {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Event, [Parameter(Mandatory)][object]$Body)

    $record = [ordered]@{
        schema = 'autowow.oracle.gear-fixture.evidence.v1'
        run_id = $runId
        timestamp_utc = [datetime]::UtcNow.ToString('o')
        event = $Event
        body = $Body
    }
    Add-Content -LiteralPath $receiptPath -Value ($record | ConvertTo-Json -Depth 40 -Compress)
}

function Get-OracleBridgeJson {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$WireRequest)

    if ($WireRequest -notmatch '^(fixture (status|init)|snapshot|combatlog)\s') {
        throw "Oracle lane attempted an out-of-scope bridge request: $WireRequest"
    }
    $client = [System.Net.Sockets.TcpClient]::new()
    try {
        $connect = $client.ConnectAsync($BridgeHost, $Port)
        if (-not $connect.Wait([Math]::Min($TimeoutMs, 10000)) -or -not $client.Connected) {
            throw "Could not connect to the loopback AutoWow bridge at ${BridgeHost}:$Port."
        }
        $stream = $client.GetStream()
        $stream.ReadTimeout = $TimeoutMs
        $writer = [System.IO.StreamWriter]::new($stream)
        $writer.NewLine = "`n"
        $writer.WriteLine($WireRequest)
        $writer.Flush()
        $reader = [System.IO.StreamReader]::new($stream)
        $line = $reader.ReadLine()
        if ([string]::IsNullOrWhiteSpace($line)) { throw "Bridge returned no JSON for $WireRequest." }
        $response = $line | ConvertFrom-Json
        if ($null -eq $response) { throw "Bridge returned malformed JSON for $WireRequest." }
        return $response
    }
    finally {
        $client.Dispose()
    }
}

function Get-OracleMemberObservation {
    [CmdletBinding()]
    param([Parameter(Mandatory)][psobject]$Member)

    $guid = [uint32](Get-OracleProperty $Member 'guid')
    $status = Get-OracleBridgeJson -WireRequest "fixture status $guid"
    $snapshot = Get-OracleBridgeJson -WireRequest "snapshot $guid"
    $combat = Get-OracleBridgeJson -WireRequest "combatlog $guid"
    return [pscustomobject][ordered]@{ status = $status; snapshot = $snapshot; combat = $combat }
}

function Get-OracleIdentityResult {
    [CmdletBinding()]
    param([Parameter(Mandatory)][psobject]$Member, [Parameter(Mandatory)][psobject]$Observation)
    return Test-OracleFixtureIdentity -Member $Member -Status $Observation.status
}

function New-OracleSummaryMarkdown {
    [CmdletBinding()]
    param([Parameter(Mandatory)][psobject]$Summary)

    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add('# AutoWoW Oracle adequate-gear fixture run')
    $lines.Add('')
    $lines.Add("- Run: $($Summary.run_id)")
    $lines.Add("- Profile size: $($Summary.profile_size)")
    $lines.Add("- Status: $($Summary.status)")
    $lines.Add("- Apply requested: $($Summary.apply_requested)")
    $lines.Add("- Live mutation calls: $($Summary.live_mutation_calls)")
    $lines.Add("- Direct SQL calls: $($Summary.direct_sql_calls)")
    $lines.Add('')
    $lines.Add('## Scope')
    $lines.Add('')
    $lines.Add('- Exact GUID, account, name, class, faction, and spec are manifest-scoped.')
    $lines.Add('- Loadout writes, when explicitly enabled, use `fixture init` → `PlayerbotFactory::InitializeFixture`.')
    $lines.Add('- Status, snapshot, and combat counters are read-only evidence.')
    $lines.Add('- Gear/talent ablation is a plan-only interface; no destructive stripping is implemented.')
    $lines.Add('')
    $lines.Add('## Members')
    $lines.Add('')
    $lines.Add('| GUID | Name | Role | Before | After |')
    $lines.Add('|---:|---|---|---|---|')
    foreach ($row in @($Summary.members)) {
        $lines.Add("| $($row.guid) | $($row.name) | $($row.role) | $($row.before_status) | $($row.after_status) |")
    }
    return ($lines -join [Environment]::NewLine)
}

$summary = [ordered]@{
    schema = 'autowow.oracle.gear-fixture.run.v1'
    schema_version = 1
    run_id = $runId
    fixture_id = [string](Get-OracleProperty $manifest 'fixture_id')
    profile_size = $Size
    started_at_utc = [datetime]::UtcNow.ToString('o')
    completed_at_utc = $null
    status = 'PLAN_ONLY'
    apply_requested = [bool]$Apply
    live_mutation_calls = 0
    direct_sql_calls = 0
    deployment_or_client_changes = 0
    receipt_path = $receiptPath
    summary_json_path = $summaryJsonPath
    summary_markdown_path = $summaryMarkdownPath
    ablation = $ablation
    members = @()
    errors = @()
}

try {
    Write-OracleEvidence -Event 'run_started' -Body ([ordered]@{
        fixture_id = $summary.fixture_id
        profile_size = $Size
        manifest_path = $manifestFullPath
        apply = [bool]$Apply
        allowed_mutation = [bool]$AllowFixtureFactoryMutation
        direct_sql = $false
        ablation_mutation_enabled = [bool](Get-OracleProperty $ablation 'mutation_enabled')
    })

    $preflightByGuid = @{}
    if ($Apply) {
        # Complete the read-only identity pass before the first factory call. This keeps a bad
        # later manifest row from producing a partially normalized comparison roster.
        foreach ($preflightMember in $members) {
            $preflightGuid = [uint32](Get-OracleProperty $preflightMember 'guid')
            $preflight = Get-OracleMemberObservation -Member $preflightMember
            Write-OracleEvidence -Event 'preflight_status' -Body $preflight.status
            Write-OracleEvidence -Event 'preflight_snapshot' -Body $preflight.snapshot
            Write-OracleEvidence -Event 'preflight_combat' -Body $preflight.combat
            $preflightIdentity = Get-OracleIdentityResult -Member $preflightMember -Observation $preflight
            if ($preflightIdentity.status -ne 'PASS') {
                Write-OracleEvidence -Event 'preflight_identity_failed' -Body ([ordered]@{
                    guid = $preflightGuid
                    reasons = @($preflightIdentity.reasons)
                })
                throw "Fail-closed identity check for GUID ${preflightGuid}: $($preflightIdentity.reasons -join ', ')"
            }
            $preflightByGuid[[string]$preflightGuid] = $preflight
        }
    }

    foreach ($member in $members) {
        $guid = [uint32](Get-OracleProperty $member 'guid')
        $row = [ordered]@{
            guid = $guid
            name = [string](Get-OracleProperty $member 'name')
            role = [string](Get-OracleProperty $member 'role_category')
            before_status = 'NOT_RUN'
            after_status = 'NOT_RUN'
            mutation = 'not_run'
            reasons = @()
        }

        if (-not $Apply) {
            $row.mutation = 'plan_only'
            $row.before_status = 'NOT_RUN'
            $row.after_status = 'NOT_RUN'
            Write-OracleEvidence -Event 'fixture_plan' -Body ([ordered]@{
                guid = $guid
                name = $row.name
                role = $row.role
                init_request = Get-OracleFixtureInitRequest -Member $member -Manifest $manifest
                status_request = "fixture status $guid"
                snapshot_request = "snapshot $guid"
                combat_request = "combatlog $guid"
            })
            $summary.members += [pscustomobject]$row
            continue
        }

        $before = $preflightByGuid[[string]$guid]
        $row.before_status = (Test-OracleFixtureEvidence -Member $member -Manifest $manifest -Status $before.status -Snapshot $before.snapshot -Combat $before.combat).status
        if ($row.before_status -eq 'PASS') {
            $row.mutation = 'skipped_already_normalized'
        }
        else {
            $request = Get-OracleFixtureInitRequest -Member $member -Manifest $manifest
            $init = Get-OracleBridgeJson -WireRequest $request
            $summary.live_mutation_calls++
            Write-OracleEvidence -Event 'fixture_init' -Body $init
            if (-not [bool](Get-OracleProperty $init 'ok') -or
                [string](Get-OracleProperty (Get-OracleProperty $init 'mutation') 'factory') -ne 'PlayerbotFactory::InitializeFixture') {
                throw "Fixture factory refused or did not identify the approved factory for GUID $guid."
            }
            $row.mutation = 'factory_applied'
        }

        $after = Get-OracleMemberObservation -Member $member
        Write-OracleEvidence -Event 'postflight_status' -Body $after.status
        Write-OracleEvidence -Event 'postflight_snapshot' -Body $after.snapshot
        Write-OracleEvidence -Event 'postflight_combat' -Body $after.combat
        $verification = Test-OracleFixtureEvidence -Member $member -Manifest $manifest -Status $after.status -Snapshot $after.snapshot -Combat $after.combat
        $row.after_status = $verification.status
        $row.reasons = @($verification.reasons)
        if ($verification.status -ne 'PASS') { throw "Postflight fixture verification failed for GUID ${guid}: $($verification.reasons -join ', ')" }
        $summary.members += [pscustomobject]$row
    }

    $summary.status = if ($Apply) { 'PASS' } else { 'PLAN_ONLY' }
    Write-OracleEvidence -Event 'run_completed' -Body ([ordered]@{ status = $summary.status; members = @($summary.members).Count; live_mutation_calls = $summary.live_mutation_calls; direct_sql_calls = $summary.direct_sql_calls })
}
catch {
    $summary.status = 'FAIL'
    $summary.errors = @($summary.errors) + $_.Exception.Message
    Write-OracleEvidence -Event 'run_failed' -Body ([ordered]@{ error = $_.Exception.Message })
}
finally {
    $summary.completed_at_utc = [datetime]::UtcNow.ToString('o')
    $summaryObject = [pscustomobject]$summary
    [System.IO.File]::WriteAllText($summaryJsonPath, ($summaryObject | ConvertTo-Json -Depth 40), [System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::WriteAllText($summaryMarkdownPath, (New-OracleSummaryMarkdown -Summary $summaryObject), [System.Text.UTF8Encoding]::new($false))
}

$summaryObject | ConvertTo-Json -Depth 40
if ($summary.status -eq 'FAIL') { exit 1 }
