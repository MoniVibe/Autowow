Set-StrictMode -Version Latest

$script:OracleControlledPhases = @('observe', 'plan', 'execute', 'verify', 'recover')
$script:OracleControlledVerbs = @('observe', 'plan', 'target', 'travel', 'interact', 'loot', 'gather', 'craft', 'train', 'fight', 'assist', 'rest', 'recover', 'choose_talent', 'equip', 'cooperate')
$script:OracleControlledStatuses = @('native', 'planned', 'blocked')
$script:OracleControlledOperators = @('eq', 'neq', 'gt', 'gte', 'lt', 'lte', 'in', 'not_in', 'exists')
$script:OracleControlledSuccessTypes = @('objective_count_increase', 'quest_complete', 'quest_rewarded', 'material_count_increase', 'skill_increase', 'craft_receipt_success', 'target_defeated', 'ally_health_stabilized', 'position_reached', 'talent_known', 'spell_known', 'loot_received', 'state_changed')
$script:OracleControlledDomains = @('quest', 'gather', 'craft', 'train', 'combat', 'cooperation', 'survival', 'travel', 'loot')
$script:OracleControlledFallbacks = @('recover', 'replan', 'idle', 'none')

function Get-OracleControlledProperty {
    param(
        [AllowNull()][object]$Object,
        [Parameter(Mandatory = $true)][string]$Name
    )

    if ($null -eq $Object) { return $null }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}

function Get-OracleControlledAgentCatalog {
    param(
        [Parameter(Mandatory = $true)][string]$Path
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Oracle controlled-agent catalog is missing: $Path"
    }
    return (Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json)
}

function Get-OracleControlledVocabulary {
    return [ordered]@{
        phases = @($script:OracleControlledPhases)
        verbs = @($script:OracleControlledVerbs)
        statuses = @($script:OracleControlledStatuses)
        operators = @($script:OracleControlledOperators)
        success_types = @($script:OracleControlledSuccessTypes)
        domains = @($script:OracleControlledDomains)
        fallbacks = @($script:OracleControlledFallbacks)
    }
}

function Add-OracleControlledError {
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][System.Collections.Generic.List[string]]$Errors,
        [Parameter(Mandatory = $true)][string]$Message
    )
    [void]$Errors.Add($Message)
}

function Test-OracleControlledIdentifier {
    param([AllowNull()][object]$Value)
    return $null -ne $Value -and [string]$Value -match '^[a-z0-9][a-z0-9_.-]*$'
}

function Test-OracleControlledRuleText {
    param([AllowNull()][object]$Value)
    $text = [string]$Value
    return -not [string]::IsNullOrWhiteSpace($text) -and
        $text.StartsWith('WHEN ', [System.StringComparison]::Ordinal) -and
        $text.Contains(' DO ') -and
        $text.Contains(' VERIFY ') -and
        $text.Contains(' ON_FAILURE ') -and
        $text.Contains(' THEN ')
}

