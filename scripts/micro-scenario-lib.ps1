Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:AutoWowMicroScenarioSchema = 'autowow.micro-scenario.v1'
$script:AutoWowMicroScenarioReceiptSchema = 'autowow.micro-scenario.receipt.v1'
$script:AutoWowMicroScenarioPlanSchema = 'autowow.micro-scenario.plan.v1'

$script:AutoWowMicroScenarioPhaseOrder = [ordered]@{
    'preflight' = 0
    'activate' = 1
    'fixture-init' = 2
    'move/stage' = 3
    'operation' = 4
    'observe' = 5
    'assert' = 6
    'cleanup' = 7
}

$script:AutoWowMicroScenarioSupportedOperations = @(
    'activate',
    'fixture-init',
    'independent',
    'quest',
    'craft',
    'craft-status',
    'fixture-accelerate',
    'fixture-accelerate-off',
    'fixture-kill',
    'questobjective',
    'professioneconomy',
    'snapshot',
    'list'
)

$script:AutoWowMicroScenarioReadOnlyOperations = @(
    'craft-status',
    'questobjective',
    'professioneconomy',
    'snapshot',
    'list'
)

$script:AutoWowMicroScenarioMutationOperations = @(
    'activate',
    'fixture-init',
    'fixture-accelerate',
    'fixture-accelerate-off',
    'fixture-kill',
    'independent',
    'quest',
    'craft'
)

$script:AutoWowMicroScenarioStateSeedKinds = @('existing-character')
$script:AutoWowMicroScenarioStateSeedConditions = @('online', 'alive', 'noncombat')

function Get-MicroScenarioProperty {
    [CmdletBinding()]
    param(
        [AllowNull()][object]$InputObject,
        [Parameter(Mandatory = $true)][string]$Name,
        [AllowNull()][object]$Default = $null
    )

    if ($null -eq $InputObject) { return $Default }
    $property = $InputObject.PSObject.Properties[$Name]
    if ($null -eq $property -or $null -eq $property.Value) { return $Default }
    return $property.Value
}

function Test-MicroScenarioSafeIdentifier {
    [CmdletBinding()]
    param([AllowNull()][string]$Value)

    return (-not [string]::IsNullOrWhiteSpace($Value) -and $Value -match '^[A-Za-z0-9][A-Za-z0-9._-]{0,95}$')
}

function ConvertTo-MicroScenarioPositiveUInt32 {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][object]$Value,
        [Parameter(Mandatory = $true)][string]$FieldName
    )

    $number = 0L
    if (-not [int64]::TryParse([string]$Value, [ref]$number) -or
        $number -le 0 -or $number -gt [uint32]::MaxValue) {
        throw "Micro-scenario field '$FieldName' must be a positive uint32."
    }
    return [uint32]$number
}

function ConvertTo-MicroScenarioPositiveUInt64 {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][object]$Value,
        [Parameter(Mandatory = $true)][string]$FieldName
    )

    $number = [uint64]0
    if (-not [uint64]::TryParse([string]$Value, [ref]$number) -or $number -eq 0) {
        throw "Micro-scenario field '$FieldName' must be a positive uint64."
    }
    return [uint64]$number
}

function ConvertTo-MicroScenarioBoundedInt {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][object]$Value,
        [Parameter(Mandatory = $true)][string]$FieldName,
        [Parameter(Mandatory = $true)][int]$Minimum,
        [Parameter(Mandatory = $true)][int]$Maximum
    )

    $number = 0L
    if (-not [int64]::TryParse([string]$Value, [ref]$number) -or
        $number -lt $Minimum -or $number -gt $Maximum) {
        throw "Micro-scenario field '$FieldName' must be an integer in [$Minimum,$Maximum]."
    }
    return [int]$number
}

function Get-MicroScenarioPathTokens {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path)) { throw 'Micro-scenario assertion path cannot be empty.' }
    $tokens = @($Path -split '\.')
    if ($tokens.Count -eq 0 -or @($tokens | Where-Object { $_ -notmatch '^[A-Za-z_][A-Za-z0-9_]*(?:\[(?:0|[1-9][0-9]*)\])?$' }).Count -gt 0) {
        throw "Micro-scenario assertion path is invalid: $Path"
    }
    return $tokens
}

