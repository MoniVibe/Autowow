[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateNotNullOrEmpty()]
    [string]$PlayerbotsLogPath,

    [Parameter(Mandatory)]
    [ValidateNotNullOrEmpty()]
    [string]$BeforeSnapshotPath,

    [Parameter(Mandatory)]
    [ValidateNotNullOrEmpty()]
    [string]$AfterSnapshotPath,

    [ValidateRange(1, [int]::MaxValue)]
    [int]$StartLine = 1,

    [ValidateRange(1, [int]::MaxValue)]
    [int]$EndLine = [int]::MaxValue,

    [Nullable[datetime]]$StartTimeUtc,
    [Nullable[datetime]]$EndTimeUtc,

    [Alias('Roster')]
    [string[]]$RosterGuid
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:AcceptanceSchema = 'autowow.raid-loot-equip-acceptance.v1'
$script:SnapshotSchema = 'raid-loot-snapshot-v1'
$script:FixtureInitPattern = '(?i)(?:\bfixture[-_]init\b|"action"\s*:\s*"fixture-init"|"operation"\s*:\s*"fixture_init")'

function Resolve-ReadOnlyInputFile {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Label
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Label not found: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function ConvertTo-ExactUInt64 {
    param(
        [AllowNull()][object]$Value,
        [Parameter(Mandatory)][string]$Label,
        [switch]$AllowZero,
        [uint64]$Maximum = [uint64]::MaxValue
    )

    $text = if ($null -eq $Value) { '' } else { "$Value" }
    [uint64]$number = 0
    if ($text -notmatch '^\d+$' -or -not [uint64]::TryParse($text, [ref]$number) -or
        $number -gt $Maximum -or (-not $AllowZero -and $number -eq 0)) {
        $minimum = if ($AllowZero) { 0 } else { 1 }
        throw "$Label must be an exact integer in the range $minimum..$Maximum."
    }
    return $number
}

function ConvertTo-ExactUInt32 {
    param(
        [AllowNull()][object]$Value,
        [Parameter(Mandatory)][string]$Label,
        [switch]$AllowZero
    )

    return [uint32](ConvertTo-ExactUInt64 -Value $Value -Label $Label -AllowZero:$AllowZero -Maximum ([uint32]::MaxValue))
}

function Get-RequiredPropertyValue {
    param(
        [Parameter(Mandatory)][object]$InputObject,
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][string]$Label
    )

    $property = $InputObject.PSObject.Properties[$Name]
    if ($null -eq $property) {
        throw "$Label is missing required property '$Name'."
    }
    return $property.Value
}

function ConvertTo-ExactRoster {
    param([AllowNull()][string[]]$Values)

    if ($null -eq $Values -or $Values.Count -eq 0) {
        return @()
    }

    $result = [System.Collections.Generic.List[uint32]]::new()
    $seen = [System.Collections.Generic.HashSet[uint32]]::new()
    foreach ($rawValue in $Values) {
        foreach ($value in @("$rawValue" -split ',')) {
            $guid = ConvertTo-ExactUInt32 -Value $value -Label 'Roster GUID'
            if (-not $seen.Add($guid)) {
                throw "Roster GUID '$guid' was supplied more than once."
            }
            [void]$result.Add($guid)
        }
    }
    return @($result | Sort-Object)
}

