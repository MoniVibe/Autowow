<#
    Phase 1 quest acceptance verifier.

    The default mode is offline and read-only: it consumes JSONL evidence and an optional saved
    `questobjective` response, then writes deterministic JSON and Markdown reports.  `-LiveSnapshot`
    is also dry-run by default.  Only `-LiveSnapshot -AllowLiveRead` may open a loopback socket, and
    that path sends exactly one `questobjective <bot-guid>` request; it never uses the control script
    and never sends `quest`, `recover`, or another control order.

    The input field aliases below are intentionally kept in one small map.  The bridge payload is
    the source-confirmed, currently unversioned contract in the Phase 1 source worktree.  Versioned
    evidence should use schema `autowow.phase1.acceptance.evidence.v1` and `schema_version = 1`.
#>
[CmdletBinding()]
param(
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$EvidencePath = '',
    [string]$BridgeSnapshotPath = '',
    [string]$JsonReportPath = '',
    [string]$MarkdownReportPath = '',
    [switch]$OfflineFixture,
    [switch]$LiveSnapshot,
    [switch]$AllowLiveRead,
    [uint32]$BotGuid = 0,
    [string]$BridgeHost = '127.0.0.1',
    [ValidateRange(1, 65535)][int]$BridgePort = 18787,
    [ValidateRange(100, 60000)][int]$TimeoutMs = 4000
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:Phase1VerifierSchema = 'autowow.phase1.acceptance.verifier.v1'
$script:Phase1EvidenceSchema = 'autowow.phase1.acceptance.evidence.v1'
$script:Phase1AcceptedEvidenceSchemas = @(
    'autowow.phase1.acceptance.evidence.v1',
    'autowow.phase1.acceptance.v1'
)
$script:Phase1AcceptedBridgeSchemas = @('autowow.bridge.questobjective.v1')

# Expected Phase 1 fixture facts.  These are the only fixture rules used by the verifier.
$script:Phase1Fixtures = @(
    [pscustomobject][ordered]@{
        id = 'q459'; quest_id = 459; objective_family = 'item'; objective_kind = 'collect_item'
        required_item_id = 3297; required_npc_or_go_entry = $null; required_count = $null
        allowed_source_entries = @(1988, 1989); finisher_entry = 1992
    },
    [pscustomobject][ordered]@{
        id = 'q789'; quest_id = 789; objective_family = 'item'; objective_kind = 'collect_item'
        required_item_id = 4862; required_npc_or_go_entry = $null; required_count = $null
        allowed_source_entries = @(3124, 3281); finisher_entry = 3143
    },
    [pscustomobject][ordered]@{
        id = 'q792'; quest_id = 792; objective_family = 'npc_or_gameobject'; objective_kind = 'creature_credit'
        required_item_id = $null; required_npc_or_go_entry = 3101; required_count = 8
        allowed_source_entries = @(3101); finisher_entry = 3145
    },
    [pscustomobject][ordered]@{
        id = 'q916'; quest_id = 916; objective_family = 'item'; objective_kind = 'collect_item'
        required_item_id = 5166; required_npc_or_go_entry = $null; required_count = $null
        allowed_source_entries = @(1986); finisher_entry = 2082
    }
)

# Expected input names.  Do not add a new alias without documenting it here and in the .md file.
$script:Phase1FieldMap = [ordered]@{
    schema = @('schema')
    schema_version = @('schema_version', 'version')
    event = @('event')
    scope = @('scope')
    fixture_id = @('fixture_id')
    quest_id = @('quest_id', 'objective.quest_id')
    bot_guid = @('bot_guid', 'guid')
    ok = @('ok')
    error = @('error')
    objective = @('objective')
    objective_family = @('objective_family', 'objective.objective_family', 'objective.family')
    objective_kind = @('objective_kind', 'objective.objective_kind', 'objective.kind')
    objective_slot = @('objective_slot', 'objective.objective_slot', 'objective.slot')
    required_npc_or_go_entry = @('required_npc_or_go_entry', 'objective.required_npc_or_go_entry', 'objective.required_entry')
    required_item_id = @('required_item_id', 'objective.required_item_id', 'objective.required_item')
    current_count = @('current_count', 'objective.current_count')
    required_count = @('required_count', 'objective.required_count')
    baseline_count = @('baseline_count', 'objective.baseline_count')
    selected_source_entry = @('selected_source_entry', 'objective.selected_source_entry')
    selected_target_guid = @('selected_target_guid', 'objective.selected_target_guid')
    finisher_entry = @('finisher_entry', 'objective.finisher_entry', 'finisher.entry')
    finisher_guid = @('finisher_guid', 'objective.finisher_guid', 'finisher.guid')
    phase = @('phase', 'objective.phase')
    failure_reason = @('failure_reason', 'objective.failure_reason')
    supported = @('supported', 'objective.supported')
    has_lock = @('has_lock', 'objective.has_lock')
    resolved_source_entries = @(
        'resolved_source_entries', 'allowed_source_entries', 'objective.resolved_source_entries',
        'objective.allowed_source_entries'
    )
    exact_finisher_match = @('exact_finisher_match', 'invariants.exact_finisher_match')
    reward_postcondition_confirmed = @(
        'reward_postcondition_confirmed', 'reward_confirmed', 'reward.turnin_verified', 'invariants.reward_postcondition_confirmed'
    )
    objective_context_present = @(
        'objective_context_present', 'invariants.objective_context_present'
    )
    unrelated_offensive_target_count = @(
        'unrelated_offensive_target_count', 'invariants.unrelated_offensive_target_count'
    )
    random_grind_fallback_count = @(
        'random_grind_fallback_count', 'random_grind_fallbacks', 'invariants.random_grind_fallback_count',
        'invariants.random_grind_fallbacks'
    )
    teleport_used = @('teleport_used', 'invariants.teleport_used')
    teleport_count = @('teleport_count', 'invariants.teleport_count')
    direct_quest_db_mutation_count = @(
        'direct_quest_db_mutation_count', 'invariants.direct_quest_db_mutation_count',
        'invariants.direct_quest_db_mutations'
    )
}

function Get-Phase1PropertyPathValue {
    [CmdletBinding()]
    param(
        [AllowNull()][object]$InputObject,
        [Parameter(Mandatory = $true)][string]$Path
    )

    $current = $InputObject
    foreach ($part in ($Path -split '\.')) {
        if ($null -eq $current) {
            return [pscustomobject]@{ found = $false; value = $null; path = $Path }
        }

        if ($current -is [System.Collections.IDictionary]) {
            if (-not $current.Contains($part)) {
                return [pscustomobject]@{ found = $false; value = $null; path = $Path }
            }
            $current = $current[$part]
            continue
        }

        $property = $current.PSObject.Properties[$part]
        if ($null -eq $property) {
            return [pscustomobject]@{ found = $false; value = $null; path = $Path }
        }
        $current = $property.Value
    }

    return [pscustomobject]@{ found = $true; value = $current; path = $Path }
}

function Get-Phase1MappedField {
    [CmdletBinding()]
    param(
        [AllowNull()][object]$Record,
        [Parameter(Mandatory = $true)][string]$Name
    )

    if (-not $script:Phase1FieldMap.Contains($Name)) {
        throw "Unknown Phase 1 field mapping '$Name'."
    }

    foreach ($path in @($script:Phase1FieldMap[$Name])) {
        $candidate = Get-Phase1PropertyPathValue -InputObject $Record -Path $path
        if ($candidate.found) {
            return $candidate
        }
    }

    return [pscustomobject]@{ found = $false; value = $null; path = $null }
}

function ConvertTo-Phase1Int64 {
    [CmdletBinding()]
    param([AllowNull()][object]$Value)

    if ($null -eq $Value -or $Value -is [bool]) {
        return [pscustomobject]@{ valid = $false; value = $null }
    }

    $parsed = [int64]0
    $text = [string]$Value
    if ([int64]::TryParse(
            $text,
            [System.Globalization.NumberStyles]::Integer,
            [System.Globalization.CultureInfo]::InvariantCulture,
            [ref]$parsed)) {
        return [pscustomobject]@{ valid = $true; value = $parsed }
    }

    return [pscustomobject]@{ valid = $false; value = $null }
}

function ConvertTo-Phase1Bool {
    [CmdletBinding()]
    param([AllowNull()][object]$Value)

    if ($Value -is [bool]) {
        return [pscustomobject]@{ valid = $true; value = [bool]$Value }
    }

    $numeric = ConvertTo-Phase1Int64 -Value $Value
    if ($numeric.valid -and $numeric.value -in @(0, 1)) {
        return [pscustomobject]@{ valid = $true; value = ($numeric.value -eq 1) }
    }

    switch -Regex (([string]$Value).Trim().ToLowerInvariant()) {
        '^true$' { return [pscustomobject]@{ valid = $true; value = $true } }
        '^false$' { return [pscustomobject]@{ valid = $true; value = $false } }
        '^yes$' { return [pscustomobject]@{ valid = $true; value = $true } }
        '^no$' { return [pscustomobject]@{ valid = $true; value = $false } }
    }

    return [pscustomobject]@{ valid = $false; value = $null }
}

function Add-Phase1Diagnostic {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][System.Collections.Generic.List[object]]$Diagnostics,
        [Parameter(Mandatory = $true)][string]$Code,
        [Parameter(Mandatory = $true)][string]$Message,
        [ValidateSet('info', 'warning', 'error')][string]$Severity = 'warning',
        [string]$Source = '',
        [int]$Line = 0
    )

    [void]$Diagnostics.Add([pscustomobject][ordered]@{
            code = $Code
            severity = $Severity
            message = $Message
            source = $Source
            line = $Line
        })
}