function Test-MicroScenarioPathExists {
    [CmdletBinding()]
    param(
        [AllowNull()][object]$InputObject,
        [Parameter(Mandatory = $true)][string]$Path
    )

    $current = $InputObject
    foreach ($token in @(Get-MicroScenarioPathTokens -Path $Path)) {
        if ($null -eq $current) { return $false }
        $name = $token
        $index = $null
        if ($token -match '^([A-Za-z_][A-Za-z0-9_]*)\[([0-9]+)\]$') {
            $name = $Matches[1]
            $index = [int]$Matches[2]
        }
        $property = $current.PSObject.Properties[$name]
        if ($null -eq $property) { return $false }
        $current = $property.Value
        if ($null -ne $index) {
            if ($current -is [string] -or $current -isnot [System.Collections.IEnumerable]) { return $false }
            $items = @($current)
            if ($index -ge $items.Count) { return $false }
            $current = $items[$index]
        }
    }
    return $true
}

function Get-MicroScenarioPathValue {
    [CmdletBinding()]
    param(
        [AllowNull()][object]$InputObject,
        [Parameter(Mandatory = $true)][string]$Path
    )

    $current = $InputObject
    foreach ($token in @(Get-MicroScenarioPathTokens -Path $Path)) {
        if ($null -eq $current) { return $null }
        $name = $token
        $index = $null
        if ($token -match '^([A-Za-z_][A-Za-z0-9_]*)\[([0-9]+)\]$') {
            $name = $Matches[1]
            $index = [int]$Matches[2]
        }
        $property = $current.PSObject.Properties[$name]
        if ($null -eq $property) { return $null }
        $current = $property.Value
        if ($null -ne $index) {
            if ($current -is [string] -or $current -isnot [System.Collections.IEnumerable]) { return $null }
            $items = @($current)
            if ($index -ge $items.Count) { return $null }
            $current = $items[$index]
        }
    }
    return $current
}

function Test-MicroScenarioBooleanValue {
    [CmdletBinding()]
    param(
        [AllowNull()][object]$Value,
        [Parameter(Mandatory = $true)][string]$FieldName
    )

    if ($Value -is [bool]) { return [bool]$Value }
    $parsed = $false
    if ([bool]::TryParse([string]$Value, [ref]$parsed)) { return $parsed }
    throw "Micro-scenario field '$FieldName' must be boolean."
}

