[CmdletBinding()]
param(
    [ValidateSet('list','activate','deactivate','independent','party','rally','deploy','route','advance','advance-point','engage','boss','boss-status','scout','pathprobe','quest','quest-acquire','questlog','questobjective','acceptance','combatlog','encounterlog','professioneconomy','craft','craft-status','guild-trade','guild-trade-status','probe-reset','probe-login','probe-setlevel','probe-place','probe-status','fixture-init','fixture-status','fixture-accelerate','fixture-accelerate-off','fixture-kill','raid-create','raid-status','raid-leave','wsg-queue','wsg-status','wsg-leave','destinations','snapshot','pause','resume','travel','recover')][string]$Action = 'list',
    [uint32]$BotGuid = 0,
    [uint32]$QuestId = 0,
    [uint32]$RecipeSpellId = 0,
    [uint32]$BuyerGuid = 0,
    [uint32]$ItemGuid = 0,
    [uint32]$ItemEntry = 0,
    [uint32]$Quantity = 0,
    [uint32]$PriceCopper = 0,
    [ValidateRange(0,10000)][uint32]$PacingPercent = 0,
    [uint64]$StableSpawnId = 0,
    [uint32]$TargetEntry = 0,
    [uint32]$TargetPlayerGuid = 0,
    [float]$CoordinateX = 0,
    [float]$CoordinateY = 0,
    [float]$CoordinateZ = 0,
    [switch]$UseCoordinateOrientation,
    [float]$CoordinateOrientation = 0,
    [switch]$UseSourceCoordinates,
    [float]$SourceX = 0,
    [float]$SourceY = 0,
    [float]$SourceZ = 0,
    [ValidateRange(1,80)][uint32]$Level = 1,
    [ValidateRange(0,19)][uint32]$SpecIndex = 0,
    [ValidateRange(0,5)][uint32]$Quality = 2,
    [ValidateSet(0,1)][uint32]$RaidDifficulty = 0,
    [uint32]$ExpectedMapId = 0,
    [ValidateRange(0,3)][uint32]$ExpectedDifficulty = 0,
    [uint32[]]$MemberGuid = @(),
    [string]$Destination = '',
    [switch]$DropOtherQuests,
    [ValidateSet('127.0.0.1')][string]$BridgeHost = '127.0.0.1',
    [ValidateRange(1,65535)][int]$Port = 18787,
    [ValidateRange(1000,120000)][int]$TimeoutMs = 5000,
    [switch]$EmitRequestOnly
)

$ErrorActionPreference = 'Stop'