function Get-Phase1AbsolutePath {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][string]$Path)

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path (Get-Location).Path $Path)
    )
}

function Get-Phase1JsonObjectsFromFile {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [ValidateSet('evidence', 'bridge')][string]$Kind,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][System.Collections.Generic.List[object]]$Diagnostics
    )

    $absolute = Get-Phase1AbsolutePath -Path $Path
    $objects = [System.Collections.Generic.List[object]]::new()
    if (-not (Test-Path -LiteralPath $absolute -PathType Leaf)) {
        Add-Phase1Diagnostic -Diagnostics $Diagnostics -Code 'input_file_missing' -Severity 'error' `
            -Message "Input file was not found: $absolute" -Source $absolute
        return @()
    }

    $lines = [System.IO.File]::ReadAllLines($absolute)
    if ($Kind -eq 'bridge') {
        $wholeText = [System.IO.File]::ReadAllText($absolute).Trim()
        if (-not [string]::IsNullOrWhiteSpace($wholeText)) {
            try {
                $whole = $wholeText | ConvertFrom-Json -Depth 100 -ErrorAction Stop
                if ($whole -is [System.Array]) {
                    $index = 0
                    foreach ($item in @($whole)) {
                        $index++
                        [void]$objects.Add([pscustomobject]@{ object = $item; source = $absolute; line = $index })
                    }
                }
                else {
                    [void]$objects.Add([pscustomobject]@{ object = $whole; source = $absolute; line = 1 })
                }
                return @($objects)
            }
            catch {
                # A bridge capture can also be JSONL.  Fall through to the line parser.
            }
        }
    }

    $lineNumber = 0
    foreach ($line in $lines) {
        $lineNumber++
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        try {
            $parsed = $line | ConvertFrom-Json -Depth 100 -ErrorAction Stop
            if ($parsed -is [System.Array]) {
                Add-Phase1Diagnostic -Diagnostics $Diagnostics -Code 'json_record_array_unsupported' `
                    -Message "A JSONL record must be an object, not an array." -Source $absolute -Line $lineNumber
                continue
            }
            [void]$objects.Add([pscustomobject]@{ object = $parsed; source = $absolute; line = $lineNumber })
        }
        catch {
            Add-Phase1Diagnostic -Diagnostics $Diagnostics -Code 'jsonl_parse_error' -Severity 'error' `
                -Message "Could not parse JSONL record at line $lineNumber." -Source $absolute -Line $lineNumber
        }
    }

    return @($objects)
}

function Test-Phase1BridgeShape {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][object]$Record)

    $guid = Get-Phase1MappedField -Record $Record -Name 'bot_guid'
    $objective = Get-Phase1MappedField -Record $Record -Name 'objective'
    $ok = Get-Phase1MappedField -Record $Record -Name 'ok'
    return ($guid.found -and $objective.found -and $ok.found)
}

function ConvertTo-Phase1NormalizedRecord {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][object]$Object,
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][int]$Line,
        [ValidateSet('evidence', 'bridge', 'offline')][string]$SourceKind,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][System.Collections.Generic.List[object]]$Diagnostics
    )

    $schemaField = Get-Phase1MappedField -Record $Object -Name 'schema'
    $versionField = Get-Phase1MappedField -Record $Object -Name 'schema_version'
    $schema = if ($schemaField.found -and $null -ne $schemaField.value) { [string]$schemaField.value } else { '' }
    $version = ConvertTo-Phase1Int64 -Value $versionField.value
    $isBridge = ($SourceKind -eq 'bridge') -or (Test-Phase1BridgeShape -Record $Object)
    $accepted = $true
    $schemaStatus = 'VERSIONED'
    $schemaGap = $false
    $recordType = 'phase1_evidence'

    if ($isBridge) {
        $recordType = 'bridge_questobjective'
        if ([string]::IsNullOrWhiteSpace($schema)) {
            $schemaStatus = 'UNVERSIONED_BRIDGE_CONTRACT'
            $schemaGap = $true
            Add-Phase1Diagnostic -Diagnostics $Diagnostics -Code 'bridge_schema_unversioned' -Severity 'warning' `
                -Message 'The current questobjective bridge response has no schema/version field; only source-confirmed fields are consumed.' `
                -Source $Source -Line $Line
        }
        elseif ($schema -notin $script:Phase1AcceptedBridgeSchemas) {
            $accepted = $false
            $schemaStatus = 'REJECTED'
            Add-Phase1Diagnostic -Diagnostics $Diagnostics -Code 'bridge_schema_unsupported' -Severity 'error' `
                -Message "Unsupported questobjective bridge schema '$schema'." -Source $Source -Line $Line
        }
        elseif (-not $version.valid -or $version.value -ne 1) {
            $accepted = $false
            $schemaStatus = 'REJECTED'
            Add-Phase1Diagnostic -Diagnostics $Diagnostics -Code 'bridge_schema_version_unsupported' -Severity 'error' `
                -Message "Unsupported questobjective bridge schema version '$($versionField.value)'." -Source $Source -Line $Line
        }

        $okField = Get-Phase1MappedField -Record $Object -Name 'ok'
        if (-not $okField.found) {
            Add-Phase1Diagnostic -Diagnostics $Diagnostics -Code 'bridge_required_field_missing' -Severity 'error' `
                -Message 'Bridge response is missing required field ok.' -Source $Source -Line $Line
        }
        else {
            $ok = ConvertTo-Phase1Bool -Value $okField.value
            if (-not $ok.valid) {
                Add-Phase1Diagnostic -Diagnostics $Diagnostics -Code 'bridge_field_type_invalid' -Severity 'error' `
                    -Message 'Bridge field ok must be boolean or 0/1.' -Source $Source -Line $Line
            }
            elseif (-not $ok.value) {
                $errorField = Get-Phase1MappedField -Record $Object -Name 'error'
                $errorText = if ($errorField.found) { [string]$errorField.value } else { 'unspecified bridge error' }
                Add-Phase1Diagnostic -Diagnostics $Diagnostics -Code 'bridge_response_not_ok' -Severity 'warning' `
                    -Message "Bridge response reported ok=false ($errorText); no success is inferred." -Source $Source -Line $Line
            }
        }

        foreach ($requiredPath in @('guid', 'objective')) {
            $required = Get-Phase1PropertyPathValue -InputObject $Object -Path $requiredPath
            if (-not $required.found) {
                Add-Phase1Diagnostic -Diagnostics $Diagnostics -Code 'bridge_required_field_missing' -Severity 'error' `
                    -Message "Bridge response is missing required field $requiredPath." -Source $Source -Line $Line
            }
        }

        $objectiveField = Get-Phase1PropertyPathValue -InputObject $Object -Path 'objective'
        if ($objectiveField.found -and $null -ne $objectiveField.value) {
            foreach ($objectivePath in @(
                    'quest_id', 'objective_family', 'objective_slot', 'objective_kind', 'phase',
                    'failure_reason', 'supported', 'required_npc_or_go_entry', 'required_item_id',
                    'current_count', 'required_count', 'baseline_count', 'selected_source_entry',
                    'selected_target_guid', 'finisher_entry', 'finisher_guid', 'has_lock')) {
                $required = Get-Phase1PropertyPathValue -InputObject $objectiveField.value -Path $objectivePath
                if (-not $required.found) {
                    Add-Phase1Diagnostic -Diagnostics $Diagnostics -Code 'bridge_objective_field_missing' -Severity 'warning' `
                        -Message "Bridge objective is missing source-contract field objective.$objectivePath." -Source $Source -Line $Line
                }
            }
        }
    }
    else {
        if ([string]::IsNullOrWhiteSpace($schema)) {
            $schemaStatus = 'UNVERSIONED_INPUT'
            $schemaGap = $true
            Add-Phase1Diagnostic -Diagnostics $Diagnostics -Code 'input_schema_unversioned' -Severity 'warning' `
                -Message 'Evidence record has no schema/version; known aliases may be read, but the report cannot be a release PASS.' `
                -Source $Source -Line $Line
        }
        elseif ($schema -notin $script:Phase1AcceptedEvidenceSchemas) {
            $accepted = $false
            $schemaStatus = 'REJECTED'
            Add-Phase1Diagnostic -Diagnostics $Diagnostics -Code 'input_schema_unsupported' -Severity 'error' `
                -Message "Unsupported evidence schema '$schema'." -Source $Source -Line $Line
        }
        elseif (-not $version.valid -or $version.value -ne 1) {
            $accepted = $false
            $schemaStatus = 'REJECTED'
            Add-Phase1Diagnostic -Diagnostics $Diagnostics -Code 'input_schema_version_unsupported' -Severity 'error' `
                -Message "Unsupported evidence schema version '$($versionField.value)'." -Source $Source -Line $Line
        }

        $eventField = Get-Phase1MappedField -Record $Object -Name 'event'
        if (-not $eventField.found -and $SourceKind -ne 'offline') {
            Add-Phase1Diagnostic -Diagnostics $Diagnostics -Code 'evidence_event_missing' -Severity 'warning' `
                -Message 'Versioned evidence should include event.' -Source $Source -Line $Line
        }
    }

    return [pscustomobject][ordered]@{
        raw = $Object
        source = $Source
        line = $Line
        source_kind = $SourceKind
        record_type = $recordType
        accepted = $accepted
        schema_status = $schemaStatus
        schema_gap = $schemaGap
    }
}

function Get-Phase1RecordQuestId {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][object]$Record)

    $field = Get-Phase1MappedField -Record $Record.raw -Name 'quest_id'
    if ($field.found) {
        $parsed = ConvertTo-Phase1Int64 -Value $field.value
        if ($parsed.valid -and $parsed.value -gt 0) {
            return [pscustomobject]@{ found = $true; valid = $true; value = $parsed.value }
        }
        return [pscustomobject]@{ found = $true; valid = $false; value = $null }
    }

    $fixture = Get-Phase1MappedField -Record $Record.raw -Name 'fixture_id'
    if ($fixture.found -and $null -ne $fixture.value) {
        $match = [regex]::Match(([string]$fixture.value).Trim(), '^(?:q)?(?<id>[0-9]+)$', 'IgnoreCase')
        if ($match.Success) {
            return [pscustomobject]@{ found = $true; valid = $true; value = [int64]$match.Groups['id'].Value }
        }
        return [pscustomobject]@{ found = $true; valid = $false; value = $null }
    }

    return [pscustomobject]@{ found = $false; valid = $false; value = $null }
}

function Test-Phase1RecordHasInvariant {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][object]$Record)

    foreach ($name in @(
            'unrelated_offensive_target_count', 'random_grind_fallback_count', 'teleport_used',
            'teleport_count', 'direct_quest_db_mutation_count', 'exact_finisher_match',
            'reward_postcondition_confirmed', 'objective_context_present')) {
        $field = Get-Phase1MappedField -Record $Record.raw -Name $name
        if ($field.found -and $null -ne $field.value) { return $true }
    }
    return $false
}

function Get-Phase1FixtureRecords {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][int64]$QuestId,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]]$Records
    )

    $selected = [System.Collections.Generic.List[object]]::new()
    foreach ($record in @($Records)) {
        if (-not $record.accepted) { continue }
        $recordQuest = Get-Phase1RecordQuestId -Record $record
        if ($recordQuest.found -and $recordQuest.valid) {
            if ($recordQuest.value -eq $QuestId) { [void]$selected.Add($record) }
        }
        elseif (-not $recordQuest.found -and (Test-Phase1RecordHasInvariant -Record $record)) {
            # A release_invariants record without quest_id is explicitly run-global by contract.
            [void]$selected.Add($record)
        }
    }
    return @($selected)
}

function Get-Phase1FieldObservations {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]]$Records,
        [Parameter(Mandatory = $true)][string]$Name
    )

    $observations = [System.Collections.Generic.List[object]]::new()
    foreach ($record in @($Records)) {
        if (-not $record.accepted) { continue }
        $field = Get-Phase1MappedField -Record $record.raw -Name $Name
        if ($field.found -and $null -ne $field.value) {
            [void]$observations.Add([pscustomobject][ordered]@{
                    source = $record.source
                    line = $record.line
                    path = $field.path
                    value = $field.value
                })
        }
    }
    return @($observations | Sort-Object source, line, path)
}

function ConvertTo-Phase1ObservationSummary {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][object[]]$Observations)

    $parts = [System.Collections.Generic.List[string]]::new()
    foreach ($observation in @($Observations)) {
        $value = try { $observation.value | ConvertTo-Json -Depth 8 -Compress } catch { [string]$observation.value }
        [void]$parts.Add("$($observation.source):$($observation.line)=$value")
    }
    return ($parts -join '; ')
}

function New-Phase1Check {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][ValidateSet('PASS', 'FAIL', 'INCONCLUSIVE', 'NOT_OBSERVED', 'UNSUPPORTED')][string]$Status,
        [Parameter(Mandatory = $true)][bool]$RequiredForPass,
        [Parameter(Mandatory = $true)][int]$ObservedCount,
        [Parameter(Mandatory = $true)][string]$Details
    )

    return [pscustomobject][ordered]@{
        name = $Name
        status = $Status
        required_for_pass = $RequiredForPass
        observed_count = $ObservedCount
        details = $Details
    }
}

function New-Phase1ExactCheck {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]]$Records,
        [Parameter(Mandatory = $true)][string]$FieldName,
        [Parameter(Mandatory = $true)][string]$CheckName,
        [Parameter(Mandatory = $true)][object]$Expected,
        [Parameter(Mandatory = $true)][bool]$RequiredForPass,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][System.Collections.Generic.List[object]]$Diagnostics,
        [switch]$Numeric
    )

    $observations = @(Get-Phase1FieldObservations -Records $Records -Name $FieldName)
    if ($observations.Count -eq 0) {
        return New-Phase1Check -Name $CheckName -Status 'NOT_OBSERVED' -RequiredForPass $RequiredForPass `
            -ObservedCount 0 -Details 'required field was not observed'
    }

    $invalid = 0
    $mismatch = 0
    $matches = 0
    foreach ($observation in $observations) {
        if ($Numeric) {
            $parsed = ConvertTo-Phase1Int64 -Value $observation.value
            if (-not $parsed.valid) { $invalid++; continue }
            if ([int64]$parsed.value -eq [int64]$Expected) { $matches++ } else { $mismatch++ }
        }
        else {
            if ([string]$observation.value -ieq [string]$Expected) { $matches++ } else { $mismatch++ }
        }
    }

    if ($mismatch -gt 0) {
        return New-Phase1Check -Name $CheckName -Status 'FAIL' -RequiredForPass $RequiredForPass `
            -ObservedCount $observations.Count -Details "expected $Expected; observed $(ConvertTo-Phase1ObservationSummary -Observations $observations)"
    }
    if ($invalid -gt 0 -or $matches -eq 0) {
        Add-Phase1Diagnostic -Diagnostics $Diagnostics -Code 'field_type_invalid' `
            -Message "Field $FieldName contained a value with the wrong type." `
            -Source ($observations[0].source) -Line ([int]$observations[0].line)
        return New-Phase1Check -Name $CheckName -Status 'INCONCLUSIVE' -RequiredForPass $RequiredForPass `
            -ObservedCount $observations.Count -Details "expected $Expected; field type or value was unusable"
    }

    return New-Phase1Check -Name $CheckName -Status 'PASS' -RequiredForPass $RequiredForPass `
        -ObservedCount $observations.Count -Details "matched $Expected"
}

function New-Phase1PositiveCountCheck {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]]$Records,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][System.Collections.Generic.List[object]]$Diagnostics
    )

    $observations = @(Get-Phase1FieldObservations -Records $Records -Name 'required_count')
    if ($observations.Count -eq 0) {
        return New-Phase1Check -Name 'required_count_observed' -Status 'NOT_OBSERVED' -RequiredForPass $true `
            -ObservedCount 0 -Details 'required_count was not observed'
    }

    $invalid = 0
    $nonPositive = 0
    foreach ($observation in $observations) {
        $parsed = ConvertTo-Phase1Int64 -Value $observation.value
        if (-not $parsed.valid) { $invalid++; continue }
        if ($parsed.value -le 0) { $nonPositive++ }
    }
    if ($nonPositive -gt 0) {
        return New-Phase1Check -Name 'required_count_observed' -Status 'FAIL' -RequiredForPass $true `
            -ObservedCount $observations.Count -Details "required_count must be positive; observed $(ConvertTo-Phase1ObservationSummary -Observations $observations)"
    }
    if ($invalid -gt 0) {
        Add-Phase1Diagnostic -Diagnostics $Diagnostics -Code 'required_count_type_invalid' `
            -Message 'required_count was present but not an integer.' -Source $observations[0].source -Line ([int]$observations[0].line)
        return New-Phase1Check -Name 'required_count_observed' -Status 'INCONCLUSIVE' -RequiredForPass $true `
            -ObservedCount $observations.Count -Details 'required_count was not an integer'
    }
    return New-Phase1Check -Name 'required_count_observed' -Status 'PASS' -RequiredForPass $true `
        -ObservedCount $observations.Count -Details 'positive required_count observed'
}