function Test-MicroScenarioValueEqual {
    [CmdletBinding()]
    param(
        [AllowNull()][object]$Actual,
        [AllowNull()][object]$Expected
    )

    if ($null -eq $Actual -or $null -eq $Expected) { return ($null -eq $Actual -and $null -eq $Expected) }
    if ($Actual -is [bool] -or $Expected -is [bool]) {
        try { return ((Test-MicroScenarioBooleanValue -Value $Actual -FieldName 'actual') -eq (Test-MicroScenarioBooleanValue -Value $Expected -FieldName 'expected')) }
        catch { return $false }
    }
    $actualNumber = 0.0
    $expectedNumber = 0.0
    if ([double]::TryParse([string]$Actual, [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$actualNumber) -and
        [double]::TryParse([string]$Expected, [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$expectedNumber)) {
        return ($actualNumber -eq $expectedNumber)
    }
    return ([string]$Actual -ceq [string]$Expected)
}

function Get-MicroScenarioAssertionResult {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][object]$Response,
        [Parameter(Mandatory = $true)][object]$Assertion
    )

    $path = [string](Get-MicroScenarioProperty -InputObject $Assertion -Name 'path' -Default '')
    if ([string]::IsNullOrWhiteSpace($path)) { throw 'Micro-scenario assertion requires path.' }
    [void](Get-MicroScenarioPathTokens -Path $path)

    $operators = @('exists', 'equals', 'not_equals', 'contains', 'gte', 'lte')
    $present = @($operators | Where-Object { $null -ne $Assertion.PSObject.Properties[$_] })
    if ($present.Count -ne 1) { throw "Micro-scenario assertion '$path' must contain exactly one supported operator." }

    $operator = $present[0]
    $actualExists = Test-MicroScenarioPathExists -InputObject $Response -Path $path
    $actual = if ($actualExists) { Get-MicroScenarioPathValue -InputObject $Response -Path $path } else { $null }
    $expected = Get-MicroScenarioProperty -InputObject $Assertion -Name $operator
    $passed = $false

    switch ($operator) {
        'exists' {
            $passed = ($actualExists -eq (Test-MicroScenarioBooleanValue -Value $expected -FieldName "assertion '$path'.exists"))
        }
        'equals' { $passed = $actualExists -and (Test-MicroScenarioValueEqual -Actual $actual -Expected $expected) }
        'not_equals' { $passed = (-not $actualExists) -or (-not (Test-MicroScenarioValueEqual -Actual $actual -Expected $expected)) }
        'contains' {
            if (-not $actualExists) { $passed = $false }
            elseif ($actual -is [string]) { $passed = ([string]$actual).Contains([string]$expected, [System.StringComparison]::Ordinal) }
            elseif ($actual -is [System.Collections.IEnumerable]) { $passed = @($actual | Where-Object { Test-MicroScenarioValueEqual -Actual $_ -Expected $expected }).Count -gt 0 }
        }
        'gte' {
            $actualNumber = 0.0
            $expectedNumber = 0.0
            $passed = $actualExists -and
                [double]::TryParse([string]$actual, [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$actualNumber) -and
                [double]::TryParse([string]$expected, [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$expectedNumber) -and
                $actualNumber -ge $expectedNumber
        }
        'lte' {
            $actualNumber = 0.0
            $expectedNumber = 0.0
            $passed = $actualExists -and
                [double]::TryParse([string]$actual, [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$actualNumber) -and
                [double]::TryParse([string]$expected, [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$expectedNumber) -and
                $actualNumber -le $expectedNumber
        }
    }

    return [pscustomobject][ordered]@{
        path = $path
        operator = $operator
        expected = $expected
        actual = $actual
        exists = $actualExists
        passed = [bool]$passed
    }
}

function Assert-MicroScenarioResponse {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][object]$Response,
        [AllowEmptyCollection()][object[]]$Assertions = @(),
        [Parameter(Mandatory = $true)][string]$StepId
    )

    $results = @($Assertions | ForEach-Object { Get-MicroScenarioAssertionResult -Response $Response -Assertion $_ })
    $failed = @($results | Where-Object { -not $_.passed })
    if ($failed.Count -gt 0) {
        throw "Micro-scenario assertion failed for step '$StepId': $($failed[0].path) $($failed[0].operator)."
    }
    return $results
}

function Get-MicroScenarioOperationArguments {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][object]$Step)

    $arguments = Get-MicroScenarioProperty -InputObject $Step -Name 'arguments' -Default $null
    if ($null -eq $arguments) { return [pscustomobject]@{} }
    if ($arguments -is [System.Collections.IEnumerable] -and $arguments -isnot [string] -and
        $arguments -isnot [pscustomobject]) {
        throw "Micro-scenario step '$($Step.id)' arguments must be a JSON object."
    }
    return $arguments
}

function Assert-MicroScenarioNoUnknownArguments {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][object]$Arguments,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][string[]]$Allowed,
        [Parameter(Mandatory = $true)][string]$StepId
    )

    foreach ($property in @($Arguments.PSObject.Properties)) {
        if ($property.Name -notin $Allowed) {
            throw "Micro-scenario step '$StepId' has unsupported argument '$($property.Name)'."
        }
    }
}