function Read-AndValidateSnapshot {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Label
    )

    try {
        $snapshot = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json -ErrorAction Stop
    }
    catch {
        throw "$Label is not valid JSON: $($_.Exception.Message)"
    }
    if ($null -eq $snapshot) {
        throw "$Label is empty."
    }
    if ("$(Get-RequiredPropertyValue -InputObject $snapshot -Name 'schema_version' -Label $Label)" -ne $script:SnapshotSchema) {
        throw "$Label schema_version must be $script:SnapshotSchema."
    }

    $roster = @(
        @(Get-RequiredPropertyValue -InputObject $snapshot -Name 'roster_guids' -Label $Label) |
            ForEach-Object { ConvertTo-ExactUInt32 -Value $_ -Label "$Label roster GUID" } |
            Sort-Object
    )
    if ($roster.Count -eq 0 -or @($roster | Select-Object -Unique).Count -ne $roster.Count) {
        throw "$Label roster_guids must contain unique non-zero uint32 values."
    }

    $characters = @(Get-RequiredPropertyValue -InputObject $snapshot -Name 'characters' -Label $Label)
    if ($characters.Count -ne $roster.Count) {
        throw "$Label characters must exactly cover roster_guids."
    }
    $characterGuids = [System.Collections.Generic.List[uint32]]::new()
    foreach ($character in $characters) {
        $characterGuid = ConvertTo-ExactUInt32 -Value (Get-RequiredPropertyValue $character 'guid' "$Label character") -Label "$Label character GUID"
        [void]$characterGuids.Add($characterGuid)
        $slots = @(Get-RequiredPropertyValue $character 'equipped_slots' "$Label character $characterGuid")
        if ($slots.Count -ne 19) {
            throw "$Label character $characterGuid must contain exactly 19 equipped slots."
        }
        $seenSlots = [System.Collections.Generic.HashSet[int]]::new()
        foreach ($slotRecord in $slots) {
            $slot = [int](ConvertTo-ExactUInt64 -Value (Get-RequiredPropertyValue $slotRecord 'slot' "$Label equipped slot") -Label "$Label equipped slot" -AllowZero -Maximum 18)
            if (-not $seenSlots.Add($slot)) {
                throw "$Label character $characterGuid contains duplicate equipped slot $slot."
            }
            $itemGuid = Get-RequiredPropertyValue $slotRecord 'item_guid' "$Label character $characterGuid slot $slot"
            $entry = Get-RequiredPropertyValue $slotRecord 'entry' "$Label character $characterGuid slot $slot"
            if (($null -eq $itemGuid) -ne ($null -eq $entry)) {
                throw "$Label character $characterGuid slot $slot has an incomplete item identity."
            }
            if ($null -ne $itemGuid) {
                [void](ConvertTo-ExactUInt32 -Value $itemGuid -Label "$Label item GUID")
                [void](ConvertTo-ExactUInt32 -Value $entry -Label "$Label item entry")
            }
        }
        if ($seenSlots.Count -ne 19) {
            throw "$Label character $characterGuid does not cover equipped slots 0..18."
        }
        foreach ($item in @(Get-RequiredPropertyValue $character 'carried_items' "$Label character $characterGuid")) {
            [void](ConvertTo-ExactUInt32 -Value (Get-RequiredPropertyValue $item 'item_guid' "$Label carried item") -Label "$Label carried item GUID")
            [void](ConvertTo-ExactUInt32 -Value (Get-RequiredPropertyValue $item 'entry' "$Label carried item") -Label "$Label carried item entry")
        }
    }
    $sortedCharacterGuids = @($characterGuids | Sort-Object)
    if (($sortedCharacterGuids -join ',') -ne ($roster -join ',')) {
        throw "$Label characters must exactly match roster_guids."
    }

    return [pscustomobject]@{ Snapshot = $snapshot; Roster = $roster }
}

function ConvertTo-UtcBoundary {
    param([Parameter(Mandatory)][datetime]$Value)

    if ($Value.Kind -eq [DateTimeKind]::Unspecified) {
        return [DateTime]::SpecifyKind($Value, [DateTimeKind]::Utc)
    }
    return $Value.ToUniversalTime()
}

function Get-LineTimestampUtc {
    param([Parameter(Mandatory)][string]$Line)

    $match = [regex]::Match($Line, '(?<!\d)(?<stamp>\d{4}-\d{2}-\d{2}[ T_]\d{2}:\d{2}:\d{2}(?:\.\d+)?(?:Z|[+-]\d{2}:?\d{2})?)')
    if (-not $match.Success) {
        return $null
    }
    $stamp = $match.Groups['stamp'].Value -replace '_', 'T'
    [datetimeoffset]$parsed = [datetimeoffset]::MinValue
    $styles = [Globalization.DateTimeStyles]::AssumeUniversal -bor [Globalization.DateTimeStyles]::AdjustToUniversal
    if (-not [datetimeoffset]::TryParse($stamp, [Globalization.CultureInfo]::InvariantCulture, $styles, [ref]$parsed)) {
        return $null
    }
    return $parsed.UtcDateTime
}