function New-Phase1CountAdvanceCheck {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]]$Records,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][System.Collections.Generic.List[object]]$Diagnostics
    )

    $pairs = [System.Collections.Generic.List[object]]::new()
    foreach ($record in @($Records)) {
        if (-not $record.accepted) { continue }
        $baseline = Get-Phase1MappedField -Record $record.raw -Name 'baseline_count'
        $current = Get-Phase1MappedField -Record $record.raw -Name 'current_count'
        if (-not $baseline.found -or -not $current.found -or $null -eq $baseline.value -or $null -eq $current.value) { continue }
        $b = ConvertTo-Phase1Int64 -Value $baseline.value
        $c = ConvertTo-Phase1Int64 -Value $current.value
        if (-not $b.valid -or -not $c.valid) {
            Add-Phase1Diagnostic -Diagnostics $Diagnostics -Code 'count_type_invalid' `
                -Message 'baseline_count and current_count must be integers.' -Source $record.source -Line $record.line
            continue
        }
        [void]$pairs.Add([pscustomobject]@{ baseline = $b.value; current = $c.value; source = $record.source; line = $record.line })
    }

    if ($pairs.Count -eq 0) {
        return New-Phase1Check -Name 'objective_count_advances_from_baseline' -Status 'NOT_OBSERVED' `
            -RequiredForPass $true -ObservedCount 0 -Details 'paired baseline_count/current_count evidence was not observed'
    }
    if (@($pairs | Where-Object { $_.current -gt $_.baseline }).Count -gt 0) {
        return New-Phase1Check -Name 'objective_count_advances_from_baseline' -Status 'PASS' `
            -RequiredForPass $true -ObservedCount $pairs.Count -Details "at least one count advanced: $($pairs[0].baseline) -> $($pairs[0].current)"
    }
    return New-Phase1Check -Name 'objective_count_advances_from_baseline' -Status 'FAIL' -RequiredForPass $true `
        -ObservedCount $pairs.Count -Details "no observed count exceeded its baseline"
}

function New-Phase1SupportedCheck {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]]$Records,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][System.Collections.Generic.List[object]]$Diagnostics
    )

    $observations = @(Get-Phase1FieldObservations -Records $Records -Name 'supported')
    if ($observations.Count -eq 0) {
        return New-Phase1Check -Name 'objective_supported' -Status 'NOT_OBSERVED' -RequiredForPass $true `
            -ObservedCount 0 -Details 'supported was not observed'
    }
    $falseCount = 0
    $invalid = 0
    foreach ($observation in $observations) {
        $parsed = ConvertTo-Phase1Bool -Value $observation.value
        if (-not $parsed.valid) { $invalid++; continue }
        if (-not $parsed.value) { $falseCount++ }
    }
    if ($falseCount -gt 0) {
        return New-Phase1Check -Name 'objective_supported' -Status 'UNSUPPORTED' -RequiredForPass $true `
            -ObservedCount $observations.Count -Details 'evidence explicitly reported supported=false'
    }
    if ($invalid -gt 0) {
        Add-Phase1Diagnostic -Diagnostics $Diagnostics -Code 'supported_type_invalid' `
            -Message 'supported was present but not boolean/0/1.' -Source $observations[0].source -Line ([int]$observations[0].line)
        return New-Phase1Check -Name 'objective_supported' -Status 'INCONCLUSIVE' -RequiredForPass $true `
            -ObservedCount $observations.Count -Details 'supported type was invalid'
    }
    return New-Phase1Check -Name 'objective_supported' -Status 'PASS' -RequiredForPass $true `
        -ObservedCount $observations.Count -Details 'all observed supported values were true'
}

function New-Phase1FailureReasonCheck {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]]$Records)

    $observations = @(Get-Phase1FieldObservations -Records $Records -Name 'failure_reason')
    if ($observations.Count -eq 0) {
        return New-Phase1Check -Name 'failure_reason_clear' -Status 'NOT_OBSERVED' -RequiredForPass $true `
            -ObservedCount 0 -Details 'failure_reason was not observed'
    }
    $failures = @($observations | Where-Object { [string]$_.value -and [string]$_.value -ine 'none' })
    if ($failures.Count -gt 0) {
        return New-Phase1Check -Name 'failure_reason_clear' -Status 'FAIL' -RequiredForPass $true `
            -ObservedCount $observations.Count -Details "non-none failure_reason observed: $(ConvertTo-Phase1ObservationSummary -Observations $failures)"
    }
    return New-Phase1Check -Name 'failure_reason_clear' -Status 'PASS' -RequiredForPass $true `
        -ObservedCount $observations.Count -Details 'all observed failure_reason values were none'
}