function Get-MicroScenarioControlParameters {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][object]$Step)

    $operation = ([string]$Step.operation).ToLowerInvariant()
    $stepId = [string]$Step.id
    $guid = [uint32](Get-MicroScenarioProperty -InputObject $Step -Name 'guid' -Default 0)
    $arguments = Get-MicroScenarioOperationArguments -Step $Step
    $parameters = [ordered]@{}

    switch ($operation) {
        'list' {
            Assert-MicroScenarioNoUnknownArguments -Arguments $arguments -Allowed @() -StepId $stepId
            $parameters.Action = 'list'
        }
        'activate' {
            Assert-MicroScenarioNoUnknownArguments -Arguments $arguments -Allowed @() -StepId $stepId
            $parameters.Action = 'activate'
            $parameters.BotGuid = $guid
        }
        'fixture-init' {
            Assert-MicroScenarioNoUnknownArguments -Arguments $arguments -Allowed @('level', 'spec_index', 'quality') -StepId $stepId
            $parameters.Action = 'fixture-init'
            $parameters.BotGuid = $guid
            $parameters.Level = ConvertTo-MicroScenarioBoundedInt -Value (Get-MicroScenarioProperty $arguments 'level') -FieldName "$stepId.arguments.level" -Minimum 1 -Maximum 80
            $parameters.SpecIndex = ConvertTo-MicroScenarioBoundedInt -Value (Get-MicroScenarioProperty $arguments 'spec_index') -FieldName "$stepId.arguments.spec_index" -Minimum 0 -Maximum 19
            $parameters.Quality = ConvertTo-MicroScenarioBoundedInt -Value (Get-MicroScenarioProperty $arguments 'quality') -FieldName "$stepId.arguments.quality" -Minimum 0 -Maximum 5
        }
        'fixture-accelerate' {
            Assert-MicroScenarioNoUnknownArguments -Arguments $arguments -Allowed @('pacing_percent') -StepId $stepId
            $parameters.Action = 'fixture-accelerate'
            $parameters.BotGuid = $guid
            $parameters.PacingPercent = ConvertTo-MicroScenarioBoundedInt -Value (Get-MicroScenarioProperty $arguments 'pacing_percent' 0) -FieldName "$stepId.arguments.pacing_percent" -Minimum 1000 -Maximum 1000
        }
        'fixture-accelerate-off' {
            Assert-MicroScenarioNoUnknownArguments -Arguments $arguments -Allowed @() -StepId $stepId
            $parameters.Action = 'fixture-accelerate-off'
            $parameters.BotGuid = $guid
        }
        'fixture-kill' {
            Assert-MicroScenarioNoUnknownArguments -Arguments $arguments -Allowed @('entry', 'stable_spawn_id') -StepId $stepId
            $parameters.Action = 'fixture-kill'
            $parameters.BotGuid = $guid
            $parameters.TargetEntry = ConvertTo-MicroScenarioPositiveUInt32 -Value (Get-MicroScenarioProperty $arguments 'entry' 0) -FieldName "$stepId.arguments.entry"
            $parameters.StableSpawnId = ConvertTo-MicroScenarioPositiveUInt64 -Value (Get-MicroScenarioProperty $arguments 'stable_spawn_id' 0) -FieldName "$stepId.arguments.stable_spawn_id"
        }
        'independent' {
            Assert-MicroScenarioNoUnknownArguments -Arguments $arguments -Allowed @() -StepId $stepId
            $parameters.Action = 'independent'
            $parameters.BotGuid = $guid
        }
        'quest' {
            Assert-MicroScenarioNoUnknownArguments -Arguments $arguments -Allowed @('quest_id', 'mode') -StepId $stepId
            $mode = [string](Get-MicroScenarioProperty $arguments 'mode' -Default 'explicit')
            if ($mode -notin @('explicit', 'acquire')) { throw "Micro-scenario step '$stepId' quest mode must be explicit or acquire." }
            $parameters.Action = if ($mode -eq 'acquire') { 'quest-acquire' } else { 'quest' }
            $parameters.BotGuid = $guid
            $questId = [uint32](Get-MicroScenarioProperty $arguments 'quest_id' -Default 0)
            if ($mode -eq 'explicit' -and $questId -ne 0) { $parameters.QuestId = ConvertTo-MicroScenarioPositiveUInt32 -Value $questId -FieldName "$stepId.arguments.quest_id" }
            elseif ($mode -eq 'acquire' -and $questId -ne 0) { throw "Micro-scenario step '$stepId' cannot combine quest mode acquire with quest_id." }
        }
        'craft' {
            Assert-MicroScenarioNoUnknownArguments -Arguments $arguments -Allowed @('recipe_spell_id') -StepId $stepId
            $parameters.Action = 'craft'
            $parameters.BotGuid = $guid
            $parameters.RecipeSpellId = ConvertTo-MicroScenarioPositiveUInt32 -Value (Get-MicroScenarioProperty $arguments 'recipe_spell_id') -FieldName "$stepId.arguments.recipe_spell_id"
        }
        'craft-status' {
            Assert-MicroScenarioNoUnknownArguments -Arguments $arguments -Allowed @() -StepId $stepId
            $parameters.Action = 'craft-status'
            $parameters.BotGuid = $guid
        }
        'questobjective' {
            Assert-MicroScenarioNoUnknownArguments -Arguments $arguments -Allowed @() -StepId $stepId
            $parameters.Action = 'questobjective'
            $parameters.BotGuid = $guid
        }
        'professioneconomy' {
            Assert-MicroScenarioNoUnknownArguments -Arguments $arguments -Allowed @() -StepId $stepId
            $parameters.Action = 'professioneconomy'
            $parameters.BotGuid = $guid
        }
        'snapshot' {
            Assert-MicroScenarioNoUnknownArguments -Arguments $arguments -Allowed @() -StepId $stepId
            $parameters.Action = 'snapshot'
            $parameters.BotGuid = $guid
        }
        default { throw "Micro-scenario operation '$operation' is not supported." }
    }

    return [pscustomobject][ordered]@{
        action = [string]$parameters.Action
        parameters = $parameters
        mutation = ($operation -in $script:AutoWowMicroScenarioMutationOperations)
        operation = $operation
    }
}