function Get-IntervalLines {
    param(
        [Parameter(Mandatory)][string[]]$Lines,
        [Parameter(Mandatory)][int]$FirstLine,
        [Parameter(Mandatory)][int]$LastLine,
        [AllowNull()][Nullable[datetime]]$StartUtc,
        [AllowNull()][Nullable[datetime]]$EndUtc
    )

    if ($FirstLine -gt $LastLine) {
        throw 'StartLine must be less than or equal to EndLine.'
    }
    if ($Lines.Count -gt 0 -and $FirstLine -gt $Lines.Count) {
        throw "StartLine $FirstLine is beyond the $($Lines.Count)-line log."
    }

    $normalizedStart = if ($null -eq $StartUtc) { $null } else { ConvertTo-UtcBoundary $StartUtc.Value }
    $normalizedEnd = if ($null -eq $EndUtc) { $null } else { ConvertTo-UtcBoundary $EndUtc.Value }
    if ($null -ne $normalizedStart -and $null -ne $normalizedEnd -and $normalizedStart -gt $normalizedEnd) {
        throw 'StartTimeUtc must be less than or equal to EndTimeUtc.'
    }

    $selected = [System.Collections.Generic.List[object]]::new()
    $boundedEnd = [Math]::Min($LastLine, $Lines.Count)
    for ($lineNumber = $FirstLine; $lineNumber -le $boundedEnd; $lineNumber++) {
        $text = $Lines[$lineNumber - 1]
        $timestamp = Get-LineTimestampUtc -Line $text
        $relevant = $text.Contains('[RaidLoot]') -or $text -match $script:FixtureInitPattern
        if (($null -ne $normalizedStart -or $null -ne $normalizedEnd) -and $relevant -and $null -eq $timestamp) {
            throw "Cannot apply UTC interval bounds because relevant log line $lineNumber has no parseable timestamp."
        }
        if ($null -ne $normalizedStart -and ($null -eq $timestamp -or $timestamp -lt $normalizedStart)) { continue }
        if ($null -ne $normalizedEnd -and ($null -eq $timestamp -or $timestamp -gt $normalizedEnd)) { continue }
        [void]$selected.Add([pscustomobject]@{ Number = $lineNumber; Text = $text; TimestampUtc = $timestamp })
    }
    return @($selected)
}

function Assert-EventFields {
    param(
        [Parameter(Mandatory)][hashtable]$Fields,
        [Parameter(Mandatory)][string[]]$Required,
        [Parameter(Mandatory)][int]$LineNumber
    )

    foreach ($name in $Required) {
        if (-not $Fields.ContainsKey($name) -or [string]::IsNullOrWhiteSpace("$($Fields[$name])")) {
            throw "Malformed RaidLoot telemetry at line ${LineNumber}: missing '$name'."
        }
    }
}

function Assert-RawGuidCounter {
    param(
        [Parameter(Mandatory)][uint64]$Raw,
        [Parameter(Mandatory)][uint32]$Counter,
        [Parameter(Mandatory)][string]$Label,
        [Parameter(Mandatory)][int]$LineNumber
    )

    if ([uint32]($Raw % ([uint64][uint32]::MaxValue + 1)) -ne $Counter) {
        throw "Malformed RaidLoot telemetry at line ${LineNumber}: $Label raw GUID and counter disagree."
    }
}

