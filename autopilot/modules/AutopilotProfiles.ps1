# AutoWoW Autopilot V1.2 - PROFILES module.
# A profile is STRATEGIC PREFERENCE, never execution authority. It biases which
# job kinds the planner prefers for a character and how upkeep is scheduled; it
# can NEVER override capability manifests, ownership leases, capability
# allow-lists, or game rules. Nothing in this module implies live execution.
#
# Dot-source AFTER AutopilotLib.ps1 (same scope); this module calls the lib's
# helpers (ConvertTo-AutopilotHashtable, ConvertTo-AutopilotUtcDateTime,
# $script:AutopilotJobKinds) and never redefines anything in it.
#
# Profile documents carry schema 'autowow.autopilot.profile.v1' /
# schema_version 1 and FAIL CLOSED (rejected with typed reasons) on unknown
# schema, unknown schema_version, or malformed JSON.

Set-StrictMode -Version Latest

$script:AutopilotProfileSchema = 'autowow.autopilot.profile.v1'
$script:AutopilotProfileSchemaVersion = 1
$script:AutopilotProfileGroupRoles = @('tank', 'healer', 'dps', 'flex')
$script:AutopilotProfileRiskTolerances = @('low', 'normal', 'high')
$script:AutopilotProfileEconomyBoolFields = @('allowSpending', 'allowSelling', 'allowMailing', 'allowAuction')
$script:AutopilotProfileRequiredFields = @(
    'schema', 'schema_version', 'name', 'description', 'allowedJobKinds', 'priorityWeights',
    'preferredZones', 'preferredMaterials', 'preferredProfessions', 'groupRole', 'groupingAllowed',
    'pvpOptIn', 'riskTolerance', 'consumableReserve', 'bagSpaceReserveSlots', 'repairThresholdPct',
    'maxDeathsBeforeBlock', 'schedule', 'economy'
)

# ---------------------------------------------------------------------------
# Internal helpers (module-private by convention; not part of the public API)
# ---------------------------------------------------------------------------

function Test-AutopilotProfileIntegral {
    # True only for integral numeric types. JSON booleans/strings/doubles are
    # rejected rather than coerced (honesty: never guess a plausible value).
    param($Value)
    return ($Value -is [int] -or $Value -is [int64] -or $Value -is [int16] -or
        $Value -is [byte] -or $Value -is [uint16] -or $Value -is [uint32] -or $Value -is [uint64])
}

function Test-AutopilotProfileList {
    param($Value)
    return ($Value -is [System.Collections.IEnumerable] -and
        $Value -isnot [string] -and
        $Value -isnot [System.Collections.IDictionary])
}

function ConvertTo-AutopilotProfileMinuteOfDay {
    # Parses a UTC 'HH:mm' wall-clock string into minutes since midnight.
    # Returns $null (never a guessed value) for anything malformed.
    param($Value)
    if ($null -eq $Value) { return $null }
    $text = [string]$Value
    if ($text -notmatch '^([01][0-9]|2[0-3]):([0-5][0-9])$') { return $null }
    return ([int]$Matches[1] * 60) + [int]$Matches[2]
}

# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

function Get-AutopilotProfilesDir {
    # Default profile directory: <autopilot>\profiles (this file lives in
    # <autopilot>\modules). Every other function takes -ProfilesDir to override.
    param([string]$ProfilesDir)
    if ($ProfilesDir) { return $ProfilesDir }
    return (Join-Path (Split-Path -Parent $PSScriptRoot) 'profiles')
}

function Get-AutopilotProfileNames {
    param([string]$ProfilesDir)
    $dir = Get-AutopilotProfilesDir -ProfilesDir $ProfilesDir
    if (-not (Test-Path $dir)) { return , @() }
    $names = @(Get-ChildItem -LiteralPath $dir -Filter '*.json' -File |
            ForEach-Object { $_.BaseName } | Sort-Object)
    return , @($names)
}

