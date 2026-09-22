# AutoWoW Autopilot V1.2 - roster discovery module.
# Read-only character facts for the strategic controller. Fixtures only in this
# slice: the fixture provider parses autowow.autopilot.roster-snapshot.v1
# documents; the bridge provider is a named placeholder that reports itself
# unavailable (facts are NEVER guessed, never scraped, never defaulted).
#
# Dot-source AFTER AutopilotLib.ps1 into the same scope; this file only calls
# lib helpers (ConvertTo-AutopilotHashtable, ConvertTo-AutopilotUtcDateTime)
# and never redefines anything the lib owns.
#
# Roster snapshot schema (authoritative):
# {
#   "schema": "autowow.autopilot.roster-snapshot.v1",
#   "schema_version": 1,
#   "source": "fixture" | "bridge",
#   "observed_utc": "<ISO8601 UTC>",
#   "server_session_id": "<string|null>",
#   "characters": [{
#     "guid": <int>, "name": "<string>", "class_id": <int|null>, "class": "<string|null>", "race": "<string|null>",
#     "level": <int|null>, "spec": "<string|null>", "roles": ["tank"|"healer"|"dps", ...],
#     "professions": [{"skill_id": <int>, "name": "<string>", "value": <int>, "max": <int>}],
#     "equipped_item_level": <number|null>, "bag_slots_free": <int|null>, "bag_slots_total": <int|null>,
#     "money_copper": <int|null>, "zone": "<string|null>", "map_id": <int|null>,
#     "alive": <bool|null>, "ghost": <bool|null>, "in_combat": <bool|null>, "online": <bool|null>,
#     "party": {"in_party": <bool|null>, "leader_guid": <int|null>, "raid": <bool|null>},
#     "guild": "<string|null>",
#     "observed_utc": "<ISO8601 UTC>",
#     "unknown_fields": ["<field name>", ...]
#   }]
# }
#
# Honesty invariants:
#   - A fact that is not known is null AND its field name appears in
#     unknown_fields for that character. Unknown facts are never guessed and
#     never defaulted to a plausible value.
#   - Unknown schema/version and malformed JSON fail closed: the provider is
#     unavailable with a typed reason; no partial data leaks out.
#   - Staleness is judged from observed_utc via ConvertTo-AutopilotUtcDateTime,
#     never guessed away.
#   - Nothing here implies live execution; a parsed fixture is evidence of a
#     recorded observation, not of a deployed capability.

Set-StrictMode -Version Latest

$script:AutopilotRosterSnapshotSchema = 'autowow.autopilot.roster-snapshot.v1'
$script:AutopilotRosterSnapshotSchemaVersion = 1

$script:AutopilotRosterRoles = @('tank', 'healer', 'dps')
$script:AutopilotRosterGatheringProfessions = @('Mining', 'Herbalism', 'Skinning')
$script:AutopilotRosterCraftingProfessions = @(
    'Alchemy', 'Blacksmithing', 'Enchanting', 'Engineering', 'Leatherworking',
    'Tailoring', 'Jewelcrafting', 'Inscription', 'Cooking', 'First Aid'
)

# Context defaults per activity. Callers may override any key via -Context.
$script:AutopilotRosterActivityContextDefaults = @{
    'dungeon' = @{ minLevel = 15 }
    'raid'    = @{ minLevel = 80; minItemLevel = 180 }
    'gather'  = @{ minSkill = 1 }
    'craft'   = @{ minSkill = 1 }
    'pvp'     = @{ minLevel = 10 }
}