function New-Phase1ContextCheck {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]]$Records,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][System.Collections.Generic.List[object]]$Diagnostics
    )

    $explicit = @(Get-Phase1FieldObservations -Records $Records -Name 'objective_context_present')
    $observations = $explicit
    $usingLock = $false
    if ($observations.Count -eq 0) {
        $observations = @(Get-Phase1FieldObservations -Records $Records -Name 'has_lock')
        $usingLock = $true
    }
    if ($observations.Count -eq 0) {
        return New-Phase1Check -Name 'objective_context_present' -Status 'NOT_OBSERVED' -RequiredForPass $true `
            -ObservedCount 0 -Details 'objective context was not observed'
    }

    $valid = 0
    $trueCount = 0
    $falseCount = 0
    foreach ($observation in $observations) {
        $parsed = ConvertTo-Phase1Bool -Value $observation.value
        if (-not $parsed.valid) { continue }
        $valid++
        if ($parsed.value) { $trueCount++ } else { $falseCount++ }
    }
    if ($trueCount -gt 0) {
        return New-Phase1Check -Name 'objective_context_present' -Status 'PASS' -RequiredForPass $true `
            -ObservedCount $observations.Count -Details 'objective context was explicitly present'
    }
    if ($valid -eq 0) {
        Add-Phase1Diagnostic -Diagnostics $Diagnostics -Code 'objective_context_type_invalid' `
            -Message 'Objective context field was present but not boolean/0/1.' -Source $observations[0].source -Line ([int]$observations[0].line)
        return New-Phase1Check -Name 'objective_context_present' -Status 'INCONCLUSIVE' -RequiredForPass $true `
            -ObservedCount $observations.Count -Details 'objective context field type was invalid'
    }
    if ($usingLock) {
        # has_lock=false after a completed objective is a valid bridge state, but it is not proof
        # that the context was present during execution.
        $completed = $false
        foreach ($record in @($Records)) {
            $b = Get-Phase1MappedField -Record $record.raw -Name 'baseline_count'
            $c = Get-Phase1MappedField -Record $record.raw -Name 'current_count'
            $r = Get-Phase1MappedField -Record $record.raw -Name 'required_count'
            $bp = ConvertTo-Phase1Int64 -Value $b.value
            $cp = ConvertTo-Phase1Int64 -Value $c.value
            $rp = ConvertTo-Phase1Int64 -Value $r.value
            if ($bp.valid -and $cp.valid -and $rp.valid -and $cp.value -ge $rp.value) { $completed = $true }
        }
        if ($completed) {
            return New-Phase1Check -Name 'objective_context_present' -Status 'NOT_OBSERVED' -RequiredForPass $true `
                -ObservedCount $observations.Count -Details 'bridge reported has_lock=false after completion; presence during execution was not observed'
        }
    }
    return New-Phase1Check -Name 'objective_context_present' -Status 'FAIL' -RequiredForPass $true `
        -ObservedCount $observations.Count -Details 'evidence explicitly reported objective context absent'
}

function New-Phase1RewardCheck {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]]$Records,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][System.Collections.Generic.List[object]]$Diagnostics
    )

    $observations = @(Get-Phase1FieldObservations -Records $Records -Name 'reward_postcondition_confirmed')
    if ($observations.Count -eq 0) {
        return New-Phase1Check -Name 'reward_postcondition_confirmed' -Status 'NOT_OBSERVED' -RequiredForPass $true `
            -ObservedCount 0 -Details 'reward postcondition was not observed'
    }
    $falseCount = 0
    $invalid = 0
    foreach ($observation in $observations) {
        $parsed = ConvertTo-Phase1Bool -Value $observation.value
        if (-not $parsed.valid) { $invalid++; continue }
        if (-not $parsed.value) { $falseCount++ }
    }
    if ($falseCount -gt 0) {
        return New-Phase1Check -Name 'reward_postcondition_confirmed' -Status 'FAIL' -RequiredForPass $true `
            -ObservedCount $observations.Count -Details 'evidence explicitly reported reward postcondition false'
    }
    if ($invalid -gt 0) {
        Add-Phase1Diagnostic -Diagnostics $Diagnostics -Code 'reward_postcondition_type_invalid' `
            -Message 'Reward postcondition field was present but not boolean/0/1.' -Source $observations[0].source -Line ([int]$observations[0].line)
        return New-Phase1Check -Name 'reward_postcondition_confirmed' -Status 'INCONCLUSIVE' -RequiredForPass $true `
            -ObservedCount $observations.Count -Details 'reward postcondition field type was invalid'
    }
    return New-Phase1Check -Name 'reward_postcondition_confirmed' -Status 'PASS' -RequiredForPass $true `
        -ObservedCount $observations.Count -Details 'reward postcondition was confirmed'
}

function ConvertTo-Phase1EntryList {
    [CmdletBinding()]
    param([AllowNull()][object]$Value)

    $entries = [System.Collections.Generic.List[int64]]::new()
    $invalid = 0
    foreach ($item in @($Value)) {
        $candidate = $item
        if ($item -is [System.Collections.IDictionary] -or $item -is [pscustomobject]) {
            foreach ($path in @('entry', 'source_entry', 'id')) {
                $field = Get-Phase1PropertyPathValue -InputObject $item -Path $path
                if ($field.found) { $candidate = $field.value; break }
            }
        }
        $parsed = ConvertTo-Phase1Int64 -Value $candidate
        if (-not $parsed.valid -or $parsed.value -le 0) { $invalid++; continue }
        [void]$entries.Add($parsed.value)
    }
    return [pscustomobject]@{ values = @($entries | Sort-Object -Unique); invalid = $invalid }
}

function New-Phase1SourceCheck {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]]$Records,
        [Parameter(Mandatory = $true)][int64[]]$Allowed,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][System.Collections.Generic.List[object]]$Diagnostics
    )

    $observations = @(Get-Phase1FieldObservations -Records $Records -Name 'selected_source_entry')
    $resolvedObservations = @(Get-Phase1FieldObservations -Records $Records -Name 'resolved_source_entries')
    if ($observations.Count -eq 0 -and $resolvedObservations.Count -eq 0) {
        return New-Phase1Check -Name 'selected_source_is_allowed' -Status 'NOT_OBSERVED' -RequiredForPass $true `
            -ObservedCount 0 -Details 'selected_source_entry was not observed'
    }

    $resolvedInvalid = 0
    $resolvedMismatch = $false
    foreach ($resolved in $resolvedObservations) {
        $parsedList = ConvertTo-Phase1EntryList -Value $resolved.value
        $resolvedInvalid += $parsedList.invalid
        $expected = @($Allowed | Sort-Object -Unique)
        $missing = @($expected | Where-Object { $_ -notin $parsedList.values })
        $extra = @($parsedList.values | Where-Object { $_ -notin $expected })
        if ($missing.Count -gt 0 -or $extra.Count -gt 0) { $resolvedMismatch = $true }
    }
    if ($resolvedMismatch) {
        return New-Phase1Check -Name 'selected_source_is_allowed' -Status 'FAIL' -RequiredForPass $true `
            -ObservedCount ($observations.Count + $resolvedObservations.Count) -Details "resolved source list was not exactly [$($Allowed -join ', ')]"
    }
    if ($resolvedInvalid -gt 0) {
        Add-Phase1Diagnostic -Diagnostics $Diagnostics -Code 'resolved_source_type_invalid' `
            -Message 'resolved_source_entries/allowed_source_entries must contain positive integer entries.' `
            -Source $(if ($resolvedObservations.Count) { $resolvedObservations[0].source } else { '' }) `
            -Line ([int](if ($resolvedObservations.Count) { $resolvedObservations[0].line } else { 0 }))
        return New-Phase1Check -Name 'selected_source_is_allowed' -Status 'INCONCLUSIVE' -RequiredForPass $true `
            -ObservedCount ($observations.Count + $resolvedObservations.Count) -Details 'resolved source list contained an invalid entry'
    }

    $invalid = 0
    $notAllowed = 0
    $allowedSeen = 0
    foreach ($observation in $observations) {
        $parsed = ConvertTo-Phase1Int64 -Value $observation.value
        if (-not $parsed.valid) { $invalid++; continue }
        if ($parsed.value -eq 0) { continue }
        if ($parsed.value -in $Allowed) { $allowedSeen++ } else { $notAllowed++ }
    }
    if ($notAllowed -gt 0) {
        return New-Phase1Check -Name 'selected_source_is_allowed' -Status 'FAIL' -RequiredForPass $true `
            -ObservedCount ($observations.Count + $resolvedObservations.Count) -Details "selected source was outside allowlist [$($Allowed -join ', ')]"
    }
    if ($invalid -gt 0) {
        Add-Phase1Diagnostic -Diagnostics $Diagnostics -Code 'selected_source_type_invalid' `
            -Message 'selected_source_entry was present but not an integer.' -Source $observations[0].source -Line ([int]$observations[0].line)
        return New-Phase1Check -Name 'selected_source_is_allowed' -Status 'INCONCLUSIVE' -RequiredForPass $true `
            -ObservedCount ($observations.Count + $resolvedObservations.Count) -Details 'selected source type was invalid'
    }
    if ($allowedSeen -eq 0) {
        return New-Phase1Check -Name 'selected_source_is_allowed' -Status 'INCONCLUSIVE' -RequiredForPass $true `
            -ObservedCount ($observations.Count + $resolvedObservations.Count) -Details 'selected_source_entry was only zero/unselected'
    }
    return New-Phase1Check -Name 'selected_source_is_allowed' -Status 'PASS' -RequiredForPass $true `
        -ObservedCount ($observations.Count + $resolvedObservations.Count) -Details "selected source matched allowlist [$($Allowed -join ', ')]"
}