function Test-AutopilotProfile {
    # Semantic validation is authoritative; JSON-Schema validation (Test-Json
    # against schemas\profile.schema.json) runs additionally when the engine
    # supports the schema draft, mirroring Test-AutopilotJob.
    param(
        [Parameter(Mandatory)]$Profile,
        [string]$SchemaPath
    )
    if (-not $SchemaPath) {
        $SchemaPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'schemas\profile.schema.json'
    }
    $errors = [System.Collections.Generic.List[string]]::new()
    $doc = ConvertTo-AutopilotHashtable $Profile

    if (-not ($doc -is [System.Collections.IDictionary])) {
        $errors.Add('profile document must be a JSON object (fail closed)')
        return [pscustomobject]@{ Valid = $false; Errors = [string[]]@($errors); SchemaEngine = 'skipped' }
    }

    foreach ($field in $script:AutopilotProfileRequiredFields) {
        if (-not $doc.Contains($field)) { $errors.Add("missing required field '$field'") }
    }

    if ($errors.Count -eq 0) {
        # Fail closed on unknown schema id / version before trusting anything else.
        if ([string]$doc.schema -ne $script:AutopilotProfileSchema) {
            $errors.Add("unknown schema '$($doc.schema)' (fail closed): expected $($script:AutopilotProfileSchema)")
        }
        elseif (-not (Test-AutopilotProfileIntegral $doc.schema_version) -or
            [int64]$doc.schema_version -ne $script:AutopilotProfileSchemaVersion) {
            $errors.Add("unsupported schema_version '$($doc.schema_version)' (fail closed): expected $($script:AutopilotProfileSchemaVersion)")
        }
    }

    if ($errors.Count -eq 0) {
        if (-not ($doc.name -is [string]) -or [string]$doc.name -notmatch '^[a-z0-9][a-z0-9-]*$') {
            $errors.Add("name '$($doc.name)' must be a lowercase kebab-case string matching the profile file name")
        }
        if (-not ($doc.description -is [string]) -or [string]::IsNullOrWhiteSpace([string]$doc.description)) {
            $errors.Add('description must be a non-empty string')
        }

        # allowedJobKinds: subset of the closed Autopilot kind list.
        if (-not (Test-AutopilotProfileList $doc.allowedJobKinds)) {
            $errors.Add('allowedJobKinds must be an array of job kinds')
        }
        else {
            foreach ($kind in @($doc.allowedJobKinds)) {
                if ([string]$kind -notin $script:AutopilotJobKinds) {
                    $errors.Add("unknown job kind '$kind' in allowedJobKinds")
                }
            }
        }

        # priorityWeights: jobKind -> integer -20..+20 (strategic bias only).
        if (-not ($doc.priorityWeights -is [System.Collections.IDictionary])) {
            $errors.Add('priorityWeights must be an object mapping jobKind to an integer weight')
        }
        else {
            foreach ($key in @($doc.priorityWeights.Keys)) {
                if ([string]$key -notin $script:AutopilotJobKinds) {
                    $errors.Add("priorityWeights names unknown job kind '$key'")
                }
                $weight = $doc.priorityWeights[[string]$key]
                if (-not (Test-AutopilotProfileIntegral $weight)) {
                    $errors.Add("priorityWeights['$key'] must be an integer")
                }
                elseif ([int64]$weight -lt -20 -or [int64]$weight -gt 20) {
                    $errors.Add("priorityWeights['$key'] = $weight is out of range -20..20")
                }
            }
        }

        foreach ($listField in @('preferredZones', 'preferredProfessions')) {
            if (-not (Test-AutopilotProfileList $doc[$listField])) {
                $errors.Add("$listField must be an array of strings")
            }
            else {
                foreach ($entry in @($doc[$listField])) {
                    if (-not ($entry -is [string]) -or [string]::IsNullOrWhiteSpace($entry)) {
                        $errors.Add("$listField entries must be non-empty strings")
                    }
                }
            }
        }

        if (-not (Test-AutopilotProfileList $doc.preferredMaterials)) {
            $errors.Add('preferredMaterials must be an array of { itemId, name? } objects')
        }
        else {
            foreach ($material in @($doc.preferredMaterials)) {
                if (-not ($material -is [System.Collections.IDictionary]) -or -not $material.Contains('itemId')) {
                    $errors.Add('preferredMaterials entries require an itemId')
                    continue
                }
                if (-not (Test-AutopilotProfileIntegral $material.itemId) -or [int64]$material.itemId -lt 1) {
                    $errors.Add("preferredMaterials itemId '$($material.itemId)' must be a positive integer")
                }
                if ($material.Contains('name') -and $null -ne $material.name -and -not ($material.name -is [string])) {
                    $errors.Add('preferredMaterials name must be a string when present')
                }
            }
        }

        if ($null -ne $doc.groupRole -and [string]$doc.groupRole -notin $script:AutopilotProfileGroupRoles) {
            $errors.Add("groupRole '$($doc.groupRole)' must be one of $($script:AutopilotProfileGroupRoles -join ', ') or null")
        }
        foreach ($boolField in @('groupingAllowed', 'pvpOptIn')) {
            if (-not ($doc[$boolField] -is [bool])) { $errors.Add("$boolField must be a boolean") }
        }
        if (-not ($doc.riskTolerance -is [string]) -or [string]$doc.riskTolerance -notin $script:AutopilotProfileRiskTolerances) {
            $errors.Add("riskTolerance '$($doc.riskTolerance)' must be one of $($script:AutopilotProfileRiskTolerances -join ', ')")
        }

        if (-not (Test-AutopilotProfileList $doc.consumableReserve)) {
            $errors.Add('consumableReserve must be an array of { itemId, minCount } objects')
        }
        else {
            foreach ($reserve in @($doc.consumableReserve)) {
                if (-not ($reserve -is [System.Collections.IDictionary]) -or
                    -not $reserve.Contains('itemId') -or -not $reserve.Contains('minCount')) {
                    $errors.Add('consumableReserve entries require itemId and minCount')
                    continue
                }
                if (-not (Test-AutopilotProfileIntegral $reserve.itemId) -or [int64]$reserve.itemId -lt 1) {
                    $errors.Add("consumableReserve itemId '$($reserve.itemId)' must be a positive integer")
                }
                if (-not (Test-AutopilotProfileIntegral $reserve.minCount) -or [int64]$reserve.minCount -lt 0) {
                    $errors.Add("consumableReserve minCount '$($reserve.minCount)' must be an integer greater than or equal to 0")
                }
            }
        }

        foreach ($intSpec in @(
                @{ field = 'bagSpaceReserveSlots'; min = 0; max = $null },
                @{ field = 'repairThresholdPct'; min = 0; max = 100 },
                @{ field = 'maxDeathsBeforeBlock'; min = 0; max = $null })) {
            $value = $doc[[string]$intSpec.field]
            if (-not (Test-AutopilotProfileIntegral $value)) {
                $errors.Add("$($intSpec.field) must be an integer")
            }
            elseif ([int64]$value -lt [int64]$intSpec.min -or ($null -ne $intSpec.max -and [int64]$value -gt [int64]$intSpec.max)) {
                $errors.Add("$($intSpec.field) = $value is out of range")
            }
        }

        if (-not ($doc.schedule -is [System.Collections.IDictionary]) -or -not $doc.schedule.Contains('playWindowsUtc')) {
            $errors.Add('schedule must be an object with a playWindowsUtc array')
        }
        elseif (-not (Test-AutopilotProfileList $doc.schedule.playWindowsUtc)) {
            $errors.Add('schedule.playWindowsUtc must be an array (empty = always active)')
        }
        else {
            foreach ($window in @($doc.schedule.playWindowsUtc)) {
                if (-not ($window -is [System.Collections.IDictionary]) -or
                    -not $window.Contains('start') -or -not $window.Contains('end')) {
                    $errors.Add('schedule.playWindowsUtc entries require start and end')
                    continue
                }
                foreach ($edge in @('start', 'end')) {
                    if ($null -eq (ConvertTo-AutopilotProfileMinuteOfDay $window[$edge])) {
                        $errors.Add("schedule window $edge '$($window[$edge])' must be a UTC 'HH:mm' string (00:00-23:59)")
                    }
                }
            }
        }

        if (-not ($doc.economy -is [System.Collections.IDictionary])) {
            $errors.Add('economy must be an object')
        }
        else {
            foreach ($boolField in $script:AutopilotProfileEconomyBoolFields) {
                if (-not $doc.economy.Contains($boolField) -or -not ($doc.economy[$boolField] -is [bool])) {
                    $errors.Add("economy.$boolField must be a boolean")
                }
            }
            if (-not $doc.economy.Contains('maxSpendCopper') -or
                -not (Test-AutopilotProfileIntegral $doc.economy.maxSpendCopper) -or
                [int64]$doc.economy.maxSpendCopper -lt 0) {
                $errors.Add('economy.maxSpendCopper must be an integer greater than or equal to 0')
            }
        }
    }

    $schemaEngine = 'skipped'
    if ($SchemaPath -and (Test-Path $SchemaPath) -and $errors.Count -eq 0) {
        try {
            $json = $doc | ConvertTo-Json -Depth 30
            if (Test-Json -Json $json -SchemaFile $SchemaPath -ErrorAction Stop) { $schemaEngine = 'passed' }
        }
        catch {
            # Engine limitations (draft support) are reported but only real
            # schema violations count as errors, exactly like Test-AutopilotJob.
            if ($_.FullyQualifiedErrorId -match 'InvalidJsonAgainstSchema') {
                $errors.Add("JSON Schema violation: $($_.Exception.Message)")
                $schemaEngine = 'failed'
            }
            else { $schemaEngine = "unavailable: $($_.Exception.Message)" }
        }
    }

    return [pscustomobject]@{
        Valid        = ($errors.Count -eq 0)
        Errors       = [string[]]@($errors)
        SchemaEngine = $schemaEngine
    }
}