function Get-MicroScenarioControlRequest {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][object]$Step,
        [Parameter(Mandatory = $true)][string]$ControlPath
    )

    $control = Get-MicroScenarioControlParameters -Step $Step
    $parameters = @{}
    foreach ($property in @($control.parameters.GetEnumerator())) { $parameters[$property.Key] = $property.Value }
    $parameters.EmitRequestOnly = $true
    $parameters.BridgeHost = '127.0.0.1'
    $parameters.Port = 18787
    $parameters.TimeoutMs = 5000
    $output = @(& $ControlPath @parameters | ForEach-Object { [string]$_ })
    $request = @($output | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Select-Object -Last 1)
    if ($request.Count -ne 1) { throw "AutoWow control did not emit a request for micro-scenario step '$($Step.id)'." }
    return [string]$request[0]
}

function Test-MicroScenarioAllowedOperationForPhase {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][object]$Step)

    $phase = [string]$Step.phase
    $operation = ([string]$Step.operation).ToLowerInvariant()
    switch ($phase) {
        'preflight' {
            if ($operation -notin @('list', 'snapshot', 'craft-status', 'questobjective', 'professioneconomy')) {
                throw "Micro-scenario preflight step '$($Step.id)' must use a read-only operation."
            }
        }
        'activate' {
            if ($operation -ne 'activate') { throw "Micro-scenario activate step '$($Step.id)' must use activate." }
        }
        'fixture-init' {
            if ($operation -ne 'fixture-init') { throw "Micro-scenario fixture-init step '$($Step.id)' must use fixture-init." }
        }
        'move/stage' {
            if ($operation -notin $script:AutoWowMicroScenarioReadOnlyOperations) {
                throw "Micro-scenario move/stage step '$($Step.id)' must be read-only; no movement verb is invented."
            }
        }
        'operation' {
            if ($operation -notin @('independent', 'quest', 'craft', 'craft-status', 'fixture-accelerate', 'fixture-accelerate-off', 'fixture-kill', 'questobjective', 'professioneconomy', 'snapshot', 'list')) {
                throw "Micro-scenario operation step '$($Step.id)' uses an unsupported operation."
            }
        }
        'observe' {
            if ($operation -notin $script:AutoWowMicroScenarioReadOnlyOperations) {
                throw "Micro-scenario observe step '$($Step.id)' must be read-only."
            }
        }
        'assert' {
            if ($operation -notin $script:AutoWowMicroScenarioReadOnlyOperations) {
                throw "Micro-scenario assert step '$($Step.id)' must be read-only."
            }
        }
        'cleanup' {
            if ($operation -ne 'fixture-accelerate-off') {
                throw "Micro-scenario cleanup step '$($Step.id)' must use fixture-accelerate-off."
            }
        }
        default { throw "Micro-scenario phase '$phase' is not supported." }
    }
    return $true
}