function ConvertFrom-RaidLootLine {
    param([Parameter(Mandatory)][object]$LineRecord)

    $marker = $LineRecord.Text.IndexOf('[RaidLoot]', [StringComparison]::Ordinal)
    if ($marker -lt 0) { return $null }
    $payload = $LineRecord.Text.Substring($marker + '[RaidLoot]'.Length).Trim()
    $fields = @{}
    foreach ($match in [regex]::Matches($payload, '(?<!\S)(?<key>[a-z][a-z0-9_]*)=(?<value>[^\s]+)')) {
        $key = $match.Groups['key'].Value
        if ($fields.ContainsKey($key)) {
            throw "Malformed RaidLoot telemetry at line $($LineRecord.Number): duplicate '$key'."
        }
        $fields[$key] = $match.Groups['value'].Value
    }
    Assert-EventFields -Fields $fields -Required @('event') -LineNumber $LineRecord.Number
    $eventName = "$($fields['event'])"

    switch ($eventName) {
        'vote' {
            Assert-EventFields $fields @('source','bot','bot_guid','item','item_guid','usage','vote') $LineRecord.Number
            [void](ConvertTo-ExactUInt64 $fields.bot_guid 'vote bot_guid')
            [void](ConvertTo-ExactUInt32 $fields.item 'vote item')
            [void](ConvertTo-ExactUInt64 $fields.item_guid 'vote item_guid')
            [void](ConvertTo-ExactUInt32 $fields.usage 'vote usage' -AllowZero)
        }
        'award' {
            Assert-EventFields $fields @('winner','winner_guid','winner_guid_counter','item','entry','roll_guid','roll_guid_counter','item_guid','item_guid_counter','count','vote') $LineRecord.Number
            $winnerRaw = ConvertTo-ExactUInt64 $fields.winner_guid 'award winner_guid'
            $winnerCounter = ConvertTo-ExactUInt32 $fields.winner_guid_counter 'award winner_guid_counter'
            $rollRaw = ConvertTo-ExactUInt64 $fields.roll_guid 'award roll_guid'
            $rollCounter = ConvertTo-ExactUInt32 $fields.roll_guid_counter 'award roll_guid_counter'
            $itemRaw = ConvertTo-ExactUInt64 $fields.item_guid 'award item_guid'
            $itemCounter = ConvertTo-ExactUInt32 $fields.item_guid_counter 'award item_guid_counter'
            [void](ConvertTo-ExactUInt32 $fields.item 'award item')
            [void](ConvertTo-ExactUInt32 $fields.entry 'award entry')
            [void](ConvertTo-ExactUInt32 $fields.count 'award count')
            Assert-RawGuidCounter $winnerRaw $winnerCounter 'winner' $LineRecord.Number
            Assert-RawGuidCounter $rollRaw $rollCounter 'roll' $LineRecord.Number
            Assert-RawGuidCounter $itemRaw $itemCounter 'item' $LineRecord.Number
        }
        'evaluate' {
            Assert-EventFields $fields @('trigger','source','bot','bot_guid','bot_guid_counter','item','entry','item_guid','item_guid_counter','usage','selected') $LineRecord.Number
            $botRaw = ConvertTo-ExactUInt64 $fields.bot_guid 'evaluate bot_guid'
            $botCounter = ConvertTo-ExactUInt32 $fields.bot_guid_counter 'evaluate bot_guid_counter'
            $itemRaw = ConvertTo-ExactUInt64 $fields.item_guid 'evaluate item_guid'
            $itemCounter = ConvertTo-ExactUInt32 $fields.item_guid_counter 'evaluate item_guid_counter'
            [void](ConvertTo-ExactUInt32 $fields.item 'evaluate item')
            [void](ConvertTo-ExactUInt32 $fields.entry 'evaluate entry')
            [void](ConvertTo-ExactUInt32 $fields.usage 'evaluate usage' -AllowZero)
            if ("$($fields.selected)" -ne 'true') { throw "Malformed RaidLoot telemetry at line $($LineRecord.Number): selected must be true." }
            Assert-RawGuidCounter $botRaw $botCounter 'bot' $LineRecord.Number
            Assert-RawGuidCounter $itemRaw $itemCounter 'item' $LineRecord.Number
        }
        'equip' {
            Assert-EventFields $fields @('bot','bot_guid','bot_guid_counter','item','entry','item_guid','item_guid_counter','slot','previous_item','previous_item_guid_counter','result') $LineRecord.Number
            $botRaw = ConvertTo-ExactUInt64 $fields.bot_guid 'equip bot_guid'
            $botCounter = ConvertTo-ExactUInt32 $fields.bot_guid_counter 'equip bot_guid_counter'
            $itemRaw = ConvertTo-ExactUInt64 $fields.item_guid 'equip item_guid'
            $itemCounter = ConvertTo-ExactUInt32 $fields.item_guid_counter 'equip item_guid_counter'
            [void](ConvertTo-ExactUInt32 $fields.item 'equip item')
            [void](ConvertTo-ExactUInt32 $fields.entry 'equip entry')
            [void](ConvertTo-ExactUInt64 $fields.slot 'equip slot' -AllowZero -Maximum 18)
            $previousItem = ConvertTo-ExactUInt32 $fields.previous_item 'equip previous_item' -AllowZero
            $previousCounter = ConvertTo-ExactUInt32 $fields.previous_item_guid_counter 'equip previous_item_guid_counter' -AllowZero
            if (($previousItem -eq 0) -ne ($previousCounter -eq 0)) {
                throw "Malformed RaidLoot telemetry at line $($LineRecord.Number): previous item entry and counter must both be zero or both be non-zero."
            }
            if ("$($fields.result)" -ne 'equipped') { throw "Malformed RaidLoot telemetry at line $($LineRecord.Number): result must be equipped." }
            Assert-RawGuidCounter $botRaw $botCounter 'bot' $LineRecord.Number
            Assert-RawGuidCounter $itemRaw $itemCounter 'item' $LineRecord.Number
        }
        default {
            throw "Malformed RaidLoot telemetry at line $($LineRecord.Number): unknown event '$eventName'."
        }
    }

    return [pscustomobject]@{
        Event = $eventName
        Fields = $fields
        Line = $LineRecord.Number
        TimestampUtc = $LineRecord.TimestampUtc
    }
}

