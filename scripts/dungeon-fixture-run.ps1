[CmdletBinding()]
param(
    [ValidateSet('Plan', 'Execute', 'Evaluate', 'Teardown')]
    [string]$Action = 'Plan',

    [string]$FixtureId = 'awdk1',

    [ValidateRange(68, 80)]
    [int]$Level = 70,

    [ValidateSet('common', 'uncommon', 'rare', 'epic', 'legendary')]
    [string]$GearQuality = 'rare',

    [switch]$Execute,

    [string]$EvidencePath,

    [string]$ReceiptPath,

    [string]$OutputPath,

    [string]$GapReportPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'dungeon-fixture-lib.ps1')

if ($Execute -and $Action -notin @('Execute', 'Teardown')) {
    throw '-Execute is valid only with -Action Execute or -Action Teardown.'
}
if ($Action -eq 'Execute' -and -not $Execute) {
    throw 'DNG-EXEC-REQUIRES-EXPLICIT: use -Action Execute -Execute. No mutation is attempted without both.'
}
if ($Action -eq 'Evaluate' -and [string]::IsNullOrWhiteSpace($EvidencePath)) {
    throw '-EvidencePath is required for -Action Evaluate.'
}

$definition = Get-DungeonFixtureDefinition -FixtureId $FixtureId -Level $Level -GearQuality $GearQuality
$provisionPlan = Get-DungeonFixtureProvisionPlan -Definition $definition
$teardownPlan = Get-DungeonFixtureTeardownPlan -Definition $definition

if ([string]::IsNullOrWhiteSpace($ReceiptPath)) {
    $ReceiptPath = Get-DungeonFixtureDefaultReceiptPath -FixtureId $FixtureId
}

function Write-DungeonFixturePlannedCommands {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][psobject]$Definition,
        [Parameter(Mandatory = $true)][psobject]$Plan
    )

    Write-DungeonFixtureReceipt -Path $Path -FixtureId $Definition.fixture_id -Event 'plan_created' -Data ([pscustomobject][ordered]@{
            plan_kind = $Plan.plan_kind
            execute_allowed_now = $Plan.execute_allowed_now
            operation_count = @($Plan.operations).Count
            read_only_query_count = @($Plan.read_only_db_queries).Count
        }) | Out-Null

    foreach ($operation in @($Plan.operations)) {
        Write-DungeonFixtureReceipt -Path $Path -FixtureId $Definition.fixture_id -Event 'command_planned' -Data ([pscustomobject][ordered]@{
                sequence = $operation.sequence
                stage_id = $operation.stage_id
                surface = $operation.surface
                action = $operation.action
                command = $operation.command
                mutation = $operation.mutation
                supported = $operation.supported
                scope = $operation.scope
                reason = $operation.reason
            }) | Out-Null
    }
}

function Write-DungeonFixtureOutput {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][psobject]$Document,
        [string]$Path
    )

    $json = $Document | ConvertTo-Json -Depth 50
    if (-not [string]::IsNullOrWhiteSpace($Path)) {
        $parent = Split-Path -Parent $Path
        if ($parent) {
            New-Item -ItemType Directory -Path $parent -Force | Out-Null
        }
        $utf8NoBom = [System.Text.UTF8Encoding]::new($false)
        [System.IO.File]::WriteAllText($Path, $json, $utf8NoBom)
    }
    Write-Output $json
}