function Read-MicroScenarioManifest {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Micro-scenario manifest was not found: $Path" }
    $resolvedPath = [System.IO.Path]::GetFullPath((Resolve-Path -LiteralPath $Path).Path)
    try { $document = Get-Content -LiteralPath $resolvedPath -Raw | ConvertFrom-Json -Depth 100 }
    catch { throw "Micro-scenario manifest is not valid JSON: $resolvedPath" }

    if ([string](Get-MicroScenarioProperty $document 'schema' '') -ne $script:AutoWowMicroScenarioSchema) {
        throw "Micro-scenario manifest schema must be $($script:AutoWowMicroScenarioSchema)."
    }
    $scenarioId = [string](Get-MicroScenarioProperty $document 'scenario_id' '')
    if (-not (Test-MicroScenarioSafeIdentifier $scenarioId)) { throw 'Micro-scenario scenario_id is missing or unsafe.' }

    $allowedGuids = @((Get-MicroScenarioProperty $document 'allowed_guids' @()) | ForEach-Object {
        ConvertTo-MicroScenarioPositiveUInt32 -Value $_ -FieldName 'allowed_guids'
    })
    if ($allowedGuids.Count -eq 0 -or @($allowedGuids | Sort-Object -Unique).Count -ne $allowedGuids.Count) {
        throw 'Micro-scenario allowed_guids must contain unique positive GUIDs.'
    }

    $stateSeeds = @()
    foreach ($seed in @((Get-MicroScenarioProperty $document 'state_seeds' @()))) {
        $seedId = [string](Get-MicroScenarioProperty $seed 'seed_id' '')
        if (-not (Test-MicroScenarioSafeIdentifier $seedId)) { throw 'Every micro-scenario state seed needs a safe seed_id.' }
        $kind = ([string](Get-MicroScenarioProperty $seed 'kind' '')).ToLowerInvariant()
        if ($kind -notin $script:AutoWowMicroScenarioStateSeedKinds) {
            throw "Micro-scenario state seed '$seedId' kind '$kind' is unsupported; refusing to guess how to provision it."
        }
        $seedGuid = ConvertTo-MicroScenarioPositiveUInt32 -Value (Get-MicroScenarioProperty $seed 'guid' 0) -FieldName "$seedId.guid"
        if ($seedGuid -notin $allowedGuids) { throw "Micro-scenario state seed '$seedId' GUID $seedGuid is not allowlisted." }
        $conditions = @((Get-MicroScenarioProperty $seed 'conditions' @()) | ForEach-Object { ([string]$_).ToLowerInvariant() })
        foreach ($condition in $conditions) {
            if ($condition -notin $script:AutoWowMicroScenarioStateSeedConditions) {
                throw "Micro-scenario state seed '$seedId' condition '$condition' is unsupported."
            }
        }
        $stateSeeds += [pscustomobject][ordered]@{
            seed_id = $seedId
            kind = $kind
            guid = $seedGuid
            conditions = @($conditions)
        }
    }
    if (@($stateSeeds | Select-Object -ExpandProperty seed_id -Unique).Count -ne $stateSeeds.Count) {
        throw 'Micro-scenario state seed IDs must be unique.'
    }

    $limits = Get-MicroScenarioProperty $document 'limits' $null
    $defaultStepTimeout = if ($null -eq $limits) { 30 } else { ConvertTo-MicroScenarioBoundedInt -Value (Get-MicroScenarioProperty $limits 'step_timeout_seconds' 30) -FieldName 'limits.step_timeout_seconds' -Minimum 1 -Maximum 120 }
    $maxRunSeconds = if ($null -eq $limits) { 600 } else { ConvertTo-MicroScenarioBoundedInt -Value (Get-MicroScenarioProperty $limits 'max_run_seconds' 600) -FieldName 'limits.max_run_seconds' -Minimum 1 -Maximum 3600 }

    $rawSteps = @((Get-MicroScenarioProperty $document 'steps' @()))
    if ($rawSteps.Count -eq 0) { throw 'Micro-scenario manifest must contain at least one step.' }
    $steps = @()
    $expectedSequence = 1
    $previousPhaseIndex = -1
    foreach ($rawStep in $rawSteps) {
        $stepId = [string](Get-MicroScenarioProperty $rawStep 'step_id' '')
        if (-not (Test-MicroScenarioSafeIdentifier $stepId)) { throw 'Every micro-scenario step needs a safe step_id.' }
        $sequence = ConvertTo-MicroScenarioBoundedInt -Value (Get-MicroScenarioProperty $rawStep 'sequence' 0) -FieldName "$stepId.sequence" -Minimum 1 -Maximum 10000
        if ($sequence -ne $expectedSequence) { throw "Micro-scenario steps must have contiguous deterministic sequence numbers; expected $expectedSequence, got $sequence." }
        $expectedSequence++
        $phase = [string](Get-MicroScenarioProperty $rawStep 'phase' '')
        if (-not $script:AutoWowMicroScenarioPhaseOrder.Contains($phase)) { throw "Micro-scenario step '$stepId' has unsupported phase '$phase'." }
        $phaseIndex = [int]$script:AutoWowMicroScenarioPhaseOrder[$phase]
        if ($phaseIndex -lt $previousPhaseIndex) { throw "Micro-scenario phase order regressed at step '$stepId'." }
        $previousPhaseIndex = $phaseIndex
        $operation = ([string](Get-MicroScenarioProperty $rawStep 'operation' '')).ToLowerInvariant()
        if ($operation -notin $script:AutoWowMicroScenarioSupportedOperations) { throw "Micro-scenario step '$stepId' operation '$operation' is unsupported." }
        $guid = [uint32](Get-MicroScenarioProperty $rawStep 'guid' 0)
        if ($operation -ne 'list') {
            $guid = ConvertTo-MicroScenarioPositiveUInt32 -Value $guid -FieldName "$stepId.guid"
            if ($guid -notin $allowedGuids) { throw "Micro-scenario step '$stepId' GUID $guid is outside allowed_guids." }
        }
        [void](Test-MicroScenarioAllowedOperationForPhase -Step ([pscustomobject]@{ id = $stepId; phase = $phase; operation = $operation }))
        $arguments = Get-MicroScenarioProperty $rawStep 'arguments' $null
        if ($null -eq $arguments) { $arguments = [pscustomobject]@{} }
        $assertions = @((Get-MicroScenarioProperty $rawStep 'assertions' @()))
        if ($phase -eq 'assert' -and $assertions.Count -eq 0) { throw "Micro-scenario assert step '$stepId' needs at least one assertion." }
        foreach ($assertion in $assertions) { [void](Get-MicroScenarioAssertionResult -Response ([pscustomobject]@{}) -Assertion $assertion) }
        $timeoutSeconds = ConvertTo-MicroScenarioBoundedInt -Value (Get-MicroScenarioProperty $rawStep 'timeout_seconds' $defaultStepTimeout) -FieldName "$stepId.timeout_seconds" -Minimum 1 -Maximum $defaultStepTimeout
        $durationSeconds = ConvertTo-MicroScenarioBoundedInt -Value (Get-MicroScenarioProperty $rawStep 'duration_seconds' 0) -FieldName "$stepId.duration_seconds" -Minimum 0 -Maximum ($timeoutSeconds - 1)
        $pollSeconds = ConvertTo-MicroScenarioBoundedInt -Value (Get-MicroScenarioProperty $rawStep 'poll_seconds' 1) -FieldName "$stepId.poll_seconds" -Minimum 1 -Maximum 60
        if ($durationSeconds -gt 0 -and $phase -notin @('observe', 'assert')) { throw "Micro-scenario step '$stepId' may only use duration_seconds in observe/assert phases." }
        $steps += [pscustomobject][ordered]@{
            sequence = $sequence
            step_id = $stepId
            phase = $phase
            operation = $operation
            guid = $guid
            arguments = $arguments
            assertions = @($assertions)
            timeout_seconds = $timeoutSeconds
            duration_seconds = $durationSeconds
            poll_seconds = $pollSeconds
        }
    }
    if (@($steps | Select-Object -ExpandProperty step_id -Unique).Count -ne $steps.Count) { throw 'Micro-scenario step IDs must be unique.' }
    if (@($steps | Where-Object phase -eq 'preflight').Count -eq 0) { throw 'Micro-scenario must begin with at least one preflight step.' }
    if (@($steps | Where-Object phase -eq 'operation').Count -eq 0) { throw 'Micro-scenario must contain an operation phase.' }
    foreach ($seed in @($stateSeeds | Where-Object { @($_.conditions).Count -gt 0 })) {
        if (@($steps | Where-Object { $_.operation -eq 'snapshot' -and $_.guid -eq $seed.guid }).Count -eq 0) {
            throw "Micro-scenario state seed '$($seed.seed_id)' has conditions but no snapshot step for GUID $($seed.guid)."
        }
    }

    return [pscustomobject][ordered]@{
        schema = $script:AutoWowMicroScenarioSchema
        schema_version = 1
        scenario_id = $scenarioId
        run_id = [string](Get-MicroScenarioProperty $document 'run_id' '')
        allowed_guids = @($allowedGuids)
        state_seeds = @($stateSeeds)
        limits = [pscustomobject][ordered]@{ step_timeout_seconds = $defaultStepTimeout; max_run_seconds = $maxRunSeconds }
        steps = @($steps)
        source_path = $resolvedPath
    }
}