function New-Phase1ZeroInvariantCheck {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]]$Records,
        [Parameter(Mandatory = $true)][string]$FieldName,
        [Parameter(Mandatory = $true)][string]$CheckName,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][System.Collections.Generic.List[object]]$Diagnostics
    )

    $observations = @(Get-Phase1FieldObservations -Records $Records -Name $FieldName)
    if ($observations.Count -eq 0) {
        return New-Phase1Check -Name $CheckName -Status 'NOT_OBSERVED' -RequiredForPass $true `
            -ObservedCount 0 -Details 'invariant field was not observed'
    }
    $invalid = 0
    $nonZero = 0
    foreach ($observation in $observations) {
        $parsed = ConvertTo-Phase1Int64 -Value $observation.value
        if (-not $parsed.valid) { $invalid++; continue }
        if ($parsed.value -ne 0) { $nonZero++ }
    }
    if ($nonZero -gt 0) {
        return New-Phase1Check -Name $CheckName -Status 'FAIL' -RequiredForPass $true `
            -ObservedCount $observations.Count -Details "non-zero value observed: $(ConvertTo-Phase1ObservationSummary -Observations $observations)"
    }
    if ($invalid -gt 0) {
        Add-Phase1Diagnostic -Diagnostics $Diagnostics -Code 'invariant_type_invalid' `
            -Message "$FieldName must be an integer." -Source $observations[0].source -Line ([int]$observations[0].line)
        return New-Phase1Check -Name $CheckName -Status 'INCONCLUSIVE' -RequiredForPass $true `
            -ObservedCount $observations.Count -Details 'invariant type was invalid'
    }
    return New-Phase1Check -Name $CheckName -Status 'PASS' -RequiredForPass $true `
        -ObservedCount $observations.Count -Details 'all observed values were zero'
}

function New-Phase1TeleportCheck {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]]$Records,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][System.Collections.Generic.List[object]]$Diagnostics
    )

    $used = @(Get-Phase1FieldObservations -Records $Records -Name 'teleport_used')
    $count = @(Get-Phase1FieldObservations -Records $Records -Name 'teleport_count')
    if ($used.Count -eq 0 -and $count.Count -eq 0) {
        return New-Phase1Check -Name 'teleport_not_used' -Status 'NOT_OBSERVED' -RequiredForPass $true `
            -ObservedCount 0 -Details 'teleport_used/teleport_count was not observed'
    }
    $invalid = 0
    $usedFailure = $false
    foreach ($observation in $used) {
        $parsed = ConvertTo-Phase1Bool -Value $observation.value
        if (-not $parsed.valid) { $invalid++; continue }
        if ($parsed.value) { $usedFailure = $true }
    }
    foreach ($observation in $count) {
        $parsed = ConvertTo-Phase1Int64 -Value $observation.value
        if (-not $parsed.valid) { $invalid++; continue }
        if ($parsed.value -ne 0) { $usedFailure = $true }
    }
    if ($usedFailure) {
        return New-Phase1Check -Name 'teleport_not_used' -Status 'FAIL' -RequiredForPass $true `
            -ObservedCount ($used.Count + $count.Count) -Details 'teleport was explicitly used or counted'
    }
    if ($invalid -gt 0) {
        Add-Phase1Diagnostic -Diagnostics $Diagnostics -Code 'teleport_field_type_invalid' `
            -Message 'teleport_used must be boolean/0/1 and teleport_count must be an integer.' `
            -Source $(if ($used.Count) { $used[0].source } else { $count[0].source }) `
            -Line ([int](if ($used.Count) { $used[0].line } else { $count[0].line }))
        return New-Phase1Check -Name 'teleport_not_used' -Status 'INCONCLUSIVE' -RequiredForPass $true `
            -ObservedCount ($used.Count + $count.Count) -Details 'teleport evidence type was invalid'
    }
    return New-Phase1Check -Name 'teleport_not_used' -Status 'PASS' -RequiredForPass $true `
        -ObservedCount ($used.Count + $count.Count) -Details 'teleport_used=false/0 and teleport_count=0 where observed'
}