function Get-SnapshotCharacter {
    param([Parameter(Mandatory)][object]$Snapshot, [Parameter(Mandatory)][uint32]$Guid)
    return ($Snapshot.characters | Where-Object { [uint32]$_.guid -eq $Guid } | Select-Object -First 1)
}

function Get-EquippedSnapshotSlot {
    param(
        [Parameter(Mandatory)][object]$Snapshot,
        [Parameter(Mandatory)][uint32]$CharacterGuid,
        [Parameter(Mandatory)][int]$Slot
    )
    $character = Get-SnapshotCharacter $Snapshot $CharacterGuid
    if ($null -eq $character) { return $null }
    return ($character.equipped_slots | Where-Object { [int]$_.slot -eq $Slot } | Select-Object -First 1)
}

function Find-SnapshotItemInstances {
    param([Parameter(Mandatory)][object]$Snapshot, [Parameter(Mandatory)][uint32]$ItemGuid)

    $found = [System.Collections.Generic.List[object]]::new()
    foreach ($character in @($Snapshot.characters)) {
        foreach ($slot in @($character.equipped_slots)) {
            if ($null -ne $slot.item_guid -and [uint32]$slot.item_guid -eq $ItemGuid) {
                [void]$found.Add([pscustomobject]@{ CharacterGuid = [uint32]$character.guid; Location = 'equipped'; Slot = [int]$slot.slot; Item = $slot })
            }
        }
        foreach ($item in @($character.carried_items)) {
            if ([uint32]$item.item_guid -eq $ItemGuid) {
                [void]$found.Add([pscustomobject]@{ CharacterGuid = [uint32]$character.guid; Location = 'carried'; Slot = [int]$item.slot; Item = $item })
            }
        }
    }
    return @($found)
}

function New-AcceptanceResult {
    param(
        [Parameter(Mandatory)][string]$Status,
        [Parameter(Mandatory)][AllowEmptyCollection()][string[]]$Reasons,
        [Parameter(Mandatory)][int]$TelemetryEventCount,
        [Parameter(Mandatory)][int]$QualifyingCandidateCount,
        [AllowNull()][object]$Sample,
        [Parameter(Mandatory)][uint32[]]$Roster,
        [Parameter(Mandatory)][int]$EffectiveStartLine,
        [Parameter(Mandatory)][int]$EffectiveEndLine
    )

    return [ordered]@{
        schema = $script:AcceptanceSchema
        status = $Status
        read_only = $true
        reasons = @($Reasons | Select-Object -Unique)
        interval = [ordered]@{
            start_line = $EffectiveStartLine
            end_line = $EffectiveEndLine
            start_time_utc = if ($null -eq $StartTimeUtc) { $null } else { (ConvertTo-UtcBoundary $StartTimeUtc.Value).ToString('o') }
            end_time_utc = if ($null -eq $EndTimeUtc) { $null } else { (ConvertTo-UtcBoundary $EndTimeUtc.Value).ToString('o') }
        }
        roster_guids = @($Roster)
        telemetry_event_count = $TelemetryEventCount
        qualifying_candidate_count = $QualifyingCandidateCount
        sample = $Sample
    }
}