# Requirement tables, encoded as data. Each requirement declares:
#   requirement - stable name used in the suitability report's missing[] list
#   fields      - roster fact fields the judgment NEEDS (dotted paths allowed).
#                 A null needed fact routes to unknown[] (why='fact not
#                 observed') instead of missing[] - honesty over guessing.
#   test        - scriptblock (resolved-values-hashtable, context) -> bool;
#                 only invoked when every needed field is non-null.
#   detail      - scriptblock (resolved-values-hashtable, context) -> string
#                 explaining the definitive miss, citing the table's threshold.
$script:AutopilotRosterActivityRequirements = [ordered]@{
    'dungeon' = @(
        [ordered]@{
            requirement = 'online'
            fields      = @('online')
            test        = { param($v, $c) [bool]$v['online'] }
            detail      = { param($v, $c) 'character is offline (online=false); dungeon requires an online character' }
        }
        [ordered]@{
            requirement = 'alive'
            fields      = @('alive')
            test        = { param($v, $c) [bool]$v['alive'] }
            detail      = { param($v, $c) 'character is dead (alive=false); dungeon requires a living character' }
        }
        [ordered]@{
            requirement = 'minimum level'
            fields      = @('level')
            test        = { param($v, $c) [int64]$v['level'] -ge [int64]$c.minLevel }
            detail      = { param($v, $c) "level $($v['level']) is below the dungeon minimum $($c.minLevel) (Context.minLevel, default 15)" }
        }
        [ordered]@{
            requirement = 'role assigned'
            fields      = @('roles')
            test        = { param($v, $c) @($v['roles']).Count -ge 1 }
            detail      = { param($v, $c) 'no role assigned (roles list is empty); dungeon requires at least one of tank/healer/dps' }
        }
        [ordered]@{
            requirement = 'no conflicting raid group'
            fields      = @('party.raid')
            test        = { param($v, $c) -not [bool]$v['party.raid'] }
            detail      = { param($v, $c) 'character is in a raid group (party.raid=true), which conflicts with a 5-man dungeon roster' }
        }
    )
    'raid' = @(
        [ordered]@{
            requirement = 'online'
            fields      = @('online')
            test        = { param($v, $c) [bool]$v['online'] }
            detail      = { param($v, $c) 'character is offline (online=false); raid requires an online character' }
        }
        [ordered]@{
            requirement = 'alive'
            fields      = @('alive')
            test        = { param($v, $c) [bool]$v['alive'] }
            detail      = { param($v, $c) 'character is dead (alive=false); raid requires a living character' }
        }
        [ordered]@{
            requirement = 'minimum level'
            fields      = @('level')
            test        = { param($v, $c) [int64]$v['level'] -ge [int64]$c.minLevel }
            detail      = { param($v, $c) "level $($v['level']) is below the raid minimum $($c.minLevel) (Context.minLevel, default 80)" }
        }
        [ordered]@{
            requirement = 'minimum equipped item level'
            fields      = @('equipped_item_level')
            test        = { param($v, $c) [double]$v['equipped_item_level'] -ge [double]$c.minItemLevel }
            detail      = { param($v, $c) "equipped_item_level $($v['equipped_item_level']) is below the raid minimum $($c.minItemLevel) (Context.minItemLevel, default 180)" }
        }
        [ordered]@{
            requirement = 'role assigned'
            fields      = @('roles')
            test        = { param($v, $c) @($v['roles']).Count -ge 1 }
            detail      = { param($v, $c) 'no role assigned (roles list is empty); raid requires at least one of tank/healer/dps' }
        }
    )
    'gather' = @(
        [ordered]@{
            requirement = 'online'
            fields      = @('online')
            test        = { param($v, $c) [bool]$v['online'] }
            detail      = { param($v, $c) 'character is offline (online=false); gather requires an online character' }
        }
        [ordered]@{
            requirement = 'alive'
            fields      = @('alive')
            test        = { param($v, $c) [bool]$v['alive'] }
            detail      = { param($v, $c) 'character is dead (alive=false); gather requires a living character' }
        }
        [ordered]@{
            requirement = 'gathering profession'
            fields      = @('professions')
            test        = {
                param($v, $c)
                @($v['professions'] | Where-Object {
                        $_ -is [System.Collections.IDictionary] -and $_.Contains('name') -and $_.Contains('value') -and
                        ([string]$_.name -in $script:AutopilotRosterGatheringProfessions) -and ([int64]$_.value -ge [int64]$c.minSkill)
                    }).Count -ge 1
            }
            detail      = { param($v, $c) "no gathering profession ($($script:AutopilotRosterGatheringProfessions -join '/')) at skill value >= $($c.minSkill) (Context.minSkill, default 1)" }
        }
        [ordered]@{
            requirement = 'bag space'
            fields      = @('bag_slots_free')
            test        = { param($v, $c) [int64]$v['bag_slots_free'] -ge 4 }
            detail      = { param($v, $c) "bag_slots_free $($v['bag_slots_free']) is below the gather minimum of 4 free slots" }
        }
    )
    'craft' = @(
        [ordered]@{
            requirement = 'online'
            fields      = @('online')
            test        = { param($v, $c) [bool]$v['online'] }
            detail      = { param($v, $c) 'character is offline (online=false); craft requires an online character' }
        }
        [ordered]@{
            requirement = 'crafting profession'
            fields      = @('professions')
            test        = {
                param($v, $c)
                @($v['professions'] | Where-Object {
                        $_ -is [System.Collections.IDictionary] -and $_.Contains('name') -and $_.Contains('value') -and
                        ([string]$_.name -in $script:AutopilotRosterCraftingProfessions) -and ([int64]$_.value -ge [int64]$c.minSkill)
                    }).Count -ge 1
            }
            detail      = { param($v, $c) "no crafting profession ($($script:AutopilotRosterCraftingProfessions -join '/')) at skill value >= $($c.minSkill) (Context.minSkill, default 1)" }
        }
        [ordered]@{
            # money_copper must be an OBSERVED fact before a craft plan can
            # budget reagents. Any non-null value satisfies this requirement;
            # a null routes to unknown[] via the shared needed-field rule.
            requirement = 'money observed'
            fields      = @('money_copper')
            test        = { param($v, $c) $true }
            detail      = { param($v, $c) 'unreachable: money_copper only needs to be an observed (non-null) fact' }
        }
    )
    'pvp' = @(
        [ordered]@{
            requirement = 'online'
            fields      = @('online')
            test        = { param($v, $c) [bool]$v['online'] }
            detail      = { param($v, $c) 'character is offline (online=false); pvp requires an online character' }
        }
        [ordered]@{
            requirement = 'alive'
            fields      = @('alive')
            test        = { param($v, $c) [bool]$v['alive'] }
            detail      = { param($v, $c) 'character is dead (alive=false); pvp requires a living character' }
        }
        [ordered]@{
            requirement = 'minimum level'
            fields      = @('level')
            test        = { param($v, $c) [int64]$v['level'] -ge [int64]$c.minLevel }
            detail      = { param($v, $c) "level $($v['level']) is below the pvp minimum $($c.minLevel) (Context.minLevel, default 10)" }
        }
    )
}