function New-Phase1ExactFinisherInvariantCheck {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]]$Records,
        [Parameter(Mandatory = $true)][int64]$ExpectedFinisher,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][System.Collections.Generic.List[object]]$Diagnostics
    )

    $entryCheck = New-Phase1ExactCheck -Records $Records -FieldName 'finisher_entry' -CheckName 'finisher_entry_exact' `
        -Expected $ExpectedFinisher -RequiredForPass $true -Diagnostics $Diagnostics -Numeric
    $explicit = @(Get-Phase1FieldObservations -Records $Records -Name 'exact_finisher_match')
    $explicitFalse = $false
    $explicitInvalid = 0
    foreach ($observation in $explicit) {
        $parsed = ConvertTo-Phase1Bool -Value $observation.value
        if (-not $parsed.valid) { $explicitInvalid++; continue }
        if (-not $parsed.value) { $explicitFalse = $true }
    }
    if ($explicitFalse -or $entryCheck.status -eq 'FAIL') {
        return New-Phase1Check -Name 'exact_finisher_match' -Status 'FAIL' -RequiredForPass $true `
            -ObservedCount ($explicit.Count + $entryCheck.observed_count) -Details "expected finisher $ExpectedFinisher"
    }
    if ($entryCheck.status -eq 'PASS' -or (@($explicit | Where-Object {
                    (ConvertTo-Phase1Bool -Value $_.value).valid -and (ConvertTo-Phase1Bool -Value $_.value).value
                }).Count -gt 0)) {
        return New-Phase1Check -Name 'exact_finisher_match' -Status 'PASS' -RequiredForPass $true `
            -ObservedCount ($explicit.Count + $entryCheck.observed_count) -Details "finisher matched $ExpectedFinisher"
    }
    if ($explicitInvalid -gt 0) {
        Add-Phase1Diagnostic -Diagnostics $Diagnostics -Code 'exact_finisher_type_invalid' `
            -Message 'exact_finisher_match must be boolean/0/1.' -Source $explicit[0].source -Line ([int]$explicit[0].line)
        return New-Phase1Check -Name 'exact_finisher_match' -Status 'INCONCLUSIVE' -RequiredForPass $true `
            -ObservedCount ($explicit.Count + $entryCheck.observed_count) -Details 'exact finisher evidence type was invalid'
    }
    return New-Phase1Check -Name 'exact_finisher_match' -Status 'NOT_OBSERVED' -RequiredForPass $true `
        -ObservedCount ($explicit.Count + $entryCheck.observed_count) -Details 'exact finisher match was not observed'
}

function New-Phase1FixtureResult {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][object]$Fixture,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]]$Records,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][System.Collections.Generic.List[object]]$Diagnostics
    )

    $checks = [System.Collections.Generic.List[object]]::new()
    $identityQuest = New-Phase1ExactCheck -Records $Records -FieldName 'quest_id' -CheckName 'objective_quest_id_exact' `
        -Expected $Fixture.quest_id -RequiredForPass $true -Diagnostics $Diagnostics -Numeric
    [void]$checks.Add($identityQuest)
    [void]$checks.Add((New-Phase1ExactCheck -Records $Records -FieldName 'objective_family' -CheckName 'objective_family_exact' `
            -Expected $Fixture.objective_family -RequiredForPass $true -Diagnostics $Diagnostics))
    [void]$checks.Add((New-Phase1ExactCheck -Records $Records -FieldName 'objective_kind' -CheckName 'objective_kind_exact' `
            -Expected $Fixture.objective_kind -RequiredForPass $true -Diagnostics $Diagnostics))

    if ($null -ne $Fixture.required_item_id) {
        [void]$checks.Add((New-Phase1ExactCheck -Records $Records -FieldName 'required_item_id' -CheckName 'required_item_exact' `
                -Expected $Fixture.required_item_id -RequiredForPass $true -Diagnostics $Diagnostics -Numeric))
    }
    else {
        [void]$checks.Add((New-Phase1ExactCheck -Records $Records -FieldName 'required_npc_or_go_entry' -CheckName 'required_creature_exact' `
                -Expected $Fixture.required_npc_or_go_entry -RequiredForPass $true -Diagnostics $Diagnostics -Numeric))
    }
    if ($null -ne $Fixture.required_count) {
        [void]$checks.Add((New-Phase1ExactCheck -Records $Records -FieldName 'required_count' -CheckName 'required_count_exact' `
                -Expected $Fixture.required_count -RequiredForPass $true -Diagnostics $Diagnostics -Numeric))
    }
    else {
        [void]$checks.Add((New-Phase1PositiveCountCheck -Records $Records -Diagnostics $Diagnostics))
    }

    [void]$checks.Add((New-Phase1SourceCheck -Records $Records -Allowed $Fixture.allowed_source_entries -Diagnostics $Diagnostics))
    [void]$checks.Add((New-Phase1ExactCheck -Records $Records -FieldName 'finisher_entry' -CheckName 'finisher_entry_exact' `
            -Expected $Fixture.finisher_entry -RequiredForPass $true -Diagnostics $Diagnostics -Numeric))
    [void]$checks.Add((New-Phase1SupportedCheck -Records $Records -Diagnostics $Diagnostics))
    [void]$checks.Add((New-Phase1FailureReasonCheck -Records $Records))
    [void]$checks.Add((New-Phase1CountAdvanceCheck -Records $Records -Diagnostics $Diagnostics))

    $invariants = [System.Collections.Generic.List[object]]::new()
    [void]$invariants.Add((New-Phase1ExactFinisherInvariantCheck -Records $Records -ExpectedFinisher $Fixture.finisher_entry -Diagnostics $Diagnostics))
    [void]$invariants.Add((New-Phase1RewardCheck -Records $Records -Diagnostics $Diagnostics))
    [void]$invariants.Add((New-Phase1ContextCheck -Records $Records -Diagnostics $Diagnostics))
    [void]$invariants.Add((New-Phase1ZeroInvariantCheck -Records $Records -FieldName 'unrelated_offensive_target_count' `
            -CheckName 'unrelated_offensive_targets_zero' -Diagnostics $Diagnostics))
    [void]$invariants.Add((New-Phase1ZeroInvariantCheck -Records $Records -FieldName 'random_grind_fallback_count' `
            -CheckName 'random_grind_fallbacks_zero' -Diagnostics $Diagnostics))
    [void]$invariants.Add((New-Phase1TeleportCheck -Records $Records -Diagnostics $Diagnostics))
    [void]$invariants.Add((New-Phase1ZeroInvariantCheck -Records $Records -FieldName 'direct_quest_db_mutation_count' `
            -CheckName 'direct_quest_db_mutations_zero' -Diagnostics $Diagnostics))

    $all = @($checks) + @($invariants)
    $status = 'PASS'
    if (@($all | Where-Object { $_.status -in @('FAIL', 'UNSUPPORTED') }).Count -gt 0) {
        $status = 'FAIL'
    }
    elseif (@($all | Where-Object { $_.status -in @('INCONCLUSIVE', 'NOT_OBSERVED') }).Count -gt 0) {
        $status = 'INCONCLUSIVE'
    }

    return [pscustomobject][ordered]@{
        id = $Fixture.id
        quest_id = $Fixture.quest_id
        status = $status
        observed_record_count = @($Records).Count
        expected = [pscustomobject][ordered]@{
            objective_family = $Fixture.objective_family
            objective_kind = $Fixture.objective_kind
            required_item_id = $Fixture.required_item_id
            required_npc_or_go_entry = $Fixture.required_npc_or_go_entry
            required_count = $Fixture.required_count
            allowed_source_entries = @($Fixture.allowed_source_entries)
            finisher_entry = $Fixture.finisher_entry
        }
        checks = @($checks)
        release_invariants = @($invariants)
    }
}