$logPath = Resolve-ReadOnlyInputFile $PlayerbotsLogPath 'Playerbots log'
$beforePath = Resolve-ReadOnlyInputFile $BeforeSnapshotPath 'Before snapshot'
$afterPath = Resolve-ReadOnlyInputFile $AfterSnapshotPath 'After snapshot'
$requestedRoster = @(ConvertTo-ExactRoster $RosterGuid)
$beforeData = Read-AndValidateSnapshot $beforePath 'Before snapshot'
$afterData = Read-AndValidateSnapshot $afterPath 'After snapshot'

if (($beforeData.Roster -join ',') -ne ($afterData.Roster -join ',')) {
    throw 'Before and after snapshot rosters must exactly match.'
}
if ($requestedRoster.Count -gt 0 -and ($requestedRoster -join ',') -ne ($beforeData.Roster -join ',')) {
    throw 'Optional roster must exactly match both snapshot rosters.'
}
$effectiveRoster = [uint32[]]$beforeData.Roster

$logLines = @(Get-Content -LiteralPath $logPath)
$intervalLines = @(Get-IntervalLines -Lines $logLines -FirstLine $StartLine -LastLine $EndLine -StartUtc $StartTimeUtc -EndUtc $EndTimeUtc)
$effectiveEndLine = if ($intervalLines.Count -eq 0) { [Math]::Min($EndLine, $logLines.Count) } else { $intervalLines[-1].Number }

$fixtureEvidence = @($intervalLines | Where-Object { $_.Text -match $script:FixtureInitPattern })
if ($fixtureEvidence.Count -gt 0) {
    $result = New-AcceptanceResult -Status 'RESET_REQUIRED' -Reasons @('fixture_init_evidence_in_interval') `
        -TelemetryEventCount 0 -QualifyingCandidateCount 0 -Sample $null -Roster $effectiveRoster `
        -EffectiveStartLine $StartLine -EffectiveEndLine $effectiveEndLine
    $result | ConvertTo-Json -Depth 10
    return
}

$events = [System.Collections.Generic.List[object]]::new()
foreach ($line in $intervalLines) {
    if (-not $line.Text.Contains('[RaidLoot]')) { continue }
    $eventRecord = ConvertFrom-RaidLootLine $line
    if ($null -ne $eventRecord) { [void]$events.Add($eventRecord) }
}
$eventArray = @($events)
$qualifyingVotes = @($eventArray | Where-Object {
    $_.Event -eq 'vote' -and $_.Fields.vote -eq 'need' -and [uint32]$_.Fields.usage -in @(1,2,3)
})

$candidates = [System.Collections.Generic.List[object]]::new()
$chainReasons = [System.Collections.Generic.List[string]]::new()
foreach ($vote in $qualifyingVotes) {
    $sameWinnerAwards = @($eventArray | Where-Object {
        $_.Event -eq 'award' -and $_.Line -gt $vote.Line -and $_.Fields.vote -eq 'need' -and
        [uint64]$_.Fields.winner_guid -eq [uint64]$vote.Fields.bot_guid -and
        [uint32]$_.Fields.item -eq [uint32]$vote.Fields.item
    } | Sort-Object Line)
    if ($sameWinnerAwards.Count -eq 0) { continue }

    $award = $sameWinnerAwards | Where-Object { [uint64]$_.Fields.roll_guid -eq [uint64]$vote.Fields.item_guid } | Select-Object -First 1
    if ($null -eq $award) {
        [void]$chainReasons.Add('mismatched_roll_guid')
        continue
    }
    if ([uint32]$award.Fields.item -ne [uint32]$award.Fields.entry) {
        [void]$chainReasons.Add('award_entry_alias_mismatch')
        continue
    }
    [void]$candidates.Add([pscustomobject]@{ Vote = $vote; Award = $award })
}