function Get-AutopilotProfile {
    # Loads and validates a profile by name. Throws a typed error listing every
    # validation failure; malformed JSON and unknown schema/version fail closed.
    param(
        [Parameter(Mandatory)][string]$Name,
        [string]$ProfilesDir,
        [string]$SchemaPath
    )
    $dir = Get-AutopilotProfilesDir -ProfilesDir $ProfilesDir
    $file = Join-Path $dir "$Name.json"
    if (-not (Test-Path $file)) {
        # Direct assignment (no extra @() around the call): the function already
        # returns a comma-wrapped array, and re-wrapping the command output
        # would nest it and stringify as 'System.Object[]'.
        $available = Get-AutopilotProfileNames -ProfilesDir $ProfilesDir
        $listing = if (@($available).Count -gt 0) { @($available) -join ', ' } else { '(none)' }
        throw "Unknown profile '$Name'. Available profiles: $listing."
    }
    try {
        $doc = ConvertTo-AutopilotHashtable (Get-Content -LiteralPath $file -Raw | ConvertFrom-Json)
    }
    catch {
        throw "Profile '$Name' rejected (malformed JSON, fail closed): $($_.Exception.Message)"
    }
    $validation = Test-AutopilotProfile -Profile $doc -SchemaPath $SchemaPath
    if (-not $validation.Valid) {
        throw "Profile '$Name' rejected: $($validation.Errors -join '; ')"
    }
    if ([string]$doc.name -ne $Name) {
        throw "Profile '$Name' rejected: document name '$($doc.name)' does not match the file name '$Name'"
    }
    return $doc
}