$wsgActions = @('wsg-queue', 'wsg-status', 'wsg-leave')
if ($Action -ne 'list' -and $Action -notin $wsgActions -and $BotGuid -eq 0) {
    throw 'BotGuid is required for every bot-scoped bridge action.'
}
if ($Action -eq 'party' -and $MemberGuid.Count -eq 0) {
    throw 'At least one MemberGuid is required for party.'
}
if ($Action -eq 'travel' -and [string]::IsNullOrWhiteSpace($Destination)) {
    throw 'Destination is required for travel.'
}
if ($QuestId -ne 0 -and $Action -notin @('quest','acceptance','probe-place','probe-status')) {
    throw 'QuestId is valid only with the quest, acceptance, probe-place or probe-status action.'
}
if ($Action -eq 'probe-place' -and $QuestId -eq 0) {
    throw 'probe-place requires QuestId.'
}
if ($DropOtherQuests -and $Action -ne 'probe-place') {
    throw 'DropOtherQuests is valid only with probe-place.'
}
if ($Action -eq 'probe-setlevel' -and -not $PSBoundParameters.ContainsKey('Level')) {
    throw 'probe-setlevel requires an explicit Level (up or down).'
}
if ($RecipeSpellId -ne 0 -and $Action -ne 'craft') {
    throw 'RecipeSpellId is valid only with the craft action.'
}
if ($Action -eq 'guild-trade') {
    if ($BuyerGuid -eq 0 -or $BuyerGuid -eq $BotGuid -or $ItemGuid -eq 0 -or
        $ItemEntry -eq 0 -or $Quantity -ne 1 -or $PriceCopper -lt 1 -or $PriceCopper -gt 500) {
        throw 'guild-trade requires a distinct BuyerGuid, exact ItemGuid and ItemEntry, Quantity 1, and PriceCopper 1..500.'
    }
} elseif ($BuyerGuid -ne 0 -or $ItemGuid -ne 0 -or $ItemEntry -ne 0 -or
    $Quantity -ne 0 -or $PriceCopper -ne 0) {
    throw 'Guild trade parameters are valid only with guild-trade.'
}
if ($PacingPercent -ne 0 -and $Action -notin @('fixture-accelerate')) {
    throw 'PacingPercent is valid only with fixture-accelerate.'
}
if ($StableSpawnId -ne 0 -and $Action -ne 'fixture-kill') {
    throw 'StableSpawnId is valid only with fixture-kill.'
}
if ($Action -eq 'fixture-accelerate' -and $PacingPercent -ne 1000) {
    throw 'Fixture acceleration requires exactly 1000 percent (10x) pacing.'
}
if ($Action -eq 'fixture-kill' -and ($StableSpawnId -eq 0 -or $TargetEntry -eq 0)) {
    throw 'Fixture kill requires TargetEntry and StableSpawnId.'
}
if ($TargetEntry -ne 0 -and $Action -notin @('engage','fixture-kill')) {
    throw 'TargetEntry is valid only with engage or fixture-kill.'
}
if ($Action -eq 'craft' -and $RecipeSpellId -eq 0) {
    throw 'RecipeSpellId is required for craft.'
}
if ($TargetEntry -ne 0 -and $Action -notin @('engage', 'fixture-kill')) {
    throw 'TargetEntry is valid only with engage or fixture-kill.'
}
if ($TargetPlayerGuid -ne 0 -and $Action -ne 'engage') {
    throw 'TargetPlayerGuid is valid only with the engage action.'
}
if ($TargetEntry -ne 0 -and $TargetPlayerGuid -ne 0) {
    throw 'TargetEntry and TargetPlayerGuid are mutually exclusive.'
}
if ($Action -eq 'route' -and [string]::IsNullOrWhiteSpace($Destination)) {
    throw 'Destination is required for route.'
}
if ($Action -eq 'probe-reset') {
    $roster = @($BotGuid) + @($MemberGuid)
    $uniqueRoster = @($roster | Sort-Object -Unique)
    if ([string]::IsNullOrWhiteSpace($Destination) -or $Destination -notmatch '(?-i)-exterior$') {
        throw 'probe-reset requires an exact named -exterior Destination.'
    }
    if ($ExpectedMapId -eq 0) { throw 'probe-reset requires a positive ExpectedMapId.' }
    if ($uniqueRoster.Count -ne $roster.Count -or @($roster | Where-Object { $_ -eq 0 }).Count -ne 0) {
        throw 'probe-reset requires a unique positive roster from BotGuid plus MemberGuid.'
    }
}
if (($ExpectedMapId -ne 0 -or $ExpectedDifficulty -ne 0) -and $Action -ne 'probe-reset') {
    throw 'ExpectedMapId and ExpectedDifficulty are valid only with probe-reset.'
}
if ($Action -eq 'advance' -and [string]::IsNullOrWhiteSpace($Destination)) {
    throw 'Destination is required for advance.'
}
if ($UseCoordinateOrientation -and $Action -ne 'advance-point') {
    throw 'UseCoordinateOrientation is valid only with advance-point.'
}
if ($Action -in $wsgActions) {
    $uniqueRoster = @($MemberGuid | Sort-Object -Unique)
    if ($MemberGuid.Count -ne 20 -or $uniqueRoster.Count -ne 20 -or @($MemberGuid | Where-Object { $_ -eq 0 }).Count -ne 0) {
        throw "$Action requires exactly 20 unique positive MemberGuid values in deterministic roster order."
    }
    if ($BotGuid -ne 0) {
        throw 'BotGuid is not valid for atomic WSG roster actions; supply all 20 GUIDs through MemberGuid.'
    }
}
if ($Action -eq 'raid-create') {
    $uniqueMembers = @($MemberGuid | Sort-Object -Unique)
    $totalRaidSize = $MemberGuid.Count + 1
    if ($totalRaidSize -notin @(10, 25, 40)) {
        throw 'raid-create requires leader BotGuid plus exactly 9, 24, or 39 MemberGuid values.'
    }
    if ($uniqueMembers.Count -ne $MemberGuid.Count -or
        @($MemberGuid | Where-Object { $_ -eq 0 -or $_ -eq $BotGuid }).Count -ne 0) {
        throw 'raid-create requires unique positive MemberGuid values that do not repeat the leader BotGuid.'
    }
}
if ($Action -in @('raid-status', 'raid-leave') -and $MemberGuid.Count -ne 0) {
    throw "$Action accepts only BotGuid; MemberGuid is not valid."
}