function New-AutopilotRosterProvider {
    <#
    Provider kinds:
      fixture     - parses an autowow.autopilot.roster-snapshot.v1 document from
                    -Path. Unknown schema/version, malformed JSON, and broken
                    character entries all fail closed (Available=$false with a
                    typed reason; zero characters exposed).
      bridge      - reserved future live endpoint. Until it is deployed it
                    reports unavailable rather than inferring or scraping.
      unavailable - no roster evidence at all. Facts are never guessed.
    #>
    param(
        [Parameter(Mandatory)][ValidateSet('fixture', 'bridge', 'unavailable')][string]$Kind,
        [string]$Path
    )
    $provider = [ordered]@{
        Kind            = $Kind
        Available       = $false
        Reason          = 'no roster snapshot evidence; facts are never guessed'
        SnapshotPath    = $null
        ObservedUtc     = $null
        ServerSessionId = $null
        Characters      = [ordered]@{}
    }
    switch ($Kind) {
        'unavailable' { }
        'bridge' {
            $provider.Reason = 'bridge roster provider not deployed; fail closed - facts are never guessed'
        }
        'fixture' {
            if (-not $Path) { throw 'fixture roster provider requires -Path.' }
            $provider.SnapshotPath = $Path
            if (-not (Test-Path -LiteralPath $Path)) {
                $provider.Reason = "roster snapshot not found: $Path"
                break
            }
            try {
                $doc = ConvertTo-AutopilotHashtable (Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json)
            }
            catch {
                $provider.Reason = "roster snapshot unreadable (fail closed): $($_.Exception.Message)"
                break
            }
            if (-not ($doc -is [System.Collections.IDictionary]) -or
                -not $doc.Contains('schema') -or [string]$doc.schema -ne $script:AutopilotRosterSnapshotSchema -or
                -not $doc.Contains('schema_version') -or [int64]$doc.schema_version -ne $script:AutopilotRosterSnapshotSchemaVersion) {
                $provider.Reason = "roster snapshot schema not supported (fail closed): expected $($script:AutopilotRosterSnapshotSchema) v$($script:AutopilotRosterSnapshotSchemaVersion)"
                break
            }
            $structurallyBroken = $null
            foreach ($required in @('source', 'observed_utc', 'characters')) {
                if (-not $doc.Contains($required) -or $null -eq $doc[$required]) {
                    $structurallyBroken = "roster snapshot missing required field '$required' (fail closed)"
                    break
                }
            }
            if ($structurallyBroken) { $provider.Reason = $structurallyBroken; break }
            if ([string]$doc.source -notin @('fixture', 'bridge')) {
                $provider.Reason = "roster snapshot source '$($doc.source)' not recognized (fail closed)"
                break
            }
            try {
                $observed = ConvertTo-AutopilotUtcDateTime $doc.observed_utc
            }
            catch {
                $provider.Reason = "roster snapshot observed_utc unparseable (fail closed): $($_.Exception.Message)"
                break
            }
            $characters = [ordered]@{}
            $characterProblem = $null
            foreach ($character in @($doc.characters)) {
                if (-not ($character -is [System.Collections.IDictionary]) -or -not $character.Contains('guid') -or $null -eq $character.guid) {
                    $characterProblem = 'roster snapshot contains a character entry without a guid (fail closed)'
                    break
                }
                try { $guid = [int64]$character.guid } catch { $characterProblem = "roster snapshot character guid '$($character.guid)' is not an integer (fail closed)"; break }
                if ($guid -lt 1) { $characterProblem = "roster snapshot character guid '$guid' must be positive (fail closed)"; break }
                if ($characters.Contains($guid)) { $characterProblem = "roster snapshot contains duplicate character guid '$guid' (fail closed)"; break }
                $characters[$guid] = $character
            }
            if ($characterProblem) { $provider.Reason = $characterProblem; break }
            $provider.Available = $true
            $provider.Reason = 'roster snapshot loaded'
            $provider.ObservedUtc = $observed
            $provider.ServerSessionId = if ($doc.Contains('server_session_id') -and $null -ne $doc.server_session_id) { [string]$doc.server_session_id } else { $null }
            $provider.Characters = $characters
        }
    }
    return [pscustomobject]$provider
}