switch ($Action) {
    'Plan' {
        Write-DungeonFixtureReceipt -Path $ReceiptPath -FixtureId $definition.fixture_id -Event 'run_started' -Data ([pscustomobject]@{ mode = 'dry-run'; action = $Action }) | Out-Null
        Write-DungeonFixturePlannedCommands -Path $ReceiptPath -Definition $definition -Plan $provisionPlan
        Write-DungeonFixtureReceipt -Path $ReceiptPath -FixtureId $definition.fixture_id -Event 'run_completed' -Data ([pscustomobject]@{ status = 'PLAN_ONLY'; action = $Action }) | Out-Null
        $document = [pscustomobject][ordered]@{
            schema = $script:DungeonFixturePlanSchema
            schema_version = 1
            action = $Action
            receipt_path = $ReceiptPath
            definition = $definition
            plan = $provisionPlan
        }
        Write-DungeonFixtureOutput -Document $document -Path $OutputPath
        break
    }

    'Execute' {
        Write-DungeonFixtureReceipt -Path $ReceiptPath -FixtureId $definition.fixture_id -Event 'run_started' -Data ([pscustomobject]@{ mode = 'explicit-execute'; action = $Action }) | Out-Null
        Write-DungeonFixturePlannedCommands -Path $ReceiptPath -Definition $definition -Plan $provisionPlan
        if ([string]::IsNullOrWhiteSpace($GapReportPath)) {
            $GapReportPath = [System.IO.Path]::ChangeExtension($ReceiptPath, '.gap.json')
        }
        $gapDocument = [pscustomobject][ordered]@{
            schema = 'autowow.dungeon.fixture.gap.v1'
            schema_version = 1
            fixture_id = $definition.fixture_id
            execute_allowed_now = $provisionPlan.execute_allowed_now
            blockers = @($provisionPlan.gap_report)
        }
        Write-DungeonFixtureOutput -Document $gapDocument -Path $GapReportPath | Out-Null
        Write-DungeonFixtureReceipt -Path $ReceiptPath -FixtureId $definition.fixture_id -Event 'execution_blocked' -Data ([pscustomobject][ordered]@{
                code = 'DNG-EXEC-BLOCKED'
                gap_report_path = $GapReportPath
                blocker_ids = @($provisionPlan.gap_report | Where-Object severity -eq 'blocker' | ForEach-Object id)
            }) | Out-Null
        Assert-DungeonFixtureExecutionAllowed -Plan $provisionPlan -GapReportPath $GapReportPath
        throw 'DNG-EXEC-ADAPTER-MISSING: no live command adapter is installed for this fixture lane.'
    }

    'Evaluate' {
        if (-not (Test-Path -LiteralPath $EvidencePath -PathType Leaf)) {
            throw "Evidence file not found: $EvidencePath"
        }
        $evidence = Get-Content -LiteralPath $EvidencePath -Raw | ConvertFrom-Json -Depth 50
        Write-DungeonFixtureReceipt -Path $ReceiptPath -FixtureId $definition.fixture_id -Event 'evaluation_started' -Data ([pscustomobject]@{ evidence_path = $EvidencePath }) | Out-Null
        $gates = Get-DungeonFixtureGateResults -Evidence $evidence -Definition $definition
        Write-DungeonFixtureReceipt -Path $ReceiptPath -FixtureId $definition.fixture_id -Event 'evaluation_completed' -Data ([pscustomobject]@{ status = $gates.status; next_stage = $gates.next_stage; gate_count = @($gates.gates).Count }) | Out-Null
        $document = [pscustomobject][ordered]@{
            schema = $script:DungeonFixtureAcceptanceSchema
            schema_version = 1
            action = $Action
            receipt_path = $ReceiptPath
            evidence_path = $EvidencePath
            definition = $definition
            acceptance = $gates
        }
        Write-DungeonFixtureOutput -Document $document -Path $OutputPath
        break
    }

    'Teardown' {
        Write-DungeonFixtureReceipt -Path $ReceiptPath -FixtureId $definition.fixture_id -Event 'teardown_started' -Data ([pscustomobject]@{ mode = if ($Execute) { 'explicit-execute' } else { 'dry-run' }; action = $Action }) | Out-Null
        Write-DungeonFixturePlannedCommands -Path $ReceiptPath -Definition $definition -Plan $teardownPlan
        if (-not $Execute) {
            Write-DungeonFixtureReceipt -Path $ReceiptPath -FixtureId $definition.fixture_id -Event 'teardown_completed' -Data ([pscustomobject]@{ status = 'PLAN_ONLY' }) | Out-Null
            $document = [pscustomobject][ordered]@{
                schema = $script:DungeonFixturePlanSchema
                schema_version = 1
                action = $Action
                receipt_path = $ReceiptPath
                definition = $definition
                plan = $teardownPlan
            }
            Write-DungeonFixtureOutput -Document $document -Path $OutputPath
            break
        }

        if ([string]::IsNullOrWhiteSpace($GapReportPath)) {
            $GapReportPath = [System.IO.Path]::ChangeExtension($ReceiptPath, '.teardown-gap.json')
        }
        $gapDocument = [pscustomobject][ordered]@{
            schema = 'autowow.dungeon.fixture.gap.v1'
            schema_version = 1
            fixture_id = $definition.fixture_id
            execute_allowed_now = $teardownPlan.execute_allowed_now
            blockers = @($teardownPlan.gap_report)
        }
        Write-DungeonFixtureOutput -Document $gapDocument -Path $GapReportPath | Out-Null
        Write-DungeonFixtureReceipt -Path $ReceiptPath -FixtureId $definition.fixture_id -Event 'teardown_blocked' -Data ([pscustomobject][ordered]@{
                code = 'DNG-EXEC-BLOCKED'
                gap_report_path = $GapReportPath
                blocker_ids = @($teardownPlan.gap_report | ForEach-Object id)
            }) | Out-Null
        throw "DNG-EXEC-BLOCKED: safe owner-aware teardown is not available. Gap report: $GapReportPath"
    }
}