function Get-MicroScenarioManifestHash {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][string]$Path)

    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Write-MicroScenarioReceipt {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$RunId,
        [Parameter(Mandatory = $true)][string]$ScenarioId,
        [Parameter(Mandatory = $true)][string]$Event,
        [ValidateSet('INFO', 'PASS', 'FAIL', 'SKIP', 'PLAN')][string]$Status = 'INFO',
        [int]$Sequence = 0,
        [string]$StepId = '',
        [string]$Phase = '',
        [AllowNull()][object]$Data = $null
    )

    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    $record = [ordered]@{
        schema = $script:AutoWowMicroScenarioReceiptSchema
        schema_version = 1
        ts = [DateTimeOffset]::UtcNow.ToString('o')
        run_id = $RunId
        scenario_id = $ScenarioId
        event = $Event
        status = $Status
        sequence = $Sequence
        step_id = if ([string]::IsNullOrWhiteSpace($StepId)) { $null } else { $StepId }
        phase = if ([string]::IsNullOrWhiteSpace($Phase)) { $null } else { $Phase }
        data = $Data
    }
    $line = $record | ConvertTo-Json -Compress -Depth 100
    [System.IO.File]::AppendAllText($Path, $line + [Environment]::NewLine, [System.Text.UTF8Encoding]::new($false))
    return [pscustomobject]$record
}