function Get-AutopilotRosterFact {
    # The character's ordered hashtable, or $null when the provider is
    # unavailable or the guid is absent. $null is the honest answer, never a
    # synthesized placeholder character.
    param(
        [Parameter(Mandatory)]$Provider,
        [Parameter(Mandatory)][int64]$Guid
    )
    if ($null -eq $Provider -or -not $Provider.Available) { return $null }
    # Characters is keyed by boxed [int64]; the [int64] parameter cast keeps the
    # OrderedDictionary key indexer engaged (an int32 literal would hit the
    # POSITIONAL indexer instead - that trap is why the cast below is explicit).
    $key = [int64]$Guid
    if ($Provider.Characters.Contains($key)) { return $Provider.Characters[$key] }
    return $null
}

function Get-AutopilotRosterFacts {
    param([Parameter(Mandatory)]$Provider)
    if ($null -eq $Provider -or -not $Provider.Available) { return , @() }
    return , @($Provider.Characters.Values)
}

function Test-AutopilotRosterFactFresh {
    # Freshness is judged ONLY from the fact's own observed_utc via
    # ConvertTo-AutopilotUtcDateTime (never naive string+Parse, which shifts by
    # machine timezone). Fail closed: no fact, no observed_utc, an unparseable
    # timestamp, or a future-dated observation all report not-fresh.
    param(
        $Fact,
        [Parameter(Mandatory)][datetime]$Now,
        [int]$MaxAgeSeconds = 300
    )
    if ($null -eq $Fact -or -not ($Fact -is [System.Collections.IDictionary])) { return $false }
    if (-not $Fact.Contains('observed_utc') -or $null -eq $Fact['observed_utc']) { return $false }
    try { $observed = ConvertTo-AutopilotUtcDateTime $Fact['observed_utc'] } catch { return $false }
    $ageSeconds = ($Now.ToUniversalTime() - $observed).TotalSeconds
    return (($ageSeconds -ge 0) -and ($ageSeconds -le $MaxAgeSeconds))
}

function Get-AutopilotRosterFieldValue {
    # Resolves a dotted field path (e.g. 'party.raid') against a character fact.
    # Any absent segment resolves to $null (= not observed). Arrays are returned
    # with the unary-comma wrap so a KNOWN-empty list ([]) survives as an empty
    # array instead of unrolling to $null and masquerading as unknown.
    param(
        [Parameter(Mandatory)]$Fact,
        [Parameter(Mandatory)][string]$Field
    )
    $current = $Fact
    foreach ($segment in $Field.Split('.')) {
        if ($null -eq $current) { return $null }
        if (-not ($current -is [System.Collections.IDictionary])) { return $null }
        if (-not $current.Contains($segment)) { return $null }
        $current = $current[$segment]
    }
    if ($null -eq $current) { return $null }
    if ($current -is [System.Collections.IEnumerable] -and $current -isnot [string] -and $current -isnot [System.Collections.IDictionary]) {
        return , @($current)
    }
    return $current
}