function Get-Phase1OfflineFixtureObjects {
    [CmdletBinding()]
    param()

    $objects = [System.Collections.Generic.List[object]]::new()
    $index = 0
    foreach ($fixture in $script:Phase1Fixtures) {
        $index++
        $requiredCount = if ($null -ne $Fixture.required_count) { [int]$Fixture.required_count } else { 1 }
        $currentCount = $requiredCount
        [void]$objects.Add([pscustomobject][ordered]@{
                schema = $script:Phase1EvidenceSchema
                schema_version = 1
                event = 'phase1_acceptance_snapshot'
                scope = 'quest'
                bot_guid = 9000 + $index
                quest_id = $fixture.quest_id
                objective = [ordered]@{
                    quest_id = $fixture.quest_id
                    objective_family = $fixture.objective_family
                    objective_slot = 0
                    objective_kind = $fixture.objective_kind
                    phase = 'verify_reward'
                    failure_reason = 'none'
                    supported = $true
                    required_npc_or_go_entry = if ($null -eq $fixture.required_npc_or_go_entry) { 0 } else { $fixture.required_npc_or_go_entry }
                    required_item_id = if ($null -eq $fixture.required_item_id) { 0 } else { $fixture.required_item_id }
                    current_count = $currentCount
                    required_count = $requiredCount
                    baseline_count = 0
                    selected_source_entry = $fixture.allowed_source_entries[0]
                    selected_target_guid = 70000 + $index
                    finisher_entry = $fixture.finisher_entry
                    finisher_guid = 80000 + $index
                    has_lock = $false
                }
                allowed_source_entries = @($fixture.allowed_source_entries)
                exact_finisher_match = $true
                reward_postcondition_confirmed = $true
                objective_context_present = $true
                unrelated_offensive_target_count = 0
                random_grind_fallback_count = 0
                teleport_used = $false
                direct_quest_db_mutation_count = 0
            })
    }
    return @($objects)
}

function Send-Phase1QuestObjectiveReadOnly {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][uint32]$BotGuid,
        [Parameter(Mandatory = $true)][string]$Host,
        [Parameter(Mandatory = $true)][int]$Port,
        [Parameter(Mandatory = $true)][int]$TimeoutMilliseconds
    )

    if ($Host -notin @('127.0.0.1', 'localhost', '::1')) {
        throw "Refusing non-loopback bridge host '$Host'."
    }
    if ($BotGuid -eq 0) { throw 'BotGuid must be positive for a live snapshot.' }

    $client = [System.Net.Sockets.TcpClient]::new()
    try {
        $connected = $client.ConnectAsync($Host, $Port).Wait($TimeoutMilliseconds)
        if (-not $connected -or -not $client.Connected) {
            throw "Could not connect to loopback bridge at ${Host}:$Port."
        }
        $stream = $client.GetStream()
        $stream.ReadTimeout = $TimeoutMilliseconds
        $stream.WriteTimeout = $TimeoutMilliseconds
        $writer = [System.IO.StreamWriter]::new($stream)
        $writer.NewLine = "`n"
        $writer.WriteLine("questobjective $BotGuid")
        $writer.Flush()
        $reader = [System.IO.StreamReader]::new($stream)
        $line = $reader.ReadLine()
        if ([string]::IsNullOrWhiteSpace($line)) { throw 'Loopback bridge returned an empty questobjective response.' }
        return ($line | ConvertFrom-Json -Depth 100 -ErrorAction Stop)
    }
    finally {
        $client.Dispose()
    }
}

function Get-Phase1ReportStatus {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]]$Fixtures,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][object[]]$Diagnostics,
        [Parameter(Mandatory = $true)][bool]$SchemaGap
    )

    $checks = @($Fixtures | ForEach-Object { @($_.checks) + @($_.release_invariants) })
    if (@($checks | Where-Object { $_.status -in @('FAIL', 'UNSUPPORTED') }).Count -gt 0) { return 'FAIL' }
    if ($SchemaGap -or @($Diagnostics | Where-Object { $_.severity -in @('warning', 'error') }).Count -gt 0) {
        return 'INCONCLUSIVE'
    }
    if (@($checks | Where-Object { $_.status -in @('INCONCLUSIVE', 'NOT_OBSERVED') }).Count -gt 0) {
        return 'INCONCLUSIVE'
    }
    return 'PASS'
}

function ConvertTo-Phase1MarkdownCell {
    [CmdletBinding()]
    param([AllowNull()][object]$Value)
    return ([string]$Value -replace '\|', '\|').Replace("`r", ' ').Replace("`n", ' ')
}

function ConvertTo-Phase1Markdown {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][object]$Report)

    $lines = [System.Collections.Generic.List[string]]::new()
    [void]$lines.Add('# Phase 1 quest acceptance verifier')
    [void]$lines.Add('')
    [void]$lines.Add("Status: **$($Report.status)**")
    [void]$lines.Add('')
    [void]$lines.Add('This report is deterministic for the supplied evidence. It is offline unless an explicit live read was enabled.')
    [void]$lines.Add('')
    [void]$lines.Add(("- Verifier schema: ``{0}``" -f $Report.verifier_schema))
    [void]$lines.Add("- Input records accepted: $($Report.schema.records_accepted)")
    [void]$lines.Add("- Input records rejected: $($Report.schema.records_rejected)")
    [void]$lines.Add("- Schema gaps: $($Report.schema.schema_gaps)")
    [void]$lines.Add("- Live snapshot requested: $($Report.live_snapshot.requested); executed: $($Report.live_snapshot.executed); dry-run: $($Report.live_snapshot.dry_run)")
    [void]$lines.Add('')
    [void]$lines.Add('## Fixture results')
    [void]$lines.Add('')
    [void]$lines.Add('| Fixture | Quest | Status | Records |')
    [void]$lines.Add('|---|---:|---|---:|')
    foreach ($fixture in @($Report.fixtures | Sort-Object quest_id)) {
        [void]$lines.Add("| $($fixture.id) | $($fixture.quest_id) | $($fixture.status) | $($fixture.observed_record_count) |")
    }
    [void]$lines.Add('')

    foreach ($fixture in @($Report.fixtures | Sort-Object quest_id)) {
        [void]$lines.Add("### $($fixture.id) (quest $($fixture.quest_id))")
        [void]$lines.Add('')
        [void]$lines.Add('| Check | Required for PASS | Status | Details |')
        [void]$lines.Add('|---|---|---|---|')
        foreach ($check in @($fixture.checks)) {
            [void]$lines.Add("| $($check.name) | $($check.required_for_pass) | $($check.status) | $(ConvertTo-Phase1MarkdownCell $check.details) |")
        }
        foreach ($check in @($fixture.release_invariants)) {
            [void]$lines.Add("| release: $($check.name) | $($check.required_for_pass) | $($check.status) | $(ConvertTo-Phase1MarkdownCell $check.details) |")
        }
        [void]$lines.Add('')
    }

    [void]$lines.Add('## Diagnostics')
    [void]$lines.Add('')
    if (@($Report.diagnostics).Count -eq 0) {
        [void]$lines.Add('None.')
    }
    else {
        foreach ($diagnostic in @($Report.diagnostics)) {
            $where = if ([string]::IsNullOrWhiteSpace([string]$diagnostic.source)) { '' } else { " ($($diagnostic.source):$($diagnostic.line))" }
            [void]$lines.Add(("- [{0}] ``{1}``: {2}{3}" -f $diagnostic.severity, $diagnostic.code, (ConvertTo-Phase1MarkdownCell $diagnostic.message), $where))
        }
    }
    [void]$lines.Add('')
    [void]$lines.Add('## Status semantics')
    [void]$lines.Add('')
    [void]$lines.Add('- `PASS`: every required fixture field and release invariant was observed and passed.')
    [void]$lines.Add('- `FAIL`: evidence explicitly contradicts a fixture or invariant, including unsupported objectives.')
    [void]$lines.Add('- `INCONCLUSIVE`: required evidence was missing, malformed, unversioned, or not safe to interpret.')
    [void]$lines.Add('- `NOT_OBSERVED`: a check-level status meaning no usable observation was supplied; it is never treated as success.')
    [void]$lines.Add('')
    [void]$lines.Add('The verifier does not infer live success from movement, XP, quest-log presence, or a missing failure message.')
    return (($lines -join [Environment]::NewLine) + [Environment]::NewLine)
}

function Write-Phase1Reports {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][object]$Report,
        [Parameter(Mandatory = $true)][string]$JsonPath,
        [Parameter(Mandatory = $true)][string]$MarkdownPath
    )

    foreach ($path in @($JsonPath, $MarkdownPath)) {
        $directory = Split-Path -Parent $path
        if (-not [string]::IsNullOrWhiteSpace($directory)) {
            New-Item -ItemType Directory -Path $directory -Force | Out-Null
        }
    }
    $utf8NoBom = [System.Text.UTF8Encoding]::new($false)
    $json = $Report | ConvertTo-Json -Depth 30
    [System.IO.File]::WriteAllText($JsonPath, $json + [Environment]::NewLine, $utf8NoBom)
    [System.IO.File]::WriteAllText($MarkdownPath, (ConvertTo-Phase1Markdown -Report $Report), $utf8NoBom)
}

