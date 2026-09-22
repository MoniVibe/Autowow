[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateNotNullOrEmpty()]
    [string]$InputJson,

    [switch]$UseJev,

    [ValidateRange(1, 60)]
    [int]$TimeoutSeconds = 15,

    [ValidateNotNullOrEmpty()]
    [string]$Model = 'jev-latest'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-RequiredProperty {
    param([Parameter(Mandatory)]$Object, [Parameter(Mandatory)][string]$Name)

    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { throw "Input is missing required property '$Name'." }
    return $property.Value
}

function ConvertTo-ValidatedAdvisorInput {
    param([Parameter(Mandatory)][string]$Json)

    try { $inputObject = ConvertFrom-Json -InputObject $Json -Depth 30 }
    catch { throw "InputJson is not valid JSON: $($_.Exception.Message)" }

    $state = Get-RequiredProperty -Object $inputObject -Name 'state'
    $allowlist = @(Get-RequiredProperty -Object $inputObject -Name 'allowlist')
    if ([string]::IsNullOrWhiteSpace([string](Get-RequiredProperty $state 'faction'))) {
        throw "Input state property 'faction' must be non-empty."
    }

    $guilds = @(Get-RequiredProperty $state 'guilds')
    if ($guilds.Count -eq 0) { throw "Input state property 'guilds' must contain at least one guild." }
    $guildIds = [System.Collections.Generic.HashSet[long]]::new()
    foreach ($guild in $guilds) {
        $guildId = [long](Get-RequiredProperty $guild 'guildId')
        if ($guildId -le 0 -or -not $guildIds.Add($guildId)) {
            throw 'Guild IDs must be unique positive database IDs.'
        }
        if ([long](Get-RequiredProperty $guild 'copper') -lt 0) { throw "Guild $guildId has negative copper." }
        [void](Get-RequiredProperty $guild 'inventory')
    }
    [void](Get-RequiredProperty $state 'activeContracts')
    [void](Get-RequiredProperty $state 'bottlenecks')

    if ($allowlist.Count -lt 2 -or $allowlist.Count -gt 4) {
        throw 'Allowlist must contain 2 to 4 legal strategic choices.'
    }

    $choiceIds = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $choices = for ($index = 0; $index -lt $allowlist.Count; $index++) {
        $candidate = $allowlist[$index]
        if ($candidate -is [string]) {
            $id = [string]$candidate
            $description = [string]$candidate
            $rank = $index + 1
        } else {
            $id = [string](Get-RequiredProperty $candidate 'id')
            $descriptionProperty = $candidate.PSObject.Properties['description']
            $description = if ($null -eq $descriptionProperty) { $id } else { [string]$descriptionProperty.Value }
            $rankProperty = $candidate.PSObject.Properties['baselineRank']
            $rank = if ($null -eq $rankProperty) { $index + 1 } else { [int]$rankProperty.Value }
        }
        if ([string]::IsNullOrWhiteSpace($id) -or -not $choiceIds.Add($id)) {
            throw 'Allowlist choice IDs must be unique non-empty strings.'
        }
        [pscustomobject]@{ id = $id; description = $description; baseline_rank = $rank; input_order = $index }
    }

    [pscustomobject]@{
        state = $state
        choices = @($choices | Sort-Object baseline_rank, input_order, id)
    }
}

function New-AdvisorResult {
    param($Validated, [string]$Mode, [string]$SelectedChoice, [object[]]$Ranking,
        [bool]$JevAttempted, $Confidence = $null, $ResolvedModel = $null, $Usage = $null,
        $CostUsd = $null, $FallbackReason = $null)

    [pscustomobject][ordered]@{
        schema_version = 1
        mode = $Mode
        selected_choice = $SelectedChoice
        ranking = @($Ranking)
        jev_attempted = $JevAttempted
        confidence = $Confidence
        model = $ResolvedModel
        usage = $Usage
        cost_usd = $CostUsd
        fallback_reason = $FallbackReason
        advisory_only = $true
    }
}