function Test-AutopilotProfileJobKindAllowed {
    # Strategic gate only: a profile refusing a kind removes it from planning
    # preference; a profile allowing a kind grants NO execution authority -
    # capability manifests, ownership leases, and allow-lists still gate
    # everything downstream.
    param(
        [Parameter(Mandatory)]$Profile,
        [Parameter(Mandatory)][string]$JobKind
    )
    $doc = ConvertTo-AutopilotHashtable $Profile
    $name = if ($doc -is [System.Collections.IDictionary] -and $doc.Contains('name')) { [string]$doc.name } else { '(unnamed)' }

    if ($JobKind -notin $script:AutopilotJobKinds) {
        return [pscustomobject]@{
            allowed = $false
            reason  = "profile '$name': unknown job kind '$JobKind' is not in the Autopilot kind list"
        }
    }
    $allowedKinds = @()
    if ($doc -is [System.Collections.IDictionary] -and $doc.Contains('allowedJobKinds')) {
        $allowedKinds = @(@($doc.allowedJobKinds) | ForEach-Object { [string]$_ })
    }
    if ($JobKind -notin $allowedKinds) {
        return [pscustomobject]@{
            allowed = $false
            reason  = "profile '$name' does not list '$JobKind' in allowedJobKinds"
        }
    }
    if ($JobKind -in @('Battleground', 'OpenWorldPvp')) {
        $pvpOptIn = ($doc.Contains('pvpOptIn') -and $doc.pvpOptIn -is [bool] -and $doc.pvpOptIn)
        if (-not $pvpOptIn) {
            return [pscustomobject]@{
                allowed = $false
                reason  = "profile '$name' lists '$JobKind' but pvpOptIn is false"
            }
        }
    }
    if ($JobKind -in @('FormParty', 'Dungeon', 'Raid')) {
        $grouping = ($doc.Contains('groupingAllowed') -and $doc.groupingAllowed -is [bool] -and $doc.groupingAllowed)
        if (-not $grouping) {
            return [pscustomobject]@{
                allowed = $false
                reason  = "profile '$name' lists '$JobKind' but groupingAllowed is false"
            }
        }
    }
    return [pscustomobject]@{
        allowed = $true
        reason  = "profile '$name' allows '$JobKind' via allowedJobKinds (strategic preference only, never execution authority)"
    }
}