function Invoke-Phase1AcceptanceVerifier {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$ServerRoot,
        [string]$EvidencePath = '',
        [string]$BridgeSnapshotPath = '',
        [string]$JsonReportPath = '',
        [string]$MarkdownReportPath = '',
        [switch]$OfflineFixture,
        [switch]$LiveSnapshot,
        [switch]$AllowLiveRead,
        [uint32]$BotGuid = 0,
        [string]$BridgeHost = '127.0.0.1',
        [int]$BridgePort = 18787,
        [int]$TimeoutMs = 4000
    )

    $diagnostics = [System.Collections.Generic.List[object]]::new()
    $records = [System.Collections.Generic.List[object]]::new()
    $liveExecuted = $false
    $liveDryRun = $false

    if (-not [string]::IsNullOrWhiteSpace($EvidencePath)) {
        foreach ($raw in @(Get-Phase1JsonObjectsFromFile -Path $EvidencePath -Kind 'evidence' -Diagnostics $diagnostics)) {
            [void]$records.Add((ConvertTo-Phase1NormalizedRecord -Object $raw.object -Source $raw.source -Line $raw.line `
                    -SourceKind 'evidence' -Diagnostics $diagnostics))
        }
    }
    if (-not [string]::IsNullOrWhiteSpace($BridgeSnapshotPath)) {
        foreach ($raw in @(Get-Phase1JsonObjectsFromFile -Path $BridgeSnapshotPath -Kind 'bridge' -Diagnostics $diagnostics)) {
            [void]$records.Add((ConvertTo-Phase1NormalizedRecord -Object $raw.object -Source $raw.source -Line $raw.line `
                    -SourceKind 'bridge' -Diagnostics $diagnostics))
        }
    }
    if ($OfflineFixture) {
        $offlineIndex = 0
        foreach ($object in @(Get-Phase1OfflineFixtureObjects)) {
            $offlineIndex++
            [void]$records.Add((ConvertTo-Phase1NormalizedRecord -Object $object -Source 'offline://phase1-fixture' `
                    -Line $offlineIndex -SourceKind 'offline' -Diagnostics $diagnostics))
        }
    }

    if ($LiveSnapshot) {
        if (-not $AllowLiveRead) {
            $liveDryRun = $true
            Add-Phase1Diagnostic -Diagnostics $diagnostics -Code 'live_snapshot_dry_run' -Severity 'warning' `
                -Message 'LiveSnapshot was requested but no live read was issued. Add -AllowLiveRead for the explicit loopback questobjective read.'
        }
        elseif ($BotGuid -eq 0) {
            Add-Phase1Diagnostic -Diagnostics $diagnostics -Code 'live_snapshot_bot_guid_missing' -Severity 'error' `
                -Message 'LiveSnapshot requires a positive -BotGuid.'
        }
        else {
            try {
                $liveObject = Send-Phase1QuestObjectiveReadOnly -BotGuid $BotGuid -Host $BridgeHost -Port $BridgePort `
                    -TimeoutMilliseconds $TimeoutMs
                [void]$records.Add((ConvertTo-Phase1NormalizedRecord -Object $liveObject -Source "live://$BridgeHost`:$BridgePort" `
                        -Line 1 -SourceKind 'bridge' -Diagnostics $diagnostics))
                $liveExecuted = $true
            }
            catch {
                Add-Phase1Diagnostic -Diagnostics $diagnostics -Code 'live_snapshot_error' -Severity 'error' `
                    -Message 'The explicit read-only questobjective snapshot failed; no success is inferred.'
            }
        }
    }

    if ($records.Count -eq 0) {
        Add-Phase1Diagnostic -Diagnostics $diagnostics -Code 'no_evidence' -Severity 'warning' `
            -Message 'No evidence records were supplied. All fixture results remain INCONCLUSIVE.'
    }

    $acceptedRecords = @($records | Where-Object { $_.accepted })
    $fixtureResults = [System.Collections.Generic.List[object]]::new()
    foreach ($fixture in $script:Phase1Fixtures) {
        $fixtureRecords = @(Get-Phase1FixtureRecords -QuestId $fixture.quest_id -Records $acceptedRecords)
        [void]$fixtureResults.Add((New-Phase1FixtureResult -Fixture $fixture -Records $fixtureRecords -Diagnostics $diagnostics))
    }

    $schemaGap = (@($records | Where-Object { $_.schema_gap }).Count -gt 0) -or
        (@($diagnostics | Where-Object { $_.code -match 'schema|jsonl_parse|input_file_missing' }).Count -gt 0)
    $sortedDiagnostics = @($diagnostics | Sort-Object code, source, line, message)
    $status = Get-Phase1ReportStatus -Fixtures @($fixtureResults) -Diagnostics $sortedDiagnostics -SchemaGap $schemaGap

    $resolvedServerRoot = Get-Phase1AbsolutePath -Path $ServerRoot
    $jsonPath = if ([string]::IsNullOrWhiteSpace($JsonReportPath)) {
        Join-Path $resolvedServerRoot 'logs\phase1-acceptance-verifier.json'
    }
    else { Get-Phase1AbsolutePath -Path $JsonReportPath }
    $markdownPath = if ([string]::IsNullOrWhiteSpace($MarkdownReportPath)) {
        Join-Path $resolvedServerRoot 'logs\phase1-acceptance-verifier.md'
    }
    else { Get-Phase1AbsolutePath -Path $MarkdownReportPath }

    if ([string]::IsNullOrWhiteSpace($JsonReportPath) -and -not [string]::IsNullOrWhiteSpace($MarkdownReportPath)) {
        $jsonPath = [System.IO.Path]::ChangeExtension($markdownPath, '.json')
    }
    if ([string]::IsNullOrWhiteSpace($MarkdownReportPath) -and -not [string]::IsNullOrWhiteSpace($JsonReportPath)) {
        $markdownPath = [System.IO.Path]::ChangeExtension($jsonPath, '.md')
    }

    $report = [pscustomobject][ordered]@{
        verifier_schema = $script:Phase1VerifierSchema
        input_contract = $script:Phase1EvidenceSchema
        status = $status
        deterministic = $true
        mode = if ($OfflineFixture) { 'offline_fixture' } elseif ($LiveSnapshot) { 'live_snapshot' } else { 'offline_evidence' }
        inputs = [pscustomobject][ordered]@{
            evidence_path = if ([string]::IsNullOrWhiteSpace($EvidencePath)) { '' } else { Get-Phase1AbsolutePath $EvidencePath }
            bridge_snapshot_path = if ([string]::IsNullOrWhiteSpace($BridgeSnapshotPath)) { '' } else { Get-Phase1AbsolutePath $BridgeSnapshotPath }
        }
        live_snapshot = [pscustomobject][ordered]@{
            requested = [bool]$LiveSnapshot
            executed = [bool]$liveExecuted
            dry_run = [bool]$liveDryRun
            command = if ($LiveSnapshot) { 'questobjective <bot-guid>' } else { '' }
            control_commands_issued = @()
        }
        schema = [pscustomobject][ordered]@{
            accepted_input_schema = $script:Phase1AcceptedEvidenceSchemas
            accepted_bridge_schema = $script:Phase1AcceptedBridgeSchemas
            records_read = $records.Count
            records_accepted = $acceptedRecords.Count
            records_rejected = @($records | Where-Object { -not $_.accepted }).Count
            schema_gaps = @($records | Where-Object { $_.schema_gap }).Count
        }
        fixtures = @($fixtureResults)
        diagnostics = $sortedDiagnostics
        report_paths = [pscustomobject][ordered]@{ json = $jsonPath; markdown = $markdownPath }
    }

    Write-Phase1Reports -Report $report -JsonPath $jsonPath -MarkdownPath $markdownPath
    return $report
}

$result = Invoke-Phase1AcceptanceVerifier -ServerRoot $ServerRoot -EvidencePath $EvidencePath `
    -BridgeSnapshotPath $BridgeSnapshotPath -JsonReportPath $JsonReportPath -MarkdownReportPath $MarkdownReportPath `
    -OfflineFixture:$OfflineFixture -LiveSnapshot:$LiveSnapshot -AllowLiveRead:$AllowLiveRead -BotGuid $BotGuid `
    -BridgeHost $BridgeHost -BridgePort $BridgePort -TimeoutMs $TimeoutMs
Write-Output ("Phase 1 acceptance verifier: {0}" -f $result.status)
Write-Output ("JSON report: {0}" -f $result.report_paths.json)
Write-Output ("Markdown report: {0}" -f $result.report_paths.markdown)
