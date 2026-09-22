[CmdletBinding()]
param(
    [string]$Root
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($Root)) {
    $scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
    $Root = (Resolve-Path (Join-Path $scriptRoot '..\..')).Path
}

function Read-Text([string]$RelativePath) {
    $path = Join-Path $Root $RelativePath
    if (-not (Test-Path -LiteralPath $path)) {
        throw "missing file: $path"
    }
    return Get-Content -LiteralPath $path -Raw
}

function Assert-Contains([string]$Text, [string]$Needle, [string]$Label) {
    if ($Text.IndexOf($Needle, [System.StringComparison]::Ordinal) -lt 0) {
        throw "[$Label] missing: $Needle"
    }
}

function Assert-NotContains([string]$Text, [string]$Needle, [string]$Label) {
    if ($Text.IndexOf($Needle, [System.StringComparison]::Ordinal) -ge 0) {
        throw "[$Label] forbidden: $Needle"
    }
}

function Assert-Before([string]$Text, [string]$First, [string]$Second, [string]$Label) {
    $firstIndex = $Text.IndexOf($First, [System.StringComparison]::Ordinal)
    $secondIndex = $Text.IndexOf($Second, [System.StringComparison]::Ordinal)
    if ($firstIndex -lt 0 -or $secondIndex -lt 0 -or $firstIndex -ge $secondIndex) {
        throw "[$Label] expected '$First' before '$Second'"
    }
}

function Slice([string]$Text, [string]$Start, [string]$End) {
    $begin = $Text.IndexOf($Start, [System.StringComparison]::Ordinal)
    if ($begin -lt 0) { throw "slice start missing: $Start" }
    $finish = $Text.IndexOf($End, $begin + $Start.Length, [System.StringComparison]::Ordinal)
    if ($finish -lt 0) { return $Text.Substring($begin) }
    return $Text.Substring($begin, $finish - $begin)
}

$policy = Read-Text 'src\Ai\World\Gathering\WorkerGatherDeathRecoveryPolicy.h'
foreach ($marker in @(
    'MaxReleaseAttempts = 2',
    'MaxCorpseRouteAttempts = 8',
    'MaxReclaimAttempts = 4',
    'DeathRecoveryAction::ReleaseSpirit',
    'DeathRecoveryAction::WalkToCorpse',
    'DeathRecoveryAction::ReclaimCorpse',
    'DeathRecoveryAction::Wait',
    'DeathRecoveryPhase::Blocked',
    'DeathRecoveryPhase::Recovered'
)) {
    Assert-Contains $policy $marker 'policy'
}

$release = Read-Text 'src\Ai\Base\Actions\ReleaseSpiritAction.cpp'
$releaseBody = Slice $release 'bool ReleaseSpiritAction::Execute' 'void ReleaseSpiritAction::IncrementDeathCount'
Assert-Contains $releaseBody 'PlanDeathRecovery' 'worker manual release'
Assert-Before $releaseBody 'if (AutoWowGather::IsExplicitWorker' 'DurabilityRepairAll' 'worker manual release guard'
$workerRelease = Slice $release `
    'if (AutoWowGather::IsExplicitWorker(bot->GetGUID().GetCounter()) &&' `
    'if (IsReleasedGroupedInstanceCorpse(bot)'
Assert-Contains $workerRelease 'PlanDeathRecovery' 'worker release'
Assert-Contains $workerRelease 'HandleRepopRequestOpcode' 'worker release'
Assert-NotContains $workerRelease 'DurabilityRepairAll' 'worker release'
Assert-NotContains $workerRelease 'TeleportTo(' 'worker release'
Assert-NotContains $workerRelease 'ResurrectPlayer(' 'worker release'

$revive = Read-Text 'src\Ai\Base\Actions\ReviveFromCorpseAction.cpp'
$reviveBody = Slice $revive 'bool ReviveFromCorpseAction::Execute' 'bool FindCorpseAction::Execute'
Assert-Contains $reviveBody 'PlanDeathRecovery' 'worker reclaim'
Assert-Contains $reviveBody 'HandleReclaimCorpseOpcode' 'worker reclaim'
Assert-NotContains $reviveBody 'TeleportTo(' 'worker reclaim'
Assert-NotContains $reviveBody 'ResurrectPlayer(' 'worker reclaim'

$findBody = Slice $revive 'bool FindCorpseAction::Execute' 'bool FindCorpseAction::isUseful'
Assert-Contains $findBody 'PlanDeathRecovery' 'worker corpse route'
Assert-Contains $findBody 'no_teleport_policy_missing' 'worker corpse route'
Assert-Before $findBody 'no_teleport_policy_missing' 'TeleportTo(' 'worker corpse route guard'

$workerAction = Read-Text 'src\Ai\World\Gathering\WorkerGatherAction.cpp'
Assert-Contains $workerAction 'ObserveWorkerAlive' 'worker resume'
Assert-Contains $workerAction 'if (!bot->IsAlive())' 'worker resume'

$state = Read-Text 'src\Ai\World\Gathering\GatheringWorkerState.cpp'
Assert-Contains $state 'worker.gatherReceipt = WorkerState::GatherReceipt{}' 'stale receipt reset'
Assert-Contains $state 'worker.deathRecovery = transition.next' 'recovery state persistence'

$bridge = Read-Text 'src\AutoWow\AutoWowBridge.cpp'
foreach ($marker in @(
    ',\"death_recovery\"',
    'DeathRecoveryPhaseName',
    'release_attempts',
    'route_attempts',
    'reclaim_attempts',
    'recoverySnapshot.deathRecovery.active',
    'DeathRecoveryPhase::Blocked'
)) {
    Assert-Contains $bridge $marker 'bridge telemetry'
}

$cmake = Read-Text 'mod-playerbots.cmake'
Assert-Contains $cmake 'WorkerGatherDeathRecoveryPolicyTest.cpp' 'test registration'

Write-Output 'worker-gather-death-recovery static contract: PASS'