function Get-AutopilotProfileJobPriority {
    # Base priority plus the profile's weight for the kind (missing weight = 0),
    # clamped to the planner's 0..100 priority range.
    param(
        [Parameter(Mandatory)]$Profile,
        [Parameter(Mandatory)][string]$JobKind,
        [Parameter(Mandatory)][int]$BasePriority
    )
    $doc = ConvertTo-AutopilotHashtable $Profile
    $weight = 0
    if ($doc -is [System.Collections.IDictionary] -and $doc.Contains('priorityWeights') -and
        $doc.priorityWeights -is [System.Collections.IDictionary] -and $doc.priorityWeights.Contains($JobKind)) {
        $weight = [int][int64]$doc.priorityWeights[$JobKind]
    }
    $result = $BasePriority + $weight
    if ($result -lt 0) { return 0 }
    if ($result -gt 100) { return 100 }
    return $result
}

function Test-AutopilotProfileScheduleActive {
    # UTC wall-clock windows. A window may wrap midnight (e.g. 22:00-06:00).
    # Empty playWindowsUtc = always active. Start is inclusive, end exclusive.
    # A malformed window grants nothing (fail closed) rather than guessing.
    param(
        [Parameter(Mandatory)]$Profile,
        [Parameter(Mandatory)][datetime]$Now
    )
    $doc = ConvertTo-AutopilotHashtable $Profile
    $utc = ConvertTo-AutopilotUtcDateTime $Now
    $windows = @()
    if ($doc -is [System.Collections.IDictionary] -and $doc.Contains('schedule') -and
        $doc.schedule -is [System.Collections.IDictionary] -and $doc.schedule.Contains('playWindowsUtc')) {
        $windows = @($doc.schedule.playWindowsUtc)
    }
    if ($windows.Count -eq 0) { return $true }

    $nowMinute = ($utc.Hour * 60) + $utc.Minute
    foreach ($window in $windows) {
        if (-not ($window -is [System.Collections.IDictionary])) { continue }
        $start = ConvertTo-AutopilotProfileMinuteOfDay $(if ($window.Contains('start')) { $window['start'] } else { $null })
        $end = ConvertTo-AutopilotProfileMinuteOfDay $(if ($window.Contains('end')) { $window['end'] } else { $null })
        if ($null -eq $start -or $null -eq $end) { continue }
        if ($start -le $end) {
            if ($nowMinute -ge $start -and $nowMinute -lt $end) { return $true }
        }
        else {
            # Wraps midnight, e.g. 22:00-06:00.
            if ($nowMinute -ge $start -or $nowMinute -lt $end) { return $true }
        }
    }
    return $false
}
