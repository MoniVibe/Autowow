<#
.SYNOPSIS
    Runs one deterministic AutoWow micro-scenario manifest.

.DESCRIPTION
    The default action is a side-effect-free plan. Live bridge execution requires both
    -Apply and -Execute. The runner only emits operations supported by autowow-control.ps1,
    never invents a wire verb, and records every step in an append-only JSONL receipt.
#>
[CmdletBinding()]
param(
    [ValidateSet('plan', 'run', 'status')][string]$Action = 'plan',
    [Parameter(Mandatory = $true)][string]$ManifestPath,
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$RunId = '',
    [string]$ReceiptPath = '',
    [switch]$Apply,
    [switch]$Execute,
    [switch]$Resume
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'micro-scenario-lib.ps1')

$ServerRoot = [System.IO.Path]::GetFullPath($ServerRoot)
$ManifestPath = [System.IO.Path]::GetFullPath((Resolve-Path -LiteralPath $ManifestPath -ErrorAction Stop).Path)
$controlPath = Join-Path $ServerRoot 'scripts\autowow-control.ps1'
if (-not (Test-Path -LiteralPath $controlPath -PathType Leaf)) { throw "AutoWow control script is missing: $controlPath" }

$manifest = Read-MicroScenarioManifest -Path $ManifestPath
$manifestHash = Get-MicroScenarioManifestHash -Path $ManifestPath
$scenarioId = [string]$manifest.scenario_id
if ([string]::IsNullOrWhiteSpace($RunId)) { $RunId = [string]$manifest.run_id }
if ([string]::IsNullOrWhiteSpace($RunId)) {
    $RunId = '{0}-{1}' -f $scenarioId, [DateTimeOffset]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
}
if (-not (Test-MicroScenarioSafeIdentifier -Value $RunId)) { throw "Unsafe micro-scenario run id: $RunId" }

if ([string]::IsNullOrWhiteSpace($ReceiptPath)) {
    $ReceiptPath = Join-Path $ServerRoot ("logs\micro-scenarios\{0}\{1}.jsonl" -f $scenarioId, $RunId)
}
$ReceiptPath = [System.IO.Path]::GetFullPath($ReceiptPath)
$receipts = @(Read-MicroScenarioReceipts -Path $ReceiptPath)
Assert-MicroScenarioReceiptIdentity -Receipts $receipts -RunId $RunId -ScenarioId $scenarioId -ManifestHash $manifestHash

function ConvertTo-ControlStep {
    param([Parameter(Mandatory = $true)][object]$Step)

    return [pscustomobject][ordered]@{
        id = [string]$Step.step_id
        phase = [string]$Step.phase
        operation = [string]$Step.operation
        guid = [uint32]$Step.guid
        arguments = $Step.arguments
    }
}

function Get-StepData {
    param([Parameter(Mandatory = $true)][object]$Step)

    $controlStep = ConvertTo-ControlStep -Step $Step
    $control = Get-MicroScenarioControlParameters -Step $controlStep
    $parameters = [ordered]@{}
    foreach ($entry in @($control.parameters.GetEnumerator())) {
        $parameters[$entry.Key] = $entry.Value
    }
    return [pscustomobject][ordered]@{
        operation = [string]$control.operation
        action = [string]$control.action
        mutation = [bool]$control.mutation
        parameters = $parameters
        request = Get-MicroScenarioControlRequest -Step $controlStep -ControlPath $controlPath
    }
}