if ($candidates.Count -eq 0) {
    $status = if ($chainReasons.Count -gt 0) { 'FAIL' } else { 'NO_SAMPLE' }
    $reasons = if ($chainReasons.Count -gt 0) { @($chainReasons) } else { @('no_qualifying_natural_need_upgrade') }
    $result = New-AcceptanceResult -Status $status -Reasons $reasons -TelemetryEventCount $eventArray.Count `
        -QualifyingCandidateCount 0 -Sample $null -Roster $effectiveRoster -EffectiveStartLine $StartLine `
        -EffectiveEndLine $effectiveEndLine
    $result | ConvertTo-Json -Depth 10
    return
}

$passingSample = $null
foreach ($candidate in $candidates) {
    $vote = $candidate.Vote
    $award = $candidate.Award
    $entry = [uint32]$award.Fields.entry
    $winnerRaw = [uint64]$award.Fields.winner_guid
    $winnerCounter = [uint32]$award.Fields.winner_guid_counter
    $itemRaw = [uint64]$award.Fields.item_guid
    $itemCounter = [uint32]$award.Fields.item_guid_counter

    if ($winnerCounter -notin $effectiveRoster) {
        [void]$chainReasons.Add('winner_not_in_snapshot_roster')
        continue
    }

    $sameEntryEvaluations = @($eventArray | Where-Object {
        $_.Event -eq 'evaluate' -and $_.Line -gt $award.Line -and
        [uint64]$_.Fields.bot_guid -eq $winnerRaw -and [uint32]$_.Fields.bot_guid_counter -eq $winnerCounter -and
        [uint32]$_.Fields.entry -eq $entry -and [uint32]$_.Fields.usage -in @(1,2,3) -and $_.Fields.selected -eq 'true'
    } | Sort-Object Line)
    $evaluation = $sameEntryEvaluations | Where-Object {
        [uint64]$_.Fields.item_guid -eq $itemRaw -and [uint32]$_.Fields.item_guid_counter -eq $itemCounter
    } | Select-Object -First 1
    if ($null -eq $evaluation) {
        [void]$chainReasons.Add($(if ($sameEntryEvaluations.Count -gt 0) { 'mismatched_evaluation_item_guid' } else { 'missing_exact_awarded_instance_evaluation' }))
        continue
    }
    if ([uint32]$evaluation.Fields.item -ne [uint32]$evaluation.Fields.entry) {
        [void]$chainReasons.Add('evaluation_entry_alias_mismatch')
        continue
    }

    $sameEntryEquips = @($eventArray | Where-Object {
        $_.Event -eq 'equip' -and $_.Line -gt $evaluation.Line -and $_.Fields.result -eq 'equipped' -and
        [uint64]$_.Fields.bot_guid -eq $winnerRaw -and [uint32]$_.Fields.bot_guid_counter -eq $winnerCounter -and
        [uint32]$_.Fields.entry -eq $entry
    } | Sort-Object Line)
    $equip = $sameEntryEquips | Where-Object {
        [uint64]$_.Fields.item_guid -eq $itemRaw -and [uint32]$_.Fields.item_guid_counter -eq $itemCounter
    } | Select-Object -First 1
    if ($null -eq $equip) {
        [void]$chainReasons.Add($(if ($sameEntryEquips.Count -gt 0) { 'mismatched_equip_item_guid' } else { 'missing_exact_awarded_instance_equip' }))
        continue
    }
    if ([uint32]$equip.Fields.item -ne [uint32]$equip.Fields.entry) {
        [void]$chainReasons.Add('equip_entry_alias_mismatch')
        continue
    }

    $slot = [int]$equip.Fields.slot
    $previousEntry = [uint32]$equip.Fields.previous_item
    $previousCounter = [uint32]$equip.Fields.previous_item_guid_counter
    $beforeSlot = Get-EquippedSnapshotSlot $beforeData.Snapshot $winnerCounter $slot
    $afterSlot = Get-EquippedSnapshotSlot $afterData.Snapshot $winnerCounter $slot
    if ($null -eq $beforeSlot -or $null -eq $afterSlot) {
        [void]$chainReasons.Add('snapshot_character_or_slot_missing')
        continue
    }

    $beforeAwardedInstances = @(Find-SnapshotItemInstances $beforeData.Snapshot $itemCounter)
    $afterAwardedInstances = @(Find-SnapshotItemInstances $afterData.Snapshot $itemCounter)
    if ($beforeAwardedInstances.Count -ne 0) {
        [void]$chainReasons.Add('awarded_item_already_present_before')
        continue
    }
    if ($afterAwardedInstances.Count -ne 1 -or $afterAwardedInstances[0].CharacterGuid -ne $winnerCounter -or
        $afterAwardedInstances[0].Location -ne 'equipped' -or $afterAwardedInstances[0].Slot -ne $slot -or
        [uint32]$afterAwardedInstances[0].Item.entry -ne $entry) {
        [void]$chainReasons.Add('after_snapshot_awarded_item_not_equipped_in_logged_slot')
        continue
    }
    if ($previousEntry -eq 0) {
        if ($null -ne $beforeSlot.item_guid -or $null -ne $beforeSlot.entry) {
            [void]$chainReasons.Add('before_snapshot_expected_empty_slot_mismatch')
            continue
        }
    }
    elseif ($null -eq $beforeSlot.item_guid -or [uint32]$beforeSlot.item_guid -ne $previousCounter -or
            [uint32]$beforeSlot.entry -ne $previousEntry) {
        [void]$chainReasons.Add('before_snapshot_previous_item_mismatch')
        continue
    }
    if ($null -eq $afterSlot.item_guid -or [uint32]$afterSlot.item_guid -ne $itemCounter -or [uint32]$afterSlot.entry -ne $entry) {
        [void]$chainReasons.Add('after_snapshot_slot_transition_mismatch')
        continue
    }

    $passingSample = [ordered]@{
        vote_line = $vote.Line
        award_line = $award.Line
        evaluation_line = $evaluation.Line
        equip_line = $equip.Line
        winner = $award.Fields.winner
        winner_guid = $winnerRaw
        winner_guid_counter = $winnerCounter
        roll_guid = [uint64]$award.Fields.roll_guid
        roll_guid_counter = [uint32]$award.Fields.roll_guid_counter
        item_entry = $entry
        item_guid = $itemRaw
        item_guid_counter = $itemCounter
        vote_usage = [uint32]$vote.Fields.usage
        evaluation_usage = [uint32]$evaluation.Fields.usage
        evaluation_trigger = $evaluation.Fields.trigger
        evaluation_source = $evaluation.Fields.source
        equipped_slot = $slot
        previous_item_entry = $previousEntry
        previous_item_guid_counter = $previousCounter
        snapshot_transition = [ordered]@{
            before_item_guid_counter = if ($null -eq $beforeSlot.item_guid) { $null } else { [uint32]$beforeSlot.item_guid }
            before_entry = if ($null -eq $beforeSlot.entry) { $null } else { [uint32]$beforeSlot.entry }
            after_item_guid_counter = [uint32]$afterSlot.item_guid
            after_entry = [uint32]$afterSlot.entry
        }
    }
    break
}

if ($null -ne $passingSample) {
    $result = New-AcceptanceResult -Status 'PASS' -Reasons @() -TelemetryEventCount $eventArray.Count `
        -QualifyingCandidateCount $candidates.Count -Sample $passingSample -Roster $effectiveRoster `
        -EffectiveStartLine $StartLine -EffectiveEndLine $effectiveEndLine
}
else {
    $result = New-AcceptanceResult -Status 'FAIL' -Reasons @($chainReasons) -TelemetryEventCount $eventArray.Count `
        -QualifyingCandidateCount $candidates.Count -Sample $null -Roster $effectiveRoster `
        -EffectiveStartLine $StartLine -EffectiveEndLine $effectiveEndLine
}
$result | ConvertTo-Json -Depth 10