$validated = ConvertTo-ValidatedAdvisorInput -Json $InputJson
$baselineRanking = @($validated.choices | ForEach-Object { $_.id })
$baselineChoice = $baselineRanking[0]

if (-not $UseJev) {
    New-AdvisorResult $validated 'baseline' $baselineChoice $baselineRanking $false |
        ConvertTo-Json -Depth 20 -Compress
    return
}

try {
    $apiKey = [Environment]::GetEnvironmentVariable('JEV_API_KEY', 'Process')
    if ([string]::IsNullOrWhiteSpace($apiKey)) { throw 'JEV_API_KEY is not set for this process.' }

    $criteria = [ordered]@{}
    foreach ($choice in $validated.choices) { $criteria[$choice.id] = $choice.description }
    $request = [ordered]@{
        model = $Model
        state = $validated.state
        questions = [ordered]@{
            strategy = [ordered]@{
                type = 'choice'
                instructions = 'Select exactly one legal milestone-level guild-economy strategy. This is advisory only.'
                criteria = $criteria
            }
        }
    }

    $response = Invoke-RestMethod -Uri 'https://jevtypesafeai.com/api/v1/decide' -Method Post `
        -Headers @{ Authorization = "Bearer $apiKey" } -ContentType 'application/json' `
        -Body ($request | ConvertTo-Json -Depth 30 -Compress) -TimeoutSec $TimeoutSeconds

    $answer = Get-RequiredProperty (Get-RequiredProperty $response 'answers') 'strategy'
    if ([string](Get-RequiredProperty $answer 'type') -ne 'choice') { throw 'Jev returned the wrong answer type.' }
    $selected = [string](Get-RequiredProperty $answer 'choice')
    if ($selected -notin $baselineRanking) { throw "Jev returned a choice outside the controller allowlist." }

    $probabilities = Get-RequiredProperty $answer 'probabilities'
    foreach ($id in $baselineRanking) {
        $property = $probabilities.PSObject.Properties[$id]
        if ($null -eq $property -or [double]$property.Value -lt 0 -or [double]$property.Value -gt 1) {
            throw 'Jev returned invalid or incomplete choice probabilities.'
        }
    }
    $ranking = @($baselineRanking | Sort-Object @{ Expression = { -[double]$probabilities.PSObject.Properties[$_].Value } }, @{ Expression = { [array]::IndexOf($baselineRanking, $_) } })
    $usage = Get-RequiredProperty $response 'usage'
    $cost = [double](Get-RequiredProperty $usage 'cost_usd')
    if ($cost -lt 0) { throw 'Jev returned an invalid cost.' }

    New-AdvisorResult $validated 'jev' $selected $ranking $true ([double](Get-RequiredProperty $answer 'confidence')) `
        ([string](Get-RequiredProperty $response 'model')) $usage $cost |
        ConvertTo-Json -Depth 20 -Compress
} catch {
    $errorMessage = [string]$_.Exception.Message
    $fallbackReason = switch -Wildcard ($errorMessage) {
        'JEV_API_KEY is not set*' { 'JEV_API_KEY is not set for this process.'; break }
        '*outside the controller allowlist*' { 'Jev returned a choice outside the controller allowlist.'; break }
        '*wrong answer type*' { 'Jev response validation failed: wrong answer type.'; break }
        '*invalid or incomplete choice probabilities*' { 'Jev response validation failed: invalid choice probabilities.'; break }
        '*invalid cost*' { 'Jev response validation failed: invalid cost.'; break }
        "Input is missing required property*" { 'Jev response validation failed: required response data is missing.'; break }
        default { 'Jev request failed; deterministic baseline selected.' }
    }
    New-AdvisorResult $validated 'fallback' $baselineChoice $baselineRanking $true $null $null $null $null `
        $fallbackReason | ConvertTo-Json -Depth 20 -Compress
}