function Invoke-Step {
    param([Parameter(Mandatory = $true)][object]$Step)

    $controlStep = ConvertTo-ControlStep -Step $Step
    $control = Get-MicroScenarioControlParameters -Step $controlStep
    $parameters = @{}
    foreach ($entry in @($control.parameters.GetEnumerator())) {
        $parameters[$entry.Key] = $entry.Value
    }
    $raw = @(& $controlPath @parameters | ForEach-Object { [string]$_ } |
        Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    $line = @($raw | Select-Object -Last 1)
    if ($line.Count -ne 1) { throw "Bridge returned no response for step '$($Step.step_id)'." }
    try { $response = $line[0] | ConvertFrom-Json -Depth 100 }
    catch { throw "Bridge returned invalid JSON for step '$($Step.step_id)'." }
    return [pscustomobject][ordered]@{
        control = $control
        request = Get-MicroScenarioControlRequest -Step $controlStep -ControlPath $controlPath
        response = $response
    }
}

function Invoke-StepObservations {
    param([Parameter(Mandatory = $true)][object]$Step)

    $samples = [System.Collections.Generic.List[object]]::new()
    [void]$samples.Add((Invoke-Step -Step $Step))
    $deadline = [DateTimeOffset]::UtcNow.AddSeconds([int]$Step.duration_seconds)
    while ([int]$Step.duration_seconds -gt 0 -and [DateTimeOffset]::UtcNow -lt $deadline) {
        Start-Sleep -Seconds ([int]$Step.poll_seconds)
        [void]$samples.Add((Invoke-Step -Step $Step))
    }
    return @($samples)
}

function Test-StepPostconditions {
    param(
        [Parameter(Mandatory = $true)][object]$Step,
        [Parameter(Mandatory = $true)][object]$Response
    )

    $results = [System.Collections.Generic.List[object]]::new()
    foreach ($assertion in @($Step.assertions)) {
        [void]$results.Add((Get-MicroScenarioAssertionResult -Response $Response -Assertion $assertion))
    }
    $failed = @($results | Where-Object { -not $_.passed })
    if ($failed.Count -gt 0) {
        throw "Micro-scenario assertion failed for '$($Step.step_id)': $($failed[0].path) $($failed[0].operator)."
    }
    return @($results)
}

if ($Action -eq 'status') {
    [pscustomobject][ordered]@{
        schema = $script:AutoWowMicroScenarioReceiptSchema
        scenario_id = $scenarioId
        run_id = $RunId
        manifest_sha256 = $manifestHash
        receipt = $ReceiptPath
        receipt_count = @($receipts).Count
        completed_steps = @(Get-MicroScenarioCompletedStepIds -Receipts $receipts)
        last_event = if (@($receipts).Count -gt 0) { $receipts[-1].event } else { $null }
    } | ConvertTo-Json -Depth 20
    return
}

if ($Action -eq 'plan') {
    $plannedSteps = @($manifest.steps | ForEach-Object {
        $data = Get-StepData -Step $_
        [pscustomobject][ordered]@{
            sequence = [int]$_.sequence
            step_id = [string]$_.step_id
            phase = [string]$_.phase
            operation = $data.operation
            mutation = $data.mutation
            request = $data.request
            assertions = @($_.assertions)
        }
    })
    [pscustomobject][ordered]@{
        schema = $script:AutoWowMicroScenarioPlanSchema
        schema_version = 1
        scenario_id = $scenarioId
        run_id = $RunId
        manifest_sha256 = $manifestHash
        fixture_only = $true
        dry_run = $true
        execute_required = $true
        allowed_guids = @($manifest.allowed_guids)
        steps = $plannedSteps
        receipt = $ReceiptPath
    } | ConvertTo-Json -Depth 100
    return
}

if ($Execute -and -not $Apply) { throw 'Live micro-scenario execution requires both -Apply and -Execute.' }
if (@($receipts).Count -gt 0 -and -not $Resume) {
    throw "Receipt already exists for run '$RunId'; use -Resume to continue idempotently."
}

$completed = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
foreach ($stepId in @(Get-MicroScenarioCompletedStepIds -Receipts $receipts)) {
    [void]$completed.Add([string]$stepId)
}
$sequence = 0
try {
    Write-MicroScenarioReceipt -Path $ReceiptPath -RunId $RunId -ScenarioId $scenarioId -Event 'run_started' -Status 'INFO' -Data ([ordered]@{
        manifest_sha256 = $manifestHash
        execute = [bool]$Execute
        apply = [bool]$Apply
        allowed_guids = @($manifest.allowed_guids)
    }) | Out-Null

    foreach ($step in @($manifest.steps | Sort-Object sequence)) {
        $sequence = [int]$step.sequence
        if ($completed.Contains([string]$step.step_id)) {
            Write-MicroScenarioReceipt -Path $ReceiptPath -RunId $RunId -ScenarioId $scenarioId -Event 'step_skipped' -Status 'SKIP' -Sequence $sequence -StepId $step.step_id -Phase $step.phase -Data @{ reason = 'already_completed'; manifest_sha256 = $manifestHash } | Out-Null
            continue
        }

        $stepData = Get-StepData -Step $step
        if ($stepData.mutation -and -not $Execute) {
            Write-MicroScenarioReceipt -Path $ReceiptPath -RunId $RunId -ScenarioId $scenarioId -Event 'step_planned' -Status 'PLAN' -Sequence $sequence -StepId $step.step_id -Phase $step.phase -Data ([ordered]@{ manifest_sha256 = $manifestHash; request = $stepData.request; mutation = $true }) | Out-Null
            continue
        }

        Write-MicroScenarioReceipt -Path $ReceiptPath -RunId $RunId -ScenarioId $scenarioId -Event 'step_started' -Status 'INFO' -Sequence $sequence -StepId $step.step_id -Phase $step.phase -Data ([ordered]@{ manifest_sha256 = $manifestHash; request = $stepData.request; mutation = $stepData.mutation }) | Out-Null
        $samples = if ([int]$step.duration_seconds -gt 0) { @(Invoke-StepObservations -Step $step) } else { @(Invoke-Step -Step $step) }
        $last = $samples[-1]
        if ($null -ne $last.response.PSObject.Properties['ok'] -and -not [bool]$last.response.ok) {
            throw "Bridge rejected micro-scenario step '$($step.step_id)': $($last.response.error)"
        }
        $assertions = @(Test-StepPostconditions -Step $step -Response $last.response)
        $seed = @(Get-MicroScenarioSeedForGuid -Manifest $manifest -Guid ([uint32]$step.guid))
        if ($step.operation -eq 'snapshot' -and $seed.Count -gt 0) {
            foreach ($seedEntry in $seed) { [void](Test-MicroScenarioStateSeedSnapshot -Seed $seedEntry -Response $last.response) }
        }
        Write-MicroScenarioReceipt -Path $ReceiptPath -RunId $RunId -ScenarioId $scenarioId -Event 'step_completed' -Status 'PASS' -Sequence $sequence -StepId $step.step_id -Phase $step.phase -Data ([ordered]@{
            manifest_sha256 = $manifestHash
            request = $last.request
            sample_count = @($samples).Count
            response = $last.response
            assertions = $assertions
        }) | Out-Null
        [void]$completed.Add([string]$step.step_id)
    }

    $result = Write-MicroScenarioReceipt -Path $ReceiptPath -RunId $RunId -ScenarioId $scenarioId -Event 'run_completed' -Status 'PASS' -Sequence $sequence -Data @{ manifest_sha256 = $manifestHash; completed_steps = @($completed) }
    [pscustomobject][ordered]@{ ok = $true; scenario_id = $scenarioId; run_id = $RunId; status = 'PASS'; receipt = $ReceiptPath; completed_steps = @($completed); terminal = $result.event } | ConvertTo-Json -Depth 30
}
catch {
    Write-MicroScenarioReceipt -Path $ReceiptPath -RunId $RunId -ScenarioId $scenarioId -Event 'run_failed' -Status 'FAIL' -Sequence $sequence -Data @{ manifest_sha256 = $manifestHash; error = $_.Exception.Message } | Out-Null
    throw
}
