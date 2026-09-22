[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateNotNullOrEmpty()]
    [string]$PlayerbotsLogPath,

    [ValidateRange(1, [int]::MaxValue)]
    [int]$StartLine = 1,

    [Alias('Window')]
    [ValidateRange(1, [int]::MaxValue)]
    [Nullable[int]]$WindowLines
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:Schema = 'autowow.corpse-rescue-acceptance.v1'
$windowWasSupplied = $PSBoundParameters.ContainsKey('WindowLines')

function Resolve-ReadOnlyLogPath {
    param([Parameter(Mandatory)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Playerbots log not found: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function New-CorpseRescueEvidence {
    param(
        [Parameter(Mandatory)][int]$LineNumber,
        [Parameter(Mandatory)][string]$Event,
        [Parameter(Mandatory)][string]$Raw,
        [Parameter(Mandatory)][bool]$Valid,
        [Parameter(Mandatory)][AllowEmptyCollection()][string[]]$Errors,
        [Parameter(Mandatory)][object]$Fields
    )

    return [pscustomobject][ordered]@{
        line_number = $LineNumber
        event = $Event
        valid = $Valid
        errors = @($Errors)
        fields = [pscustomobject]$Fields
        raw = $Raw
    }
}

function New-MalformedCorpseRescueEvidence {
    param(
        [Parameter(Mandatory)][int]$LineNumber,
        [Parameter(Mandatory)][string]$Event,
        [Parameter(Mandatory)][string]$Raw,
        [Parameter(Mandatory)][string]$Error
    )

    return New-CorpseRescueEvidence -LineNumber $LineNumber -Event $Event -Raw $Raw `
        -Valid $false -Errors @($Error) -Fields ([ordered]@{})
}

function ConvertFrom-CorpseRescueLine {
    param(
        [Parameter(Mandatory)][int]$LineNumber,
        [Parameter(Mandatory)][string]$Text
    )

    $marker = $Text.IndexOf('[CorpseRescue]', [StringComparison]::Ordinal)
    if ($marker -lt 0) { return $null }

    $payload = $Text.Substring($marker + '[CorpseRescue]'.Length).Trim()
    if ($payload -match '^cast_started(?:\s|$)') {
        $eventName = 'cast_started'
        $pattern = '^cast_started\s+healer=(?<healer>\S+)\s+target=(?<target>\S+)\s+spell=(?<spell>.+?)\s+mode=(?<mode>unit|corpse)(?:\s+corpse=(?<corpse>[0-9]+))?\s+caster_combat=(?<caster>true|false)(?:\s+group_combat=(?<group>true|false))?\s+combat_res=(?<combatRes>true|false)\s*$'
        $match = [regex]::Match($payload, $pattern, [Text.RegularExpressions.RegexOptions]::CultureInvariant)
        if (-not $match.Success) {
            return New-MalformedCorpseRescueEvidence $LineNumber $eventName $Text `
                'cast_started fields do not match the required telemetry contract'
        }

        $mode = $match.Groups['mode'].Value
        $hasCorpse = $match.Groups['corpse'].Success
        if ($mode -eq 'unit' -and $hasCorpse) {
            return New-MalformedCorpseRescueEvidence $LineNumber $eventName $Text `
                'corpse is only valid when mode=corpse'
        }

        [uint64]$corpse = 0
        if ($hasCorpse -and
            (-not [uint64]::TryParse($match.Groups['corpse'].Value, [ref]$corpse) -or $corpse -eq 0)) {
            return New-MalformedCorpseRescueEvidence $LineNumber $eventName $Text `
                'corpse must be a non-zero unsigned integer'
        }

        $fields = [ordered]@{
            healer = $match.Groups['healer'].Value
            target = $match.Groups['target'].Value
            spell = $match.Groups['spell'].Value
            mode = $mode
            corpse = if ($hasCorpse) { $corpse } else { $null }
            caster_combat = ($match.Groups['caster'].Value -ceq 'true')
            group_combat = if ($match.Groups['group'].Success) {
                $match.Groups['group'].Value -ceq 'true'
            } else { $null }
            combat_res = ($match.Groups['combatRes'].Value -ceq 'true')
        }
        return New-CorpseRescueEvidence $LineNumber $eventName $Text $true @() $fields
    }

    if ($payload -match '^accept(?:\s|$)') {
        $eventName = 'accept'
        $pattern = '^accept\s+target=(?<target>\S+)\s+source=(?<source>\S+)\s+released=(?<released>true|false)\s+alive_after=(?<alive>true|false)\s*$'
        $match = [regex]::Match($payload, $pattern, [Text.RegularExpressions.RegexOptions]::CultureInvariant)
        if (-not $match.Success) {
            return New-MalformedCorpseRescueEvidence $LineNumber $eventName $Text `
                'accept fields do not match the required telemetry contract'
        }

        $fields = [ordered]@{
            target = $match.Groups['target'].Value
            source = $match.Groups['source'].Value
            released = ($match.Groups['released'].Value -ceq 'true')
            alive_after = ($match.Groups['alive'].Value -ceq 'true')
        }
        return New-CorpseRescueEvidence $LineNumber $eventName $Text $true @() $fields
    }

    if ($payload -match '^completed(?:\s|$)') {
        $eventName = 'completed'
        $pattern = '^completed\s+target=(?<target>\S+)\s+alive=(?<alive>true|false)\s+map=(?<map>[0-9]+)\s+instance=(?<instance>[0-9]+)\s*$'
        $match = [regex]::Match($payload, $pattern, [Text.RegularExpressions.RegexOptions]::CultureInvariant)
        if (-not $match.Success) {
            return New-MalformedCorpseRescueEvidence $LineNumber $eventName $Text `
                'completed fields do not match the required telemetry contract'
        }

        [uint32]$map = 0
        [uint32]$instance = 0
        if (-not [uint32]::TryParse($match.Groups['map'].Value, [ref]$map)) {
            return New-MalformedCorpseRescueEvidence $LineNumber $eventName $Text `
                'completed map must be an unsigned 32-bit integer'
        }
        if (-not [uint32]::TryParse($match.Groups['instance'].Value, [ref]$instance)) {
            return New-MalformedCorpseRescueEvidence $LineNumber $eventName $Text `
                'completed instance must be an unsigned 32-bit integer'
        }

        $fields = [ordered]@{
            target = $match.Groups['target'].Value
            alive = ($match.Groups['alive'].Value -ceq 'true')
            map = $map
            instance = $instance
        }
        return New-CorpseRescueEvidence $LineNumber $eventName $Text $true @() $fields
    }

    if ($payload -match '^cast_blocked(?:\s|$)') {
        $eventName = 'cast_blocked'
        $reasonMatch = [regex]::Match($payload, '(?<!\S)reason=(?<reason>\S+)')
        if (-not $reasonMatch.Success) {
            return New-MalformedCorpseRescueEvidence $LineNumber $eventName $Text `
                'cast_blocked is missing reason'
        }
        if ($reasonMatch.Groups['reason'].Value -cne 'combat') {
            return $null
        }

        $pattern = '^cast_blocked\s+healer=(?<healer>\S+)\s+target=(?<target>\S+)\s+reason=combat\s+caster_combat=(?<caster>true|false)(?:\s+group_combat=(?<group>true|false))?\s+combat_res=(?<combatRes>true|false)\s*$'
        $match = [regex]::Match($payload, $pattern, [Text.RegularExpressions.RegexOptions]::CultureInvariant)
        if (-not $match.Success) {
            return New-MalformedCorpseRescueEvidence $LineNumber $eventName $Text `
                'combat cast_blocked fields do not match the required telemetry contract'
        }

        $fields = [ordered]@{
            healer = $match.Groups['healer'].Value
            target = $match.Groups['target'].Value
            reason = 'combat'
            caster_combat = ($match.Groups['caster'].Value -ceq 'true')
            group_combat = if ($match.Groups['group'].Success) {
                $match.Groups['group'].Value -ceq 'true'
            } else { $null }
            combat_res = ($match.Groups['combatRes'].Value -ceq 'true')
        }
        return New-CorpseRescueEvidence $LineNumber $eventName $Text $true @() $fields
    }

    # Movement and cast-failure diagnostics are outside this acceptance contract.
    return $null
}

function Get-ResurrectionChainClassification {
    param(
        [Parameter(Mandatory)][object]$Started,
        [Parameter(Mandatory)][object]$Accepted,
        [Parameter(Mandatory)][object]$Completed
    )

    if (-not $Completed.fields.alive) { return $null }

    if ($Started.fields.mode -eq 'corpse') {
        if ($Accepted.fields.released) { return 'RELEASED_CORPSE_PASS' }
        return $null
    }
    if ($Started.fields.combat_res -and $Started.fields.caster_combat) {
        return 'COMBAT_RES_PASS'
    }
    if (-not $Started.fields.combat_res -and -not $Started.fields.caster_combat) {
        return 'POSTCOMBAT_RES_PASS'
    }
    return $null
}

function New-CorpseRescueResult {
    param(
        [Parameter(Mandatory)][string]$Status,
        [Parameter(Mandatory)][AllowEmptyCollection()][string[]]$Reasons,
        [Parameter(Mandatory)][AllowEmptyCollection()][object[]]$Evidence,
        [Parameter(Mandatory)][AllowEmptyCollection()][object[]]$MatchedEvidence,
        [Parameter(Mandatory)][int]$TotalLineCount,
        [Parameter(Mandatory)][int]$EffectiveEndLine,
        [Parameter(Mandatory)][int]$PairCount,
        [Parameter(Mandatory)][int]$ChainCount,
        [Parameter(Mandatory)][int]$QualifyingMatchCount,
        [Parameter(Mandatory)][int]$MalformedCount,
        [Parameter(Mandatory)][string]$ResolvedLogPath
    )

    return [ordered]@{
        schema = $script:Schema
        status = $Status
        read_only = $true
        reasons = @($Reasons | Select-Object -Unique)
        source = [ordered]@{
            playerbots_log = $ResolvedLogPath
        }
        interval = [ordered]@{
            start_line = $StartLine
            window_lines = if ($windowWasSupplied) { [int]$WindowLines } else { $null }
            end_line = $EffectiveEndLine
            total_line_count = $TotalLineCount
        }
        telemetry_event_count = $Evidence.Count
        malformed_event_count = $MalformedCount
        matched_pair_count = $PairCount
        matched_chain_count = $ChainCount
        qualifying_match_count = $QualifyingMatchCount
        matched_evidence = @($MatchedEvidence)
        evidence = @($Evidence)
    }
}

$logPath = Resolve-ReadOnlyLogPath -Path $PlayerbotsLogPath
$lines = @(Get-Content -LiteralPath $logPath)
$totalLineCount = $lines.Count

if ($totalLineCount -gt 0 -and $StartLine -gt $totalLineCount) {
    throw "StartLine $StartLine is beyond the $totalLineCount-line log."
}

$requestedEnd = if ($windowWasSupplied) {
    [int64]$StartLine + [int64]$WindowLines - 1
}
else {
    [int64]::MaxValue
}
$effectiveEndLine = if ($totalLineCount -eq 0) {
    0
}
else {
    [int][Math]::Min([int64]$totalLineCount, $requestedEnd)
}

$events = [System.Collections.Generic.List[object]]::new()
if ($effectiveEndLine -ge $StartLine) {
    for ($lineNumber = $StartLine; $lineNumber -le $effectiveEndLine; $lineNumber++) {
        $parsed = ConvertFrom-CorpseRescueLine -LineNumber $lineNumber -Text $lines[$lineNumber - 1]
        if ($null -ne $parsed) { [void]$events.Add($parsed) }
    }
}
$eventArray = @($events)
$malformed = @($eventArray | Where-Object { -not $_.valid })
$validEvents = @($eventArray | Where-Object { $_.valid })
$starts = @($validEvents | Where-Object { $_.event -eq 'cast_started' })
$accepts = @($validEvents | Where-Object { $_.event -eq 'accept' })
$completions = @($validEvents | Where-Object { $_.event -eq 'completed' })

# Match each accept to the earliest still-unmatched prior start for the same exact target.
# This FIFO rule preserves log chronology and prevents one accept from satisfying duplicate starts.
$usedStartLines = [System.Collections.Generic.HashSet[int]]::new()
$pairs = [System.Collections.Generic.List[object]]::new()
foreach ($accept in $accepts) {
    $matchingStart = $null
    foreach ($start in $starts) {
        if ($start.line_number -ge $accept.line_number) { break }
        if (-not $usedStartLines.Contains($start.line_number) -and
            $start.fields.target -ceq $accept.fields.target) {
            $matchingStart = $start
            break
        }
    }
    if ($null -eq $matchingStart) { continue }

    [void]$usedStartLines.Add($matchingStart.line_number)
    [void]$pairs.Add([pscustomobject]@{
        Started = $matchingStart
        Accepted = $accept
    })
}
$pairArray = @($pairs)

# Match each completion to the earliest still-unmatched prior accepted pair for the same target.
# A completion is consumed once, so duplicate targets cannot share post-teleport evidence.
$usedAcceptLines = [System.Collections.Generic.HashSet[int]]::new()
$chains = [System.Collections.Generic.List[object]]::new()
foreach ($completion in $completions) {
    $matchingPair = $null
    foreach ($pair in $pairArray) {
        if ($pair.Accepted.line_number -ge $completion.line_number) { break }
        if (-not $usedAcceptLines.Contains($pair.Accepted.line_number) -and
            $pair.Accepted.fields.target -ceq $completion.fields.target) {
            $matchingPair = $pair
            break
        }
    }
    if ($null -eq $matchingPair) { continue }

    [void]$usedAcceptLines.Add($matchingPair.Accepted.line_number)
    $classification = Get-ResurrectionChainClassification -Started $matchingPair.Started `
        -Accepted $matchingPair.Accepted -Completed $completion
    [void]$chains.Add([pscustomobject]@{
        Started = $matchingPair.Started
        Accepted = $matchingPair.Accepted
        Completed = $completion
        Classification = $classification
    })
}
$chainArray = @($chains)
$qualifyingChains = @($chainArray | Where-Object { -not [string]::IsNullOrEmpty($_.Classification) })

$policyViolation = $starts | Where-Object {
    $_.fields.caster_combat -and -not $_.fields.combat_res
} | Select-Object -First 1

if ($null -ne $policyViolation) {
    $result = New-CorpseRescueResult -Status 'POLICY_VIOLATION' `
        -Reasons @('normal_resurrection_started_in_combat') -Evidence $eventArray `
        -MatchedEvidence @($policyViolation) -TotalLineCount $totalLineCount `
        -EffectiveEndLine $effectiveEndLine -PairCount $pairArray.Count `
        -ChainCount $chainArray.Count -QualifyingMatchCount $qualifyingChains.Count `
        -MalformedCount $malformed.Count `
        -ResolvedLogPath $logPath
    $result | ConvertTo-Json -Depth 12
    return
}

if ($malformed.Count -gt 0) {
    $result = New-CorpseRescueResult -Status 'INCOMPLETE' `
        -Reasons @('malformed_telemetry', "malformed_telemetry_line:$($malformed[0].line_number)") `
        -Evidence $eventArray -MatchedEvidence @($malformed[0]) -TotalLineCount $totalLineCount `
        -EffectiveEndLine $effectiveEndLine -PairCount $pairArray.Count `
        -ChainCount $chainArray.Count -QualifyingMatchCount $qualifyingChains.Count `
        -MalformedCount $malformed.Count `
        -ResolvedLogPath $logPath
    $result | ConvertTo-Json -Depth 12
    return
}

if ($eventArray.Count -eq 0) {
    $result = New-CorpseRescueResult -Status 'NO_SAMPLE' `
        -Reasons @('no_acceptance_relevant_corpse_rescue_telemetry') -Evidence @() `
        -MatchedEvidence @() -TotalLineCount $totalLineCount -EffectiveEndLine $effectiveEndLine `
        -PairCount 0 -ChainCount 0 -QualifyingMatchCount 0 -MalformedCount 0 `
        -ResolvedLogPath $logPath
    $result | ConvertTo-Json -Depth 12
    return
}

if ($qualifyingChains.Count -gt 0) {
    $selected = $qualifyingChains[0]
    $result = New-CorpseRescueResult -Status $selected.Classification -Reasons @() `
        -Evidence $eventArray `
        -MatchedEvidence @($selected.Started, $selected.Accepted, $selected.Completed) `
        -TotalLineCount $totalLineCount -EffectiveEndLine $effectiveEndLine `
        -PairCount $pairArray.Count -ChainCount $chainArray.Count `
        -QualifyingMatchCount $qualifyingChains.Count -MalformedCount 0 `
        -ResolvedLogPath $logPath
    $result | ConvertTo-Json -Depth 12
    return
}

$incompleteReasons = [System.Collections.Generic.List[string]]::new()
foreach ($chain in $chainArray) {
    if (-not $chain.Completed.fields.alive) {
        [void]$incompleteReasons.Add('matching_completion_not_alive')
    }
    elseif ($chain.Started.fields.mode -eq 'corpse' -and -not $chain.Accepted.fields.released) {
        [void]$incompleteReasons.Add('corpse_mode_accept_not_released')
    }
    elseif ($chain.Started.fields.combat_res -and -not $chain.Started.fields.caster_combat) {
        [void]$incompleteReasons.Add('combat_res_without_combat_context')
    }
}
if (@($validEvents | Where-Object { $_.event -eq 'cast_blocked' }).Count -gt 0) {
    [void]$incompleteReasons.Add('combat_cast_blocked_without_qualifying_completion')
}
if ($starts.Count -gt $pairArray.Count) {
    [void]$incompleteReasons.Add('cast_started_without_matching_accept')
}
if ($accepts.Count -gt $pairArray.Count) {
    [void]$incompleteReasons.Add('accept_without_matching_cast_started')
}
if ($pairArray.Count -gt $chainArray.Count) {
    [void]$incompleteReasons.Add('accept_without_matching_completion')
}
if ($completions.Count -gt $chainArray.Count) {
    [void]$incompleteReasons.Add('completion_without_matching_accept')
}
if ($incompleteReasons.Count -eq 0) {
    [void]$incompleteReasons.Add('no_qualifying_complete_chain')
}

$incompleteEvidence = if ($chainArray.Count -gt 0) {
    @($chainArray[0].Started, $chainArray[0].Accepted, $chainArray[0].Completed)
}
elseif ($pairArray.Count -gt 0) {
    @($pairArray[0].Started, $pairArray[0].Accepted)
}
else {
    @($eventArray[0])
}
$result = New-CorpseRescueResult -Status 'INCOMPLETE' -Reasons @($incompleteReasons) `
    -Evidence $eventArray -MatchedEvidence $incompleteEvidence -TotalLineCount $totalLineCount `
    -EffectiveEndLine $effectiveEndLine -PairCount $pairArray.Count `
    -ChainCount $chainArray.Count -QualifyingMatchCount 0 -MalformedCount 0 `
    -ResolvedLogPath $logPath
$result | ConvertTo-Json -Depth 12