$request = switch ($Action) {
    'list' { 'list' }
    'activate' { "activate $BotGuid" }
    'deactivate' { "deactivate $BotGuid" }
    'independent' { "independent $BotGuid" }
    'party' { "party $BotGuid $($MemberGuid -join ' ')" }
    'rally' { "rally $BotGuid" }
    'deploy' { "deploy $BotGuid" }
    'route' { "route $BotGuid $Destination" }
    'advance' { "advance $BotGuid $Destination" }
    'advance-point' {
        $wire = "advancepoint $BotGuid $CoordinateX $CoordinateY $CoordinateZ"
        if ($UseCoordinateOrientation) { $wire += " $CoordinateOrientation" }
        $wire
    }
    'engage' {
        if ($TargetEntry -ne 0) { "engage $BotGuid entry $TargetEntry" }
        elseif ($TargetPlayerGuid -ne 0) { "engage $BotGuid player $TargetPlayerGuid" }
        else { "engage $BotGuid" }
    }
    'boss' { "boss $BotGuid" }
    'boss-status' { "boss status $BotGuid" }
    'scout' { "scout $BotGuid" }
    'pathprobe' {
        $wire = "pathprobe $BotGuid $CoordinateX $CoordinateY $CoordinateZ"
        if ($UseSourceCoordinates) { $wire += " $SourceX $SourceY $SourceZ" }
        $wire
    }
    'quest' { if ($QuestId -ne 0) { "quest $BotGuid $QuestId" } else { "quest $BotGuid" } }
    'quest-acquire' { "quest $BotGuid acquire" }
    'questlog' { "questlog $BotGuid" }
    'questobjective' { "questobjective $BotGuid" }
    'acceptance' { if ($QuestId -ne 0) { "acceptance $BotGuid $QuestId" } else { "acceptance $BotGuid" } }
    'combatlog' { "combatlog $BotGuid" }
    'encounterlog' { "encounterlog $BotGuid" }
    'professioneconomy' { "professioneconomy $BotGuid" }
    'craft' { "craft $BotGuid $RecipeSpellId" }
    'craft-status' { "craft-status $BotGuid" }
    'guild-trade' { "guild-trade $BotGuid $BuyerGuid $ItemGuid $ItemEntry $Quantity $PriceCopper" }
    'guild-trade-status' { "guild-trade-status $BotGuid" }
    'probe-reset' { "probe-reset $Destination $ExpectedMapId $ExpectedDifficulty $BotGuid $($MemberGuid -join ' ')".TrimEnd() }
    'probe-login' { "probe-login $BotGuid" }
    'probe-setlevel' { "probe-setlevel $BotGuid $Level $SpecIndex $Quality" }
    'probe-place' { if ($DropOtherQuests) { "probe-place $BotGuid $QuestId drop-others" } else { "probe-place $BotGuid $QuestId" } }
    'probe-status' { if ($QuestId -ne 0) { "probe-status $BotGuid $QuestId" } else { "probe-status $BotGuid" } }
    'fixture-init' { "fixture init $BotGuid $Level $SpecIndex $Quality" }
    'fixture-status' { "fixture status $BotGuid" }
    'fixture-accelerate' { "fixture accelerate $BotGuid $PacingPercent" }
    'fixture-accelerate-off' { "fixture accelerate-off $BotGuid" }
    'fixture-kill' { "fixture kill $BotGuid entry $TargetEntry spawn $StableSpawnId" }
    'raid-create' { "raid create $BotGuid $($MemberGuid -join ' ') difficulty=$RaidDifficulty" }
    'raid-status' { "raid status $BotGuid" }
    'raid-leave' { "raid leave $BotGuid" }
    'wsg-queue' { "wsg queue $($MemberGuid -join ' ')" }
    'wsg-status' { "wsg status $($MemberGuid -join ' ')" }
    'wsg-leave' { "wsg leave $($MemberGuid -join ' ')" }
    'destinations' { "destinations $BotGuid" }
    'snapshot' { "snapshot $BotGuid" }
    'pause' { "pause $BotGuid" }
    'resume' { "resume $BotGuid" }
    'travel' { "travel $BotGuid $Destination" }
    'recover' { "recover $BotGuid" }
}

if ($EmitRequestOnly) { return $request }

$client = [System.Net.Sockets.TcpClient]::new()
try {
    $connect = $client.ConnectAsync($BridgeHost, $Port)
    if (-not $connect.Wait([Math]::Min($TimeoutMs, 10000)) -or -not $client.Connected) {
        throw "Could not connect to the AutoWow bridge at ${BridgeHost}:$Port."
    }

    $stream = $client.GetStream()
    $stream.ReadTimeout = $TimeoutMs
    $writer = [System.IO.StreamWriter]::new($stream)
    $writer.NewLine = "`n"
    $writer.WriteLine($request)
    $writer.Flush()

    $reader = [System.IO.StreamReader]::new($stream)
    $response = $reader.ReadLine()
    if ([string]::IsNullOrWhiteSpace($response)) {
        throw 'The AutoWow bridge returned an empty response.'
    }

    $response
}
finally {
    $client.Dispose()
}