function Get-AutopilotRosterSuitability {
    <#
    Judges one character against one activity's requirement table.
    Result: {activity, guid, suitable, missing, unknown, summary} where
      suitable = $true  - every requirement observed and met
                 $false - at least one requirement DEFINITIVELY missing
                          (unknown[] may also be populated alongside)
                 $null  - unknown facts prevent judgment and nothing is
                          definitively missing; never guessed to a verdict
      missing  = [{requirement, detail}] definitive failures, citing thresholds
      unknown  = [{field, why}] needed facts that were not observed
    Explanations over scores.
    #>
    param(
        [Parameter(Mandatory)]$Provider,
        [Parameter(Mandatory)][int64]$Guid,
        [Parameter(Mandatory)][ValidateSet('dungeon', 'raid', 'gather', 'craft', 'pvp')][string]$Activity,
        [hashtable]$Context
    )
    # Resolve context: activity defaults overridden by caller-supplied keys.
    $ctx = [ordered]@{}
    foreach ($key in $script:AutopilotRosterActivityContextDefaults[$Activity].Keys) {
        $ctx[$key] = $script:AutopilotRosterActivityContextDefaults[$Activity][$key]
    }
    if ($Context) {
        foreach ($key in $Context.Keys) { $ctx[[string]$key] = $Context[$key] }
    }

    $missing = [System.Collections.Generic.List[object]]::new()
    $unknown = [System.Collections.Generic.List[object]]::new()

    $fact = Get-AutopilotRosterFact -Provider $Provider -Guid $Guid
    if ($null -eq $fact) {
        $why = if ($null -eq $Provider -or -not $Provider.Available) {
            "roster provider unavailable: $(if ($Provider) { $Provider.Reason } else { 'no provider' })"
        }
        else { 'guid not present in the roster snapshot' }
        $unknown.Add([pscustomobject]@{ field = 'character'; why = $why })
        return [pscustomobject]@{
            activity = $Activity
            guid     = $Guid
            suitable = $null
            missing  = @()
            unknown  = @($unknown)
            summary  = ('cannot judge {0} for guid {1}: no roster fact is available ({2}); suitability is never guessed.' -f $Activity, $Guid, $why)
        }
    }

    $requirements = @($script:AutopilotRosterActivityRequirements[$Activity])
    foreach ($requirement in $requirements) {
        $values = @{}
        $nullFields = @()
        foreach ($field in @($requirement.fields)) {
            $value = Get-AutopilotRosterFieldValue -Fact $fact -Field $field
            $values[$field] = $value
            if ($null -eq $value) { $nullFields += $field }
        }
        if (@($nullFields).Count -gt 0) {
            # HONESTY: a null fact needed by a requirement is UNKNOWN, not
            # missing. It can never be counted as a definitive failure.
            foreach ($field in $nullFields) {
                if (@($unknown | Where-Object { $_.field -eq $field }).Count -eq 0) {
                    $unknown.Add([pscustomobject]@{ field = $field; why = 'fact not observed' })
                }
            }
            continue
        }
        if (-not (& $requirement.test $values $ctx)) {
            $missing.Add([pscustomobject]@{
                    requirement = [string]$requirement.requirement
                    detail      = [string](& $requirement.detail $values $ctx)
                })
        }
    }

    # Verdict: a definitive miss wins ($false, with any unknowns still listed);
    # otherwise any unknown forces $null; only a fully observed clean pass is $true.
    $suitable = if (@($missing).Count -gt 0) { $false }
    elseif (@($unknown).Count -gt 0) { $null }
    else { $true }

    $summary = if ($true -eq $suitable) {
        'guid {0} is suitable for {1}: all {2} requirements are observed and met.' -f $Guid, $Activity, @($requirements).Count
    }
    elseif ($false -eq $suitable) {
        $text = 'guid {0} is not suitable for {1}: {2}.' -f $Guid, $Activity, (@($missing | ForEach-Object { $_.detail }) -join '; ')
        if (@($unknown).Count -gt 0) {
            $text += (' Additionally {0} needed fact(s) were not observed: {1}.' -f @($unknown).Count, (@($unknown | ForEach-Object { $_.field }) -join ', '))
        }
        $text
    }
    else {
        'cannot judge {0} for guid {1}: needed fact(s) not observed ({2}); no requirement is definitively missing, so suitability stays unknown rather than guessed.' -f $Activity, $Guid, (@($unknown | ForEach-Object { $_.field }) -join ', ')
    }

    return [pscustomobject]@{
        activity = $Activity
        guid     = $Guid
        suitable = $suitable
        missing  = @($missing)
        unknown  = @($unknown)
        summary  = $summary
    }
}
