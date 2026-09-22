<#
    Fixed-roster lifecycle wrapper for the persistent economy gatherers.

    Mutating actions require -ReleaseLiveMutations. The only mutable GUIDs are
    3, 6, and 49. Database evidence is delegated to the existing
    SELECT-only gathering-proof.ps1 harness.
#>
[CmdletBinding()]
param(
    [ValidateSet('preflight', 'proof', 'start', 'status', 'stop')]
    [string]$Action = 'status',
    [ValidateRange(30, 3600)]
    [int]$ProofSeconds = 180,
    [ValidateRange(1, 60)]
    [int]$PollSeconds = 10,
    [switch]$ReleaseLiveMutations,
    [string]$ServerRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$ManifestPath = (Join-Path $PSScriptRoot 'gather-lane-manifest.json'),
    [string]$ControlPath = (Join-Path $PSScriptRoot 'autowow-control.ps1'),
    [string]$ProofPath = (Join-Path $PSScriptRoot 'gathering-proof.ps1'),
    [ValidateSet('127.0.0.1')]
    [string]$BridgeHost = '127.0.0.1',
    [ValidateRange(1, 65535)]
    [int]$BridgePort = 18787
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$allowed = @([uint32]3, [uint32]6, [uint32]49)
$expectedNames = @{ 3 = 'Emealar'; 6 = 'Kurl'; 49 = 'Pikli' }
$expectedProfessions = @{ 3 = 'Herbalism'; 6 = 'Mining'; 49 = 'Herbalism' }
$logRoot = Join-Path $ServerRoot 'logs\gather-lane'
[void](New-Item -ItemType Directory -Path $logRoot -Force)
$runId = 'gather-lane-' + [DateTimeOffset]::UtcNow.ToString('yyyyMMdd-HHmmssfff')
$receiptPath = Join-Path $logRoot "$runId.jsonl"

function Add-Receipt {
    param([string]$Event, [string]$Status, [object]$Details)
    [ordered]@{
        schema = 'autowow.gather-lane.receipt.v1'
        run_id = $runId
        utc = [DateTimeOffset]::UtcNow.ToString('o')
        action = $Action
        event = $Event
        status = $Status
        details = $Details
    } | ConvertTo-Json -Depth 20 -Compress | Add-Content -LiteralPath $receiptPath -Encoding utf8
}

function Invoke-Control {
    param(
        [Parameter(Mandatory)][ValidateSet('list', 'activate', 'deactivate', 'deploy', 'snapshot')][string]$BridgeAction,
        [uint32]$Guid = 0
    )
    if ($BridgeAction -ne 'list' -and $Guid -notin $allowed) {
        throw "Refusing bridge mutation/read for non-cohort GUID $Guid."
    }
    $controlArgs = @{ Action = $BridgeAction; BridgeHost = $BridgeHost; Port = $BridgePort }
    if ($BridgeAction -ne 'list') { $controlArgs.BotGuid = $Guid }
    $raw = & $ControlPath @controlArgs
    $value = $raw | ConvertFrom-Json
    if (-not $value.ok) { throw "Bridge rejected $BridgeAction for ${Guid}: $raw" }
    return $value
}

function Read-Manifest {
    $manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
    if ($manifest.schema -ne 'autowow.gathering-proof.fixture-roster.v1' -or $manifest.purpose -ne 'gathering-proof') {
        throw 'Gather manifest schema or purpose is invalid.'
    }
    $members = @($manifest.members)
    $manifestGuids = @($members | ForEach-Object { [uint32]$_.guid } | Sort-Object)
    if (($manifestGuids -join ',') -ne (@($allowed | Sort-Object) -join ',')) {
        throw "Manifest roster must be exactly $($allowed -join ',')."
    }
    foreach ($member in $members) {
        if ($expectedNames[[int]$member.guid] -ne [string]$member.name -or
            $expectedProfessions[[int]$member.guid] -ne [string]$member.profession) {
            throw "Manifest identity mismatch for GUID $($member.guid)."
        }
    }
    $protected = @($manifest.protected_guids | ForEach-Object { [uint32]$_ })
    if (@($protected | Where-Object { $_ -in $allowed }).Count -ne 0) {
        throw 'Manifest overlaps cohort and protected GUIDs.'
    }
    foreach ($required in @(2,7,8,10,11,12,13,14,15,18,19,20,24,25,26,41,45,60,68,72,83,19032,19033)) {
        if ([uint32]$required -notin $protected) { throw "Manifest omitted protected GUID $required." }
    }
    return $manifest
}

function Get-BotFromList {
    param([object]$List, [uint32]$Guid)
    $matches = @($List.bots | Where-Object { [uint32]$_.guid -eq $Guid } | Select-Object -First 1)
    if ($matches.Count -eq 0) { return $null }
    return $matches[0]
}

function Get-Status {
    $list = Invoke-Control -BridgeAction list
    $rows = [System.Collections.Generic.List[object]]::new()
    foreach ($guid in $allowed) {
        $bot = Get-BotFromList -List $list -Guid $guid
        if ($null -eq $bot) {
            [void]$rows.Add([ordered]@{ guid = $guid; name = $expectedNames[[int]$guid]; online = $false })
            continue
        }
        $snapshot = Invoke-Control -BridgeAction snapshot -Guid $guid
        $state = $snapshot.bot
        [void]$rows.Add([ordered]@{
            guid = $guid
            name = $expectedNames[[int]$guid]
            online = $true
            group_members = $bot.group.members
            map = $state.position.map
            x = $state.position.x
            y = $state.position.y
            z = $state.position.z
            alive = $state.alive
            gather_route = $state.gather_route
        })
    }
    return [ordered]@{
        cohort = @($rows)
        other_online_guids = @($list.bots | Where-Object { [uint32]$_.guid -notin $allowed } | ForEach-Object { [uint32]$_.guid })
    }
}

function Assert-MutationRelease {
    if (-not $ReleaseLiveMutations) { throw "$Action requires -ReleaseLiveMutations." }
}

[void](Read-Manifest)
if (-not (Test-Path -LiteralPath $ControlPath -PathType Leaf)) { throw "Missing control script: $ControlPath" }
if (-not (Test-Path -LiteralPath $ProofPath -PathType Leaf)) { throw "Missing gathering proof: $ProofPath" }

try {
    Add-Receipt -Event 'start' -Status 'PASS' -Details ([ordered]@{ mutable_guids = $allowed; release = [bool]$ReleaseLiveMutations })
    switch ($Action) {
        'preflight' {
            $output = & $ProofPath -ServerRoot $ServerRoot -FixtureRosterPath $ManifestPath -DurationSeconds $ProofSeconds -PollSeconds $PollSeconds
            Add-Receipt -Event 'preflight' -Status 'PASS' -Details ([ordered]@{ output = @($output) })
        }
        'proof' {
            Assert-MutationRelease
            $proofReceipt = Join-Path $logRoot "$runId-proof.json"
            & $ProofPath -ServerRoot $ServerRoot -FixtureRosterPath $ManifestPath -DurationSeconds $ProofSeconds -PollSeconds $PollSeconds -OutputPath $proofReceipt -Apply
            Add-Receipt -Event 'proof' -Status 'PASS' -Details ([ordered]@{ proof_receipt = $proofReceipt })
        }
        'start' {
            Assert-MutationRelease
            $initial = Invoke-Control -BridgeAction list
            foreach ($guid in $allowed) {
                $bot = Get-BotFromList -List $initial -Guid $guid
                if ($null -eq $bot) {
                    [void](Invoke-Control -BridgeAction activate -Guid $guid)
                }
            }
            $deadline = [DateTimeOffset]::UtcNow.AddSeconds(45)
            do {
                Start-Sleep -Milliseconds 500
                $list = Invoke-Control -BridgeAction list
                $online = @($allowed | Where-Object { $null -ne (Get-BotFromList -List $list -Guid $_) })
            } while ($online.Count -ne $allowed.Count -and [DateTimeOffset]::UtcNow -lt $deadline)
            if ($online.Count -ne $allowed.Count) { throw "Only $($online.Count)/$($allowed.Count) gatherers activated." }
            foreach ($guid in $allowed) {
                $bot = Get-BotFromList -List $list -Guid $guid
                if ([int]$bot.group.members -gt 0 -or [bool]$bot.paused) { throw "GUID $guid is grouped or paused." }
                $deploy = Invoke-Control -BridgeAction deploy -Guid $guid
                if ($deploy.mode -ne 'worker_gather' -or [int]$deploy.configured -ne 1) { throw "GUID $guid did not enter worker_gather." }
            }
            $status = Get-Status
            Add-Receipt -Event 'cohort_online' -Status 'PASS' -Details $status
            $status | ConvertTo-Json -Depth 20
        }
        'status' {
            $status = Get-Status
            Add-Receipt -Event 'status' -Status 'PASS' -Details $status
            $status | ConvertTo-Json -Depth 20
        }
        'stop' {
            Assert-MutationRelease
            $list = Invoke-Control -BridgeAction list
            foreach ($guid in $allowed) {
                if ($null -ne (Get-BotFromList -List $list -Guid $guid)) {
                    [void](Invoke-Control -BridgeAction deactivate -Guid $guid)
                }
            }
            Add-Receipt -Event 'cohort_stopped' -Status 'PASS' -Details ([ordered]@{ guids = $allowed })
        }
    }
    Add-Receipt -Event 'finish' -Status 'PASS' -Details ([ordered]@{ receipt = $receiptPath })
}
catch {
    Add-Receipt -Event 'failure' -Status 'FAIL' -Details ([ordered]@{ error = $_.Exception.Message })
    throw
}

Write-Output "Gather lane $Action complete. Receipt: $receiptPath"