function Test-OracleControlledAgentCatalog {
    param(
        [Parameter(Mandatory = $true)][object]$Catalog
    )

    $errors = [System.Collections.Generic.List[string]]::new()
    $schema = Get-OracleControlledProperty -Object $Catalog -Name 'schema'
    if ([string]$schema -ne 'autowow.oracle.controlled-agent.v1') {
        Add-OracleControlledError -Errors $errors -Message "Unexpected catalog schema: $schema"
    }

    $language = Get-OracleControlledProperty -Object $Catalog -Name 'language'
    if ($null -eq $language) {
        Add-OracleControlledError -Errors $errors -Message 'Catalog language section is missing.'
    } else {
        if ([bool](Get-OracleControlledProperty -Object $language -Name 'formal_ste_compliance')) {
            Add-OracleControlledError -Errors $errors -Message 'Catalog must not claim formal ASD-STE100 compliance.'
        }
        $ruleOrder = @((Get-OracleControlledProperty -Object $language -Name 'rule_order'))
        if (@($ruleOrder) -join ',' -ne 'WHEN,DO,VERIFY,ON_FAILURE,THEN') {
            Add-OracleControlledError -Errors $errors -Message 'Catalog rule order must be WHEN,DO,VERIFY,ON_FAILURE,THEN.'
        }
    }

    $doctrines = @((Get-OracleControlledProperty -Object $Catalog -Name 'doctrines'))
    if ($doctrines.Count -eq 0) {
        Add-OracleControlledError -Errors $errors -Message 'Catalog must define at least one doctrine.'
    }
    $doctrineIds = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($doctrine in $doctrines) {
        $id = Get-OracleControlledProperty -Object $doctrine -Name 'id'
        if (-not (Test-OracleControlledIdentifier $id)) { Add-OracleControlledError $errors "Invalid doctrine id: $id" }
        if (-not $doctrineIds.Add([string]$id)) { Add-OracleControlledError $errors "Duplicate doctrine id: $id" }
        if ([bool](Get-OracleControlledProperty -Object $doctrine -Name 'requires_leader')) {
            Add-OracleControlledError $errors "Doctrine $id requires a leader; independent doctrines must not require one."
        }
        $cooperation = [string](Get-OracleControlledProperty -Object $doctrine -Name 'cooperation')
        if ($cooperation -notin @('voluntary', 'none')) { Add-OracleControlledError $errors "Doctrine $id has invalid cooperation mode: $cooperation" }
        $priorities = @((Get-OracleControlledProperty -Object $doctrine -Name 'priorities'))
        foreach ($priority in $priorities) {
            if ([string]$priority -notin $script:OracleControlledDomains) { Add-OracleControlledError $errors "Doctrine $id has invalid priority domain: $priority" }
        }
    }

    $contracts = @((Get-OracleControlledProperty -Object $Catalog -Name 'contracts'))
    if ($contracts.Count -eq 0) {
        Add-OracleControlledError -Errors $errors -Message 'Catalog must define at least one contract.'
    }
    $contractIds = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($contract in $contracts) {
        $id = Get-OracleControlledProperty -Object $contract -Name 'id'
        $domain = [string](Get-OracleControlledProperty -Object $contract -Name 'domain')
        $status = [string](Get-OracleControlledProperty -Object $contract -Name 'status')
        if (-not (Test-OracleControlledIdentifier $id)) { Add-OracleControlledError $errors "Invalid contract id: $id" }
        if (-not $contractIds.Add([string]$id)) { Add-OracleControlledError $errors "Duplicate contract id: $id" }
        if ($domain -notin $script:OracleControlledDomains) { Add-OracleControlledError $errors "Contract $id has invalid domain: $domain" }
        if ($status -notin $script:OracleControlledStatuses) { Add-OracleControlledError $errors "Contract $id has invalid status: $status" }
        if (-not (Test-OracleControlledRuleText (Get-OracleControlledProperty -Object $contract -Name 'controlled_rule'))) {
            Add-OracleControlledError $errors "Contract $id does not use the required WHEN/DO/VERIFY/ON_FAILURE/THEN rule order."
        }

        $preconditions = @((Get-OracleControlledProperty -Object $contract -Name 'preconditions'))
        if ($preconditions.Count -eq 0) { Add-OracleControlledError $errors "Contract $id has no preconditions." }
        $preconditionIds = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
        foreach ($precondition in $preconditions) {
            $preconditionId = Get-OracleControlledProperty -Object $precondition -Name 'id'
            $subject = [string](Get-OracleControlledProperty -Object $precondition -Name 'subject')
            $operator = [string](Get-OracleControlledProperty -Object $precondition -Name 'operator')
            if (-not (Test-OracleControlledIdentifier $preconditionId)) { Add-OracleControlledError $errors "Contract $id has invalid precondition id: $preconditionId" }
            if (-not $preconditionIds.Add([string]$preconditionId)) { Add-OracleControlledError $errors "Contract $id has duplicate precondition id: $preconditionId" }
            if ([string]::IsNullOrWhiteSpace($subject) -or $subject -notmatch '^[a-z][a-z0-9_.]*$') { Add-OracleControlledError $errors "Contract $id has invalid precondition subject: $subject" }
            if ($operator -notin $script:OracleControlledOperators) { Add-OracleControlledError $errors "Contract $id has invalid operator: $operator" }
        }

        $action = Get-OracleControlledProperty -Object $contract -Name 'action'
        if ($null -eq $action) {
            Add-OracleControlledError $errors "Contract $id action is missing."
        } else {
            $phase = [string](Get-OracleControlledProperty -Object $action -Name 'phase')
            $verb = [string](Get-OracleControlledProperty -Object $action -Name 'verb')
            $operation = [string](Get-OracleControlledProperty -Object $action -Name 'operation')
            $target = [string](Get-OracleControlledProperty -Object $action -Name 'target')
            if ($phase -notin $script:OracleControlledPhases) { Add-OracleControlledError $errors "Contract $id has invalid action phase: $phase" }
            if ($verb -notin $script:OracleControlledVerbs) { Add-OracleControlledError $errors "Contract $id has invalid action verb: $verb" }
            if ([string]::IsNullOrWhiteSpace($operation) -or $operation -notmatch '^[a-z][a-z0-9_.-]*$') { Add-OracleControlledError $errors "Contract $id has invalid operation: $operation" }
            if ([string]::IsNullOrWhiteSpace($target) -or $target -notmatch '^[a-z][a-z0-9_.-]*$') { Add-OracleControlledError $errors "Contract $id has invalid target: $target" }
            if ($operation -match 'random|generic|nearby') { Add-OracleControlledError $errors "Contract $id uses a non-deterministic operation: $operation" }
        }

        $successes = @((Get-OracleControlledProperty -Object $contract -Name 'success'))
        if ($successes.Count -eq 0) { Add-OracleControlledError $errors "Contract $id has no success signal." }
        $successIds = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
        foreach ($success in $successes) {
            $successId = Get-OracleControlledProperty -Object $success -Name 'id'
            $successType = [string](Get-OracleControlledProperty -Object $success -Name 'type')
            $source = [string](Get-OracleControlledProperty -Object $success -Name 'source')
            if (-not (Test-OracleControlledIdentifier $successId)) { Add-OracleControlledError $errors "Contract $id has invalid success id: $successId" }
            if (-not $successIds.Add([string]$successId)) { Add-OracleControlledError $errors "Contract $id has duplicate success id: $successId" }
            if ($successType -notin $script:OracleControlledSuccessTypes) { Add-OracleControlledError $errors "Contract $id has invalid success type: $successType" }
            if ([string]::IsNullOrWhiteSpace($source)) { Add-OracleControlledError $errors "Contract $id success signal $successId has no source." }
        }

        $failure = Get-OracleControlledProperty -Object $contract -Name 'failure'
        if ($null -eq $failure) {
            Add-OracleControlledError $errors "Contract $id failure policy is missing."
        } else {
            $failureCode = [string](Get-OracleControlledProperty -Object $failure -Name 'code')
            $nextPhase = [string](Get-OracleControlledProperty -Object $failure -Name 'next_phase')
            $fallback = [string](Get-OracleControlledProperty -Object $failure -Name 'fallback')
            $retryLimit = Get-OracleControlledProperty -Object $failure -Name 'retry_limit'
            if ($failureCode -notmatch '^[a-z][a-z0-9_.-]*:[a-z][a-z0-9_.-]*$') { Add-OracleControlledError $errors "Contract $id has invalid failure code: $failureCode" }
            if ($nextPhase -notin $script:OracleControlledPhases) { Add-OracleControlledError $errors "Contract $id has invalid failure phase: $nextPhase" }
            if ($fallback -notin $script:OracleControlledFallbacks) { Add-OracleControlledError $errors "Contract $id has invalid fallback: $fallback" }
            if ($null -eq $retryLimit -or [int]$retryLimit -lt 0) { Add-OracleControlledError $errors "Contract $id has invalid retry limit: $retryLimit" }
            if ($status -eq 'planned' -and $fallback -eq 'recover' -and [int]$retryLimit -gt 0) { Add-OracleControlledError $errors "Planned contract $id must not claim an executable recovery loop." }
        }
    }

    return [pscustomobject]@{
        valid = ($errors.Count -eq 0)
        schema = [string]$schema
        doctrine_count = $doctrines.Count
        contract_count = $contracts.Count
        errors = @($errors)
    }
}

function Assert-OracleControlledAgentCatalog {
    param(
        [Parameter(Mandatory = $true)][string]$Path
    )
    $result = Test-OracleControlledAgentCatalog -Catalog (Get-OracleControlledAgentCatalog -Path $Path)
    if (-not $result.valid) {
        throw ('Oracle controlled-agent catalog validation failed: ' + (@($result.errors) -join '; '))
    }
    return $true
}