function Read-MicroScenarioReceipts {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return @() }
    $records = @()
    foreach ($line in @(Get-Content -LiteralPath $Path)) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        try { $record = $line | ConvertFrom-Json -Depth 100 }
        catch { throw "Micro-scenario receipt contains invalid JSON: $Path" }
        if ([string](Get-MicroScenarioProperty $record 'schema' '') -ne $script:AutoWowMicroScenarioReceiptSchema) {
            throw "Micro-scenario receipt has an unexpected schema: $Path"
        }
        $records += $record
    }
    return @($records)
}

function Get-MicroScenarioCompletedStepIds {
    [CmdletBinding()]
    param([AllowEmptyCollection()][object[]]$Receipts = @())

    return @($Receipts | Where-Object { $_.event -eq 'step_completed' -and $_.status -eq 'PASS' } | ForEach-Object { [string]$_.step_id } | Sort-Object -Unique)
}

function Assert-MicroScenarioReceiptIdentity {
    [CmdletBinding()]
    param(
        [AllowEmptyCollection()][object[]]$Receipts = @(),
        [Parameter(Mandatory = $true)][string]$RunId,
        [Parameter(Mandatory = $true)][string]$ScenarioId,
        [Parameter(Mandatory = $true)][string]$ManifestHash
    )

    foreach ($receipt in $Receipts) {
        if ([string]$receipt.run_id -ne $RunId -or [string]$receipt.scenario_id -ne $ScenarioId) {
            throw 'Micro-scenario receipt identity does not match the requested run; refusing to append.'
        }
        $recordHash = [string](Get-MicroScenarioProperty -InputObject $receipt.data -Name 'manifest_sha256' -Default '')
        if (-not [string]::IsNullOrWhiteSpace($recordHash) -and $recordHash -ne $ManifestHash) {
            throw 'Micro-scenario manifest hash changed for an existing run; refusing to resume.'
        }
    }
}

function Test-MicroScenarioStateSeedSnapshot {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][object]$Seed,
        [Parameter(Mandatory = $true)][object]$Response
    )

    $bot = Get-MicroScenarioProperty -InputObject $Response -Name 'bot' -Default $Response
    foreach ($condition in @($Seed.conditions)) {
        switch ($condition) {
            'online' {
                if (-not [bool](Get-MicroScenarioProperty -InputObject $Response -Name 'ok' -Default $false) -or $null -eq $bot) {
                    throw "State seed '$($Seed.seed_id)' requires an online bot."
                }
            }
            'alive' {
                if ($null -ne $bot.PSObject.Properties['dead']) {
                    if ([bool]$bot.dead) { throw "State seed '$($Seed.seed_id)' requires an alive bot." }
                }
                elseif ($null -ne $bot.PSObject.Properties['alive']) {
                    if (-not [bool]$bot.alive) { throw "State seed '$($Seed.seed_id)' requires an alive bot." }
                }
                else { throw "State seed '$($Seed.seed_id)' cannot prove alive: snapshot has no dead/alive field." }
            }
            'noncombat' {
                if ($null -eq $bot.PSObject.Properties['combat']) { throw "State seed '$($Seed.seed_id)' cannot prove noncombat: snapshot has no combat field." }
                if ([bool]$bot.combat) { throw "State seed '$($Seed.seed_id)' requires a noncombat bot." }
            }
        }
    }
    return $true
}

function Get-MicroScenarioSeedForGuid {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][object]$Manifest,
        [Parameter(Mandatory = $true)][uint32]$Guid
    )

    return @($Manifest.state_seeds | Where-Object { [uint32]$_.guid -eq $Guid })
}
